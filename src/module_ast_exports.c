/* 从 parser AST 提取模块顶层导出名 + 与符号表扫描链对拍（compiler-only）
 *
 * 见 include/module_ast_exports.h 的口径说明。
 * 本文件刻意**不**参与 sources_core.txt —— VM-only 构建里没有 parser ✓
 */
#include "include/lenolang.h"
#include "include/leno_ast.h"
#include "include/leno_parser.h"
#include "include/module_symbol_table.h"       // module_symbol_table_get_shared / _export_names
#include "include/module_loader.h"             // module_set_export_names_provider / MAX_EXPORT_NAME_LEN
#include "include/module_ast_exports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 符号表侧的源码读取器（定义在 module_symbol_table 内，非 static ⇒ 直接复用）：
//   复用它而不是自己 fopen —— 它已处理 UTF-8/中文路径与 BOM，
//   而"对拍"的前提正是**两边读同一份源码** ✓
extern char* read_module_file(const char* file_path, const char* current_file);

// ============================================================================
// 内部：可增长的名字清单（策略与 module_symbol_table_add_export_name 对齐：
//   空名/重名忽略 ⇒ 两边对"同名重复导出"的处理一致，不会造出假差异 ✓）
// ============================================================================

static void ast_exports_push(AstExportList* list, const char* name, const char* sig) {
    if (!list || !name || !name[0]) return;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->names[i], name) == 0) return;
    }
    if (list->count >= list->capacity) {
        int new_cap = list->capacity == 0 ? 16 : list->capacity * 2;
        char** gn = (char**)realloc(list->names, sizeof(char*) * new_cap);
        if (!gn) return;                 // 内存不足：跳过该名字（与扫描链同策略 ✓）
        list->names = gn;
        char** gs = (char**)realloc(list->sigs, sizeof(char*) * new_cap);
        if (!gs) return;
        list->sigs = gs;
        list->capacity = new_cap;
    }
    char* s = strdup(name);
    if (!s) return;
    list->names[list->count] = s;
    list->sigs[list->count] = sig ? strdup(sig) : NULL;
    list->count++;
}

// 声明摘要：只用**两侧都有的**计数/布尔字段（符号表侧与 AST 侧同名字段），
//   刻意不含类型名 —— 类型名映射（TypeInfo ↔ TypeKind + struct_name）是另一层工程，
//   放进来会把"真差异"淹没在映射口径差里 ✗
static void ast_exports_sig(Ast* decl, char* buf, size_t sz) {
    switch (decl->kind) {
        case AST_FUNC_DEF:
            snprintf(buf, sz, "func(pc=%d,async=%d)", decl->u.func.pcnt, decl->u.func.is_async);
            break;
        case AST_VAR_DECL:
            snprintf(buf, sz, "var(const=%d)", decl->u.var_decl.is_const);
            break;
        case AST_DESTRUCT_DECL:
            snprintf(buf, sz, "var(const=%d)", decl->u.destruct_decl.is_const);
            break;
        case AST_STRUCT_DEF:
            snprintf(buf, sz, "struct(f=%d,m=%d)", decl->u.struct_def.field_count, decl->u.struct_def.method_count);
            break;
        case AST_CSTRUCT_DEF:
            snprintf(buf, sz, "cstruct(f=%d)", decl->u.cstruct_def.field_count);
            break;
        case AST_FACE_DEF:
            snprintf(buf, sz, "face(m=%d)", decl->u.face_def.method_count);
            break;
        case AST_ENUM_DEF:
            snprintf(buf, sz, "enum(m=%d)", decl->u.enum_def.member_count);
            break;
        case AST_ALIAS:
            snprintf(buf, sz, "alias");
            break;
        case AST_CFUNC_DECL:
            snprintf(buf, sz, "cfunc(pc=%d)", decl->u.cfunc_decl.param_count);
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

// 单条声明 ⇒ 贡献哪些导出名。
//   ★ 这份 switch 必须与扫描链 12 处 module_symbol_table_add_export_name **逐字对齐**：
//     func(scan_func:318) / var(scan_var:411) / 解构每个名字(scan_var:75) /
//     struct(:1074) / face(:272) / enum(:143) / cstruct(:354) / alias(:65) / cfunc(scan_pass1:322)
static void ast_exports_collect_decl(Ast* decl, AstExportList* out) {
    if (!decl) return;
    char sig[96];
    sig[0] = '\0';
    switch (decl->kind) {
        case AST_FUNC_DEF:
            ast_exports_sig(decl, sig, sizeof(sig));
            ast_exports_push(out, decl->u.func.name, sig);
            break;
        case AST_VAR_DECL:
            ast_exports_sig(decl, sig, sizeof(sig));
            ast_exports_push(out, decl->u.var_decl.name, sig);
            break;
        case AST_DESTRUCT_DECL:
            // export var[int,int,int](a,b,c) ⇒ 三个名字各自进表（不是整体一个 ✓）
            ast_exports_sig(decl, sig, sizeof(sig));
            for (int i = 0; i < decl->u.destruct_decl.slot_count; i++) {
                ast_exports_push(out, decl->u.destruct_decl.names[i], sig);
            }
            break;
        case AST_STRUCT_DEF:
            ast_exports_sig(decl, sig, sizeof(sig));
            ast_exports_push(out, decl->u.struct_def.name, sig);
            break;
        case AST_FACE_DEF:
            ast_exports_sig(decl, sig, sizeof(sig));
            ast_exports_push(out, decl->u.face_def.name, sig);
            break;
        case AST_ENUM_DEF:
            ast_exports_sig(decl, sig, sizeof(sig));
            ast_exports_push(out, decl->u.enum_def.name, sig);
            break;
        case AST_CSTRUCT_DEF:
            ast_exports_sig(decl, sig, sizeof(sig));
            ast_exports_push(out, decl->u.cstruct_def.name, sig);
            break;
        case AST_ALIAS:
            ast_exports_push(out, decl->u.alias.name, "alias");
            break;
        case AST_CFUNC_DECL:
            ast_exports_sig(decl, sig, sizeof(sig));
            ast_exports_push(out, decl->u.cfunc_decl.name, sig);
            break;
        // ⚠ AST_CLIB_DEF 故意**不收**：扫描链 12 处 add 里没有 clib（`clib xxx { }` 不进导出表）
        //   ⇒ 这里收了就会造出"仅 AST 有"的假差异 ✗
        default:
            break;
    }
}

int module_ast_collect_exports(const char* src, AstExportList* out) {
    if (!out) return -1;
    out->names = NULL;
    out->sigs = NULL;
    out->count = 0;
    out->capacity = 0;
    if (!src) return -1;

    Parser parser;
    parser_init(&parser, src);
    if (parser_parse(&parser) < 0) {
        ast_free(parser.root);
        return -1;      // 语法错误：两边都读不出，不参与比较
    }

    Ast* root = parser.root;
    if (root && root->kind == AST_BLOCK) {
        for (int i = 0; i < root->u.block.count; i++) {
            Ast* stmt = root->u.block.items[i];
            if (!stmt) continue;
            // "顶层 export" 在 AST 上就是 AST_EXPORT 包一层（与扫描链的顶层 export 闸门等价 ✓）
            if (stmt->kind == AST_EXPORT && stmt->u.export.decl) {
                ast_exports_collect_decl(stmt->u.export.decl, out);
            }
        }
    }

    ast_free(parser.root);   // 名字已 strdup 出来 ⇒ 先收集后释放才安全 ✓
    return 0;
}

void module_ast_exports_free(AstExportList* list) {
    if (!list) return;
    if (list->names) {
        for (int i = 0; i < list->count; i++) free(list->names[i]);
        free(list->names);
    }
    if (list->sigs) {
        for (int i = 0; i < list->count; i++) free(list->sigs[i]);
        free(list->sigs);
    }
    list->names = NULL;
    list->sigs = NULL;
    list->count = 0;
    list->capacity = 0;
}

// ============================================================================
// 对拍
// ============================================================================

static int cmp_str(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

// 名字是否在数组里（名字数少 ⇒ O(n²) 足够，不做哈希）
static int name_in(const char* const* arr, int n, const char* name) {
    for (int i = 0; i < n; i++) {
        if (strcmp(arr[i], name) == 0) return 1;
    }
    return 0;
}

static void print_names(const char* tag, char** names, int n) {
    printf("  %-22s %d:", tag, n);
    for (int i = 0; i < n; i++) printf(" %s", names[i]);
    printf("\n");
}

// 符号表侧的"声明摘要"：格式与 ast_exports_sig **逐字一致**，只用两侧都有的字段 ✓
static void scan_side_sig(ModuleSymbolTable* t, const char* name, char* buf, size_t sz) {
    buf[0] = '\0';
    if (!t) { snprintf(buf, sz, "?"); return; }
    ModuleFuncSymbol* f = module_symbol_table_find_func(t, name);
    if (f) {
        snprintf(buf, sz, "func(pc=%d,async=%d)", f->param_count, f->is_async);
        return;
    }
    ModuleStructSymbol* s = module_symbol_table_find_struct(t, name);
    if (s) {
        if (s->is_cstruct) snprintf(buf, sz, "cstruct(f=%d)", s->field_count);
        else snprintf(buf, sz, "struct(f=%d,m=%d)", s->field_count, s->method_count);
        return;
    }
    ModuleEnumSymbol* e = module_symbol_table_find_enum(t, name);
    if (e) {
        snprintf(buf, sz, "enum(m=%d)", e->member_count);
        return;
    }
    ModuleFaceSymbol* fa = module_symbol_table_find_face(t, name);
    if (fa) {
        snprintf(buf, sz, "face(m=%d)", fa->method_count);
        return;
    }
    // ⚠ cfunc 必须**排在 var 之前**查：`export cfunc X(...)` 的 X 在符号表里同时进了 cfuncs[]
    //   与 vars[]（后者是扫描链的既有行为），先查 var 会把 cfunc 误报成 var ⇒ 假差异 ✗
    ModuleCfuncSymbol* cf0 = module_symbol_table_find_cfunc(t, name);
    if (cf0) {
        snprintf(buf, sz, "cfunc(pc=%d)", cf0->param_count);
        return;
    }
    ModuleVarSymbol* v = module_symbol_table_find_var(t, name);
    if (v) {
        snprintf(buf, sz, "var(const=%d)", v->is_const);
        return;
    }
    if (module_symbol_table_find_alias(t, name)) {
        snprintf(buf, sz, "alias");
        return;
    }
    ModuleCfuncSymbol* cf = module_symbol_table_find_cfunc(t, name);
    if (cf) {
        snprintf(buf, sz, "cfunc(pc=%d)", cf->param_count);
        return;
    }
    snprintf(buf, sz, "?");
}

int module_ast_export_diff_file(const char* path, int verbose) {
    if (!path) return -1;

    // ① 源码（与扫描链读的是同一份 —— 复用同一个读取器 ✓）
    //   ⚠ current_file 必须传 NULL：传 path 自己会让 module_resolve_path 以**自身目录为基准再拼一遍**
    //     （`examples\clib\` + `examples\clib\test.leno` ⇒ 拼成不存在的路径 ✗）；
    //     NULL ⇒ 直接用 file_path 并按 cwd 规范化 ✓
    char* src = read_module_file(path, NULL);
    if (!src) {
        printf("export-diff: %s\n  !! 读不了源码（跳过）\n", path);
        return 2;      // 2 = 无法比较（≠ 不一致）：批量脚本据此把它排除在统计外 ✓
    }

    // ② 扫描链侧（当前唯一来源）—— 同样 current_file=NULL ✓
    ModuleSymbolTable* table = module_symbol_table_get_shared(path, NULL);
    int scan_count = 0;
    const char* const* scan_names = table ? module_symbol_table_export_names(table, &scan_count) : NULL;
    if (!scan_names) scan_count = 0;

    // ③ AST 侧（迁移目标）
    AstExportList ast_list;
    int ast_ok = module_ast_collect_exports(src, &ast_list);
    free(src);
    if (ast_ok != 0) {
        // 解析失败 = 两边都读不出 ⇒ 不参与比较（assert/error_col 下那些"故意的语法错误"用例都走这里 ✓）
        printf("export-diff: %s\n  !! AST 解析失败（语法错误 ⇒ 不参与比较）\n", path);
        module_ast_exports_free(&ast_list);
        return 2;
    }

    // ④ 比较：**用副本排序** ⇒ 顺序不算差异（两份产出的遍历顺序本就可能不同 ✓）
    //   ⚠ 绝不能原地排 `ast_list.names`：那会打断 `names[i] ↔ sigs[i]` 的下标对齐，
    //     之后摘要比对会整体错位（症状是"名字互换"式假差异，如 makeColor 拿到 makeSize 的参数数）
    char** scan_sorted = (char**)malloc(sizeof(char*) * (scan_count > 0 ? (size_t)scan_count : 1));
    char** ast_sorted = (char**)malloc(sizeof(char*) * (ast_list.count > 0 ? (size_t)ast_list.count : 1));
    if (!scan_sorted || !ast_sorted) {
        free(scan_sorted);
        free(ast_sorted);
        module_ast_exports_free(&ast_list);
        return -1;
    }
    for (int i = 0; i < scan_count; i++) scan_sorted[i] = (char*)scan_names[i];
    for (int i = 0; i < ast_list.count; i++) ast_sorted[i] = ast_list.names[i];
    qsort(scan_sorted, (size_t)scan_count, sizeof(char*), cmp_str);
    qsort(ast_sorted, (size_t)ast_list.count, sizeof(char*), cmp_str);

    int only_scan = 0;
    int only_ast = 0;
    for (int i = 0; i < scan_count; i++) {
        if (!name_in((const char* const*)ast_sorted, ast_list.count, scan_sorted[i])) {
            if (only_scan == 0) printf("export-diff: %s\n", path);
            printf("  [仅扫描链有] %s\n", scan_sorted[i]);
            only_scan++;
        }
    }
    for (int i = 0; i < ast_list.count; i++) {
        if (!name_in((const char* const*)scan_sorted, scan_count, ast_sorted[i])) {
            if (only_scan == 0 && only_ast == 0) printf("export-diff: %s\n", path);
            printf("  [仅 AST 有]   %s\n", ast_sorted[i]);
            only_ast++;
        }
    }

    // 签名级比较：只比**名字集合的交集**（名字差异上面已报过，避免重复刷屏 ✓）
    int sig_bad = 0;
    for (int i = 0; i < ast_list.count; i++) {
        int found = 0;
        for (int k = 0; k < scan_count && !found; k++) {
            if (strcmp(scan_sorted[k], ast_list.names[i]) == 0) found = 1;
        }
        if (!found) continue;
        char ss[96];
        scan_side_sig(table, ast_list.names[i], ss, sizeof(ss));
        const char* as = ast_list.sigs[i] ? ast_list.sigs[i] : "";
        if (strcmp(ss, as) != 0) {
            if (only_scan == 0 && only_ast == 0 && sig_bad == 0) printf("export-diff: %s\n", path);
            printf("  [摘要不一致] %s：扫描链 %s / AST %s\n", ast_list.names[i], ss, as);
            sig_bad++;
        }
    }

    int same = (only_scan == 0 && only_ast == 0 && sig_bad == 0);
    if (!same) {
        print_names("扫描链(符号表):", scan_sorted, scan_count);
        print_names("AST(parser)   :", ast_list.names, ast_list.count);
    } else if (verbose) {
        printf("export-diff: %s\n", path);
        print_names("一致（含摘要）:", ast_list.names, ast_list.count);
    }

    free(scan_sorted);
    free(ast_sorted);
    module_ast_exports_free(&ast_list);
    return same ? 0 : 1;
}

// ============================================================================
// 基于 parser AST 的导出名提供者（编译期由 main.c 注册）
// ============================================================================

// 缓存：路径 → 名字清单。
//   为什么必须有：`module_has_method` 在语义分析里会被**反复**调用，每次 parse 一遍模块
//   （几十 ms/次）⇒ 秒级开销，不可接受 ✗（扫描链侧本来就有等价的进程内记忆化 ✓）
//   键用 file_path 原样：调用点传的形式不同（相对/绝对）只会多缓存一份，不影响正确性 ✓
#define AST_EXPORT_CACHE_MAX 512
typedef struct {
    char* key;
    char** names;
    int count;
} AstExportCacheEntry;

static AstExportCacheEntry g_ast_export_cache[AST_EXPORT_CACHE_MAX];
static int g_ast_export_cache_count = 0;

static const AstExportCacheEntry* ast_export_cache_lookup(const char* key) {
    for (int i = 0; i < g_ast_export_cache_count; i++) {
        if (strcmp(g_ast_export_cache[i].key, key) == 0) return &g_ast_export_cache[i];
    }
    return NULL;
}

static void ast_export_cache_store(const char* key, char** names, int count) {
    if (g_ast_export_cache_count >= AST_EXPORT_CACHE_MAX) return;   // 满了不再缓存（正确性不受影响 ✓）
    AstExportCacheEntry* e = &g_ast_export_cache[g_ast_export_cache_count];
    e->key = strdup(key);
    if (!e->key) return;
    e->names = (char**)malloc(sizeof(char*) * (count > 0 ? (size_t)count : 1));
    if (!e->names) {
        free(e->key);
        return;
    }
    for (int i = 0; i < count; i++) e->names[i] = strdup(names[i]);
    e->count = count;
    g_ast_export_cache_count++;
}

// 提供者：签名与 module_loader 的 copy_module_export_names 同形（固定二维缓冲 ✓）
//   返回 ≥0 = 名字数（可能是 0）；<0 = 本路径读不了/语法错 ⇒ 调用方**回退扫描链** ✓
static int ast_export_names_provider(const char* file_path, const char* current_file,
                                     char (*out)[MAX_EXPORT_NAME_LEN], int max_names) {
    if (!file_path || !out || max_names <= 0) return -1;

    const AstExportCacheEntry* hit = ast_export_cache_lookup(file_path);
    if (hit) {
        int n = hit->count < max_names ? hit->count : max_names;
        for (int i = 0; i < n; i++) {
            strncpy(out[i], hit->names[i], MAX_EXPORT_NAME_LEN - 1);
            out[i][MAX_EXPORT_NAME_LEN - 1] = '\0';
        }
        return n;
    }

    char* src = read_module_file(file_path, current_file);
    if (!src) return -1;      // 读不了 ⇒ 回退（由回退路径按原样报错 ✓）
    AstExportList list;
    int ok = module_ast_collect_exports(src, &list);
    free(src);
    if (ok != 0) {
        module_ast_exports_free(&list);
        return -1;            // 语法错 ⇒ 回退（编译本来就会在 parse 阶段报错 ✓）
    }

    // 诊断（可选）：`LENO_DEBUG_EXPORTS=1` 时打印"这条导出名是 AST 给的"。
    //   为什么要它：provider 与扫描链**刻意语义等价**（对拍 0 差异）⇒ 光看程序输出
    //   分不出走的是哪条路 ✗；这个开关让"迁移真的生效"这件事可被直接观测 ✓
    {
        static int ast_dbg = -1;
        if (ast_dbg < 0) {
            const char* e = getenv("LENO_DEBUG_EXPORTS");
            ast_dbg = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (ast_dbg) {
            fprintf(stderr, "[export-names] 走 parser AST: %s (%d 个导出名)\n", file_path, list.count);
        }
    }

    ast_export_cache_store(file_path, list.names, list.count);
    int n = list.count < max_names ? list.count : max_names;
    for (int i = 0; i < n; i++) {
        strncpy(out[i], list.names[i], MAX_EXPORT_NAME_LEN - 1);
        out[i][MAX_EXPORT_NAME_LEN - 1] = '\0';
    }
    module_ast_exports_free(&list);
    return n;
}

void module_ast_exports_register(void) {
    const char* e = getenv("LENO_DEBUG_EXPORTS");
    if (e && e[0] && e[0] != '0') {
        fprintf(stderr, "[export-names] 已注册 AST 提供者\n");
    }
    module_set_export_names_provider(ast_export_names_provider);
}

// ============================================================================
// S10 迁移：AST 符号填充器 —— 逐类把扫描链的活接过来
// ============================================================================
// 为什么用"原地覆盖"而不是"重新建条目"：
//   扫描链的建表入口（add_func）是**追加**语义，重加会造出重复条目 ✗；
//   而 find_func 返回的是可写结构体指针 ⇒ 直接覆盖字段即可，零重复风险 ✓
// 本轮接管：**func**（顶层 `func`，含 `export` 包裹与本地非导出两种）。
// 覆盖字段只取 AST 侧**现成且更准**的：返回类型用 parser 解析好的 TypeInfo
//   （扫描链走的是自己那套"文本→类型"，那是第三份类型解析）；参数个数/默认值个数/
//   泛型参数个数/async 同样直接来自 AST ✓
// 仍留给扫描链的：param_text / param_default_texts（AST 里是表达式而非文本）⇒
//   等这些消费者也迁走后再一起换掉 ✓

// 前向声明：聚合类型修正（定义见下；func 的返回类型也要用它 ✓）
static void ast_fix_agg_kind(ModuleSymbolTable* table, TypeInfo* t);
// 前向声明：当前填充阶段（定义见文件末；struct 的字段/方法能否整体覆盖取决于它 ✓）
int ast_fill_phase(void);

// 别名展开：把 `export alias Color = int` 这类名字解析到**最底层**的 kind ✓
//   为什么必须有它（2026-10-01 实测）：parser 不认识别名（那是模块符号表的知识）⇒
//   对 `setColor(Color color)` 里的 `Color` 一律给 TYPE_STRUCT + struct_name="Color" ✗；
//   而扫描链是靠"use 传导别名进表 + 本地别名表"把 `Color` 解析成 int 的
//   （sym_table_import_alias.inc:139 / scan_pass2_init.inc:482 / scan_alias.inc:56）✓
//   少了它：跨模块调用报「setColor 第 1 个参数类型不匹配: 期望 struct Color, 实际 int」
//   —— plane_war 一次 23 个此类错误 ✓
// 非聚合 kind ⇒ 原样返回（只对"名字型"kind 才需要查别名）；depth 防别名环 ✓
static TypeKind ast_alias_unwrap(ModuleSymbolTable* table, TypeKind kind, const char* name, int depth) {
    if (!table || !name || depth > 8) return kind;
    if (kind != TYPE_STRUCT && kind != TYPE_FACE && kind != TYPE_CSTRUCT && kind != TYPE_CLIB) return kind;
    ModuleAliasSymbol* a = module_symbol_table_find_alias(table, name);
    if (!a || !a->type_info) return kind;
    return ast_alias_unwrap(table, a->type_info->kind, a->type_info->struct_name, depth + 1);
}

// 名字型类型的**完整解析副本**：别名**整体展开**（而不是只换 kind）+ 聚合 kind 修正 ✓
//   为什么不能只换 kind（2026-10-01 实测 test_alias_use_generic）：`alias SizeF = Dict[string, float]`
//   若只取 kind=TYPE_DICT，**键值类型就丢了** ⇒ 消费方 `ts.w` 取不到 float ⇒
//   「变量 'w' 声明类型与初始化值类型不匹配」✗。同理 `alias Node = TreeNode` 也要带出底层聚合名 ✓
//   返回的副本归调用方所有（需要 type_free / 交给会 type_copy 的 add_* ✓）
static TypeInfo* ast_resolved_copy(ModuleSymbolTable* table, TypeInfo* ti, int depth) {
    if (!ti) return NULL;
    if (ti->kind == TYPE_STRUCT && ti->struct_name && depth < 8) {
        ModuleAliasSymbol* a = module_symbol_table_find_alias(table, ti->struct_name);
        if (a && a->type_info) return ast_resolved_copy(table, a->type_info, depth + 1);
    }
    TypeInfo* cp = type_copy(ti);
    if (cp) ast_fix_agg_kind(table, cp);
    return cp;
}

// 取"按本模块声明修正后"的 TypeKind（不改动入参 ✓）
//   为什么要它：TypeInfo 是 AST 的（不能改，也不该改 —— 同一份 AST 可能被多次读）；
//   而符号表要的是修正后的 kind ⇒ 只读地算一个出来 ✓（逻辑与 ast_fix_agg_kind 同源 ✓）
static TypeKind ast_kind_of(ModuleSymbolTable* table, TypeInfo* ti) {
    if (!ti) return TYPE_ANY;
    if (ti->kind == TYPE_STRUCT && ti->struct_name) {
        if (module_symbol_table_find_clib(table, ti->struct_name)) return TYPE_CLIB;
        if (module_symbol_table_find_face(table, ti->struct_name)) return TYPE_FACE;
        ModuleStructSymbol* s = module_symbol_table_find_struct(table, ti->struct_name);
        if (s && s->is_cstruct) return TYPE_CSTRUCT;
        return ast_alias_unwrap(table, ti->kind, ti->struct_name, 0);   // ★ 别名展开
    }
    return ti->kind;
}

static void ast_fill_one_func(ModuleSymbolTable* table, Ast* fn) {
    if (!fn || !fn->u.func.name) return;
    ModuleFuncSymbol* sym = module_symbol_table_find_func(table, fn->u.func.name);
    if (!sym) return;   // 建表职责本轮仍归扫描链：这里只覆盖已存在的条目 ✓

    // ---- 返回类型组：return_type / return_type_info / return_struct_name 必须一起换 ----
    //   实测：只换前两个、把 return_struct_name 留在扫描链 ⇒ 365/41 ✗；
    //   补齐后 395/11（parser 对未知名统一给 TYPE_STRUCT，不知道它是 clib）⇒ 加修正；
    //   修正后仍剩 4 个，是**依赖模块**的 clib（本表查不到）⇒ AST 把扫描链判定的
    //   TYPE_CLIB **降级**了 ⇒ 再加"退化放弃"保护 ✓
    //   规则：**同源字段组整组换；两套来源冲突时只允许更精确的替换** ✓
    if (fn->u.func.return_type) {
        TypeInfo* rt = type_copy(fn->u.func.return_type);
        if (rt) {
            ast_fix_agg_kind(table, rt);
            int degrade = (rt->kind == TYPE_STRUCT && sym->return_type_info &&
                           sym->return_type_info->kind != TYPE_STRUCT);
            if (degrade) {
                type_free(rt);
            } else {
                sym->return_type = rt->kind;
                sym->return_type_info = rt;
                sym->return_struct_name = rt->struct_name ? strdup(rt->struct_name) : NULL;
            }
        }
    }

    // ---- 参数组：param_count / param_types / param_struct_names 必须一起换 ----
    //   AST 侧是并行数组：param_types[i]（TypeInfo*）同时给出 kind 与聚合名，
    //   所以这一组**天然同源**，可以整组搬 ✓
    //   ⚠ param_text / param_default_texts 仍是**文本**、且按下标/default_count 索引 ⇒ 本轮不动 ✓
    //      （它们与 param_count 不同源，所以更不能只换 param_count ✓）
    //   ⚠ **带泛型形参的函数先跳过**：泛型形参（如 `T`）在符号表里有专门表示（见
    //     ModuleFuncSymbol::type_param_names 那套），与 AST 的 TypeInfo 口径尚未对齐
    //     （实测整组换后 400/6：generic_constraint_cross / generic_nullable_param /
    //      generic_pair_import / widget_deep / plane_war_headless / sdl_capture）⇒
    //     先只接管**无泛型形参**的函数，泛型那批等形参表示对齐后再开 ✓
    if (fn->u.func.param_types && fn->u.func.pcnt > 0 && fn->u.func.type_param_count == 0) {
        int pc = fn->u.func.pcnt;
        TypeKind* pts = (TypeKind*)malloc(sizeof(TypeKind) * pc);
        char** psn = (char**)malloc(sizeof(char*) * pc);
        if (pts && psn) {
            int degrade = 0;
            for (int i = 0; i < pc; i++) {
                TypeInfo* ti = fn->u.func.param_types[i];
                pts[i] = ast_kind_of(table, ti);
                psn[i] = (ti && ti->struct_name) ? strdup(ti->struct_name) : NULL;
                // 与返回组同一条规则：AST 判成 TYPE_STRUCT，而扫描链在那个位置已有更精确的
                //   结论（如 TYPE_CLIB）⇒ 说明该名字来自**依赖模块**、本表里查不到
                //   （实测 test_plane_war_headless / test_sdl_capture / test_widget_deep）
                //   ⇒ **整组放弃**（部分换会让数量与数组不同源 ✗）
                if (pts[i] == TYPE_STRUCT && sym->param_types && i < sym->param_count &&
                    sym->param_types[i] != TYPE_STRUCT) {
                    degrade = 1;
                }
            }
            if (degrade) {
                for (int i = 0; i < pc; i++) free(psn[i]);
                free(pts);
                free(psn);
            } else {
                // 旧数组不释放（同返回组：符号表进程内长存活、量小，先避 use-after-free ✓）
                sym->param_count = pc;
                sym->param_types = pts;
                sym->param_struct_names = psn;
            }
        } else {
            free(pts);
            free(psn);
        }
    }

    sym->is_async = fn->u.func.is_async;
}

// ---- 聚合类型修正 ----
//   为什么要修正：parser 对"未知名"统一给 TYPE_STRUCT（它不知道那个名字是 face/cstruct/clib，
//   那些是模块符号表的知识）⇒ AST 侧拿到 TypeInfo 后必须按**本模块声明**改对，
//   否则 `test_clib_cross*` 那类用例全红（实测 395/11 ✗）。
//   实现直接用符号表的 find_* 查（不需要自己再收集一遍名字集合 —— 表本来就是现成的 ✓）
static void ast_fix_agg_kind(ModuleSymbolTable* table, TypeInfo* t) {
    if (!table || !t || t->kind != TYPE_STRUCT || !t->struct_name) return;
    if (module_symbol_table_find_clib(table, t->struct_name)) {
        t->kind = TYPE_CLIB;
    } else if (module_symbol_table_find_face(table, t->struct_name)) {
        t->kind = TYPE_FACE;
    } else if (module_symbol_table_find_struct(table, t->struct_name) &&
               module_symbol_table_find_struct(table, t->struct_name)->is_cstruct) {
        t->kind = TYPE_CSTRUCT;
    } else {
        // ★ 别名（`export alias Color = int`）：解析到最底层 kind
        //   若底层**不是**聚合类型 ⇒ 名字必须清掉 —— 否则消费者看到「kind=INT 却带聚合名」
        //   这种自相矛盾的组合（struct_name 不释放：本副本随 ast_fix 调用方释放，量小 ✓）
        TypeKind k = ast_alias_unwrap(table, t->kind, t->struct_name, 0);
        if (k != TYPE_STRUCT && k != TYPE_FACE && k != TYPE_CSTRUCT && k != TYPE_CLIB) {
            t->struct_name = NULL;
        }
        t->kind = k;
    }
}

// ---- alias 类接管 ----
//   这一类最能体现"AST 提供更多信息"：扫描链要把类型**拼回字符串**再
//   `parse_type_from_string` 反解（第三份类型解析），AST 侧 `u.alias.type` 已经
//   是 parser 解析好的 TypeInfo ⇒ 直接 type_copy 进表 ✓
static void ast_fill_one_alias(ModuleSymbolTable* table, Ast* al) {
    if (!al || !al->u.alias.name || !al->u.alias.type) return;
    ModuleAliasSymbol* sym = module_symbol_table_find_alias(table, al->u.alias.name);
    TypeInfo* t = type_copy(al->u.alias.type);   // ⚠ 必须拷：AST 随后会被 ast_free，直接放指针会悬垂 ✗
    if (!t) return;
    ast_fix_agg_kind(table, t);
    if (!sym) {
        // ★ 建表路径（S10 删除老解析的前置）：scan_alias.inc 整段退役后由这里建条目 ✓
        //   add_alias 内部**复制**类型（扫描链随后 type_free 临时值 ⇒ 同一约定 ✓）✓
        module_symbol_table_add_alias(table, al->u.alias.name, t);
        type_free(t);
        return;
    }
    sym->type_info = t;   // 旧值不释放：符号表是进程内长存活缓存、每模块一份，量极小（TODO：并入 GC）
}

// ---- var 类接管 ----
//   `ModuleVarSymbol` 四个值字段（type / struct_name / type_info / is_const）**全部**来自
//   AST 的同一个 TypeInfo 与同一个节点 ⇒ 天然同源，整类一起换 ✓
//   扫描链为了拿这些要自己解析"类型在前/带泛型"的文本（scan_var.inc 那一大段），
//   AST 侧 u.var_decl.type 已经是 parser 的结果 ⇒ 这正是"AST 提供更多信息"的直接体现 ✓
static void ast_fill_one_var(ModuleSymbolTable* table, Ast* vd) {
    if (!vd || !vd->u.var_decl.name) return;
    ModuleVarSymbol* sym = module_symbol_table_find_var(table, vd->u.var_decl.name);
    if (!sym) return;
    TypeInfo* src = vd->u.var_decl.type;
    if (!src) return;

    // ★ 关键判据（2026-10-01 实测查明，此前误判为"parser 不解析类型名"，实为写法的区别）：
    //   TYPE_INFER(1) / TYPE_UNKNOWN(0) ⇒ 该 var **本来就没写类型标注**（如 `var x = 1`），
    //     AST 里没有类型信息，扫描链是从**初值**推断出来的 ⇒ 覆盖过去就是降级 ✗
    //   而显式标注（`int x = 5` / `Array[int] a = ...`）parser 已解析出真实 kind ⇒ 可以接管 ✓
    if (src->kind == TYPE_INFER || src->kind == TYPE_UNKNOWN) return;

    // 退化保护：显式标注但名字属于依赖模块等本表查不到的情形 ⇒ 保留扫描链结论 ✓
    if (src->kind == TYPE_STRUCT && sym->type_info && sym->type_info->kind != TYPE_STRUCT) return;

    // 整组换：type / struct_name / type_info 与 is_const 同源（都来自这一个 AST 节点）✓
    TypeInfo* cp = type_copy(src);
    if (!cp) return;
    ast_fix_agg_kind(table, cp);
    sym->type = cp->kind;
    sym->struct_name = cp->struct_name ? strdup(cp->struct_name) : NULL;
    sym->type_info = cp;      // 旧值不释放（量小、先避 use-after-free ✓）
    sym->is_const = vd->u.var_decl.is_const;
}

    // ⚠ 本轮**只做观测**，不改任何字段（改了必红：整类 396/10、只换 type+is_const 399/7）。
    //   （曾用 [vardiff] 诊断查明此处，结论已固化进上面的判据；诊断代码已移除 ✓）

// ---- clib 类接管 ----
//   ModuleClibSymbol 与它的 funcs[] 全是扁平的（TypeKind + 名字 + 计数），
//   没有嵌套 TypeInfo 指针 ⇒ 可以整类换 ✓ 与 cfunc 同类。
static void ast_fill_one_clib(ModuleSymbolTable* table, Ast* cd) {
    if (!cd || !cd->u.clib_def.name || !cd->u.clib_def.func_names) return;
    ModuleClibSymbol* sym = module_symbol_table_find_clib(table, cd->u.clib_def.name);
    if (!sym) return;
    int fc = cd->u.clib_def.func_count;
    if (fc < 0) return;
    ModuleClibFuncSymbol* fs =
        (ModuleClibFuncSymbol*)malloc(sizeof(ModuleClibFuncSymbol) * (fc > 0 ? (size_t)fc : 1));
    if (!fs) return;
    for (int i = 0; i < fc; i++) {
        memset(&fs[i], 0, sizeof(ModuleClibFuncSymbol));
        fs[i].name = cd->u.clib_def.func_names[i] ? strdup(cd->u.clib_def.func_names[i]) : NULL;
        TypeInfo* rt = cd->u.clib_def.func_return_types ? cd->u.clib_def.func_return_types[i] : NULL;
        fs[i].return_type = ast_kind_of(table, rt);
        fs[i].return_element_type = TYPE_PTR;      // 约定值：表示"无元素类型" ✓
        fs[i].return_struct_name = (rt && rt->struct_name) ? strdup(rt->struct_name) : NULL;
        int pc = cd->u.clib_def.func_param_counts ? cd->u.clib_def.func_param_counts[i] : 0;
        fs[i].param_count = pc;
        if (pc > 0 && cd->u.clib_def.func_param_types && cd->u.clib_def.func_param_types[i]) {
            TypeKind* pts = (TypeKind*)malloc(sizeof(TypeKind) * pc);
            TypeKind* pets = (TypeKind*)malloc(sizeof(TypeKind) * pc);
            char** psn = (char**)malloc(sizeof(char*) * pc);
            if (pts && pets && psn) {
                for (int k = 0; k < pc; k++) {
                    TypeInfo* ti = cd->u.clib_def.func_param_types[i][k];
                    pts[k] = ast_kind_of(table, ti);
                    pets[k] = TYPE_PTR;
                    psn[k] = (ti && ti->struct_name) ? strdup(ti->struct_name) : NULL;
                }
                fs[i].param_types = pts;
                fs[i].param_element_types = pets;
                fs[i].param_struct_names = psn;
            } else {
                free(pts); free(pets); free(psn);
            }
        }
    }
    sym->func_count = fc;
    sym->funcs = fs;      // 旧数组不释放（同前：量小、先避 use-after-free ✓）
}

// 关联常量的**值文本**：符号表侧存的是**原始文本**（消费者 visit_module.inc:97/165 按文本解析：
//   true/false/null/"引号串"/数值）⇒ 这里从 AST 字面量还原成同款文本 ✓
// 非字面量（如 `= OtherMod.CONST`）⇒ 返回 NULL：消费者取不到文本时退回 val_null()，
//   与扫描链"文本存下了但解析不出来"是同一下场（不会比原来更差 ✓）
static char* ast_const_value_text(Ast* v) {
    if (!v) return NULL;
    char buf[64];
    switch (v->kind) {
        case AST_NUM:
            if (v->u.num.is_bigint && v->u.num.bigint_str) return strdup(v->u.num.bigint_str);
            if (v->u.num.is_float) {
                snprintf(buf, sizeof(buf), "%g", v->u.num.value);
            } else {
                snprintf(buf, sizeof(buf), "%lld", (long long)v->u.num.value);
            }
            return strdup(buf);
        case AST_STRING: {
            if (!v->u.string.value) return NULL;
            // 带引号还原（消费者的字符串分支就是判首字符是引号 ⇒ 必须带 ✓）
            size_t n = strlen(v->u.string.value);
            char* s = (char*)malloc(n + 3);
            if (!s) return NULL;
            s[0] = '"';
            memcpy(s + 1, v->u.string.value, n);
            s[n + 1] = '"';
            s[n + 2] = '\0';
            return s;
        }
        case AST_BOOL: return strdup(v->u.boolean ? "true" : "false");
        case AST_NULL: return strdup("null");
        default: return NULL;
    }
}

// 该类型名是否是"泛型形参"（本 struct 的 T/U… 或本方法的）⇒ 返回形参名，否则 NULL ✓
//   为什么要单独认它：泛型形参在符号表里**不是聚合类型**，而是 TYPE_GENERIC_PARAM + 形参名
//   （扫描链经 mod_resolve_param_type 的**唯一实现**这么归，见 scan_struct.inc:335/369）
//   ⇒ AST 给的 `struct T` 必须翻译过来，否则跨模块调用报
//     「push 第 1 个参数类型不匹配: 期望 struct T, 实际 int」✗（8 个泛型用例）
static const char* ast_type_param_name(Ast* sd, Ast* fn, TypeInfo* ti) {
    if (!ti || !ti->struct_name) return NULL;
    const char* want = ti->struct_name;
    if (sd && sd->u.struct_def.type_params) {
        for (int i = 0; i < sd->u.struct_def.type_param_count; i++) {
            if (sd->u.struct_def.type_params[i] && strcmp(sd->u.struct_def.type_params[i], want) == 0)
                return sd->u.struct_def.type_params[i];
        }
    }
    if (fn && fn->u.func.type_params) {
        for (int i = 0; i < fn->u.func.type_param_count; i++) {
            if (fn->u.func.type_params[i] && strcmp(fn->u.func.type_params[i], want) == 0)
                return fn->u.func.type_params[i];
        }
    }
    return NULL;
}

// ---- struct / face 的"扁平元信息"接管 ----
//   为什么只接管这几项：structs[] / faces[] 的主体是**结构体数组**（fields / methods），
//   其中含 TypeInfo* ⇒ 触碰表示层（见 var 那段结论）✗；
//   而泛型形参名、impl 名这些是**纯字符串数组 + 计数**，与 AST 同一节点 ⇒ 天然同源，
//   可以安全接管 ✓（计数必须与它的平行数组一起换 —— 本迁移反复验证的规律 ✓）
static void ast_fill_one_struct_meta(ModuleSymbolTable* table, Ast* sd) {
    if (!sd || !sd->u.struct_def.name) return;
    ModuleStructSymbol* sym = module_symbol_table_find_struct(table, sd->u.struct_def.name);

    // 条目已存在（scan_struct 仍在建表）⇒ **任何阶段都只覆盖"扁平元信息"**（泛型形参名 / impl 名）。
    //   为什么 phase==1（语义之后）**也不**整体覆盖字段/方法（2026-10-01 定案，实测抓到 3 个回归）：
    //     · AST 的 TypeInfo 与符号表那套是**两套表示层**（后者随 .lenosymc 往返、被语义/代码生成直接读）
    //       ⇒ 换指针/换数组会让消费者读到"另一个世界"的东西 ✗
    //     · 具体症状（`--export-diff` 同批发现，均为全新编译才复现 ⇒ 缓存掩盖了它）：
    //       - generic_face_mid.leno(23,5) 「返回类型不匹配：期望 string，实际 any」
    //         （User 的方法表被换掉 ⇒ fmt 查不到 ⇒ 退化成 any ✗）
    //       - test_plane_war_headless 多出 [struct与null比较] 警告
    //         （fields[i].nullable 被 AST 侧覆盖 ⇒ 丢了可空性 ✗）
    //     · 而当初扫描链那份本来就是正确的，再覆盖一次纯属自伤 ✗
    //       （2026-10-01：scan_struct.inc 已整文件退役，建表由本文件的建表路径承担；
    //        这里"条目已存在就不碰 fields/methods"的判据**仍然保留** —— 它对 func/var
    //        那些仍由扫描链产出的类同样成立，且能挡住"两套表示层互踩"✗）
    if (sym) {
        int tpc = sd->u.struct_def.type_param_count;
        if (tpc >= 0 && sd->u.struct_def.type_params) {
            char** tpn = (char**)malloc(sizeof(char*) * (tpc > 0 ? (size_t)tpc : 1));
            if (tpn) {
                for (int i = 0; i < tpc; i++) {
                    tpn[i] = sd->u.struct_def.type_params[i]
                                 ? strdup(sd->u.struct_def.type_params[i]) : NULL;
                }
                sym->type_param_count = tpc;
                sym->type_param_names = tpn;
            }
        }
        int ic = sd->u.struct_def.impl_count;
        if (ic >= 0 && sd->u.struct_def.impl_names) {
            char** inn = (char**)malloc(sizeof(char*) * (ic > 0 ? (size_t)ic : 1));
            if (inn) {
                for (int i = 0; i < ic; i++) {
                    inn[i] = sd->u.struct_def.impl_names[i]
                                 ? strdup(sd->u.struct_def.impl_names[i]) : NULL;
                }
                sym->impl_count = ic;
                sym->impl_names = inn;
            }
        }
        return;
    }

    // 到这里 = 语义之后（phase 1，类型已完整）或条目不存在（建表）⇒ 整体搬字段与方法 ✓
    {
        int fc = sd->u.struct_def.field_count;
        int mc = sd->u.struct_def.method_count;
        if (fc < 0 || mc < 0) return;
        ModuleStructField* fields = (ModuleStructField*)calloc((size_t)(fc > 0 ? fc : 1), sizeof(ModuleStructField));
        ModuleStructMethod* methods = (ModuleStructMethod*)calloc((size_t)(mc > 0 ? mc : 1), sizeof(ModuleStructMethod));
        if (!fields || !methods) {
            free(fields);
            free(methods);
            return;
        }
        for (int i = 0; i < fc; i++) {
            fields[i].name = (sd->u.struct_def.field_names && sd->u.struct_def.field_names[i])
                                 ? strdup(sd->u.struct_def.field_names[i]) : NULL;
            TypeInfo* ti = sd->u.struct_def.field_types ? sd->u.struct_def.field_types[i] : NULL;
            fields[i].element_type = TYPE_PTR;
            const char* f_gname = ast_type_param_name(sd, NULL, ti);
            if (ti && f_gname) {
                // 泛型形参字段（`struct Pair[K,V] { K first }`）：与扫描链**同一口径**
                //   （scan_struct.inc:875-880）：type = TYPE_GENERIC_PARAM、struct_name 留空、
                //   **名字放进 type_info**（type_generic_param）—— 消费方优先读 type_info ✓
                //   缺了它 ⇒ 跨模块构造报「字段 'first' 类型不匹配: 期望 'struct K'，实际 'string'」✗
                fields[i].type = TYPE_GENERIC_PARAM;
                fields[i].struct_name = NULL;
                fields[i].type_info = type_generic_param(f_gname);
                fields[i].nullable = ti->nullable;
                fields[i].line = ti->line;
            } else if (ti) {
                fields[i].type = ast_kind_of(table, ti);
                fields[i].struct_name = ti->struct_name ? strdup(ti->struct_name) : NULL;
                if (ti->element_type) {
                    fields[i].element_type = ast_kind_of(table, ti->element_type);
                    fields[i].element_struct_name = ti->element_type->struct_name
                        ? strdup(ti->element_type->struct_name) : NULL;
                }
                fields[i].type_info = type_copy(ti);
                if (fields[i].type_info) ast_fix_agg_kind(table, fields[i].type_info);
                fields[i].nullable = ti->nullable;
                fields[i].line = ti->line;
            } else {
                fields[i].type = TYPE_ANY;
            }
            fields[i].is_private = sd->u.struct_def.field_private ? sd->u.struct_def.field_private[i] : 0;
        }
        for (int i = 0; i < mc; i++) {
            Ast* fn = sd->u.struct_def.methods ? sd->u.struct_def.methods[i] : NULL;
            if (!fn) continue;
            // ⚠ 方法名必须是 **`Struct::方法名`** 全名（2026-10-01 修）：
            //   符号表里方法的键就是这个格式（scan_struct.inc:275 同款），
            //   而 `module_symbol_table_find_struct_method` 也按 "%s::%s" 查
            //   ⇒ 这里存裸名会让所有跨模块方法解析失败（实测症状：
            //      `类型 'struct User' 没有方法 'fmt'` ✗）
            if (fn->u.func.name) {
                char mkey[256];
                snprintf(mkey, sizeof(mkey), "%s::%s", sd->u.struct_def.name, fn->u.func.name);
                methods[i].name = strdup(mkey);
            } else {
                methods[i].name = NULL;
            }
            TypeInfo* rt = fn->u.func.return_type;
            const char* rt_gname = ast_type_param_name(sd, fn, rt);
            if (rt_gname) {
                // 返回泛型形参：符号表口径是 kind + 形参名（不是聚合类型）✓
                methods[i].return_type = TYPE_GENERIC_PARAM;
                methods[i].return_struct_name = NULL;
                methods[i].return_type_param_name = strdup(rt_gname);
                methods[i].return_generic_count = 0;
                methods[i].return_type_info = NULL;
            } else {
                // ⚠ 必须用**完整解析副本**（别名整体展开）：只取 kind 会丢元素/键值类型，
                //   而且**扁平三元组**（return_type / return_struct_name / return_type_info）
                //   必须同源 ⇒ 名字也从解析后的副本取（别名指向 Dict 时名字为空 ✓）
                TypeInfo* rti = ast_resolved_copy(table, rt, 0);
                methods[i].return_type = rti ? rti->kind : TYPE_ANY;
                methods[i].return_struct_name = (rti && rti->struct_name) ? strdup(rti->struct_name) : NULL;
                methods[i].return_type_info = rti;
            }
            int pc = fn->u.func.pcnt;
            methods[i].param_count = pc;
            if (pc > 0 && fn->u.func.param_types) {
                TypeKind* pts = (TypeKind*)malloc(sizeof(TypeKind) * pc);
                char** psn = (char**)malloc(sizeof(char*) * pc);
                char** pg = (char**)calloc((size_t)pc, sizeof(char*));
                if (pts && psn && pg) {
                    for (int k = 0; k < pc; k++) {
                        TypeInfo* ti = fn->u.func.param_types[k];
                        const char* gname = ast_type_param_name(sd, fn, ti);
                        if (gname) {
                            pts[k] = TYPE_GENERIC_PARAM;
                            psn[k] = NULL;
                            pg[k] = strdup(gname);
                        } else {
                            pts[k] = ast_kind_of(table, ti);
                            psn[k] = (ti && ti->struct_name) ? strdup(ti->struct_name) : NULL;
                        }
                    }
                    methods[i].param_types = pts;
                    methods[i].param_struct_names = psn;
                    methods[i].param_generic_names = pg;
                } else {
                    free(pts);
                    free(psn);
                    free(pg);
                }
            }
            methods[i].line = fn->line;
            methods[i].is_async = fn->u.func.is_async;
            methods[i].is_private = fn->u.func.is_private;
        }
        if (sym) {
            // 整组覆盖（phase 1）：构造出来的数组**所有权交给符号表** ⇒ 这里不要释放 ✓
            sym->field_count = fc;
            sym->fields = fields;
            sym->method_count = mc;
            sym->methods = methods;
            int tpc2 = sd->u.struct_def.type_param_count;
            sym->type_param_count = tpc2;
            if (sd->u.struct_def.type_params && tpc2 >= 0) {
                char** tpn2 = (char**)malloc(sizeof(char*) * (tpc2 > 0 ? (size_t)tpc2 : 1));
                if (tpn2) {
                    for (int i = 0; i < tpc2; i++) {
                        tpn2[i] = sd->u.struct_def.type_params[i]
                                      ? strdup(sd->u.struct_def.type_params[i]) : NULL;
                    }
                    sym->type_param_names = tpn2;
                }
            }
            if (sd->u.struct_def.impl_names) {
                int ic2 = sd->u.struct_def.impl_count;
                char** inn2 = (char**)malloc(sizeof(char*) * (ic2 > 0 ? (size_t)ic2 : 1));
                if (inn2) {
                    for (int i = 0; i < ic2; i++) {
                        inn2[i] = sd->u.struct_def.impl_names[i]
                                      ? strdup(sd->u.struct_def.impl_names[i]) : NULL;
                    }
                    sym->impl_count = ic2;
                    sym->impl_names = inn2;
                }
            }
            return;
        }
        // 建表（条目不存在）：add_struct 内部复制 ⇒ 随后释放本地构造 ✓
        module_symbol_table_add_struct(table, sd->u.struct_def.name, fc, fields, mc, methods, 0,
                                       sd->u.struct_def.type_param_count, sd->u.struct_def.type_params);
        // ⚠ impl 必须单独设置（add_struct 的形参里没有它，见 module_symbol_table.h 的说明）：
        //   缺了它 ⇒ 语义判不出"该 struct 实现了哪个 face" ⇒ 经 face 的方法解析整条路径走不到 ✗
        module_symbol_table_set_struct_impls(table, sd->u.struct_def.name,
                                            sd->u.struct_def.impl_count, sd->u.struct_def.impl_names);
        // ⚠ 关联常量同理（add_struct 也没有 const 形参）：
        //   缺了它 ⇒ 跨模块引用 `spl.Splitter.HORIZONTAL` 报
        //   「struct 'Splitter' 没有字段 'HORIZONTAL'」✗（plane_war 冷跑的下一批错误）
        if (sd->u.struct_def.const_count > 0) {
            int cc = sd->u.struct_def.const_count;
            char** cnames = (char**)calloc((size_t)cc, sizeof(char*));
            char** cvals = (char**)calloc((size_t)cc, sizeof(char*));
            if (cnames && cvals) {
                for (int i = 0; i < cc; i++) {
                    cnames[i] = (sd->u.struct_def.const_names && sd->u.struct_def.const_names[i])
                                   ? strdup(sd->u.struct_def.const_names[i]) : NULL;
                    cvals[i] = (sd->u.struct_def.const_values && sd->u.struct_def.const_values[i])
                                   ? ast_const_value_text(sd->u.struct_def.const_values[i]) : NULL;
                }
                module_symbol_table_set_struct_consts(table, sd->u.struct_def.name, cc, cnames, cvals);
                for (int i = 0; i < cc; i++) {
                    free(cnames[i]);
                    free(cvals[i]);
                }
            }
            free(cnames);
            free(cvals);
        }
        for (int i = 0; i < fc; i++) {
            free(fields[i].name);
            free(fields[i].struct_name);
            free(fields[i].element_struct_name);
        }
        for (int i = 0; i < mc; i++) {
            free(methods[i].name);
            free(methods[i].return_struct_name);
            if (methods[i].param_struct_names) {
                for (int k = 0; k < methods[i].param_count; k++) free(methods[i].param_struct_names[k]);
                free(methods[i].param_struct_names);
            }
            free(methods[i].param_types);
        }
        free(fields);
        free(methods);
    }
}

static void ast_fill_one_face_meta(ModuleSymbolTable* table, Ast* fd) {
    if (!fd || !fd->u.face_def.name) return;
    ModuleFaceSymbol* sym = module_symbol_table_find_face(table, fd->u.face_def.name);
    if (sym) {
        sym->type_param_count = fd->u.face_def.type_param_count;   // 已有条目 ⇒ 只覆盖这项（无平行数组 ✓）
        return;
    }
    // ★ 建表路径（scan_face.inc 退役的前提）：把 methods 数组整体建出来 ✓
    int mc = fd->u.face_def.method_count;
    if (mc < 0) return;
    ModuleFaceMethodSymbol* methods =
        (ModuleFaceMethodSymbol*)calloc((size_t)(mc > 0 ? mc : 1), sizeof(ModuleFaceMethodSymbol));
    if (!methods) return;
    for (int i = 0; i < mc; i++) {
        methods[i].name = (fd->u.face_def.method_names && fd->u.face_def.method_names[i])
                              ? strdup(fd->u.face_def.method_names[i]) : NULL;
        TypeInfo* rt = fd->u.face_def.method_return_types ? fd->u.face_def.method_return_types[i] : NULL;
        methods[i].return_type = ast_kind_of(table, rt);
        methods[i].return_struct_name = (rt && rt->struct_name) ? strdup(rt->struct_name) : NULL;
        int pc = fd->u.face_def.method_param_counts ? fd->u.face_def.method_param_counts[i] : 0;
        methods[i].param_count = pc;
        if (pc > 0 && fd->u.face_def.method_param_types && fd->u.face_def.method_param_types[i]) {
            TypeKind* pts = (TypeKind*)malloc(sizeof(TypeKind) * pc);
            char** psn = (char**)malloc(sizeof(char*) * pc);
            if (pts && psn) {
                for (int k = 0; k < pc; k++) {
                    TypeInfo* ti = fd->u.face_def.method_param_types[i][k];
                    pts[k] = ast_kind_of(table, ti);
                    psn[k] = (ti && ti->struct_name) ? strdup(ti->struct_name) : NULL;
                }
                methods[i].param_types = pts;
                methods[i].param_struct_names = psn;
            } else {
                free(pts);
                free(psn);
            }
        }
    }
    // add_face 内部复制（与扫描链同一约定）⇒ 随后释放临时结构 ✓
    module_symbol_table_add_face(table, fd->u.face_def.name, mc, methods,
                                 fd->u.face_def.type_param_count);
    for (int i = 0; i < mc; i++) {
        free(methods[i].name);
        free(methods[i].return_struct_name);
        if (methods[i].param_struct_names) {
            for (int k = 0; k < methods[i].param_count; k++) free(methods[i].param_struct_names[k]);
            free(methods[i].param_struct_names);
        }
        free(methods[i].param_types);
    }
    free(methods);
}

// ---- cstruct：**建表**（scan_cstruct.inc 退役的前提）----
//   为什么现在要建表而不是覆盖：AST 填充器只做覆盖时，删掉 scan_cstruct.inc 那张表就没人建了 ✗
//   字段映射：AST 的 field_types[i] 一个 TypeInfo 同时给出 kind、聚合名与元素类型 ✓
static void ast_fill_one_cstruct(ModuleSymbolTable* table, Ast* cd) {
    if (!cd || !cd->u.cstruct_def.name) return;
    if (module_symbol_table_find_struct(table, cd->u.cstruct_def.name)) return;  // 已有条目 ⇒ 交给覆盖路径
    int fc = cd->u.cstruct_def.field_count;
    if (fc < 0) return;
    ModuleStructField* fields =
        (ModuleStructField*)calloc((size_t)(fc > 0 ? fc : 1), sizeof(ModuleStructField));
    if (!fields) return;
    for (int i = 0; i < fc; i++) {
        fields[i].name = (cd->u.cstruct_def.field_names && cd->u.cstruct_def.field_names[i])
                             ? strdup(cd->u.cstruct_def.field_names[i]) : NULL;
        TypeInfo* ti = cd->u.cstruct_def.field_types ? cd->u.cstruct_def.field_types[i] : NULL;
        fields[i].element_type = TYPE_PTR;      // 约定值：表示"无元素类型"（与扫描链一致 ✓）
        if (ti) {
            fields[i].type = ast_kind_of(table, ti);
            fields[i].struct_name = ti->struct_name ? strdup(ti->struct_name) : NULL;
            if (ti->element_type) {
                // ⚠ 元素类型也要走同一个修正：嵌套 cstruct 的元素名在本表里能查到，
                //   直接取 kind 会停在 TYPE_STRUCT（test_nested_cstruct_field_type 抓到的 ✓）
                fields[i].element_type = ast_kind_of(table, ti->element_type);
                fields[i].element_struct_name = ti->element_type->struct_name
                    ? strdup(ti->element_type->struct_name) : NULL;
            }
            fields[i].type_info = type_copy(ti);
            if (fields[i].type_info) {
                // ⚠ 副本里的 kind 也要修正：消费者读的是 type_info 而不是上面的扁平 type。
                //   嵌套 cstruct 字段（`cstruct Rect { Point top_left }`）不修就仍是 TYPE_STRUCT
                //   ⇒ 消费方看到 struct/any ⇒ 「不能在 any 上访问字段」硬编译错
                //   （test_nested_cstruct_field_type 抓到的正是这条 ✓）
                ast_fix_agg_kind(table, fields[i].type_info);
            }
            fields[i].nullable = ti->nullable;
            fields[i].line = ti->line;
        } else {
            fields[i].type = TYPE_ANY;
        }
        fields[i].is_private = 0;   // cstruct 字段没有 pri 语法 ✓
    }
    // add_struct 内部复制字段（与扫描链同一约定 ✓）⇒ 随后释放我们造的临时名字
    module_symbol_table_add_struct(table, cd->u.cstruct_def.name, fc, fields,
                                   0, NULL, /*is_cstruct=*/1, 0, NULL);
    for (int i = 0; i < fc; i++) {
        free(fields[i].name);
        free(fields[i].struct_name);
        free(fields[i].element_struct_name);
        // type_info 不释放：所有权已随 add_struct 转移（与 scan_cstruct.inc 同约定 ✓）
    }
    free(fields);
}

// ---- enum 类接管 ----
//   member_names / member_values 与 member_count 是同一节点的**同源平行数组** ⇒ 整组换 ✓
//   （与 func 的 param 组同一条规律：要么整组换，要么别动 ✓）
static void ast_fill_one_enum(ModuleSymbolTable* table, Ast* ed) {
    if (!ed || !ed->u.enum_def.name) return;
    ModuleEnumSymbol* sym = module_symbol_table_find_enum(table, ed->u.enum_def.name);
    int mc = ed->u.enum_def.member_count;
    if (mc < 0) return;
    char** mn = (char**)malloc(sizeof(char*) * (mc > 0 ? (size_t)mc : 1));
    int64_t* mv = (int64_t*)malloc(sizeof(int64_t) * (mc > 0 ? (size_t)mc : 1));
    if (!mn || !mv) {
        free(mn);
        free(mv);
        return;
    }
    for (int i = 0; i < mc; i++) {
        mn[i] = (ed->u.enum_def.member_names && ed->u.enum_def.member_names[i])
                    ? strdup(ed->u.enum_def.member_names[i]) : NULL;
        mv[i] = ed->u.enum_def.member_values ? ed->u.enum_def.member_values[i] : 0;
    }
    if (!sym) {
        // ★ 建表路径（S10 删除老解析的前置）：条目不存在时由 AST 直接建出来。
        //   add_enum 内部会**复制**名字（扫描链随后自己 free 临时 buf ⇒ 同一约定 ✓）✓
        //   有这条之后，scan_enum.inc 的"符号建表"部分就可以停用，只留它的导出名登记 ✓
        module_symbol_table_add_enum(table, ed->u.enum_def.name, mc, mn, mv);
        for (int i = 0; i < mc; i++) free(mn[i]);
        free(mn);
        free(mv);
        return;
    }
    // 旧数组不释放（同前：符号表进程内长存活、量小，先避 use-after-free ✓）
    sym->member_count = mc;
    sym->member_names = mn;
    sym->member_values = mv;
}

// ---- cfunc 类接管 ----
//   ModuleCfuncSymbol 的字段全是扁平量（TypeKind / 名字 / 计数），没有嵌套结构指针
//   ⇒ 可以整套换，不碰表示层 ✓（这正是"能托管"与"不能托管"的分界线：是否触碰 TypeInfo 内部）
//   ⚠ param_element_types / return_element_type 恒填 TYPE_PTR —— 那是这套符号表里表示
//     "无元素类型"的约定值（Ptr[T] 的 T）；AST 侧没有对应的扁平字段 ⇒ 保守取此值 ✓
static void ast_fill_one_cfunc(ModuleSymbolTable* table, Ast* cf) {
    if (!cf || !cf->u.cfunc_decl.name) return;
    ModuleCfuncSymbol* sym = module_symbol_table_find_cfunc(table, cf->u.cfunc_decl.name);
    if (!sym) return;
    int pc = cf->u.cfunc_decl.param_count;
    if (pc > 0 && cf->u.cfunc_decl.param_types) {
        TypeKind* pts = (TypeKind*)malloc(sizeof(TypeKind) * pc);
        TypeKind* pets = (TypeKind*)malloc(sizeof(TypeKind) * pc);
        char** psn = (char**)malloc(sizeof(char*) * pc);
        char** pnm = (char**)malloc(sizeof(char*) * pc);
        if (pts && pets && psn && pnm) {
            for (int i = 0; i < pc; i++) {
                TypeInfo* ti = cf->u.cfunc_decl.param_types[i];
                pts[i] = ast_kind_of(table, ti);
                pets[i] = TYPE_PTR;
                psn[i] = (ti && ti->struct_name) ? strdup(ti->struct_name) : NULL;
                pnm[i] = (cf->u.cfunc_decl.param_names && cf->u.cfunc_decl.param_names[i])
                             ? strdup(cf->u.cfunc_decl.param_names[i]) : NULL;
            }
            sym->param_count = pc;
            sym->param_types = pts;
            sym->param_element_types = pets;
            sym->param_struct_names = psn;
            sym->param_names = pnm;
        } else {
            free(pts); free(pets); free(psn); free(pnm);
        }
    }
    if (cf->u.cfunc_decl.return_type) {
        sym->return_type = ast_kind_of(table, cf->u.cfunc_decl.return_type);
        sym->return_element_type = TYPE_PTR;
        sym->return_struct_name = cf->u.cfunc_decl.return_type->struct_name
                                      ? strdup(cf->u.cfunc_decl.return_type->struct_name) : NULL;
    }
}

static void ast_symbol_fill_provider(ModuleSymbolTable* table, const char* src) {
    if (!table || !src) return;
    Parser p;
    parser_init(&p, src);
    if (parser_parse(&p) < 0) {
        ast_free(p.root);
        return;         // 语法错 ⇒ 不覆盖（编译本来就会在 parse 阶段报错 ✓）
    }
    Ast* root = p.root;
    if (root && root->kind == AST_BLOCK) {
        for (int i = 0; i < root->u.block.count; i++) {
            Ast* st = root->u.block.items[i];
            if (!st) continue;
            Ast* d = (st->kind == AST_EXPORT && st->u.export.decl) ? st->u.export.decl : st;
            if (d->kind == AST_FUNC_DEF) ast_fill_one_func(table, d);
            else if (d->kind == AST_ALIAS) ast_fill_one_alias(table, d);
            else if (d->kind == AST_CFUNC_DECL) ast_fill_one_cfunc(table, d);
            else if (d->kind == AST_ENUM_DEF) ast_fill_one_enum(table, d);
            else if (d->kind == AST_STRUCT_DEF) ast_fill_one_struct_meta(table, d);
            else if (d->kind == AST_FACE_DEF) ast_fill_one_face_meta(table, d);
            else if (d->kind == AST_CLIB_DEF) ast_fill_one_clib(table, d);
            else if (d->kind == AST_CSTRUCT_DEF) ast_fill_one_cstruct(table, d);
            else if (d->kind == AST_VAR_DECL) ast_fill_one_var(table, d);
            // ⚠ var 类当前只接管**扁平两项**（type / is_const），type_info 仍留给扫描链
            //   —— 连"只收窄到基本类型"也红：396/10，test_export_const_type /
            //   test_nested_2d / test_nested_generic_field / test_lenosys / test_native_module_resolve …）
            //   ⇒ 说明障碍不在类型种类，而在**类型表示层本身**：AST 的 TypeInfo 与符号表那套
            //     （经 mod_scan_params / parse_type_from_string 产出、随 .lenosymc 往返、
            //      被语义与代码生成直接读取）不是同一套结构 ⇒ 换指针会让消费者读到"另一个世界"的东西 ✗
            //   结论：**先对齐表示层（或让消费者改用 AST 的 TypeInfo），再谈接管各类符号** ✓
            //   在此之前，alias / func 返回与参数组能接管，是因为它们只需要 TypeKind + 聚合名 ✓
            // else if (d->kind == AST_VAR_DECL) ast_fill_one_var(table, d);
        }
    }
    ast_free(p.root);
}

// 填充阶段：0 = 扫描阶段（只有语法信息）；1 = **语义分析之后**（类型已解析完整）✓
//   为什么必须有这个概念：struct 的字段类型 / 方法签名、var 的推断型，
//   在语法阶段是**不完整**的（实测停用 scan_struct 后 371/35）⇒ 这些类只能在 phase==1
//   做整组覆盖；在 phase==0 用它们会拿"未解析的类型"把正确的表覆盖坏 ✗
static int g_ast_fill_phase = 0;

int ast_fill_phase(void) {
    return g_ast_fill_phase;
}

// 第二次填充入口：由 module_compiler 在**语义分析之后**调用，传**已语义化的 AST**
//   （void* 是为了不在 core 头里引入 Ast 类型 ✓）
void module_ast_symbols_fill_from_ast(void* table_v, void* ast_root) {
    ModuleSymbolTable* table = (ModuleSymbolTable*)table_v;
    Ast* root = (Ast*)ast_root;
    if (!table || !root || root->kind != AST_BLOCK) return;
    g_ast_fill_phase = 1;
    for (int i = 0; i < root->u.block.count; i++) {
        Ast* st = root->u.block.items[i];
        if (!st) continue;
        Ast* d = (st->kind == AST_EXPORT && st->u.export.decl) ? st->u.export.decl : st;
        if (d->kind == AST_FUNC_DEF) ast_fill_one_func(table, d);
        else if (d->kind == AST_ALIAS) ast_fill_one_alias(table, d);
        else if (d->kind == AST_CFUNC_DECL) ast_fill_one_cfunc(table, d);
        else if (d->kind == AST_ENUM_DEF) ast_fill_one_enum(table, d);
        else if (d->kind == AST_STRUCT_DEF) ast_fill_one_struct_meta(table, d);
        else if (d->kind == AST_FACE_DEF) ast_fill_one_face_meta(table, d);
        else if (d->kind == AST_CLIB_DEF) ast_fill_one_clib(table, d);
        else if (d->kind == AST_CSTRUCT_DEF) ast_fill_one_cstruct(table, d);
        else if (d->kind == AST_VAR_DECL) ast_fill_one_var(table, d);
    }
    g_ast_fill_phase = 0;
}

void module_ast_symbols_register(void) {
    module_symbol_table_set_ast_fill_provider(ast_symbol_fill_provider);
}
