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
/* 单个 UTF-8 字符宽度：ASCII 半角，CJK 全角，换行符宽度为 0 */
static int tc_char_width(ObjGUITextBox* tb, const char* s, int bp) {
    unsigned char c = (unsigned char)s[bp];
    if (c == '\n' || c == '\r') return 0;
    if (c < 0x80) return (tb->font_size * 6) / 10;
    return tb->font_size;
}
/* 计算从开头到字节位置 bp 的像素宽度 */
static int tc_text_width_to(ObjGUITextBox* tb, const char* s, int bp) {
    int x = 0, p = 0;
    while (s[p] && p < bp) {
        x += tc_char_width(tb, s, p);
        p = tc_next(s, p);
    }
    return x;
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
/* 多行时返回最宽行的像素宽度，单行时返回整段文本宽度 */
static int tb_text_total_width(ObjGUITextBox* tb) {
    if (!tb->multiline) return tc_text_width_to(tb, tb->text, tb->text_len);
    int max_w = 0, total = tb_count_lines(tb->text);
    for (int i = 0; i < total; i++) {
        int ls = tb_line_start(tb->text, i);
        int le = tb_line_end(tb->text, i);
        int w = tc_text_width_to(tb, tb->text + ls, le - ls);
        if (w > max_w) max_w = w;
    }
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
    int visible_w = tb->width - 2 * tb->border_width - 2 * tb->padding_x;
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
        int visible_h = tb->height - 2 * tb->border_width - 2 * tb->padding_y;
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
    tb_ensure_cursor_visible(tb);
}
static void tb_backspace(ObjGUITextBox* tb) {
    if (!tb || tb->cursor_pos <= 0) return;
    int pv = tc_prev(tb->text, tb->cursor_pos);
    int ln = tb->cursor_pos - pv;
    memmove(tb->text + pv, tb->text + tb->cursor_pos, tb->text_len - tb->cursor_pos + 1);
    tb->text_len -= ln; tb->cursor_pos = pv;
    tb_ensure_cursor_visible(tb);
}
static void tb_del(ObjGUITextBox* tb) {
    if (!tb || tb->cursor_pos >= tb->text_len) return;
    int nx = tc_next(tb->text, tb->cursor_pos);
    int ln = nx - tb->cursor_pos;
    memmove(tb->text + tb->cursor_pos, tb->text + nx, tb->text_len - nx + 1);
    tb->text_len -= ln;
    tb_ensure_cursor_visible(tb);
}
static int tb_mx2cp(ObjGUITextBox* tb, float mx, float my) {
    if (!tb->multiline) {
        int rx = (int)(mx - tb->x - tb->padding_x) + tb->scroll_x;
        if (rx <= 0) return 0;
        int p = 0, x = 0;
        while (tb->text[p]) {
            int cw = tc_char_width(tb, tb->text, p);
            if (x + cw / 2 >= rx) return p;
            x += cw;
            p = tc_next(tb->text, p);
        }
        return tb->text_len;
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
    int p = start, x = 0;
    while (p < end && tb->text[p]) {
        int cw = tc_char_width(tb, tb->text, p);
        if (x + cw / 2 >= rx) return p;
        x += cw;
        p = tc_next(tb->text, p);
    }
    return end;
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

    if (!tb->multiline) {
        /* selection highlight */
        int tx = dx + tb->padding_x - tb->scroll_x, ty = dy + (dh - tb->font_size) / 2;
        if (tb->focused && tb->sel_start >= 0 && tb->sel_len > 0) {
            int sx = tx + tc_text_width_to(tb, tb->text, tb->sel_start);
            int sw = tc_text_width_to(tb, tb->text + tb->sel_start, tb->sel_len);
            leno_gui_platform_set_draw_color(r, tb->sel_r, tb->sel_g, tb->sel_b, tb->sel_a);
            leno_gui_platform_render_fill_rect(r, sx, dy + 2, sw, dh - 4);
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
            leno_gui_platform_draw_text_font(r, tb->font->platform, tb->placeholder, tx, ty);
        }
        /* cursor */
        if (tb->focused && tb->blink_visible) {
            int cx = tx + tc_text_width_to(tb, tb->text, tb->cursor_pos);
            leno_gui_platform_set_draw_color(r, tb->cursor_r, tb->cursor_g, tb->cursor_b, tb->cursor_a);
            leno_gui_platform_render_fill_rect(r, cx, dy + 4, 2, dh - 8);
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
            if (tb->focused && tb->sel_start >= 0 && tb->sel_len > 0 && sel_end > ls && tb->sel_start < le) {
                int ss = tb->sel_start > ls ? tb->sel_start : ls;
                int se = sel_end < le ? sel_end : le;
                int sx = tx + tc_text_width_to(tb, tb->text + ls, ss - ls);
                int sw = tc_text_width_to(tb, tb->text + ss, se - ss);
                leno_gui_platform_set_draw_color(r, tb->sel_r, tb->sel_g, tb->sel_b, tb->sel_a);
                leno_gui_platform_render_fill_rect(r, sx, ly, sw, tb->font_size);
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
                leno_gui_platform_render_fill_rect(r, cx, ly + 2, 2, tb->font_size - 4);
            }
        }
        /* placeholder */
        if (tb->text_len == 0 && tb->placeholder && tb->placeholder[0] && tb->font) {
            int ty = dy + tb->border_width + tb->padding_y;
            leno_gui_platform_set_draw_color(r, tb->placeholder_r, tb->placeholder_g, tb->placeholder_b, tb->placeholder_a);
            leno_gui_platform_draw_text_font(r, tb->font->platform, tb->placeholder, tx, ty);
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
        leno_gui_platform_set_draw_color(r, 220, 220, 220, 255);
        leno_gui_platform_render_fill_rect(r, track_x, track_y, track_w, sb_size);
        if (text_w > avail_w) {
            int thumb_w = (avail_w * avail_w) / text_w; if (thumb_w < sb_size) thumb_w = sb_size;
            int max = text_w - avail_w;
            int thumb_x = track_x + (max > 0 ? (tb->scroll_x * (avail_w - thumb_w)) / max : 0);
            leno_gui_platform_set_draw_color(r, 150, 150, 150, 255);
            leno_gui_platform_render_fill_rect(r, thumb_x, track_y, thumb_w, sb_size);
        }
    }
    if (need_v) {
        int track_x = dx + dw - tb->border_width - sb_size;
        int track_y = dy + tb->border_width;
        int track_h = avail_h;
        leno_gui_platform_set_draw_color(r, 220, 220, 220, 255);
        leno_gui_platform_render_fill_rect(r, track_x, track_y, sb_size, track_h);
        if (text_h > avail_h) {
            int thumb_h = (avail_h * avail_h) / text_h; if (thumb_h < sb_size) thumb_h = sb_size;
            int max = text_h - avail_h;
            int thumb_y = track_y + (max > 0 ? (tb->scroll_y * (avail_h - thumb_h)) / max : 0);
            leno_gui_platform_set_draw_color(r, 150, 150, 150, 255);
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

/* ===== Event ===== */
int gui_textbox_handle_event(ObjGUIWindow* win, LenoGUIEvent* ev) {
    if (!win || !ev) return 0;
    ObjGUITextBox* tb;

    /* mouse down: set focus / start drag selection */
    if (ev->type == LENO_GUI_EVT_MOUSE_DOWN && ev->mouse_button == LENO_GUI_MOUSE_LEFT) {
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

    /* mouse move: drag selection */
    if (ev->type == LENO_GUI_EVT_MOUSE_MOVE) {
        tb = win->textboxes;
        while (tb) { tb->hovered = tbox_hit(tb, ev->mouse_x, ev->mouse_y); tb = tb->next; }
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

    /* mouse up: end drag selection */
    if (ev->type == LENO_GUI_EVT_MOUSE_UP && ev->mouse_button == LENO_GUI_MOUSE_LEFT) {
        if (win->focused_textbox) {
            win->focused_textbox->dragging = 0;
            return 1;
        }
        return 0;
    }

    /* mouse wheel: vertical scroll for multiline */
    if (ev->type == LENO_GUI_EVT_MOUSE_WHEEL && win->focused_textbox && win->focused_textbox->multiline) {
        ObjGUITextBox* tb = win->focused_textbox;
        int visible_h = tb->height - 2 * tb->border_width - 2 * tb->padding_y;
        int total_h = tb_count_lines(tb->text) * tb->font_size;
        if (total_h > visible_h) {
            int delta = (int)(ev->wheel_y * tb->font_size * 3);
            tb->scroll_y += delta;
            if (tb->scroll_y < 0) tb->scroll_y = 0;
            if (tb->scroll_y > total_h - visible_h) tb->scroll_y = total_h - visible_h;
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
                if (clip && clip[0]) { tb_insert(tb, clip); if (!val_is_null(tb->on_change)) call_leno_closure(tb->on_change, 0, NULL); }
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
    TypeKind anchor_params[] = {TYPE_INT, TYPE_INT, TYPE_INT};

    textbox_register_method_with_params("set_text", make_native(tb_set_text, 2, "set_text"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_1);
    textbox_register_method_with_params("set_placeholder", make_native(tb_set_ph, 2, "set_placeholder"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_1);
    textbox_register_method_with_params("set_password", make_native(tb_set_pwd, 2, "set_password"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    textbox_register_method_with_params("set_max_length", make_native(tb_set_maxlen, 2, "set_max_length"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    textbox_register_method_with_params("on_change", make_native(tb_on_change, 2, "on_change"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("on_submit", make_native(tb_on_submit, 2, "on_submit"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    textbox_register_method_with_params("set_anchor", make_native(tb_set_anchor, 4, "set_anchor"), 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, anchor_params);
}
