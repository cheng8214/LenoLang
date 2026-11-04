#include "semantic_internal.h"
#include "include/leno_hash.h"

// 模块级变量初始化顺序检查（定义在 semantic_visit_ast.c，2026-10-23）
void mv_check_top(Semantic* s, Ast* block);

// 注解表清空（定义在 semantic_annotations.c，2026-10-31）
//   ⚠ 表是**进程级**的静态数组 ⇒ 这次分析不清，上一个文件的 @optkeys 会串到下一个 ✗
extern void annos_reset_all(void);

// ============================================================================
// 函数表哈希表实现
// ============================================================================

#define FUNC_TABLE_INITIAL_CAPACITY 64    // 初始容量（必须是2的幂）
#define FUNC_TABLE_MAX_LOAD 0.75           // 最大负载因子

// 初始化函数表
void func_table_init(FuncTable* table) {
    table->capacity = FUNC_TABLE_INITIAL_CAPACITY;
    table->count = 0;
    table->entries = (FuncEntry**)calloc(table->capacity, sizeof(FuncEntry*));
}

// 释放函数表
void func_table_free(FuncTable* table) {
    if (!table->entries) return;
    
    for (int i = 0; i < table->capacity; i++) {
        FuncEntry* entry = table->entries[i];
        while (entry) {
            FuncEntry* next = entry->next;
            free(entry->name);
            // 不释放 entry->func，因为大部分指向 AST 树中的节点，由 ast_free 统一释放
            // 占位符节点的泄漏很小，可接受
            free(entry);
            entry = next;
        }
    }
    free(table->entries);
    table->entries = NULL;
    table->capacity = 0;
    table->count = 0;
}

// 扩容哈希表
static void func_table_resize(FuncTable* table) {
    int old_capacity = table->capacity;
    FuncEntry** old_entries = table->entries;
    
    // 新容量翻倍
    int new_capacity = old_capacity * 2;
    FuncEntry** new_entries = (FuncEntry**)calloc(new_capacity, sizeof(FuncEntry*));
    if (!new_entries) return;  // 扩容失败，继续使用旧表
    
    // 重新哈希所有条目
    for (int i = 0; i < old_capacity; i++) {
        FuncEntry* entry = old_entries[i];
        while (entry) {
            FuncEntry* next = entry->next;
            // 计算新索引
            uint32_t hash = leno_fnv1a(entry->name);
            int index = hash & (new_capacity - 1);
            // 插入到新表
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    
    free(old_entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
}

// 添加函数到表（返回1成功，0失败）
int func_table_add(FuncTable* table, const char* name, Ast* func) {
    if (!table->entries) {
        func_table_init(table);
    }
    
    // 检查是否需要扩容
    if (table->count >= table->capacity * FUNC_TABLE_MAX_LOAD) {
        func_table_resize(table);
    }
    
    // 计算哈希索引
    uint32_t hash = leno_fnv1a(name);
    int index = hash & (table->capacity - 1);
    
    // 检查是否已存在
    FuncEntry* entry = table->entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            // 已存在，更新函数指针
            entry->func = func;
            return 1;
        }
        entry = entry->next;
    }
    
    // 创建新条目
    FuncEntry* new_entry = (FuncEntry*)malloc(sizeof(FuncEntry));
    if (!new_entry) return 0;
    
    new_entry->name = strdup(name);
    new_entry->func = func;
    new_entry->next = table->entries[index];
    table->entries[index] = new_entry;
    table->count++;
    
    return 1;
}

// 查找函数（O(1)）
Ast* func_table_find(FuncTable* table, const char* name) {
    if (!table->entries || table->count == 0) return NULL;
    
    uint32_t hash = leno_fnv1a(name);
    int index = hash & (table->capacity - 1);
    
    FuncEntry* entry = table->entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->func;
        }
        entry = entry->next;
    }
    return NULL;
}

// ============================================================================
// 公共 API
// ============================================================================

void semantic_init(Semantic* s, Ast* root) {
    s->root = root;
    s->current = NULL;
    s->root_scope = NULL;
    func_table_init(&s->func_table);
    s->current_func = NULL;
    s->func_stack_depth = 0;
    s->local_index = 0;
    // ★ 槽位回收的两个记账数组**必须显式清零**：本函数不是整体 memset（Semantic 是 main.c
    //   的栈变量，见上面泛型字段那段同样的教训）⇒ 漏清就是读栈上垃圾：
    //   func_pinned 为垃圾 ⇒ 回退下界错 ⇒ 槽位被误复用（闭包读到别的变量的值）；
    //   func_max_index 为垃圾 ⇒ func->local_count 取到垃圾值。
    for (int _i = 0; _i < 64; _i++) {
        s->func_max_index[_i] = 0;
        s->func_pinned[_i] = 0;
    }
    s->imported_module_count = 0;
    s->is_module = 0;
    s->is_lsp_mode = 0;
    s->in_clib = 0;
    // ★ 2026-11-04：同下面那几条 —— Semantic 是**栈变量**，不 memset ⇒ 漏清就是垃圾值。
    //   漏了它会让**同文件 extend** 的 pri 访问被误判成"跨模块"而报错（实测 ✗）
    s->in_cross_module_extend = 0;
    s->has_module_load_failure = 0;
    s->in_main_func = 0;
    s->for_in_lhs = NULL;   // ★ Q3：for 头 `x in y` 的定向提示用（漏清就是读栈上垃圾）
    s->for_in_lhs_name = NULL;
    // ★ US1：同上 —— 本函数**不是**整体 memset ⇒ 漏清就是读栈上垃圾。
    //   实测（2026-10-12）：漏了这一行时，**模块模式**下这个字段是垃圾非零 ⇒
    //   类型归一化整段被跳过 ⇒ 模块里写 `m.Pt` 直接报"未定义的类型: Pt"✗（入口脚本模式恰好是 0）
    s->in_type_prepass = 0;
    // ⚠ 泛型需求推断（B 方案）的三个字段**必须**在这里显式清零：本函数**不是**整体 memset
    //   （Semantic 是 main.c 的栈变量）⇒ 漏了就是读栈上垃圾 ⇒ 一调 native 检查点就段错误（实测 ✗）
    s->cur_generic_func = NULL;
    s->cur_generic_struct = NULL;
    s->cur_struct_method_name = NULL;
    s->reqs = NULL;
    s->req_count = 0;
    s->req_capacity = 0;
    s->pending = NULL;
    s->pending_count = 0;
    s->pending_capacity = 0;
    // ★ 2026-10-16：多类型 case 的窄化上下文 —— 同上，**漏清就是读栈上垃圾**
    //   （实测：漏了这行 ⇒ union_narrow_count 是垃圾 7688 ⇒ 检查函数一进去就乱翻数组 ✗）
    s->union_narrow_count = 0;
    memset(s->union_narrow, 0, sizeof(s->union_narrow));
    s->ast_depth = 0;   // ★ 递归深度守护（同上面的教训：必须显式清零）
    s->owned_placeholders = NULL;
    s->owned_placeholder_count = 0;
    s->owned_placeholder_capacity = 0;

    // 初始化 imported_modules 数组
    memset(s->imported_modules, 0, sizeof(s->imported_modules));
}

// ============================================================================
// extend 合并（2026-11-02，设计见 docs/struct_extend_设计草案.md）
// ----------------------------------------------------------------------------
// 把顶层 block 里的 `extend X { func ... }` 的方法**追加进目标 struct 的 methods 数组**
// ⇒ 调用解析 / 字段可见性 / codegen 全部自动复用"写在 struct 里"的那套逻辑 ✓
//
// ⚠ 必须在"预注册 struct"之前调用：下面那些流程按 `method_count` 一次性分配方法表，
//   合并晚了多出来的方法会被截断，表现为"类型 'struct X' 没有方法 Y" ✗（实测踩过）
// ⚠ 一期只支持**同文件**：跨模块 extend 要把方法挂到别的模块的 class 上，属于另一套
//   运行时机制（设计文档 6.3），这里直接报错而不是静默忽略。
// ============================================================================
static void extend_merge_into_block(Semantic* s, Ast* block) {
    if (!block || block->kind != AST_BLOCK) return;
    AstList* list = &block->u.block;
    for (int i = 0; i < list->count; i++) {
        Ast* ext = list->items[i];
        if (!ext || ext->kind != AST_EXTEND_DEF) continue;

        if (ext->u.extend_def.module_name) {
            // ★ 2026-11-02：跨模块（`extend player.Player`）—— 这里**既不合并不校验**：
            //   · 不合并：方法体留在 extend 节点上，由 codegen 生成 `OP_STRUCT_EXTEND`，
            //     运行期挂到目标 class 上（按名字查表，不需要编译期索引 ✓）
            //   · 不校验：本函数在 `semantic_analyze` **最开头**调用，那时
            //     `s->imported_modules` 还是空的（import 在 visit 阶段才登记）
            //     ⇒ 在这里查"模块是否导入"必然误报"没有导入"（实测踩过 ✗）
            //   校验放在 visit 到该节点时做（那时 import 已处理完）。
            (void)s;
            continue;
        }

        Ast* target = NULL;
        // 目标名**存在但不是 struct** 时，给一句点名的报错（enum/face/cstruct 都没有方法体）
        const char* not_struct_kind = NULL;
        for (int j = 0; j < list->count; j++) {
            Ast* sd = list->items[j];
            if (!sd) continue;
            // ★ 2026-11-04：`export struct` 包在 AST_EXPORT 里 —— 此前只认裸 AST_STRUCT_DEF，
            //   于是模块里最常见的写法 `export struct Player` + `extend Player` 直接报
            //   「目标类型在本文件里没有定义」（明明就在上面），方法全部不生效 ✗（实测）。
            Ast* decl = (sd->kind == AST_EXPORT && sd->u.export.decl) ? sd->u.export.decl : sd;
            if (!decl) continue;
            if (decl->kind == AST_STRUCT_DEF && decl->u.struct_def.name &&
                strcmp(decl->u.struct_def.name, ext->u.extend_def.type_name) == 0) {
                target = decl;
                break;
            }
            if (!not_struct_kind) {
                if (decl->kind == AST_ENUM_DEF && decl->u.enum_def.name &&
                    strcmp(decl->u.enum_def.name, ext->u.extend_def.type_name) == 0) {
                    not_struct_kind = "enum（枚举）";
                } else if (decl->kind == AST_FACE_DEF && decl->u.face_def.name &&
                           strcmp(decl->u.face_def.name, ext->u.extend_def.type_name) == 0) {
                    not_struct_kind = "face（接口）";
                } else if (decl->kind == AST_CSTRUCT_DEF && decl->u.cstruct_def.name &&
                           strcmp(decl->u.cstruct_def.name, ext->u.extend_def.type_name) == 0) {
                    not_struct_kind = "cstruct";
                }
            }
        }
        if (!target) {
            if (not_struct_kind) {
                error_add_at_fmt(ERR_SEMANTIC, ext->u.extend_def.line, ext->column,
                    "extend 的目标 '%s' 是 %s —— extend 只能给 struct 加方法；"
                    "enum/face/cstruct 没有方法体，请把方法写进一个 struct",
                    ext->u.extend_def.type_name, not_struct_kind);
            } else {
                error_add_at_fmt(ERR_SEMANTIC, ext->u.extend_def.line, 0,
                    "extend 的目标类型 '%s' 在本文件里没有定义", ext->u.extend_def.type_name);
            }
            continue;
        }

        for (int k = 0; k < ext->u.extend_def.method_count; k++) {
            Ast* fn = ext->u.extend_def.methods[k];
            if (!fn || fn->kind != AST_FUNC_DEF || !fn->u.func.name) continue;

            // 重名：与 struct 内已有方法、或先前 extend 进来的方法撞名 ⇒ 报错。
            //   不静默覆盖，否则"我改的方法怎么没生效"几乎查不出来 ✓
            Ast* dup = NULL;
            for (int m = 0; m < target->u.struct_def.method_count; m++) {
                Ast* ex = target->u.struct_def.methods[m];
                if (ex && ex->kind == AST_FUNC_DEF && ex->u.func.name &&
                    strcmp(ex->u.func.name, fn->u.func.name) == 0) { dup = ex; break; }
            }
            if (dup) {
                error_add_at_fmt(ERR_SEMANTIC, fn->line, 0,
                    "extend 的方法 '%s' 重复定义（%s 里第 %d 行已有同名方法）",
                    fn->u.func.name, target->u.struct_def.name, dup->line);
                continue;
            }

            Ast** grown = (Ast**)realloc(target->u.struct_def.methods,
                                         sizeof(Ast*) * (target->u.struct_def.method_count + 1));
            if (!grown) continue;
            target->u.struct_def.methods = grown;
            target->u.struct_def.methods[target->u.struct_def.method_count++] = fn;
            ext->u.extend_def.methods[k] = NULL;    // 所有权转移 ⇒ 不能再由 extend 节点释放
        }
        // 转移完毕：清空，避免 ast_free 二次释放（见 ast.c 的 AST_EXTEND_DEF 分支）
        free(ext->u.extend_def.methods);
        ext->u.extend_def.methods = NULL;
        ext->u.extend_def.method_count = 0;
    }
}

// ============================================================================
// 跨模块 extend 重名：本文件 vs 本文件 / 已导入模块（2026-11-03，2026-11-04 补两类）
// ----------------------------------------------------------------------------
// 与 semantic_register_extends_from_module 那条互补（那条管 **import 与 import 之间**）。
// 这里管本文件自己写的 `extend m.X { func f }` 撞名，三种来源都查：
//   ① 本文件的**另一个** extend 块（同一类型、同一方法）
//   ② 导入模块里 **struct 自带的方法**（同名）—— 运行期方法查找优先命中 struct 自带的
//   ③ 导入模块里的 **extend**（同名）—— 先注册的赢
// 这三种此前全都完全静默，而且本文件/后写的那份**永远不生效**：
//   · 编译期：import 的占位符/前一个块先进 func_table，后来的被 `func_table_find` 跳过；
//   · 运行期：OP_STRUCT_EXTEND 按初始化顺序**先到先占**，方法查找取**第一个**同名项。
// 实测：两个块写 `f(): 1 / f(): 2` ⇒ 输出 1；扩展写 `f(): 1`、struct 自带 `f(): 9` ⇒ 输出 9。
// 做法：所有 import 都处理完（visit_list 之后）再扫顶层跨模块 extend ⇒ 与源码顺序无关 ✓
// ============================================================================
static void extend_check_local_dups(Semantic* s, Ast* root) {
    if (!s || !root || root->kind != AST_BLOCK) return;
    AstList* list = &root->u.block;
    for (int i = 0; i < list->count; i++) {
        Ast* ext = list->items[i];
        if (!ext || ext->kind != AST_EXTEND_DEF) continue;
        if (!ext->u.extend_def.module_name) continue;   // 同文件 extend 由 extend_merge_into_block 管
        const char* type_name = ext->u.extend_def.type_name;
        if (!type_name || ext->u.extend_def.method_count <= 0) continue;
        for (int k = 0; k < ext->u.extend_def.method_count; k++) {
            Ast* fn = ext->u.extend_def.methods[k];
            if (!fn || fn->kind != AST_FUNC_DEF || !fn->u.func.name) continue;
            char key[256];
            snprintf(key, sizeof(key), "%s::%s", type_name, fn->u.func.name);

            // ① 本文件里**后面的** module-qualified extend 块（i+1 起，保证只报一次）
            for (int j = i + 1; j < list->count; j++) {
                Ast* other = list->items[j];
                if (!other || other->kind != AST_EXTEND_DEF) continue;
                if (!other->u.extend_def.module_name) continue;
                if (!other->u.extend_def.type_name ||
                    strcmp(other->u.extend_def.type_name, type_name) != 0) continue;
                for (int k2 = 0; k2 < other->u.extend_def.method_count; k2++) {
                    Ast* fn2 = other->u.extend_def.methods[k2];
                    if (!fn2 || fn2->kind != AST_FUNC_DEF || !fn2->u.func.name) continue;
                    if (strcmp(fn2->u.func.name, fn->u.func.name) != 0) continue;
                    error_add_at_fmt(ERR_SEMANTIC, fn2->line, fn2->column,
                        "extend 方法 '%s' 在本文件里重复定义（第 %d 行已经写过）—— "
                        "运行期只有先注册的那个生效、后写的会被静默忽略；请只保留一处",
                        key, fn->line);
                }
            }

            for (int im = 0; im < s->imported_module_count; im++) {
                ModuleSymbolTable* t = s->imported_modules[im].sym_table;
                if (!t) continue;
                const char* mod_file = s->imported_modules[im].file_path
                                           ? s->imported_modules[im].file_path : "?";

                // ② 导入模块里 struct **自带**的方法（不是 extends 段）
                ModuleStructSymbol* ss = module_symbol_table_find_struct(t, type_name);
                if (ss && ss->methods) {
                    int hit = 0;
                    for (int mi = 0; mi < ss->method_count; mi++) {
                        if (ss->methods[mi].name && strcmp(ss->methods[mi].name, key) == 0) {
                            hit = 1;
                            break;
                        }
                    }
                    if (hit) {
                        error_add_at_fmt(ERR_SEMANTIC, fn->line, fn->column,
                            "extend 方法 '%s' 与导入模块 %s 里 struct 自带的方法同名 —— "
                            "方法查找优先命中 struct 自带的，这个扩展方法永远不会被调用；"
                            "请改名，或把实现写回 struct 定义里",
                            key, mod_file);
                    }
                }

                // ③ 导入模块里的 extends 段（原有用例）
                for (int ei = 0; ei < t->extend_count; ei++) {
                    ModuleExtendSymbol* ex = &t->extends[ei];
                    if (!ex->type_name || strcmp(ex->type_name, type_name) != 0) continue;
                    for (int mi = 0; mi < ex->method_count; mi++) {
                        ModuleStructMethod* m = &ex->methods[mi];
                        if (!m->name || strcmp(m->name, fn->u.func.name) != 0) continue;
                        error_add_at_fmt(ERR_SEMANTIC, fn->line, fn->column,
                            "extend 方法 '%s::%s' 在导入模块 %s 里也有定义 —— "
                            "运行期先注册的那个生效、本文件这份会被静默忽略；请只保留一处",
                            ex->type_name, fn->u.func.name, mod_file);
                        break;
                    }
                }
            }
        }
    }
}

void semantic_analyze(Semantic* s, Ast* ast) {
    annos_reset_all();          // 注解（@optkeys）表每个分析单元清一次 ✓
    // import 别名同理：表是进程级静态，而别名是编译单元级声明 ⇒ 不清空会把上一个
    //   单元的 `import "strings" as st` 带进来（见 native.h 的 native_clear_module_aliases）
    native_clear_module_aliases();
    // 创建全局作用域
    s->current = scope_new(NULL, 0);
    s->root_scope = s->current;

    // 注册所有模块的 native 函数元信息
    native_register_all_module_metas();
    
    // 注册所有实例方法的元信息（用于编译期参数检查）
    native_register_all_instance_method_metas();

    // 将 native 函数注册到作用域
    int count;
    const NativeFunctionMeta* builtins = native_get_all_functions(&count);
    for (int i = 0; i < count; i++) {
        scope_define(s->current, builtins[i].name, SYM_NATIVE);
    }

    // ★ 先把 extend 的方法并进目标 struct（必须在下面的预注册之前，见函数注释）
    extend_merge_into_block(s, ast);

    // 预注册所有 struct/face 定义（支持前向引用）
    if (ast && ast->kind == AST_BLOCK) {
        AstList* list = &ast->u.block;
        for (int i = 0; i < list->count; i++) {
            Ast* stmt = list->items[i];
            if (stmt->kind == AST_STRUCT_DEF) {
                // 预注册 struct 类型符号
                Symbol* sym = scope_define(s->current, stmt->u.struct_def.name, SYM_STRUCT);
                if (sym) {
                    sym->type = type_new(TYPE_STRUCT);
                    sym->type->struct_name = strdup(stmt->u.struct_def.name);
                    // 预注册字段信息，确保前向引用时能解析字段索引
                    sym->struct_field_count = stmt->u.struct_def.field_count;
                    sym->struct_field_names = (char**)malloc(sizeof(char*) * stmt->u.struct_def.field_count);
                    sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * stmt->u.struct_def.field_count);
                    for (int j = 0; j < stmt->u.struct_def.field_count; j++) {
                        // B5：重复字段名检查——此前静默取前者，后者被丢弃
                        for (int k = 0; k < j; k++) {
                            if (stmt->u.struct_def.field_names[j] && stmt->u.struct_def.field_names[k] &&
                                strcmp(stmt->u.struct_def.field_names[j], stmt->u.struct_def.field_names[k]) == 0) {
                                error_add_at_fmt(ERR_SEMANTIC, stmt->line, stmt->column,
                                         "struct '%s' 存在重复字段 '%s'（字段 %d 与字段 %d 重名）",
                                         stmt->u.struct_def.name ? stmt->u.struct_def.name : "?",
                                         stmt->u.struct_def.field_names[j], k + 1, j + 1);
                            }
                        }
                        sym->struct_field_names[j] = strdup(stmt->u.struct_def.field_names[j]);
                        sym->struct_field_types[j] = type_copy(stmt->u.struct_def.field_types[j]);
                    }
                    // 泛型形参名也要预注册：前向定义的泛型 struct（定义在使用点之后）
                    // 在函数体检查时，符号还是这份预注册的残缺版；infer_field_type
                    // 靠 sym->struct_type_params + 实例泛型实参做字段类型替换
                    // （如 Array[T] items → Array[int] items），缺了它元素方法调用
                    // 会误报"期望类型: struct"
                    if (stmt->u.struct_def.type_param_count > 0 && stmt->u.struct_def.type_params) {
                        sym->struct_type_param_count = stmt->u.struct_def.type_param_count;
                        sym->struct_type_params = (char**)malloc(sizeof(char*) * stmt->u.struct_def.type_param_count);
                        for (int j = 0; j < stmt->u.struct_def.type_param_count; j++) {
                            sym->struct_type_params[j] = strdup(stmt->u.struct_def.type_params[j]);
                        }
                    }
                }
                // 同时注册到全局 struct 定义表，确保 type_is_compatible 能找到
                if (stmt->u.struct_def.name && !struct_def_find(stmt->u.struct_def.name)) {
                    ObjStructDef* early_def = struct_def_new(stmt->u.struct_def.name, stmt->u.struct_def.field_count, stmt->u.struct_def.method_count);
                    if (early_def) {
                        early_def->impl_count = stmt->u.struct_def.impl_count;
                        early_def->type_param_count = stmt->u.struct_def.type_param_count;
                        if (early_def->type_param_count > 0 && stmt->u.struct_def.type_params) {
                            early_def->type_param_names = (char**)malloc(sizeof(char*) * early_def->type_param_count);
                            for (int j = 0; j < early_def->type_param_count; j++) {
                                early_def->type_param_names[j] = strdup(stmt->u.struct_def.type_params[j]);
                            }
                        }
                        if (early_def->impl_count > 0) {
                            early_def->impl_names = (char**)malloc(sizeof(char*) * early_def->impl_count);
                            for (int j = 0; j < early_def->impl_count; j++) {
                                early_def->impl_names[j] = strdup(stmt->u.struct_def.impl_names[j]);
                            }
                        }
                        for (int j = 0; j < stmt->u.struct_def.method_count; j++) {
                            early_def->methods[j].name = strdup(stmt->u.struct_def.methods[j]->u.func.name);
                        }
                        struct_def_register(early_def);
                    }
                }
            } else if (stmt->kind == AST_FACE_DEF) {
                // 预注册 face 类型符号
                Symbol* sym = scope_define(s->current, stmt->u.face_def.name, SYM_TYPE);
                if (sym) {
                    sym->type = type_new(TYPE_FACE);
                    sym->type->struct_name = strdup(stmt->u.face_def.name);
                }
                // 同时注册到全局 face 定义表
                if (stmt->u.face_def.name && !face_def_find(stmt->u.face_def.name)) {
                    ObjFaceDef* early_fdef = face_def_new(stmt->u.face_def.name, stmt->u.face_def.method_count);
                    if (early_fdef) {
                        face_def_set_type_params(early_fdef, stmt->u.face_def.type_param_count,
                            stmt->u.face_def.type_params, stmt->u.face_def.type_param_constraints);
                        for (int j = 0; j < stmt->u.face_def.method_count; j++) {
                            early_fdef->methods[j].name = strdup(stmt->u.face_def.method_names[j]);
                            early_fdef->methods[j].param_count = stmt->u.face_def.method_param_counts[j];
                            if (stmt->u.face_def.method_return_types && stmt->u.face_def.method_return_types[j]) {
                                early_fdef->methods[j].return_type = type_copy(stmt->u.face_def.method_return_types[j]);
                                resolve_generic_in_type(early_fdef->methods[j].return_type,
                                    stmt->u.face_def.type_params, stmt->u.face_def.type_param_constraints,
                                    stmt->u.face_def.type_param_count);
                            } else {
                                early_fdef->methods[j].return_type = NULL;
                            }
                        }
                        face_def_register(early_fdef);
                    }
                }
            } else if (stmt->kind == AST_CSTRUCT_DEF) {
                // 预注册 cstruct 类型符号
                Symbol* sym = scope_define(s->current, stmt->u.cstruct_def.name, SYM_CSTRUCT);
                if (sym) {
                    sym->type = type_new(TYPE_CSTRUCT);
                    sym->type->struct_name = strdup(stmt->u.cstruct_def.name);
                }
            } else if (stmt->kind == AST_ENUM_DEF) {
                // 预注册 enum 类型符号 —— 四种类型（struct/cstruct/face/enum）里唯一
                // 此前没进预注册的。缺了它，跨种类同名的报错会落在**先声明**的那条上、
                // 且文案指向源码里更靠后的另一种类型：实测 `enum E1`(37 行) + `face E1`
                // (40 行) 报在 37 行说"已经定义为 face" —— 定位与归因都错位。
                // kind 沿用主阶段将采用的取值（SYM_MODULE/SYM_GLOBAL），保证注册后
                // 行为逐位不变：这里只提前占位，不改语义。
                SymKind ekind = (s->current && s->current->parent == NULL)
                    ? (s->is_module ? SYM_MODULE : SYM_GLOBAL)
                    : SYM_LOCAL;
                Symbol* sym = scope_define(s->current, stmt->u.enum_def.name, ekind);
                if (sym) {
                    sym->type = type_new(TYPE_ENUM);
                    sym->type->struct_name = strdup(stmt->u.enum_def.name);
                }
            } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl) {
                // 预注册 export 中的 struct/face 定义
                Ast* decl = stmt->u.export.decl;
                if (decl->kind == AST_STRUCT_DEF && decl->u.struct_def.name &&
                    !scope_resolve(s->current, decl->u.struct_def.name)) {
                    Symbol* sym = scope_define(s->current, decl->u.struct_def.name, SYM_STRUCT);
                    if (sym) {
                        sym->type = type_new(TYPE_STRUCT);
                        sym->type->struct_name = strdup(decl->u.struct_def.name);
                        // 预注册字段信息，确保前向引用时能解析字段索引
                        sym->struct_field_count = decl->u.struct_def.field_count;
                        sym->struct_field_names = (char**)malloc(sizeof(char*) * decl->u.struct_def.field_count);
                        sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * decl->u.struct_def.field_count);
                        for (int j = 0; j < decl->u.struct_def.field_count; j++) {
                            // B5：重复字段名检查（export 路径，与根预注册一致；
                            // 定位用 decl 自身行列，预注册与主阶段重复报错时会被去重合并）
                            for (int k = 0; k < j; k++) {
                                if (decl->u.struct_def.field_names[j] && decl->u.struct_def.field_names[k] &&
                                    strcmp(decl->u.struct_def.field_names[j], decl->u.struct_def.field_names[k]) == 0) {
                                    error_add_at_fmt(ERR_SEMANTIC, decl->line, decl->column,
                                             "struct '%s' 存在重复字段 '%s'（字段 %d 与字段 %d 重名）",
                                             decl->u.struct_def.name ? decl->u.struct_def.name : "?",
                                             decl->u.struct_def.field_names[j], k + 1, j + 1);
                                }
                            }
                            sym->struct_field_names[j] = strdup(decl->u.struct_def.field_names[j]);
                            sym->struct_field_types[j] = type_copy(decl->u.struct_def.field_types[j]);
                        }
                    }
                    if (!struct_def_find(decl->u.struct_def.name)) {
                        ObjStructDef* early_def = struct_def_new(decl->u.struct_def.name, decl->u.struct_def.field_count, decl->u.struct_def.method_count);
                        if (early_def) {
                            early_def->impl_count = decl->u.struct_def.impl_count;
                            early_def->type_param_count = decl->u.struct_def.type_param_count;
                            if (early_def->type_param_count > 0 && decl->u.struct_def.type_params) {
                                early_def->type_param_names = (char**)malloc(sizeof(char*) * early_def->type_param_count);
                                for (int j = 0; j < early_def->type_param_count; j++) {
                                    early_def->type_param_names[j] = strdup(decl->u.struct_def.type_params[j]);
                                }
                            }
                            if (early_def->impl_count > 0) {
                                early_def->impl_names = (char**)malloc(sizeof(char*) * early_def->impl_count);
                                for (int j = 0; j < early_def->impl_count; j++) {
                                    early_def->impl_names[j] = strdup(decl->u.struct_def.impl_names[j]);
                                }
                            }
                            for (int j = 0; j < decl->u.struct_def.method_count; j++) {
                                early_def->methods[j].name = strdup(decl->u.struct_def.methods[j]->u.func.name);
                            }
                            struct_def_register(early_def);
                        }
                    }
                } else if (decl->kind == AST_FACE_DEF && decl->u.face_def.name &&
                           !scope_resolve(s->current, decl->u.face_def.name)) {
                    Symbol* sym = scope_define(s->current, decl->u.face_def.name, SYM_TYPE);
                    if (sym) {
                        sym->type = type_new(TYPE_FACE);
                        sym->type->struct_name = strdup(decl->u.face_def.name);
                    }
                    if (!face_def_find(decl->u.face_def.name)) {
                        ObjFaceDef* early_fdef = face_def_new(decl->u.face_def.name, decl->u.face_def.method_count);
                        if (early_fdef) {
                            for (int j = 0; j < decl->u.face_def.method_count; j++) {
                                early_fdef->methods[j].name = strdup(decl->u.face_def.method_names[j]);
                                early_fdef->methods[j].param_count = decl->u.face_def.method_param_counts[j];
                            }
                            face_def_register(early_fdef);
                        }
                    }
                } else if (decl->kind == AST_ENUM_DEF && decl->u.enum_def.name &&
                           !scope_resolve(s->current, decl->u.enum_def.name)) {
                    // export enum：与上面的 struct/face 同样提前占位（kind 同主阶段）
                    SymKind ekind = (s->current && s->current->parent == NULL)
                        ? (s->is_module ? SYM_MODULE : SYM_GLOBAL)
                        : SYM_LOCAL;
                    Symbol* sym = scope_define(s->current, decl->u.enum_def.name, ekind);
                    if (sym) {
                        sym->type = type_new(TYPE_ENUM);
                        sym->type->struct_name = strdup(decl->u.enum_def.name);
                    }
                }
            }
        }
    }

    // 预注册所有 clib 定义（★ 2026-10-10）
    //   与 visit_block.inc 里那份「预注册所有 clib 定义（确保变量类型声明可以正确解析）」保持一致：
    //   入口路径此前只在**访问到 clib 语句**时才注册 ⇒ 下面这轮函数形参/返回类型的预归一化
    //   看不到这个名字 ⇒ 形参里的 clib 名字归一不成 TYPE_CLIB ✗
    if (ast && ast->kind == AST_BLOCK) {
        AstList* clist = &ast->u.block;
        for (int i = 0; i < clist->count; i++) {
            Ast* stmt = clist->items[i];
            Ast* clib_decl = NULL;
            if (stmt->kind == AST_CLIB_DEF) {
                clib_decl = stmt;
            } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                       stmt->u.export.decl->kind == AST_CLIB_DEF) {
                clib_decl = stmt->u.export.decl;
            }
            if (!clib_decl || !clib_decl->u.clib_def.name || !s->current) continue;
            if (scope_resolve_local(s->current, clib_decl->u.clib_def.name)) continue;
            Symbol* sym = scope_define(s->current, clib_decl->u.clib_def.name, SYM_CLIB);
            if (sym) {
                TypeInfo* clib_type = type_new(TYPE_CLIB);
                clib_type->struct_name = strdup(clib_decl->u.clib_def.name);
                sym->type = clib_type;
                clib_decl->u.clib_def.ref.kind = sym->kind;
                clib_decl->u.clib_def.ref.index = sym->index;
                // parser 已分配过 ref.name ⇒ 覆盖前先释放
                free(clib_decl->u.clib_def.ref.name);
                clib_decl->u.clib_def.ref.name = strdup(sym->name);
                clib_decl->u.clib_def.ref.type_kind = TYPE_CLIB;
            }
        }
    }

    // 预注册所有全局函数定义（支持前向引用）
    if (ast && ast->kind == AST_BLOCK) {
        AstList* list = &ast->u.block;
        for (int i = 0; i < list->count; i++) {
            Ast* stmt = list->items[i];
            if (stmt->kind == AST_FUNC_DEF) {
                Symbol* sym = scope_define(s->current, stmt->u.func.name, SYM_GLOBAL_FUNC);
                if (sym) {
                    stmt->u.func.ref.kind = sym->kind;
                    stmt->u.func.ref.index = sym->index;
                    stmt->u.func.ref.name = strdup(sym->name);
                }
                // 将函数添加到函数表（用于参数类型检查）
                func_table_add(&s->func_table, stmt->u.func.name, stmt);
            } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                       stmt->u.export.decl->kind == AST_FUNC_DEF) {
                // 预注册 export func 定义（支持 export func 的前向引用）
                Ast* decl = stmt->u.export.decl;
                if (decl->u.func.name) {
                    Symbol* sym = scope_define(s->current, decl->u.func.name, SYM_GLOBAL_FUNC);
                    if (sym) {
                        decl->u.func.ref.kind = sym->kind;
                        decl->u.func.ref.index = sym->index;
                        decl->u.func.ref.name = strdup(sym->name);
                    }
                    // 将函数添加到函数表（用于参数类型检查）
                    func_table_add(&s->func_table, decl->u.func.name, decl);
                }
            }
        }
    }

    // 预归一化所有全局函数的形参/返回类型（struct → face/cstruct/clib/enum/alias）（★ 2026-10-10）
    //   必须在「访问函数体」之前跑完：调用点的实参检查（visit_expr.inc）读的是
    //   `func_def->u.func.param_types[i]` ⇒ 若被调函数定义在后面（前向引用），检查会发生在
    //   它自己的归一化之前 ⇒ 形参是 clib 时假报「期望 struct k32_charset, 实际 clib k32_charset」✗
    //   （web_charset.leno 5 处：`isValidUtf8`(L294) 调 `_iconvIsValid`(L606) 这类后向引用）。
    //   与 visit_block.inc「第二轮：转换所有函数的参数类型」同口径（那份对 export func 还会漏 ✗）。
    if (ast && ast->kind == AST_BLOCK) {
        AstList* flist = &ast->u.block;
        // ★ US1（2026-10-12）：这一段是**类型预归一化阶段** —— 它早于 AST_IMPORT / AST_USE 的访问
        //   ⇒ 模块别名还没登记。置标志让归一化函数**跳过"模块限定名 `m.Pt` 的隐式 use"**
        //   （留到主阶段那一遍 ✓），免得在这里报假错误「模块 'm' 未导入」✗
        s->in_type_prepass = 1;
        for (int i = 0; i < flist->count; i++) {
            Ast* fstmt = flist->items[i];
            Ast* fn = NULL;
            if (fstmt->kind == AST_FUNC_DEF) {
                fn = fstmt;
            } else if (fstmt->kind == AST_EXPORT && fstmt->u.export.decl &&
                       fstmt->u.export.decl->kind == AST_FUNC_DEF) {
                fn = fstmt->u.export.decl;
            }
            if (!fn) continue;
            for (int j = 0; j < fn->u.func.pcnt; j++) {
                resolve_alias_in_type(s, &fn->u.func.param_types[j], fn->line);
            }
            resolve_alias_in_type(s, &fn->u.func.return_type, fn->line);
        }
        s->in_type_prepass = 0;
    }

    // 预注册所有全局变量声明（支持前向引用）
    // struct 方法体可能引用在 struct 之后定义的全局变量，需要提前注册
    if (ast && ast->kind == AST_BLOCK) {
        AstList* list = &ast->u.block;
        for (int i = 0; i < list->count; i++) {
            Ast* stmt = list->items[i];
            if (stmt->kind == AST_VAR_DECL && stmt->u.var_decl.name) {
                // 预注册全局变量（支持前向引用）
                // 如果有明确类型声明（如 int x = 30），使用声明的类型
                // 如果是 var（类型推断），暂设为 any，visit 时会更新
                Symbol* sym = scope_resolve_local(s->current, stmt->u.var_decl.name);
                if (!sym) {
                    sym = scope_define(s->current, stmt->u.var_decl.name, SYM_GLOBAL);
                    if (sym) {
                        if (stmt->u.var_decl.type && stmt->u.var_decl.type->kind != TYPE_INFER) {
                            sym->type = type_copy(stmt->u.var_decl.type);
                        } else {
                            sym->type = type_new(TYPE_ANY);
                        }
                        stmt->u.var_decl.ref.kind = sym->kind;
                        stmt->u.var_decl.ref.index = sym->index;
                        stmt->u.var_decl.ref.name = strdup(sym->name);
                    }
                }
            } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                       stmt->u.export.decl->kind == AST_VAR_DECL) {
                // 预注册 export var 定义
                Ast* decl = stmt->u.export.decl;
                if (decl->u.var_decl.name) {
                    Symbol* sym = scope_resolve_local(s->current, decl->u.var_decl.name);
                    if (!sym) {
                        sym = scope_define(s->current, decl->u.var_decl.name, SYM_GLOBAL);
                        if (sym) {
                            if (decl->u.var_decl.type && decl->u.var_decl.type->kind != TYPE_INFER) {
                                sym->type = type_copy(decl->u.var_decl.type);
                            } else {
                                sym->type = type_new(TYPE_ANY);
                            }
                            decl->u.var_decl.ref.kind = sym->kind;
                            decl->u.var_decl.ref.index = sym->index;
                            decl->u.var_decl.ref.name = strdup(sym->name);
                        }
                    }
                }
            }
        }
    }

    // 单遍遍历
    if (ast && ast->kind == AST_BLOCK) {
        visit_list(s, &ast->u.block);
        // ★ 2026-10-23：顶层变量初始化顺序检查
        //   ⚠ 顶层**不会**进 visit_block.inc 的 AST_BLOCK 分支（这里直接 visit_list），
        //     所以必须挂在这个入口上；且要在 visit_list **之后**（ref 才绑定好）
        mv_check_top(s, ast);
    } else {
        visit(s, ast);
    }

    // ★ 泛型需求的**收尾复查**（2026-10-02）：需求是在"访问定义体"时记录的，而调用点可能
    //   更早被访问（main 在前 / struct 前向引用）⇒ 当场判会**漏过** ✗
    //   ⇒ 所有定义体都访问完，这里统一复查（与源码顺序无关 ✓）
    semantic_flush_pending_req_checks(s);

    // ★ 本文件/导入模块的 extend 重名（2026-11-03，2026-11-04 扩到本文件内 + struct 自带方法）
    //   —— 必须在 import 全部登记之后
    extend_check_local_dups(s, ast);
}

void semantic_cleanup(Semantic* s) {
    // 注意：AST 类型缓存在 AST 节点被释放时会自动清理
    // 这里只清理函数表和其他资源
    func_table_free(&s->func_table);
    semantic_free_generic_requirements(s);   // 泛型需求表（B 方案，2026-10-02）

    // 语义阶段自建的占位 AST（不在 AST 树里）⇒ 由这里释放
    if (s->owned_placeholders) {
        for (int i = 0; i < s->owned_placeholder_count; i++) {
            if (s->owned_placeholders[i]) ast_free(s->owned_placeholders[i]);
        }
        free(s->owned_placeholders);
        s->owned_placeholders = NULL;
        s->owned_placeholder_count = 0;
        s->owned_placeholder_capacity = 0;
    }
    
    for (int i = 0; i < s->imported_module_count; i++) {
        free(s->imported_modules[i].alias);
        free(s->imported_modules[i].file_path);
        free(s->imported_modules[i].real_name);   // 原生模块的真实名（用户模块为 NULL，free 安全 ✓）
        s->imported_modules[i].real_name = NULL;
        // 释放模块符号表
        if (s->imported_modules[i].sym_table) {
            module_symbol_table_destroy(s->imported_modules[i].sym_table);
            s->imported_modules[i].sym_table = NULL;
        }
    }
    s->imported_module_count = 0;

    s->root_scope = NULL;
    s->current = NULL;
    s->is_module = 0;
    s->is_lsp_mode = 0;
}

// 模块语义分析 - 将模块级别的符号标记为 SYM_MODULE
void semantic_analyze_module(Semantic* s, Ast* ast) {
    annos_reset_all();          // 同 semantic_analyze：模块级分析也要清注解表 ✓
    // ★ 2026-11-02：模块别名表同样要清 —— 它是进程级静态表，而别名是**本编译单元**
    //   的声明。不清的话，前面编译过的模块里的 `import "strings" as st` 会留在这里，
    //   让本模块的局部变量 `st` 声明失败、后续 `st.xxx` 报「模块 'st' 未导入」✗
    native_clear_module_aliases();
    // 标记为模块模式
    s->is_module = 1;

    // 创建模块作用域
    s->current = scope_new(NULL, 0);
    s->root_scope = s->current;

    // ⚠ 函数表已由 semantic_init 初始化；这里**不能**再 init 一次
    //   （func_table_init 只是把 entries 指针置空 ⇒ 前一份表数组直接泄漏）

    // 注册所有模块的 native 函数元信息
    native_register_all_module_metas();
    
    // 注册所有实例方法的元信息（用于编译期参数检查）
    native_register_all_instance_method_metas();

    // 将 native 函数注册到作用域
    int count;
    const NativeFunctionMeta* builtins = native_get_all_functions(&count);
    for (int i = 0; i < count; i++) {
        scope_define(s->current, builtins[i].name, SYM_NATIVE);
    }

    // ★ 先把 extend 的方法并进目标 struct（必须在预注册之前，见函数注释）
    extend_merge_into_block(s, ast);

    // 预注册所有模块类型定义（struct/cstruct/face）和函数定义（使用 SYM_MODULE 类型）
    // 同时提前注册 struct/face 到全局定义表，确保函数体中能通过 struct_def_find/face_def_find 找到
    if (ast && ast->kind == AST_BLOCK) {
        AstList* list = &ast->u.block;
        for (int i = 0; i < list->count; i++) {
            Ast* stmt = list->items[i];
            if (!stmt) continue;

            if (stmt->kind == AST_STRUCT_DEF) {
                if (stmt->u.struct_def.name && !scope_resolve(s->current, stmt->u.struct_def.name)) {
                    Symbol* sym = scope_define(s->current, stmt->u.struct_def.name, SYM_STRUCT);
                    if (sym) {
                        sym->type = type_new(TYPE_STRUCT);
                        sym->type->struct_name = strdup(stmt->u.struct_def.name);
                        // 预注册字段信息，确保前向引用时能解析字段索引
                        sym->struct_field_count = stmt->u.struct_def.field_count;
                        sym->struct_field_names = (char**)malloc(sizeof(char*) * stmt->u.struct_def.field_count);
                        sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * stmt->u.struct_def.field_count);
                        for (int j = 0; j < stmt->u.struct_def.field_count; j++) {
                            sym->struct_field_names[j] = strdup(stmt->u.struct_def.field_names[j]);
                            sym->struct_field_types[j] = type_copy(stmt->u.struct_def.field_types[j]);
                        }
                    }
                }
                if (!struct_def_find(stmt->u.struct_def.name)) {
                    ObjStructDef* early_def = struct_def_new(stmt->u.struct_def.name, stmt->u.struct_def.field_count, stmt->u.struct_def.method_count);
                    if (early_def) {
                        early_def->impl_count = stmt->u.struct_def.impl_count;
                        if (early_def->impl_count > 0) {
                            early_def->impl_names = (char**)malloc(sizeof(char*) * early_def->impl_count);
                            for (int j = 0; j < early_def->impl_count; j++) {
                                early_def->impl_names[j] = strdup(stmt->u.struct_def.impl_names[j]);
                            }
                        }
                        for (int j = 0; j < stmt->u.struct_def.method_count; j++) {
                            early_def->methods[j].name = strdup(stmt->u.struct_def.methods[j]->u.func.name);
                        }
                        struct_def_register(early_def);
                    }
                }
            } else if (stmt->kind == AST_FACE_DEF) {
                if (stmt->u.face_def.name && !scope_resolve(s->current, stmt->u.face_def.name)) {
                    Symbol* sym = scope_define(s->current, stmt->u.face_def.name, SYM_TYPE);
                    if (sym) {
                        sym->type = type_new(TYPE_FACE);
                        sym->type->struct_name = strdup(stmt->u.face_def.name);
                    }
                }
                if (!face_def_find(stmt->u.face_def.name)) {
                    ObjFaceDef* early_fdef = face_def_new(stmt->u.face_def.name, stmt->u.face_def.method_count);
                    if (early_fdef) {
                        face_def_set_type_params(early_fdef, stmt->u.face_def.type_param_count,
                            stmt->u.face_def.type_params, stmt->u.face_def.type_param_constraints);
                        for (int j = 0; j < stmt->u.face_def.method_count; j++) {
                            early_fdef->methods[j].name = strdup(stmt->u.face_def.method_names[j]);
                            early_fdef->methods[j].param_count = stmt->u.face_def.method_param_counts[j];
                            if (stmt->u.face_def.method_return_types && stmt->u.face_def.method_return_types[j]) {
                                early_fdef->methods[j].return_type = type_copy(stmt->u.face_def.method_return_types[j]);
                                resolve_generic_in_type(early_fdef->methods[j].return_type,
                                    stmt->u.face_def.type_params, stmt->u.face_def.type_param_constraints,
                                    stmt->u.face_def.type_param_count);
                            } else {
                                early_fdef->methods[j].return_type = NULL;
                            }
                        }
                        face_def_register(early_fdef);
                    }
                }
            } else if (stmt->kind == AST_CSTRUCT_DEF) {
                if (stmt->u.cstruct_def.name && !scope_resolve(s->current, stmt->u.cstruct_def.name)) {
                    Symbol* sym = scope_define(s->current, stmt->u.cstruct_def.name, SYM_CSTRUCT);
                    if (sym) {
                        sym->type = type_new(TYPE_CSTRUCT);
                        sym->type->struct_name = strdup(stmt->u.cstruct_def.name);
                    }
                }
            } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl) {
                Ast* decl = stmt->u.export.decl;
                if (decl->kind == AST_STRUCT_DEF && decl->u.struct_def.name &&
                    !scope_resolve(s->current, decl->u.struct_def.name)) {
                    Symbol* sym = scope_define(s->current, decl->u.struct_def.name, SYM_STRUCT);
                    if (sym) {
                        sym->type = type_new(TYPE_STRUCT);
                        sym->type->struct_name = strdup(decl->u.struct_def.name);
                        // 预注册字段信息，确保前向引用时能解析字段索引
                        sym->struct_field_count = decl->u.struct_def.field_count;
                        sym->struct_field_names = (char**)malloc(sizeof(char*) * decl->u.struct_def.field_count);
                        sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * decl->u.struct_def.field_count);
                        for (int j = 0; j < decl->u.struct_def.field_count; j++) {
                            // B5：重复字段名检查（export 路径，与根预注册一致；
                            // 定位用 decl 自身行列，预注册与主阶段重复报错时会被去重合并）
                            for (int k = 0; k < j; k++) {
                                if (decl->u.struct_def.field_names[j] && decl->u.struct_def.field_names[k] &&
                                    strcmp(decl->u.struct_def.field_names[j], decl->u.struct_def.field_names[k]) == 0) {
                                    error_add_at_fmt(ERR_SEMANTIC, decl->line, decl->column,
                                             "struct '%s' 存在重复字段 '%s'（字段 %d 与字段 %d 重名）",
                                             decl->u.struct_def.name ? decl->u.struct_def.name : "?",
                                             decl->u.struct_def.field_names[j], k + 1, j + 1);
                                }
                            }
                            sym->struct_field_names[j] = strdup(decl->u.struct_def.field_names[j]);
                            sym->struct_field_types[j] = type_copy(decl->u.struct_def.field_types[j]);
                        }
                    }
                    if (!struct_def_find(decl->u.struct_def.name)) {
                        ObjStructDef* early_def = struct_def_new(decl->u.struct_def.name, decl->u.struct_def.field_count, decl->u.struct_def.method_count);
                        if (early_def) {
                            early_def->impl_count = decl->u.struct_def.impl_count;
                            early_def->type_param_count = decl->u.struct_def.type_param_count;
                            if (early_def->type_param_count > 0 && decl->u.struct_def.type_params) {
                                early_def->type_param_names = (char**)malloc(sizeof(char*) * early_def->type_param_count);
                                for (int j = 0; j < early_def->type_param_count; j++) {
                                    early_def->type_param_names[j] = strdup(decl->u.struct_def.type_params[j]);
                                }
                            }
                            if (early_def->impl_count > 0) {
                                early_def->impl_names = (char**)malloc(sizeof(char*) * early_def->impl_count);
                                for (int j = 0; j < early_def->impl_count; j++) {
                                    early_def->impl_names[j] = strdup(decl->u.struct_def.impl_names[j]);
                                }
                            }
                            for (int j = 0; j < decl->u.struct_def.method_count; j++) {
                                early_def->methods[j].name = strdup(decl->u.struct_def.methods[j]->u.func.name);
                            }
                            struct_def_register(early_def);
                        }
                    }
                } else if (decl->kind == AST_FACE_DEF && decl->u.face_def.name &&
                           !scope_resolve(s->current, decl->u.face_def.name)) {
                    Symbol* sym = scope_define(s->current, decl->u.face_def.name, SYM_TYPE);
                    if (sym) {
                        sym->type = type_new(TYPE_FACE);
                        sym->type->struct_name = strdup(decl->u.face_def.name);
                    }
                    if (!face_def_find(decl->u.face_def.name)) {
                        ObjFaceDef* early_fdef = face_def_new(decl->u.face_def.name, decl->u.face_def.method_count);
                        if (early_fdef) {
                            for (int j = 0; j < decl->u.face_def.method_count; j++) {
                                early_fdef->methods[j].name = strdup(decl->u.face_def.method_names[j]);
                                early_fdef->methods[j].param_count = decl->u.face_def.method_param_counts[j];
                            }
                            face_def_register(early_fdef);
                        }
                    }
                } else if (decl->kind == AST_CSTRUCT_DEF && decl->u.cstruct_def.name &&
                           !scope_resolve(s->current, decl->u.cstruct_def.name)) {
                    Symbol* sym = scope_define(s->current, decl->u.cstruct_def.name, SYM_CSTRUCT);
                    if (sym) {
                        sym->type = type_new(TYPE_CSTRUCT);
                        sym->type->struct_name = strdup(decl->u.cstruct_def.name);
                    }
                } else if (decl->kind == AST_FUNC_DEF && decl->u.func.name) {
                    Symbol* sym = scope_define(s->current, decl->u.func.name, SYM_MODULE);
                    if (sym) {
                        decl->u.func.ref.kind = sym->kind;
                        decl->u.func.ref.index = sym->index;
                        decl->u.func.ref.name = strdup(sym->name);
                        sym->type = type_function(NULL, NULL, 0);
                    }
                    // 将函数添加到函数表（支持前向引用类型推断）
                    func_table_add(&s->func_table, decl->u.func.name, decl);
                }
            } else if (stmt->kind == AST_FUNC_DEF) {
                if (stmt->u.func.name) {
                    Symbol* sym = scope_define(s->current, stmt->u.func.name, SYM_MODULE);
                    if (sym) {
                        stmt->u.func.ref.kind = sym->kind;
                        stmt->u.func.ref.index = sym->index;
                        stmt->u.func.ref.name = strdup(sym->name);
                        sym->type = type_function(NULL, NULL, 0);
                    }
                    // 将函数添加到函数表（支持前向引用类型推断）
                    func_table_add(&s->func_table, stmt->u.func.name, stmt);
                }
            } else if (stmt->kind == AST_ALIAS && stmt->u.alias.type) {
                // 类型别名注册到语义作用域，使 resolve_alias_in_type 能找到
                if (!scope_resolve(s->current, stmt->u.alias.name)) {
                    Symbol* sym = scope_define(s->current, stmt->u.alias.name, SYM_TYPE);
                    if (sym) {
                        sym->type = type_copy(stmt->u.alias.type);
                    }
                }
            }
        }
    }

    // 预注册所有模块级全局变量声明（支持前向引用）
    // struct 方法体可能引用在 struct 之后定义的全局变量，需要提前注册
    if (ast && ast->kind == AST_BLOCK) {
        AstList* list = &ast->u.block;
        for (int i = 0; i < list->count; i++) {
            Ast* stmt = list->items[i];
            if (stmt->kind == AST_VAR_DECL && stmt->u.var_decl.name) {
                Symbol* sym = scope_resolve_local(s->current, stmt->u.var_decl.name);
                if (!sym) {
                    SymKind kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                    sym = scope_define(s->current, stmt->u.var_decl.name, kind);
                    if (sym) {
                        if (stmt->u.var_decl.type && stmt->u.var_decl.type->kind != TYPE_INFER) {
                            sym->type = type_copy(stmt->u.var_decl.type);
                        } else {
                            sym->type = type_new(TYPE_ANY);
                        }
                        stmt->u.var_decl.ref.kind = sym->kind;
                        stmt->u.var_decl.ref.index = sym->index;
                        stmt->u.var_decl.ref.name = strdup(sym->name);
                    }
                }
            } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                       stmt->u.export.decl->kind == AST_VAR_DECL) {
                Ast* decl = stmt->u.export.decl;
                if (decl->u.var_decl.name) {
                    Symbol* sym = scope_resolve_local(s->current, decl->u.var_decl.name);
                    if (!sym) {
                        SymKind kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                        sym = scope_define(s->current, decl->u.var_decl.name, kind);
                        if (sym) {
                            if (decl->u.var_decl.type && decl->u.var_decl.type->kind != TYPE_INFER) {
                                sym->type = type_copy(decl->u.var_decl.type);
                            } else {
                                sym->type = type_new(TYPE_ANY);
                            }
                            decl->u.var_decl.ref.kind = sym->kind;
                            decl->u.var_decl.ref.index = sym->index;
                            decl->u.var_decl.ref.name = strdup(sym->name);
                        }
                    }
                }
            } else if (stmt->kind == AST_DESTRUCT_DECL) {
                // 预注册解构声明的所有变量
                for (int j = 0; j < stmt->u.destruct_decl.slot_count; j++) {
                    const char* dname = stmt->u.destruct_decl.names[j];
                    if (dname && !scope_resolve_local(s->current, dname)) {
                        SymKind dkind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                        Symbol* dsym = scope_define(s->current, dname, dkind);
                        if (dsym) {
                            if (stmt->u.destruct_decl.slot_types[j]) {
                                dsym->type = type_copy(stmt->u.destruct_decl.slot_types[j]);
                            } else {
                                dsym->type = type_new(TYPE_ANY);
                            }
                            stmt->u.destruct_decl.refs[j].kind = dsym->kind;
                            stmt->u.destruct_decl.refs[j].index = dsym->index;
                            if (!stmt->u.destruct_decl.refs[j].name) {
                                stmt->u.destruct_decl.refs[j].name = strdup(dsym->name);
                            }
                        }
                    }
                }
            } else if (stmt->kind == AST_EXPORT && stmt->u.export.decl &&
                       stmt->u.export.decl->kind == AST_DESTRUCT_DECL) {
                Ast* decl = stmt->u.export.decl;
                for (int j = 0; j < decl->u.destruct_decl.slot_count; j++) {
                    const char* dname = decl->u.destruct_decl.names[j];
                    if (dname && !scope_resolve_local(s->current, dname)) {
                        SymKind dkind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                        Symbol* dsym = scope_define(s->current, dname, dkind);
                        if (dsym) {
                            if (decl->u.destruct_decl.slot_types[j]) {
                                dsym->type = type_copy(decl->u.destruct_decl.slot_types[j]);
                            } else {
                                dsym->type = type_new(TYPE_ANY);
                            }
                            decl->u.destruct_decl.refs[j].kind = dsym->kind;
                            decl->u.destruct_decl.refs[j].index = dsym->index;
                            if (!decl->u.destruct_decl.refs[j].name) {
                                decl->u.destruct_decl.refs[j].name = strdup(dsym->name);
                            }
                        }
                    }
                }
            }
        }
    }

    // 单遍遍历
    if (ast && ast->kind == AST_BLOCK) {
        visit_list(s, &ast->u.block);
        // ★ 2026-10-23：顶层变量初始化顺序检查
        //   ⚠ 顶层**不会**进 visit_block.inc 的 AST_BLOCK 分支（这里直接 visit_list），
        //     所以必须挂在这个入口上；且要在 visit_list **之后**（ref 才绑定好）
        mv_check_top(s, ast);
    } else {
        visit(s, ast);
    }

    // ★ 泛型需求的**收尾复查**（2026-10-02）：需求是在"访问定义体"时记录的，而调用点可能
    //   更早被访问（main 在前 / struct 前向引用）⇒ 当场判会**漏过** ✗
    //   ⇒ 所有定义体都访问完，这里统一复查（与源码顺序无关 ✓）
    semantic_flush_pending_req_checks(s);

    // ★ 本文件/导入模块的 extend 重名（2026-11-03，2026-11-04 扩到本文件内 + struct 自带方法）
    //   —— 必须在 import 全部登记之后
    extend_check_local_dups(s, ast);
}
