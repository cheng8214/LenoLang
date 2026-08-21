/**
 * 文档符号服务 (DocumentSymbol)
 *
 * 扫描源代码中的顶层声明，生成 LSP DocumentSymbol 树：
 * - func / export func        -> Function
 * - struct / export struct     -> Struct (子符号: 字段、方法)
 * - cstruct / export cstruct  -> Struct (子符号: 字段)
 * - enum / export enum        -> Enum (子符号: 枚举成员)
 * - face / export face        -> Interface (子符号: 方法签名)
 * - clib / export clib        -> Module (子符号: C 函数)
 * - var / const / export var  -> Variable / Constant
 * - import                    -> Module
 *
 * VS Code 使用此信息显示大纲面板、面包屑导航和 Ctrl+Shift+O 符号跳转。
 */

#include "leno_lsp.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========== 工具函数 ========== */


// 检查 p 是否在行首边界
static bool at_line_boundary(const char* content, const char* p) {
    if (p == content) return true;
    const char* q = p - 1;
    while (q >= content && isspace((unsigned char)*q)) {
        if (*q == '\n') return true;
        q--;
    }
    return (q < content);
}

// 提取标识符 [a-zA-Z_][a-zA-Z0-9_]*
static const char* extract_identifier(const char* p, char** out_name) {
    *out_name = NULL;
    if (!p) return p;
    if (!(isalpha((unsigned char)*p) || *p == '_')) return p;

    const char* start = p;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
    int len = (int)(p - start);

    *out_name = (char*)malloc(len + 1);
    if (*out_name) {
        memcpy(*out_name, start, len);
        (*out_name)[len] = '\0';
    }
    return p;
}

// offset -> LspPosition (0-based)
static LspPosition offset_to_pos(const char* content, int offset) {
    LspPosition pos = {0, 0};
    for (int i = 0; i < offset && content[i]; i++) {
        if (content[i] == '\n') {
            pos.line++;
            pos.character = 0;
        } else {
            pos.character++;
        }
    }
    return pos;
}

// 查找匹配的右花括号 } (跳过字符串、注释、嵌套花括号)
static int find_matching_brace(const char* content, int start) {
    if (!content || content[start] != '{') return -1;

    int depth = 1;
    int i = start + 1;
    while (content[i] && depth > 0) {
        if (content[i] == '"' || content[i] == '\'') {
            char quote = content[i];
            i++;
            while (content[i] && content[i] != quote) {
                if (content[i] == '\\' && content[i + 1]) i++;
                i++;
            }
            if (content[i]) i++;
            continue;
        }
        if (content[i] == '/' && content[i + 1] == '/') {
            while (content[i] && content[i] != '\n') i++;
            continue;
        }
        if (content[i] == '/' && content[i + 1] == '*') {
            i += 2;
            while (content[i] && !(content[i] == '*' && content[i + 1] == '/')) i++;
            if (content[i]) i += 2;
            continue;
        }
        if (content[i] == '{') depth++;
        else if (content[i] == '}') depth--;
        if (depth == 0) return i;
        i++;
    }
    return -1;
}

// 跳过 packed / align(N) 属性前缀
static const char* skip_attributes(const char* p) {
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncmp(p, "packed", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
            p += 6;
            continue;
        }
        if (strncmp(p, "align(", 6) == 0) {
            p += 6;
            while (*p && *p != ')') p++;
            if (*p) p++;
            continue;
        }
        break;
    }
    return p;
}

// 在 content 中从 from 行的行首跳过空白注释，定位到下一个非空白字符
// 返回该字符偏移，同时更新 *p_line 到该字符所在行号
static int skip_ws_comments_inline(const char* content, int from, int* p_line) {
    int i = from;
    while (content[i]) {
        if (content[i] == '/' && content[i + 1] == '/') {
            while (content[i] && content[i] != '\n') i++;
            continue;
        }
        if (content[i] == '/' && content[i + 1] == '*') {
            i += 2;
            while (content[i] && !(content[i] == '*' && content[i + 1] == '/')) {
                if (content[i] == '\n') (*p_line)++;
                i++;
            }
            if (content[i]) i += 2;
            continue;
        }
        if (isspace((unsigned char)content[i])) {
            if (content[i] == '\n') (*p_line)++;
            i++;
            continue;
        }
        break;
    }
    return i;
}

/* ========== 子符号解析 ========== */

// 在 struct/cstruct 块内解析字段和方法
static void parse_struct_children(const char* content, int block_start,
                                  int block_end, LspDocumentSymbol* parent) {
    int cap = 8;
    LspDocumentSymbol* children = (LspDocumentSymbol*)malloc(sizeof(LspDocumentSymbol) * cap);
    int count = 0;

    int i = block_start;
    while (i < block_end) {
        i = skip_ws_comments_inline(content, i, &(int){0});
        if (i >= block_end) break;

        // 检查是否是 func (struct 方法)
        if (strncmp(content + i, "func", 4) == 0 &&
            !isalnum((unsigned char)content[i + 4]) && content[i + 4] != '_') {
            int func_start = i;
            int j = i + 4;
            while (j < block_end && isspace((unsigned char)content[j])) j++;

            char* method_name = NULL;
            int name_start = j;
            j = (int)(extract_identifier(content + j, &method_name) - content);
            int name_len = j - name_start;

            if (method_name && name_len > 0) {
                // 找方法体结束
                int brace_pos = -1;
                int k = j;
                while (k < block_end) {
                    if (content[k] == '{') { brace_pos = k; break; }
                    if (content[k] == '\n') break;
                    k++;
                }

                int method_end;
                if (brace_pos >= 0) {
                    int match = find_matching_brace(content, brace_pos);
                    method_end = (match >= 0) ? match + 1 : block_end;
                } else {
                    method_end = j;
                    while (method_end < block_end && content[method_end] != '\n') method_end++;
                }

                if (count >= cap) {
                    cap *= 2;
                    children = (LspDocumentSymbol*)realloc(children, sizeof(LspDocumentSymbol) * cap);
                }

                children[count].name = method_name;
                children[count].kind = LSP_SYM_METHOD;
                children[count].detail = NULL;
                children[count].range.start = offset_to_pos(content, func_start);
                children[count].range.end = offset_to_pos(content, method_end);
                children[count].selectionRange.start = offset_to_pos(content, name_start);
                children[count].selectionRange.end = offset_to_pos(content, name_start + name_len);
                children[count].children = NULL;
                children[count].children_count = 0;
                count++;

                i = method_end;
                continue;
            }
            if (method_name) free(method_name);
        }

        // 解析字段: <Type> <name> [array_suffix] [= default] [;]
        int type_start = i;
        while (i < block_end && (isalnum((unsigned char)content[i]) || content[i] == '_')) i++;
        if (i == type_start) { i++; continue; }

        int type_len = i - type_start;

        // 跳过泛型 [X]
        if (i < block_end && content[i] == '[') {
            int depth = 1; i++;
            while (i < block_end && depth > 0) {
                if (content[i] == '[') depth++;
                else if (content[i] == ']') depth--;
                i++;
            }
        }
        // 跳过 ?
        if (i < block_end && content[i] == '?') i++;
        // 跳过空白
        while (i < block_end && isspace((unsigned char)content[i])) i++;

        // 提取字段名
        if (i < block_end && (isalpha((unsigned char)content[i]) || content[i] == '_')) {
            int name_start = i;
            while (i < block_end && (isalnum((unsigned char)content[i]) || content[i] == '_')) i++;
            int name_len = i - name_start;

            // 跳过 [N] 数组后缀
            if (i < block_end && content[i] == '[') {
                int depth = 1; i++;
                while (i < block_end && depth > 0) {
                    if (content[i] == '[') depth++;
                    else if (content[i] == ']') depth--;
                    i++;
                }
            }

            int field_end = i;
            while (field_end < block_end && content[field_end] != '\n') field_end++;

            char* field_name = (char*)malloc(name_len + 1);
            if (field_name) {
                memcpy(field_name, content + name_start, name_len);
                field_name[name_len] = '\0';
            }

            char* detail = (char*)malloc(type_len + 1);
            if (detail) {
                memcpy(detail, content + type_start, type_len);
                detail[type_len] = '\0';
            }

            if (count >= cap) {
                cap *= 2;
                children = (LspDocumentSymbol*)realloc(children, sizeof(LspDocumentSymbol) * cap);
            }

            children[count].name = field_name;
            children[count].kind = LSP_SYM_FIELD;
            children[count].detail = detail;
            children[count].range.start = offset_to_pos(content, type_start);
            children[count].range.end = offset_to_pos(content, field_end);
            children[count].selectionRange.start = offset_to_pos(content, name_start);
            children[count].selectionRange.end = offset_to_pos(content, name_start + name_len);
            children[count].children = NULL;
            children[count].children_count = 0;
            count++;

            i = field_end;
            continue;
        }

        while (i < block_end && content[i] != '\n') i++;
    }

    parent->children = children;
    parent->children_count = count;
}

// 在 enum 块内解析枚举成员
static void parse_enum_children(const char* content, int block_start,
                                int block_end, LspDocumentSymbol* parent) {
    int cap = 8;
    LspDocumentSymbol* children = (LspDocumentSymbol*)malloc(sizeof(LspDocumentSymbol) * cap);
    int count = 0;

    int i = block_start;
    while (i < block_end) {
        i = skip_ws_comments_inline(content, i, &(int){0});
        if (i >= block_end) break;

        if (isalpha((unsigned char)content[i]) || content[i] == '_') {
            int name_start = i;
            while (i < block_end && (isalnum((unsigned char)content[i]) || content[i] == '_')) i++;
            int name_len = i - name_start;

            int member_end = i;
            while (member_end < block_end && content[member_end] != ',' && content[member_end] != '\n') member_end++;
            if (member_end < block_end && content[member_end] == ',') member_end++;

            char* member_name = (char*)malloc(name_len + 1);
            if (member_name) {
                memcpy(member_name, content + name_start, name_len);
                member_name[name_len] = '\0';
            }

            if (count >= cap) {
                cap *= 2;
                children = (LspDocumentSymbol*)realloc(children, sizeof(LspDocumentSymbol) * cap);
            }

            children[count].name = member_name;
            children[count].kind = LSP_SYM_ENUM_MEMBER;
            children[count].detail = NULL;
            children[count].range.start = offset_to_pos(content, name_start);
            children[count].range.end = offset_to_pos(content, member_end);
            children[count].selectionRange.start = offset_to_pos(content, name_start);
            children[count].selectionRange.end = offset_to_pos(content, name_start + name_len);
            children[count].children = NULL;
            children[count].children_count = 0;
            count++;

            i = member_end;
            continue;
        }
        i++;
    }

    parent->children = children;
    parent->children_count = count;
}

// 在 face 块内解析方法签名
static void parse_face_children(const char* content, int block_start,
                                int block_end, LspDocumentSymbol* parent) {
    int cap = 8;
    LspDocumentSymbol* children = (LspDocumentSymbol*)malloc(sizeof(LspDocumentSymbol) * cap);
    int count = 0;

    int i = block_start;
    while (i < block_end) {
        i = skip_ws_comments_inline(content, i, &(int){0});
        if (i >= block_end) break;

        if (strncmp(content + i, "func", 4) == 0 &&
            !isalnum((unsigned char)content[i + 4]) && content[i + 4] != '_') {
            int func_start = i;
            int j = i + 4;
            while (j < block_end && isspace((unsigned char)content[j])) j++;

            char* method_name = NULL;
            int name_start = j;
            j = (int)(extract_identifier(content + j, &method_name) - content);
            int name_len = j - name_start;

            if (method_name) {
                int method_end = j;
                while (method_end < block_end && content[method_end] != '\n') method_end++;

                // 提取返回类型
                char* detail = NULL;
                int k = j;
                while (k < method_end && content[k] != ':' && content[k] != '(') k++;
                if (k < method_end && content[k] == ':') {
                    k++;
                    while (k < method_end && isspace((unsigned char)content[k])) k++;
                    int rt_start = k;
                    while (k < method_end && !isspace((unsigned char)content[k]) &&
                           content[k] != '{' && content[k] != '(') k++;
                    int rt_len = k - rt_start;
                    if (rt_len > 0) {
                        detail = (char*)malloc(rt_len + 1);
                        if (detail) {
                            memcpy(detail, content + rt_start, rt_len);
                            detail[rt_len] = '\0';
                        }
                    }
                }

                if (count >= cap) {
                    cap *= 2;
                    children = (LspDocumentSymbol*)realloc(children, sizeof(LspDocumentSymbol) * cap);
                }

                children[count].name = method_name;
                children[count].kind = LSP_SYM_METHOD;
                children[count].detail = detail;
                children[count].range.start = offset_to_pos(content, func_start);
                children[count].range.end = offset_to_pos(content, method_end);
                children[count].selectionRange.start = offset_to_pos(content, name_start);
                children[count].selectionRange.end = offset_to_pos(content, name_start + name_len);
                children[count].children = NULL;
                children[count].children_count = 0;
                count++;

                i = method_end;
                continue;
            }
        }

        while (i < block_end && content[i] != '\n') i++;
    }

    parent->children = children;
    parent->children_count = count;
}

// 在 clib 块内解析 C 函数声明
static void parse_clib_children(const char* content, int block_start,
                                int block_end, LspDocumentSymbol* parent) {
    int cap = 8;
    LspDocumentSymbol* children = (LspDocumentSymbol*)malloc(sizeof(LspDocumentSymbol) * cap);
    int count = 0;

    int i = block_start;
    while (i < block_end) {
        i = skip_ws_comments_inline(content, i, &(int){0});
        if (i >= block_end) break;

        // 每行格式: <返回类型> FuncName(params)
        int line_start = i;
        int type_start = i;
        while (i < block_end && (isalnum((unsigned char)content[i]) || content[i] == '_')) i++;
        int type_len = i - type_start;

        // 跳过 [X]
        if (i < block_end && content[i] == '[') {
            int depth = 1; i++;
            while (i < block_end && depth > 0) {
                if (content[i] == '[') depth++;
                else if (content[i] == ']') depth--;
                i++;
            }
        }
        // 跳过空白
        while (i < block_end && isspace((unsigned char)content[i])) i++;

        // 提取函数名
        if (i < block_end && (isalpha((unsigned char)content[i]) || content[i] == '_')) {
            int name_start = i;
            while (i < block_end && (isalnum((unsigned char)content[i]) || content[i] == '_')) i++;
            int name_len = i - name_start;

            if (i < block_end && content[i] == '(') {
                char* func_name = (char*)malloc(name_len + 1);
                if (func_name) {
                    memcpy(func_name, content + name_start, name_len);
                    func_name[name_len] = '\0';
                }

                char* detail = NULL;
                if (type_len > 0) {
                    detail = (char*)malloc(type_len + 1);
                    if (detail) {
                        memcpy(detail, content + type_start, type_len);
                        detail[type_len] = '\0';
                    }
                }

                int line_end = i;
                while (line_end < block_end && content[line_end] != '\n') line_end++;

                if (count >= cap) {
                    cap *= 2;
                    children = (LspDocumentSymbol*)realloc(children, sizeof(LspDocumentSymbol) * cap);
                }

                children[count].name = func_name;
                children[count].kind = LSP_SYM_FUNCTION;
                children[count].detail = detail;
                children[count].range.start = offset_to_pos(content, line_start);
                children[count].range.end = offset_to_pos(content, line_end);
                children[count].selectionRange.start = offset_to_pos(content, name_start);
                children[count].selectionRange.end = offset_to_pos(content, name_start + name_len);
                children[count].children = NULL;
                children[count].children_count = 0;
                count++;

                i = line_end;
                continue;
            }
        }

        while (i < block_end && content[i] != '\n') i++;
    }

    parent->children = children;
    parent->children_count = count;
}

/* ========== 主扫描函数 ========== */

LspDocumentSymbol* lsp_get_document_symbols(const char* content, int* count) {
    *count = 0;
    if (!content) return NULL;

    int cap = 16;
    LspDocumentSymbol* symbols = (LspDocumentSymbol*)malloc(sizeof(LspDocumentSymbol) * cap);
    if (!symbols) return NULL;

    const char* p = content;
    int line = 0;

    while (*p) {
        // 跳过行注释
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            if (*p) { p++; line++; }
            continue;
        }
        // 跳过块注释
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') line++;
                p++;
            }
            if (*p) p += 2;
            continue;
        }
        // 跳过字符串字面量
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            p++;
            while (*p && *p != quote) {
                if (*p == '\\' && *(p + 1)) p++;
                if (*p == '\n') line++;
                p++;
            }
            if (*p) p++;
            continue;
        }

        // 检查行首边界
        if (!at_line_boundary(content, p)) {
            if (*p == '\n') { p++; line++; }
            else p++;
            continue;
        }

        int decl_offset = (int)(p - content);
        int decl_line = line;

        // 跳过 packed / align(N) 属性
        const char* after_attr = skip_attributes(p);
        if (after_attr != p) {
            for (const char* q = p; q < after_attr; q++) {
                if (*q == '\n') line++;
            }
            p = after_attr;
        }

        // 检查 "export " 前缀
        bool is_export = false;
        if (strncmp(p, "export", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
            is_export = true;
            p += 6;
            while (*p && isspace((unsigned char)*p)) {
                if (*p == '\n') line++;
                p++;
            }
        }

        // 匹配声明关键字
        int kw_len = 0;
        enum { KW_FUNC, KW_STRUCT, KW_CSTRUCT, KW_ENUM, KW_FACE, KW_CLIB,
               KW_VAR, KW_CONST, KW_IMPORT, KW_NONE } kw = KW_NONE;

        // cstruct 必须先于 struct 检查
        if (strncmp(p, "cstruct", 7) == 0 && !isalnum((unsigned char)p[7]) && p[7] != '_') {
            kw = KW_CSTRUCT; kw_len = 7;
        } else if (strncmp(p, "clib", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
            kw = KW_CLIB; kw_len = 4;
        } else if (strncmp(p, "cfunc", 5) == 0 && !isalnum((unsigned char)p[5]) && p[5] != '_') {
            kw = KW_FUNC; kw_len = 5;  // cfunc 归类为函数
        } else if (strncmp(p, "struct", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
            kw = KW_STRUCT; kw_len = 6;
        } else if (strncmp(p, "func", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
            kw = KW_FUNC; kw_len = 4;
        } else if (strncmp(p, "enum", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
            kw = KW_ENUM; kw_len = 4;
        } else if (strncmp(p, "face", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
            kw = KW_FACE; kw_len = 4;
        } else if (strncmp(p, "const", 5) == 0 && !isalnum((unsigned char)p[5]) && p[5] != '_') {
            kw = KW_CONST; kw_len = 5;
        } else if (strncmp(p, "var", 3) == 0 && !isalnum((unsigned char)p[3]) && p[3] != '_') {
            kw = KW_VAR; kw_len = 3;
        } else if (strncmp(p, "import", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
            kw = KW_IMPORT; kw_len = 6;
        }

        if (kw == KW_NONE) {
            if (*p == '\n') { p++; line++; }
            else p++;
            continue;
        }

        // 跳过关键字
        p += kw_len;
        while (*p && isspace((unsigned char)*p)) {
            if (*p == '\n') line++;
            p++;
        }

        // --- import ---
        if (kw == KW_IMPORT) {
            char* import_name = NULL;
            if (*p == '"') {
                p++;
                const char* start = p;
                while (*p && *p != '"') {
                    if (*p == '\n') line++;
                    p++;
                }
                int len = (int)(p - start);
                import_name = (char*)malloc(len + 1);
                if (import_name) {
                    memcpy(import_name, start, len);
                    import_name[len] = '\0';
                }
                if (*p) p++;
            } else {
                p = extract_identifier(p, &import_name);
            }

            if (import_name) {
                // 查找 "as alias"
                const char* q = p;
                while (*q && *q != '\n') {
                    if (strncmp(q, "as ", 3) == 0 && (q == p || isspace((unsigned char)q[-1]))) {
                        q += 3;
                        while (*q && isspace((unsigned char)*q)) q++;
                        char* alias = NULL;
                        extract_identifier(q, &alias);
                        if (alias) {
                            free(import_name);
                            import_name = alias;
                        }
                        break;
                    }
                    q++;
                }

                int import_end = (int)(p - content);
                while (content[import_end] && content[import_end] != '\n') import_end++;

                if (*count >= cap) {
                    cap *= 2;
                    symbols = (LspDocumentSymbol*)realloc(symbols, sizeof(LspDocumentSymbol) * cap);
                }

                symbols[*count].name = import_name;
                symbols[*count].kind = LSP_SYM_MODULE;
                symbols[*count].detail = strdup("import");
                symbols[*count].range.start = offset_to_pos(content, decl_offset);
                symbols[*count].range.end = offset_to_pos(content, import_end);
                symbols[*count].selectionRange.start = offset_to_pos(content, decl_offset);
                symbols[*count].selectionRange.end = offset_to_pos(content, import_end);
                symbols[*count].children = NULL;
                symbols[*count].children_count = 0;
                (*count)++;

                while (*p && *p != '\n') p++;
                if (*p) { p++; line++; }
            }
            continue;
        }

        // --- func ---
        if (kw == KW_FUNC) {
            char* func_name = NULL;
            int name_offset = (int)(p - content);
            p = extract_identifier(p, &func_name);
            int name_len = (int)(p - content) - name_offset;

            if (func_name && name_len > 0) {
                // 查找 {
                int brace_pos = -1;
                const char* bp = p;
                int scan_line = line;
                while (*bp) {
                    if (*bp == '"' || *bp == '\'') {
                        char q = *bp; bp++;
                        while (*bp && *bp != q) {
                            if (*bp == '\\' && *(bp + 1)) bp++;
                            if (*bp == '\n') scan_line++;
                            bp++;
                        }
                        if (*bp) bp++;
                        continue;
                    }
                    if (*bp == '/' && bp[1] == '/') {
                        while (*bp && *bp != '\n') bp++;
                        continue;
                    }
                    if (*bp == '{') { brace_pos = (int)(bp - content); break; }
                    if (*bp == '\n') scan_line++;
                    bp++;
                }

                int func_end;
                int end_line = scan_line;
                if (brace_pos >= 0) {
                    int match = find_matching_brace(content, brace_pos);
                    if (match >= 0) {
                        func_end = match + 1;
                        end_line = 0;
                        for (int i = 0; i <= match; i++)
                            if (content[i] == '\n') end_line++;
                    } else {
                        func_end = (int)(bp - content) + 1;
                        end_line = scan_line;
                    }
                } else {
                    func_end = name_offset + name_len;
                    while (content[func_end] && content[func_end] != '\n') func_end++;
                }

                // 提取返回类型作为 detail
                char* detail = NULL;
                {
                    int k = name_offset + name_len;
                    while (k < func_end && content[k] != ':' && content[k] != '{' && content[k] != '\n') k++;
                    if (k < func_end && content[k] == ':') {
                        k++;
                        while (k < func_end && isspace((unsigned char)content[k])) k++;
                        int rt_start = k;
                        while (k < func_end && !isspace((unsigned char)content[k]) &&
                               content[k] != '{' && content[k] != '(' && content[k] != '\n') k++;
                        int rt_len = k - rt_start;
                        if (rt_len > 0) {
                            detail = (char*)malloc(rt_len + 1);
                            if (detail) {
                                memcpy(detail, content + rt_start, rt_len);
                                detail[rt_len] = '\0';
                            }
                        }
                    }
                }

                char detail_buf[128];
                if (!detail) {
                    if (is_export) {
                        snprintf(detail_buf, sizeof(detail_buf), "export");
                        detail = strdup(detail_buf);
                    }
                } else if (is_export) {
                    snprintf(detail_buf, sizeof(detail_buf), "export -> %s", detail);
                    free(detail);
                    detail = strdup(detail_buf);
                }

                if (*count >= cap) {
                    cap *= 2;
                    symbols = (LspDocumentSymbol*)realloc(symbols, sizeof(LspDocumentSymbol) * cap);
                }

                symbols[*count].name = func_name;
                symbols[*count].kind = LSP_SYM_FUNCTION;
                symbols[*count].detail = detail;
                symbols[*count].range.start.line = decl_line;
                symbols[*count].range.start.character = 0;
                symbols[*count].range.end = offset_to_pos(content, func_end);
                symbols[*count].selectionRange.start = offset_to_pos(content, name_offset);
                symbols[*count].selectionRange.end = offset_to_pos(content, name_offset + name_len);
                symbols[*count].children = NULL;
                symbols[*count].children_count = 0;
                (*count)++;

                p = content + func_end;
                line = end_line;
                continue;
            }
            if (func_name) free(func_name);
        }

        // --- struct / cstruct ---
        if (kw == KW_STRUCT || kw == KW_CSTRUCT) {
            char* type_name = NULL;
            int name_offset = (int)(p - content);
            p = extract_identifier(p, &type_name);
            int name_len = (int)(p - content) - name_offset;

            if (type_name && name_len > 0) {
                // 查找 {
                int brace_pos = -1;
                const char* bp = p;
                int scan_line = line;
                while (*bp) {
                    if (*bp == '"' || *bp == '\'') {
                        char q = *bp; bp++;
                        while (*bp && *bp != q) {
                            if (*bp == '\\' && *(bp + 1)) bp++;
                            if (*bp == '\n') scan_line++;
                            bp++;
                        }
                        if (*bp) bp++;
                        continue;
                    }
                    if (*bp == '/' && bp[1] == '/') {
                        while (*bp && *bp != '\n') bp++;
                        continue;
                    }
                    if (*bp == '{') { brace_pos = (int)(bp - content); break; }
                    if (*bp == '\n') scan_line++;
                    bp++;
                }

                int type_end;
                int end_line = scan_line;
                if (brace_pos >= 0) {
                    int match = find_matching_brace(content, brace_pos);
                    if (match >= 0) {
                        type_end = match + 1;
                        end_line = 0;
                        for (int i = 0; i <= match; i++)
                            if (content[i] == '\n') end_line++;
                    } else {
                        type_end = (int)(bp - content) + 1;
                        end_line = scan_line;
                    }
                } else {
                    type_end = name_offset + name_len;
                    while (content[type_end] && content[type_end] != '\n') type_end++;
                }

                // detail
                char detail_buf[128];
                const char* kind_str = (kw == KW_CSTRUCT) ? "cstruct" : "struct";
                if (is_export)
                    snprintf(detail_buf, sizeof(detail_buf), "export %s", kind_str);
                else
                    snprintf(detail_buf, sizeof(detail_buf), "%s", kind_str);

                if (*count >= cap) {
                    cap *= 2;
                    symbols = (LspDocumentSymbol*)realloc(symbols, sizeof(LspDocumentSymbol) * cap);
                }

                symbols[*count].name = type_name;
                symbols[*count].kind = LSP_SYM_STRUCT;
                symbols[*count].detail = strdup(detail_buf);
                symbols[*count].range.start.line = decl_line;
                symbols[*count].range.start.character = 0;
                symbols[*count].range.end = offset_to_pos(content, type_end);
                symbols[*count].selectionRange.start = offset_to_pos(content, name_offset);
                symbols[*count].selectionRange.end = offset_to_pos(content, name_offset + name_len);
                symbols[*count].children = NULL;
                symbols[*count].children_count = 0;

                // 解析子符号 (字段和方法)
                if (brace_pos >= 0) {
                    int match = find_matching_brace(content, brace_pos);
                    if (match >= 0) {
                        parse_struct_children(content, brace_pos + 1, match, &symbols[*count]);
                    }
                }

                (*count)++;

                p = content + type_end;
                line = end_line;
                continue;
            }
            if (type_name) free(type_name);
        }

        // --- enum ---
        if (kw == KW_ENUM) {
            char* enum_name = NULL;
            int name_offset = (int)(p - content);
            p = extract_identifier(p, &enum_name);
            int name_len = (int)(p - content) - name_offset;

                if (enum_name && name_len > 0) {
                int brace_pos = -1;
                const char* bp = p;
                int scan_line = line;
                while (*bp) {
                    if (*bp == '{') { brace_pos = (int)(bp - content); break; }
                    if (*bp == '\n') scan_line++;
                    bp++;
                }

                int type_end;
                int end_line = scan_line;
                if (brace_pos >= 0) {
                    int match = find_matching_brace(content, brace_pos);
                    if (match >= 0) {
                        type_end = match + 1;
                        end_line = 0;
                        for (int i = 0; i <= match; i++)
                            if (content[i] == '\n') end_line++;
                    } else {
                        type_end = (int)(bp - content) + 1;
                        end_line = scan_line;
                    }
                } else {
                    type_end = name_offset + name_len;
                    while (content[type_end] && content[type_end] != '\n') type_end++;
                }

                char detail_buf[128];
                snprintf(detail_buf, sizeof(detail_buf), "%s", is_export ? "export enum" : "enum");

                if (*count >= cap) {
                    cap *= 2;
                    symbols = (LspDocumentSymbol*)realloc(symbols, sizeof(LspDocumentSymbol) * cap);
                }

                symbols[*count].name = enum_name;
                symbols[*count].kind = LSP_SYM_ENUM;
                symbols[*count].detail = strdup(detail_buf);
                symbols[*count].range.start.line = decl_line;
                symbols[*count].range.start.character = 0;
                symbols[*count].range.end = offset_to_pos(content, type_end);
                symbols[*count].selectionRange.start = offset_to_pos(content, name_offset);
                symbols[*count].selectionRange.end = offset_to_pos(content, name_offset + name_len);
                symbols[*count].children = NULL;
                symbols[*count].children_count = 0;

                if (brace_pos >= 0) {
                    int match2 = find_matching_brace(content, brace_pos);
                    if (match2 >= 0) {
                        parse_enum_children(content, brace_pos + 1, match2, &symbols[*count]);
                    }
                }

                (*count)++;

                p = content + type_end;
                line = end_line;
                continue;
            }
            if (enum_name) free(enum_name);
        }

        // --- face ---
        if (kw == KW_FACE) {
            char* face_name = NULL;
            int name_offset = (int)(p - content);
            p = extract_identifier(p, &face_name);
            int name_len = (int)(p - content) - name_offset;

            if (face_name && name_len > 0) {
                int brace_pos = -1;
                const char* bp = p;
                int scan_line = line;
                while (*bp) {
                    if (*bp == '{') { brace_pos = (int)(bp - content); break; }
                    if (*bp == '\n') scan_line++;
                    bp++;
                }

                int type_end;
                int end_line = scan_line;
                if (brace_pos >= 0) {
                    int match = find_matching_brace(content, brace_pos);
                    if (match >= 0) {
                        type_end = match + 1;
                        end_line = 0;
                        for (int i = 0; i <= match; i++)
                            if (content[i] == '\n') end_line++;
                    } else {
                        type_end = (int)(bp - content) + 1;
                        end_line = scan_line;
                    }
                } else {
                    type_end = name_offset + name_len;
                    while (content[type_end] && content[type_end] != '\n') type_end++;
                }

                char detail_buf[128];
                snprintf(detail_buf, sizeof(detail_buf), "%s", is_export ? "export face" : "face");

                if (*count >= cap) {
                    cap *= 2;
                    symbols = (LspDocumentSymbol*)realloc(symbols, sizeof(LspDocumentSymbol) * cap);
                }

                symbols[*count].name = face_name;
                symbols[*count].kind = LSP_SYM_INTERFACE;
                symbols[*count].detail = strdup(detail_buf);
                symbols[*count].range.start.line = decl_line;
                symbols[*count].range.start.character = 0;
                symbols[*count].range.end = offset_to_pos(content, type_end);
                symbols[*count].selectionRange.start = offset_to_pos(content, name_offset);
                symbols[*count].selectionRange.end = offset_to_pos(content, name_offset + name_len);
                symbols[*count].children = NULL;
                symbols[*count].children_count = 0;

                if (brace_pos >= 0) {
                    int match2 = find_matching_brace(content, brace_pos);
                    if (match2 >= 0) {
                        parse_face_children(content, brace_pos + 1, match2, &symbols[*count]);
                    }
                }

                (*count)++;

                p = content + type_end;
                line = end_line;
                continue;
            }
            if (face_name) free(face_name);
        }

        // --- clib ---
        if (kw == KW_CLIB) {
            char* clib_name = NULL;
            int name_offset = (int)(p - content);
            p = extract_identifier(p, &clib_name);
            int name_len = (int)(p - content) - name_offset;

            if (clib_name && name_len > 0) {
                int brace_pos = -1;
                const char* bp = p;
                int scan_line = line;
                while (*bp) {
                    if (*bp == '{') { brace_pos = (int)(bp - content); break; }
                    if (*bp == '\n') scan_line++;
                    bp++;
                }

                int type_end;
                int end_line = scan_line;
                if (brace_pos >= 0) {
                    int match = find_matching_brace(content, brace_pos);
                    if (match >= 0) {
                        type_end = match + 1;
                        end_line = 0;
                        for (int i = 0; i <= match; i++)
                            if (content[i] == '\n') end_line++;
                    } else {
                        type_end = (int)(bp - content) + 1;
                        end_line = scan_line;
                    }
                } else {
                    type_end = name_offset + name_len;
                    while (content[type_end] && content[type_end] != '\n') type_end++;
                }

                char detail_buf[128];
                snprintf(detail_buf, sizeof(detail_buf), "%s", is_export ? "export clib" : "clib");

                if (*count >= cap) {
                    cap *= 2;
                    symbols = (LspDocumentSymbol*)realloc(symbols, sizeof(LspDocumentSymbol) * cap);
                }

                symbols[*count].name = clib_name;
                symbols[*count].kind = LSP_SYM_MODULE;
                symbols[*count].detail = strdup(detail_buf);
                symbols[*count].range.start.line = decl_line;
                symbols[*count].range.start.character = 0;
                symbols[*count].range.end = offset_to_pos(content, type_end);
                symbols[*count].selectionRange.start = offset_to_pos(content, name_offset);
                symbols[*count].selectionRange.end = offset_to_pos(content, name_offset + name_len);
                symbols[*count].children = NULL;
                symbols[*count].children_count = 0;

                if (brace_pos >= 0) {
                    int match2 = find_matching_brace(content, brace_pos);
                    if (match2 >= 0) {
                        parse_clib_children(content, brace_pos + 1, match2, &symbols[*count]);
                    }
                }

                (*count)++;

                p = content + type_end;
                line = end_line;
                continue;
            }
            if (clib_name) free(clib_name);
        }

        // --- var / const ---
        if (kw == KW_VAR || kw == KW_CONST) {
            char* var_name = NULL;
            int name_offset = (int)(p - content);
            p = extract_identifier(p, &var_name);
            int name_len = (int)(p - content) - name_offset;

            if (var_name && name_len > 0) {
                int var_end = name_offset + name_len;
                while (content[var_end] && content[var_end] != '\n') var_end++;

                char detail_buf[128];
                snprintf(detail_buf, sizeof(detail_buf), "%s%s",
                         is_export ? "export " : "",
                         kw == KW_CONST ? "const" : "var");

                if (*count >= cap) {
                    cap *= 2;
                    symbols = (LspDocumentSymbol*)realloc(symbols, sizeof(LspDocumentSymbol) * cap);
                }

                symbols[*count].name = var_name;
                symbols[*count].kind = (kw == KW_CONST) ? LSP_SYM_CONSTANT : LSP_SYM_VARIABLE;
                symbols[*count].detail = strdup(detail_buf);
                symbols[*count].range.start.line = decl_line;
                symbols[*count].range.start.character = 0;
                symbols[*count].range.end = offset_to_pos(content, var_end);
                symbols[*count].selectionRange.start = offset_to_pos(content, name_offset);
                symbols[*count].selectionRange.end = offset_to_pos(content, name_offset + name_len);
                symbols[*count].children = NULL;
                symbols[*count].children_count = 0;
                (*count)++;

                p = content + var_end;
                continue;
            }
            if (var_name) free(var_name);
        }

        // 未匹配任何关键字，跳过
        if (*p == '\n') { p++; line++; }
        else p++;
    }

    if (*count == 0) {
        free(symbols);
        return NULL;
    }

    return symbols;
}

/* ========== 释放函数 ========== */

static void free_symbol_recursive(LspDocumentSymbol* sym) {
    if (!sym) return;
    if (sym->children) {
        for (int i = 0; i < sym->children_count; i++) {
            free_symbol_recursive(&sym->children[i]);
        }
        free(sym->children);
    }
    free(sym->name);
    free(sym->detail);
}

void lsp_free_document_symbols(LspDocumentSymbol* symbols, int count) {
    if (!symbols) return;
    for (int i = 0; i < count; i++) {
        free_symbol_recursive(&symbols[i]);
    }
    free(symbols);
}

/* ========== LSP 请求处理 ========== */

static JsonValue* symbol_to_json(const LspDocumentSymbol* sym) {
    JsonValue* obj = json_object_new();

    json_object_set(obj, "name", json_string_new(sym->name));
    json_object_set(obj, "kind", json_int_new((int)sym->kind));

    if (sym->detail) {
        json_object_set(obj, "detail", json_string_new(sym->detail));
    }

    // range
    JsonValue* range = json_object_new();
    JsonValue* start = json_object_new();
    json_object_set(start, "line", json_int_new((int)sym->range.start.line));
    json_object_set(start, "character", json_int_new((int)sym->range.start.character));
    JsonValue* end = json_object_new();
    json_object_set(end, "line", json_int_new((int)sym->range.end.line));
    json_object_set(end, "character", json_int_new((int)sym->range.end.character));
    json_object_set(range, "start", start);
    json_object_set(range, "end", end);
    json_object_set(obj, "range", range);

    // selectionRange
    JsonValue* sel_range = json_object_new();
    JsonValue* sel_start = json_object_new();
    json_object_set(sel_start, "line", json_int_new((int)sym->selectionRange.start.line));
    json_object_set(sel_start, "character", json_int_new((int)sym->selectionRange.start.character));
    JsonValue* sel_end = json_object_new();
    json_object_set(sel_end, "line", json_int_new((int)sym->selectionRange.end.line));
    json_object_set(sel_end, "character", json_int_new((int)sym->selectionRange.end.character));
    json_object_set(sel_range, "start", sel_start);
    json_object_set(sel_range, "end", sel_end);
    json_object_set(obj, "selectionRange", sel_range);

    // children (递归)
    if (sym->children && sym->children_count > 0) {
        JsonValue* children = json_array_new();
        for (int i = 0; i < sym->children_count; i++) {
            json_array_add(children, symbol_to_json(&sym->children[i]));
        }
        json_object_set(obj, "children", children);
    }

    return obj;
}

char* lsp_handle_document_symbol(LspServer* server, int id, JsonValue* params) {
    if (!server || server->state != LSP_STATE_INITIALIZED) {
        return lsp_create_error(id, LSP_ERROR_SERVER_NOT_INITIALIZED,
                                "Server not initialized");
    }

    JsonValue* text_doc = json_object_get(params, "textDocument");
    if (!text_doc) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing textDocument");
    }

    JsonValue* uri = json_object_get(text_doc, "uri");
    if (!uri || uri->type != JSON_STRING) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing document URI");
    }

    LspTextDocument* doc = lsp_document_get(server, json_string_value(uri));
    if (!doc) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Document not found");
    }

    int count = 0;
    LspDocumentSymbol* symbols = lsp_get_document_symbols(doc->content, &count);

    // 构建响应: DocumentSymbol[] 数组
    JsonValue* result = NULL;
    if (symbols && count > 0) {
        result = json_array_new();
        for (int i = 0; i < count; i++) {
            json_array_add(result, symbol_to_json(&symbols[i]));
        }
    } else {
        result = json_array_new();  // 空数组
    }

    char* response = lsp_create_response(id, result);

    json_free(result);
    lsp_free_document_symbols(symbols, count);

    return response;
}
