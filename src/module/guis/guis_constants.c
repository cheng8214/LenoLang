/* Leno GUI - 模块常量注册实现
 * 集中管理所有 guis 模块常量定义
 */

#include "include/native.h"
#include "leno_guis.h"
#include "leno_guis_log.h"

void guis_register_constants(void) {
    /* ===== 模块常量：按键码 ===== */
    native_register_module_const("guis", "KEY_UNKNOWN",    LENO_GUI_KEY_UNKNOWN);
    native_register_module_const("guis", "KEY_RETURN",     LENO_GUI_KEY_RETURN);
    native_register_module_const("guis", "KEY_ESCAPE",     LENO_GUI_KEY_ESCAPE);
    native_register_module_const("guis", "KEY_BACKSPACE",  LENO_GUI_KEY_BACKSPACE);
    native_register_module_const("guis", "KEY_TAB",        LENO_GUI_KEY_TAB);
    native_register_module_const("guis", "KEY_SPACE",      LENO_GUI_KEY_SPACE);
    native_register_module_const("guis", "KEY_DELETE",     LENO_GUI_KEY_DELETE);
    native_register_module_const("guis", "KEY_LEFT",       LENO_GUI_KEY_LEFT);
    native_register_module_const("guis", "KEY_RIGHT",      LENO_GUI_KEY_RIGHT);
    native_register_module_const("guis", "KEY_UP",         LENO_GUI_KEY_UP);
    native_register_module_const("guis", "KEY_DOWN",       LENO_GUI_KEY_DOWN);
    native_register_module_const("guis", "KEY_INSERT",     LENO_GUI_KEY_INSERT);
    native_register_module_const("guis", "KEY_HOME",       LENO_GUI_KEY_HOME);
    native_register_module_const("guis", "KEY_END",        LENO_GUI_KEY_END);
    native_register_module_const("guis", "KEY_PAGEUP",     LENO_GUI_KEY_PAGEUP);
    native_register_module_const("guis", "KEY_PAGEDOWN",   LENO_GUI_KEY_PAGEDOWN);
    native_register_module_const("guis", "KEY_F1",         LENO_GUI_KEY_F1);
    native_register_module_const("guis", "KEY_F2",         LENO_GUI_KEY_F2);
    native_register_module_const("guis", "KEY_F3",         LENO_GUI_KEY_F3);
    native_register_module_const("guis", "KEY_F4",         LENO_GUI_KEY_F4);
    native_register_module_const("guis", "KEY_F5",         LENO_GUI_KEY_F5);
    native_register_module_const("guis", "KEY_F6",         LENO_GUI_KEY_F6);
    native_register_module_const("guis", "KEY_F7",         LENO_GUI_KEY_F7);
    native_register_module_const("guis", "KEY_F8",         LENO_GUI_KEY_F8);
    native_register_module_const("guis", "KEY_F9",         LENO_GUI_KEY_F9);
    native_register_module_const("guis", "KEY_F10",        LENO_GUI_KEY_F10);
    native_register_module_const("guis", "KEY_F11",        LENO_GUI_KEY_F11);
    native_register_module_const("guis", "KEY_F12",        LENO_GUI_KEY_F12);
    native_register_module_const("guis", "KEY_LSHIFT",     LENO_GUI_KEY_LSHIFT);
    native_register_module_const("guis", "KEY_RSHIFT",     LENO_GUI_KEY_RSHIFT);
    native_register_module_const("guis", "KEY_LCTRL",      LENO_GUI_KEY_LCTRL);
    native_register_module_const("guis", "KEY_RCTRL",      LENO_GUI_KEY_RCTRL);
    native_register_module_const("guis", "KEY_LALT",       LENO_GUI_KEY_LALT);
    native_register_module_const("guis", "KEY_RALT",       LENO_GUI_KEY_RALT);
    native_register_module_const("guis", "KEY_CAPSLOCK",   LENO_GUI_KEY_CAPSLOCK);
    native_register_module_const("guis", "KEY_NUMLOCK",    LENO_GUI_KEY_NUMLOCK);

    /* ===== 模块常量：修饰键标志 ===== */
    native_register_module_const("guis", "MOD_SHIFT",      LENO_GUI_MOD_SHIFT);
    native_register_module_const("guis", "MOD_CTRL",       LENO_GUI_MOD_CTRL);
    native_register_module_const("guis", "MOD_ALT",        LENO_GUI_MOD_ALT);
    native_register_module_const("guis", "MOD_SUPER",      LENO_GUI_MOD_SUPER);

    /* ===== 模块常量：鼠标按钮 ===== */
    native_register_module_const("guis", "MOUSE_LEFT",     LENO_GUI_MOUSE_LEFT);
    native_register_module_const("guis", "MOUSE_MIDDLE",   LENO_GUI_MOUSE_MIDDLE);
    native_register_module_const("guis", "MOUSE_RIGHT",    LENO_GUI_MOUSE_RIGHT);

    /* ===== 模块常量：事件类型 ===== */
    native_register_module_const("guis", "EVT_QUIT",              LENO_GUI_EVT_QUIT);
    native_register_module_const("guis", "EVT_WINDOW_CLOSE",      LENO_GUI_EVT_WINDOW_CLOSE);
    native_register_module_const("guis", "EVT_WINDOW_RESIZE",     LENO_GUI_EVT_WINDOW_RESIZE);
    native_register_module_const("guis", "EVT_WINDOW_MOVE",       LENO_GUI_EVT_WINDOW_MOVE);
    native_register_module_const("guis", "EVT_WINDOW_FOCUS",      LENO_GUI_EVT_WINDOW_FOCUS);
    native_register_module_const("guis", "EVT_WINDOW_UNFOCUS",    LENO_GUI_EVT_WINDOW_UNFOCUS);
    native_register_module_const("guis", "EVT_WINDOW_SHOW",       LENO_GUI_EVT_WINDOW_SHOW);
    native_register_module_const("guis", "EVT_WINDOW_HIDE",       LENO_GUI_EVT_WINDOW_HIDE);
    native_register_module_const("guis", "EVT_WINDOW_EXPOSED",    LENO_GUI_EVT_WINDOW_EXPOSED);
    native_register_module_const("guis", "EVT_WINDOW_MINIMIZED",  LENO_GUI_EVT_WINDOW_MINIMIZED);
    native_register_module_const("guis", "EVT_WINDOW_MAXIMIZED",  LENO_GUI_EVT_WINDOW_MAXIMIZED);
    native_register_module_const("guis", "EVT_WINDOW_RESTORED",   LENO_GUI_EVT_WINDOW_RESTORED);
    native_register_module_const("guis", "EVT_KEY_DOWN",          LENO_GUI_EVT_KEY_DOWN);
    native_register_module_const("guis", "EVT_KEY_UP",            LENO_GUI_EVT_KEY_UP);
    native_register_module_const("guis", "EVT_TEXT_INPUT",        LENO_GUI_EVT_TEXT_INPUT);
    native_register_module_const("guis", "EVT_MOUSE_MOVE",        LENO_GUI_EVT_MOUSE_MOVE);
    native_register_module_const("guis", "EVT_MOUSE_DOWN",        LENO_GUI_EVT_MOUSE_DOWN);
    native_register_module_const("guis", "EVT_MOUSE_UP",          LENO_GUI_EVT_MOUSE_UP);
    native_register_module_const("guis", "EVT_MOUSE_WHEEL",       LENO_GUI_EVT_MOUSE_WHEEL);
    native_register_module_const("guis", "EVT_DROP_FILE",         LENO_GUI_EVT_DROP_FILE);
    native_register_module_const("guis", "EVT_DROP_TEXT",         LENO_GUI_EVT_DROP_TEXT);
    native_register_module_const("guis", "EVT_DROP_BEGIN",        LENO_GUI_EVT_DROP_BEGIN);
    native_register_module_const("guis", "EVT_DROP_COMPLETE",     LENO_GUI_EVT_DROP_COMPLETE);
    native_register_module_const("guis", "EVT_FILEDIALOG_RESULT", LENO_GUI_EVT_FILEDIALOG_RESULT);

    /* ===== 模块常量：逻辑呈现模式 ===== */
    native_register_module_const("guis", "LOGICAL_PRESENTATION_DISABLED",      LENO_GUI_LOGICAL_PRESENTATION_DISABLED);
    native_register_module_const("guis", "LOGICAL_PRESENTATION_STRETCH",       LENO_GUI_LOGICAL_PRESENTATION_STRETCH);
    native_register_module_const("guis", "LOGICAL_PRESENTATION_LETTERBOX",     LENO_GUI_LOGICAL_PRESENTATION_LETTERBOX);
    native_register_module_const("guis", "LOGICAL_PRESENTATION_OVERSCAN",      LENO_GUI_LOGICAL_PRESENTATION_OVERSCAN);
    native_register_module_const("guis", "LOGICAL_PRESENTATION_INTEGER_SCALE", LENO_GUI_LOGICAL_PRESENTATION_INTEGER_SCALE);

    /* ===== 模块常量：窗口标志 ===== */
    native_register_module_const("guis", "WIN_RESIZABLE",     LENO_GUI_WIN_RESIZABLE);
    native_register_module_const("guis", "WIN_FULLSCREEN",    LENO_GUI_WIN_FULLSCREEN);
    native_register_module_const("guis", "WIN_BORDERLESS",    LENO_GUI_WIN_BORDERLESS);
    native_register_module_const("guis", "WIN_HIDDEN",        LENO_GUI_WIN_HIDDEN);
    native_register_module_const("guis", "WIN_ALWAYS_ON_TOP", LENO_GUI_WIN_ALWAYS_ON_TOP);

    /* ===== 模块常量：光标类型 ===== */
    native_register_module_const("guis", "CURSOR_DEFAULT",      LENO_GUI_CURSOR_DEFAULT);
    native_register_module_const("guis", "CURSOR_TEXT",         LENO_GUI_CURSOR_TEXT);
    native_register_module_const("guis", "CURSOR_WAIT",         LENO_GUI_CURSOR_WAIT);
    native_register_module_const("guis", "CURSOR_CROSSHAIR",    LENO_GUI_CURSOR_CROSSHAIR);
    native_register_module_const("guis", "CURSOR_PROGRESS",     LENO_GUI_CURSOR_PROGRESS);
    native_register_module_const("guis", "CURSOR_RESIZE_NWSE",  LENO_GUI_CURSOR_RESIZE_NWSE);
    native_register_module_const("guis", "CURSOR_RESIZE_NESW",  LENO_GUI_CURSOR_RESIZE_NESW);
    native_register_module_const("guis", "CURSOR_RESIZE_EW",    LENO_GUI_CURSOR_RESIZE_EW);
    native_register_module_const("guis", "CURSOR_RESIZE_NS",    LENO_GUI_CURSOR_RESIZE_NS);
    native_register_module_const("guis", "CURSOR_MOVE",         LENO_GUI_CURSOR_MOVE);
    native_register_module_const("guis", "CURSOR_NOT_ALLOWED",  LENO_GUI_CURSOR_NOT_ALLOWED);
    native_register_module_const("guis", "CURSOR_POINTER",      LENO_GUI_CURSOR_POINTER);

    /* ===== 模块常量：文件对话框类型 ===== */
    native_register_module_const("guis", "FILEDIALOG_OPENFILE",   LENO_GUI_FILEDIALOG_OPENFILE);
    native_register_module_const("guis", "FILEDIALOG_SAVEFILE",   LENO_GUI_FILEDIALOG_SAVEFILE);
    native_register_module_const("guis", "FILEDIALOG_OPENFOLDER", LENO_GUI_FILEDIALOG_OPENFOLDER);

    /* ===== 模块常量：翻转标志 ===== */
    native_register_module_const("guis", "FLIP_NONE",       LENO_GUI_FLIP_NONE);
    native_register_module_const("guis", "FLIP_HORIZONTAL", LENO_GUI_FLIP_HORIZONTAL);
    native_register_module_const("guis", "FLIP_VERTICAL",   LENO_GUI_FLIP_VERTICAL);

    /* ===== 模块常量：纹理访问模式 ===== */
    native_register_module_const("guis", "IMAGEACCESS_STATIC",    LENO_GUI_IMAGEACCESS_STATIC);
    native_register_module_const("guis", "IMAGEACCESS_STREAMING", LENO_GUI_IMAGEACCESS_STREAMING);
    native_register_module_const("guis", "IMAGEACCESS_TARGET",    LENO_GUI_IMAGEACCESS_TARGET);

    /* ===== 模块常量：日志类别 ===== */
    native_register_module_const("guis", "LOG_CATEGORY_APPLICATION", LENO_GUI_LOG_CATEGORY_APPLICATION);
    native_register_module_const("guis", "LOG_CATEGORY_ERROR",       LENO_GUI_LOG_CATEGORY_ERROR);
    native_register_module_const("guis", "LOG_CATEGORY_ASSERT",      LENO_GUI_LOG_CATEGORY_ASSERT);
    native_register_module_const("guis", "LOG_CATEGORY_SYSTEM",      LENO_GUI_LOG_CATEGORY_SYSTEM);
    native_register_module_const("guis", "LOG_CATEGORY_VIDEO",       LENO_GUI_LOG_CATEGORY_VIDEO);
    native_register_module_const("guis", "LOG_CATEGORY_RENDER",      LENO_GUI_LOG_CATEGORY_RENDER);
    native_register_module_const("guis", "LOG_CATEGORY_INPUT",       LENO_GUI_LOG_CATEGORY_INPUT);
    native_register_module_const("guis", "LOG_CATEGORY_EVENT",       LENO_GUI_LOG_CATEGORY_EVENT);
    native_register_module_const("guis", "LOG_CATEGORY_WINDOW",      LENO_GUI_LOG_CATEGORY_WINDOW);

    /* ===== 模块常量：日志优先级 ===== */
    native_register_module_const("guis", "LOG_PRIORITY_TRACE",    LENO_GUI_LOG_PRIORITY_TRACE);
    native_register_module_const("guis", "LOG_PRIORITY_VERBOSE",  LENO_GUI_LOG_PRIORITY_VERBOSE);
    native_register_module_const("guis", "LOG_PRIORITY_DEBUG",    LENO_GUI_LOG_PRIORITY_DEBUG);
    native_register_module_const("guis", "LOG_PRIORITY_INFO",     LENO_GUI_LOG_PRIORITY_INFO);
    native_register_module_const("guis", "LOG_PRIORITY_WARN",     LENO_GUI_LOG_PRIORITY_WARN);
    native_register_module_const("guis", "LOG_PRIORITY_ERROR",    LENO_GUI_LOG_PRIORITY_ERROR);
    native_register_module_const("guis", "LOG_PRIORITY_CRITICAL", LENO_GUI_LOG_PRIORITY_CRITICAL);
}
