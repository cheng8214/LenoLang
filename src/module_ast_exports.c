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
