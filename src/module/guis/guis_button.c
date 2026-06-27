/* Leno GUI - GButton 按钮实例方法 + 绘制 + 事件处理
 *
 * GButton 实例方法 (btn.method()):
 *   btn.set_text(text)                  设置按钮文字
 *   btn.set_pos(x, y)                   设置按钮位置
 *   btn.set_size(w, h)                  设置按钮尺寸
 *   btn.set_visible(bool)               设置可见性
 *   btn.set_enabled(bool)               设置启用状态
 *   btn.set_opacity(val)                设置透明度 (0~255)
 *   btn.on_click(callback)              设置点击回调
 *   btn.close()                         移除按钮
 *
 * 样式字段 (win.add_button(style)):
 *   opacity                             透明度 (0~255, 默认255)
 *   border_width                        边框宽度 (0=无边框)
 *   border_color                        边框颜色 (GRgb)
 *   shadow_offset_x, shadow_offset_y    阴影偏移
 *   shadow_radius                       阴影模糊半径 (0=无阴影)
 *   shadow_color                        阴影颜色 (GRgb)
 *
 * 内部函数（供 win.run 调用）:
 *   gui_button_draw_all()               绘制所有按钮
 *   gui_button_handle_event()           处理按钮事件
 *   gui_button_free_all()               释放所有按钮
 */
#include "include/native.h"
#include "include/leno_value.h"
#include "guis_internal.h"
#include <string.h>

/* UTF-8 辅助：计算单个字符的字节长度 */
static int utf8_single_char_len(const char* s) {
    unsigned char c = (unsigned char)*s;
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* UTF-8 辅助：计算字符串中的字符数（非字节数） */
static int utf8_char_count(const char* s) {
    int count = 0;
    while (*s) {
        s += utf8_single_char_len(s);
        count++;
    }
    return count;
}

/* 光标名称 → 系统光标类型 */
static int cursor_name_to_type(const char* name) {
    if (!name || !name[0]) return -1;
    if (strcmp(name, "arrow") == 0)     return LENO_GUI_CURSOR_DEFAULT;
    if (strcmp(name, "default") == 0)   return LENO_GUI_CURSOR_DEFAULT;
    if (strcmp(name, "hand") == 0)      return LENO_GUI_CURSOR_POINTER;
    if (strcmp(name, "pointer") == 0)   return LENO_GUI_CURSOR_POINTER;
    if (strcmp(name, "ibeam") == 0)     return LENO_GUI_CURSOR_TEXT;
    if (strcmp(name, "text") == 0)      return LENO_GUI_CURSOR_TEXT;
    if (strcmp(name, "wait") == 0)      return LENO_GUI_CURSOR_WAIT;
    if (strcmp(name, "crosshair") == 0) return LENO_GUI_CURSOR_CROSSHAIR;
    if (strcmp(name, "progress") == 0)  return LENO_GUI_CURSOR_PROGRESS;
    if (strcmp(name, "resize_nwse") == 0) return LENO_GUI_CURSOR_RESIZE_NWSE;
    if (strcmp(name, "resize_nesw") == 0) return LENO_GUI_CURSOR_RESIZE_NESW;
    if (strcmp(name, "resize_ew") == 0)   return LENO_GUI_CURSOR_RESIZE_EW;
    if (strcmp(name, "resize_ns") == 0)   return LENO_GUI_CURSOR_RESIZE_NS;
    if (strcmp(name, "move") == 0)        return LENO_GUI_CURSOR_MOVE;
    if (strcmp(name, "not_allowed") == 0) return LENO_GUI_CURSOR_NOT_ALLOWED;
    return -1;
}

/* ============================================================================
 * GButton 实例方法
 * ============================================================================ */

/* btn.set_text(text) */
static Value btn_set_text_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();

    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_STRING) {
        ObjString* str = (ObjString*)val_as_obj(args[1]);
        if (btn->text) free(btn->text);
        btn->text = strdup(str->chars);
    }
    return val_null();
}

/* btn.set_pos(x, y) */
static Value btn_set_pos_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->x = val_as_int(args[1]);
    btn->y = val_as_int(args[2]);
    return val_null();
}

/* btn.set_size(w, h) */
static Value btn_set_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->width = val_as_int(args[1]);
    btn->height = val_as_int(args[2]);
    return val_null();
}

/* btn.set_visible(bool) */
static Value btn_set_visible_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->visible = val_as_bool(args[1]) ? 1 : 0;
    return val_null();
}

/* btn.set_enabled(bool) */
static Value btn_set_enabled_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->enabled = val_as_bool(args[1]) ? 1 : 0;
    return val_null();
}

/* btn.set_opacity(val) - 设置透明度 (0~255) */
static Value btn_set_opacity_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->opacity = val_as_int(args[1]);
    if (btn->opacity < 0) btn->opacity = 0;
    if (btn->opacity > 255) btn->opacity = 255;
    return val_null();
}

/* btn.set_anchor(anchor, margin_x, margin_y) - 设置锚点布局 */
/* anchor: 0=无, 1=左上, 2=右上, 3=左下, 4=右下, 5=居中, 6=上中, 7=下中 */
static Value btn_set_anchor_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->anchor = val_as_int(args[1]);
    btn->anchor_margin_x = val_as_int(args[2]);
    btn->anchor_margin_y = val_as_int(args[3]);
    return val_null();
}

/* 根据锚点重新计算按钮位置（窗口 resize 时调用） */
void gui_button_update_anchors(ObjGUIWindow* win, int win_w, int win_h) {
    if (!win) return;
    ObjGUIButton* btn = (ObjGUIButton*)win->buttons;
    while (btn) {
        if (btn->anchor > 0) {
            int mx = btn->anchor_margin_x;
            int my = btn->anchor_margin_y;
            switch (btn->anchor) {
                case 1: /* 左上 */     btn->x = mx; btn->y = my; break;
                case 2: /* 右上 */     btn->x = win_w - btn->width - mx; btn->y = my; break;
                case 3: /* 左下 */     btn->x = mx; btn->y = win_h - btn->height - my; break;
                case 4: /* 右下 */     btn->x = win_w - btn->width - mx; btn->y = win_h - btn->height - my; break;
                case 5: /* 居中 */     btn->x = (win_w - btn->width) / 2; btn->y = (win_h - btn->height) / 2; break;
                case 6: /* 上中 */     btn->x = (win_w - btn->width) / 2; btn->y = my; break;
                case 7: /* 下中 */     btn->x = (win_w - btn->width) / 2; btn->y = win_h - btn->height - my; break;
            }
        }
        btn = btn->next;
    }
}

/* btn.on_click(callback) - 设置点击回调 */
static Value btn_on_click_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn) return val_null();
    btn->on_click = args[1];
    gc_write_barrier((Object*)btn, args[1]);
    return val_null();
}

/* btn.close() - 从窗口移除按钮 */
static Value btn_close_func(int argc, Value* args) {
    (void)argc;
    ObjGUIButton* btn = as_button(args[0]);
    if (!btn || !btn->window) return val_null();

    /* 从窗口按钮链表中移除 */
    ObjGUIWindow* win = btn->window;
    ObjGUIButton** pp = (ObjGUIButton**)&win->buttons;
    while (*pp) {
        if (*pp == btn) {
            *pp = btn->next;
            win->button_count--;
            break;
        }
        pp = (ObjGUIButton**)&(*pp)->next;
    }
    if (btn->text) { free(btn->text); btn->text = NULL; }
    if (btn->font_name) { free(btn->font_name); btn->font_name = NULL; }
    if (btn->cursor) { free(btn->cursor); btn->cursor = NULL; }
    btn->next = NULL;
    btn->window = NULL;
    return val_null();
}

/* ============================================================================
 * 按钮绘制（自绘圆角矩形 + 文字）
 * ============================================================================ */

/* 绘制单个按钮 */
static void gui_button_draw_one(ObjGUIButton* btn, ObjGUIRenderer* ren) {
    if (!btn->visible || !ren || !ren->platform) return;

    /* 默认悬停/按下效果：未设置 hover_color/press_color 时降低透明度 */
    int no_hover = (btn->hover_r == btn->bg_r && btn->hover_g == btn->bg_g &&
                    btn->hover_b == btn->bg_b && btn->hover_a == btn->bg_a);
    int no_press = (btn->press_r == btn->bg_r && btn->press_g == btn->bg_g &&
                    btn->press_b == btn->bg_b && btn->press_a == btn->bg_a);

    /* 选择颜色：按下 > 悬停 > 默认 */
    int r, g, b, a;
    if (!btn->enabled) {
        /* 禁用状态：灰化 */
        r = 128; g = 128; b = 128; a = 200;
    } else if (btn->pressed) {
        if (no_press) {
            /* 未设置 press_color：保持 hover 颜色（如果悬停）或 bg 颜色 */
            if (btn->hovered) {
                r = btn->hover_r; g = btn->hover_g; b = btn->hover_b; a = btn->hover_a;
            } else {
                r = btn->bg_r; g = btn->bg_g; b = btn->bg_b; a = btn->bg_a;
            }
        } else {
            r = btn->press_r; g = btn->press_g; b = btn->press_b; a = btn->press_a;
        }
    } else if (btn->hovered) {
        r = btn->hover_r; g = btn->hover_g; b = btn->hover_b; a = btn->hover_a;
    } else {
        r = btn->bg_r; g = btn->bg_g; b = btn->bg_b; a = btn->bg_a;
    }

    /* 应用整体透明度 */
    int opacity = btn->opacity;
    if (btn->enabled && !btn->pressed && btn->hovered && no_hover) {
        if (btn->gradient_count < 2) {
            opacity = (opacity * 200) / 255;  /* 纯色按钮：悬停变淡 */
        }
    } else if (btn->enabled && btn->pressed && no_press) {
        if (btn->gradient_count < 2) {
            opacity = (opacity * 170) / 255;  /* 纯色按钮：按下更淡 */
        }
    }
    if (opacity < 255) {
        a = (a * opacity) / 255;
    }

    /* 计算绘制位置和尺寸（支持悬停缩放） */
    int dx = btn->x, dy = btn->y, dw = btn->width, dh = btn->height, dr = btn->radius;
    if (btn->hovered && btn->hover_scale > 0.0f && btn->hover_scale != 1.0f) {
        float s = btn->hover_scale;
        int cx = btn->x + btn->width / 2;
        int cy = btn->y + btn->height / 2;
        dw = (int)(btn->width * s);
        dh = (int)(btn->height * s);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        dx = cx - dw / 2;
        dy = cy - dh / 2;
        dr = (int)(btn->radius * s);
    }

    /* 模拟按下：未设置 press_color 时整体向下偏移 */
    if (btn->enabled && btn->pressed && no_press && btn->press_effect) {
        dy += btn->press_offset;
    }

    /* 1. 绘制阴影（在按钮下方） */
    if (btn->shadow_radius > 0 && btn->shadow_a > 0) {
        leno_gui_platform_render_draw_shadow(ren->platform,
            dx, dy, dw, dh, dr,
            btn->shadow_offset_x, btn->shadow_offset_y, btn->shadow_radius,
            (uint8_t)btn->shadow_r, (uint8_t)btn->shadow_g,
            (uint8_t)btn->shadow_b, (uint8_t)btn->shadow_a);
    }

    /* 2. 绘制背景（渐变或纯色） */
    if (btn->gradient_count >= 2) {
        uint8_t gr[4], gg[4], gb[4], ga[4];
        for (int i = 0; i < btn->gradient_count && i < 4; i++) {
            gr[i] = (uint8_t)btn->gradient_r[i];
            gg[i] = (uint8_t)btn->gradient_g[i];
            gb[i] = (uint8_t)btn->gradient_b[i];
            ga[i] = (uint8_t)btn->gradient_a[i];
        }
        if (btn->gradient_radial) {
            leno_gui_platform_render_fill_gradient_radial_rect(ren->platform,
                dx, dy, dw, dh, dr,
                btn->gradient_count, gr, gg, gb, ga, opacity);
        } else {
            leno_gui_platform_render_fill_gradient_rounded_rect(ren->platform,
                dx, dy, dw, dh, dr,
                btn->gradient_count, gr, gg, gb, ga, opacity, 0);
        }
    } else {
        /* 纯色背景 */
        leno_gui_platform_set_draw_color(ren->platform, r, g, b, a);
        leno_gui_platform_render_fill_rounded_rect(ren->platform, dx, dy, dw, dh, dr);
        /* 纯色按钮默认按下效果：叠加黑色层 */
        if (btn->enabled && btn->pressed && no_press) {
            int ov = (40 * opacity) / 255;
            if (ov > 0) {
                leno_gui_platform_set_draw_color(ren->platform, 0, 0, 0, ov);
                leno_gui_platform_render_fill_rounded_rect(ren->platform, dx, dy, dw, dh, dr);
            }
        }
    }

    /* 渐变按钮的悬停/按下效果：叠加 hover_color/press_color 或默认白/黑层 */
    if (btn->gradient_count >= 2 && btn->enabled) {
        if (btn->pressed) {
            if (!no_press) {
                int pa = (btn->press_a * opacity) / 255;
                if (pa > 0) {
                    leno_gui_platform_set_draw_color(ren->platform, btn->press_r, btn->press_g, btn->press_b, pa);
                    leno_gui_platform_render_fill_rounded_rect(ren->platform, dx, dy, dw, dh, dr);
                }
            } else {
                int ov = (40 * opacity) / 255;
                leno_gui_platform_set_draw_color(ren->platform, 0, 0, 0, ov);
                leno_gui_platform_render_fill_rounded_rect(ren->platform, dx, dy, dw, dh, dr);
            }
        } else if (btn->hovered) {
            if (!no_hover) {
                int ha = (btn->hover_a * opacity) / 255;
                if (ha > 0) {
                    leno_gui_platform_set_draw_color(ren->platform, btn->hover_r, btn->hover_g, btn->hover_b, ha);
                    leno_gui_platform_render_fill_rounded_rect(ren->platform, dx, dy, dw, dh, dr);
                }
            } else {
                int ov = (30 * opacity) / 255;
                leno_gui_platform_set_draw_color(ren->platform, 255, 255, 255, ov);
                leno_gui_platform_render_fill_rounded_rect(ren->platform, dx, dy, dw, dh, dr);
            }
        }
    }

    /* 3. 绘制边框 */
    if (btn->border_width > 0) {
        int ba = btn->border_a;
        if (opacity < 255) ba = (ba * opacity) / 255;
        leno_gui_platform_set_draw_color(ren->platform, btn->border_r, btn->border_g, btn->border_b, ba);

        if (btn->border_style == 1) {
            /* 虚线边框 */
            for (int i = 0; i < btn->border_width; i++) {
                int dash = 6, gap = 4;
                int bw = dw - 2 * i;
                int bh = dh - 2 * i;
                int bx = dx + i;
                int by = dy + i;
                /* 上边 */
                int pos = 0;
                while (pos < bw) {
                    for (int k = 0; k < dash && pos + k < bw; k++)
                        leno_gui_platform_render_draw_point(ren->platform, bx + pos + k, by);
                    pos += dash + gap;
                }
                /* 下边 */
                pos = 0;
                while (pos < bw) {
                    for (int k = 0; k < dash && pos + k < bw; k++)
                        leno_gui_platform_render_draw_point(ren->platform, bx + pos + k, by + bh - 1);
                    pos += dash + gap;
                }
                /* 左边 */
                pos = 0;
                while (pos < bh) {
                    for (int k = 0; k < dash && pos + k < bh; k++)
                        leno_gui_platform_render_draw_point(ren->platform, bx, by + pos + k);
                    pos += dash + gap;
                }
                /* 右边 */
                pos = 0;
                while (pos < bh) {
                    for (int k = 0; k < dash && pos + k < bh; k++)
                        leno_gui_platform_render_draw_point(ren->platform, bx + bw - 1, by + pos + k);
                    pos += dash + gap;
                }
            }
        } else if (btn->border_style == 2) {
            /* 点状边框 */
            for (int i = 0; i < btn->border_width; i++) {
                int gap = 4;
                int bw = dw - 2 * i;
                int bh = dh - 2 * i;
                int bx = dx + i;
                int by = dy + i;
                for (int pos = 0; pos < bw; pos += gap) {
                    leno_gui_platform_render_draw_point(ren->platform, bx + pos, by);
                    leno_gui_platform_render_draw_point(ren->platform, bx + pos, by + bh - 1);
                }
                for (int pos = 0; pos < bh; pos += gap) {
                    leno_gui_platform_render_draw_point(ren->platform, bx, by + pos);
                    leno_gui_platform_render_draw_point(ren->platform, bx + bw - 1, by + pos);
                }
            }
        } else {
            /* 实线边框 */
            for (int i = 0; i < btn->border_width; i++) {
                leno_gui_platform_render_draw_rounded_rect(ren->platform,
                    dx + i, dy + i,
                    dw - 2 * i, dh - 2 * i,
                    dr > i ? dr - i : 0);
            }
        }
    }

    /* 4. 绘制文字 */
    if (btn->text && btn->font && btn->font->platform) {
        int ta = btn->text_a;
        if (opacity < 255) ta = (ta * opacity) / 255;
        leno_gui_platform_set_draw_color(ren->platform, btn->text_r, btn->text_g, btn->text_b, ta);

        /* 计算文字尺寸（含字间距） */
        int tw = 0, th = 0;
        int char_count = utf8_char_count(btn->text);
        int extra_spacing = btn->letter_spacing * (char_count > 0 ? char_count - 1 : 0);
        leno_gui_platform_text_size_font(ren->platform, btn->font->platform, btn->text, &tw, &th);
        tw += extra_spacing;

        /* 根据对齐方式计算文字位置（考虑内边距） */
        int content_x = dx + btn->padding_x;
        int content_w = dw - 2 * btn->padding_x;
        int tx, ty;

        if (btn->text_align == 0) {
            /* 左对齐 */
            tx = content_x;
        } else if (btn->text_align == 2) {
            /* 右对齐 */
            tx = content_x + content_w - tw;
        } else {
            /* 居中 */
            tx = dx + (dw - tw) / 2;
        }
        ty = dy + (dh - th) / 2;

        /* 绘制文字（含字间距） */
        if (btn->letter_spacing > 0 && char_count > 1) {
            int cx = tx;
            const char* p = btn->text;
            while (*p) {
                int clen = utf8_single_char_len(p);
                char ch[5] = {0};
                memcpy(ch, p, clen);
                int cw = 0, chh = 0;
                leno_gui_platform_text_size_font(ren->platform, btn->font->platform, ch, &cw, &chh);
                leno_gui_platform_draw_text_font(ren->platform, btn->font->platform, ch, cx, ty);
                cx += cw + btn->letter_spacing;
                p += clen;
            }
        } else {
            leno_gui_platform_draw_text_font(ren->platform, btn->font->platform, btn->text, tx, ty);
        }

        /* 文字装饰线 */
        if (btn->text_decoration > 0 && char_count > 0) {
            int line_y;
            int line_w = tw;
            if (line_w > content_w) line_w = content_w;
            int line_x = tx;
            if (line_x < content_x) line_x = content_x;

            switch (btn->text_decoration) {
                case 1: /* underline */
                    line_y = ty + th + 2;
                    break;
                case 2: /* strikethrough */
                    line_y = ty + th / 2;
                    break;
                case 3: /* overline */
                    line_y = ty - 2;
                    break;
                default:
                    line_y = ty + th + 2;
            }

            if (line_y >= dy && line_y < dy + dh) {
                leno_gui_platform_set_draw_color(ren->platform, btn->text_r, btn->text_g, btn->text_b, ta);
                for (int i = 0; i < line_w && line_x + i < dx + dw - btn->padding_x; i++) {
                    leno_gui_platform_render_draw_point(ren->platform, line_x + i, line_y);
                }
            }
        }
    }

    /* 5. 焦点样式 */
    if (btn->focus_width > 0 && btn->focus_a > 0) {
        int fw = btn->focus_width;
        leno_gui_platform_set_draw_color(ren->platform, btn->focus_r, btn->focus_g, btn->focus_b, btn->focus_a);
        for (int i = 0; i < fw; i++) {
            leno_gui_platform_render_draw_rounded_rect(ren->platform,
                dx - fw + i, dy - fw + i,
                dw + 2 * (fw - i), dh + 2 * (fw - i),
                dr + fw - i);
        }
    }
}

/* 绘制窗口上所有按钮 */
void gui_button_draw_all(ObjGUIWindow* win, ObjGUIRenderer* ren) {
    if (!win || !ren) return;
    ObjGUIButton* btn = (ObjGUIButton*)win->buttons;
    while (btn) {
        gui_button_draw_one(btn, ren);
        btn = btn->next;
    }
}

/* ============================================================================
 * 按钮事件处理
 * ============================================================================ */

/* 检测点是否在按钮区域内 */
static int point_in_button(ObjGUIButton* btn, float mx, float my) {
    return mx >= btn->x && mx <= btn->x + btn->width &&
           my >= btn->y && my <= btn->y + btn->height;
}

/* 处理窗口上所有按钮的事件，返回 1 表示事件被按钮消费 */
int gui_button_handle_event(ObjGUIWindow* win, LenoGUIEvent* ev) {
    if (!win || !ev) return 0;

    int consumed = 0;
    ObjGUIButton* btn = (ObjGUIButton*)win->buttons;

    while (btn) {
        if (!btn->visible || !btn->enabled) {
            btn = btn->next;
            continue;
        }

        if (ev->type == LENO_GUI_EVT_MOUSE_MOVE) {
            int was_hovered = btn->hovered;
            btn->hovered = point_in_button(btn, ev->mouse_x, ev->mouse_y);
            if (btn->hovered && !was_hovered) {
                consumed = 1;
                if (btn->cursor && btn->cursor[0]) {
                    int ct = cursor_name_to_type(btn->cursor);
                    if (ct >= 0) leno_gui_platform_set_system_cursor(ct);
                }
            } else if (!btn->hovered && was_hovered) {
                if (btn->cursor && btn->cursor[0]) {
                    leno_gui_platform_set_system_cursor(LENO_GUI_CURSOR_DEFAULT);
                }
            }
        } else if (ev->type == LENO_GUI_EVT_MOUSE_DOWN && ev->mouse_button == LENO_GUI_MOUSE_LEFT) {
            if (point_in_button(btn, ev->mouse_x, ev->mouse_y)) {
                btn->pressed = 1;
                consumed = 1;
            }
        } else if (ev->type == LENO_GUI_EVT_MOUSE_UP && ev->mouse_button == LENO_GUI_MOUSE_LEFT) {
            if (btn->pressed && point_in_button(btn, ev->mouse_x, ev->mouse_y)) {
                /* 触发点击回调 */
                if (!val_is_null(btn->on_click)) {
                    call_leno_closure(btn->on_click, 0, NULL);
                }
                consumed = 1;
            }
            btn->pressed = 0;
        }

        btn = btn->next;
    }
    return consumed;
}

/* 释放窗口上所有按钮资源 */
void gui_button_free_all(ObjGUIWindow* win) {
    if (!win) return;
    ObjGUIButton* btn = (ObjGUIButton*)win->buttons;
    while (btn) {
        ObjGUIButton* next = btn->next;
        if (btn->text) { free(btn->text); btn->text = NULL; }
        if (btn->font_name) { free(btn->font_name); btn->font_name = NULL; }
        if (btn->cursor) { free(btn->cursor); btn->cursor = NULL; }
        btn = next;
    }
    win->buttons = NULL;
    win->button_count = 0;
}

/* ============================================================================
 * as_button 辅助函数
 * ============================================================================ */

ObjGUIButton* as_button(Value v) {
    if (!val_is_obj(v)) return NULL;
    Object* obj = val_as_obj(v);
    if (obj->type != OBJ_GUI_BUTTON) return NULL;
    return (ObjGUIButton*)obj;
}

/* ============================================================================
 * 注册 GButton 实例方法
 * ============================================================================ */

extern void button_register_method_with_params(const char* name, ObjNative* method, int arity,
                                                int min_arity, int max_arity,
                                                TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
extern ObjNative* make_native(NativeFn fn, int arity, const char* name);
extern void button_init_methods(void);

void guis_init_button_instance_methods(void) {
    button_init_methods();

    TypeKind no_params[] = {};
    TypeKind str_1[] = {TYPE_STRING};
    TypeKind int_1[] = {TYPE_INT};
    TypeKind int_2[] = {TYPE_INT, TYPE_INT};
    TypeKind bool_1[] = {TYPE_BOOL};
    TypeKind any_1[] = {TYPE_ANY};

    button_register_method_with_params("set_text", make_native(btn_set_text_func, 2, "set_text"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_1);
    button_register_method_with_params("set_pos", make_native(btn_set_pos_func, 3, "set_pos"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2);
    button_register_method_with_params("set_size", make_native(btn_set_size_func, 3, "set_size"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2);
    button_register_method_with_params("set_visible", make_native(btn_set_visible_func, 2, "set_visible"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    button_register_method_with_params("set_enabled", make_native(btn_set_enabled_func, 2, "set_enabled"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    button_register_method_with_params("set_opacity", make_native(btn_set_opacity_func, 2, "set_opacity"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_1);
    button_register_method_with_params("on_click", make_native(btn_on_click_func, 2, "on_click"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    button_register_method_with_params("close", make_native(btn_close_func, 1, "close"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);

    /* 锚点布局 */
    TypeKind anchor_params[] = {TYPE_INT, TYPE_INT, TYPE_INT};  /* anchor, margin_x, margin_y */
    button_register_method_with_params("set_anchor", make_native(btn_set_anchor_func, 4, "set_anchor"), 3, -1, -1, TYPE_NULL, TYPE_UNKNOWN, anchor_params);
}
