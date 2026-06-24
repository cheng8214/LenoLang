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
static int tc_bytetochar(const char* s, int bp) {
    int cc = 0, p = 0; if (!s) return 0;
    while (p < bp && s[p]) { cc++; p = tc_next(s, p); } return cc;
}
static int tc_chartobyte(const char* s, int ci) {
    int p = 0, i = 0; if (!s) return 0;
    while (s[p] && i < ci) { p = tc_next(s, p); i++; } return p;
}

/* ===== Helpers ===== */
ObjGUITextBox* as_textbox(Value v) {
    if (!val_is_obj(v)) return NULL;
    return (ObjGUITextBox*)val_as_obj(v);
}
static int tc_cw(ObjGUITextBox* tb) { return (tb->font_size * 6) / 10; }
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
static void tb_del_sel(ObjGUITextBox* tb) {
    if (tb->sel_start < 0 || tb->sel_len <= 0) return;
    int ds = tb->sel_start, dl = tb->sel_len;
    if (ds > tb->text_len) ds = tb->text_len;
    if (ds + dl > tb->text_len) dl = tb->text_len - ds;
    memmove(tb->text + ds, tb->text + ds + dl, tb->text_len - ds - dl + 1);
    tb->text_len -= dl; tb->cursor_pos = ds;
    tb->sel_start = -1; tb->sel_len = 0;
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
}
static void tb_backspace(ObjGUITextBox* tb) {
    if (!tb || tb->cursor_pos <= 0) return;
    int pv = tc_prev(tb->text, tb->cursor_pos);
    int ln = tb->cursor_pos - pv;
    memmove(tb->text + pv, tb->text + tb->cursor_pos, tb->text_len - tb->cursor_pos + 1);
    tb->text_len -= ln; tb->cursor_pos = pv;
}
static void tb_del(ObjGUITextBox* tb) {
    if (!tb || tb->cursor_pos >= tb->text_len) return;
    int nx = tc_next(tb->text, tb->cursor_pos);
    int ln = nx - tb->cursor_pos;
    memmove(tb->text + tb->cursor_pos, tb->text + nx, tb->text_len - nx + 1);
    tb->text_len -= ln;
}
static int tb_mx2cp(ObjGUITextBox* tb, float mx) {
    int rx = (int)(mx - tb->x - tb->padding_x);
    int cw = tc_cw(tb); if (cw <= 0) cw = 1;
    int ci = rx / cw; if (ci < 0) ci = 0;
    int tc = tc_strlen(tb->text); if (ci > tc) ci = tc;
    return tc_chartobyte(tb->text, ci);
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
    /* selection highlight */
    int tx = dx + tb->padding_x, ty = dy + (dh - tb->font_size) / 2, cw = tc_cw(tb);
    if (tb->focused && tb->sel_start >= 0 && tb->sel_len > 0) {
        int sx = tx + tc_bytetochar(tb->text, tb->sel_start) * cw;
        int sw = tc_bytetochar(tb->text + tb->sel_start, tb->sel_len) * cw;
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
        int cx = tx + tc_bytetochar(tb->text, tb->cursor_pos) * cw;
        leno_gui_platform_set_draw_color(r, tb->cursor_r, tb->cursor_g, tb->cursor_b, tb->cursor_a);
        leno_gui_platform_render_fill_rect(r, cx, dy + 4, 2, dh - 8);
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

    /* mouse down: set focus */
    if (ev->type == LENO_GUI_EVT_MOUSE_DOWN && ev->mouse_button == LENO_GUI_MOUSE_LEFT) {
        if (win->focused_textbox) { win->focused_textbox->focused = 0; win->focused_textbox->blink_visible = 0; }
        tb = win->textboxes; ObjGUITextBox* hit = NULL;
        while (tb) {
            if (tb->visible && tb->enabled && tbox_hit(tb, ev->mouse_x, ev->mouse_y)) { hit = tb; break; }
            tb = tb->next;
        }
        if (hit) {
            hit->focused = 1; hit->blink_visible = 1; hit->last_blink = leno_gui_platform_get_ticks();
            hit->cursor_pos = tb_mx2cp(hit, ev->mouse_x); hit->sel_start = -1; hit->sel_len = 0;
            win->focused_textbox = hit; leno_gui_platform_start_text_input(); return 1;
        }
        win->focused_textbox = NULL; leno_gui_platform_stop_text_input(); return 0;
    }

    /* mouse move: drag selection */
    if (ev->type == LENO_GUI_EVT_MOUSE_MOVE) {
        tb = win->textboxes;
        while (tb) { tb->hovered = tbox_hit(tb, ev->mouse_x, ev->mouse_y); tb = tb->next; }
        if (win->focused_textbox) {
            /* drag selection could go here */
        }
        return 0;
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
            tb->sel_start = -1; tb->sel_len = 0; return 1;
        case LENO_GUI_KEY_RIGHT:
            if (ctrl) tb->cursor_pos = tb->text_len;
            else { tb->cursor_pos = tc_next(tb->text, tb->cursor_pos); if (tb->cursor_pos > tb->text_len) tb->cursor_pos = tb->text_len; }
            tb->sel_start = -1; tb->sel_len = 0; return 1;
        case LENO_GUI_KEY_HOME: tb->cursor_pos = 0; tb->sel_start = -1; tb->sel_len = 0; return 1;
        case LENO_GUI_KEY_END:  tb->cursor_pos = tb->text_len; tb->sel_start = -1; tb->sel_len = 0; return 1;
        case LENO_GUI_KEY_RETURN:
            if (!val_is_null(tb->on_submit)) { call_leno_closure(tb->on_submit, 0, NULL); }
            return 1;
        default: break;
        }
        /* Ctrl shortcuts */
        if (ctrl) {
            switch (ev->key) {
            case 'A': case 'a': tb->sel_start = 0; tb->sel_len = tb->text_len; tb->cursor_pos = tb->text_len; return 1;
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
        if (ev->text[0] == '\r' || ev->text[0] == '\n') return 1; /* handled by KEY_RETURN */
        if (ev->text[0] >= 0x20) {
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
