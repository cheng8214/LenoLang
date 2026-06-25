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
static int tc_prev(const char* s, int bp) {
    if (bp <= 0) return 0;
    int p = bp - 1;
    while (p > 0 && (s[p] & 0xC0) == 0x80) p--;
    return p;
}
static int tc_next(const char* s, int bp) {
    if (!s || bp < 0) return bp;
    return bp + tc_blen((unsigned char)s[bp]);
}

/* ===== Helpers ===== */
ObjGUITextBox* as_textbox(Value v) {
    if (!val_is_obj(v)) return NULL;
    return (ObjGUITextBox*)val_as_obj(v);
}
/* 精确测量 UTF-8 文本前 bp 字节的像素宽度（GetTextExtentExPointW，与渲染一致） */
static int tc_text_width_to(ObjGUITextBox* tb, const char* s, int bp) {
    if (bp <= 0 || !s || !s[0] || !tb->font || !tb->font->platform) return 0;
    /* 密码模式：测量 '*' 掩码宽度，与渲染保持一致 */
    if (tb->password) {
        int n = 0, i = 0;
        while (s[i] && i < bp) { n++; i = tc_next(s, i); }
        if (n > 200) n = 200;
        char mask[256];
        memset(mask, '*', (size_t)n);
        mask[n] = '\0';
        return leno_gui_platform_text_width_utf8(tb->font->platform, mask, n);
    }
    return leno_gui_platform_text_width_utf8(tb->font->platform, s, bp);
}
/* ===== 多行辅助函数（基于单字符串中的 \n） ===== */
static int tb_count_lines(const char* text) {
    if (!text || !text[0]) return 1;
    int n = 1; for (int i = 0; text[i]; i++) if (text[i] == '\n') n++; return n;
}
static int tb_line_start(const char* text, int line) {
    int ln = 0, p = 0;
    while (text[p] && ln < line) { if (text[p] == '\n') ln++; p++; }
    return p;
}
/* 返回某行结束的字节位置（指向换行符或字符串末尾） */
static int tb_line_end(const char* text, int line) {
    int p = tb_line_start(text, line);
    while (text[p] && text[p] != '\n') p++;
    return p;
}
static int tb_current_line(const char* text, int pos) {
    int ln = 0;
    for (int i = 0; i < pos && text[i]; i++) if (text[i] == '\n') ln++;
    return ln;
}
static void tb_pos_to_line_col(const char* text, int pos, int* line, int* col) {
    int ln = 0, c = 0;
    for (int i = 0; i < pos && text[i]; i++) {
        if (text[i] == '\n') { ln++; c = 0; }
        else c++;
    }
    if (line) *line = ln;
    if (col) *col = c;
}
static int tb_line_col_to_pos(const char* text, int line, int col) {
    int ln = 0, c = 0, p = 0;
    while (text[p]) {
        if (ln == line && c == col) return p;
        if (text[p] == '\n') { ln++; c = 0; }
        else c++;
        if (ln > line) return p;
        p++;
    }
    return p;
}
static int tb_line_y(ObjGUITextBox* tb, int line) {
    return tb->y + tb->border_width + tb->padding_y + line * tb->font_size - tb->scroll_y;
}
/* 标记文本缓存失效（文本修改时调用） */
static void tb_mark_dirty(ObjGUITextBox* tb) { tb->text_is_dirty = 1; }

/* 多行时返回最宽行的像素宽度，单行时返回整段文本宽度。
   参考 Scintilla：缓存结果，只在文本变更时重算，避免每帧遍历 N 行调用 GDI */
static int tb_text_total_width(ObjGUITextBox* tb) {
    if (!tb->multiline) return tc_text_width_to(tb, tb->text, tb->text_len);
    if (!tb->text_is_dirty) return tb->cached_max_text_width;
    int max_w = 0, total = tb_count_lines(tb->text);
    for (int i = 0; i < total; i++) {
        int ls = tb_line_start(tb->text, i);
        int le = tb_line_end(tb->text, i);
        int w = tc_text_width_to(tb, tb->text + ls, le - ls);
        if (w > max_w) max_w = w;
    }
    tb->cached_max_text_width = max_w;
    tb->text_is_dirty = 0;
    return max_w;
}
/* 返回光标在当前行内的 x 像素坐标 */
static int tb_cursor_x(ObjGUITextBox* tb) {
    if (!tb->multiline) return tc_text_width_to(tb, tb->text, tb->cursor_pos);
    int line = tb_current_line(tb->text, tb->cursor_pos);
    int ls = tb_line_start(tb->text, line);
    return tc_text_width_to(tb, tb->text + ls, tb->cursor_pos - ls);
}
static int tbox_hit(ObjGUITextBox* tb, float mx, float my) {
    return mx >= tb->x && mx <= tb->x + tb->width &&
           my >= tb->y && my <= tb->y + tb->height;
}

/* ===== Text buffer ===== */
static void tb_grow(ObjGUITextBox* tb, int need) {
    if (need <= tb->text_cap) return;
    int nc = tb->text_cap * 2; if (nc < need) nc = need + 64;
    char* nb = (char*)realloc(tb->text, nc);
    if (!nb) return;
    tb->text = nb; tb->text_cap = nc;
}
/* 保证光标在可见区域内，超出时滚动 */
static void tb_ensure_cursor_visible(ObjGUITextBox* tb) {
    int visible_w = tb->width - 2 * tb->border_width;  /* 与滚动条 avail_w 一致，不含 padding */
    if (visible_w < 1) visible_w = 1;
    int cx = tb_cursor_x(tb);
    if (cx < tb->scroll_x) {
        tb->scroll_x = cx;
    } else if (cx > tb->scroll_x + visible_w) {
        tb->scroll_x = cx - visible_w;
    }
    int total_w = tb_text_total_width(tb);
    if (total_w <= visible_w) tb->scroll_x = 0;
    else if (tb->scroll_x > total_w - visible_w) tb->scroll_x = total_w - visible_w;
    if (tb->scroll_x < 0) tb->scroll_x = 0;

    if (tb->multiline) {
        int line = tb_current_line(tb->text, tb->cursor_pos);
        int cy = line * tb->font_size;
        int visible_h = tb->height - 2 * tb->border_width;  /* 与滚动条 avail_h 一致，不含 padding */
        if (visible_h < 1) visible_h = 1;
        if (cy < tb->scroll_y) {
            tb->scroll_y = cy;
        } else if (cy + tb->font_size > tb->scroll_y + visible_h) {
            tb->scroll_y = cy + tb->font_size - visible_h;
        }
        int total_h = tb_count_lines(tb->text) * tb->font_size;
        if (total_h <= visible_h) tb->scroll_y = 0;
        else if (tb->scroll_y > total_h - visible_h) tb->scroll_y = total_h - visible_h;
        if (tb->scroll_y < 0) tb->scroll_y = 0;
    }
}
static void tb_del_sel(ObjGUITextBox* tb) {
    if (tb->sel_start < 0 || tb->sel_len <= 0) return;
    int ds = tb->sel_start, dl = tb->sel_len;
    if (ds > tb->text_len) ds = tb->text_len;
    if (ds + dl > tb->text_len) dl = tb->text_len - ds;
    memmove(tb->text + ds, tb->text + ds + dl, tb->text_len - ds - dl + 1);
    tb->text_len -= dl; tb->cursor_pos = ds;
    tb->sel_start = -1; tb->sel_len = 0;
    tb_mark_dirty(tb);
    tb_ensure_cursor_visible(tb);
}
static void tb_insert(ObjGUITextBox* tb, const char* s) {
    if (!s || !s[0]) return;
    if (tb->sel_start >= 0 && tb->sel_len > 0) tb_del_sel(tb);
    int sl = (int)strlen(s);
    if (tb->max_length > 0 && tc_strlen(tb->text) + tc_strlen(s) > tb->max_length) return;
    tb_grow(tb, tb->text_len + sl + 1);
    memmove(tb->text + tb->cursor_pos + sl, tb->text + tb->cursor_pos, tb->text_len - tb->cursor_pos + 1);
    memcpy(tb->text + tb->cursor_pos, s, sl);
    tb->text_len += sl; tb->text[tb->text_len] = '\0'; tb->cursor_pos += sl;
    tb_mark_dirty(tb);
    tb_ensure_cursor_visible(tb);
}
static void tb_backspace(ObjGUITextBox* tb) {
    if (!tb || tb->cursor_pos <= 0) return;
    int pv = tc_prev(tb->text, tb->cursor_pos);
    int ln = tb->cursor_pos - pv;
    memmove(tb->text + pv, tb->text + tb->cursor_pos, tb->text_len - tb->cursor_pos + 1);
    tb->text_len -= ln; tb->cursor_pos = pv;
    tb_mark_dirty(tb);
    tb_ensure_cursor_visible(tb);
}
static void tb_del(ObjGUITextBox* tb) {
    if (!tb || tb->cursor_pos >= tb->text_len) return;
    int nx = tc_next(tb->text, tb->cursor_pos);
    int ln = nx - tb->cursor_pos;
    memmove(tb->text + tb->cursor_pos, tb->text + nx, tb->text_len - nx + 1);
    tb->text_len -= ln;
    tb_mark_dirty(tb);
    tb_ensure_cursor_visible(tb);
}
static int tb_mx2cp(ObjGUITextBox* tb, float mx, float my) {
    if (!tb->multiline) {
        int rx = (int)(mx - tb->x - tb->padding_x) + tb->scroll_x;
        if (rx <= 0) return 0;
        /* 二分搜索 + GDI 精确测量，确保与渲染位置一致 */
        int lo = 0, hi = tb->text_len;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            /* 确保 mid 在字符边界上 */
            while (mid > 0 && (tb->text[mid] & 0xC0) == 0x80) mid--;
            int w = tc_text_width_to(tb, tb->text, mid);
            if (w < rx) {
                lo = tc_next(tb->text, mid);
                if (lo > tb->text_len) lo = tb->text_len;
            } else {
                hi = mid;
            }
        }
        return lo;
    }
    /* 多行：先确定行 */
    int ry = (int)(my - tb->y - tb->border_width - tb->padding_y) + tb->scroll_y;
    int line = ry / tb->font_size;
    if (line < 0) line = 0;
    int total_lines = tb_count_lines(tb->text);
    if (line >= total_lines) line = total_lines - 1;
    int start = tb_line_start(tb->text, line);
    int end = tb_line_end(tb->text, line);
    int rx = (int)(mx - tb->x - tb->border_width - tb->padding_x) + tb->scroll_x;
    if (rx <= 0) return start;
    /* 行内二分搜索 */
    int lo2 = start, hi2 = end;
    while (lo2 < hi2) {
        int mid = (lo2 + hi2) / 2;
        while (mid > start && (tb->text[mid] & 0xC0) == 0x80) mid--;
        int w = tc_text_width_to(tb, tb->text + start, mid - start);
        if (w < rx) {
            int nxt = tc_next(tb->text, mid);
            if (nxt > end) nxt = end;
            lo2 = nxt;
        } else {
            hi2 = mid;
        }
    }
    return lo2;
}

/* ===== Instance methods ===== */
static Value tb_set_text(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    const char* s = NULL;
    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_STRING)
        s = ((ObjString*)val_as_obj(args[1]))->chars;
    int sl = s ? (int)strlen(s) : 0;
    tb_grow(tb, sl + 1);
    if (s && sl > 0) { memcpy(tb->text, s, sl); tb->text[sl] = '\0'; tb->text_len = sl; }
    else { tb->text[0] = '\0'; tb->text_len = 0; }
    tb->cursor_pos = tb->text_len; tb->sel_start = -1; tb->sel_len = 0;
    tb_mark_dirty(tb);
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
    tb->on_change = args[1]; return val_null();
}
static Value tb_on_submit(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    tb->on_submit = args[1]; return val_null();
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
/* 滚动条颜色 setter */
TB_SET_COLOR(tb_set_sb_track,       sb_track_r, sb_track_g, sb_track_b, sb_track_a)
TB_SET_COLOR(tb_set_sb_thumb,       sb_thumb_r, sb_thumb_g, sb_thumb_b, sb_thumb_a)
TB_SET_COLOR(tb_set_sb_thumb_hover, sb_thumb_hover_r, sb_thumb_hover_g, sb_thumb_hover_b, sb_thumb_hover_a)
TB_SET_COLOR(tb_set_sb_thumb_press, sb_thumb_press_r, sb_thumb_press_g, sb_thumb_press_b, sb_thumb_press_a)

/* ----- 查找 & 选中 ----- */
static Value tb_get_text(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    ObjString* s = tb->text && tb->text[0] ? intern_string(tb->text, strlen(tb->text)) : intern_string("", 0);
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
    if (start >= tb->text_len) return val_int(-1);
    int nlen = (int)strlen(needle);
    char* pos = strstr(tb->text + start, needle);
    if (!pos) return val_int(-1);
    int found = (int)(pos - tb->text);
    /* 选中找到的文本 */
    tb->sel_start = found;
    tb->sel_len = nlen;
    tb->cursor_pos = found + nlen;
    tb->blink_visible = 1;
    tb->last_blink = leno_gui_platform_get_ticks();
    tb_ensure_cursor_visible(tb);
    return val_int(found);
}
static Value tb_select(int argc, Value* args) {
    (void)argc; ObjGUITextBox* tb = as_textbox(args[0]); if (!tb) return val_null();
    int start = val_as_int(args[1]);
    int len = val_as_int(args[2]);
    if (start < 0) start = 0;
    if (start + len > tb->text_len) len = tb->text_len - start;
    if (len <= 0) { tb->sel_start = -1; tb->sel_len = 0; return val_null(); }
    tb->sel_start = start;
    tb->sel_len = len;
    tb->cursor_pos = start + len;
    tb->blink_visible = 1;
    tb->last_blink = leno_gui_platform_get_ticks();
    tb_ensure_cursor_visible(tb);
    return val_null();
}

/* ===== Draw ===== */
static void tb_draw_one(ObjGUITextBox* tb, ObjGUIRenderer* ren) {
    if (!tb->visible) return;
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

    if (!tb->multiline) {
        /* selection highlight */
        int tx = dx + tb->padding_x - tb->scroll_x;
        int ty = dy + (dh - tb->font_size) / 2 - lead / 2;  /* 可见字形居中 */
        if (tb->sel_start >= 0 && tb->sel_len > 0) {
            int sx = tx + tc_text_width_to(tb, tb->text, tb->sel_start);
            int sw = tc_text_width_to(tb, tb->text + tb->sel_start, tb->sel_len);
            leno_gui_platform_set_draw_color(r, tb->sel_r, tb->sel_g, tb->sel_b, tb->sel_a);
            leno_gui_platform_render_fill_rect(r, sx, ty + lead, sw, tb->font_size);
        }
        /* text or placeholder */
        if (tb->text_len > 0 && tb->font) {
            leno_gui_platform_set_draw_color(r, tb->text_r, tb->text_g, tb->text_b, tb->text_a);
            if (tb->password) {
                char pwd[128] = {0}; int n = tc_strlen(tb->text); if (n > 120) n = 120;
                for (int i = 0; i < n; i++) pwd[i] = '*';
                leno_gui_platform_draw_text_font(r, tb->font->platform, pwd, tx, ty);
            } else {
                leno_gui_platform_draw_text_font(r, tb->font->platform, tb->text, tx, ty);
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
            int cx = tx + tc_text_width_to(tb, tb->text, tb->cursor_pos);
            leno_gui_platform_set_draw_color(r, tb->cursor_r, tb->cursor_g, tb->cursor_b, tb->cursor_a);
            leno_gui_platform_render_fill_rect(r, cx, ty + lead, 2, tb->font_size);
        }
    } else {
        /* ===== 多行绘制 ===== */
        int tx = dx + tb->border_width + tb->padding_x - tb->scroll_x;
        int total_lines = tb_count_lines(tb->text);
        int sel_end = tb->sel_start + tb->sel_len;
        for (int line = 0; line < total_lines; line++) {
            int ly = tb_line_y(tb, line);
            if (ly + tb->font_size < clip_y || ly > clip_y + clip_h) continue;
            int ls = tb_line_start(tb->text, line);
            int le = tb_line_end(tb->text, line);
            /* 选区高亮 */
            if (tb->sel_start >= 0 && tb->sel_len > 0 && sel_end > ls && tb->sel_start < le) {
                int ss = tb->sel_start > ls ? tb->sel_start : ls;
                int se = sel_end < le ? sel_end : le;
                int sx = tx + tc_text_width_to(tb, tb->text + ls, ss - ls);
                int sw = tc_text_width_to(tb, tb->text + ss, se - ss);
                leno_gui_platform_set_draw_color(r, tb->sel_r, tb->sel_g, tb->sel_b, tb->sel_a);
                leno_gui_platform_render_fill_rect(r, sx, ly + lead, sw, tb->font_size);
            }
            /* 文本 */
            if (tb->text_len > 0 && tb->font && ls < le) {
                char save = tb->text[le];
                tb->text[le] = '\0';
                leno_gui_platform_set_draw_color(r, tb->text_r, tb->text_g, tb->text_b, tb->text_a);
                leno_gui_platform_draw_text_font(r, tb->font->platform, tb->text + ls, tx, ly);
                tb->text[le] = save;
            }
            /* 光标 */
            if (tb->focused && tb->blink_visible && tb->cursor_pos >= ls && tb->cursor_pos <= le) {
                int cx = tx + tc_text_width_to(tb, tb->text + ls, tb->cursor_pos - ls);
                leno_gui_platform_set_draw_color(r, tb->cursor_r, tb->cursor_g, tb->cursor_b, tb->cursor_a);
                leno_gui_platform_render_fill_rect(r, cx, ly + lead, 2, tb->font_size);
            }
        }
        /* placeholder */
        if (tb->text_len == 0 && tb->placeholder && tb->placeholder[0] && tb->font) {
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
    int text_w = tb_text_total_width(tb);
    int text_h = tb->multiline ? tb_count_lines(tb->text) * tb->font_size : tb->font_size;
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
    int text_w = tb_text_total_width(tb);
    int text_h = tb->multiline ? tb_count_lines(tb->text) * tb->font_size : tb->font_size;
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
            /* 设置 IME 候选窗/合成窗位置为文本框左下角（参考 SDL3） */
            leno_gui_platform_set_ime_caret_pos(hit->x + hit->padding_x, hit->y + hit->height);
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
                int text_w = tb_text_total_width(tb);
                int text_h = tb->multiline ? tb_count_lines(tb->text) * tb->font_size : tb->font_size;
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
            int text_w = tb_text_total_width(ftb);
            int text_h = ftb->multiline ? tb_count_lines(ftb->text) * ftb->font_size : ftb->font_size;
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
        int visible_h = wtb->height - 2 * wtb->border_width;
        int visible_w2 = wtb->width - 2 * wtb->border_width;
        int total_h = wtb->multiline ? tb_count_lines(wtb->text) * wtb->font_size : wtb->font_size;
        int total_w2 = tb_text_total_width(wtb);

        /* 垂直滚动（Windows: 向前滚 wheel_y>0 → scroll_y 应减小） */
        if (ev->wheel_y != 0.0f && wtb->multiline && total_h > visible_h) {
            int delta = (int)(ev->wheel_y * wtb->font_size * 3);
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
            if (ctrl) tb->cursor_pos = 0; else tb->cursor_pos = tc_prev(tb->text, tb->cursor_pos);
            tb->sel_start = -1; tb->sel_len = 0; tb_ensure_cursor_visible(tb); return 1;
        case LENO_GUI_KEY_RIGHT:
            if (ctrl) tb->cursor_pos = tb->text_len;
            else { tb->cursor_pos = tc_next(tb->text, tb->cursor_pos); if (tb->cursor_pos > tb->text_len) tb->cursor_pos = tb->text_len; }
            tb->sel_start = -1; tb->sel_len = 0; tb_ensure_cursor_visible(tb); return 1;
        case LENO_GUI_KEY_HOME:
            if (tb->multiline) {
                int line = tb_current_line(tb->text, tb->cursor_pos);
                tb->cursor_pos = tb_line_start(tb->text, line);
            } else { tb->cursor_pos = 0; }
            tb->sel_start = -1; tb->sel_len = 0; tb_ensure_cursor_visible(tb); return 1;
        case LENO_GUI_KEY_END:
            if (tb->multiline) {
                int line = tb_current_line(tb->text, tb->cursor_pos);
                tb->cursor_pos = tb_line_end(tb->text, line);
            } else { tb->cursor_pos = tb->text_len; }
            tb->sel_start = -1; tb->sel_len = 0; tb_ensure_cursor_visible(tb); return 1;
        case LENO_GUI_KEY_UP:
        case LENO_GUI_KEY_DOWN:
            if (tb->multiline) {
                int line, col;
                tb_pos_to_line_col(tb->text, tb->cursor_pos, &line, &col);
                int total = tb_count_lines(tb->text);
                if (ev->key == LENO_GUI_KEY_UP) { if (line > 0) line--; } else { if (line + 1 < total) line++; }
                tb->cursor_pos = tb_line_col_to_pos(tb->text, line, col);
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
            case 'A': case 'a': tb->sel_start = 0; tb->sel_len = tb->text_len; tb->cursor_pos = tb->text_len; tb_ensure_cursor_visible(tb); return 1;
            case 'C': case 'c': {
                if (tb->sel_start >= 0 && tb->sel_len > 0) {
                    char c = tb->text[tb->sel_start + tb->sel_len]; tb->text[tb->sel_start + tb->sel_len] = '\0';
                    leno_gui_platform_set_clipboard_text(tb->text + tb->sel_start);
                    tb->text[tb->sel_start + tb->sel_len] = c;
                } else leno_gui_platform_set_clipboard_text(tb->text);
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
                    char c = tb->text[tb->sel_start + tb->sel_len]; tb->text[tb->sel_start + tb->sel_len] = '\0';
                    leno_gui_platform_set_clipboard_text(tb->text + tb->sel_start);
                    tb->text[tb->sel_start + tb->sel_len] = c;
                }
                tb_del_sel(tb); if (!val_is_null(tb->on_change)) call_leno_closure(tb->on_change, 0, NULL);
                return 1;
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
        if (tb->text) { free(tb->text); tb->text = NULL; }
        if (tb->placeholder) { free(tb->placeholder); tb->placeholder = NULL; }
        if (tb->font_name) { free(tb->font_name); tb->font_name = NULL; }
        if (tb->placeholder_font_name) { free(tb->placeholder_font_name); tb->placeholder_font_name = NULL; }
        if (tb->placeholder_font && tb->placeholder_font->platform) {
            leno_gui_platform_destroy_font(tb->placeholder_font->platform);
            tb->placeholder_font->platform = NULL;
        }
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
}
