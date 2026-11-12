#include "../include/lenolang.h"
#include "parser_internal.h"

// ============================================================================
// 语句解析分发器
// ============================================================================

// ★ 2026-10-14：当前 token 是不是**指定的那个词**（长度 + 内容都比 ⇒ `let` 不会误配 `letter` ✓）
//   用途：识别"跨语言关键字"（它们在 Leno 里都**不是关键字** ⇒ 会被当标识符往下走，
//   最后报一串指不出真因的语法错 ✗，见 parse_statement 里的说明 ✓）
static int cur_ident_is(Parser* p, const char* word) {
    if (p->lex.current.type != TOK_IDENT) return 0;
    size_t n = strlen(word);
    return (size_t)p->lex.current.len == n &&
           strncmp(p->lex.current.text, word, n) == 0;
}

// ==================== 注解（2026-10-31）====================
//   语法：`@名字` / `@名字(字面量)` / `@名字([字符串, ...])`
//   设计见 docs/注解系统与optkeys设计草案.md ✓
//   ⚠ 一期只收**字面量参数**：允许表达式就要引入"注解求值"（失败语义/顺序/循环依赖），
//     收益不值当 —— 报错文案明确写"注解参数必须是字面量" ✓

// 复制 token 文本（`Lexer` 的 text 指向源串，不保证 null 结尾 ⇒ 按 len 拷）
static char* anno_dup_text(Token tok) {
    char* s = (char*)malloc((size_t)tok.len + 1);
    if (!s) return NULL;
    memcpy(s, tok.text, (size_t)tok.len);
    s[tok.len] = '\0';
    return s;
}

// 只释放"内部字符串"，**不**释放结构体本身 —— 给栈上的单个 AstAnno 用
//   ⚠ 早期版本在失败路径里对 `&a`（栈变量）调了 anno_free_list ⇒ free 栈指针（UB ✗），
//     gcc 的 -Wfree-nonheap-object 当场抓到（2026-10-31）
static void anno_free_body(AstAnno* a) {
    if (!a) return;
    free(a->name);        a->name = NULL;
    free(a->arg_str);     a->arg_str = NULL;
    if (a->arg_list) {
        for (int k = 0; k < a->arg_list_len; k++) free(a->arg_list[k]);
        free(a->arg_list);
        a->arg_list = NULL;
        a->arg_list_len = 0;
    }
}

static void anno_free_list(AstAnno* annos, int n) {
    if (!annos) return;
    for (int i = 0; i < n; i++) anno_free_body(&annos[i]);
    free(annos);
}

// 注解能挂的"真正声明"（穿透 export 包装 ⇒ `@x export func …` 也合法 ✓）
static Ast* anno_target_of(Ast* a) {
    while (a && a->kind == AST_EXPORT) a = a->u.export.decl;
    return a;
}

static int anno_attachable(Ast* target) {
    if (!target) return 0;
    switch (target->kind) {
        case AST_FUNC_DEF:
        case AST_STRUCT_DEF:
        case AST_VAR_DECL:      // const 也走这个 kind（is_const 字段区分）
            return 1;
        default:
            return 0;
    }
}

// 已知注解表（2026-10-31）：拼错直接报，等于没写的东西最难发现 ✓
//   后续加 @deprecated / @arity 时在这里登记，并在别处补消费逻辑 ✓
static int anno_is_known(const char* name) {
    return name && strcmp(name, "optkeys") == 0;
}

// 解析一个 `@名字[(参数)]`，追加到 annos 数组末尾；失败返回 0
static int parse_one_anno(Parser* p, AstAnno** annos, int* n, int* cap) {
    int line = p->lex.current.line;
    int col = p->lex.current.column;
    lexer_next(&p->lex);                       // 吃掉 '@'

    if (p->lex.current.type != TOK_IDENT) {
        error_add_at_fmt(ERR_SYNTAX, line, 0, "'@' 后面必须跟注解名（如 `@optkeys([...])`）");
        return 0;
    }
    AstAnno a;
    memset(&a, 0, sizeof(a));
    a.line = line;
    a.name = anno_dup_text(p->lex.current);
    lexer_next(&p->lex);                       // 吃掉名字

    // 未知注解 ⇒ 报错（带"你是不是想要"）
    if (!anno_is_known(a.name)) {
        const char* hint = (a.name && strlen(a.name) <= 8) ? "optkeys" : NULL;
        if (hint) {
            error_add_at_fmt(ERR_SYNTAX, line, 0,
                "不认识的注解 '@%s'\n  提示: 你是不是想要 '@%s'？", a.name, hint);
        } else {
            error_add_at_fmt(ERR_SYNTAX, line, 0,
                "不认识的注解 '@%s'（当前只支持 @optkeys）", a.name);
        }
        anno_free_body(&a);
        return 0;
    }

    // 可选参数
    if (p->lex.current.type == TOK_LPAREN) {
        lexer_next(&p->lex);
        if (p->lex.current.type == TOK_STRING) {
            a.arg_kind = 1;
            a.arg_str = anno_dup_text(p->lex.current);
            lexer_next(&p->lex);
        } else if (p->lex.current.type == TOK_LBRACKET) {
            // 字符串数组
            a.arg_kind = 5;
            lexer_next(&p->lex);
            int listCap = 0;
            while (p->lex.current.type != TOK_RBRACKET && p->lex.current.type != TOK_EOF) {
                if (p->lex.current.type != TOK_STRING) {
                    error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                        "注解的数组参数只能是字符串字面量（如 `@optkeys([\"w\", \"h\"])`）");
                    anno_free_body(&a);
                    return 0;
                }
                if (a.arg_list_len >= listCap) {
                    listCap = listCap ? listCap * 2 : 8;
                    a.arg_list = (char**)realloc(a.arg_list, sizeof(char*) * (size_t)listCap);
                }
                a.arg_list[a.arg_list_len++] = anno_dup_text(p->lex.current);
                lexer_next(&p->lex);
                if (p->lex.current.type == TOK_COMMA) lexer_next(&p->lex);
            }
            if (p->lex.current.type != TOK_RBRACKET) {
                error_add_at_fmt(ERR_SYNTAX, line, 0, "注解的数组参数没有用 ']' 收尾");
                anno_free_body(&a);
                return 0;
            }
            lexer_next(&p->lex);
        } else if (p->lex.current.type == TOK_NUM) {
            // 词法层只有 TOK_NUM（不区分整数/小数）⇒ 按文本里有没有 '.' 分流
            char* tmp = anno_dup_text(p->lex.current);
            if (tmp && strchr(tmp, '.')) {
                a.arg_kind = 3;
                a.arg_float = atof(tmp);
            } else {
                a.arg_kind = 2;
                a.arg_int = tmp ? atoll(tmp) : 0;
            }
            free(tmp);
            lexer_next(&p->lex);
        } else if (p->lex.current.type == TOK_TRUE || p->lex.current.type == TOK_FALSE) {
            a.arg_kind = 4;
            a.arg_bool = (p->lex.current.type == TOK_TRUE) ? 1 : 0;
            lexer_next(&p->lex);
        } else {
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                "注解参数必须是字面量（字符串 / 数字 / true / false / 字符串数组）");
            anno_free_body(&a);
            return 0;
        }
        if (p->lex.current.type != TOK_RPAREN) {
            // ★ 2026-11-07：`@optkeys("a", "b")`（多参数）此前只报"期望 ')'"，
            //   看不出"注解只收一个参数、多个值要写成数组" ✗
            if (p->lex.current.type == TOK_COMMA) {
                error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                    "注解只接受**一个**参数 —— 多个值请写成字符串数组（如 `@optkeys([\"a\", \"b\"])`）");
            } else {
                error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                    "注解参数后面期望 ')'");
            }
            anno_free_body(&a);
            return 0;
        }
        lexer_next(&p->lex);                   // 吃掉 ')'
    }

    // ★ 2026-11-08：`@optkeys` 的参数必须是字符串数组（或字符串 "*"）。
    //   此前 `@optkeys(1)` / `@optkeys(true)` / 裸 `@optkeys` 都**静默接受** ⇒ 等于没写 ✗
    if (strcmp(a.name, "optkeys") == 0 &&
        (a.arg_kind != 1 && a.arg_kind != 5)) {
        error_add_at_fmt(ERR_SYNTAX, line, col,
            "@optkeys 的参数必须是字符串数组（如 `@optkeys([\"w\", \"h\"])`）或通配 \"*\""
            "%s", a.arg_kind == 0 ? " —— 当前没给参数（等于没写 ✗）" :
                  " —— 当前给的是数字/bool（等于没写 ✗）");
        anno_free_body(&a);
        return 0;
    }

    // 追加
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *annos = (AstAnno*)realloc(*annos, sizeof(AstAnno) * (size_t)(*cap));
    }
    (*annos)[(*n)++] = a;
    return 1;
}

// 预读一个 token，判断当前标识符是不是"关键字形态"（而不是同名变量被赋值/调用/索引）。
// ★ 2026-11-07：跨语言关键字提示要防误报 —— 实测 `bool match = true; … match = false`
//   是**合法变量赋值**，若只看词面就会把 `match` 当关键字报错 ✗。
static int ident_looks_like_keyword(Parser* p) {
    Lexer saved = p->lex;
    lexer_next(&p->lex);
    LenoTokenType t = p->lex.current.type;
    p->lex = saved;
    error_set_column(saved.current.column);
    switch (t) {
        case TOK_EQ: case TOK_PLUSEQ: case TOK_MINUSEQ: case TOK_STAREQ:
        case TOK_SLASHEQ: case TOK_MODEQ: case TOK_BITANDEQ: case TOK_BITOREQ:
        case TOK_BITXOREQ: case TOK_SHLEQ: case TOK_SHREQ: case TOK_USHREQ:
        case TOK_COALESCE_ASSIGN: case TOK_INC: case TOK_DEC:
        case TOK_LPAREN: case TOK_LBRACKET: case TOK_DOT: case TOK_COMMA:
        case TOK_SEMI: case TOK_RPAREN: case TOK_RBRACKET: case TOK_RBRACE:
            return 0;   // 赋值 / 调用 / 索引 / 成员访问 ⇒ 是变量，不是关键字
        default:
            return 1;
    }
}

Ast* parse_statement(Parser* p) {
    Ast* stmt = NULL;

    // 注解（2026-10-31）：先把 `@xxx` 链收完，再递归解析真正的声明，最后挂上去。
    //   递归调用自身 ⇒ `@a @b export func …` 天然可用（export 那条也走本函数 ✓）
    if (p->lex.current.type == TOK_AT) {
        AstAnno* annos = NULL;
        int anno_n = 0, anno_cap = 0;
        while (p->lex.current.type == TOK_AT) {
            if (!parse_one_anno(p, &annos, &anno_n, &anno_cap)) {
                anno_free_list(annos, anno_n);
                return NULL;
            }
        }
        Ast* inner = parse_statement(p);
        if (!inner) { anno_free_list(annos, anno_n); return NULL; }
        Ast* target = anno_target_of(inner);
        if (!anno_attachable(target)) {
            error_add_at_fmt(ERR_SYNTAX, annos[0].line, 0,
                "注解只能用在模块级声明（func / struct / var / const）之前");
            anno_free_list(annos, anno_n);
            return inner;          // 声明本身已经解析出来了 ⇒ 丢掉注解但不中断编译（少刷级联错 ✓）
        }
        target->annos = annos;
        target->anno_count = anno_n;
        // ★ 2026-11-08：@optkeys 描述"这个函数调用时还能接受哪些可选键" ⇒ 只能挂函数。
        //   此前挂在 `var` / `struct` 上被**静默接受**（等于没写 ✗）
        if (target->kind != AST_FUNC_DEF) {
            for (int ai = 0; ai < anno_n; ai++) {
                if (strcmp(annos[ai].name, "optkeys") == 0) {
                    error_add_at_fmt(ERR_SYNTAX, annos[ai].line, 0,
                        "@optkeys 只能挂在函数上 —— 它描述的是该函数调用时可接受的可选键；"
                        "当前声明不是函数 ✗");
                    target->annos = NULL;
                    target->anno_count = 0;
                    anno_free_list(annos, anno_n);
                    return inner;
                }
            }
        }
        return inner;
    }

    // defer（2026-10-22）：单独一层，不进下面的各分支 —— parse_block_internal
    // 会把它收集起来、在本块结束时逆序降级成 finally ✓
    if (p->lex.current.type == TOK_DEFER) {
        return parse_defer_stmt(p);
    }

    // ★ 2026-10-14：**跨语言关键字** ⇒ 定向报错（AI / 跨语言作者的高频笔误）
    //   这些词在 Leno 里都**不是关键字** ⇒ 解析器把它们当"类型名"或"表达式"继续往下走，
    //   最后报的是「声明语句后期望换行或 ';'」「期望表达式」这类**指不出真因**的话 ✗
    //   （实测：同一个文件里写 `let x = 1` + `function f(){}` 报 **9 条**、`elif` 报 2 条 ✓）。
    //   放在**语句入口这一处** = 单一收口（不必在每个声明分支里各写一遍 ✓）。
    //   ⚠ 只在**行首/语句开头**命中 ⇒ `var let = 1` 这类"真有个同名变量"的写法完全不受影响 ✓
    if (p->lex.current.type == TOK_IDENT) {
        const char* sub = NULL;
        if (cur_ident_is(p, "let")) {
            sub = "Leno **没有 `let`** ✗ —— 声明变量写 `int x = 1`（显式类型）"
                  "或 `var x = 1`（按初值推断）✓";
        } else if (cur_ident_is(p, "function")) {
            sub = "Leno **没有 `function`** ✗ —— 函数定义写 `func 名字(参数): 返回类型 { … }`"
                  "（返回类型可省 ⇒ 推断为 any）✓";
        } else if (cur_ident_is(p, "def")) {
            sub = "Leno **没有 `def`** ✗（那是 Python 的写法）—— 函数定义写 "
                  "`func 名字(参数): 返回类型 { … }` ✓";
        } else if (cur_ident_is(p, "elif") || cur_ident_is(p, "elseif")) {
            sub = "Leno **没有 `elif` / `elseif`** ✗ —— 用 `eif`："
                  "`if c1 { … } eif c2 { … } else { … }`（`else if` 两个词的写法也合法 ✓）";
        } else if (cur_ident_is(p, "del")) {
            // ★ 2026-10-24：`del d["k"]` 原来报「声明语句后期望换行或 ';'，但同一行出现符号」，
            //   完全看不出"Leno 没有 del 关键字"（第四轮报告 Q4）。
            sub = "Leno **没有 `del`** ✗（那是 Python 的）—— 删除字典键用 `d.remove(key)`，"
                  "删除数组元素用 `arr.remove(index)`（键名/索引不用加引号也行）";
        } else if (cur_ident_is(p, "static")) {
            // ★ 2026-10-24：`static func` 原来报 3 条级联语法错误（期望表达式 / 期望 '}' …），
            //   完全没提"Leno 不支持 static"。struct 方法本来就不占实例内存、
            //   模块级函数天然全局 ⇒ 根本没有 static 这个概念。
            sub = "Leno **没有 `static`** ✗ —— struct 方法本来就不占实例内存、"
                  "模块级函数天然是全局的，直接写 `func 名字(参数): 返回类型 { … }` 即可；"
                  "常量写在模块顶层用 `const X = 1` ✓";
        } else if (cur_ident_is(p, "class")) {
            // ★ 2026-11-07：跨语言高频笔误。此前落到表达式解析，最后报的是
            //   「声明语句后期望换行或 ';'」，完全看不出"Leno 没有 class" ✗
            //   ⚠ 仅当后面不是赋值/调用形态时才提示（同名变量 `class = 1` 必须放行 ✓）
            if (ident_looks_like_keyword(p)) {
                sub = "Leno **没有 `class`** ✗ —— 值类型写 `struct 名字 { 字段 / 方法 }`，"
                      "接口写 `face 名字 { func 方法签名 }` ✓";
            }
        } else if (cur_ident_is(p, "interface")) {
            if (ident_looks_like_keyword(p)) {
                sub = "Leno **没有 `interface`** ✗ —— 接口用 `face 名字 { func 方法() }` 声明，"
                      "struct 用 `impl` 实现（`struct S impl F { … }`）✓";
            }
        } else if (cur_ident_is(p, "fn")) {
            if (ident_looks_like_keyword(p)) {
                sub = "Leno **没有 `fn`** ✗ —— 函数定义写 `func 名字(参数): 返回类型 { … }` ✓";
            }
        } else if (cur_ident_is(p, "pub")) {
            if (ident_looks_like_keyword(p)) {
                sub = "Leno **没有 `pub`** ✗ —— 模块级公开用 `export`，struct 内仅本类型可见用 `pri` ✓";
            }
        } else if (cur_ident_is(p, "match")) {
            if (ident_looks_like_keyword(p)) {
                sub = "Leno **没有 `match`** ✗ —— 分支匹配用 `switch 表达式 { case 值 { … } default { … } }` ✓";
            }
        }
        if (sub) {
            int bad_line = p->lex.current.line;
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "%s", sub);
            // 跳到本行末尾（但**见了 `{` 就停**）：这些词后面多半还跟着一截旧语法 ⇒
            //   不跳会再刷一串级联错误 ✗。
            //   ⚠ 必须保留那个 `{` 并整块跳掉 —— 否则 `function f() {` 的 `{` 被吃掉后，
            //     函数体会退化成**顶层语句**、末尾那个 `}` 再报一条「期望表达式」✗
            //     （实测：不保留时 `let` + `function` 一个文件报 5 条而非 3 条 ✓）。
            while (p->lex.current.type != TOK_EOF &&
                   p->lex.current.line == bad_line &&
                   p->lex.current.type != TOK_LBRACE) {
                lexer_next(&p->lex);
            }
            if (p->lex.current.type == TOK_LBRACE) {
                Ast* skipped_body = parse_block_internal(p);
                if (skipped_body) ast_free(skipped_body);
            }
            return NULL;
        }
    }

    // ★ 2026-11-07：`x := 1`（Go / 部分脚本语言习惯）此前落到表达式解析 ⇒
    //   只报「期望表达式」（列号指向 '='），看不出"Leno 没有 :=" ✗。
    //   只在**语句开头**的 `标识符 :=` 命中（`{"k": v}` 等表达式上下文不受影响 ✓）。
    if (p->lex.current.type == TOK_IDENT) {
        Lexer saved = p->lex;
        lexer_next(&p->lex);
        int is_walrus = 0;
        if (p->lex.current.type == TOK_COLON) {
            lexer_next(&p->lex);
            is_walrus = (p->lex.current.type == TOK_EQ);
        }
        p->lex = saved;
        error_set_column(saved.current.column);
        if (is_walrus) {
            int bad_line = p->lex.current.line;
            error_add_at_fmt(ERR_SYNTAX, bad_line, p->lex.current.column,
                "Leno **没有 `:=`** ✗ —— 声明写 `类型 名 = 值` 或 `var 名 = 值`，"
                "已有变量赋值直接写 `名 = 值` ✓");
            while (p->lex.current.type != TOK_EOF &&
                   p->lex.current.line == bad_line &&
                   p->lex.current.type != TOK_LBRACE) {
                lexer_next(&p->lex);
            }
            if (p->lex.current.type == TOK_LBRACE) {
                Ast* skipped_body = parse_block_internal(p);
                if (skipped_body) ast_free(skipped_body);
            }
            return NULL;
        }
    }

    switch (p->lex.current.type) {
        // 词法错误：跳过错误 token 并返回 NULL，让 parser 继续解析
        case TOK_ERROR:
            lexer_next(&p->lex);  // 跳过错误 token
            return NULL;

        // 控制流语句
        case TOK_IF:
            stmt = parse_if_stmt(p);
            break;
        case TOK_WHILE:
            stmt = parse_while_stmt(p);
            break;
        case TOK_FOR:
            stmt = parse_for_stmt(p);
            break;
        case TOK_SWITCH:
            stmt = parse_switch_stmt(p);
            break;

        // else / eif 掉在 if 之外：★ 2026-11-08
        //   ⚠ `if … { } eif … { }` 的 eif 由 parse_if_stmt **内部**消费，走不到这里
        //   ⇒ 到了这里就是**孤立的** else / eif（此前 else 只报「期望表达式」，
        //     eif 更糟：会被当成新的 if 静默执行 ✗）
        case TOK_ELSE:
        case TOK_EIF: {
            int is_eif = (p->lex.current.type == TOK_EIF);
            int bad_line = p->lex.current.line;
            if (is_eif) {
                error_add_at_fmt(ERR_SYNTAX, bad_line, p->lex.current.column,
                    "`eif` 只能跟在 `if … { … }` 后面（当前没有可衔接的 if）—— "
                    "写法：`if 条件 { … } eif 条件2 { … } else { … }`");
            } else {
                error_add_at_fmt(ERR_SYNTAX, bad_line, p->lex.current.column,
                    "`else` 只能跟在 `if … { … }` 后面（当前没有可衔接的 if）—— "
                    "写法：`if 条件 { … } else { … }`；中间条件用 `eif` 接");
            }
            while (p->lex.current.type != TOK_EOF &&
                   p->lex.current.line == bad_line &&
                   p->lex.current.type != TOK_LBRACE) {
                lexer_next(&p->lex);
            }
            if (p->lex.current.type == TOK_LBRACE) {
                Ast* skipped_body = parse_block_internal(p);
                if (skipped_body) ast_free(skipped_body);
            }
            return NULL;
        }

        // impl 只能写在 struct 头部（`struct S impl F`）：★ 2026-11-08
        //   Rust / Swift 风格的顶层 `impl F for S { … }` 此前落到表达式解析 ⇒
        //   只报「期望表达式」，看不出正确写法 ✗ ⇒ 定向报错并整块跳过。
        case TOK_IMPL: {
            int bad_line = p->lex.current.line;
            error_add_at_fmt(ERR_SYNTAX, bad_line, p->lex.current.column,
                "Leno 的 impl 写在 **struct 头部** —— 写成 `struct S impl F { … }`；"
                "没有顶层 `impl F for S { … }` 这种写法 ✗");
            while (p->lex.current.type != TOK_EOF &&
                   p->lex.current.line == bad_line &&
                   p->lex.current.type != TOK_LBRACE) {
                lexer_next(&p->lex);
            }
            if (p->lex.current.type == TOK_LBRACE) {
                Ast* skipped_body = parse_block_internal(p);
                if (skipped_body) ast_free(skipped_body);
            }
            return NULL;
        }

        // catch / finally 落在 try 之外：★ 2026-11-08
        //   此前掉进 default ⇒ 报「期望表达式」（catch 还会连报两次），看不出
        //   "这两个子句只能跟在 try 后面" ✗。定向报错并整块跳过，避免级联。
        case TOK_CATCH:
        case TOK_FINALLY: {
            int is_catch = (p->lex.current.type == TOK_CATCH);
            int bad_line = p->lex.current.line;
            error_add_at_fmt(ERR_SYNTAX, bad_line, p->lex.current.column,
                "`%s` 只能跟在 try 语句后面 —— 当前不在任何 try 中 ✗（写法：try { … } catch e { … } finally { … }）",
                is_catch ? "catch" : "finally");
            while (p->lex.current.type != TOK_EOF &&
                   p->lex.current.line == bad_line &&
                   p->lex.current.type != TOK_LBRACE) {
                lexer_next(&p->lex);
            }
            if (p->lex.current.type == TOK_LBRACE) {
                Ast* skipped_body = parse_block_internal(p);
                if (skipped_body) ast_free(skipped_body);
            }
            return NULL;
        }

        // case / default 落在 switch 之外：★ 2026-11-07
        //   此前掉进 default ⇒ parse_expression_stmt ⇒ 报「期望表达式」（列号还指向 case 后面），
        //   完全看不出"这两个词只能写在 switch 里" ✗。这里定向报错并整块跳过，避免级联。
        case TOK_CASE:
        case TOK_DEFAULT: {
            int is_case = (p->lex.current.type == TOK_CASE);
            int bad_line = p->lex.current.line;
            error_add_at_fmt(ERR_SYNTAX, bad_line, p->lex.current.column,
                "`%s` 只能写在 switch 语句内部 —— 当前不在任何 switch 中 ✗",
                is_case ? "case" : "default");
            // 跳到本行末尾（见了 `{` 就停），再整块跳过 case 体，避免再刷一串级联错
            while (p->lex.current.type != TOK_EOF &&
                   p->lex.current.line == bad_line &&
                   p->lex.current.type != TOK_LBRACE) {
                lexer_next(&p->lex);
            }
            if (p->lex.current.type == TOK_LBRACE) {
                Ast* skipped_body = parse_block_internal(p);
                if (skipped_body) ast_free(skipped_body);
            }
            return NULL;
        }

        // 函数定义 或 函数类型变量声明（func():void v = ...）
        case TOK_FUNC: {
            // 预读：func 后面跟 ( 说明是函数类型（如 func(int):int），走变量声明
            // func 后面跟 IDENT 说明是函数定义（如 func foo()）
            Lexer saved = p->lex;
            lexer_next(&p->lex);  // 跳过 func
            int is_func_type = (p->lex.current.type == TOK_LPAREN);
            p->lex = saved;  // 恢复
            error_set_column(saved.current.column);  // 恢复列号

            if (is_func_type) {
                stmt = parse_var_decl_internal(p);
            } else {
                stmt = parse_func_stmt(p);
            }
            break;
        }
        
        // async 函数定义
        case TOK_ASYNC:
            stmt = parse_func_stmt(p);
            break;

        // 跳转语句
        case TOK_RETURN:
            stmt = parse_return_stmt(p);
            break;
        case TOK_BREAK:
            stmt = parse_break_stmt(p);
            break;
        case TOK_CONTINUE:
            stmt = parse_continue_stmt(p);
            break;

        // 异常处理
        case TOK_TRY:
            stmt = parse_try_stmt(p);
            break;
        case TOK_THROW:
            stmt = parse_throw_stmt(p);
            break;

        // 模块系统
        case TOK_IMPORT:
            stmt = parse_import_stmt(p);
            break;
        case TOK_EXPORT:
            stmt = parse_export_stmt(p);
            break;
        case TOK_USE:
            stmt = parse_use_stmt(p);
            break;

        // pri 只能修饰 struct 成员：出现在模块级/语句位 ⇒ 直接点名病因
        //   ★ 2026-09-30 加：以前 `TOK_PRI` 不在本 switch 里 ⇒ 掉进 default ⇒ parse_expression_stmt
        //     ⇒ 报"期望表达式"（指不到病因 ✗；实测在文件顶层写 `pri func f() {}` 就是这么红的 ✓）
        //   ★ 顺带说明为什么不需要它：模块级**不加 export 就是文件私有**（Leno 的可见性规则 ✓）
        //     ⇒ 顶层根本用不到 pri；pri 只用于 struct 内"仅本 struct 可见"的字段/方法/关联常量 ✓
        case TOK_PRI:
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                "pri 只能用于 struct 成员；模块级不加 'export' 就是文件私有，请去掉 'pri'");
            lexer_next(&p->lex);   // 跳过 pri，让后面的 func/var 照常解析（尽量不连带报错 ✓）
            return NULL;

        // 变量声明（var/const 关键字或类型关键字开头，或自定义 struct 类型）
        case TOK_VAR:
        case TOK_CONST:
        case TOK_INT_TYPE:
        case TOK_FLOAT_TYPE:
        case TOK_STRING_TYPE:
        case TOK_BOOL_TYPE:
        case TOK_ARRAY_TYPE:
        case TOK_DICT_TYPE:
        // case TOK_BINT:  // 已移除：对外统一用 int
        case TOK_FILE_TYPE:
        case TOK_SOCKET_TYPE:
        case TOK_CHANNEL_TYPE:
        case TOK_THREAD_TYPE:
        case TOK_FUTURE_TYPE:
        case TOK_PTR_TYPE:
        case TOK_ANY_TYPE:  // any 类型变量声明（语义分析阶段会限制）
        // C 布局类型（i32/u8 等）：允许解析为变量声明，语义分析阶段会限制使用范围
        case TOK_I8:  case TOK_U8:
        case TOK_I16: case TOK_U16:
        case TOK_I32: case TOK_U32:
        case TOK_I64: case TOK_U64:
        case TOK_F32: case TOK_F64:
        case TOK_C_INT: case TOK_C_UINT:
        case TOK_C_LONG: case TOK_C_ULONG:
        case TOK_C_LONGLONG: case TOK_C_ULONGLONG:
        case TOK_C_SIZE: case TOK_C_SSIZE:
        case TOK_STR8: case TOK_STR16:
            stmt = parse_var_decl_internal(p);
            break;

        // struct 定义
        case TOK_STRUCT:
            stmt = parse_struct_stmt(p);
            break;

        // extend 声明：把某个 struct 的方法写到别处（不产生子类型，见设计文档）
        case TOK_EXTEND:
            stmt = parse_extend_stmt(p);
            break;

        // face 定义
        case TOK_FACE:
            stmt = parse_face_stmt(p);
            break;

        // cstruct 定义（C 布局结构体）
        // packed cstruct / align(N) cstruct 也路由到这里
        // 注意：packed 和 align 是上下文关键字，在 default 分支中通过文本匹配识别
        case TOK_CSTRUCT:
        case TOK_UNION:      // union 与 cstruct 共用同一套解析/布局，只是偏移算法不同
            stmt = parse_cstruct_stmt(p);
            break;

        // clib 定义（C 库函数签名声明）
        case TOK_CLIB:
            stmt = parse_clib_stmt(p);
            break;
        // cfunc 声明（C 回调函数签名）
        case TOK_CFUNC:
            stmt = parse_cfunc_stmt(p);
            break;

        // enum 定义
        case TOK_ENUM:
            stmt = parse_enum_stmt(p);
            break;

        // 类型别名
        case TOK_ALIAS:
            stmt = parse_alias_stmt(p);
            break;

        // 代码块
        case TOK_LBRACE:
            stmt = parse_block_internal(p);
            // ★ 2026-11-05：独立的块语句 `{ … }` 才是**词法块**（语义阶段据此建作用域）。
            //   其他 AST_BLOCK 是语法糖容器（use 批量 / 多变量声明 / 函数体），不能标记，
            //   否则容器里的声明会被关进临时作用域、出块即失效 ✗
            if (stmt) stmt->lexical_scope = 1;
            break;

        // 默认情况：检查是否是入口函数定义、自定义类型变量声明或表达式语句
        default:
            // 检查是否是未支持的关键字（this）
            if (p->lex.current.type == TOK_IDENT) {
                int ident_len = p->lex.current.len;
                const char* ident_text = p->lex.current.text;
                if (ident_len == 4 && strncmp(ident_text, "this", 4) == 0) {
                    error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                        "不支持的关键字 'this'，请使用 'self' 引用当前对象");
                    lexer_next(&p->lex);  // 跳过 this
                    return NULL;
                }
            }
            // 检查是否是 packed/align cstruct 定义
            // packed 和 align 是上下文关键字，仅在 cstruct 前缀位置才特殊
            if (is_cstruct_layout_attr(p)) {
                stmt = parse_cstruct_stmt(p);
                break;
            }
            // 检查是否是入口函数定义（如 main() { }）
            if (is_entry_function_def(p)) {
                stmt = parse_entry_func_stmt(p);
            }
            // 检查是否是自定义类型变量声明（如 Point b = ... 或 Inner2? a = ...）
            // 当前 token 是标识符，且下一个 token 也是标识符（或 ? 后跟标识符）
            else if (p->lex.current.type == TOK_IDENT) {
                // 预读检查下一个 token
                Lexer saved = p->lex;
                lexer_next(&p->lex);
                int is_type_decl = 0;
                if (p->lex.current.type == TOK_IDENT) {
                    // Type varName = ...
                    is_type_decl = 1;
                } else if (p->lex.current.type == TOK_QUESTION) {
                    // Type? varName = ... （可空类型声明）
                    lexer_next(&p->lex);
                    is_type_decl = (p->lex.current.type == TOK_IDENT);
                } else if (p->lex.current.type == TOK_LBRACKET) {
                    // Type[T] varName = ... （泛型类型声明，如 Array[int] arr）
                    // 需要找到匹配的 ] 后检查是否跟标识符
                    int bracket_depth = 1;
                    lexer_next(&p->lex);
                    while (p->lex.current.type != TOK_EOF && bracket_depth > 0) {
                        if (p->lex.current.type == TOK_LBRACKET) bracket_depth++;
                        else if (p->lex.current.type == TOK_RBRACKET) bracket_depth--;
                        if (bracket_depth > 0) lexer_next(&p->lex);
                    }
                    if (bracket_depth == 0) {
                        lexer_next(&p->lex);
                        if (p->lex.current.type == TOK_IDENT) {
                            is_type_decl = 1;
                        } else if (p->lex.current.type == TOK_QUESTION) {
                            // Type[T]? varName = ...
                            lexer_next(&p->lex);
                            is_type_decl = (p->lex.current.type == TOK_IDENT);
                        }
                    }
                } else if (p->lex.current.type == TOK_DOT) {
                    // Module.Type varName = ... —— 带模块前缀的类型声明
                    // ★ US1（2026-10-12）：**现在支持** ✓（此前报"不支持带模块前缀的类型声明"）。
                    //   这段前瞻的职责从"报错"变成"**区分声明与表达式**"：
                    //   类型名后面跟标识符 ⇒ 声明（`m.Pt p = …`）✓；跟别的东西 ⇒ 表达式（`m.func()`）✓
                    lexer_next(&p->lex);  // 消费 '.'
                    if (p->lex.current.type == TOK_IDENT) {
                        char* mod_name = copy_string(saved.current.text, saved.current.len);
                        char* type_name = copy_string(p->lex.current.text, p->lex.current.len);
                        lexer_next(&p->lex);  // 消费类型名
                        // ★ US1：**多级**限定（`m.Pt.Sub`）也按"类型位置"处理 —— 这样它会走进
                        //   parse_type_internal 的兜底拒绝，报出**正确**的那句（"类型位置最多只能写
                        //   模块.类型 一级"✓）。不吞进来的话，整条语句会被当成**表达式**去解析 ⇒
                        //   实测报「未定义的变量: x」✗ 完全指不到真因（用户得自己猜半天）。
                        //   ⚠ 顺带把 `m.obj.field = 5` 这类**表达式**也吞一段，但下面 qualified_decl
                        //   判不出来（`=` 不是标识符）⇒ 照旧走表达式路径 ✓（`saved` 会恢复 lexer ✓）
                        while (p->lex.current.type == TOK_DOT) {
                            Lexer cds = p->lex;
                            lexer_next(&p->lex);
                            if (p->lex.current.type != TOK_IDENT) { p->lex = cds; break; }
                            lexer_next(&p->lex);
                        }
                        int qualified_decl = 0;
                        if (p->lex.current.type == TOK_IDENT) {
                            // Module.Type varName = ...
                            qualified_decl = 1;
                        } else if (p->lex.current.type == TOK_QUESTION) {
                            // Module.Type? varName = ...
                            Lexer qsaved = p->lex;
                            lexer_next(&p->lex);
                            qualified_decl = (p->lex.current.type == TOK_IDENT);
                            p->lex = qsaved;
                        } else if (p->lex.current.type == TOK_LBRACKET) {
                            // Module.Type[T] varName = ...
                            int bracket_depth = 1;
                            lexer_next(&p->lex);
                            while (p->lex.current.type != TOK_EOF && bracket_depth > 0) {
                                if (p->lex.current.type == TOK_LBRACKET) bracket_depth++;
                                else if (p->lex.current.type == TOK_RBRACKET) bracket_depth--;
                                if (bracket_depth > 0) lexer_next(&p->lex);
                            }
                            if (bracket_depth == 0) {
                                lexer_next(&p->lex);
                                if (p->lex.current.type == TOK_IDENT) {
                                    qualified_decl = 1;
                                } else if (p->lex.current.type == TOK_QUESTION) {
                                    lexer_next(&p->lex);
                                    qualified_decl = (p->lex.current.type == TOK_IDENT);
                                }
                            }
                        }
                        if (qualified_decl) {
                            // ★ US1（2026-10-12，见 docs/类型可见性与use逃生阀修复追踪.md）：
                            //   `模块.类型 变量名 = ...` **现在是合法声明** ✓ ——
                            //   交给下面的普通声明路径（`parse_var_decl_internal` ⇒ `parse_type`
                            //   里吃下 `m.Pt` 并记进 TypeInfo.module_qualifier ✓，
                            //   真正的"去模块符号表查类型"在语义层归一化时做 ✓）。
                            //   上面那段前瞻已经精确区分了"声明"与"表达式"：
                            //   `m.func()` 在类型名后面跟的是 `(` 而不是标识符 ⇒ qualified_decl=0
                            //   ⇒ 仍旧走表达式路径 ✓（不会把模块函数调用误判成声明 ✓）。
                            is_type_decl = 1;
                        }
                        free(mod_name);
                        free(type_name);
                    }
                }
                p->lex = saved; // 恢复 lexer 状态
                error_set_column(saved.current.column);  // 恢复列号

                // ★ 2026-11-08：`foo(a, b) { … }`（漏写 func 的 JS / Python 写法）
                //   此前落到表达式语句 ⇒ 报「未定义的函数: foo」+「未定义的变量: a」+
                //   「return 只能写在函数体内」等一串，完全看不出"定义函数要写 func" ✗。
                //   判据：`标识符 ( … )` 后紧跟 `{` 或 `:`（返回类型）⇒ 函数体形态 ✓
                //   （普通调用 `foo(a)` 后面不会紧跟 `{` ⇒ 不会误伤 ✓）
                if (!is_type_decl) {
                    Lexer look = p->lex;
                    lexer_next(&p->lex);   // 跳过函数名
                    int looks_like_def = 0;
                    if (p->lex.current.type == TOK_LPAREN) {
                        int depth = 1;
                        lexer_next(&p->lex);
                        while (p->lex.current.type != TOK_EOF && depth > 0) {
                            if (p->lex.current.type == TOK_LPAREN) depth++;
                            else if (p->lex.current.type == TOK_RPAREN) depth--;
                            if (depth > 0) lexer_next(&p->lex);
                        }
                        if (depth == 0) {
                            int close_line = p->lex.current.line;
                            lexer_next(&p->lex);   // 跳过 ')'
                            // ⚠ 必须**同一行**紧跟 `{` / `:` 才算函数体形态：
                            //   否则 `assert_true(...)` 换行后接一个独立代码块 `{ … }` 会被误判 ✗
                            if ((p->lex.current.type == TOK_LBRACE ||
                                 p->lex.current.type == TOK_COLON) &&
                                p->lex.current.line == close_line) {
                                looks_like_def = 1;
                            }
                        }
                    }
                    p->lex = look;   // ⚠ 无论命中与否都要恢复（否则标识符已被吃掉）
                    error_set_column(look.current.column);

                    if (looks_like_def) {
                        int bad_line = look.current.line;
                        error_add_at_fmt(ERR_SYNTAX, bad_line, look.current.column,
                            "定义函数要写 `func 名字(参数): 返回类型 { … }` —— "
                            "只有 `main()` 可以省略 func ✗");
                        // 跳到本行末尾再整块跳过函数体，避免「未定义的变量 / return 只能…」级联
                        while (p->lex.current.type != TOK_EOF &&
                               p->lex.current.line == bad_line &&
                               p->lex.current.type != TOK_LBRACE) {
                            lexer_next(&p->lex);
                        }
                        if (p->lex.current.type == TOK_LBRACE) {
                            Ast* skipped_body = parse_block_internal(p);
                            if (skipped_body) ast_free(skipped_body);
                        }
                        return NULL;
                    }
                }

                if (is_type_decl) {
                    stmt = parse_var_decl_internal(p);
                } else {
                    stmt = parse_expression_stmt(p);
                }
            } else {
                stmt = parse_expression_stmt(p);
            }
            break;
    }

    // 可选的分号（语句结束符在 Leno 中是可选的）
    match(p, TOK_SEMI);

    return stmt;
}

// ============================================================================
// 程序解析
// ============================================================================

Ast* parse_program_internal(Parser* p) {
    int line = p->lex.current.line;
    Ast* ast = ast_new(AST_BLOCK, line);
    ast_list_init(&ast->u.block);

    // 循环解析所有语句直到文件结束
    while (p->lex.current.type != TOK_EOF) {
        Ast* stmt = parse_statement(p);
        if (stmt) {
            // ★ 2026-10-23：模块顶层**不是代码块**，defer 没有"块退出"的时机
            //   （降级只发生在 parse_block_internal 里）
            //   之前会**静默忽略** —— 用户以为写了释放，实际什么都没执行 ✗
            if (stmt->kind == AST_DEFER) {
                error_add_at_fmt(ERR_SYNTAX, stmt->line, stmt->column,
                                 "defer 只能写在函数体或代码块内（模块顶层没有块退出时机）");
                ast_free(stmt);
            } else {
                ast_list_add(&ast->u.block, stmt);
            }
        }
        // 错误恢复：如果解析语句失败，同步到下一个语句开始位置
        // 这样可以收集更多错误而不是遇到第一个错误就停止
        if (!stmt && p->lex.current.type != TOK_EOF) {
            parser_synchronize(p);
            // 在顶层，消费碰到的分号和大括号（它们是分隔符，不属于任何语句）
            if (p->lex.current.type == TOK_SEMI || p->lex.current.type == TOK_RBRACE) {
                lexer_next(&p->lex);
            }
        }
    }

    return ast;
}

// ============================================================================
// 公共 API 实现
// ============================================================================

// 初始化解析器
void parser_init(Parser* p, const char* src) {
    lexer_init(&p->lex, src);
    p->root = NULL;
    p->current_scope = NULL;
    p->struct_names = NULL;
    p->struct_count = 0;
    p->struct_capacity = 0;
    p->alias_names = NULL;
    p->alias_types = NULL;
    p->alias_type_params = NULL;
    p->alias_type_param_counts = NULL;
    p->alias_count = 0;
    p->alias_capacity = 0;
}

// 释放 Parser 自己在解析过程中累积的辅助表（struct 名称 / 类型别名）。
// ⚠ 只释放 Parser 独占的堆内存；AST（p->root）由调用方 ast_free。
void parser_free(Parser* p) {
    if (!p) return;
    for (int i = 0; i < p->struct_count; i++) {
        free(p->struct_names[i]);
    }
    free(p->struct_names);
    p->struct_names = NULL;
    p->struct_count = 0;
    p->struct_capacity = 0;

    for (int i = 0; i < p->alias_count; i++) {
        free(p->alias_names[i]);
        if (p->alias_types[i]) type_free(p->alias_types[i]);
        if (p->alias_type_params[i]) {
            for (int j = 0; j < p->alias_type_param_counts[i]; j++) {
                free(p->alias_type_params[i][j]);
            }
            free(p->alias_type_params[i]);
        }
    }
    free(p->alias_names);
    free(p->alias_types);
    free(p->alias_type_params);
    free(p->alias_type_param_counts);
    p->alias_names = NULL;
    p->alias_types = NULL;
    p->alias_type_params = NULL;
    p->alias_type_param_counts = NULL;
    p->alias_count = 0;
    p->alias_capacity = 0;

    // Lexer 的大整数字符串登记池（lexer_init 分配；Token 会被回退快照复制 ⇒ 只能在这里统一释放）
    lexer_free(&p->lex);
}

// 添加 struct 名称到集合
static void add_struct_name(Parser* p, const char* name) {
    // 检查是否已存在
    for (int i = 0; i < p->struct_count; i++) {
        if (strcmp(p->struct_names[i], name) == 0) {
            return;
        }
    }

    // 扩容
    if (p->struct_count >= p->struct_capacity) {
        p->struct_capacity = p->struct_capacity == 0 ? 16 : p->struct_capacity * 2;
        p->struct_names = realloc(p->struct_names, sizeof(char*) * p->struct_capacity);
    }

    // 添加
    p->struct_names[p->struct_count++] = strdup(name);
}

// 检查名称是否是 struct 类型
int is_struct_name(Parser* p, const char* name) {
    for (int i = 0; i < p->struct_count; i++) {
        if (strcmp(p->struct_names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

// 添加别名到表
void add_alias(Parser* p, const char* name, TypeInfo* type) {
    add_alias_with_params(p, name, type, 0, NULL);
}

void add_alias_with_params(Parser* p, const char* name, TypeInfo* type, int tp_count, char** tp_names) {
    if (p->alias_count >= p->alias_capacity) {
        p->alias_capacity = p->alias_capacity == 0 ? 8 : p->alias_capacity * 2;
        p->alias_names = realloc(p->alias_names, sizeof(char*) * p->alias_capacity);
        p->alias_types = realloc(p->alias_types, sizeof(TypeInfo*) * p->alias_capacity);
        p->alias_type_params = realloc(p->alias_type_params, sizeof(char**) * p->alias_capacity);
        p->alias_type_param_counts = realloc(p->alias_type_param_counts, sizeof(int) * p->alias_capacity);
    }
    p->alias_names[p->alias_count] = strdup(name);
    p->alias_types[p->alias_count] = type ? type_copy(type) : NULL;
    if (tp_count > 0 && tp_names) {
        p->alias_type_params[p->alias_count] = (char**)malloc(sizeof(char*) * tp_count);
        for (int i = 0; i < tp_count; i++) {
            p->alias_type_params[p->alias_count][i] = strdup(tp_names[i]);
        }
        p->alias_type_param_counts[p->alias_count] = tp_count;
    } else {
        p->alias_type_params[p->alias_count] = NULL;
        p->alias_type_param_counts[p->alias_count] = 0;
    }
    p->alias_count++;
}

// 查找别名
TypeInfo* find_alias(Parser* p, const char* name) {
    for (int i = 0; i < p->alias_count; i++) {
        if (strcmp(p->alias_names[i], name) == 0) {
            return p->alias_types[i];
        }
    }
    return NULL;
}

// 查找别名并获取类型参数信息
TypeInfo* find_alias_with_params(Parser* p, const char* name, char*** out_params, int* out_param_count) {
    for (int i = 0; i < p->alias_count; i++) {
        if (strcmp(p->alias_names[i], name) == 0) {
            if (out_params) *out_params = p->alias_type_params[i];
            if (out_param_count) *out_param_count = p->alias_type_param_counts[i];
            return p->alias_types[i];
        }
    }
    return NULL;
}

// 预扫描收集所有 struct 定义
static void prescan_struct_definitions(Parser* p) {
    // 保存当前 lexer 状态
    Lexer saved = p->lex;

    // 扫描所有 token
    while (p->lex.current.type != TOK_EOF) {
        if (p->lex.current.type == TOK_STRUCT) {
            lexer_next(&p->lex); // 跳过 'struct'
            if (p->lex.current.type == TOK_IDENT) {
                add_struct_name(p, p->lex.current.text);
            }
        }
        lexer_next(&p->lex);
    }

    // 恢复 lexer 状态
    p->lex = saved;
            error_set_column(saved.current.column);  // 恢复列号
}

// 执行解析
int parser_parse(Parser* p) {
    // 记录解析前的错误数量，只关注本次解析新增的错误
    // 避免模块编译时，前序模块的语义错误导致后续模块的 parser_parse 误判为失败
    int errors_before = errors.count;

    // 预扫描收集 struct 定义，支持前向引用
    prescan_struct_definitions(p);

    p->root = parse_program_internal(p);
    return (errors.count > errors_before) ? -1 : 0;
}
