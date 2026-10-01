/* parser AST → 模块符号表（compiler-only）
 *
 * 见 include/module_ast_exports.h 的说明：导出名提供者 + AST 符号填充器。
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
//   复用它而不是自己 fopen —— 它已处理 UTF-8/中文路径与 BOM ✓
extern char* read_module_file(const char* file_path, const char* current_file);

// ============================================================================
// 内部：可增长的名字清单（空名/重名忽略 ⇒ 同一名字重复导出只算一次 ✓）
// ============================================================================

static void ast_exports_push(AstExportList* list, const char* name) {
    if (!list || !name || !name[0]) return;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->names[i], name) == 0) return;
    }
    if (list->count >= list->capacity) {
        int new_cap = list->capacity == 0 ? 16 : list->capacity * 2;
        char** gn = (char**)realloc(list->names, sizeof(char*) * new_cap);
        if (!gn) return;                 // 内存不足：跳过该名字
        list->names = gn;
        list->capacity = new_cap;
    }
    char* s = strdup(name);
    if (!s) return;
    list->names[list->count++] = s;
}

// 单条声明 ⇒ 贡献哪些导出名。
//   ★ 口径 = **顶层 `export` 声明**贡献自己的名字；解构贡献**每个**名字 ✓
static void ast_exports_collect_decl(Ast* decl, AstExportList* out) {
    if (!decl) return;
    switch (decl->kind) {
        case AST_FUNC_DEF:
            ast_exports_push(out, decl->u.func.name);
            break;
        case AST_VAR_DECL:
            ast_exports_push(out, decl->u.var_decl.name);
            break;
        case AST_DESTRUCT_DECL:
            // export var[int,int,int](a,b,c) ⇒ 三个名字各自进表（不是整体一个 ✓）
            for (int i = 0; i < decl->u.destruct_decl.slot_count; i++) {
                ast_exports_push(out, decl->u.destruct_decl.names[i]);
            }
            break;
        case AST_STRUCT_DEF:
            ast_exports_push(out, decl->u.struct_def.name);
            break;
        case AST_FACE_DEF:
            ast_exports_push(out, decl->u.face_def.name);
            break;
        case AST_ENUM_DEF:
            ast_exports_push(out, decl->u.enum_def.name);
            break;
        case AST_CSTRUCT_DEF:
            ast_exports_push(out, decl->u.cstruct_def.name);
            break;
        case AST_ALIAS:
            ast_exports_push(out, decl->u.alias.name);
            break;
        case AST_CFUNC_DECL:
            ast_exports_push(out, decl->u.cfunc_decl.name);
            break;
        // ⚠ AST_CLIB_DEF 故意**不收**：`clib xxx { }` 不是"顶层 export 声明" ✓
        default:
            break;
    }
}

int module_ast_collect_exports(const char* src, AstExportList* out) {
    if (!out) return -1;
    out->names = NULL;
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
    list->names = NULL;
    list->count = 0;
    list->capacity = 0;
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
// 前向声明：泛型形参名判定（定义见下；**func 的参数组**也要用它 ⇒ 必须先声明 ✓）
static const char* ast_type_param_name(Ast* sd, Ast* fn, TypeInfo* ti);
// 前向声明：字面量 → 值文本（定义见下；**func 的默认值**也要用它 ✓）
static char* ast_const_value_text(Ast* v);
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
    if (!sym) {
        // ★ 建表路径（scan_func.inc 退役的前提）：AST 直接建条目 ✓
        //   param_text / param_default_texts 一律 NULL，依据：
        //     · param_text —— 语义侧 ⑱ 转正后**已改读 param_types**（visit_module.inc:907-910
        //       明确记着"就地解析 param_text"那段已删除），别处也没有消费者 ✓
        //     · defaults —— 消费者只用 default_count（算"必需参数个数"）；文本数组是
        //       "参数表→类型"那份重复实现的残留 ⇒ 随扫描链一起退役 ✓
        //   缓存序列化对两者都 NULL 安全（sym_cache_write_string 有 NULL 标记、defaults 有 has 位 ✓）
        TypeInfo* rti = ast_resolved_copy(table, fn->u.func.return_type, 0);
        // ⚠ 默认值**必须给文本**（此前的判断是错的）：调用点在**另一个模块**里，拿不到被调函数
        //   的 AST ⇒ 只能从符号表读 param_default_texts 来补默认值（assert/xmod_defaults.leno
        //   的注释写得很明白）⇒ 传 NULL 会让默认值变 null ⇒「加法运算: null 不能参与运算」✗
        //   （实测 test_cross_module_default_args；文本由 ast_const_value_text 从字面量还原 ✓）
        char** dtexts = NULL;
        if (fn->u.func.pcnt > 0) {
            dtexts = (char**)calloc((size_t)fn->u.func.pcnt, sizeof(char*));
            if (dtexts && fn->u.func.param_defaults) {
                for (int i = 0; i < fn->u.func.pcnt; i++) {
                    if (fn->u.func.param_defaults[i]) {
                        dtexts[i] = ast_const_value_text(fn->u.func.param_defaults[i]);
                    }
                }
            }
        }
        module_symbol_table_add_func(table, fn->u.func.name,
                                     rti ? rti->kind : TYPE_ANY,
                                     (rti && rti->struct_name) ? rti->struct_name : NULL,
                                     fn->u.func.type_param_count, rti,
                                     NULL, fn->u.func.pcnt, fn->u.func.default_count, dtexts,
                                     fn->u.func.is_async);
        if (dtexts) {
            for (int i = 0; i < fn->u.func.pcnt; i++) free(dtexts[i]);
            free(dtexts);
        }
        if (rti) type_free(rti);   // add_func 内部 type_copy（见 sym_table_add.inc:17）✓
        sym = module_symbol_table_find_func(table, fn->u.func.name);
        if (!sym) return;
        // add_func 把 type_param_names / param_types 等**显式置空**（扩容槽位是垃圾值）
        // ⇒ 泛型形参名必须由这里补填，否则泛型函数的形参名整批丢失 ✗
        if (fn->u.func.type_param_count > 0 && fn->u.func.type_params) {
            int tpc = fn->u.func.type_param_count;
            char** tpn = (char**)calloc((size_t)tpc, sizeof(char*));
            if (tpn) {
                for (int i = 0; i < tpc; i++) {
                    tpn[i] = fn->u.func.type_params[i] ? strdup(fn->u.func.type_params[i]) : NULL;
                }
                sym->type_param_names = tpn;
            }
        }
    }

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
    //   ★ 泛型形参也一并接管（此前为了"形参口径未对齐"整类跳过 ⇒ 现在对齐了）：
    //     func 侧符号表**没有** param_generic_names 字段（见 module_symbol_table.h:19-30）
    //     ⇒ 泛型形参名由 type_param_names 承担、param_types 记 TYPE_GENERIC_PARAM ✓
    if (fn->u.func.param_types && fn->u.func.pcnt > 0) {
        int pc = fn->u.func.pcnt;
        TypeKind* pts = (TypeKind*)malloc(sizeof(TypeKind) * pc);
        char** psn = (char**)malloc(sizeof(char*) * pc);
        if (pts && psn) {
            int degrade = 0;
            for (int i = 0; i < pc; i++) {
                TypeInfo* ti = fn->u.func.param_types[i];
                if (ast_type_param_name(NULL, fn, ti) != NULL) {
                    pts[i] = TYPE_GENERIC_PARAM;
                    psn[i] = NULL;
                    continue;
                }
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
// ============================================================================
// 依赖表获取（**带环保护**）
// ============================================================================
// 为什么必须防环：填充期间"取依赖模块的表"会触发对方也扫表+填充；而 get_shared 的记忆化
//   要等**本表填完**才生效 ⇒ `A use B` + `B use A` 会让 A 被当成"没见过"再建一张新表，
//   如此往复 ⇒ **无限递归** ✗（文本扫描链时代不会有这事：那时取依赖表发生在
//   scan_depth 内部，`scan_stack` 还在栈上，环会被当场判成"检测到循环依赖" ✓）
// 两道保险：① 路径已在"解析中"集合里 ⇒ 直接不取（真环）；② 深度上限（兜底，防路径口径不一致）✓
static const char* g_ast_dep_active[16];
static int g_ast_dep_n = 0;

static ModuleSymbolTable* ast_dep_table(ModuleSymbolTable* table, const char* path) {
    if (!table || !path || !path[0]) return NULL;
    if (g_ast_dep_n >= 16) return NULL;                 // ② 兜底：过深就停
    for (int i = 0; i < g_ast_dep_n; i++) {             // ① 真环：同一个依赖正在解析中
        if (g_ast_dep_active[i] && strcmp(g_ast_dep_active[i], path) == 0) return NULL;
    }
    g_ast_dep_active[g_ast_dep_n++] = path;
    ModuleSymbolTable* dep = module_symbol_table_get_shared(path, table->module_path);
    g_ast_dep_n--;
    return dep;
}

// ---- 本模块的 import 别名表（供 `export const X = base.Y` 这类初始值递归解析）----
//   为什么需要它：跨模块常量的类型必须**穿过中间模块**传播（test_export_const_type 整条用例
//   就是这个：B 里 `export const RE_INT = base.INT_VAL`，C 访问 B.RE_INT 时类型要是 int 不是 any ✓）
//   AST 侧 module_access 只记了别名（"base"）⇒ 得先用本模块的 import 语句把它解析成路径 ✓
// ⚠ 可重入：解析依赖模块会触发对方也跑填充器 ⇒ 必须 save/restore（见 provider 里的用法 ✓）
typedef struct {
    char* key;    // 别名（import "…" as X 的 X；裸包名 import 时就是包名 ✓）
    char* path;   // 待解析的路径/包名（交给 module_symbol_table_get_shared 解析 ✓）
} AstImportRef;
static AstImportRef* g_ast_imports = NULL;
static int g_ast_import_count = 0;

static void ast_imports_free(AstImportRef* arr, int n) {
    if (!arr) return;
    for (int i = 0; i < n; i++) {
        free(arr[i].key);
        free(arr[i].path);
    }
    free(arr);
}

static void ast_imports_build(Ast* root, AstImportRef** out, int* out_n) {
    *out = NULL;
    *out_n = 0;
    if (!root || root->kind != AST_BLOCK) return;
    AstImportRef* arr = NULL;
    int n = 0;
    for (int i = 0; i < root->u.block.count; i++) {
        Ast* st = root->u.block.items[i];
        if (!st || st->kind != AST_IMPORT) continue;
        // key = 该 import 在 `use` 语句里出现的模块名：别名优先 → 裸包名 → **从路径取基名**
        //   ⚠ 最后那条必须有：`import Win32` 这类裸名 import 在 AST 上可能只剩 file_path
        //     （alias / module_name 为空）⇒ 少了它 `use Win32.RegValueInfo` 就找不到依赖 ✗
        //     （实测 test_lenosys / test_native_module_resolve 的 sys_win.leno:18）
        const char* key = (st->u.import.alias && st->u.import.alias[0])
                              ? st->u.import.alias
                              : ((st->u.import.module_name && st->u.import.module_name[0])
                                     ? st->u.import.module_name : NULL);
        char keybuf[128];
        keybuf[0] = '\0';
        if (!key && st->u.import.file_path) {
            const char* fp = st->u.import.file_path;
            const char* b = fp;
            for (const char* q = fp; *q; q++) {
                if (*q == '/' || *q == '\\') b = q + 1;
            }
            const char* dot = strrchr(b, '.');
            size_t nlen = (dot && dot > b) ? (size_t)(dot - b) : strlen(b);
            if (nlen >= sizeof(keybuf)) nlen = sizeof(keybuf) - 1;
            memcpy(keybuf, b, nlen);
            keybuf[nlen] = '\0';
            if (keybuf[0]) key = keybuf;
        }
        const char* path = st->u.import.file_path ? st->u.import.file_path : st->u.import.module_name;
        if (!key || !path) continue;
        AstImportRef* grown = (AstImportRef*)realloc(arr, sizeof(AstImportRef) * (size_t)(n + 1));
        if (!grown) break;
        arr = grown;
        arr[n].key = strdup(key);
        arr[n].path = strdup(path);
        n++;
    }
    *out = arr;
    *out_n = n;
}

// `mod.MEMBER` 的类型：查**依赖模块**的符号表（与 scan_var.inc:297-336 同口径）：
//   变量 → enum 成员(可作常量 ⇒ int) → struct/face/cstruct/clib → 函数返回类型 ✓
//   解析不出 ⇒ NULL（调用方按 TYPE_ANY 处理，不比原来更差 ✓）
static TypeInfo* ast_infer_module_access(ModuleSymbolTable* table, Ast* init) {
    if (!table || !init) return NULL;
    const char* mname = init->u.module_access.module_name;
    const char* member = init->u.module_access.member_name;
    if (!mname || !member) return NULL;
    const char* path = NULL;
    for (int i = 0; i < g_ast_import_count; i++) {
        if (g_ast_imports[i].key && strcmp(g_ast_imports[i].key, mname) == 0) {
            path = g_ast_imports[i].path;
            break;
        }
    }
    if (!path) return NULL;
    ModuleSymbolTable* dep = ast_dep_table(table, path);
    if (!dep) return NULL;
    ModuleVarSymbol* v = module_symbol_table_find_var(dep, member);
    if (v) {
        if (v->type_info) return type_copy(v->type_info);
        if (v->type != TYPE_ANY) return type_new(v->type);
        return NULL;
    }
    if (module_symbol_table_find_enum(dep, member)) return type_new(TYPE_INT);
    ModuleStructSymbol* s = module_symbol_table_find_struct(dep, member);
    if (s) {
        TypeInfo* t = type_new(s->is_cstruct ? TYPE_CSTRUCT : TYPE_STRUCT);
        if (t) t->struct_name = strdup(s->name);
        return t;
    }
    if (module_symbol_table_find_face(dep, member)) {
        TypeInfo* t = type_new(TYPE_FACE);
        if (t) t->struct_name = strdup(member);
        return t;
    }
    if (module_symbol_table_find_clib(dep, member)) {
        TypeInfo* t = type_new(TYPE_CLIB);
        if (t) t->struct_name = strdup(member);
        return t;
    }
    ModuleFuncSymbol* f = module_symbol_table_find_func(dep, member);
    if (f) {
        if (f->return_type_info) return type_copy(f->return_type_info);
        if (f->return_type != TYPE_ANY) return type_new(f->return_type);
    }
    return NULL;
}

// 未标注变量（`var x = 1`）的类型推断：只看初值的**语法种类** ✓
//   为什么不复用语义的 infer_expr_type：本填充器跑在**语义之前**（get_shared 收口点，
//   被跨模块查询触发时那个模块可能根本没编译过）⇒ 只能做**声明级**推断 ✓
//   推断不出 ⇒ NULL（调用方按"没有信息"处理，不会比原来更差 ✓）
static TypeInfo* ast_infer_var_type(ModuleSymbolTable* table, Ast* init) {
    if (!init) return NULL;
    switch (init->kind) {
        case AST_NUM:    return type_new(init->u.num.is_float ? TYPE_FLOAT : TYPE_INT);
        case AST_STRING: return type_new(TYPE_STRING);
        case AST_BOOL:   return type_new(TYPE_BOOL);
        case AST_ARRAY: {
            // ⚠ 必须带**元素类型**：只给裸 Array 的话，跨模块 `mod.ARR[0]` 取不到元素类型 ⇒
            //   退化成 any ⇒「变量 'a0' 声明类型与初始化值类型不匹配」✗（test_var / test_nested_2d）
            //   元素类型取**首个**元素（与扫描链同一口径）；嵌套数组靠递归得到 Array[Array[int]] ✓
            TypeInfo* elem = NULL;
            if (init->u.array.items && init->u.array.count > 0) {
                elem = ast_infer_var_type(table, init->u.array.items[0]);
            }
            return type_array(elem);   // type_array 直接持有入参（见 type.c:26）⇒ 无需另拷 ✓
        }
        case AST_DICT: {
            TypeInfo* k = NULL;
            TypeInfo* v = NULL;
            if (init->u.dict.entries && init->u.dict.count > 0) {
                k = ast_infer_var_type(table, init->u.dict.entries[0].key);
                v = ast_infer_var_type(table, init->u.dict.entries[0].value);
            }
            return type_dict(k, v);
        }
        case AST_STRUCT_INIT: {
            if (!init->u.struct_init.struct_name) return NULL;
            TypeInfo* named = type_new(TYPE_STRUCT);
            if (!named) return NULL;
            named->struct_name = strdup(init->u.struct_init.struct_name);
            TypeInfo* r = ast_resolved_copy(table, named, 0);   // 别名/聚合 kind 修正 ✓
            type_free(named);
            return r;
        }
        case AST_VAR: {
            // 引用本模块已登记的变量/常量 ⇒ 直接用那张表的结论（与扫描链同口径 ✓）
            ModuleVarSymbol* v = module_symbol_table_find_var(table, init->u.var.name);
            if (!v) return NULL;
            if (v->type_info) return type_copy(v->type_info);
            if (v->type != TYPE_ANY) return type_new(v->type);
            return NULL;
        }
        // `mod.MEMBER`（含 `export const X = base.Y` 的跨模块常量）⇒ 递归查依赖模块 ✓
        case AST_MODULE_ACCESS: return ast_infer_module_access(table, init);
        case AST_UNARY:
            // 取负等一元运算：类型跟操作数（`export const NEG_VAL = -100` ✓ test_export_const_type）
            if (init->u.unary.operand) return ast_infer_var_type(table, init->u.unary.operand);
            return NULL;
        case AST_BINOP: {
            // 算术/拼接：两侧都判得出时取结论 —— 数值取更宽（float 优先）、字符串相加仍是 string ✓
            TypeInfo* l = ast_infer_var_type(table, init->u.binop.l);
            TypeInfo* r = ast_infer_var_type(table, init->u.binop.r);
            TypeKind lk = l ? l->kind : TYPE_ANY;
            TypeKind rk = r ? r->kind : TYPE_ANY;
            if (l) type_free(l);
            if (r) type_free(r);
            int ln = (lk == TYPE_INT || lk == TYPE_FLOAT);
            int rn = (rk == TYPE_INT || rk == TYPE_FLOAT);
            if (ln && rn) return type_new((lk == TYPE_FLOAT || rk == TYPE_FLOAT) ? TYPE_FLOAT : TYPE_INT);
            if (lk == TYPE_STRING && rk == TYPE_STRING) return type_new(TYPE_STRING);
            if (ln) return type_new(lk);
            if (rn) return type_new(rk);
            return NULL;
        }
        // 调用等 ⇒ 声明级判不出：留空，交给后续（不比原来更差 ✓）
        default: return NULL;
    }
}

static void ast_fill_one_var(ModuleSymbolTable* table, Ast* vd) {
    if (!vd || !vd->u.var_decl.name) return;
    ModuleVarSymbol* sym = module_symbol_table_find_var(table, vd->u.var_decl.name);
    TypeInfo* src = vd->u.var_decl.type;

    // ★ 判据（2026-10-01 实测查明，此前误判为"parser 不解析类型名"，实为写法区别）：
    //   有显式标注（`int x = 5` / `Array[int] a = ...`）⇒ parser 已给真实类型，用它 ✓
    //   TYPE_INFER(1) / TYPE_UNKNOWN(0) = **本来就没写标注**（`var x = 1`）⇒ 改从初值推断 ✓
    //   （此前只要见到 TYPE_INFER 就 return ⇒ 建表路径永远拿不到未标注变量 ✗）
    TypeInfo* resolved = NULL;
    if (src && src->kind != TYPE_INFER && src->kind != TYPE_UNKNOWN) {
        resolved = ast_resolved_copy(table, src, 0);
    } else {
        resolved = ast_infer_var_type(table, vd->u.var_decl.init);
    }

    if (!sym) {
        // ★ 建表路径（scan_var.inc 退役的前提）：AST 直接建条目 ✓
        //   add_var 内部**复制** type_info（见 sym_table_add.inc:378）⇒ 这里随后释放临时值 ✓
        module_symbol_table_add_var(table, vd->u.var_decl.name,
                                    resolved ? resolved->kind : TYPE_ANY,
                                    (resolved && resolved->struct_name) ? resolved->struct_name : NULL,
                                    vd->u.var_decl.is_const, resolved);
        if (resolved) type_free(resolved);
        return;
    }
    if (!resolved) return;   // 已有条目且本轮没算出更好的 ⇒ 保留扫描链结论 ✓

    // 退化保护（原判据保留）：AST 判成 TYPE_STRUCT 而表里已有更精确的结论
    //   ⇒ 说明该名字来自依赖模块、本表查不到 ⇒ 不覆盖 ✓
    if (resolved->kind == TYPE_STRUCT && sym->type_info && sym->type_info->kind != TYPE_STRUCT) {
        type_free(resolved);
        return;
    }
    sym->type = resolved->kind;
    sym->struct_name = resolved->struct_name ? strdup(resolved->struct_name) : NULL;
    sym->type_info = resolved;   // 所有权转移（旧值不释放：量小、先避 use-after-free ✓）
    sym->is_const = vd->u.var_decl.is_const;
}

// ---- 解构声明接管（`var (a, b) = ...` / `const {x, y} = ...`）----
//   扫描链在这里为**每个**名字建条目（scan_var.inc:65-75 那条重复路径）⇒ AST 侧同样逐个建 ✓
//   槽位类型 slot_types[i] 由 parser 解析好 ⇒ 走同一个 ast_resolved_copy（别名整体展开 ✓）
static void ast_fill_one_destruct(ModuleSymbolTable* table, Ast* dd) {
    if (!dd || !dd->u.destruct_decl.names) return;
    int n = dd->u.destruct_decl.slot_count;
    for (int i = 0; i < n; i++) {
        const char* nm = dd->u.destruct_decl.names[i];
        if (!nm || !nm[0]) continue;
        if (module_symbol_table_find_var(table, nm)) continue;   // 已有条目 ⇒ 交给覆盖路径 ✓
        TypeInfo* st = dd->u.destruct_decl.slot_types ? dd->u.destruct_decl.slot_types[i] : NULL;
        TypeInfo* r = st ? ast_resolved_copy(table, st, 0) : NULL;
        module_symbol_table_add_var(table, nm, r ? r->kind : TYPE_ANY,
                                    (r && r->struct_name) ? r->struct_name : NULL,
                                    dd->u.destruct_decl.is_const, r);
        if (r) type_free(r);
    }
}

// ============================================================================
// `use` / `import` 传导接管（S10 收尾 —— 文本扫描链**最后**一块活）
// ============================================================================
// 为什么需要它：本模块**自己的**声明由上面的 ast_fill_one_* 建表；而 `use dep.X` 的语义是
//   "把 dep 模块的类型 X 引进本模块符号表" —— 这条**跨模块引进**的链在 AST 侧原本没有对应物，
//   一直由 scan_pass2_init.inc 的文本扫描做（认 use 行 → 找 dep 表 → 逐个 add_*）。
//   实测（把扫描链整体停用）：只红 22 个，且**全部**是 use 链 / clib 跨模块 / cfunc 传导 /
//   enum 链 / alias 泛型 —— 即这一块就是扫描链剩下的全部价值 ✓
// 做法：parser 已把 `use` 解析成 AST_USE 节点 ⇒ 按节点做同样的事（不再需要文本匹配）✓
//   依赖路径用本模块 import 语句建的别名表（g_ast_imports，见 ast_infer_module_access）✓

// alias 底层类型依赖的**递归传导**（与 sym_table_import_alias.inc 的语义对齐）：
//   `use m.EventHandler`（= func(Event):bool）时，Event 也要一起进来，否则
//   "返回类型里的聚合名在本表查不到" ⇒ 消费方看到 struct/any ✗
static void ast_transmit_type_deps(ModuleSymbolTable* table, ModuleSymbolTable* dep,
                                   TypeInfo* ti, int depth) {
    if (!table || !dep || !ti || depth > 8) return;
    switch (ti->kind) {
        case TYPE_FUNCTION:
            if (ti->param_types) {
                for (int i = 0; i < ti->param_count; i++) {
                    ast_transmit_type_deps(table, dep, ti->param_types[i], depth + 1);
                }
            }
            if (ti->return_type) ast_transmit_type_deps(table, dep, ti->return_type, depth + 1);
            return;
        case TYPE_ARRAY:
        case TYPE_PTR_GENERIC:
            if (ti->element_type) ast_transmit_type_deps(table, dep, ti->element_type, depth + 1);
            return;
        case TYPE_DICT:
            if (ti->key_type) ast_transmit_type_deps(table, dep, ti->key_type, depth + 1);
            if (ti->value_type) ast_transmit_type_deps(table, dep, ti->value_type, depth + 1);
            return;
        default: break;
    }
    if (!ti->struct_name) return;
    const char* nm = ti->struct_name;
    // 本地已有 ⇒ 不动（与扫描链的 !find_* 判据一致 ✓）
    if (module_symbol_table_find_struct(table, nm) || module_symbol_table_find_face(table, nm) ||
        module_symbol_table_find_enum(table, nm) || module_symbol_table_find_alias(table, nm) ||
        module_symbol_table_find_clib(table, nm)) {
        return;
    }
    ModuleStructSymbol* ss = module_symbol_table_find_struct(dep, nm);
    if (ss) {
        module_symbol_table_add_struct(table, nm, ss->field_count, ss->fields, ss->method_count,
                                       ss->methods, ss->is_cstruct, ss->type_param_count,
                                       ss->type_param_names);
        module_symbol_table_set_struct_impls(table, nm, ss->impl_count, ss->impl_names);
        module_symbol_table_set_struct_consts(table, nm, ss->const_count, ss->const_names,
                                              ss->const_value_strs);
        return;
    }
    ModuleFaceSymbol* fs = module_symbol_table_find_face(dep, nm);
    if (fs) {
        module_symbol_table_add_face(table, nm, fs->method_count, fs->methods, fs->type_param_count);
        return;
    }
    ModuleEnumSymbol* es = module_symbol_table_find_enum(dep, nm);
    if (es) {
        module_symbol_table_add_enum(table, nm, es->member_count, es->member_names, es->member_values);
        return;
    }
    ModuleAliasSymbol* as = module_symbol_table_find_alias(dep, nm);
    if (as) {
        module_symbol_table_add_alias(table, nm, as->type_info ? type_copy(as->type_info) : NULL);
        if (as->type_info) ast_transmit_type_deps(table, dep, as->type_info, depth + 1);
        return;
    }
    ModuleClibSymbol* cs = module_symbol_table_find_clib(dep, nm);
    if (cs) module_symbol_table_add_clib(table, nm, cs->func_count, cs->funcs);
}

// `use m.X`（单名）：把 X 从 m 的表里引进本表 ✓
//   ⚠ 批量写法 `use m.(A, B, C)` 由 parser 拆成多个 AST_USE 节点 ⇒ 这里只处理单名 ✓
static void ast_fill_one_use(ModuleSymbolTable* table, Ast* us) {
    if (!table || !us) return;
    const char* mname = us->u.use.module_name;
    const char* sname = us->u.use.symbol_name;
    if (!mname || !sname || !sname[0]) return;
    const char* path = NULL;
    for (int i = 0; i < g_ast_import_count; i++) {
        if (g_ast_imports[i].key && strcmp(g_ast_imports[i].key, mname) == 0) {
            path = g_ast_imports[i].path;
            break;
        }
    }
    if (!path) return;                      // 原生模块（io / ffi / maths…）⇒ 无符号表可传导 ✓
    ModuleSymbolTable* dep = ast_dep_table(table, path);
    // 【临时探针·待删】
    fprintf(stderr, "[useprobe] %s: use %s.%s path=%s dep=%p same=%d\n",
            table->module_path ? table->module_path : "?", mname, sname, path,
            (void*)dep, (dep == table) ? 1 : 0);
    if (!dep || dep == table) return;
    // ★ 依赖登记（`.lenosymc` 失效判定要用；扫描链原先在文本路径里做）✓
    if (dep->module_path) module_symbol_table_add_dep(table, dep->module_path);

    ModuleStructSymbol* ss = module_symbol_table_find_struct(dep, sname);
    if (ss) {
        if (!module_symbol_table_find_struct(table, sname)) {
            module_symbol_table_add_struct(table, sname, ss->field_count, ss->fields,
                                           ss->method_count, ss->methods, ss->is_cstruct,
                                           ss->type_param_count, ss->type_param_names);
            module_symbol_table_set_struct_impls(table, sname, ss->impl_count, ss->impl_names);
            module_symbol_table_set_struct_consts(table, sname, ss->const_count, ss->const_names,
                                                  ss->const_value_strs);
        }
        return;
    }
    ModuleFaceSymbol* fs = module_symbol_table_find_face(dep, sname);
    if (fs) {
        if (!module_symbol_table_find_face(table, sname)) {
            module_symbol_table_add_face(table, sname, fs->method_count, fs->methods,
                                         fs->type_param_count);
        }
        return;
    }
    ModuleClibSymbol* cs = module_symbol_table_find_clib(dep, sname);
    if (cs) {
        if (!module_symbol_table_find_clib(table, sname)) {
            module_symbol_table_add_clib(table, sname, cs->func_count, cs->funcs);
        }
        return;
    }
    ModuleCfuncSymbol* cf = module_symbol_table_find_cfunc(dep, sname);
    if (cf) {
        if (!module_symbol_table_find_cfunc(table, sname)) {
            module_symbol_table_add_cfunc(table, sname, cf->param_count, cf->param_types,
                                          cf->param_element_types, cf->param_struct_names,
                                          cf->param_names, cf->return_type,
                                          cf->return_element_type, cf->return_struct_name);
        }
        return;
    }
    ModuleEnumSymbol* es = module_symbol_table_find_enum(dep, sname);
    if (es) {
        if (!module_symbol_table_find_enum(table, sname)) {
            module_symbol_table_add_enum(table, sname, es->member_count, es->member_names,
                                         es->member_values);
        }
        return;
    }
    ModuleAliasSymbol* as = module_symbol_table_find_alias(dep, sname);
    if (as) {
        if (!module_symbol_table_find_alias(table, sname)) {
            module_symbol_table_add_alias(table, sname, as->type_info ? type_copy(as->type_info) : NULL);
        }
        if (as->type_info) ast_transmit_type_deps(table, dep, as->type_info, 0);
    }
}

// ---- clib 类接管 ----
//   ModuleClibSymbol 与它的 funcs[] 全是扁平的（TypeKind + 名字 + 计数），
//   没有嵌套 TypeInfo 指针 ⇒ 可以整类换 ✓ 与 cfunc 同类。
//   ★ 建表路径尤其必要：扫描链对 `export clib X { … }` **只登记名字数组**
//     （scan_pass1.inc:179 推完 clib_names 就 continue），**从不建 clib 条目** ⇒
//     依赖符号表的新路径（AST 的类型分类、语义侧 `use m.X` 的查找、clib 的 use 传导）
//     全都找不到它 ✗（实测 test_sdl_capture / test_plane_war_headless 的 `use fnt.sdl3_ttf`）
static void ast_fill_one_clib(ModuleSymbolTable* table, Ast* cd) {
    if (!cd || !cd->u.clib_def.name || !cd->u.clib_def.func_names) return;
    ModuleClibSymbol* sym = module_symbol_table_find_clib(table, cd->u.clib_def.name);
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
    if (sym) {
        sym->func_count = fc;
        sym->funcs = fs;      // 旧数组不释放（同前：量小、先避 use-after-free ✓）
        return;
    }
    // ★ 建表：add_clib 内部深拷贝（且要求 func_count > 0）⇒ 随后释放我们造的临时数组 ✓
    if (fc > 0) {
        module_symbol_table_add_clib(table, cd->u.clib_def.name, fc, fs);
        for (int i = 0; i < fc; i++) {
            free(fs[i].name);
            free(fs[i].return_struct_name);
            if (fs[i].param_struct_names) {
                for (int k = 0; k < fs[i].param_count; k++) free(fs[i].param_struct_names[k]);
                free(fs[i].param_struct_names);
            }
            free(fs[i].param_types);
            free(fs[i].param_element_types);
        }
    }
    free(fs);
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
    int pc = cf->u.cfunc_decl.param_count;
    if (pc < 0) return;

    // ---- 先把 AST 映射成"符号表口径"的扁平数组（建表与覆盖共用同一套映射 ✓）----
    TypeKind* pts = NULL;
    TypeKind* pets = NULL;
    char** psn = NULL;
    char** pnm = NULL;
    int have_params = (pc == 0);
    if (pc > 0 && cf->u.cfunc_decl.param_types) {
        pts = (TypeKind*)malloc(sizeof(TypeKind) * pc);
        pets = (TypeKind*)malloc(sizeof(TypeKind) * pc);
        psn = (char**)malloc(sizeof(char*) * pc);
        pnm = (char**)malloc(sizeof(char*) * pc);
        if (pts && pets && psn && pnm) {
            for (int i = 0; i < pc; i++) {
                TypeInfo* ti = cf->u.cfunc_decl.param_types[i];
                pts[i] = ast_kind_of(table, ti);
                pets[i] = TYPE_PTR;
                psn[i] = (ti && ti->struct_name) ? strdup(ti->struct_name) : NULL;
                pnm[i] = (cf->u.cfunc_decl.param_names && cf->u.cfunc_decl.param_names[i])
                             ? strdup(cf->u.cfunc_decl.param_names[i]) : NULL;
            }
            have_params = 1;
        } else {
            free(pts); free(pets); free(psn); free(pnm);
            pts = NULL; pets = NULL; psn = NULL; pnm = NULL;
        }
    }
    int have_ret = (cf->u.cfunc_decl.return_type != NULL);
    TypeKind rt = TYPE_ANY;
    char* rsn = NULL;
    if (have_ret) {
        rt = ast_kind_of(table, cf->u.cfunc_decl.return_type);
        rsn = cf->u.cfunc_decl.return_type->struct_name
                  ? strdup(cf->u.cfunc_decl.return_type->struct_name) : NULL;
    }

    if (sym) {
        // 覆盖：旧数组不释放（同前：符号表进程内长存活、量小，先避 use-after-free ✓）
        if (have_params) {
            sym->param_count = pc;
            sym->param_types = pts;
            sym->param_element_types = pets;
            sym->param_struct_names = psn;
            sym->param_names = pnm;
        }
        if (have_ret) {
            sym->return_type = rt;
            sym->return_element_type = TYPE_PTR;
            sym->return_struct_name = rsn;
        }
        return;
    }

    // ★ 建表路径（scan_pass1 的非 export cfunc 分支退役的前提）：
    //   `export cfunc X(...)` 的条目原先**只在扫描链里建**（AST 侧只做覆盖）⇒ 停用扫描链后
    //   跨模块 `use m.X`、以及"把 cfunc 当参数类型"的用法全部失联
    //   （实测 test_cfunc_use / test_cfunc_chain / test_cfunc_ffi / test_cfunc_sub ✓）
    //   add_cfunc 内部深拷贝 ⇒ 随后释放临时数组 ✓
    module_symbol_table_add_cfunc(table, cf->u.cfunc_decl.name, pc, pts, pets, psn, pnm,
                                  rt, TYPE_PTR, rsn);
    if (pts) {
        for (int i = 0; i < pc; i++) {
            free(psn[i]);
            free(pnm[i]);
        }
        free(pts); free(pets); free(psn); free(pnm);
    }
    free(rsn);
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
    // 本模块 import 别名表（供 `export const X = base.Y` 递归解析依赖模块 ✓）
    //   ⚠ save/restore 必须成对：下面解析依赖会触发**对方的**填充器 ⇒ 不能覆盖外层在用的表 ✗
    AstImportRef* saved_imports = g_ast_imports;
    int saved_import_count = g_ast_import_count;
    ast_imports_build(root, &g_ast_imports, &g_ast_import_count);
    if (root && root->kind == AST_BLOCK) {
        for (int i = 0; i < root->u.block.count; i++) {
            Ast* st = root->u.block.items[i];
            if (!st) continue;
            Ast* d = (st->kind == AST_EXPORT && st->u.export.decl) ? st->u.export.decl : st;
            // ⚠ 批量 `use m.(A, B, C)`（**>1 个**）会被 parser 打成 **AST_BLOCK**，里面才是
            //   一串 AST_USE（见 parser_module.c:565 —— 单名才直接返回 AST_USE）⇒
            //   这里必须展开一层，否则跨行批量 use 整条传导都丢掉
            //   （实测 test_scanwrap 的 `use base.(Base,\n Other)` 就是这个形状 ✗）
            if (d->kind == AST_BLOCK) {
                for (int k = 0; k < d->u.block.count; k++) {
                    Ast* sub = d->u.block.items[k];
                    if (sub && sub->kind == AST_USE) ast_fill_one_use(table, sub);
                }
                continue;
            }
            if (d->kind == AST_FUNC_DEF) ast_fill_one_func(table, d);
            else if (d->kind == AST_ALIAS) ast_fill_one_alias(table, d);
            else if (d->kind == AST_CFUNC_DECL) ast_fill_one_cfunc(table, d);
            else if (d->kind == AST_ENUM_DEF) ast_fill_one_enum(table, d);
            else if (d->kind == AST_STRUCT_DEF) ast_fill_one_struct_meta(table, d);
            else if (d->kind == AST_FACE_DEF) ast_fill_one_face_meta(table, d);
            else if (d->kind == AST_CLIB_DEF) ast_fill_one_clib(table, d);
            else if (d->kind == AST_CSTRUCT_DEF) ast_fill_one_cstruct(table, d);
            else if (d->kind == AST_VAR_DECL) ast_fill_one_var(table, d);
            else if (d->kind == AST_DESTRUCT_DECL) ast_fill_one_destruct(table, d);
            else if (d->kind == AST_USE) ast_fill_one_use(table, d);   // ★ 跨模块传导（S10 收尾）
            // 注（2026-10-01）：var 类现在**既建表也覆盖**（建表路径见 ast_fill_one_var）。
            //   此前那套"先对齐表示层"的结论**已作废** —— 障碍不在表示层，而在
            //   旧实现开头就是 `if (!sym) return;`：停用 scan_var 后**没有任何人建 var 条目**，
            //   于是"接管"变成了"什么都不做"，才表现为 396/10 / 399/7 那些红 ✓
        }
    }
    ast_imports_free(g_ast_imports, g_ast_import_count);
    g_ast_imports = saved_imports;
    g_ast_import_count = saved_import_count;
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
