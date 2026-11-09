/**
 * LSP Code Action —— 「补 default 分支」（2026-11-10，P0 编辑器侧）
 *
 * 为什么先做这一个动作：
 *   编译器 2026-11-10 新增了 `WARN_SWITCH_NOT_EXHAUSTIVE`（enum 主题漏 case 且无 default）。
 *   那条警告只说"漏了谁"，用户还得自己把 default 敲出来 —— 这里把它变成一次点击。
 *
 * 判据（**纯文本、刻意保守**）：光标落在某个 `switch … {` 的块内、块内没有 `default` ⇒
 *   给一个 quickfix，在块的收尾 `}` 之前按同样的缩进插入 `default { … }`。
 *   任何一步算不准（找不到 switch / 大括号不配对）就**不提供**动作 —— 宁可没有，
 *   也不要给出一个改坏代码的修复。
 *
 * ⚠ 为什么不在这里做"索引循环 ⇒ map/filter"那种重写：那是**语义级**改写（要知道循环变量
 *   有没有被别处引用、有没有 break、add 的接收者是谁）。文本级猜错就会静默改坏逻辑，
 *   而这份文件能拿到的只有字符串。等编译器侧先出对应的 `WARN_*`（带准确的区间与符号信息）
 *   再接，才是安全的做法。
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 纯逻辑（ca_find_default_fix）不依赖协议层 ⇒ 用 -DCA_PROBE 单独编译时可以不包含它，
// 这样探针只需要 lsp_code_action.c 一个源文件就能跑（见 _probe_ca.c）。
#ifndef CA_PROBE
#include "leno_lsp.h"
#endif

#define CA_MAX_LINES 20000

// ------------------------------------------------ 小工具

static int ca_line_len(const char* start) {
    const char* p = start;
    while (*p && *p != '\n') p++;
    return (int)(p - start);
}

// 行首空白长度（缩进）
static int ca_indent_len(const char* line, int len) {
    int i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    return i;
}

// 去掉行首空白后的指针
static const char* ca_skip_ws(const char* line, int len, int* ws_len) {
    int i = ca_indent_len(line, len);
    if (ws_len) *ws_len = i;
    return line + i;
}

// 该行去掉空白后是否以给定关键字开头（按"词"匹配，避免 switchx 之类误判）
static int ca_starts_with_word(const char* line, int len, const char* word) {
    int ws = 0;
    const char* p = ca_skip_ws(line, len, &ws);
    size_t wl = strlen(word);
    if (len - ws < (int)wl) return 0;
    if (strncmp(p, word, wl) != 0) return 0;
    if (len - ws == (int)wl) return 1;
    char nxt = p[wl];
    return !(isalnum((unsigned char)nxt) || nxt == '_');
}

// 该行（去注释后）是否含关键字（用于 switch 判定）
static int ca_contains_word(const char* line, int len, const char* word) {
    size_t wl = strlen(word);
    for (int i = 0; i + (int)wl <= len; i++) {
        if (strncmp(line + i, word, wl) != 0) continue;
        int before_ok = (i == 0) || !(isalnum((unsigned char)line[i - 1]) || line[i - 1] == '_');
        int after_i = i + (int)wl;
        int after_ok = (after_i >= len) ||
                       !(isalnum((unsigned char)line[after_i]) || line[after_i] == '_');
        if (before_ok && after_ok) return 1;
    }
    return 0;
}

// 该行是否整行注释（`//` 开头）
static int ca_is_comment_line(const char* line, int len) {
    int ws = 0;
    const char* p = ca_skip_ws(line, len, &ws);
    return (len - ws >= 2 && p[0] == '/' && p[1] == '/');
}

// 统计一行里的 { 与 }（跳过字符串与行注释，够用的近似）
static void ca_count_braces(const char* line, int len, int* open, int* close) {
    int o = 0, c = 0;
    for (int i = 0; i < len; i++) {
        char ch = line[i];
        if (ch == '"' || ch == '\'') {
            char q = ch;
            i++;
            while (i < len && line[i] != q) {
                if (line[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            continue;
        }
        if (ch == '/' && i + 1 < len && line[i + 1] == '/') break;
        if (ch == '{') o++;
        else if (ch == '}') c++;
    }
    *open = o;
    *close = c;
}

// ------------------------------------------------ 纯逻辑（可单独测，不碰 JSON）

// 一次修复：在第 line 行、第 col 列（0-based，插在该位置之前）插入 text
typedef struct {
    int ok;
    int line;
    int col;
    char text[256];
} CaFix;

// 扫描 content，判断光标所在行能否补 default。
//   返回 ok=1 时 line/col/text 就是 WorkspaceEdit 需要的插入点与文本。
//   ⚠ 纯文本、保守判据：任何一步算不准就返回 ok=0（宁可不给动作，也不给错的动作）。
CaFix ca_find_default_fix(const char* content, int cur_line) {
    CaFix fix;
    memset(&fix, 0, sizeof(fix));
    if (!content || cur_line < 0) return fix;

    static const char* line_start[CA_MAX_LINES];
    static int line_len[CA_MAX_LINES];
    int line_count = 0;
    {
        const char* p = content;
        while (*p && line_count < CA_MAX_LINES) {
            line_start[line_count] = p;
            int len = ca_line_len(p);
            line_len[line_count] = len;
            line_count++;
            p += len;
            if (*p == '\n') p++;
        }
    }
    if (line_count == 0 || cur_line >= line_count) return fix;

    // 整份文档的大括号必须配平：不配平说明这段代码本身还没写完/写坏了，
    // 文本级扫描会把别人的 `}` 当成 switch 的收尾 ⇒ 直接不给动作。
    //   （实测：`main() { switch x { case 1 {} }` 少一个 `}` 时，最外层 `}` 会被误认。）
    {
        int total = 0;
        for (int l = 0; l < line_count; l++) {
            int o = 0, c = 0;
            ca_count_braces(line_start[l], line_len[l], &o, &c);
            total += o - c;
        }
        if (total != 0) return fix;
    }

    // 向上找最近的 switch 行（整行注释里的 "switch" 不算）
    int sw_line = -1;
    for (int l = cur_line; l >= 0; l--) {
        if (ca_is_comment_line(line_start[l], line_len[l])) continue;
        if (ca_contains_word(line_start[l], line_len[l], "switch")) { sw_line = l; break; }
    }
    if (sw_line < 0) return fix;

    int depth = 0;
    int close_line = -1;
    int has_default = 0;
    int saw_open = 0;
    for (int l = sw_line; l < line_count; l++) {
        int o = 0, c = 0;
        ca_count_braces(line_start[l], line_len[l], &o, &c);
        if (l > sw_line && ca_starts_with_word(line_start[l], line_len[l], "default")) {
            has_default = 1;
        }
        depth += o - c;
        if (!saw_open && o > 0) saw_open = 1;
        if (saw_open && depth <= 0) { close_line = l; break; }
    }
    if (close_line < 0 || has_default) return fix;

    int close_ws = ca_indent_len(line_start[close_line], line_len[close_line]);
    int n1 = close_ws + 4;
    if (n1 > 60) n1 = 60;
    int n2 = n1 + 4;
    if (n2 > 62) n2 = 62;

    fix.ok = 1;
    fix.line = close_line;
    fix.col = close_ws;
    // 手工拼（不用 snprintf 的 %*s：宽度是运行期的，拼出来更直观）
    int w = 0;
    w += snprintf(fix.text + w, sizeof(fix.text) - (size_t)w, "default {\n");
    for (int i = 0; i < n2 && w < (int)sizeof(fix.text) - 2; i++) fix.text[w++] = ' ';
    w += snprintf(fix.text + w, sizeof(fix.text) - (size_t)w,
                  "// 兜底：主题是 enum 时建议把 case 写全，default 只做防御\n");
    for (int i = 0; i < n1 && w < (int)sizeof(fix.text) - 2; i++) fix.text[w++] = ' ';
    w += snprintf(fix.text + w, sizeof(fix.text) - (size_t)w, "}\n");
    fix.text[w] = '\0';
    return fix;
}

// ------------------------------------------------ 主处理

#ifndef CA_PROBE
char* lsp_handle_code_action(LspServer* server, int id, JsonValue* params) {
    if (!server || server->state != LSP_STATE_INITIALIZED) {
        return lsp_create_error(id, LSP_ERROR_SERVER_NOT_INITIALIZED, "Server not initialized");
    }

    JsonValue* text_doc = json_object_get(params, "textDocument");
    if (!text_doc) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing textDocument");
    }
    JsonValue* uri_val = json_object_get(text_doc, "uri");
    if (!uri_val) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing URI");
    }
    const char* uri = json_string_value(uri_val);

    LspTextDocument* doc = lsp_document_get(server, uri);
    if (!doc || !doc->content) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Document not found");
    }
    const char* content = doc->content;

    // 光标行（range 没给就取 position）
    int cur_line = 0;
    JsonValue* pos = json_object_get(params, "position");
    if (!pos) {
        JsonValue* range = json_object_get(params, "range");
        if (range) pos = json_object_get(range, "start");
    }
    if (pos) {
        JsonValue* ln = json_object_get(pos, "line");
        if (ln) cur_line = json_int_value(ln);
    }
    if (cur_line < 0) cur_line = 0;

    // ---- 判定（纯逻辑，见 ca_find_default_fix）----
    CaFix fix = ca_find_default_fix(content, cur_line);
    JsonValue* result = json_array_new();
    if (!fix.ok) {
        char* resp = lsp_create_response(id, result);
        json_free(result);
        return resp;
    }

    JsonValue* action = json_object_new();
    json_object_set(action, "title",
                    json_string_new("补 default 分支（这个 switch 没有兜底）"));
    json_object_set(action, "kind", json_string_new("quickfix"));

    JsonValue* range = json_object_new();
    JsonValue* start_pos = json_object_new();
    JsonValue* end_pos = json_object_new();
    json_object_set(start_pos, "line", json_int_new(fix.line));
    json_object_set(start_pos, "character", json_int_new(fix.col));
    json_object_set(end_pos, "line", json_int_new(fix.line));
    json_object_set(end_pos, "character", json_int_new(fix.col));
    json_object_set(range, "start", start_pos);
    json_object_set(range, "end", end_pos);

    JsonValue* text_edit = json_object_new();
    json_object_set(text_edit, "range", range);
    json_object_set(text_edit, "newText", json_string_new(fix.text));

    JsonValue* edits = json_array_new();
    json_array_add(edits, text_edit);

    JsonValue* changes = json_object_new();
    json_object_set(changes, uri, edits);

    JsonValue* edit = json_object_new();
    json_object_set(edit, "changes", changes);
    json_object_set(action, "edit", edit);

    json_array_add(result, action);

    char* resp = lsp_create_response(id, result);
    json_free(result);
    return resp;
}
#endif  // CA_PROBE
