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
    } else {
        ModuleStructSymbol* s = module_symbol_table_find_struct(table, t->struct_name);
        if (s && s->is_cstruct) t->kind = TYPE_CSTRUCT;
    }
}

// ---- alias 类接管 ----
//   这一类最能体现"AST 提供更多信息"：扫描链要把类型**拼回字符串**再
//   `parse_type_from_string` 反解（第三份类型解析），AST 侧 `u.alias.type` 已经
//   是 parser 解析好的 TypeInfo ⇒ 直接 type_copy 进表 ✓
static void ast_fill_one_alias(ModuleSymbolTable* table, Ast* al) {
    if (!al || !al->u.alias.name || !al->u.alias.type) return;
    ModuleAliasSymbol* sym = module_symbol_table_find_alias(table, al->u.alias.name);
    if (!sym) return;
    TypeInfo* t = type_copy(al->u.alias.type);   // ⚠ 必须拷：AST 随后会被 ast_free，直接放指针会悬垂 ✗
    if (!t) return;
    ast_fix_agg_kind(table, t);
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
    if (!src) return;   // 无类型标注（靠 init 推断）⇒ 保留扫描链结论 ✓
    // ⚠ 收窄到**基本类型**：实测整类换后 396/10（test_nested_2d / test_nested_generic_field /
    //   test_export_const_type / test_lenosys …）—— 聚合与泛型类型的 AST 表示与扫描链那套
    //   口径不同，而消费者（语义/代码生成）依赖扫描链的表示 ⇒ 那些先留给扫描链；
    //   基本类型（int/string/bool/float/…）两边一致，换过来是纯收益 ✓
    switch (src->kind) {
        case TYPE_STRUCT: case TYPE_FACE: case TYPE_CSTRUCT: case TYPE_CLIB:
        case TYPE_ARRAY: case TYPE_DICT: case TYPE_PTR: case TYPE_PTR_GENERIC:
        case TYPE_FUNCTION:
            return;
        default:
            break;
    }
    // ⚠ 只换**扁平两项**：type（TypeKind）与 is_const。
    //   实测：连 type_info 一起换会红（396/10，同一批用例）⇒ 障碍在 TypeInfo 这一层，
    //   与类型种类无关；而 type 与 is_const 是扁平量，换过来无耦合 ✓
    //   struct_name 仍留给扫描链（它与 type_info 同源，单独换反而会把两者拆开 ✗）
    sym->type = ast_kind_of(table, src);
    sym->is_const = vd->u.var_decl.is_const;
}

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

// ---- struct / face 的"扁平元信息"接管 ----
//   为什么只接管这几项：structs[] / faces[] 的主体是**结构体数组**（fields / methods），
//   其中含 TypeInfo* ⇒ 触碰表示层（见 var 那段结论）✗；
//   而泛型形参名、impl 名这些是**纯字符串数组 + 计数**，与 AST 同一节点 ⇒ 天然同源，
//   可以安全接管 ✓（计数必须与它的平行数组一起换 —— 本迁移反复验证的规律 ✓）
static void ast_fill_one_struct_meta(ModuleSymbolTable* table, Ast* sd) {
    if (!sd || !sd->u.struct_def.name) return;
    ModuleStructSymbol* sym = module_symbol_table_find_struct(table, sd->u.struct_def.name);
    if (!sym) return;

    int tpc = sd->u.struct_def.type_param_count;
    if (tpc >= 0 && sd->u.struct_def.type_params) {
        char** tpn = (char**)malloc(sizeof(char*) * (tpc > 0 ? (size_t)tpc : 1));
        if (tpn) {
            for (int i = 0; i < tpc; i++) {
                tpn[i] = sd->u.struct_def.type_params[i]
                             ? strdup(sd->u.struct_def.type_params[i]) : NULL;
            }
            sym->type_param_count = tpc;
            sym->type_param_names = tpn;     // 旧数组不释放（同前，量小、先避 use-after-free ✓）
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
}

static void ast_fill_one_face_meta(ModuleSymbolTable* table, Ast* fd) {
    if (!fd || !fd->u.face_def.name) return;
    ModuleFaceSymbol* sym = module_symbol_table_find_face(table, fd->u.face_def.name);
    if (!sym) return;
    sym->type_param_count = fd->u.face_def.type_param_count;   // 单个计数、无平行数组 ⇒ 安全 ✓
}

// ---- enum 类接管 ----
//   member_names / member_values 与 member_count 是同一节点的**同源平行数组** ⇒ 整组换 ✓
//   （与 func 的 param 组同一条规律：要么整组换，要么别动 ✓）
static void ast_fill_one_enum(ModuleSymbolTable* table, Ast* ed) {
    if (!ed || !ed->u.enum_def.name) return;
    ModuleEnumSymbol* sym = module_symbol_table_find_enum(table, ed->u.enum_def.name);
    if (!sym) return;
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
            // else if (d->kind == AST_VAR_DECL) ast_fill_one_var(table, d);
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

void module_ast_symbols_register(void) {
    module_symbol_table_set_ast_fill_provider(ast_symbol_fill_provider);
}
