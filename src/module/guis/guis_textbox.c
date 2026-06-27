/* Leno GUI - GTextBox
 *
 * GTextBox 实例方法 (tb.method()):
 *   tb.set_text(text)    tb.set_placeholder(text)
 *   tb.set_password(bool) tb.set_max_length(n)
 *   tb.on_change(cb)     tb.on_submit(cb)
 *   tb.set_anchor(a, mx, my)
 *
 * 内部函数:
 *   gui_textbox_draw_all/event/anchor/free_all
 */
#include "include/native.h"
#include "include/leno_value.h"
#include "include/string_table.h"
#include "guis_internal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TB_CAP 256
#define BLINK_MS 500

/* 行高 = 字体大小 + 行间距（由用户设定，默认 4 像素） */
static inline int tb_line_height(ObjGUITextBox* tb) {
    int sp = tb->line_spacing > 0 ? tb->line_spacing : 4;
    return tb->font_size + sp;
}

/* ===== UTF-8 helpers ===== */
static int tc_blen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}
static int tc_strlen(const char* s) {
    int c = 0; if (!s) return 0;
    while (*s) { c++; s += tc_blen((unsigned char)*s); } return c;
}
/* ===== Gap Buffer (Scintilla-style document model) ===== */
static inline int gb_len(const GapBuffer* gb) {
    return gb->gap_start + (gb->cap - gb->gap_end);
}
static inline char gb_at(const GapBuffer* gb, int pos) {
    if (pos < gb->gap_start) return gb->buf[pos];
    return gb->buf[pos + (gb->gap_end - gb->gap_start)];
}
static void gb_move_gap_to(GapBuffer* gb, int pos) {
    if (pos == gb->gap_start) return;
    if (pos < gb->gap_start) {
        int move = gb->gap_start - pos;
        int gap_size = gb->gap_end - gb->gap_start;
        memmove(gb->buf + pos + gap_size, gb->buf + pos, move);
        gb->gap_start = pos;
        gb->gap_end = pos + gap_size;
    } else {
        int move = pos - gb->gap_start;
        memmove(gb->buf + gb->gap_start, gb->buf + gb->gap_end, move);
        gb->gap_start += move;
        gb->gap_end += move;
    }
}
static void gb_ensure_gap(GapBuffer* gb, int need) {
    int gap = gb->gap_end - gb->gap_start;
    if (gap >= need) return;
    int old_len = gb_len(gb);
    int new_cap = gb->cap * 2;
    if (new_cap < old_len + need + 64) new_cap = old_len + need + 64;

    /* 新分配 + 拷贝 + 释放旧缓冲区，避免 realloc 失败时旧缓冲区被破坏 */
    char* nb = (char*)malloc(new_cap);
    if (!nb) return;  /* 分配失败，保持旧缓冲区不变 */
    /* 拷贝 gap 之前的数据 */
    if (gb->gap_start > 0) memcpy(nb, gb->buf, gb->gap_start);
    /* 拷贝 gap 之后的数据到新缓冲区末尾 */
    int tail = gb->cap - gb->gap_end;
    if (tail > 0) memcpy(nb + new_cap - tail, gb->buf + gb->gap_end, tail);
    /* 释放旧缓冲区，切换到新缓冲区 */
    free(gb->buf);
    gb->buf = nb;
    gb->gap_end = new_cap - tail;
    gb->cap = new_cap;
}
static void gb_insert(GapBuffer* gb, int pos, const char* s, int slen) {
    if (slen <= 0 || !s) return;
    gb_move_gap_to(gb, pos);
    gb_ensure_gap(gb, slen);
    if (!gb->buf) return;
    /* 再次检查 gap 是否足够，防止 ensure_gap 扩容失败 */
    if (gb->gap_end - gb->gap_start < slen) return;
    memcpy(gb->buf + gb->gap_start, s, slen);
    gb->gap_start += slen;
}
static void gb_delete(GapBuffer* gb, int pos, int dlen) {
    if (dlen <= 0 || pos < 0) return;
    int len = gb_len(gb);
    if (pos + dlen > len) dlen = len - pos;
    if (dlen <= 0) return;
    gb_move_gap_to(gb, pos);
    gb->gap_end += dlen;
}
static void gb_clear(GapBuffer* gb) {
    gb->gap_start = 0;
    gb->gap_end = gb->cap;
}
static void gb_free(GapBuffer* gb) {
    if (gb->buf) { free(gb->buf); gb->buf = NULL; }
    gb->cap = 0; gb->gap_start = 0; gb->gap_end = 0;
}
static void gb_get_range(const GapBuffer* gb, int pos, int len, char* out) {
    int i = 0;
    int total = gb_len(gb);
    if (len > total - pos) len = total - pos;
    while (i < len && pos < total) {
        out[i++] = gb_at(gb, pos++);
    }
    out[i] = '\0';
}
static char* gb_to_cstring(const GapBuffer* gb) {
    int len = gb_len(gb);
    char* s = (char*)malloc(len + 1);
    if (!s) return NULL;
    gb_get_range(gb, 0, len, s);
    return s;
}
static int gb_next(const GapBuffer* gb, int pos) {
    if (pos < 0 || pos >= gb_len(gb)) return pos;
    return pos + tc_blen((unsigned char)gb_at(gb, pos));
}
static int gb_prev(const GapBuffer* gb, int pos) {
    if (pos <= 0) return 0;
    int p = pos - 1;
    while (p > 0 && (gb_at(gb, p) & 0xC0) == 0x80) p--;
    return p;
}
static int gb_strlen(const GapBuffer* gb) {
    int c = 0, p = 0;
    int len = gb_len(gb);
    while (p < len) { c++; p = gb_next(gb, p); }
    return c;
}

/* ===== Helpers ===== */
ObjGUITextBox* as_textbox(Value v) {
    if (!val_is_obj(v)) return NULL;
    return (ObjGUITextBox*)val_as_obj(v);
}
/* 测量文本宽度（DrawTextW DT_CALCRECT，与渲染一致） */
/* start/len 为 GapBuffer 逻辑字节范围 */
static int tc_text_width_to(ObjGUITextBox* tb, LenoGUIPlatformRenderer* ren, int start, int len) {
    if (len <= 0 || !tb->font || !tb->font->platform) return 0;
    if (tb->password) {
        int n = 0, p = start;
        int end = start + len;
        int total = gb_len(&tb->gb);
        if (end > total) end = total;
        while (p < end) { n++; p = gb_next(&tb->gb, p); }
        if (n > 200) n = 200;
        char mask[256];
        memset(mask, '*', (size_t)n);
        mask[n] = '\0';
        int w = 0, h = 0;
        leno_gui_platform_text_size_font(ren, tb->font->platform, mask, &w, &h);
        return w;
    }
    char* tmp = (char*)malloc(len + 1);
    if (!tmp) return 0;
    gb_get_range(&tb->gb, start, len, tmp);
    int w = 0, h = 0;
    leno_gui_platform_text_size_font(ren, tb->font->platform, tmp, &w, &h);
    free(tmp);
    return w;
}
/* ===== 行索引 —— 每帧重建，零增量同步，杜绝 bug ===== */
static void tb_lines_ensure(ObjGUITextBox* tb) {
    if (!tb->text_is_dirty) return;
    int tlen = gb_len(&tb->gb);
    int need = 1;
    for (int i = 0; i < tlen; i++)
        if (gb_at(&tb->gb, i) == '\n') need++;
    if (need > tb->line_cap) {
        int nc = tb->line_cap * 2; if (nc < need) nc = need + 64;
        int* nl = (int*)realloc(tb->line_starts, nc * sizeof(int));
        if (!nl) return;
        tb->line_starts = nl; tb->line_cap = nc;
    }
    int n = 0;
    tb->line_starts[n++] = 0;
    for (int i = 0; i < tlen && n < tb->line_cap; i++)
        if (gb_at(&tb->gb, i) == '\n') tb->line_starts[n++] = i + 1;
    tb->line_count = n;
    tb->text_is_dirty = 0;
}

/* ---- 行查询（基于 line_starts 数组，调用前须 tb_lines_ensure）---- */
static int tb_line_start_tb(ObjGUITextBox* tb, int line) {
    if (!tb->line_starts || line < 0) return 0;
    if (line >= tb->line_count) line = tb->line_count - 1;
    return tb->line_starts[line];
}
static int tb_line_end_tb(ObjGUITextBox* tb, int line) {
    if (line + 1 < tb->line_count) return tb->line_starts[line + 1] - 1;
    return gb_len(&tb->gb);
}
static int tb_current_line_tb(ObjGUITextBox* tb, int pos) {
    if (!tb->line_starts || tb->line_count == 0) return 0;
    int lo = 0, hi = tb->line_count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (tb->line_starts[mid] <= pos) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

static void tb_invalidate_layouts_from(ObjGUITextBox* tb, int line);

/* 增量更新行索引：在 pos 处插入/删除 delta 字节后调用（delta 正为插入，负为删除） */
static void tb_update_line_index(ObjGUITextBox* tb, int pos, int delta) {
    if (!tb->line_starts || tb->line_count == 0) {
        tb->text_is_dirty = 1;
        return;
    }
    int line = tb_current_line_tb(tb, pos);
    /* 调整受影响行之后的所有行起始位置 */
    for (int i = line + 1; i < tb->line_count; i++) {
        tb->line_starts[i] += delta;
    }
    int scan_start = tb->line_starts[line];
    int tlen = gb_len(&tb->gb);
    int new_line = line + 1;
    for (int i = scan_start; i < tlen; i++) {
        if (gb_at(&tb->gb, i) == '\n') {
            int next_start = i + 1;
            if (new_line < tb->line_count && tb->line_starts[new_line] == next_start) {
                /* 与旧索引一致（已考虑 delta），后续无需再扫描 */
                tb->line_count = new_line;
                tb_invalidate_layouts_from(tb, line);
                return;
            }
            if (new_line >= tb->line_cap) {
                int nc = tb->line_cap * 2;
                int* nl = (int*)realloc(tb->line_starts, nc * sizeof(int));
                if (!nl) { tb->text_is_dirty = 1; tb_invalidate_layouts_from(tb, line); return; }
                tb->line_starts = nl; tb->line_cap = nc;
            }
            tb->line_starts[new_line++] = next_start;
        }
    }
    tb->line_count = new_line;
    /* 增量失效：只有修改行及之后的 Layout 需要重算 */
    tb_invalidate_layouts_from(tb, line);
}

static void tb_pos_to_line_col_ex(ObjGUITextBox* tb, int pos, int* line, int* col) {
    int ln = tb_current_line_tb(tb, pos);
    int ls = tb_line_start_tb(tb, ln);
    if (line) *line = ln;
    if (col) *col = pos - ls;
}
static int tb_line_col_to_pos(ObjGUITextBox* tb, int line, int col) {
    int ls = tb_line_start_tb(tb, line);
    return ls + col;
}
static int tb_line_y(ObjGUITextBox* tb, int line) {
    return tb->y + tb->border_width + tb->padding_y + line * tb_line_height(tb) - tb->scroll_y;
}
static void tb_mark_dirty(ObjGUITextBox* tb);
static void tb_ensure_cursor_visible(ObjGUITextBox* tb);

/* ===== Undo/Redo ===== */
#define TB_UNDO_MAX_ACTIONS 500
#define TB_UNDO_MAX_BYTES (2 * 1024 * 1024)
#define TB_UNDO_MERGE_INTERVAL_MS 750

static size_t tb_undo_action_size(TBUndoAction* a) {
    size_t s = sizeof(TBUndoAction);
    if (a->text) s += strlen(a->text);
    if (a->old_text) s += strlen(a->old_text);
    return s;
}

void tb_undo_free_stack(TBUndoStack* stack) {
    TBUndoAction* a = stack->top;
    while (a) {
        TBUndoAction* n = a->next;
        if (a->text) free(a->text);
        if (a->old_text) free(a->old_text);
        free(a);
        a = n;
    }
    stack->top = NULL;
    stack->count = 0;
    stack->total_size = 0;
}

static int tb_undo_text_mergeable(const char* text, int len) {
    if (!text || len <= 0) return 0;
    for (int i = 0; i < len; i++)
        if (text[i] == '\n' || text[i] == '\r') return 0;
    return 1;
}

static void tb_undo_drop_oldest(ObjGUITextBox* tb) {
    if (!tb->undo_stack.top) return;
    TBUndoAction* bottom = NULL;
    TBUndoAction* prev = NULL;
    if (!tb->undo_stack.top->next) {
        bottom = tb->undo_stack.top;
        tb->undo_stack.top = NULL;
    } else {
        TBUndoAction* p = tb->undo_stack.top;
        while (p->next->next) p = p->next;
        prev = p;
        bottom = p->next;
        prev->next = NULL;
    }
    tb->undo_stack.total_size -= tb_undo_action_size(bottom);
    tb->undo_stack.count--;
    if (bottom->text) free(bottom->text);
    if (bottom->old_text) free(bottom->old_text);
    free(bottom);
}

static void tb_undo_clear_redo(ObjGUITextBox* tb) {
    tb_undo_free_stack(&tb->redo_stack);
}
static TBUndoAction* tb_undo_new_action(TBUndoType type, int pos, const char* text, const char* old_text,
                                        int cursor_before, int cursor_after, int sel_start, int sel_len, int group) {
    TBUndoAction* a = (TBUndoAction*)malloc(sizeof(TBUndoAction));
    if (!a) return NULL;
    a->type = type; a->pos = pos;
    a->text = text ? strdup(text) : NULL;
    a->old_text = old_text ? strdup(old_text) : NULL;
    a->cursor_before = cursor_before; a->cursor_after = cursor_after;
    a->sel_start = sel_start; a->sel_len = sel_len;
    a->group = group; a->next = NULL;
    return a;
}
static void tb_undo_push(ObjGUITextBox* tb, TBUndoType type, int pos, const char* text, const char* old_text,
                         int cursor_before, int cursor_after, int sel_start, int sel_len) {
    if (!tb->undo_enabled) return;
    int text_len = text ? (int)strlen(text) : 0;

    /* 连续普通输入合并为一个撤销组 */
    int merge = 0;
    if (type == TB_UNDO_INSERT && tb->undo_last_type == TB_UNDO_INSERT && pos == tb->undo_last_pos) {
        uint64_t now = leno_gui_platform_get_ticks();
        if (now - tb->undo_last_ticks < TB_UNDO_MERGE_INTERVAL_MS &&
            tb_undo_text_mergeable(text, text_len)) {
            merge = 1;
        }
    }
    if (!merge) tb->undo_group++;

    TBUndoAction* a = tb_undo_new_action(type, pos, text, old_text, cursor_before, cursor_after, sel_start, sel_len, tb->undo_group);
    if (!a) return;
    a->next = tb->undo_stack.top;
    tb->undo_stack.top = a;
    tb->undo_stack.count++;
    tb->undo_stack.total_size += tb_undo_action_size(a);
    tb_undo_clear_redo(tb);

    /* 限制 undo 栈大小：超出数量或内存上限时删除最老记录 */
    while (tb->undo_stack.count > TB_UNDO_MAX_ACTIONS || tb->undo_stack.total_size > TB_UNDO_MAX_BYTES) {
        tb_undo_drop_oldest(tb);
    }

    tb->undo_last_ticks = leno_gui_platform_get_ticks();
    tb->undo_last_pos = (type == TB_UNDO_INSERT) ? pos + text_len : pos;
    tb->undo_last_type = type;
    tb->undo_last_group = tb->undo_group;
}
static int tb_undo(ObjGUITextBox* tb) {
    if (!tb->undo_stack.top) return 0;
    int group = tb->undo_stack.top->group;
    int any = 0;
    while (tb->undo_stack.top && tb->undo_stack.top->group == group) {
        TBUndoAction* a = tb->undo_stack.top;
        tb->undo_stack.top = a->next;
        tb->undo_stack.count--;
        int delta = 0;
        switch (a->type) {
        case TB_UNDO_INSERT:
            delta = a->text ? -(int)strlen(a->text) : 0;
            gb_delete(&tb->gb, a->pos, a->text ? (int)strlen(a->text) : 0);
            break;
        case TB_UNDO_DELETE:
            delta = a->text ? (int)strlen(a->text) : 0;
            if (a->text) gb_insert(&tb->gb, a->pos, a->text, (int)strlen(a->text));
            break;
        case TB_UNDO_REPLACE:
            delta = (a->old_text ? (int)strlen(a->old_text) : 0) - (a->text ? (int)strlen(a->text) : 0);
            gb_delete(&tb->gb, a->pos, a->text ? (int)strlen(a->text) : 0);
            if (a->old_text) gb_insert(&tb->gb, a->pos, a->old_text, (int)strlen(a->old_text));
            break;
        }
        if (delta != 0) tb_update_line_index(tb, a->pos, delta);
        tb->cursor_pos = a->cursor_before;
        tb->sel_start = a->sel_start; tb->sel_len = a->sel_len;
        a->next = tb->redo_stack.top;
        tb->redo_stack.top = a;
        tb->redo_stack.count++;
        any = 1;
    }
    if (any) {
        tb_mark_dirty(tb);
        tb_ensure_cursor_visible(tb);
    }
    return any;
}
static int tb_redo(ObjGUITextBox* tb) {
    if (!tb->redo_stack.top) return 0;
    int group = tb->redo_stack.top->group;
    int any = 0;
    /* redo 需要按同一组原顺序执行，但栈是反的；先把整组弹到临时数组再正序执行 */
    TBUndoAction* group_actions[64];
    int count = 0;
    while (tb->redo_stack.top && tb->redo_stack.top->group == group && count < 64) {
        group_actions[count++] = tb->redo_stack.top;
        tb->redo_stack.top = tb->redo_stack.top->next;
        tb->redo_stack.count--;
    }
    for (int i = count - 1; i >= 0; i--) {
        TBUndoAction* a = group_actions[i];
        int delta = 0;
        switch (a->type) {
        case TB_UNDO_INSERT:
            delta = a->text ? (int)strlen(a->text) : 0;
            if (a->text) gb_insert(&tb->gb, a->pos, a->text, (int)strlen(a->text));
            break;
        case TB_UNDO_DELETE:
            delta = a->text ? -(int)strlen(a->text) : 0;
            gb_delete(&tb->gb, a->pos, a->text ? (int)strlen(a->text) : 0);
            break;
        case TB_UNDO_REPLACE:
            delta = (a->text ? (int)strlen(a->text) : 0) - (a->old_text ? (int)strlen(a->old_text) : 0);
            gb_delete(&tb->gb, a->pos, a->old_text ? (int)strlen(a->old_text) : 0);
            if (a->text) gb_insert(&tb->gb, a->pos, a->text, (int)strlen(a->text));
            break;
        }
        if (delta != 0) tb_update_line_index(tb, a->pos, delta);
        a->next = tb->undo_stack.top;
        tb->undo_stack.top = a;
        tb->undo_stack.count++;
        tb->cursor_pos = a->cursor_after;
        tb->sel_start = a->sel_start; tb->sel_len = a->sel_len;
        any = 1;
    }
    if (any) {
        tb_mark_dirty(tb);
        tb_ensure_cursor_visible(tb);
    }
    return any;
}

/* ===== LineLayout 缓存管理（Scintilla View 层） ===== */
void tb_free_layouts(ObjGUITextBox* tb) {
    if (!tb->layouts) return;
    for (int i = 0; i < tb->layout_cap; i++) {
        if (tb->layouts[i].offsets) { free(tb->layouts[i].offsets); tb->layouts[i].offsets = NULL; }
        tb->layouts[i].valid = 0;
    }
    free(tb->layouts); tb->layouts = NULL;
    tb->layout_cap = 0;
}
static void tb_ensure_layouts(ObjGUITextBox* tb) {
    if (tb->layout_cap >= tb->line_count) return;
    int nc = tb->layout_cap * 2; if (nc < tb->line_count) nc = tb->line_count + 16;
    LineLayout* nl = (LineLayout*)realloc(tb->layouts, nc * sizeof(LineLayout));
    if (!nl) return;
    memset(nl + tb->layout_cap, 0, (nc - tb->layout_cap) * sizeof(LineLayout));
    tb->layouts = nl; tb->layout_cap = nc;
}
static void tb_invalidate_layouts(ObjGUITextBox* tb) {
    if (!tb->layouts) return;
    for (int i = 0; i < tb->layout_cap; i++) {
        tb->layouts[i].valid = 0;
        if (tb->layouts[i].offsets) { free(tb->layouts[i].offsets); tb->layouts[i].offsets = NULL; }
    }
    tb->cached_max_text_width = 0;
    tb->cached_max_text_width_line = -1;
}
static void tb_invalidate_layouts_from(ObjGUITextBox* tb, int line) {
    if (!tb->layouts) return;
    for (int i = line; i < tb->layout_cap; i++) {
        tb->layouts[i].valid = 0;
        if (tb->layouts[i].offsets) { free(tb->layouts[i].offsets); tb->layouts[i].offsets = NULL; }
    }
    /* 如果失效范围包含当前已知的最宽行，最大宽度缓存需要重新计算 */
    if (tb->cached_max_text_width_line >= line) {
        tb->cached_max_text_width = 0;
        tb->cached_max_text_width_line = -1;
    }
}
static void tb_layout_line(ObjGUITextBox* tb, LenoGUIPlatformRenderer* ren, int line);
static LineLayout* tb_get_layout(ObjGUITextBox* tb, LenoGUIPlatformRenderer* ren, int line) {
    if (line < 0 || line >= tb->line_count) return NULL;
    tb_ensure_layouts(tb);
    LineLayout* lo = &tb->layouts[line];
    if (lo->valid && lo->line == line && lo->start == tb_line_start_tb(tb, line)) return lo;
    tb_layout_line(tb, ren, line);
    return lo->valid ? lo : NULL;
}
/* 行内第 col 个字符的结束 x 偏移（col 从 1 开始）；col=0 返回 0 */
static int tb_line_char_x(ObjGUITextBox* tb, LenoGUIPlatformRenderer* ren, int line, int col) {
    if (col <= 0) return 0;
    LineLayout* lo = tb_get_layout(tb, ren, line);
    if (!lo || !lo->valid) return 0;
    if (col > lo->char_count) col = lo->char_count;
    return lo->offsets[col - 1];
}
/* 根据 x 像素坐标返回字符列（用于鼠标定位） */
static int tb_line_col_from_x(ObjGUITextBox* tb, LenoGUIPlatformRenderer* ren, int line, int x) {
    if (x <= 0) return 0;
    LineLayout* lo = tb_get_layout(tb, ren, line);
    if (!lo || !lo->valid) return 0;
    for (int i = 0; i < lo->char_count; i++) {
        if (x <= lo->offsets[i]) return i + 1;
    }
    return lo->char_count;
}
/* 整行像素宽度 */
static int tb_line_total_width(ObjGUITextBox* tb, LenoGUIPlatformRenderer* ren, int line) {
    LineLayout* lo = tb_get_layout(tb, ren, line);
    if (!lo || !lo->valid) return 0;
    return lo->total_width;
}

static void tb_mark_dirty(ObjGUITextBox* tb) {
    tb->text_is_dirty = 1;
    tb->cached_cursor_pos = -1;
    /* cached_max_text_width 由 tb_layout_line / tb_invalidate_layouts_from 增量维护，
       不再每次全量清零，避免大文本每次击键遍历所有行。 */
}

/* 计算指定行的 LineLayout：逐字符测量，填充 offsets 数组 */
static void tb_layout_line(ObjGUITextBox* tb, LenoGUIPlatformRenderer* ren, int line) {
    if (line < 0 || line >= tb->line_count || !tb->layouts) return;
    LineLayout* lo = &tb->layouts[line];
    int ls = tb_line_start_tb(tb, line);
    int le = tb_line_end_tb(tb, line);
    int len = le - ls;
    lo->line = line; lo->start = ls; lo->len = len;
    lo->total_width = 0; lo->char_count = 0; lo->valid = 0;
    if (len <= 0 || !tb->font || !tb->font->platform) { lo->valid = 1; return; }
    /* 统计字符数 */
    int cc = 0;
    for (int i = 0; i < len; ) { cc++; i += tc_blen((unsigned char)gb_at(&tb->gb, ls + i)); }
    lo->char_count = cc;
    if (lo->offsets) { free(lo->offsets); lo->offsets = NULL; }
    lo->offsets = (int*)malloc(sizeof(int) * cc);
    if (!lo->offsets) return;
    int x = 0, idx = 0;
    for (int i = 0; i < len; ) {
        int cl = tc_blen((unsigned char)gb_at(&tb->gb, ls + i));
        int w = tc_text_width_to(tb, ren, ls + i, cl);
        x += w;
        lo->offsets[idx++] = x;
        i += cl;
    }
    lo->total_width = x;
    lo->valid = 1;
    /* 增量维护最宽行缓存 */
    if (x > tb->cached_max_text_width) {
        tb->cached_max_text_width = x;
        tb->cached_max_text_width_line = line;
    }
}

/* 多行时返回最宽行的像素宽度（tb_lines_ensure 管理行索引，此处独立管理宽度缓存） */
/* 优先用缓存，避免每帧绘制时全量扫描；ren 仅在缓存未命中时参与测量 */
static int tb_text_total_width(ObjGUITextBox* tb, LenoGUIPlatformRenderer* ren) {
    if (!tb->multiline) return tc_text_width_to(tb, ren, 0, gb_len(&tb->gb));
    tb_lines_ensure(tb);
    if (tb->cached_max_text_width > 0) return tb->cached_max_text_width;
    int max_w = 0, total = tb->line_count;
    for (int i = 0; i < total; i++) {
        int w = tb_line_total_width(tb, ren, i);
        if (w > max_w) max_w = w;
    }
    tb->cached_max_text_width = max_w;
    tb->cached_max_text_width_line = (max_w > 0) ? 0 : -1; /* 简化：不再精确追踪最宽行行号，靠增量失效重建 */
    return max_w;
}
/* 返回光标在当前行内的 x 像素坐标 */
/* ren 非 NULL 时用绘制 DC 测量，消除中文宽度偏差 */
static int tb_cursor_x(ObjGUITextBox* tb, LenoGUIPlatformRenderer* ren) {
    if (tb->cached_cursor_pos == tb->cursor_pos) return tb->cached_cursor_x;
    tb_lines_ensure(tb);
    int cx;
    if (!tb->multiline) cx = tc_text_width_to(tb, ren, 0, tb->cursor_pos);
    else {
        int line = tb_current_line_tb(tb, tb->cursor_pos);
        int ls = tb_line_start_tb(tb, line);
        int col = 0;
        for (int i = ls; i < tb->cursor_pos; ) { col++; i += tc_blen((unsigned char)gb_at(&tb->gb, i)); }
        cx = tb_line_char_x(tb, ren, line, col);
    }
    tb->cached_cursor_x = cx;
    tb->cached_cursor_pos = tb->cursor_pos;
    return cx;
}
static int tbox_hit(ObjGUITextBox* tb, float mx, float my) {
    return mx >= tb->x && mx <= tb->x + tb->width &&
           my >= tb->y && my <= tb->y + tb->height;
}
/* 根据当前光标位置更新 IME 候选窗/合成窗坐标 */
static void tb_update_ime_pos(ObjGUITextBox* tb) {
    if (!tb->focused) return;
    int line = tb_current_line_tb(tb, tb->cursor_pos);
    int cx = tb_cursor_x(tb, NULL);
    int lh = tb_line_height(tb);
    int cy = line * lh;
    int ime_x = tb->x + tb->padding_x - tb->scroll_x + cx;
    int ime_y = tb->y + tb->padding_y - tb->scroll_y + cy + tb->font_size;
    leno_gui_platform_set_ime_caret_pos(ime_x, ime_y);
}

/* 保证光标在可见区域内，超出时滚动
 * 性能关键：大文本时用缓存避免每次击键扫描全文+调 GDI */
static void tb_ensure_cursor_visible(ObjGUITextBox* tb) {
    tb_lines_ensure(tb);
    int visible_w = tb->width - 2 * tb->border_width;
    if (visible_w < 1) visible_w = 1;
    int cx = tb_cursor_x(tb, NULL);
    if (cx < tb->scroll_x) tb->scroll_x = cx;
    else if (cx + tb->font_size > tb->scroll_x + visible_w) tb->scroll_x = cx + tb->font_size - visible_w;
    /* 用缓存宽度，避免每次击键 GDI 遍历测量 */
    int total_w = tb->padding_x + (tb->cached_max_text_width > 0 ? tb->cached_max_text_width
                : tb_text_total_width(tb, NULL));
    if (total_w <= visible_w) tb->scroll_x = 0;
    else if (tb->scroll_x > total_w - visible_w) tb->scroll_x = total_w - visible_w;
    if (tb->scroll_x < 0) tb->scroll_x = 0;

    if (tb->multiline) {
        int line = tb_current_line_tb(tb, tb->cursor_pos);
        int lh = tb_line_height(tb);
        int cy = line * lh;
        int visible_h = tb->height - 2 * tb->border_width;
        if (visible_h < 1) visible_h = 1;
        if (cy < tb->scroll_y) tb->scroll_y = cy;
        else if (cy + lh > tb->scroll_y + visible_h)
            tb->scroll_y = cy + lh - visible_h;
        int total_h = tb->padding_y + tb->line_count * lh + 2;
        if (total_h <= visible_h) tb->scroll_y = 0;
        else if (tb->scroll_y > total_h - visible_h) tb->scroll_y = total_h - visible_h;
        if (tb->scroll_y < 0) tb->scroll_y = 0;
    }
    tb_update_ime_pos(tb);
}
static void tb_del_sel(ObjGUITextBox* tb) {
    if (tb->sel_start < 0 || tb->sel_len <= 0) return;
    int ds = tb->sel_start, dl = tb->sel_len;
    int tlen = gb_len(&tb->gb);
    if (ds > tlen) ds = tlen;
    if (ds + dl > tlen) dl = tlen - ds;
    char* old = (char*)malloc(dl + 1);
    if (old) { gb_get_range(&tb->gb, ds, dl, old); old[dl] = '\0'; }
    gb_delete(&tb->gb, ds, dl);
    tb_update_line_index(tb, ds, -dl);
    tb_undo_push(tb, TB_UNDO_DELETE, ds, old, NULL, ds, ds, -1, 0);
    if (old) free(old);
    tb->cursor_pos = ds;
    tb->sel_start = -1; tb->sel_len = 0;
    tb_mark_dirty(tb);
    tb_ensure_cursor_visible(tb);
}
static void tb_insert(ObjGUITextBox* tb, const char* s) {
    if (!s || !s[0]) return;
    if (tb->sel_start >= 0 && tb->sel_len > 0) tb_del_sel(tb);
    int sl = (int)strlen(s);
    if (tb->max_length > 0 && gb_strlen(&tb->gb) + tc_strlen(s) > tb->max_length) return;
    int pos = tb->cursor_pos;
    gb_insert(&tb->gb, pos, s, sl);
    tb_update_line_index(tb, pos, sl);
    tb_undo_push(tb, TB_UNDO_INSERT, pos, s, NULL, pos, pos + sl, -1, 0);
    tb->cursor_pos += sl;
    tb_mark_dirty(tb);
    tb_ensure_cursor_visible(tb);
}
static void tb_backspace(ObjGUITextBox* tb) {
    if (!tb || tb->cursor_pos <= 0) return;
    int pv = gb_prev(&tb->gb, tb->cursor_pos);
    int ln = tb->cursor_pos - pv;
    char* old = (char*)malloc(ln + 1);
    if (old) { gb_get_range(&tb->gb, pv, ln, old); old[ln] = '\0'; }
    gb_delete(&tb->gb, pv, ln);
    tb_update_line_index(tb, pv, -ln);
    tb_undo_push(tb, TB_UNDO_DELETE, pv, old, NULL, pv, tb->cursor_pos, -1, 0);
    if (old) free(old);
    tb->cursor_pos = pv;
    tb_mark_dirty(tb);
    tb_ensure_cursor_visible(tb);
}
static void tb_del(ObjGUITextBox* tb) {
    if (!tb || tb->cursor_pos >= gb_len(&tb->gb)) return;
    int nx = gb_next(&tb->gb, tb->cursor_pos);
    int ln = nx - tb->cursor_pos;
    char* old = (char*)malloc(ln + 1);
    if (old) { gb_get_range(&tb->gb, tb->cursor_pos, ln, old); old[ln] = '\0'; }
    gb_delete(&tb->gb, tb->cursor_pos, ln);
    tb_update_line_index(tb, tb->cursor_pos, -ln);
    tb_undo_push(tb, TB_UNDO_DELETE, tb->cursor_pos, old, NULL, tb->cursor_pos, tb->cursor_pos, -1, 0);
    if (old) free(old);
    tb_mark_dirty(tb);
    tb_ensure_cursor_visible(tb);
}
/* 将字节位置转为字符列（相对于行首，从 0 开始） */
static int tb_pos_to_col(ObjGUITextBox* tb, int line, int pos) {
    int ls = tb_line_start_tb(tb, line);
    int col = 0;
    for (int i = ls; i < pos; ) { col++; i += tc_blen((unsigned char)gb_at(&tb->gb, i)); }
    return col;
}
/* 将字符列转为字节位置（col 从 0 开始） */
static int tb_line_charcol_to_pos(ObjGUITextBox* tb, int line, int col) {
    int ls = tb_line_start_tb(tb, line);
    int le = tb_line_end_tb(tb, line);
    int pos = ls;
    for (int i = 0; i < col && pos < le; i++) pos += tc_blen((unsigned char)gb_at(&tb->gb, pos));
    return pos;
}
static int tb_mx2cp(ObjGUITextBox* tb, float mx, float my) {
    tb_lines_ensure(tb);
    int tlen = gb_len(&tb->gb);
    if (!tb->multiline) {
        int rx = (int)(mx - tb->x - tb->padding_x) + tb->scroll_x;
        if (rx <= 0) return 0;
        int lo = 0, hi = tlen;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            while (mid > 0 && (gb_at(&tb->gb, mid) & 0xC0) == 0x80) mid--;
            int w = tc_text_width_to(tb, NULL, 0, mid);
            if (w < rx) {
                lo = gb_next(&tb->gb, mid);
                if (lo > tlen) lo = tlen;
            } else {
                hi = mid;
            }
        }
        return lo;
    }
    int ry = (int)(my - tb->y - tb->border_width - tb->padding_y) + tb->scroll_y;
    int line = ry / tb_line_height(tb);
    if (line < 0) line = 0;
    int total_lines = tb->line_count;
    if (line >= total_lines) line = total_lines - 1;
    int rx = (int)(mx - tb->x - tb->border_width - tb->padding_x) + tb->scroll_x;
    if (rx <= 0) return tb_line_start_tb(tb, line);
    int col = tb_line_col_from_x(tb, NULL, line, rx);
    return tb_line_charcol_to_pos(tb, line, col);
}

/* ===== Instance methods ===== */
static Value tb_set_text(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    char* s_buf = NULL;
    int sl = 0;
    /* 将 Leno 字符串拷贝到独立 C 堆缓冲区，切断 GC 依赖 */
    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_STRING) {
        ObjString* str = (ObjString*)val_as_obj(args[1]);
        sl = (int)strlen(str->chars);
        s_buf = (char*)malloc(sl + 1);
        if (s_buf) memcpy(s_buf, str->chars, sl + 1);
    }
    int old_len = gb_len(&tb->gb);
    char* old_text = NULL;
    if (old_len > 0) {
        old_text = (char*)malloc(old_len + 1);
        if (old_text) { gb_get_range(&tb->gb, 0, old_len, old_text); old_text[old_len] = '\0'; }
    }
    tb->undo_enabled = 0;
    gb_clear(&tb->gb);
    if (s_buf && sl > 0) gb_insert(&tb->gb, 0, s_buf, sl);
    tb->undo_enabled = 1;
    tb->cursor_pos = gb_len(&tb->gb); tb->sel_start = -1; tb->sel_len = 0;
    tb->text_is_dirty = 1;
    tb->cached_cursor_pos = -1;
    tb->cached_max_text_width = 0;
    tb->cached_max_text_width_line = -1;
    tb_invalidate_layouts(tb);
    if (old_text) free(old_text);
    if (s_buf) free(s_buf);
    return val_null();
}
static Value tb_set_ph(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    const char* s = NULL;
    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_STRING)
        s = ((ObjString*)val_as_obj(args[1]))->chars;
    if (tb->placeholder) { free(tb->placeholder); tb->placeholder = NULL; }
    if (s) tb->placeholder = strdup(s);
    return val_null();
}
static Value tb_set_placeholder_color(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_RGB) {
        ObjRgb* rgb = (ObjRgb*)val_as_obj(args[1]);
        tb->placeholder_r = rgb->r; tb->placeholder_g = rgb->g;
        tb->placeholder_b = rgb->b; tb->placeholder_a = rgb->a;
    }
    return val_null();
}
void gui_textbox_update_placeholder_font(ObjGUITextBox* tb) {
    int fs = tb->placeholder_font_size > 0 ? tb->placeholder_font_size : tb->font_size;
    const char* fn = tb->placeholder_font_name ? tb->placeholder_font_name
                   : (tb->font_name ? tb->font_name : "Microsoft YaHei");
    LenoGUIPlatformFont* pf = leno_gui_platform_load_font(fn, fs);
    if (pf) {
        if (tb->placeholder_font && tb->placeholder_font->platform)
            leno_gui_platform_destroy_font(tb->placeholder_font->platform);
        if (!tb->placeholder_font) {
            tb->placeholder_font = (ObjGUIFont*)calloc(1, sizeof(ObjGUIFont));
        }
        tb->placeholder_font->platform = pf;
    }
}
static Value tb_set_placeholder_font_size(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->placeholder_font_size = val_as_int(args[1]);
    gui_textbox_update_placeholder_font(tb);
    return val_null();
}
static Value tb_set_pwd(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->password = val_as_bool(args[1]) ? 1 : 0; return val_null();
}
static Value tb_set_maxlen(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->max_length = val_as_int(args[1]); return val_null();
}
static Value tb_on_change(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->on_change = args[1]; gc_write_barrier((Object*)tb, args[1]); return val_null();
}
static Value tb_on_submit(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->on_submit = args[1]; gc_write_barrier((Object*)tb, args[1]); return val_null();
}
static Value tb_set_anchor(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->anchor = val_as_int(args[1]); tb->anchor_margin_x = val_as_int(args[2]); tb->anchor_margin_y = val_as_int(args[3]);
    return val_null();
}

/* ----- 颜色 setter ----- */
#define TB_SET_COLOR(name, field_r, field_g, field_b, field_a) \
static Value name(int argc, Value* args) { \
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null(); \
    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_RGB) { \
        ObjRgb* rgb = (ObjRgb*)val_as_obj(args[1]); \
        tb->field_r = rgb->r; tb->field_g = rgb->g; tb->field_b = rgb->b; tb->field_a = rgb->a; \
    } \
    return val_null(); \
}
TB_SET_COLOR(tb_set_cursor_color,    cursor_r, cursor_g, cursor_b, cursor_a)
TB_SET_COLOR(tb_set_text_color,      text_r,   text_g,   text_b,   text_a)
TB_SET_COLOR(tb_set_bg_color,        bg_r,     bg_g,     bg_b,     bg_a)
TB_SET_COLOR(tb_set_border_color,    border_r, border_g, border_b, border_a)
TB_SET_COLOR(tb_set_focus_color,     focus_r,  focus_g,  focus_b,  focus_a)
TB_SET_COLOR(tb_set_selection_color, sel_r,    sel_g,    sel_b,    sel_a)

/* ----- 尺寸/样式 setter ----- */
static Value tb_set_border_width(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->border_width = val_as_int(args[1]); return val_null();
}
static Value tb_set_radius(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->radius = val_as_int(args[1]); return val_null();
}
static Value tb_set_pos(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->x = val_as_int(args[1]); tb->y = val_as_int(args[2]); return val_null();
}
static Value tb_set_size(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->width = val_as_int(args[1]); tb->height = val_as_int(args[2]); return val_null();
}
static Value tb_set_padding(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->padding_x = val_as_int(args[1]); tb->padding_y = val_as_int(args[2]); return val_null();
}
static Value tb_set_font_size(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    int fs = val_as_int(args[1]);
    if (fs <= 0) return val_null();
    tb->font_size = fs;
    if (tb->font) { leno_gui_platform_destroy_font(tb->font->platform); tb->font->platform = NULL; }
    LenoGUIPlatformFont* pf = leno_gui_platform_load_font(tb->font_name ? tb->font_name : "Microsoft YaHei", fs);
    if (pf) tb->font->platform = pf;
    gui_textbox_update_placeholder_font(tb);
    return val_null();
}
static Value tb_set_enabled(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->enabled = val_as_bool(args[1]) ? 1 : 0; return val_null();
}
static Value tb_set_letter_spacing(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->letter_spacing = val_as_int(args[1]); return val_null();
}
static Value tb_set_line_spacing(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->line_spacing = val_as_int(args[1]); return val_null();
}
/* 滚动条颜色 setter */
TB_SET_COLOR(tb_set_sb_track,       sb_track_r, sb_track_g, sb_track_b, sb_track_a)
TB_SET_COLOR(tb_set_sb_thumb,       sb_thumb_r, sb_thumb_g, sb_thumb_b, sb_thumb_a)
TB_SET_COLOR(tb_set_sb_thumb_hover, sb_thumb_hover_r, sb_thumb_hover_g, sb_thumb_hover_b, sb_thumb_hover_a)
TB_SET_COLOR(tb_set_sb_thumb_press, sb_thumb_press_r, sb_thumb_press_g, sb_thumb_press_b, sb_thumb_press_a)

/* ----- 查找 & 选中 ----- */
static Value tb_get_text(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    int tlen = gb_len(&tb->gb);
    if (tlen == 0) return val_obj((Object*)intern_string("", 0));
    char* tmp = gb_to_cstring(&tb->gb);
    ObjString* s = intern_string(tmp, tlen);
    free(tmp);
    return val_obj((Object*)s);
}
static Value tb_find(int argc, Value* args) {
    ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_int(-1);
    const char* needle = NULL;
    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_STRING)
        needle = ((ObjString*)val_as_obj(args[1]))->chars;
    if (!needle || !needle[0]) return val_int(-1);
    int start = 0;
    if (argc >= 3) start = val_as_int(args[2]);
    if (start < 0) start = 0;
    int tlen = gb_len(&tb->gb);
    if (start >= tlen) return val_int(-1);
    /* 提取到连续字符串后查找（find 非高频，此方案足够） */
    int search_len = tlen - start;
    char* hay = (char*)malloc(search_len + 1);
    if (!hay) return val_int(-1);
    gb_get_range(&tb->gb, start, search_len, hay);
    char* pos = strstr(hay, needle);
    int result = pos ? (int)(pos - hay) + start : -1;
    free(hay);
    return val_int(result);
}
static Value tb_set_range_color(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    int start = val_as_int(args[1]);
    int len = val_as_int(args[2]);
    if (start < 0 || len <= 0) return val_null();
    int tlen = gb_len(&tb->gb);
    if (start >= tlen) return val_null();
    if (start + len > tlen) len = tlen - start;
    int r = 255, g = 0, b = 0, a = 255;
    if (val_is_obj(args[3]) && val_as_obj(args[3])->type == OBJ_RGB) {
        ObjRgb* rgb = (ObjRgb*)val_as_obj(args[3]);
        r = rgb->r; g = rgb->g; b = rgb->b; a = rgb->a;
    }
    /* 尝试合并到相邻同色范围 */
    for (int i = 0; i < tb->color_range_count; i++) {
        if (tb->color_ranges[i].r == r && tb->color_ranges[i].g == g &&
            tb->color_ranges[i].b == b && tb->color_ranges[i].a == a) {
            int range_end = tb->color_ranges[i].start + tb->color_ranges[i].len;
            if (start == range_end) {
                tb->color_ranges[i].len += len; return val_null();
            }
            if (start + len == tb->color_ranges[i].start) {
                tb->color_ranges[i].start = start;
                tb->color_ranges[i].len += len; return val_null();
            }
        }
    }
    if (tb->color_range_count >= TB_MAX_COLOR_RANGES) return val_null();
    int idx = tb->color_range_count++;
    tb->color_ranges[idx].start = start;
    tb->color_ranges[idx].len = len;
    tb->color_ranges[idx].r = r; tb->color_ranges[idx].g = g;
    tb->color_ranges[idx].b = b; tb->color_ranges[idx].a = a;
    return val_null();
}
static Value tb_clear_colors(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->color_range_count = 0; return val_null();
}
static Value tb_select(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    int start = val_as_int(args[1]);
    int len = val_as_int(args[2]);
    int tlen = gb_len(&tb->gb);
    if (start < 0) start = 0;
    if (start + len > tlen) len = tlen - start;
    if (len <= 0) { tb->sel_start = -1; tb->sel_len = 0; return val_null(); }
    tb->sel_start = start;
    tb->sel_len = len;
    tb->cursor_pos = start + len;
    tb->blink_visible = 1;
    tb->last_blink = leno_gui_platform_get_ticks();
    tb_ensure_cursor_visible(tb);
    return val_null();
}

#define TB_DRAW_BUF 4096

/* 辅助：从 GapBuffer 提取一段文本并绘制 */
static void tb_draw_range(ObjGUITextBox* tb, LenoGUIPlatformRenderer* r,
    LenoGUIPlatformFont* font, int start, int len, int tx, int ty) {
    if (len <= 0) return;
    if (len < TB_DRAW_BUF) {
        char buf[TB_DRAW_BUF];
        gb_get_range(&tb->gb, start, len, buf);
        leno_gui_platform_draw_text_font(r, font, buf, tx, ty);
    } else {
        char* buf = (char*)malloc(len + 1);
        if (buf) {
            gb_get_range(&tb->gb, start, len, buf);
            leno_gui_platform_draw_text_font(r, font, buf, tx, ty);
            free(buf);
        }
    }
}

/* 按颜色范围分段渲染文本（不修改原文本，避免测量冲突） */
static void tb_draw_text_colored(ObjGUITextBox* tb, LenoGUIPlatformRenderer* r,
    LenoGUIPlatformFont* font, int abs_start, int text_len, int tx, int ty) {
    if (text_len <= 0) return;
    int p = 0, seg_start = 0;
    int cur_r = tb->text_r, cur_g = tb->text_g, cur_b = tb->text_b, cur_a = tb->text_a;
    int seg_x = 0;
    while (p < text_len) {
        int cr = tb->text_r, cg = tb->text_g, cb = tb->text_b, ca = tb->text_a;
        int abs_p = abs_start + p;
        for (int i = 0; i < tb->color_range_count; i++) {
            if (abs_p >= tb->color_ranges[i].start && abs_p < tb->color_ranges[i].start + tb->color_ranges[i].len) {
                cr = tb->color_ranges[i].r; cg = tb->color_ranges[i].g;
                cb = tb->color_ranges[i].b; ca = tb->color_ranges[i].a;
                break;
            }
        }
        int next = gb_next(&tb->gb, abs_start + p) - abs_start;
        if ((cr != cur_r || cg != cur_g || cb != cur_b || ca != cur_a) && p > seg_start) {
            leno_gui_platform_set_draw_color(r, cur_r, cur_g, cur_b, cur_a);
            tb_draw_range(tb, r, font, abs_start + seg_start, p - seg_start, tx + seg_x, ty);
            seg_start = p;
            seg_x = tc_text_width_to(tb, r, abs_start, p);
            cur_r = cr; cur_g = cg; cur_b = cb; cur_a = ca;
        }
        p = next;
    }
    if (p > seg_start) {
        leno_gui_platform_set_draw_color(r, cur_r, cur_g, cur_b, cur_a);
        tb_draw_range(tb, r, font, abs_start + seg_start, p - seg_start, tx + seg_x, ty);
    }
}

/* ===== Draw ===== */
static void tb_draw_one(ObjGUITextBox* tb, ObjGUIRenderer* ren) {
    if (!tb->visible) return;
    tb_lines_ensure(tb);
    LenoGUIPlatformRenderer* r = ren->platform;
    int dx = tb->x, dy = tb->y, dw = tb->width, dh = tb->height, rad = tb->radius;
    /* bg */
    leno_gui_platform_set_draw_color(r, tb->bg_r, tb->bg_g, tb->bg_b, tb->bg_a);
    leno_gui_platform_render_fill_rounded_rect(r, dx, dy, dw, dh, rad);
    /* border */
    if (tb->border_width > 0) {
        if (tb->focused)
            leno_gui_platform_set_draw_color(r, tb->focus_r, tb->focus_g, tb->focus_b, tb->focus_a);
        else
            leno_gui_platform_set_draw_color(r, tb->border_r, tb->border_g, tb->border_b, tb->border_a);
        leno_gui_platform_render_draw_rounded_rect(r, dx, dy, dw, dh, rad);
    }
    /* 设置裁剪区域为文本框内部，防止文字/光标/选区超出边框 */
    int clip_x = dx + tb->border_width;
    int clip_y = dy + tb->border_width;
    int clip_w = dw - 2 * tb->border_width;
    int clip_h = dh - 2 * tb->border_width;
    if (clip_w < 1) clip_w = 1;
    if (clip_h < 1) clip_h = 1;
    leno_gui_platform_set_clip_rect(r, clip_x, clip_y, clip_w, clip_h);

    /* 获取字体内聚留白（光标对齐）；GDI 会多算变音符空间，减 2px 补偿 */
    int lead = tb->font ? leno_gui_platform_font_internal_leading(tb->font->platform) : 0;
    if (lead > 0) lead = lead - 2;

    int tlen = gb_len(&tb->gb);
    if (!tb->multiline) {
        /* selection highlight */
        int tx = dx + tb->padding_x - tb->scroll_x;
        int ty = dy + (dh - tb->font_size) / 2 - lead / 2;  /* 可见字形居中 */
        if (tb->sel_start >= 0 && tb->sel_len > 0) {
            int sx = tx + tc_text_width_to(tb, r, 0, tb->sel_start);
            int sw = tc_text_width_to(tb, r, tb->sel_start, tb->sel_len);
            leno_gui_platform_set_draw_color(r, tb->sel_r, tb->sel_g, tb->sel_b, tb->sel_a);
            leno_gui_platform_render_fill_rect(r, sx, ty + lead, sw, tb->font_size);
        }
        /* text or placeholder */
        if (tlen > 0 && tb->font) {
            leno_gui_platform_set_draw_color(r, tb->text_r, tb->text_g, tb->text_b, tb->text_a);
            if (tb->password) {
                char pwd[128] = {0}; int n = gb_strlen(&tb->gb); if (n > 120) n = 120;
                for (int i = 0; i < n; i++) pwd[i] = '*';
                leno_gui_platform_draw_text_font(r, tb->font->platform, pwd, tx, ty);
            } else if (tb->color_range_count > 0) {
                tb_draw_text_colored(tb, r, tb->font->platform, 0, tlen, tx, ty);
            } else {
                tb_draw_range(tb, r, tb->font->platform, 0, tlen, tx, ty);
            }
        } else if (tb->placeholder && tb->placeholder[0] && tb->font) {
            leno_gui_platform_set_draw_color(r, tb->placeholder_r, tb->placeholder_g, tb->placeholder_b, tb->placeholder_a);
            LenoGUIPlatformFont* ph_font = (tb->placeholder_font && tb->placeholder_font->platform)
                ? tb->placeholder_font->platform : tb->font->platform;
            int ph_y = ty;
            /* placeholder 字体可能比主字体小，微调 Y 居中 */
            if (tb->placeholder_font_size > 0 && tb->placeholder_font_size < tb->font_size)
                ph_y = ty + (tb->font_size - tb->placeholder_font_size) / 2;
            leno_gui_platform_draw_text_font(r, ph_font, tb->placeholder, tx, ph_y);
        }
        /* cursor */
        if (tb->focused && tb->blink_visible) {
            int cx = tx + tb_cursor_x(tb, r);
            leno_gui_platform_set_draw_color(r, tb->cursor_r, tb->cursor_g, tb->cursor_b, tb->cursor_a);
            leno_gui_platform_render_fill_rect(r, cx, ty + lead, 2, tb->font_size);
        }
    } else {
        /* ===== 多行绘制 ===== */
        int tx = dx + tb->border_width + tb->padding_x - tb->scroll_x;
        int total_lines = tb->line_count;
        int sel_end = tb->sel_start + tb->sel_len;
        for (int line = 0; line < total_lines; line++) {
            int ly = tb_line_y(tb, line);
            if (ly + tb->font_size < clip_y || ly > clip_y + clip_h) continue;
            int ls = tb_line_start_tb(tb, line);
            int le = tb_line_end_tb(tb, line);
            /* 选区高亮（使用 LineLayout 缓存） */
            if (tb->sel_start >= 0 && tb->sel_len > 0 && sel_end > ls && tb->sel_start < le) {
                int ss = tb->sel_start > ls ? tb->sel_start : ls;
                int se = sel_end < le ? sel_end : le;
                int col_s = tb_pos_to_col(tb, line, ss);
                int col_e = tb_pos_to_col(tb, line, se);
                int sx = tx + tb_line_char_x(tb, r, line, col_s);
                int sw = tb_line_char_x(tb, r, line, col_e) - sx;
                leno_gui_platform_set_draw_color(r, tb->sel_r, tb->sel_g, tb->sel_b, tb->sel_a);
                leno_gui_platform_render_fill_rect(r, sx, ly + lead, sw, tb->font_size);
            }
            /* 文本 */
            if (tlen > 0 && tb->font && ls < le) {
                if (tb->color_range_count > 0) {
                    tb_draw_text_colored(tb, r, tb->font->platform, ls, le - ls, tx, ly);
                } else {
                    leno_gui_platform_set_draw_color(r, tb->text_r, tb->text_g, tb->text_b, tb->text_a);
                    tb_draw_range(tb, r, tb->font->platform, ls, le - ls, tx, ly);
                }
            }
            /* 光标 */
            if (tb->focused && tb->blink_visible && tb->cursor_pos >= ls && tb->cursor_pos <= le) {
                int cx = tx + tb_cursor_x(tb, r);
                leno_gui_platform_set_draw_color(r, tb->cursor_r, tb->cursor_g, tb->cursor_b, tb->cursor_a);
                leno_gui_platform_render_fill_rect(r, cx, ly + lead, 2, tb->font_size);
            }
        }
        /* placeholder */
        if (tlen == 0 && tb->placeholder && tb->placeholder[0] && tb->font) {
            int ph_ty = dy + tb->border_width + tb->padding_y;
            LenoGUIPlatformFont* ph_font = (tb->placeholder_font && tb->placeholder_font->platform)
                ? tb->placeholder_font->platform : tb->font->platform;
            if (tb->placeholder_font_size > 0 && tb->placeholder_font_size < tb->font_size)
                ph_ty = ph_ty + (tb->font_size - tb->placeholder_font_size) / 2;
            leno_gui_platform_set_draw_color(r, tb->placeholder_r, tb->placeholder_g, tb->placeholder_b, tb->placeholder_a);
            leno_gui_platform_draw_text_font(r, ph_font, tb->placeholder, tx, ph_ty);
        }
    }

    leno_gui_platform_disable_clip_rect(r);

    /* scrollbars */
    int sb_size = 8;
    int inner_w = dw - 2 * tb->border_width;
    int inner_h = dh - 2 * tb->border_width;
    int text_w = tb_text_total_width(tb, r) + tb->padding_x;
    int text_h = tb->multiline ? tb->padding_y + tb->line_count * tb_line_height(tb) + 2 : tb->font_size;
    int need_h = text_w > inner_w - (text_h > inner_h ? sb_size : 0);
    int need_v = text_h > inner_h - (text_w > inner_w ? sb_size : 0);
    int avail_w = inner_w - (need_v ? sb_size : 0);
    int avail_h = inner_h - (need_h ? sb_size : 0);
    if (need_h) {
        int track_x = dx + tb->border_width;
        int track_y = dy + dh - tb->border_width - sb_size;
        int track_w = avail_w;
        /* 轨道 */
        leno_gui_platform_set_draw_color(r, tb->sb_track_r, tb->sb_track_g, tb->sb_track_b, tb->sb_track_a);
        leno_gui_platform_render_fill_rect(r, track_x, track_y, track_w, sb_size);
        if (text_w > avail_w) {
            int thumb_w = (avail_w * avail_w) / text_w; if (thumb_w < sb_size) thumb_w = sb_size;
            int max = text_w - avail_w;
            int thumb_x = track_x + (max > 0 ? (tb->scroll_x * (avail_w - thumb_w)) / max : 0);
            /* 滑块：按下/悬停/普通 */
            if (tb->sb_h_dragging)
                leno_gui_platform_set_draw_color(r, tb->sb_thumb_press_r, tb->sb_thumb_press_g, tb->sb_thumb_press_b, tb->sb_thumb_press_a);
            else if (tb->sb_h_hovered)
                leno_gui_platform_set_draw_color(r, tb->sb_thumb_hover_r, tb->sb_thumb_hover_g, tb->sb_thumb_hover_b, tb->sb_thumb_hover_a);
            else
                leno_gui_platform_set_draw_color(r, tb->sb_thumb_r, tb->sb_thumb_g, tb->sb_thumb_b, tb->sb_thumb_a);
            leno_gui_platform_render_fill_rect(r, thumb_x, track_y, thumb_w, sb_size);
        }
    }
    if (need_v) {
        int track_x = dx + dw - tb->border_width - sb_size;
        int track_y = dy + tb->border_width;
        int track_h = avail_h;
        /* 轨道 */
        leno_gui_platform_set_draw_color(r, tb->sb_track_r, tb->sb_track_g, tb->sb_track_b, tb->sb_track_a);
        leno_gui_platform_render_fill_rect(r, track_x, track_y, sb_size, track_h);
        if (text_h > avail_h) {
            int thumb_h = (avail_h * avail_h) / text_h; if (thumb_h < sb_size) thumb_h = sb_size;
            int max = text_h - avail_h;
            int thumb_y = track_y + (max > 0 ? (tb->scroll_y * (avail_h - thumb_h)) / max : 0);
            /* 滑块：按下/悬停/普通 */
            if (tb->sb_v_dragging)
                leno_gui_platform_set_draw_color(r, tb->sb_thumb_press_r, tb->sb_thumb_press_g, tb->sb_thumb_press_b, tb->sb_thumb_press_a);
            else if (tb->sb_v_hovered)
                leno_gui_platform_set_draw_color(r, tb->sb_thumb_hover_r, tb->sb_thumb_hover_g, tb->sb_thumb_hover_b, tb->sb_thumb_hover_a);
            else
                leno_gui_platform_set_draw_color(r, tb->sb_thumb_r, tb->sb_thumb_g, tb->sb_thumb_b, tb->sb_thumb_a);
            leno_gui_platform_render_fill_rect(r, track_x, thumb_y, sb_size, thumb_h);
        }
    }
}

void gui_textbox_draw_all(ObjGUIWindow* win, ObjGUIRenderer* ren) {
    if (!win || !ren) return;
    uint64_t now = leno_gui_platform_get_ticks();
    ObjGUITextBox* tb = win->textboxes;
    while (tb) {
        if (tb->focused && now - tb->last_blink >= BLINK_MS) {
            tb->blink_visible = !tb->blink_visible; tb->last_blink = now;
        }
        tb = tb->next;
    }
    tb = win->textboxes;
    while (tb) { tb_draw_one(tb, ren); tb = tb->next; }
}

/* ===== Scrollbar hit-test ===== */
/* 检测鼠标是否在滚动条滑块上；is_h/is_v 同时支持点击轨道跳转 */
static int tb_scrollbar_hit(ObjGUITextBox* tb, float mx, float my, int* is_h, int* is_v) {
    int sb = 8, dw = tb->width, dh = tb->height;
    int inner_w = dw - 2 * tb->border_width, inner_h = dh - 2 * tb->border_width;
    int text_w = tb_text_total_width(tb, NULL) + tb->padding_x;
    int text_h = tb->multiline ? tb->padding_y + tb->line_count * tb_line_height(tb) + 2 : tb->font_size;
    int need_h = text_w > inner_w - (text_h > inner_h ? sb : 0);
    int need_v = text_h > inner_h - (text_w > inner_w ? sb : 0);
    int avail_w = inner_w - (need_v ? sb : 0), avail_h = inner_h - (need_h ? sb : 0);
    *is_h = 0; *is_v = 0;
    int mxi = (int)mx, myi = (int)my;
    if (need_h && text_w > avail_w) {
        int tx = tb->x + tb->border_width, ty = tb->y + dh - tb->border_width - sb;
        int thumb_w = (avail_w * avail_w) / text_w;
        if (thumb_w < sb) thumb_w = sb;
        int max = text_w - avail_w;
        int thumb_x = tx + (max > 0 ? (tb->scroll_x * (avail_w - thumb_w)) / max : 0);
        if (myi >= ty && myi <= ty + sb && mxi >= tx && mxi <= tx + avail_w) {
            *is_h = 1;
            if (mxi < thumb_x || mxi > thumb_x + thumb_w) {
                /* 点击轨道空白处：跳转到对应位置 */
                float ratio = (float)(mxi - tx - thumb_w / 2) / (float)(avail_w - thumb_w);
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
                tb->scroll_x = (int)(ratio * max);
            }
            return 1;
        }
    }
    if (need_v && text_h > avail_h) {
        int tx = tb->x + dw - tb->border_width - sb, ty = tb->y + tb->border_width;
        int thumb_h = (avail_h * avail_h) / text_h;
        if (thumb_h < sb) thumb_h = sb;
        int max = text_h - avail_h;
        int thumb_y = ty + (max > 0 ? (tb->scroll_y * (avail_h - thumb_h)) / max : 0);
        if (mxi >= tx && mxi <= tx + sb && myi >= ty && myi <= ty + avail_h) {
            *is_v = 1;
            if (myi < thumb_y || myi > thumb_y + thumb_h) {
                float ratio = (float)(myi - ty - thumb_h / 2) / (float)(avail_h - thumb_h);
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
                tb->scroll_y = (int)(ratio * max);
            }
            return 1;
        }
    }
    return 0;
}

/* ===== Event ===== */
int gui_textbox_handle_event(ObjGUIWindow* win, LenoGUIEvent* ev) {
    if (!win || !ev) return 0;
    ObjGUITextBox* tb;

    /* mouse down: 优先检查滚动条点击，再处理文本框焦点 */
    if (ev->type == LENO_GUI_EVT_MOUSE_DOWN && ev->mouse_button == LENO_GUI_MOUSE_LEFT) {
        /* 先检查是否有文本框的滚动条被点击（包含轨道跳转） */
        tb = win->textboxes;
        while (tb) {
            if (tb->visible && tb->enabled) {
                int is_h, is_v;
                if (tb_scrollbar_hit(tb, ev->mouse_x, ev->mouse_y, &is_h, &is_v)) {
                    tb->blink_visible = 1; tb->last_blink = leno_gui_platform_get_ticks();
                    if (is_h) {
                        tb->sb_h_dragging = 1;
                        tb->sb_drag_start_mx = (int)ev->mouse_x;
                        tb->sb_drag_start_sx = tb->scroll_x;
                    }
                    if (is_v) {
                        tb->sb_v_dragging = 1;
                        tb->sb_drag_start_my = (int)ev->mouse_y;
                        tb->sb_drag_start_sy = tb->scroll_y;
                    }
                    tb_ensure_cursor_visible(tb);
                    return 1;
                }
            }
            tb = tb->next;
        }

        if (win->focused_textbox) { win->focused_textbox->focused = 0; win->focused_textbox->blink_visible = 0; }
        tb = win->textboxes; ObjGUITextBox* hit = NULL;
        while (tb) {
            if (tb->visible && tb->enabled && tbox_hit(tb, ev->mouse_x, ev->mouse_y)) { hit = tb; break; }
            tb = tb->next;
        }
        if (hit) {
            hit->focused = 1; hit->blink_visible = 1; hit->last_blink = leno_gui_platform_get_ticks();
            int cp = tb_mx2cp(hit, ev->mouse_x, ev->mouse_y);
            if (ev->mod_flags & LENO_GUI_MOD_SHIFT) {
                /* Shift+点击：从当前光标位置扩展到点击位置 */
                int start = hit->cursor_pos < cp ? hit->cursor_pos : cp;
                int end = hit->cursor_pos < cp ? cp : hit->cursor_pos;
                hit->sel_start = start; hit->sel_len = end - start; hit->cursor_pos = cp;
                hit->dragging = 0;
            } else {
                hit->cursor_pos = cp; hit->sel_start = -1; hit->sel_len = 0;
                hit->dragging = 1; hit->drag_start_cp = cp;
            }
            tb_ensure_cursor_visible(hit);
            win->focused_textbox = hit;
            gc_write_barrier_obj((Object*)win, (Object*)hit);
            /* 设置 IME 候选窗/合成窗位置为当前光标处 */
            tb_update_ime_pos(hit);
            leno_gui_platform_start_text_input();
            return 1;
        }
        win->focused_textbox = NULL; leno_gui_platform_stop_text_input(); return 0;
    }

    /* mouse move: 滚动条拖拽 或 文本选择拖拽 */
    if (ev->type == LENO_GUI_EVT_MOUSE_MOVE) {
        tb = win->textboxes;
        while (tb) { tb->hovered = tbox_hit(tb, ev->mouse_x, ev->mouse_y); tb = tb->next; }
        /* 检查所有文本框的滚动条 hover */
        tb = win->textboxes;
        while (tb) {
            if (tb->visible && tb->enabled) {
                int sb = 8, dw = tb->width, dh = tb->height;
                int inner_w = dw - 2 * tb->border_width, inner_h = dh - 2 * tb->border_width;
                int text_w = tb_text_total_width(tb, NULL) + tb->padding_x;
                int text_h = tb->multiline ? tb->padding_y + tb->line_count * tb_line_height(tb) + 2 : tb->font_size;
                int need_h = text_w > inner_w - (text_h > inner_h ? sb : 0);
                int need_v = text_h > inner_h - (text_w > inner_w ? sb : 0);
                int mxi = (int)ev->mouse_x, myi = (int)ev->mouse_y;
                tb->sb_h_hovered = 0; tb->sb_v_hovered = 0;
                if (need_h && text_w > inner_w - (need_v ? sb : 0)) {
                    int tx = tb->x + tb->border_width;
                    int ty = tb->y + dh - tb->border_width - sb;
                    int tw = inner_w - (need_v ? sb : 0);
                    int thumb_w = (tw * tw) / text_w; if (thumb_w < sb) thumb_w = sb;
                    int max = text_w - tw;
                    int thumb_x = tx + (max > 0 ? (tb->scroll_x * (tw - thumb_w)) / max : 0);
                    tb->sb_h_hovered = (myi >= ty && myi <= ty + sb && mxi >= thumb_x && mxi <= thumb_x + thumb_w);
                }
                if (need_v && text_h > inner_h - (need_h ? sb : 0)) {
                    int vx = tb->x + dw - tb->border_width - sb;
                    int vy = tb->y + tb->border_width;
                    int vh = inner_h - (need_h ? sb : 0);
                    int thumb_h = (vh * vh) / text_h; if (thumb_h < sb) thumb_h = sb;
                    int max = text_h - vh;
                    int thumb_y = vy + (max > 0 ? (tb->scroll_y * (vh - thumb_h)) / max : 0);
                    tb->sb_v_hovered = (mxi >= vx && mxi <= vx + sb && myi >= thumb_y && myi <= thumb_y + thumb_h);
                }
            }
            tb = tb->next;
        }
        /* 滚动条拖拽优先 */
        if (win->focused_textbox) {
            ObjGUITextBox* ftb = win->focused_textbox;
            int sb = 8, inner_w = ftb->width - 2 * ftb->border_width;
            int text_w = tb_text_total_width(ftb, NULL) + ftb->padding_x;
            int text_h = ftb->multiline ? ftb->padding_y + ftb->line_count * tb_line_height(ftb) + 2 : ftb->font_size;
            int inner_h2 = ftb->height - 2 * ftb->border_width;
            int need_v2 = text_h > inner_h2 - (text_w > inner_w ? sb : 0);

            if (ftb->sb_h_dragging) {
                int avail_w2 = inner_w - (need_v2 ? sb : 0);
                int thumb_w = (avail_w2 * avail_w2) / text_w;
                if (thumb_w < sb) thumb_w = sb;
                int max = text_w - avail_w2;
                if (max > 0) {
                    int dx = (int)ev->mouse_x - ftb->sb_drag_start_mx;
                    int track_w = avail_w2 - thumb_w;
                    if (track_w > 0) {
                        int ns = ftb->sb_drag_start_sx + (dx * max) / track_w;
                        if (ns < 0) ns = 0;
                        if (ns > max) ns = max;
                        ftb->scroll_x = ns;
                    }
                }
                return 1;
            }
            if (ftb->sb_v_dragging) {
                int inner_h_v = ftb->height - 2 * ftb->border_width;
                int need_h2 = text_w > inner_w - (text_h > inner_h_v ? sb : 0);
                int avail_h2 = inner_h_v - (need_h2 ? sb : 0);
                int thumb_h = (avail_h2 * avail_h2) / text_h;
                if (thumb_h < sb) thumb_h = sb;
                int max = text_h - avail_h2;
                if (max > 0) {
                    int dy = (int)ev->mouse_y - ftb->sb_drag_start_my;
                    int track_h = avail_h2 - thumb_h;
                    if (track_h > 0) {
                        int ns = ftb->sb_drag_start_sy + (dy * max) / track_h;
                        if (ns < 0) ns = 0;
                        if (ns > max) ns = max;
                        ftb->scroll_y = ns;
                    }
                }
                return 1;
            }
        }
        /* 文本选择拖拽 */
        if (win->focused_textbox && win->focused_textbox->dragging) {
            ObjGUITextBox* dtb = win->focused_textbox;
            int cp = tb_mx2cp(dtb, ev->mouse_x, ev->mouse_y);
            if (cp < dtb->drag_start_cp) {
                dtb->sel_start = cp; dtb->sel_len = dtb->drag_start_cp - cp; dtb->cursor_pos = cp;
            } else {
                dtb->sel_start = dtb->drag_start_cp; dtb->sel_len = cp - dtb->drag_start_cp; dtb->cursor_pos = cp;
            }
            tb_ensure_cursor_visible(dtb);
            return 1;
        }
        return 0;
    }

    /* mouse up: 结束滚动条拖拽 或 文本选择拖拽 */
    if (ev->type == LENO_GUI_EVT_MOUSE_UP && ev->mouse_button == LENO_GUI_MOUSE_LEFT) {
        if (win->focused_textbox) {
            win->focused_textbox->dragging = 0;
            win->focused_textbox->sb_h_dragging = 0;
            win->focused_textbox->sb_v_dragging = 0;
            return 1;
        }
        return 0;
    }

    /* mouse wheel: 垂直/水平滚动 */
    if (ev->type == LENO_GUI_EVT_MOUSE_WHEEL && win->focused_textbox) {
        ObjGUITextBox* wtb = win->focused_textbox;
        tb_lines_ensure(wtb);
        int visible_h = wtb->height - 2 * wtb->border_width;
        int visible_w2 = wtb->width - 2 * wtb->border_width;
        int total_h = wtb->multiline ? wtb->padding_y + wtb->line_count * tb_line_height(wtb) + 2 : wtb->font_size;
        int total_w2 = tb_text_total_width(wtb, NULL) + wtb->padding_x;

        /* 垂直滚动（Windows: 向前滚 wheel_y>0 → scroll_y 应减小） */
        if (ev->wheel_y != 0.0f && wtb->multiline && total_h > visible_h) {
            int delta = (int)(ev->wheel_y * tb_line_height(wtb) * 3);
            wtb->scroll_y -= delta;
            if (wtb->scroll_y < 0) wtb->scroll_y = 0;
            if (wtb->scroll_y > total_h - visible_h) wtb->scroll_y = total_h - visible_h;
        }
        /* 水平滚动（Shift+滚轮 或 触摸板水平滚动） */
        if (ev->wheel_x != 0.0f && total_w2 > visible_w2) {
            int delta = (int)(ev->wheel_x * wtb->font_size * 3);
            wtb->scroll_x += delta;
            if (wtb->scroll_x < 0) wtb->scroll_x = 0;
            if (wtb->scroll_x > total_w2 - visible_w2) wtb->scroll_x = total_w2 - visible_w2;
        }
        return 1;
    }

    /* keyboard: forward to focused */
    tb = win->focused_textbox;
    if (!tb || !tb->visible || !tb->enabled) return 0;
    if (ev->type != LENO_GUI_EVT_KEY_DOWN && ev->type != LENO_GUI_EVT_TEXT_INPUT) return 0;

    if (ev->type == LENO_GUI_EVT_KEY_DOWN) {
        int ctrl = (ev->mod_flags & LENO_GUI_MOD_CTRL) ? 1 : 0;
        switch (ev->key) {
        case LENO_GUI_KEY_BACKSPACE:
            if (tb->sel_start >= 0 && tb->sel_len > 0) tb_del_sel(tb); else tb_backspace(tb);
            if (!val_is_null(tb->on_change)) call_leno_closure(tb->on_change, 0, NULL);
            tb->blink_visible = 1; tb->last_blink = leno_gui_platform_get_ticks(); return 1;
        case LENO_GUI_KEY_DELETE:
            if (tb->sel_start >= 0 && tb->sel_len > 0) tb_del_sel(tb); else tb_del(tb);
            if (!val_is_null(tb->on_change)) call_leno_closure(tb->on_change, 0, NULL);
            tb->blink_visible = 1; tb->last_blink = leno_gui_platform_get_ticks(); return 1;
        case LENO_GUI_KEY_LEFT:
            if (ctrl) tb->cursor_pos = 0; else tb->cursor_pos = gb_prev(&tb->gb, tb->cursor_pos);
            tb->sel_start = -1; tb->sel_len = 0; tb_ensure_cursor_visible(tb); return 1;
        case LENO_GUI_KEY_RIGHT:
            if (ctrl) tb->cursor_pos = gb_len(&tb->gb);
            else { tb->cursor_pos = gb_next(&tb->gb, tb->cursor_pos); if (tb->cursor_pos > gb_len(&tb->gb)) tb->cursor_pos = gb_len(&tb->gb); }
            tb->sel_start = -1; tb->sel_len = 0; tb_ensure_cursor_visible(tb); return 1;
        case LENO_GUI_KEY_HOME:
            if (tb->multiline) {
                int line = tb_current_line_tb(tb, tb->cursor_pos);
                tb->cursor_pos = tb_line_start_tb(tb, line);
            } else { tb->cursor_pos = 0; }
            tb->sel_start = -1; tb->sel_len = 0; tb_ensure_cursor_visible(tb); return 1;
        case LENO_GUI_KEY_END:
            if (tb->multiline) {
                int line = tb_current_line_tb(tb, tb->cursor_pos);
                tb->cursor_pos = tb_line_end_tb(tb, line);
            } else { tb->cursor_pos = gb_len(&tb->gb); }
            tb->sel_start = -1; tb->sel_len = 0; tb_ensure_cursor_visible(tb); return 1;
        case LENO_GUI_KEY_UP:
        case LENO_GUI_KEY_DOWN:
            if (tb->multiline) {
                int line, col;
                tb_pos_to_line_col_ex(tb, tb->cursor_pos, &line, &col);
                int total = tb->line_count;
                if (ev->key == LENO_GUI_KEY_UP) { if (line > 0) line--; } else { if (line + 1 < total) line++; }
                tb->cursor_pos = tb_line_col_to_pos(tb, line, col);
                tb->sel_start = -1; tb->sel_len = 0; tb_ensure_cursor_visible(tb); return 1;
            }
            return 1;
        case LENO_GUI_KEY_RETURN:
            if (tb->multiline) {
                tb_insert(tb, "\n");
                if (!val_is_null(tb->on_change)) call_leno_closure(tb->on_change, 0, NULL);
                tb->blink_visible = 1; tb->last_blink = leno_gui_platform_get_ticks(); return 1;
            }
            if (!val_is_null(tb->on_submit)) { call_leno_closure(tb->on_submit, 0, NULL); }
            return 1;
        default: break;
        }
        /* Ctrl shortcuts */
        if (ctrl) {
            switch (ev->key) {
            case 'A': case 'a': {
                int tlen = gb_len(&tb->gb);
                tb->sel_start = 0; tb->sel_len = tlen; tb->cursor_pos = tlen; tb_ensure_cursor_visible(tb); return 1;
            }
            case 'C': case 'c': {
                if (tb->sel_start >= 0 && tb->sel_len > 0) {
                    char* buf = (char*)malloc(tb->sel_len + 1);
                    if (buf) {
                        gb_get_range(&tb->gb, tb->sel_start, tb->sel_len, buf);
                        leno_gui_platform_set_clipboard_text(buf);
                        free(buf);
                    }
                } else {
                    char* buf = gb_to_cstring(&tb->gb);
                    if (buf) { leno_gui_platform_set_clipboard_text(buf); free(buf); }
                }
                return 1;
            }
            case 'V': case 'v': {
                char* clip = leno_gui_platform_get_clipboard_text();
                if (clip && clip[0]) {
                    /* 密码模式：粘贴时过滤非 ASCII 字符 */
                    if (tb->password) {
                        char* p = clip;
                        while (*p) { if ((unsigned char)*p > 0x7F) *p = '\0'; else p++; }
                    }
                    tb_insert(tb, clip);
                    if (!val_is_null(tb->on_change)) call_leno_closure(tb->on_change, 0, NULL);
                }
                if (clip) free(clip);
                tb->blink_visible = 1; tb->last_blink = leno_gui_platform_get_ticks(); return 1;
            }
            case 'X': case 'x': {
                if (tb->sel_start >= 0 && tb->sel_len > 0) {
                    char* buf = (char*)malloc(tb->sel_len + 1);
                    if (buf) {
                        gb_get_range(&tb->gb, tb->sel_start, tb->sel_len, buf);
                        leno_gui_platform_set_clipboard_text(buf);
                        free(buf);
                    }
                }
                tb_del_sel(tb); if (!val_is_null(tb->on_change)) call_leno_closure(tb->on_change, 0, NULL);
                return 1;
            }
            case 'Z': case 'z': {
                int shift = (ev->mod_flags & LENO_GUI_MOD_SHIFT) ? 1 : 0;
                if (shift) tb_redo(tb); else tb_undo(tb);
                if (!val_is_null(tb->on_change)) call_leno_closure(tb->on_change, 0, NULL);
                tb->blink_visible = 1; tb->last_blink = leno_gui_platform_get_ticks(); return 1;
            }
            case 'Y': case 'y': {
                tb_redo(tb);
                if (!val_is_null(tb->on_change)) call_leno_closure(tb->on_change, 0, NULL);
                tb->blink_visible = 1; tb->last_blink = leno_gui_platform_get_ticks(); return 1;
            }
            default: break;
            }
        }
    }

    /* text input */
    if (ev->type == LENO_GUI_EVT_TEXT_INPUT && ev->text[0]) {
        unsigned char c0 = (unsigned char)ev->text[0];
        if (c0 == '\r' || c0 == '\n') {
            if (tb->multiline) {
                tb_insert(tb, "\n");
                if (!val_is_null(tb->on_change)) call_leno_closure(tb->on_change, 0, NULL);
                tb->blink_visible = 1; tb->last_blink = leno_gui_platform_get_ticks();
            }
            return 1;
        }
        if (c0 >= 0x20) {
            /* 密码模式：只允许 ASCII 可打印字符，拒绝中文等多字节字符 */
            if (tb->password && c0 > 0x7F) return 1;
            tb_insert(tb, ev->text);
            if (!val_is_null(tb->on_change)) call_leno_closure(tb->on_change, 0, NULL);
            tb->blink_visible = 1; tb->last_blink = leno_gui_platform_get_ticks();
        }
        return 1;
    }
    return 0;
}

/* ===== Anchors ===== */
void gui_textbox_update_anchors(ObjGUIWindow* win, int ww, int wh) {
    if (!win) return;
    ObjGUITextBox* tb = win->textboxes;
    while (tb) {
        if (tb->anchor > 0) {
            int mx = tb->anchor_margin_x, my = tb->anchor_margin_y;
            switch (tb->anchor) {
            case 1: tb->x = mx; tb->y = my; break;
            case 2: tb->x = ww - tb->width - mx; tb->y = my; break;
            case 3: tb->x = mx; tb->y = wh - tb->height - my; break;
            case 4: tb->x = ww - tb->width - mx; tb->y = wh - tb->height - my; break;
            case 5: tb->x = (ww - tb->width) / 2; tb->y = (wh - tb->height) / 2; break;
            case 6: tb->x = (ww - tb->width) / 2; tb->y = my; break;
            case 7: tb->x = (ww - tb->width) / 2; tb->y = wh - tb->height - my; break;
            }
        }
        tb = tb->next;
    }
}

/* ===== Free ===== */
void gui_textbox_free_all(ObjGUIWindow* win) {
    if (!win) return;
    ObjGUITextBox* tb = win->textboxes;
    while (tb) {
        ObjGUITextBox* n = tb->next;
        gb_free(&tb->gb);
        if (tb->placeholder) { free(tb->placeholder); tb->placeholder = NULL; }
        if (tb->font_name) { free(tb->font_name); tb->font_name = NULL; }
        if (tb->placeholder_font_name) { free(tb->placeholder_font_name); tb->placeholder_font_name = NULL; }
        if (tb->placeholder_font && tb->placeholder_font->platform) {
            leno_gui_platform_destroy_font(tb->placeholder_font->platform);
            tb->placeholder_font->platform = NULL;
        }
        if (tb->line_starts) { free(tb->line_starts); tb->line_starts = NULL; }
        tb_free_layouts(tb);
        tb_undo_free_stack(&tb->undo_stack);
        tb_undo_free_stack(&tb->redo_stack);
        tb = n;
    }
    win->textboxes = NULL; win->textbox_count = 0; win->focused_textbox = NULL;
}

/* ===== Register ===== */
extern void textbox_register_method_with_params(const char* name, ObjNative* method, int arity,
    int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
extern ObjNative* make_native(NativeFn fn, int arity, const char* name);
extern void textbox_init_methods(void);

void guis_init_textbox_instance_methods(void) {
    textbox_init_methods();
    TypeKind str_1[] = {TYPE_STRING};
    TypeKind int_1[] = {TYPE_INT};
    TypeKind bool_1[] = {TYPE_BOOL};
    TypeKind any_1[] = {TYPE_ANY};
    TypeKind int_params[] = {TYPE_INT, TYPE_INT};
    TypeKind find_params[] = {TYPE_STRING, TYPE_INT};
    TypeKind anchor_params[] = {TYPE_INT, TYPE_INT, TYPE_INT};

    textbox_register_method_with_params("set_text", make_native(tb_set_text, 2, "set_text"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_1);
    textbox_register_method_with_params("set_placeholder", make_native(tb_set_ph, 2, "set_placeholder"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_1);
    textbox_register_method_with_params("set_placeholder_color", make_native(tb_set_placeholder_color, 2, "set_placeholder_color"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("set_placeholder_font_size", make_native(tb_set_placeholder_font_size, 2, "set_placeholder_font_size"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    textbox_register_method_with_params("set_password", make_native(tb_set_pwd, 2, "set_password"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    textbox_register_method_with_params("set_max_length", make_native(tb_set_maxlen, 2, "set_max_length"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    textbox_register_method_with_params("on_change", make_native(tb_on_change, 2, "on_change"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("on_submit", make_native(tb_on_submit, 2, "on_submit"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("set_anchor", make_native(tb_set_anchor, 4, "set_anchor"), 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, anchor_params);
    /* 颜色 */
    textbox_register_method_with_params("set_cursor_color", make_native(tb_set_cursor_color, 2, "set_cursor_color"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("set_text_color", make_native(tb_set_text_color, 2, "set_text_color"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("set_bg_color", make_native(tb_set_bg_color, 2, "set_bg_color"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("set_border_color", make_native(tb_set_border_color, 2, "set_border_color"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("set_focus_color", make_native(tb_set_focus_color, 2, "set_focus_color"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("set_selection_color", make_native(tb_set_selection_color, 2, "set_selection_color"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    /* 尺寸/位置 */
    textbox_register_method_with_params("set_border_width", make_native(tb_set_border_width, 2, "set_border_width"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    textbox_register_method_with_params("set_radius", make_native(tb_set_radius, 2, "set_radius"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    textbox_register_method_with_params("set_pos", make_native(tb_set_pos, 3, "set_pos"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_params);
    textbox_register_method_with_params("set_size", make_native(tb_set_size, 3, "set_size"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_params);
    textbox_register_method_with_params("set_padding", make_native(tb_set_padding, 3, "set_padding"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_params);
    textbox_register_method_with_params("set_font_size", make_native(tb_set_font_size, 2, "set_font_size"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    textbox_register_method_with_params("set_letter_spacing", make_native(tb_set_letter_spacing, 2, "set_letter_spacing"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    textbox_register_method_with_params("set_line_spacing", make_native(tb_set_line_spacing, 2, "set_line_spacing"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    textbox_register_method_with_params("set_enabled", make_native(tb_set_enabled, 2, "set_enabled"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    /* 滚动条颜色 */
    textbox_register_method_with_params("set_sb_track", make_native(tb_set_sb_track, 2, "set_sb_track"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("set_sb_thumb", make_native(tb_set_sb_thumb, 2, "set_sb_thumb"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("set_sb_thumb_hover", make_native(tb_set_sb_thumb_hover, 2, "set_sb_thumb_hover"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("set_sb_thumb_press", make_native(tb_set_sb_thumb_press, 2, "set_sb_thumb_press"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    /* 查找 & 选中 */
    textbox_register_method_with_params("get_text", make_native(tb_get_text, 1, "get_text"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);
    textbox_register_method_with_params("find", make_native(tb_find, 3, "find"), 2, -1, -1, TYPE_INT, TYPE_UNKNOWN, find_params);
    textbox_register_method_with_params("select", make_native(tb_select, 3, "select"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_params);
    /* 颜色范围 */
    textbox_register_method_with_params("set_range_color", make_native(tb_set_range_color, 4, "set_range_color"), 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, NULL);
    textbox_register_method_with_params("clear_colors", make_native(tb_clear_colors, 1, "clear_colors"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, NULL);
}
