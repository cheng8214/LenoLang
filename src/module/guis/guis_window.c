/* Leno GUI - Win 窗口实例方法
 * 从 guis.c 拆分出来的 Win 方法实现
 *
 * Win 实例方法 (win.method()):
 *   win.show()                          显示窗口
 *   win.hide()                          隐藏窗口
 *   win.close()                         关闭窗口（销毁 + 退出 GUI）
 *   win.set_title(title)                设置窗口标题
 *   win.set_size(w, h)                  设置窗口大小
 *   win.get_size() -> [w, h]            获取窗口大小
 *   win.set_pos(x, y)                   设置窗口位置
 *   win.get_pos() -> [x, y]            获取窗口位置
 *   win.center()                        窗口居中显示
 *   win.set_min_size(w, h)              设置最小尺寸
 *   win.set_max_size(w, h)              设置最大尺寸
 *   win.set_fullscreen(bool)            设置全屏
 *   win.set_maximized(bool)             设置最大化
 *   win.set_icon(path)                  设置窗口图标
 *   win.set_bg_color(rgb)               设置背景颜色
 *   win.set_vsync(bool)                 设置垂直同步
 *   win.should_close() -> bool          是否应该关闭
 *   win.set_should_close(bool)          设置关闭标志
 *   win.set_opacity(opacity)            设置透明度
 *   win.run(on_draw, on_event)          运行事件循环
 */
#include "include/native.h"
#include "include/leno_value.h"
#include "include/string_table.h"
#include "guis_internal.h"
#include <string.h>

#define TEXTBOX_DEFAULT_CAP 256

/* ============================================================================
 * Win 窗口实例方法（win.method() 风格）
 * ============================================================================ */

/* win.show() */
static Value win_show_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (win && win->platform) leno_gui_platform_show_window(win->platform);
    return val_null();
}

/* win.hide() */
static Value win_hide_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (win && win->platform) leno_gui_platform_hide_window(win->platform);
    return val_null();
}

/* win.close() - 销毁窗口 + 退出 GUI */
static Value win_close_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (win && win->platform) {
        leno_gui_platform_destroy_window(win->platform);
        win->platform = NULL;
    }
    leno_gui_platform_quit();
    return val_null();
}

/* win.set_title(title) */
static Value win_set_title_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    ObjString* title = (ObjString*)val_as_obj(args[1]);
    if (win && win->platform) leno_gui_platform_set_window_title(win->platform, title->chars);
    return val_null();
}

/* win.set_size(w, h) */
static Value win_set_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int w = val_as_int(args[1]);
    int h = val_as_int(args[2]);
    if (win && win->platform) leno_gui_platform_set_window_size(win->platform, w, h);
    return val_null();
}

/* win.get_size() -> [w, h] */
static Value win_get_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int w = 0, h = 0;
    if (win && win->platform) leno_gui_platform_get_window_size(win->platform, &w, &h);
    return val_obj((Object*)make_int_array2(w, h));
}

/* win.set_pos(x, y) */
static Value win_set_pos_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int x = val_as_int(args[1]);
    int y = val_as_int(args[2]);
    if (win && win->platform) leno_gui_platform_set_window_position(win->platform, x, y);
    return val_null();
}

/* win.get_pos() -> [x, y] */
static Value win_get_pos_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int x = 0, y = 0;
    if (win && win->platform) leno_gui_platform_get_window_position(win->platform, &x, &y);
    return val_obj((Object*)make_int_array2(x, y));
}

/* win.set_fullscreen(bool) */
static Value win_set_fullscreen_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int fullscreen = val_as_bool(args[1]) ? 1 : 0;
    if (win && win->platform) leno_gui_platform_set_window_fullscreen(win->platform, fullscreen);
    return val_null();
}

/* win.should_close() -> bool */
static Value win_should_close_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (win && win->platform) return val_bool(leno_gui_platform_window_should_close(win->platform) != 0);
    return val_bool(true);
}

/* win.set_should_close(bool) */
static Value win_set_should_close_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int val = val_as_bool(args[1]) ? 1 : 0;
    if (win && win->platform) leno_gui_platform_set_window_should_close(win->platform, val);
    return val_null();
}

/* win.set_opacity(opacity) */
static Value win_set_opacity_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    float opacity = (float)val_as_double(args[1]);
    if (win && win->platform) leno_gui_platform_set_window_opacity(win->platform, opacity);
    return val_null();
}

/* win.set_drag_area(x, y, w, h) */
static Value win_set_drag_area_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int x = val_as_int(args[1]);
    int y = val_as_int(args[2]);
    int w = val_as_int(args[3]);
    int h = val_as_int(args[4]);
    if (win && win->platform) leno_gui_platform_set_window_drag_area(win->platform, x, y, w, h);
    return val_null();
}

/* win.clear_drag_area() */
static Value win_clear_drag_area_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (win && win->platform) leno_gui_platform_clear_window_drag_area(win->platform);
    return val_null();
}

/* win.center() - 居中窗口到屏幕中央 */
static Value win_center_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (!win || !win->platform) return val_null();

    int display_w = 0, display_h = 0;
    leno_gui_platform_get_display_size(&display_w, &display_h);

    int ww = 0, wh = 0;
    leno_gui_platform_get_window_size(win->platform, &ww, &wh);

    int cx = (display_w - ww) / 2;
    int cy = (display_h - wh) / 2;
    leno_gui_platform_set_window_position(win->platform, cx, cy);
    return val_null();
}

/* win.set_min_size(w, h) */
static Value win_set_min_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int min_w = val_as_int(args[1]);
    int min_h = val_as_int(args[2]);
    if (win && win->platform) leno_gui_platform_set_window_minimum_size(win->platform, min_w, min_h);
    return val_null();
}

/* win.set_max_size(w, h) */
static Value win_set_max_size_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int max_w = val_as_int(args[1]);
    int max_h = val_as_int(args[2]);
    if (win && win->platform) leno_gui_platform_set_window_maximum_size(win->platform, max_w, max_h);
    return val_null();
}

/* win.set_maximized(bool) */
static Value win_set_maximized_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int maximized = val_as_bool(args[1]) ? 1 : 0;
    if (win && win->platform) {
        if (maximized) {
            leno_gui_platform_show_window(win->platform);  /* SDL3: SDL_MaximizeWindow */
        }
        /* 注意: 平台层目前通过 WS_MAXIMIZE 样式的 create_window 支持最大化，
         * 运行时最大化需要在平台层添加对应的 leno_gui_platform_maximize_window 函数。
         * 这里先通过设置 fullscreen 变通实现，后续应添加专门的 maximize 平台函数。 */
        leno_gui_platform_set_window_fullscreen(win->platform, maximized);
    }
    return val_null();
}

/* win.set_icon(path) - 设置窗口图标 */

static Value win_set_icon_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    ObjString* path = (ObjString*)val_as_obj(args[1]);
    if (!win || !win->platform || !path || !path->chars) return val_null();

    int icon_w = 0, icon_h = 0;
    unsigned char* icon_data = leno_gui_platform_load_raw_pixels(path->chars, &icon_w, &icon_h);
    if (icon_data) {
        leno_gui_platform_set_window_icon(win->platform, (uint32_t*)icon_data, icon_w, icon_h);
        leno_gui_platform_free_raw_pixels(icon_data);
    }
    return val_null();
}

/* win.set_bg_color(rgb) */
static Value win_set_bg_color_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (!win) return val_null();

    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_RGB) {
        ObjRgb* rgb = (ObjRgb*)val_as_obj(args[1]);
        win->bg_r = rgb->r;
        win->bg_g = rgb->g;
        win->bg_b = rgb->b;
        win->bg_a = rgb->a;
        win->use_bg_color = 1;
    }
    return val_null();
}

/* win.clear_bg_color() - 清除自定义背景色 */
static Value win_clear_bg_color_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (win) win->use_bg_color = 0;
    return val_null();
}

/* win.set_vsync(bool) */
static Value win_set_vsync_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int enabled = val_as_bool(args[1]) ? 1 : 0;
    if (win) win->vsync_enabled = enabled;
    return val_null();
}

/* win.set_maximizable(bool) - 设置是否允许最大化 */
static Value win_set_maximizable_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    int allow = val_as_bool(args[1]) ? 1 : 0;
    if (win && win->platform) {
        leno_gui_platform_set_window_maximizable(win->platform, allow);
    }
    return val_null();
}

/* win.add_button(style) -> GButton - 创建按钮并添加到窗口 */
static Value win_add_button_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (!win) return val_null();

    /* 默认值 */
    int x = 0, y = 0, width = 100, height = 36;
    char* text = strdup("Button");
    int bg_r = 70, bg_g = 130, bg_b = 180, bg_a = 255;
    int hover_r = 100, hover_g = 160, hover_b = 210, hover_a = 255;
    int press_r = 50, press_g = 110, press_b = 160, press_a = 255;
    int text_r = 255, text_g = 255, text_b = 255, text_a = 255;
    char* font_name = strdup("Arial");
    int font_size = 16;
    int padding_x = 0, padding_y = 0;
    int text_align = 1;  /* 默认居中 */
    int font_bold = 0;
    int letter_spacing = 0;
    int radius = 6;
    int radius_tl = 0, radius_tr = 0, radius_bl = 0, radius_br = 0;
    int opacity = 255;
    int gradient_count = 0;
    int gradient_r[4] = {0}, gradient_g[4] = {0}, gradient_b[4] = {0}, gradient_a[4] = {0};
    int gradient_radial = 0;
    int border_width = 0;
    int border_r = 0, border_g = 0, border_b = 0, border_a = 255;
    int border_style = 0;  /* solid */
    int shadow_offset_x = 0, shadow_offset_y = 2;
    int shadow_radius = 0;
    int shadow_r = 0, shadow_g = 0, shadow_b = 0, shadow_a = 80;
    int text_decoration = 0;
    int focus_width = 0;
    int focus_r = 100, focus_g = 180, focus_b = 255, focus_a = 255;
    float hover_scale = 1.0f;
    int loading = 0;
    char* cursor = strdup("");
    int press_effect = 1;   /* 默认开启按下效果 */
    int press_offset = 1;   /* 默认偏移1像素 */

    /* 解析 style 字典 */
    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_DICT) {
        ObjDict* style = (ObjDict*)val_as_obj(args[1]);

        /* x, y, width, height */
        ObjString* key = intern_string("x", 1);
        Value v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) x = val_as_int(v);
        key = intern_string("y", 1);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) y = val_as_int(v);
        key = intern_string("width", 5);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) width = val_as_int(v);
        key = intern_string("height", 6);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) height = val_as_int(v);

        /* text */
        key = intern_string("text", 4);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
            free(text);
            text = strdup(((ObjString*)val_as_obj(v))->chars);
        }

        /* bg_color */
        key = intern_string("bg_color", 8);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_RGB) {
            ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
            bg_r = rgb->r; bg_g = rgb->g; bg_b = rgb->b; bg_a = rgb->a;
            /* 默认 press_color = bg_color，未设置时模拟按下而不是跳变色 */
            press_r = bg_r; press_g = bg_g; press_b = bg_b; press_a = bg_a;
        }

        /* hover_color */
        key = intern_string("hover_color", 11);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_RGB) {
            ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
            hover_r = rgb->r; hover_g = rgb->g; hover_b = rgb->b; hover_a = rgb->a;
        }

        /* press_color */
        key = intern_string("press_color", 11);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_RGB) {
            ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
            press_r = rgb->r; press_g = rgb->g; press_b = rgb->b; press_a = rgb->a;
        }

        /* text_color */
        key = intern_string("text_color", 10);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_RGB) {
            ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
            text_r = rgb->r; text_g = rgb->g; text_b = rgb->b; text_a = rgb->a;
        }

        /* font */
        key = intern_string("font", 4);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
            free(font_name);
            font_name = strdup(((ObjString*)val_as_obj(v))->chars);
        }

        /* font_size */
        key = intern_string("font_size", 9);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) font_size = val_as_int(v);

        /* radius */
        key = intern_string("radius", 6);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) radius = val_as_int(v);

        /* opacity (0~255) */
        key = intern_string("opacity", 7);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) opacity = val_as_int(v);

        /* border_width */
        key = intern_string("border_width", 12);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) border_width = val_as_int(v);

        /* border_color */
        key = intern_string("border_color", 12);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_RGB) {
            ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
            border_r = rgb->r; border_g = rgb->g; border_b = rgb->b; border_a = rgb->a;
        }

        /* shadow_offset_x */
        key = intern_string("shadow_offset_x", 15);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) shadow_offset_x = val_as_int(v);

        /* shadow_offset_y */
        key = intern_string("shadow_offset_y", 15);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) shadow_offset_y = val_as_int(v);

        /* shadow_radius */
        key = intern_string("shadow_radius", 13);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) shadow_radius = val_as_int(v);

        /* shadow_color */
        key = intern_string("shadow_color", 12);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_RGB) {
            ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
            shadow_r = rgb->r; shadow_g = rgb->g; shadow_b = rgb->b; shadow_a = rgb->a;
        }

        /* padding */
        key = intern_string("padding_x", 9);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) padding_x = val_as_int(v);
        key = intern_string("padding_y", 9);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) padding_y = val_as_int(v);

        /* text_align */
        key = intern_string("text_align", 10);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
            char* s = ((ObjString*)val_as_obj(v))->chars;
            if (strcmp(s, "left") == 0) text_align = 0;
            else if (strcmp(s, "right") == 0) text_align = 2;
            else text_align = 1;
        }

        /* font_bold */
        key = intern_string("font_bold", 9);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) font_bold = val_as_bool(v) ? 1 : 0;

        /* letter_spacing */
        key = intern_string("letter_spacing", 14);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) letter_spacing = val_as_int(v);

        /* 独立圆角 */
        key = intern_string("radius_tl", 9);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) radius_tl = val_as_int(v);
        key = intern_string("radius_tr", 9);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) radius_tr = val_as_int(v);
        key = intern_string("radius_bl", 9);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) radius_bl = val_as_int(v);
        key = intern_string("radius_br", 9);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) radius_br = val_as_int(v);

        /* gradient */
        key = intern_string("gradient", 8);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) {
            if (val_is_obj(v) && val_as_obj(v)->type == OBJ_ARRAY) {
                /* 简写: gradient: [color1, color2] */
                ObjArray* arr = (ObjArray*)val_as_obj(v);
                gradient_count = arr->count < 4 ? (int)arr->count : 4;
                for (int i = 0; i < gradient_count; i++) {
                    Value cv = arr->elements[i];
                    if (val_is_obj(cv) && val_as_obj(cv)->type == OBJ_RGB) {
                        ObjRgb* rgb = (ObjRgb*)val_as_obj(cv);
                        gradient_r[i] = rgb->r; gradient_g[i] = rgb->g;
                        gradient_b[i] = rgb->b; gradient_a[i] = rgb->a;
                    }
                }
                gradient_radial = 0;
            } else if (val_is_obj(v) && val_as_obj(v)->type == OBJ_DICT) {
                /* 完整写法: gradient: { colors: [...], radial: true } */
                ObjDict* grad = (ObjDict*)val_as_obj(v);
                ObjString* ckey = intern_string("colors", 6);
                Value cv = dict_get(grad, val_obj((Object*)ckey));
                if (!val_is_null(cv) && val_is_obj(cv) && val_as_obj(cv)->type == OBJ_ARRAY) {
                    ObjArray* arr = (ObjArray*)val_as_obj(cv);
                    gradient_count = arr->count < 4 ? (int)arr->count : 4;
                    for (int i = 0; i < gradient_count; i++) {
                        Value gv = arr->elements[i];
                        if (val_is_obj(gv) && val_as_obj(gv)->type == OBJ_RGB) {
                            ObjRgb* rgb = (ObjRgb*)val_as_obj(gv);
                            gradient_r[i] = rgb->r; gradient_g[i] = rgb->g;
                            gradient_b[i] = rgb->b; gradient_a[i] = rgb->a;
                        }
                    }
                }
                ObjString* rkey = intern_string("radial", 6);
                Value rv = dict_get(grad, val_obj((Object*)rkey));
                if (!val_is_null(rv)) gradient_radial = val_as_bool(rv) ? 1 : 0;
            }
        }

        /* border_style */
        key = intern_string("border_style", 12);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
            char* s = ((ObjString*)val_as_obj(v))->chars;
            if (strcmp(s, "dashed") == 0) border_style = 1;
            else if (strcmp(s, "dotted") == 0) border_style = 2;
            else border_style = 0;
        }

        /* text_decoration */
        key = intern_string("text_decoration", 15);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
            char* s = ((ObjString*)val_as_obj(v))->chars;
            if (strcmp(s, "underline") == 0) text_decoration = 1;
            else if (strcmp(s, "strikethrough") == 0) text_decoration = 2;
            else if (strcmp(s, "overline") == 0) text_decoration = 3;
            else text_decoration = 0;
        }

        /* focus_width, focus_color */
        key = intern_string("focus_width", 11);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) focus_width = val_as_int(v);
        key = intern_string("focus_color", 11);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_RGB) {
            ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
            focus_r = rgb->r; focus_g = rgb->g; focus_b = rgb->b; focus_a = rgb->a;
        }

        /* hover_scale */
        key = intern_string("hover_scale", 11);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) hover_scale = (float)val_as_double(v);

        /* loading */
        key = intern_string("loading", 7);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) loading = val_as_bool(v) ? 1 : 0;

        /* cursor */
        key = intern_string("cursor", 6);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
            free(cursor);
            cursor = strdup(((ObjString*)val_as_obj(v))->chars);
        }

        /* press_effect */
        key = intern_string("press_effect", 12);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) press_effect = val_as_bool(v) ? 1 : 0;

        /* press_offset */
        key = intern_string("press_offset", 12);
        v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) press_offset = val_as_int(v);
    }

    /* 创建按钮对象 */
    ObjGUIButton* btn = (ObjGUIButton*)gc_alloc(sizeof(ObjGUIButton), OBJ_GUI_BUTTON);
    if (!btn) {
        free(text);
        free(font_name);
        free(cursor);
        return val_null();
    }

    btn->window = win;
    btn->next = NULL;
    btn->x = x;
    btn->y = y;
    btn->width = width;
    btn->height = height;
    btn->text = text;
    btn->bg_r = bg_r; btn->bg_g = bg_g; btn->bg_b = bg_b; btn->bg_a = bg_a;
    btn->hover_r = hover_r; btn->hover_g = hover_g; btn->hover_b = hover_b; btn->hover_a = hover_a;
    btn->press_r = press_r; btn->press_g = press_g; btn->press_b = press_b; btn->press_a = press_a;
    btn->text_r = text_r; btn->text_g = text_g; btn->text_b = text_b; btn->text_a = text_a;
    btn->font_name = font_name;
    btn->font_size = font_size;
    btn->font = NULL;
    btn->padding_x = padding_x;
    btn->padding_y = padding_y;
    btn->text_align = text_align;
    btn->font_bold = font_bold;
    btn->letter_spacing = letter_spacing;
    btn->radius = radius;
    btn->radius_tl = radius_tl;
    btn->radius_tr = radius_tr;
    btn->radius_bl = radius_bl;
    btn->radius_br = radius_br;
    btn->opacity = opacity;
    btn->gradient_count = gradient_count;
    for (int i = 0; i < 4; i++) {
        btn->gradient_r[i] = gradient_r[i];
        btn->gradient_g[i] = gradient_g[i];
        btn->gradient_b[i] = gradient_b[i];
        btn->gradient_a[i] = gradient_a[i];
    }
    btn->gradient_radial = gradient_radial;
    btn->border_width = border_width;
    btn->border_r = border_r; btn->border_g = border_g; btn->border_b = border_b; btn->border_a = border_a;
    btn->border_style = border_style;
    btn->shadow_offset_x = shadow_offset_x;
    btn->shadow_offset_y = shadow_offset_y;
    btn->shadow_radius = shadow_radius;
    btn->shadow_r = shadow_r; btn->shadow_g = shadow_g; btn->shadow_b = shadow_b; btn->shadow_a = shadow_a;
    btn->text_decoration = text_decoration;
    btn->focus_width = focus_width;
    btn->focus_r = focus_r; btn->focus_g = focus_g; btn->focus_b = focus_b; btn->focus_a = focus_a;
    btn->hover_scale = hover_scale;
    btn->loading = loading;
    btn->cursor = cursor;
    btn->press_effect = press_effect;
    btn->press_offset = press_offset;
    btn->anchor = 0;
    btn->anchor_margin_x = 0;
    btn->anchor_margin_y = 0;
    btn->visible = 1;
    btn->enabled = 1;
    btn->hovered = 0;
    btn->pressed = 0;
    btn->focused = 0;
    btn->on_click = val_null();

    /* 加载字体 */
    LenoGUIPlatformFont* pf = leno_gui_platform_load_font(font_name, font_size);
    if (pf) {
        ObjGUIFont* font_obj = (ObjGUIFont*)gc_alloc(sizeof(ObjGUIFont), OBJ_GUI_FONT);
        if (font_obj) {
            font_obj->platform = pf;
            btn->font = font_obj;
        } else {
            leno_gui_platform_destroy_font(pf);
        }
    }

    /* 写屏障：保护 btn->window 引用 */
    gc_write_barrier_obj((Object*)btn, (Object*)win);

    /* 添加到窗口的按钮链表 */
    btn->next = (struct ObjGUIButton*)win->buttons;
    win->buttons = btn;
    win->button_count++;
    /* 有锚点时计算初始位置 */
    if (btn->anchor > 0 && win->platform) {
        int ww, wh;
        leno_gui_platform_get_window_size(win->platform, &ww, &wh);
        gui_button_update_anchors(win, ww, wh);
    }

    return val_obj((Object*)btn);
}

/* win.add_textbox(style) -> GTextBox - 创建文本框并添加到窗口 */
static Value win_add_textbox_func(int argc, Value* args) {
    (void)argc;
    ObjGUIWindow* win = as_window(args[0]);
    if (!win) return val_null();

    /* 默认值 */
    int x = 0, y = 0, width = 200, height = 36;
    int bg_r = 255, bg_g = 255, bg_b = 255, bg_a = 255;
    int border_r = 180, border_g = 180, border_b = 180, border_a = 255;
    int focus_r = 70, focus_g = 130, focus_b = 220, focus_a = 255;
    int text_r = 30, text_g = 30, text_b = 30, text_a = 255;
    int placeholder_r = 160, placeholder_g = 160, placeholder_b = 160, placeholder_a = 255;
    int cursor_r = 0, cursor_g = 0, cursor_b = 0, cursor_a = 255;
    int sel_r = 70, sel_g = 130, sel_b = 220, sel_a = 100;
    int border_width = 1;
    int radius = 4;
    char* font_name = strdup("Microsoft YaHei");  /* 默认支持中文 */
    int font_size = 16;
    int padding_x = 8, padding_y = 4;
    int max_length = 0;  /* 0=无限制 */
    int password = 0;
    int multiline = 0;

    /* 解析 style 字典 */
    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_DICT) {
        ObjDict* style = (ObjDict*)val_as_obj(args[1]);
        ObjString* key;
        Value v;

        key = intern_string("x", 1); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) x = val_as_int(v);
        key = intern_string("y", 1); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) y = val_as_int(v);
        key = intern_string("width", 5); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) width = val_as_int(v);
        key = intern_string("height", 6); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) height = val_as_int(v);

        key = intern_string("bg_color", 8); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_RGB) {
            ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
            bg_r = rgb->r; bg_g = rgb->g; bg_b = rgb->b; bg_a = rgb->a;
        }
        key = intern_string("border_color", 12); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_RGB) {
            ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
            border_r = rgb->r; border_g = rgb->g; border_b = rgb->b; border_a = rgb->a;
        }
        key = intern_string("focus_color", 11); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_RGB) {
            ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
            focus_r = rgb->r; focus_g = rgb->g; focus_b = rgb->b; focus_a = rgb->a;
        }
        key = intern_string("text_color", 10); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_RGB) {
            ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
            text_r = rgb->r; text_g = rgb->g; text_b = rgb->b; text_a = rgb->a;
        }

        key = intern_string("placeholder", 11); v = dict_get(style, val_obj((Object*)key));
        /* placeholder 在下面单独处理 */

        key = intern_string("font", 4); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
            free(font_name);
            font_name = strdup(((ObjString*)val_as_obj(v))->chars);
        }
        key = intern_string("font_size", 9); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) font_size = val_as_int(v);
        key = intern_string("padding_x", 9); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) padding_x = val_as_int(v);
        key = intern_string("padding_y", 9); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) padding_y = val_as_int(v);
        key = intern_string("border_width", 12); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) border_width = val_as_int(v);
        key = intern_string("radius", 6); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) radius = val_as_int(v);
        key = intern_string("max_length", 10); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) max_length = val_as_int(v);
        key = intern_string("password", 8); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) password = val_as_bool(v) ? 1 : 0;
        key = intern_string("multiline", 9); v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v)) multiline = val_as_bool(v) ? 1 : 0;
    }

    /* 创建文本框对象 */
    ObjGUITextBox* tb = (ObjGUITextBox*)gc_alloc(sizeof(ObjGUITextBox), OBJ_GUI_TEXTBOX);
    if (!tb) { free(font_name); return val_null(); }

    tb->window = win;
    tb->next = NULL;
    tb->x = x; tb->y = y; tb->width = width; tb->height = height;
    tb->text = (char*)malloc(TEXTBOX_DEFAULT_CAP);
    tb->text[0] = '\0';
    tb->text_len = 0;
    tb->text_cap = TEXTBOX_DEFAULT_CAP;
    tb->placeholder = NULL;
    /* 解析 placeholder */
    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_DICT) {
        ObjDict* style = (ObjDict*)val_as_obj(args[1]);
        ObjString* key = intern_string("placeholder", 11);
        Value v = dict_get(style, val_obj((Object*)key));
        if (!val_is_null(v) && val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
            tb->placeholder = strdup(((ObjString*)val_as_obj(v))->chars);
        }
    }
    tb->cursor_pos = 0;
    tb->sel_start = -1;
    tb->sel_len = 0;
    tb->scroll_x = 0;
    tb->scroll_y = 0;
    tb->dragging = 0;
    tb->drag_start_cp = 0;
    tb->multiline = multiline;

    tb->bg_r = bg_r; tb->bg_g = bg_g; tb->bg_b = bg_b; tb->bg_a = bg_a;
    tb->border_r = border_r; tb->border_g = border_g; tb->border_b = border_b; tb->border_a = border_a;
    tb->focus_r = focus_r; tb->focus_g = focus_g; tb->focus_b = focus_b; tb->focus_a = focus_a;
    tb->text_r = text_r; tb->text_g = text_g; tb->text_b = text_b; tb->text_a = text_a;
    tb->placeholder_r = placeholder_r; tb->placeholder_g = placeholder_g; tb->placeholder_b = placeholder_b; tb->placeholder_a = placeholder_a;
    tb->cursor_r = cursor_r; tb->cursor_g = cursor_g; tb->cursor_b = cursor_b; tb->cursor_a = cursor_a;
    tb->sel_r = sel_r; tb->sel_g = sel_g; tb->sel_b = sel_b; tb->sel_a = sel_a;
    tb->border_width = border_width;
    tb->radius = radius;
    tb->font_name = font_name;
    tb->font_size = font_size;
    tb->font = NULL;
    tb->padding_x = padding_x;
    tb->padding_y = padding_y;
    tb->visible = 1;
    tb->enabled = 1;
    tb->focused = 0;
    tb->hovered = 0;
    tb->password = password;
    tb->max_length = max_length;
    tb->blink_visible = 0;
    tb->last_blink = 0;
    tb->anchor = 0;
    tb->anchor_margin_x = 0;
    tb->anchor_margin_y = 0;
    tb->on_change = val_null();
    tb->on_submit = val_null();

    /* 加载字体 */
    LenoGUIPlatformFont* pf = leno_gui_platform_load_font(font_name, font_size);
    if (pf) {
        ObjGUIFont* font_obj = (ObjGUIFont*)gc_alloc(sizeof(ObjGUIFont), OBJ_GUI_FONT);
        if (font_obj) { font_obj->platform = pf; tb->font = font_obj; }
        else { leno_gui_platform_destroy_font(pf); }
    } else {
        fprintf(stderr, "[TEXTBOX] font load failed: '%s' %d\n", font_name, font_size);
    }

    gc_write_barrier_obj((Object*)tb, (Object*)win);

    /* 添加到窗口的文本框链表 */
    tb->next = win->textboxes;
    win->textboxes = tb;
    win->textbox_count++;

    if (tb->anchor > 0 && win->platform) {
        int ww, wh;
        leno_gui_platform_get_window_size(win->platform, &ww, &wh);
        gui_textbox_update_anchors(win, ww, wh);
    }

    return val_obj((Object*)tb);
}

/* ============================================================================
 * 注册 Win 实例方法
 * ============================================================================ */

/* 前向声明 */
extern void window_register_method_with_params(const char* name, ObjNative* method, int arity,
                                                int min_arity, int max_arity,
                                                TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
extern ObjNative* make_native(NativeFn fn, int arity, const char* name);
extern void window_init_methods(void);

/* win.run(on_draw, on_event) - 定义在 guis.c 中 */
extern Value win_run_func(int argc, Value* args);

void guis_init_window_instance_methods(void) {
    window_init_methods();

    TypeKind no_params[] = {};
    TypeKind int_2[] = {TYPE_INT, TYPE_INT};
    TypeKind int_4[] = {TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT};
    TypeKind str_1[] = {TYPE_STRING};
    TypeKind bool_1[] = {TYPE_BOOL};
    TypeKind float_1[] = {TYPE_FLOAT};
    TypeKind func_2[] = {TYPE_ANY, TYPE_ANY};
    TypeKind any_1[] = {TYPE_ANY};

    window_register_method_with_params("show", make_native(win_show_func, 1, "show"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
    window_register_method_with_params("hide", make_native(win_hide_func, 1, "hide"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
    window_register_method_with_params("close", make_native(win_close_func, 1, "close"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
    window_register_method_with_params("set_title", make_native(win_set_title_func, 2, "set_title"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_1);
    window_register_method_with_params("set_size", make_native(win_set_size_func, 3, "set_size"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2);
    window_register_method_with_params("get_size", make_native(win_get_size_func, 1, "get_size"), 0, -1, -1, TYPE_ARRAY, TYPE_INT, no_params);
    window_register_method_with_params("set_pos", make_native(win_set_pos_func, 3, "set_pos"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2);
    window_register_method_with_params("get_pos", make_native(win_get_pos_func, 1, "get_pos"), 0, -1, -1, TYPE_ARRAY, TYPE_INT, no_params);
    window_register_method_with_params("center", make_native(win_center_func, 1, "center"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
    window_register_method_with_params("set_min_size", make_native(win_set_min_size_func, 3, "set_min_size"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2);
    window_register_method_with_params("set_max_size", make_native(win_set_max_size_func, 3, "set_max_size"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_2);
    window_register_method_with_params("set_maximized", make_native(win_set_maximized_func, 2, "set_maximized"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    window_register_method_with_params("set_icon", make_native(win_set_icon_func, 2, "set_icon"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, str_1);
    window_register_method_with_params("set_bg_color", make_native(win_set_bg_color_func, 2, "set_bg_color"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, any_1);
    window_register_method_with_params("clear_bg_color", make_native(win_clear_bg_color_func, 1, "clear_bg_color"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);
    window_register_method_with_params("set_vsync", make_native(win_set_vsync_func, 2, "set_vsync"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    window_register_method_with_params("set_maximizable", make_native(win_set_maximizable_func, 2, "set_maximizable"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    window_register_method_with_params("set_fullscreen", make_native(win_set_fullscreen_func, 2, "set_fullscreen"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    window_register_method_with_params("should_close", make_native(win_should_close_func, 1, "should_close"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    window_register_method_with_params("set_should_close", make_native(win_set_should_close_func, 2, "set_should_close"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, bool_1);
    window_register_method_with_params("set_opacity", make_native(win_set_opacity_func, 2, "set_opacity"), 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, float_1);
    window_register_method_with_params("set_drag_area", make_native(win_set_drag_area_func, 5, "set_drag_area"), 4, -1, -1, TYPE_NULL, TYPE_UNKNOWN, int_4);
    window_register_method_with_params("clear_drag_area", make_native(win_clear_drag_area_func, 1, "clear_drag_area"), 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, no_params);

    /* win.run(on_draw, on_event) - 事件循环 */
    window_register_method_with_params("run", make_native(win_run_func, 3, "run"), 2, -1, -1, TYPE_NULL, TYPE_UNKNOWN, func_2);

    /* win.add_button(style) -> GButton */
    TypeKind dict_1[] = {TYPE_DICT};
    window_register_method_with_params("add_button", make_native(win_add_button_func, 2, "add_button"), 1, -1, -1, TYPE_BUTTON, TYPE_UNKNOWN, dict_1);

    /* win.add_textbox(style) -> GTextBox */
    window_register_method_with_params("add_textbox", make_native(win_add_textbox_func, 2, "add_textbox"), 1, -1, -1, TYPE_TEXTBOX, TYPE_UNKNOWN, dict_1);
}
