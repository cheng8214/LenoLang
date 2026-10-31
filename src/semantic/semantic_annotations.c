// semantic_annotations.c — 注解 `@optkeys` 的收集与检查（2026-10-31）
// ----------------------------------------------------------------------------
// 设计文档：docs/注解系统与optkeys设计草案.md
//
// 为什么单独一个文件：它消费的是**编译期元数据**，与类型推断 / 作用域完全无关；
//   塞进 semantic_type.c（7000+ 行）只会让两边都更难找 ✓
//
// 一期范围（有意收窄）：
//   · 只处理**同模块**的直接调用（callee 是 AST_VAR）
//   · 只查**字面量 Dict** 实参（非字面量一律放行 —— 宁漏勿误报，同 L3 的教训 ✓）
//   · 只警告、不中断编译（存量代码里可能有传错键但靠静默丢弃跑着的地方）
// 跨模块（`SDL3.createHBox`）需要把注解读进模块符号表，见设计文档第 10 节 ✓

#include "../include/lenolang.h"
#include "../include/leno_ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANNO_MAX_FUNCS 512
#define ANNO_MAX_KEYS  96

typedef struct {
    char* name;                    // 函数名（本模块内唯一）
    char* keys[ANNO_MAX_KEYS];     // 已合并（@optkeys 可叠加 ⇒ 取并集）
    int   key_count;
    int   wildcard;                // 1 = @optkeys("*") ⇒ 整个不检查
    int   decl_line;               // 首次声明的行号（"去哪看"用 ✓）
} AnnoOptKeys;

static AnnoOptKeys g_optkeys[ANNO_MAX_FUNCS];
static int g_optkeys_count = 0;

// 每次编译前调用：表是**进程级**的（一次编译可能过多个文件）⇒ 不 reset 会串味 ✗
void annos_reset_all(void) {
    for (int i = 0; i < g_optkeys_count; i++) {
        free(g_optkeys[i].name);
        for (int k = 0; k < g_optkeys[i].key_count; k++) free(g_optkeys[i].keys[k]);
    }
    memset(g_optkeys, 0, sizeof(g_optkeys));
    g_optkeys_count = 0;
}

static char* anno_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static AnnoOptKeys* anno_entry(const char* func_name) {
    for (int i = 0; i < g_optkeys_count; i++) {
        if (strcmp(g_optkeys[i].name, func_name) == 0) return &g_optkeys[i];
    }
    if (g_optkeys_count >= ANNO_MAX_FUNCS) return NULL;
    AnnoOptKeys* e = &g_optkeys[g_optkeys_count++];
    memset(e, 0, sizeof(*e));
    e->name = anno_strdup(func_name);
    return e;
}

static int anno_has_key(const AnnoOptKeys* e, const char* k) {
    for (int i = 0; i < e->key_count; i++) {
        if (strcmp(e->keys[i], k) == 0) return 1;
    }
    return 0;
}

static void anno_add_key(AnnoOptKeys* e, const char* k) {
    if (!k || !k[0] || e->key_count >= ANNO_MAX_KEYS) return;
    if (anno_has_key(e, k)) return;          // 多处声明取并集 ⇒ 重复的忽略 ✓
    e->keys[e->key_count++] = anno_strdup(k);
}

// 编辑距离（与 semantic_type_utils 里那份同口径：≤2 才给"你是不是想要"）
static int anno_lev(const char* a, const char* b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 48 || lb > 48) return 99;
    int d[49][49];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int m = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + cost < m) m = d[i - 1][j - 1] + cost;
            d[i][j] = m;
        }
    }
    return d[la][lb];
}

static const char* anno_nearest(const AnnoOptKeys* e, const char* key) {
    const char* best = NULL;
    int best_d = 3;                          // 只接受 ≤2
    for (int i = 0; i < e->key_count; i++) {
        int d = anno_lev(key, e->keys[i]);
        if (d < best_d) { best_d = d; best = e->keys[i]; if (d == 0) break; }
    }
    return best;
}

// ==================== 收集：AST_FUNC_DEF 上调用一次 ====================
void annos_collect_func(Ast* ast) {
    if (!ast || ast->kind != AST_FUNC_DEF) return;
    if (ast->anno_count <= 0 || !ast->annos) return;
    if (!ast->u.func.name || !ast->u.func.name[0]) return;

    AnnoOptKeys* e = NULL;
    for (int i = 0; i < ast->anno_count; i++) {
        AstAnno* a = &ast->annos[i];
        if (!a->name || strcmp(a->name, "optkeys") != 0) continue;

        if (!e) e = anno_entry(ast->u.func.name);
        if (!e) return;
        if (e->decl_line == 0) e->decl_line = a->line;

        // `@optkeys("*")` ⇒ 该函数接受任意键，整个不检查
        if (a->arg_kind == 1 && a->arg_str && strcmp(a->arg_str, "*") == 0) {
            e->wildcard = 1;
            continue;
        }
        if (a->arg_kind == 5) {
            for (int k = 0; k < a->arg_list_len; k++) anno_add_key(e, a->arg_list[k]);
        }
    }
}

// ==================== 检查：AST_CALL 上调用一次 ====================
static const char* anno_dict_key(Ast* k) {
    if (!k) return NULL;
    if (k->kind == AST_STRING) return k->u.string.value;
    if (k->kind == AST_VAR)    return k->u.var.name;    // `{w: 30}` 的键在建树时是标识符 ✓
    return NULL;
}

void annos_check_call(Ast* ast) {
    if (!ast || ast->kind != AST_CALL || g_optkeys_count == 0) return;

    Ast* callee = ast->u.call.callee;
    if (!callee || callee->kind != AST_VAR || !callee->u.var.name) return;

    AnnoOptKeys* e = NULL;
    for (int i = 0; i < g_optkeys_count; i++) {
        if (strcmp(g_optkeys[i].name, callee->u.var.name) == 0) { e = &g_optkeys[i]; break; }
    }
    if (!e || e->wildcard || e->key_count == 0) return;

    const char* file = error_get_filename();

    for (int i = 0; i < (int)ast->u.call.args.count; i++) {
        Ast* arg = ast->u.call.args.items[i];
        // 只查字面量 Dict：变量 / 函数返回值一律放行（静态不可知 ✓）
        if (!arg || arg->kind != AST_DICT) continue;

        for (int j = 0; j < arg->u.dict.count; j++) {
            const char* key = anno_dict_key(arg->u.dict.entries[j].key);
            if (!key || !key[0]) continue;
            if (anno_has_key(e, key)) continue;

            const char* hint = anno_nearest(e, key);
            // 一期只警告、不中断（错误通道会把编译判失败 ⇒ 存量代码升级即编不过 ✗）
            if (hint) {
                fprintf(stderr,
                        "%s:%d: [警告] %s 的 opts 不认识键 '%s'\n"
                        "        提示: 你是不是想要 '%s'？\n"
                        "        提示: 接受的键见本文件第 %d 行的 @optkeys\n",
                        file ? file : "?", arg->line, e->name, key, hint, e->decl_line);
            } else {
                fprintf(stderr,
                        "%s:%d: [警告] %s 的 opts 不认识键 '%s'\n"
                        "        提示: 接受的键见本文件第 %d 行的 @optkeys\n",
                        file ? file : "?", arg->line, e->name, key, e->decl_line);
            }
            fflush(stderr);
        }
        return;      // 一个调用只查"第一个 Dict 字面量"（见设计文档 2.3 ✓）
    }
}
