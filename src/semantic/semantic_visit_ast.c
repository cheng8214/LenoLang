#include "semantic_internal.h"

// 注解（2026-10-31）：收集 @optkeys + 在调用点检查未知键，实现在 semantic_annotations.c
//   ⚠ 一期只管**同模块**的直接调用、只查**字面量 Dict**、只警告不中断 ✓
extern void annos_reset_all(void);
extern void annos_collect_func(Ast* ast);
// 跨模块（`SDL3.createHBox({...})`）也在里面处理 —— 键表来自模块符号表
//   ⚠ 必须在 AST_CALL 阶段查：改写成 AST_MODULE_CALL 之后实参已不是原始字面量 ✗
extern void annos_check_call(Semantic* s, Ast* ast);
extern void annos_check_module(Ast* ast, const char* keys_csv, const char* mod_path, Semantic* s);
// 从被调函数的 AST 直接取注解 —— 同模块 / 跨模块通用（见 visit_expr.inc 的参数检查处 ✓）
//   Semantic* 用于值类型检查（infer_expr_type，2026-11-01）
extern void annos_check_call_def(Ast* call_ast, Ast* func_def, Semantic* s);

// ============================================================================
// 泛型类型推断辅助
// ============================================================================
#include "visitinc/visit_generic.inc"

// ============================================================================
// ★ pri：成员私有的编译期检查（默认全公有 ✓）
// ============================================================================
// 语义（2026-09-29 定稿）：
//   · 默认**全部公有** ⇒ 只有显式写了 `pri` 的成员才拦 ✓（定稿前全仓无一处 pri ⇒ 零影响 ✓）
//   · `_` 前缀**不是**规则 ✗ —— 它只是书写者的命名习惯（全仓大量在用），编译器不看它 ✓
//   · 允许：该 struct **自己的方法内部**访问（含 self.xxx ✓，靠 self 的类型判定 ✓）
//   · 拦住：其他任何地方（同文件别的 struct ✓、应用 ✓、别的模块 ✓）
//   · 报错进 ERR_SEMANTIC ⇒ `-c` 下非零退出 ✓（可进 CI ✓）
//
// ⚠ 已知边界（v1，写在这个注释里免得被误以为"全拦住了" ✗）：
//   运行期**按名字**找成员的通路拦不住 —— obj["字段名"]（vm 的 struct_get_field_index ✓）、
//   静态类型是 any/Dict（codegen 故意走运行期按名分发 ✓）、泛型回落成 any ✓。
//   要拦得把可见性编进 StructFieldInfo/StructMethodInfo 并在运行期 lookup 里校验（v2 ✓）。
// ⚠ 拿不到 struct 定义 AST 时**放行** ✗（宁可漏报，不可误报 ✓）；跨模块导入的 struct 目前
//   多数取不到 AST ⇒ 走放行 ✅（v1 的已知缺口，v2 用模块符号表补 ✓）

// 在当前文件里找 struct 定义（含 `export struct` ✓ —— 与 visit_expr.inc 里前向查找同款 ✓）
static Ast* pri_find_struct_def(Semantic* s, const char* struct_name) {
    if (!s || !struct_name || !s->root) return NULL;
    for (int i = 0; i < s->root->u.block.count; i++) {
        Ast* st = s->root->u.block.items[i];
        if (!st) continue;
        if (st->kind == AST_STRUCT_DEF && st->u.struct_def.name &&
            strcmp(st->u.struct_def.name, struct_name) == 0) {
            return st;
        }
        if (st->kind == AST_EXPORT && st->u.export.decl &&
            st->u.export.decl->kind == AST_STRUCT_DEF &&
            st->u.export.decl->u.struct_def.name &&
            strcmp(st->u.export.decl->u.struct_def.name, struct_name) == 0) {
            return st->u.export.decl;
        }
    }
    return NULL;
}

// 当前是否在 `struct_name` 自己的方法内部（看 `self` 的静态类型 ✓ —— 只有方法里才有 self ✓）
static int pri_inside_own_method(Semantic* s, const char* struct_name) {
    if (!s || !struct_name) return 0;
    // 跨模块 extend 的方法体：self 的类型虽然等于目标 struct，但代码不在定义模块里 ⇒
    //   不能算"自己的方法内部"（否则 pri 检查被整体绕过，实测能读到私有字段 ✗）
    if (s->in_cross_module_extend) return 0;
    Symbol* self_sym = scope_resolve(s->current, "self");
    if (!self_sym || !self_sym->type || !self_sym->type->struct_name) return 0;
    return strcmp(self_sym->type->struct_name, struct_name) == 0;
}

// 该字段是否私有；命中时用 out_line 带回声明行（struct 定义行 ✓；0 = 信息不可得 ✓）
static int pri_field_is_private(Semantic* s, const char* struct_name, const char* field_name, int* out_line) {
    Ast* def = pri_find_struct_def(s, struct_name);
    if (!def || !def->u.struct_def.field_private) return 0;
    for (int i = 0; i < def->u.struct_def.field_count; i++) {
        if (def->u.struct_def.field_names[i] &&
            strcmp(def->u.struct_def.field_names[i], field_name) == 0) {
            if (def->u.struct_def.field_private[i]) {
                if (out_line) *out_line = def->line;
                return 1;
            }
            return 0;   // 找到字段但没标 pri ⇒ 公有 ✓
        }
    }
    return 0;
}

// 该方法是否私有（同上 ✓）
static int pri_method_is_private(Semantic* s, const char* struct_name, const char* method_name, int* out_line) {
    Ast* def = pri_find_struct_def(s, struct_name);
    if (!def) return 0;
    for (int i = 0; i < def->u.struct_def.method_count; i++) {
        Ast* m = def->u.struct_def.methods[i];
        if (m && m->kind == AST_FUNC_DEF && m->u.func.name &&
            strcmp(m->u.func.name, method_name) == 0) {
            if (m->u.func.is_private) {
                if (out_line) *out_line = m->line;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

// ============ 跨模块：从**导入模块的符号表**里查私有位 ============
// 为什么必须有这一支：导入方**拿不到被导入 struct 的定义 AST** ✗
//   ⇒ 只靠 AST 判私有会静默放行 ✗（实测：应用 use 框架模块后碰 pri 成员，一条报错都没有 ✓）
// 数据来源：文本扫描器剥掉 `pri` 时写进 ModuleStructField/Method.is_private ✓，
//   再随 .lenosymc 缓存往返（缓存版本已升到 v32 ✓）
static int pri_imported_field_private(Semantic* s, const char* struct_name,
                                      const char* field_name, int* out_line,
                                      const char** out_file) {
    if (!s || !struct_name || !field_name) return 0;
    for (int mi = 0; mi < s->imported_module_count; mi++) {
        ModuleSymbolTable* t = s->imported_modules[mi].sym_table;
        if (!t) continue;
        ModuleStructSymbol* ss = module_symbol_table_find_struct(t, struct_name);
        if (!ss) continue;
        for (int j = 0; j < ss->field_count; j++) {
            if (ss->fields[j].name && strcmp(ss->fields[j].name, field_name) == 0) {
                if (ss->fields[j].is_private) {
                    if (out_line) *out_line = ss->fields[j].line;
                    // ★ 2026-11-04：声明**不在当前编译单元**里 ⇒ 报错文案必须用模块文件路径，
                    //   此前一律套 error_get_filename()（= 调用方文件）⇒ 指向错文件且行号对不上 ✗
                    if (out_file) *out_file = s->imported_modules[mi].file_path;
                    return 1;
                }
                return 0;   // 找到但没标 pri ⇒ 公有 ✓
            }
        }
    }
    return 0;
}

static int pri_imported_method_private(Semantic* s, const char* struct_name,
                                       const char* method_name, int* out_line,
                                       const char** out_file) {
    if (!s || !struct_name || !method_name) return 0;
    for (int mi = 0; mi < s->imported_module_count; mi++) {
        ModuleSymbolTable* t = s->imported_modules[mi].sym_table;
        if (!t) continue;
        ModuleStructMethod* m = module_symbol_table_find_struct_method(t, struct_name, method_name);
        if (m) {
            if (m->is_private) {
                if (out_line) *out_line = m->line;
                if (out_file) *out_file = s->imported_modules[mi].file_path;
                return 1;
            }
            return 0;
        }
    }
    // ★ 2026-11-04：**扩展方法**不在目标 struct 的 methods 里，而在各模块符号表的
    //   extends 段 ⇒ 同口径再扫一遍。缺了这段，`pri func` 写在跨模块 extend 里等于
    //   完全公开：主程序能 `p.helper()`、另一个扩展模块里的裸调用也能进去（实测都能跑 ✗）。
    //   语义按设计 §5.4 补齐："同一模块内的其它文件可调，模块外不可调" ——
    //   本模块自己的 extend 方法不在**导入**表里（不会命中这里）⇒ 自己调自己仍然放行 ✓
    for (int mi = 0; mi < s->imported_module_count; mi++) {
        ModuleSymbolTable* t = s->imported_modules[mi].sym_table;
        if (!t) continue;
        for (int ei = 0; ei < t->extend_count; ei++) {
            ModuleExtendSymbol* ex = &t->extends[ei];
            if (!ex->type_name || strcmp(ex->type_name, struct_name) != 0) continue;
            for (int k = 0; k < ex->method_count; k++) {
                ModuleStructMethod* em = &ex->methods[k];
                if (!em->name || strcmp(em->name, method_name) != 0) continue;
                if (em->is_private) {
                    if (out_line) *out_line = em->line;
                    if (out_file) *out_file = s->imported_modules[mi].file_path;
                    return 1;
                }
                return 0;
            }
        }
    }
    return 0;
}

// 统一的报错文案（字段/方法共用 ✓ —— 这套东西值钱的地方就在这句话 ✓）
// decl_file = 声明所在文件（NULL ⇒ 当前编译单元，**本地**声明就是这样）
static void pri_report(Semantic* s, Ast* ast, const char* kind, const char* struct_name,
                       const char* member, int decl_line, const char* decl_file) {
    (void)s;
    error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
        "'%s.%s' 是 pri 私有%s（%s 第 %d 行）—— 只有 %s 自己的方法内部可以访问它；"
        "要对外开放就把 'pri' 去掉（默认全公有）",
        struct_name, member, kind, decl_file ? decl_file : error_get_filename(),
        decl_line, struct_name);
}

// 字段访问检查（读 ✓ 写 ✓ 都走 AST_FIELD_ACCESS ✓ ⇒ 一处即可 ✓）
// ⚠ 非 static：semantic_type.c 的 infer_expr_type 里也要用（跨模块的表那时才加载好 ✓）
void pri_check_field_access(Semantic* s, Ast* ast, TypeInfo* obj_type, const char* field_name) {
    if (!obj_type || obj_type->kind != TYPE_STRUCT || !obj_type->struct_name) return;
    if (pri_inside_own_method(s, obj_type->struct_name)) return;
    int line = 0;
    const char* decl_file = NULL;
    if (pri_field_is_private(s, obj_type->struct_name, field_name, &line)) {
        pri_report(s, ast, "字段", obj_type->struct_name, field_name, line, NULL);
        return;
    }
    // 本文件里没有这个 struct 的定义（= 它是**导入**进来的 ✓）⇒ 查模块符号表 ✓
    if (pri_find_struct_def(s, obj_type->struct_name) == NULL &&
        pri_imported_field_private(s, obj_type->struct_name, field_name, &line, &decl_file)) {
        pri_report(s, ast, "字段", obj_type->struct_name, field_name, line, decl_file);
    }
}

// 方法调用检查（obj.method() ✓）
// ⚠ 非 static：semantic_type.c 的表达式侧方法解析里也要用（同字段侧的理由 ✓）
void pri_check_method_access(Semantic* s, Ast* ast, TypeInfo* obj_type, const char* method_name) {
    if (!obj_type || obj_type->kind != TYPE_STRUCT || !obj_type->struct_name) return;
    if (pri_inside_own_method(s, obj_type->struct_name)) return;
    int line = 0;
    const char* decl_file = NULL;
    if (pri_method_is_private(s, obj_type->struct_name, method_name, &line)) {
        pri_report(s, ast, "方法", obj_type->struct_name, method_name, line, NULL);
        return;
    }
    // 跨模块兜底（同字段侧 ✓）
    if (pri_find_struct_def(s, obj_type->struct_name) == NULL &&
        pri_imported_method_private(s, obj_type->struct_name, method_name, &line, &decl_file)) {
        pri_report(s, ast, "方法", obj_type->struct_name, method_name, line, decl_file);
    }
}

// ============================================================================
// use 导入 alias 时，递归导入底层类型依赖
// 当 use 导入 alias（如 EventHandler = func(Event):bool）时，
// alias 底层类型引用的其他类型（如 Event）不会自动带入当前作用域。
// 此函数递归扫描 TypeInfo，将引用的 struct/cstruct/face/enum/alias 自动导入。
// ============================================================================
static void import_type_deps(Semantic* s, ImportedModuleInfo* module_info, TypeInfo* type_info) {
    if (!type_info || !module_info || !module_info->sym_table) return;

    // 递归处理子类型
    switch (type_info->kind) {
        case TYPE_FUNCTION:
            // 函数类型：递归处理参数类型和返回类型
            if (type_info->param_types) {
                for (int i = 0; i < type_info->param_count; i++) {
                    import_type_deps(s, module_info, type_info->param_types[i]);
                }
            }
            if (type_info->return_type) {
                import_type_deps(s, module_info, type_info->return_type);
            }
            break;
        case TYPE_ARRAY:
            if (type_info->element_type) {
                import_type_deps(s, module_info, type_info->element_type);
            }
            break;
        case TYPE_DICT:
            if (type_info->key_type) {
                import_type_deps(s, module_info, type_info->key_type);
            }
            if (type_info->value_type) {
                import_type_deps(s, module_info, type_info->value_type);
            }
            break;
        case TYPE_PTR_GENERIC:
            if (type_info->element_type) {
                import_type_deps(s, module_info, type_info->element_type);
            }
            break;
        case TYPE_STRUCT:
        case TYPE_CSTRUCT:
        case TYPE_FACE:
        case TYPE_ENUM: {
            // 这些类型有 struct_name，需要检查是否已在当前作用域
            if (!type_info->struct_name) break;
            // 如果当前作用域已有该类型，无需重复导入
            Symbol* existing = scope_resolve_local(s->current, type_info->struct_name);
            if (existing) break;

            // 从源模块符号表中查找并导入
            const char* dep_name = type_info->struct_name;

            // 尝试 struct/cstruct
            ModuleStructSymbol* ssym = module_symbol_table_find_struct(module_info->sym_table, dep_name);
            if (ssym) {
                SymKind kind = ssym->is_cstruct ? SYM_CSTRUCT : SYM_STRUCT;
                Symbol* sym = scope_define(s->current, dep_name, kind);
                if (sym) {
                    // 字段与泛型参数：走**唯一实现**（此前这份只按扁平字段重建 ⇒ 丢嵌套泛型，
                    // 与 AST_USE 那份不一致；实测 Array[Array[int]] → Array[Array]。
                    // 详见 assert/test_alias_type_deps_struct_fields.leno 与
                    // semantic_type_utils.c 里 semantic_attach_struct_fields 的说明）
                    semantic_attach_struct_fields(sym, ssym);
                    // 全局 struct_def 注册 + 方法占位符注册：走**唯一实现**
                    // （此前这里那份贫：方法占位符不填 param_types / type_params ⇒ 泛型方法经
                    //   alias 这条导入路时返回类型停在未替换的 `T`、类型检查被静默跳过。
                    //   见 assert/test_generic_type_param_paths.leno 判据 3）
                    semantic_register_struct_from_module(s, ssym);
                }
                break;
            }
            // 尝试 enum
            ModuleEnumSymbol* esym = module_symbol_table_find_enum(module_info->sym_table, dep_name);
            if (esym) {
                Symbol* sym = scope_define(s->current, dep_name, SYM_TYPE);
                if (sym) {
                    sym->type = type_new(TYPE_ENUM);
                    sym->type->struct_name = strdup(dep_name);
                    sym->enum_value_count = esym->member_count;
                    sym->enum_value_names = (char**)malloc(sizeof(char*) * esym->member_count);
                    sym->enum_values = (int64_t*)malloc(sizeof(int64_t) * esym->member_count);
                    for (int mi = 0; mi < esym->member_count; mi++) {
                        sym->enum_value_names[mi] = strdup(esym->member_names[mi]);
                        sym->enum_values[mi] = esym->member_values[mi];
                    }
                }
                break;
            }
            // 尝试 face
            ModuleFaceSymbol* fsym = module_symbol_table_find_face(module_info->sym_table, dep_name);
            if (fsym) {
                Symbol* sym = scope_define(s->current, dep_name, SYM_TYPE);
                if (sym) {
                    sym->type = type_new(TYPE_FACE);
                    sym->type->struct_name = strdup(dep_name);
                }
                if (!face_def_find(dep_name)) {
                    ObjFaceDef* fdef = face_def_new(dep_name, fsym->method_count);
                    if (fdef) {
                        for (int mi = 0; mi < fsym->method_count; mi++) {
                            fdef->methods[mi].name = strdup(fsym->methods[mi].name);
                            fdef->methods[mi].return_type = type_new(fsym->methods[mi].return_type);
                            fdef->methods[mi].param_count = 0;
                            fdef->methods[mi].param_types = NULL;
                        }
                        face_def_register(fdef);
                    }
                }
                break;
            }
            // 尝试 alias（依赖也可能是另一个 alias）
            ModuleAliasSymbol* asym = module_symbol_table_find_alias(module_info->sym_table, dep_name);
            if (asym) {
                Symbol* sym = scope_define(s->current, dep_name, SYM_TYPE);
                if (sym && asym->type_info) {
                    sym->type = type_copy(asym->type_info);
                    // 递归导入 alias 的底层类型依赖
                    import_type_deps(s, module_info, asym->type_info);
                }
                break;
            }
            // clib 类型
            ModuleClibSymbol* csym = module_symbol_table_find_clib(module_info->sym_table, dep_name);
            if (csym) {
                Symbol* sym = scope_define(s->current, dep_name, SYM_TYPE);
                if (sym) {
                    sym->type = type_new(TYPE_CLIB);
                    sym->type->struct_name = strdup(dep_name);
                }
                break;
            }
            break;
        }
        default:
            break;
    }
}

// ★ T27：`if a != null` 的空窄化 —— 进入对应分支后，让 `a` 不再被判为「确定为 null」。
// ----------------------------------------------------------------------------
// T11 的 `Symbol.is_null_value` 只认「声明即 null、此后未赋值」，没把 `if a != null` 的窄化
// 当豁免路径 ⇒ `int? a = null; if a != null { a + 1 }` 被误报成编译错误（而报错提示推荐的
// 正是这种写法）。这里把窄化补上：为分支内出现的 `a` 建一个同名影子符号（复用原槽位 index），
// 其 is_null_value 为 0 —— 之后 `a` 解析到影子符号，算术守卫就不会再报「确定为 null」。
//   narrow_on_ne：then 分支传 1（条件 `a != null` 成立 ⇒ 非空）；
//                 else 分支传 0（条件 `a == null` 不成立 ⇒ 非空）。
// ⚠ 只认 `VAR != null` / `VAR == null`，且只沿 `and` 链（or 有短路语义，不收窄）；
//   原变量未被 T11 置位的一律跳过 —— 收紧改动面，保持「宁漏勿误报」。
static void apply_null_narrowing(Semantic* s, Ast* cond, int narrow_on_ne) {
    if (!cond) return;
    Ast* stack[32];
    int top = 0;
    stack[top++] = cond;
    while (top > 0) {
        Ast* node = stack[--top];
        if (!node) continue;
        if (node->kind == AST_BINOP && node->u.binop.op == TOK_AND) {
            if (top < 31) stack[top++] = node->u.binop.r;
            if (top < 31) stack[top++] = node->u.binop.l;
            continue;
        }
        if (node->kind != AST_BINOP) continue;
        // then 分支认 `!=`，else 分支认 `==`（两者成立时都意味着变量非空）
        if ((node->u.binop.op == TOK_NEQ) != narrow_on_ne) continue;
        Ast* var_side = NULL;
        if (node->u.binop.l && node->u.binop.r) {
            if (node->u.binop.l->kind == AST_VAR && node->u.binop.r->kind == AST_NULL) {
                var_side = node->u.binop.l;
            } else if (node->u.binop.r->kind == AST_VAR && node->u.binop.l->kind == AST_NULL) {
                var_side = node->u.binop.r;
            }
        }
        if (!var_side || !var_side->u.var.name) continue;

        SymRef ref;
        memset(&ref, 0, sizeof(ref));
        Symbol* original_sym = resolve_variable_with_upvalue(s, var_side->u.var.name, &ref);
        if (!original_sym || !original_sym->is_null_value) { symref_dispose(&ref); continue; }
        if (!ref.name) { symref_dispose(&ref); continue; }

        SymKind kind = (ref.kind == SYM_UPVALUE) ? SYM_UPVALUE
                     : (ref.kind == SYM_GLOBAL)  ? SYM_GLOBAL : SYM_LOCAL;
        // 已有同名守卫符号（如 `a != null and a is Point` 的 is 守卫）⇒ scope_define 返回 NULL，
        // 复用即可 —— 那种影子符号的 is_null_value 本就是 0。
        Symbol* shadow = scope_define(s->current, var_side->u.var.name, kind);
        if (shadow) {
            if (original_sym->type) shadow->type = type_copy(original_sym->type);
            shadow->index = ref.index;      // 复用原槽位，代码生成读写的是同一个变量
            shadow->is_null_value = 0;
            shadow->is_initialized = 1;
        }
        symref_dispose(&ref);
    }
}

// ============================================================================
// 访问者模式 - 单遍处理
// ============================================================================
void visit_list(Semantic* s, AstList* list);

// 「as 恒失败」诊断用：TypeKind → 短名（只覆盖本诊断会涉及的标量；其余回「该类型」）
//   见 visit_type_check.inc 里 AST_AS_CAST 的两条静态诊断（string→数值/bool、数值→bool）
static const char* sem_as_kind_name(TypeKind k) {
    switch (k) {
        case TYPE_INT:    return "int";
        case TYPE_FLOAT:  return "float";
        case TYPE_BOOL:   return "bool";
        case TYPE_BIGINT: return "bigint";
        case TYPE_STRING: return "string";
        case TYPE_I8:  return "i8";
        case TYPE_U8:  return "u8";
        case TYPE_I16: return "i16";
        case TYPE_U16: return "u16";
        case TYPE_I32: return "i32";
        case TYPE_U32: return "u32";
        case TYPE_I64: return "i64";
        case TYPE_U64: return "u64";
        case TYPE_F32: return "f32";
        case TYPE_F64: return "f64";
        default: return "该类型";
    }
}


// ============================================================================
// `use` 的类型/符号导入 —— **唯一实现**（US1，2026-10-12，见 docs/类型可见性与use逃生阀修复追踪.md）
// ----------------------------------------------------------------------------
// 两个调用方（必须同口径 ⇒ 共用同一份代码，而不是两份分派 ✗）：
//   ① 源码里的 `use m.Pt`      —— visitinc/visit_module.inc 的 case AST_USE
//   ② 类型位置写限定名 `m.Pt`   —— semantic_visit_func.c 的 resolve_alias_in_type
//      （US1：`m.Pt` 等价于**隐式 use m.Pt** ⇒ 报错文案 / 可用性判据 / 重复导入的宽容度
//        三条都不可能与 ① 漂移 ✓ —— 本仓纪律：单一事实来源 ✓）
// ★ 文案前缀用「类型导入失败：」而不是「use 语句错误：」（2026-10-12 随 US1 改）——
//   同一个实现现在也被**类型位置的限定名**（`m.Pt`）调用，那里源码里根本没有 `use` 语句
//   ⇒ 原文案会让人去找一条不存在的语句 ✗（实测 `m.Nope x` 报"use 语句错误"）
// 返回：1 = 导入成功（含"同一类型重复导入 ⇒ 静默跳过"，故**幂等** ✓）；0 = 已报错
//   ⚠ 幂等必须守住：类型归一化会对同一个类型跑不止一遍（函数签名在 visit_func 与符号
//     登记处各归一化一次 ✓）⇒ 第二遍必须无害通过 ✓
// ============================================================================
int semantic_import_use_symbol(Semantic* s, const char* module_name, const char* symbol_name,
                               int line, int column) {
            // use 语句：将模块中的 struct 类型导入到当前作用域
            // 注意：use 只支持 struct 类型，不支持 func、var 和 enum
            // - struct：编译时类型，需要用于变量声明（如 Point p）
            // - func/var：运行时实体，必须通过模块名访问（如 module.func()）
            // - enum：成员访问通过模块名（如 module.enum.member），不需要 use

            // 查找导入的模块
            ImportedModuleInfo* module_info = find_imported_module(s, module_name);
            if (!module_info) {
                // E2: 模块名拼错提示——在已导入模块的别名里找相似（use md.Point，别名是 m）
                const char* alias_names[64];
                int alias_count = 0;
                for (int i = 0; i < s->imported_module_count && alias_count < 64; i++) {
                    if (s->imported_modules[i].alias) {
                        alias_names[alias_count++] = s->imported_modules[i].alias;
                    }
                }
                const char* hint = get_similar_in_names(alias_names, alias_count, module_name);
                if (!hint[0]) hint = native_builtin_module_hint(module_name);
                if (hint[0]) {
                    error_add_at_fmt(ERR_SEMANTIC, line, column,
                                     "类型导入失败：模块 '%s' 未导入%s", module_name, hint);
                } else {
                    error_add_at_fmt(ERR_SEMANTIC, line, column,
                                     "类型导入失败：模块 '%s' 未导入", module_name);
                }
            return 0;
            }

            // ★ native 模块的类型（2026-09-27）—— 走**独立通道**，且必须在下面的"没有符号表"检查之前：
            //   native 模块的字段表在 native 类型规格注册表里，`sym_table` 恒为 NULL
            //   ⇒ 过去 `use dirs.DirEntry` 只会得到"模块 'dirs' 没有符号表"这句废话。
            //   位置放在"模块已导入"检查之后 ⇒ 与源码模块**同一规矩**：use 之前先 import ✓
            {
                const char* actual_module = native_resolve_module_alias(module_name);
                if (native_is_module(actual_module)) {
                    const NativeStructSpec* nss =
                        native_find_module_struct_spec(actual_module, symbol_name);
                    if (!nss) {
                        // 模块是 native，但没这个类型 ⇒ 把**可用类型**列出来（照 builtin 模块提示的风格）
                        const char* names[16];
                        int n = native_list_module_struct_specs(actual_module, names, 16);
                        if (n > 0) {
                            char list[192];
                            int off = 0;
                            list[0] = '\0';
                            for (int i = 0; i < n; i++) {
                                int w = snprintf(list + off, sizeof(list) - (size_t)off,
                                                 "%s%s", i == 0 ? "" : " / ", names[i]);
                                if (w <= 0) break;
                                off += (w < (int)sizeof(list) - off) ? w : (int)sizeof(list) - off - 1;
                                if (off >= (int)sizeof(list) - 1) break;
                            }
                            error_add_at_fmt(ERR_SEMANTIC, line, column,
                                     "类型导入失败：native 模块 '%s' 没有类型 '%s'（可用类型: %s）",
                                     module_name, symbol_name, list);
                        } else {
                            // ★ 2026-10-10 文案修正：原来只说"没有导出任何类型" ✗ ——
                            //   实测 `use maths.not_a_real_func` 报的就是这句 ✗，而 maths 明明
                            //   导出了一堆**函数** ✓ ⇒ 写的人以为是 maths 有问题，其实是用法不对 ✓：
                            //   `use` 只能导入**类型** ✓（本仓既有写法 `use SDL3.(Event, Renderer, …)` ✓）。
                            error_add_at_fmt(ERR_SEMANTIC, line, column,
                                     "类型导入失败：native 模块 '%s' 里没有类型 '%s'，而且它一个类型都没导出"
                                     "（use 只能导入**类型** ✓；模块里的函数/常量请直接写 '%s.名字(...)' 调用 ✓）",
                                     module_name, symbol_name, module_name);
                        }
            return 0;
                    }
                    // ⚠ 这里的 return 值分两种（**抽取函数时踩过坑**，见下）：
                    //   成功 ⇒ 1（原 case 级 `break` = 正常结束本 case ✓）
                    //   失败 ⇒ 0（原代码也是 break 出去，但那时错误已经报过 ⇒ 0 让 US1 侧去毒化 ✓）
                    //   抽取时的教训：原 case 里这一句 `break;` 位于 `if (!…)` **之外**（是成功出口 ✓），
                    //     我一开始当成"失败分支"改成了 `return 0` ⇒ 所有 native 限定名（`d.DirEntry`）
                    //     都被判成"导入失败"、**静默毒化成 any** ✗（局部变量/形参位置因为 any 合法
                    //     而看不出来，只有 struct 字段位置报"字段不能使用 any"才暴露 ✓）
                    int native_ok = semantic_import_native_type(s, nss);
                    if (!native_ok) {
                        error_add_at_fmt(ERR_SEMANTIC, line, column,
                                 "类型导入失败：'%s' 已在当前作用域中定义（与 native 类型同名）"
                                 "—— 请改用别的名字，或直接用该 native 类型",
                                 symbol_name);
                    }
                    return native_ok ? 1 : 0;
                }
            }

            if (!module_info->sym_table) {
                error_add_at_fmt(ERR_SEMANTIC, line, column,
                                 "类型导入失败：模块 '%s' 没有符号表", module_name);
            return 0;
            }

            // 尝试查找 struct 定义
            ModuleStructSymbol* struct_sym = module_symbol_table_find_struct(module_info->sym_table, symbol_name);
            if (struct_sym) {
                // 将 struct/cstruct 定义注册到当前作用域
                // cstruct 使用 SYM_CSTRUCT，struct 使用 SYM_STRUCT，以便 codegen 生成正确的指令
                SymKind kind;
                if (struct_sym->is_cstruct) {
                    kind = SYM_CSTRUCT;
                } else {
                    kind = SYM_STRUCT;
                }
                Symbol* sym = scope_define(s->current, symbol_name, kind);
                if (!sym) {
                    // scope_define 返回 NULL 可能是因为同名冲突
                    Symbol* existing = scope_resolve_local(s->current, symbol_name);
                    if (existing && existing->type && (existing->type->kind == TYPE_STRUCT || existing->type->kind == TYPE_CSTRUCT)) {
                        // 检查是否是同一类型的重复导入（来自 import_type_deps 的隐式导入）
                        // 如果现有符号是同一类型名，允许重复 use（跳过不报错）
                        if (existing->type->struct_name && strcmp(existing->type->struct_name, symbol_name) == 0) {
                            // 同一类型的重复导入，静默跳过
            return 1;
                        }
                        error_add_at_fmt(ERR_SEMANTIC, line, column,
                                 "类型导入失败：'%s' 已在当前作用域中定义（来自其他模块的 use），请使用模块名限定访问",
                                 symbol_name);
                    }
            return 0;
                }
                if (sym) {
                    // 字段与泛型参数：走**唯一实现**（与 import_type_deps 那份收成一份 ——
                    // 此前两份的字段重建深度不同：那份**不用 type_info**，实测把
                    // Array[Array[int]] 搬成 Array[Array]。见 semantic_type_utils.c 的
                    // semantic_attach_struct_fields 说明与
                    // assert/test_alias_type_deps_struct_fields.leno）
                    semantic_attach_struct_fields(sym, struct_sym);
                }
                // 全局 struct_def 注册 + 方法占位符注册：走**唯一实现**
                // （此前这里与 import_type_deps 那份各写一遍，那份**不填 param_types /
                //   type_params** ⇒ 泛型方法经 alias 那条路静默跳过类型检查。
                //   见 semantic_type_utils.c 的 semantic_register_struct_from_module 说明
                //   与 assert/test_generic_type_param_paths.leno）
                semantic_register_struct_from_module(s, struct_sym);
            return 1;
            }

            // 尝试查找 clib 类型（优先查符号表，再查函数返回类型）
            {
                int clib_found = 0;
                // 先尝试从模块符号表的 clibs 中查找
                ModuleClibSymbol* clib_sym_tbl = NULL;
                if (module_info->sym_table) {
                    clib_sym_tbl = module_symbol_table_find_clib(module_info->sym_table, symbol_name);
                }
                if (clib_sym_tbl) {
                    clib_found = 1;
                    SymKind kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                    Symbol* sym = scope_define(s->current, symbol_name, kind);
                    if (sym) {
                        sym->kind = SYM_TYPE;
                        sym->type = type_new(TYPE_CLIB);
                        sym->type->struct_name = strdup(symbol_name);
                        // 填充 clib 函数签名
                        sym->clib_func_count = clib_sym_tbl->func_count;
                        sym->clib_func_names = (char**)malloc(sizeof(char*) * clib_sym_tbl->func_count);
                        sym->clib_func_return_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * clib_sym_tbl->func_count);
                        sym->clib_func_param_counts = (int*)malloc(sizeof(int) * clib_sym_tbl->func_count);
                        sym->clib_func_param_types = (TypeInfo***)malloc(sizeof(TypeInfo**) * clib_sym_tbl->func_count);
                        for (int fi = 0; fi < clib_sym_tbl->func_count; fi++) {
                            sym->clib_func_names[fi] = strdup(clib_sym_tbl->funcs[fi].name);
                            // 构造返回类型：Ptr[T] 需要 element_type
                            if (clib_sym_tbl->funcs[fi].return_type == TYPE_PTR_GENERIC
                                && clib_sym_tbl->funcs[fi].return_element_type != TYPE_PTR) {
                                sym->clib_func_return_types[fi] = type_ptr_generic(
                                    type_new(clib_sym_tbl->funcs[fi].return_element_type));
                            } else {
                                sym->clib_func_return_types[fi] = type_new(clib_sym_tbl->funcs[fi].return_type);
                            }
                            sym->clib_func_param_counts[fi] = clib_sym_tbl->funcs[fi].param_count;
                            sym->clib_func_param_types[fi] = (TypeInfo**)malloc(sizeof(TypeInfo*) * clib_sym_tbl->funcs[fi].param_count);
                            for (int pi = 0; pi < clib_sym_tbl->funcs[fi].param_count; pi++) {
                                // 构造参数类型：Ptr[T] 需要 element_type
                                if (clib_sym_tbl->funcs[fi].param_types[pi] == TYPE_PTR_GENERIC
                                    && clib_sym_tbl->funcs[fi].param_element_types
                                    && clib_sym_tbl->funcs[fi].param_element_types[pi] != TYPE_PTR) {
                                    sym->clib_func_param_types[fi][pi] = type_ptr_generic(
                                        type_new(clib_sym_tbl->funcs[fi].param_element_types[pi]));
                                } else {
                                    sym->clib_func_param_types[fi][pi] = type_new(clib_sym_tbl->funcs[fi].param_types[pi]);
                                }
                            }
                        }
                    }
                }
                // 回退：通过检查函数返回类型
                if (!clib_found) {
                    for (int fi = 0; fi < module_info->sym_table->func_count && !clib_found; fi++) {
                        ModuleFuncSymbol* func = &module_info->sym_table->funcs[fi];
                        if (func->return_type == TYPE_CLIB && func->return_struct_name &&
                            strcmp(func->return_struct_name, symbol_name) == 0) {
                            clib_found = 1;
                            SymKind kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                            Symbol* sym = scope_define(s->current, symbol_name, kind);
                            if (sym) {
                                sym->kind = SYM_TYPE;
                                sym->type = type_new(TYPE_CLIB);
                                sym->type->struct_name = strdup(symbol_name);
                                // 从源模块符号表复制 clib 函数签名，实现编译期参数检查
                                ModuleClibSymbol* src_clib = module_symbol_table_find_clib(module_info->sym_table, symbol_name);
                                if (src_clib && src_clib->func_count > 0) {
                                    sym->clib_func_count = src_clib->func_count;
                                    sym->clib_func_names = (char**)malloc(sizeof(char*) * src_clib->func_count);
                                    sym->clib_func_return_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * src_clib->func_count);
                                    sym->clib_func_param_counts = (int*)malloc(sizeof(int) * src_clib->func_count);
                                    sym->clib_func_param_types = (TypeInfo***)malloc(sizeof(TypeInfo**) * src_clib->func_count);
                                    for (int ci = 0; ci < src_clib->func_count; ci++) {
                                        sym->clib_func_names[ci] = strdup(src_clib->funcs[ci].name);
                                        if (src_clib->funcs[ci].return_type == TYPE_PTR_GENERIC
                                            && src_clib->funcs[ci].return_element_type != TYPE_PTR) {
                                            sym->clib_func_return_types[ci] = type_ptr_generic(
                                                type_new(src_clib->funcs[ci].return_element_type));
                                        } else {
                                            sym->clib_func_return_types[ci] = type_new(src_clib->funcs[ci].return_type);
                                        }
                                        sym->clib_func_param_counts[ci] = src_clib->funcs[ci].param_count;
                                        sym->clib_func_param_types[ci] = (TypeInfo**)malloc(sizeof(TypeInfo*) * src_clib->funcs[ci].param_count);
                                        for (int pi = 0; pi < src_clib->funcs[ci].param_count; pi++) {
                                            if (src_clib->funcs[ci].param_types[pi] == TYPE_PTR_GENERIC
                                                && src_clib->funcs[ci].param_element_types
                                                && src_clib->funcs[ci].param_element_types[pi] != TYPE_PTR) {
                                                sym->clib_func_param_types[ci][pi] = type_ptr_generic(
                                                    type_new(src_clib->funcs[ci].param_element_types[pi]));
                                            } else {
                                                sym->clib_func_param_types[ci][pi] = type_new(src_clib->funcs[ci].param_types[pi]);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                // ⚠ 这一句原来写成 `if (clib_found) break;` —— 它是 **case 级** break（成功路径），
                //   不是循环内的 break ⇒ 抽取函数时**必须**改成 `return 1` ✓
                //   （2026-10-12 抽取时正是漏了它，编译器报 "break statement not within loop or switch"）
                if (clib_found) return 1;
            }

            // 尝试查找 enum 定义
            if (module_info->sym_table) {
                ModuleEnumSymbol* enum_sym = module_symbol_table_find_enum(module_info->sym_table, symbol_name);
                if (enum_sym) {
                    Symbol* sym = scope_define(s->current, symbol_name, SYM_TYPE);
                    if (sym) {
                        sym->type = type_new(TYPE_ENUM);
                        sym->type->struct_name = strdup(symbol_name);
                        // 存储 enum 成员信息到 symbol，用于 AST_NUM 转换
                        sym->enum_value_count = enum_sym->member_count;
                        sym->enum_value_names = (char**)malloc(sizeof(char*) * enum_sym->member_count);
                        sym->enum_values = (int64_t*)malloc(sizeof(int64_t) * enum_sym->member_count);
                        for (int mi = 0; mi < enum_sym->member_count; mi++) {
                            sym->enum_value_names[mi] = strdup(enum_sym->member_names[mi]);
                            sym->enum_values[mi] = enum_sym->member_values[mi];
                        }
                    }
            return 1;
                }
            }

            // 尝试查找 face 定义
            ModuleFaceSymbol* face_sym = module_symbol_table_find_face(module_info->sym_table, symbol_name);
            if (face_sym) {
                // 将 face 定义注册到当前作用域
                // face 类型使用 SYM_TYPE，不占用运行时索引
                Symbol* sym = scope_define(s->current, symbol_name, SYM_TYPE);
                if (sym) {
                    sym->type = type_new(TYPE_FACE);
                    sym->type->struct_name = strdup(symbol_name);
                }
                // 同时注册 face 定义到全局表，用于类型检查
                if (!face_def_find(symbol_name)) {
                    ObjFaceDef* fdef = face_def_new(symbol_name, face_sym->method_count);
                    if (fdef) {
                        for (int i = 0; i < face_sym->method_count; i++) {
                            fdef->methods[i].name = strdup(face_sym->methods[i].name);
                            fdef->methods[i].return_type = type_new(face_sym->methods[i].return_type);
                            fdef->methods[i].param_count = 0;
                            fdef->methods[i].param_types = NULL;
                        }
                        face_def_register(fdef);
                    }
                }
            return 1;
            }

            // 尝试查找 alias 定义
            ModuleAliasSymbol* alias_sym = module_symbol_table_find_alias(module_info->sym_table, symbol_name);
            if (alias_sym) {
                // 将 alias 注册为类型符号，让后续类型解析能识别
                // 注：import 时已将模块的 export alias 注册到解析器别名表，
                // 这里只需注册到语义作用域即可
                Symbol* sym = scope_define(s->current, symbol_name, SYM_TYPE);
                if (sym && alias_sym->type_info) {
                    sym->type = type_copy(alias_sym->type_info);
                    // 递归导入 alias 底层类型依赖（如 EventHandler = func(Event):bool 中的 Event）
                    import_type_deps(s, module_info, alias_sym->type_info);
                }
            return 1;
            }

            // 尝试查找函数定义（use module.funcName 导入函数）
            ModuleFuncSymbol* func_sym = module_symbol_table_find_func(module_info->sym_table, symbol_name);
            if (func_sym) {
                Symbol* sym = scope_define(s->current, symbol_name, SYM_FUNC_ALIAS);
                if (sym) {
                    sym->alias_module_name = strdup(module_name);
                    sym->alias_func_name = strdup(symbol_name);
                    // 设置返回类型信息，用于类型推断
                    if (func_sym->return_type != TYPE_ANY && func_sym->return_type != TYPE_INFER) {
                        if (func_sym->return_type_info) {
                            sym->type = type_copy(func_sym->return_type_info);
                        } else {
                            sym->type = type_new(func_sym->return_type);
                            if (func_sym->return_struct_name) {
                                sym->type->struct_name = strdup(func_sym->return_struct_name);
                            }
                        }
                    } else {
                        sym->type = type_new(TYPE_ANY);
                    }
                }
            return 1;
            }

            // 尝试查找 const 常量定义
            if (module_info->sym_table) {
                ModuleVarSymbol* var_sym = module_symbol_table_find_var(module_info->sym_table, symbol_name);
                if (var_sym && var_sym->is_const) {
                    error_add_at_fmt(ERR_SEMANTIC, line, column,
                                     "use 语句不能导入常量 '%s'，请使用 %s.%s 访问", symbol_name, module_name, symbol_name);
            return 0;
                }
            }

            // 尝试查找 cfunc 定义（C 回调函数签名）
            if (module_info->sym_table) {
                ModuleCfuncSymbol* cfunc_sym = module_symbol_table_find_cfunc(module_info->sym_table, symbol_name);
                if (cfunc_sym) {
                    // 注册为 SYM_CFUNC 符号
                    Symbol* sym = scope_define(s->current, symbol_name, SYM_CFUNC);
                    if (sym) {
                        TypeInfo* cfunc_type = type_new(TYPE_CFUNC);
                        cfunc_type->struct_name = strdup(symbol_name);
                        sym->type = cfunc_type;

                        // 存储 cfunc 签名信息（从 TypeKind 重建 TypeInfo*）
                        sym->cfunc_param_count = cfunc_sym->param_count;
                        if (cfunc_sym->param_count > 0) {
                            sym->cfunc_param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * cfunc_sym->param_count);
                            for (int pi = 0; pi < cfunc_sym->param_count; pi++) {
                                // Ptr[T] 需要 element_type
                                if (cfunc_sym->param_types[pi] == TYPE_PTR_GENERIC
                                    && cfunc_sym->param_element_types
                                    && cfunc_sym->param_element_types[pi] != TYPE_PTR) {
                                    sym->cfunc_param_types[pi] = type_ptr_generic(
                                        type_new(cfunc_sym->param_element_types[pi]));
                                } else {
                                    sym->cfunc_param_types[pi] = type_new(cfunc_sym->param_types[pi]);
                                }
                            }
                        } else {
                            sym->cfunc_param_types = NULL;
                        }

                        // 构造返回类型
                        if (cfunc_sym->return_type == TYPE_PTR_GENERIC
                            && cfunc_sym->return_element_type != TYPE_PTR) {
                            sym->cfunc_return_type = type_ptr_generic(
                                type_new(cfunc_sym->return_element_type));
                        } else {
                            sym->cfunc_return_type = type_new(cfunc_sym->return_type);
                        }
                    }
            return 1;
                }
            }

            // 没有找到 struct、clib、cfunc、face、alias 或函数
            // E1: 符号名拼错提示——在模块符号表所有名字桶里找最相似的候选
            const char* hint = module_symbol_table_hint(module_info->sym_table, symbol_name);
            if (hint[0]) {
                error_add_at_fmt(ERR_SEMANTIC, line, column,
                         "类型导入失败：模块 '%s' 中没有类型或函数 '%s'%s",
                         module_name, symbol_name, hint);
            } else {
                error_add_at_fmt(ERR_SEMANTIC, line, column,
                         "类型导入失败：模块 '%s' 中没有类型或函数 '%s'",
                         module_name, symbol_name);
            }
            return 0;   // 落到这里 = 模块里没有这个类型/符号（错误已由上面各分支报出 ✓）
}

// 常量整数求值：字面量 + **负数字面量**（`-1` 在 AST 里是 AST_UNARY(TOK_MINUS, AST_NUM)，
//   不是 AST_NUM ✓）。只服务"两端都是编译期常量才下判断"的检查（如 for 空区间）——
//   变量边界无法静态确定 ⇒ 返回 0（调用方不猜 ✓）
static int ast_const_int(Ast* e, long long* out) {
    if (!e || !out) return 0;
    if (e->kind == AST_NUM && !e->u.num.is_float && !e->u.num.is_bigint) {
        *out = (long long)e->u.num.value;
        return 1;
    }
    if (e->kind == AST_UNARY && !e->u.unary.is_postfix && e->u.unary.operand &&
        e->u.unary.operand->kind == AST_NUM && !e->u.unary.operand->u.num.is_float &&
        !e->u.unary.operand->u.num.is_bigint) {
        long long v = (long long)e->u.unary.operand->u.num.value;
        if (e->u.unary.op == TOK_MINUS) { *out = -v; return 1; }
        if (e->u.unary.op == TOK_PLUS)  { *out = v;  return 1; }
    }
    return 0;
}

// ============================================================================
// 模块级变量初始化顺序检查（2026-10-23）
// ----------------------------------------------------------------------------
// 背景：模块变量会被"前向引用预扫描"注册进符号表，于是
//         var B = A + 1        （A 在后面才声明）
//   能通过类型检查 —— 但顶层执行**严格按源码顺序**，B 初始化时 A 还是 null，
//   运行时报的是「加法运算: null 不能参与运算」，看着像"这个值恰好是 null"，
//   指不到真因（顺序）。
//
// 做法：顶层（ast == s->root）visit 完之后再走一遍，按源码顺序维护
//       "已声明变量"集合，拦截 init 表达式里引用了后面才声明的模块变量。
//   ⚠ 只看顶层：函数体里引用后面的变量是**合法**的（调用时机不定），不能误伤
//   ⚠ 只查 SYM_MODULE / SYM_GLOBAL：局部变量天然按顺序
//   ⚠ 递归只覆盖几种确定存在的 AST 节点，遇到别的就停 —— 少报比误报安全
// ============================================================================

static int mv_in_set(char** names, int n, const char* name) {
    for (int i = 0; i < n; i++) {
        if (names[i] && strcmp(names[i], name) == 0) return 1;
    }
    return 0;
}

// mv_check_expr 的"是否真的属于本模块的变量"判据靠这张**全量清单**：
//   `all_vars` = 本文件顶层**所有** var/const/解构声明的名字（含 export 包裹的）。
//   ⚠ 为什么需要它：模块级**函数**的符号 kind 也是 SYM_MODULE（与模块变量同一种），
//     AST_VAR 上分不出来 ⇒ 只看 kind 会把"函数当值用"全判成"变量在声明前使用" ✗
//     实测两处（都是合法写法）：
//        export var absFunc = localAbs     // 把函数重导出成变量
//        export int fromFunc = helper()    // 调用后面/前面定义的函数
//     ⇒ 只有"名字确实是本模块的模块变量、且还没走到它的声明"才报 ✓
static void mv_check_expr(Semantic* s, Ast* e, char** declared, int n,
                          char** all_vars, int all_n) {
    (void)s;
    if (!e) return;
    switch (e->kind) {
        case AST_VAR: {
            SymRef* r = &e->u.var.ref;
            if ((r->kind == SYM_MODULE || r->kind == SYM_GLOBAL) && r->name &&
                mv_in_set(all_vars, all_n, r->name) &&
                !mv_in_set(declared, n, r->name)) {
                error_add_at_fmt(ERR_SEMANTIC, e->line, e->column,
                    "变量 '%s' 在声明前使用（模块级变量按源码顺序初始化，此刻它还是 null）\n"
                    "  提示: 把它挪到这个变量之前声明；函数不受此限制（调用可以写在定义之前）",
                    r->name);
            }
            return;
        }
        case AST_BINOP:
            mv_check_expr(s, e->u.binop.l, declared, n, all_vars, all_n);
            mv_check_expr(s, e->u.binop.r, declared, n, all_vars, all_n);
            return;
        case AST_UNARY:
            mv_check_expr(s, e->u.unary.operand, declared, n, all_vars, all_n);
            return;
        case AST_CALL:
            mv_check_expr(s, e->u.call.callee, declared, n, all_vars, all_n);
            for (int i = 0; i < (int)e->u.call.args.count; i++) {
                mv_check_expr(s, e->u.call.args.items[i], declared, n, all_vars, all_n);
            }
            return;
        case AST_INDEX:      // A[0]
            mv_check_expr(s, e->u.index.obj, declared, n, all_vars, all_n);
            mv_check_expr(s, e->u.index.index, declared, n, all_vars, all_n);
            return;
        case AST_SLICE:      // A[1:2]
            mv_check_expr(s, e->u.slice.obj, declared, n, all_vars, all_n);
            mv_check_expr(s, e->u.slice.start, declared, n, all_vars, all_n);
            mv_check_expr(s, e->u.slice.end, declared, n, all_vars, all_n);
            return;
        default:
            return;     // 其它节点不深入（少报不误报）
    }
}

// `export` 会把声明**包一层** AST_EXPORT ⇒ 判定"这是什么声明"之前先剥包装层。
//   ★ 2026-10-24：原来直接看 stmt->kind，`export var/const` 整个被跳过 ⇒ 两个 bug：
//     ① 这些名字从没进"已声明"集合 ⇒ `export const LEVEL_INFO = 1` 之后的
//        `int _logLevel = LEVEL_INFO` 被误报「在声明前使用」✗（LenoHack/lib/hack_log.leno:32，
//        连带 3 个文件编不过）；
//     ② 它们的 init 也没被检查（真·前向引用漏报）。
//   export 只影响**可见性**，不影响**初始化顺序** ✓
static Ast* mv_unwrap_export(Ast* stmt) {
    if (stmt && stmt->kind == AST_EXPORT && stmt->u.export.decl) return stmt->u.export.decl;
    return stmt;
}

// ============================================================================
// 同一语句列表内的**重名定义**检查（2026-11-05）
// ----------------------------------------------------------------------------
// 此前已经查过：模块级变量（mv_check_top）、struct 重名/重复字段、extend 重名。
// 唯独**函数**没查：`func f(){}` + `func f(){}` 两个定义都"成功"，
// 函数表（func_table_add）与符号表都由后者**静默覆盖**前者 ⇒
//   ① 调用点永远只看到第二个（实测 `func dup` 返回 "second"，一个字都不报 ✗）；
//   ② 同名但签名不同也不会被当成重载（Leno 没有重载），调用第一个的签名反而报
//     「参数类型不匹配」—— 看着像调用点写错，实际是定义被顶掉。
// 这里按**同一语句列表**扫（块内同名函数 / 函数与 struct、clib、cfunc 同名）：
//   · 只比同一列表 ⇒ 合法的"局部函数遮蔽外层函数"不受影响 ✓
//   · 顶层由 semantic.c 的两个入口调用（顶层不走 AST_BLOCK 分支）；
//     嵌套块由 visit_block.inc 的 AST_BLOCK 分支调用 ✓
// ============================================================================
static Ast* dup_unwrap_export(Ast* stmt) {
    if (stmt && stmt->kind == AST_EXPORT && stmt->u.export.decl) return stmt->u.export.decl;
    return stmt;
}

// 取本列表里"会占用同名符号"的声明名（目前只认会与函数撞车的几种）
static const char* dup_decl_name(Ast* d) {
    if (!d) return NULL;
    switch (d->kind) {
        case AST_STRUCT_DEF: return d->u.struct_def.name;
        case AST_CLIB_DEF:   return d->u.clib_def.name;
        case AST_CFUNC_DECL: return d->u.cfunc_decl.name;
        default:             return NULL;
    }
}

static const char* dup_decl_kind_name(Ast* d) {
    if (!d) return "声明";
    switch (d->kind) {
        case AST_STRUCT_DEF: return "struct";
        case AST_CLIB_DEF:   return "clib";
        case AST_CFUNC_DECL: return "cfunc";
        default:             return "声明";
    }
}

void semantic_check_dup_funcs(Ast* block) {
    if (!block || block->kind != AST_BLOCK) return;
    AstList* list = &block->u.block;
    for (int i = 0; i < list->count; i++) {
        Ast* cur = dup_unwrap_export(list->items[i]);
        if (!cur || cur->kind != AST_FUNC_DEF || !cur->u.func.name) continue;

        for (int j = 0; j < i; j++) {
            Ast* prev = dup_unwrap_export(list->items[j]);
            if (!prev) continue;

            if (prev->kind == AST_FUNC_DEF && prev->u.func.name &&
                strcmp(prev->u.func.name, cur->u.func.name) == 0) {
                error_add_at_fmt(ERR_SEMANTIC, cur->line, cur->column,
                    "重复的函数定义 '%s'（第一次定义在第 %d 行）—— 此前第二个定义会**静默覆盖**"
                    "第一个，调用点永远只看到后者；Leno 没有同名重载，请改名或删掉其中一个",
                    cur->u.func.name, prev->line);
                break;
            }

            const char* dname = dup_decl_name(prev);
            if (dname && strcmp(dname, cur->u.func.name) == 0) {
                error_add_at_fmt(ERR_SEMANTIC, cur->line, cur->column,
                    "函数 '%s' 与第 %d 行的 %s 同名 —— 两者共用同一个名字，"
                    "实际只有一个能被解析到（另一个静默失效）；请给函数换个名字",
                    cur->u.func.name, prev->line, dup_decl_kind_name(prev));
                break;
            }
        }
    }
}

// 供 semantic.c 的两个入口调用（它们直接 visit_list 顶层块，不走 AST_BLOCK 分支）
void mv_check_top(Semantic* s, Ast* block) {
    int total = (int)block->u.block.count;
    // 全量清单（见 mv_check_expr 上面那段说明）：本文件顶层所有 var/const/解构声明的名字
    int all_cap = total * 2 + 8;
    char** all_vars = (char**)calloc((size_t)all_cap, sizeof(char*));
    if (!all_vars) return;
    int all_n = 0;
    for (int i = 0; i < total; i++) {
        Ast* decl = mv_unwrap_export(block->u.block.items[i]);
        if (decl->kind == AST_VAR_DECL) {
            if (all_n >= all_cap) {
                all_cap *= 2;
                all_vars = (char**)realloc(all_vars, sizeof(char*) * (size_t)all_cap);
                if (!all_vars) return;
            }
            // ★ 2026-10-24（round8 N12）：**顶层重复声明** ⇒ 报错。
            //   原来顶层脚本块不建作用域 ⇒ `var d = 1` + `var d = 2` 静默后者覆盖前者
            //   （而 main(){} 块内同样的写法会报「重复定义」✗ 两处口径不一致）。
            //   在这里判是顺手的：本次扫描本来就把顶层所有声明名收集了一遍 ✓
            if (decl->u.var_decl.name &&
                mv_in_set(all_vars, all_n, decl->u.var_decl.name)) {
                error_add_at_fmt(ERR_SEMANTIC, decl->line, decl->column,
                    "重复的模块级变量 '%s'（同一文件顶层只能声明一次；原来的行为是"
                    "**后者静默覆盖前者**）—— 想改值请写 `%s = 新值`（不带 var/const），"
                    "或给其中一个换名字",
                    decl->u.var_decl.name, decl->u.var_decl.name);
            }
            all_vars[all_n++] = decl->u.var_decl.name;
        } else if (decl->kind == AST_DESTRUCT_DECL) {
            for (int k = 0; k < decl->u.destruct_decl.slot_count; k++) {
                if (all_n >= all_cap) {
                    all_cap *= 2;
                    all_vars = (char**)realloc(all_vars, sizeof(char*) * (size_t)all_cap);
                    if (!all_vars) return;
                }
                all_vars[all_n++] = decl->u.destruct_decl.names[k];
            }
        }
    }

    int cap = 16, n = 0;
    char** declared = (char**)calloc(cap, sizeof(char*));
    if (!declared) { free(all_vars); return; }

    for (int i = 0; i < total; i++) {
        Ast* decl = mv_unwrap_export(block->u.block.items[i]);

        if (decl->kind == AST_VAR_DECL) {
            // 先查 init（此时它引用的模块变量必须已经在集合里）
            if (decl->u.var_decl.init) {
                mv_check_expr(s, decl->u.var_decl.init, declared, n, all_vars, all_n);
            }
            if (n >= cap) {
                cap *= 2;
                declared = (char**)realloc(declared, sizeof(char*) * cap);
                if (!declared) { free(all_vars); return; }
            }
            declared[n++] = decl->u.var_decl.name;
        } else if (decl->kind == AST_DESTRUCT_DECL) {
            if (decl->u.destruct_decl.init) {
                mv_check_expr(s, decl->u.destruct_decl.init, declared, n, all_vars, all_n);
            }
            for (int k = 0; k < decl->u.destruct_decl.slot_count; k++) {
                if (n >= cap) {
                    cap *= 2;
                    declared = (char**)realloc(declared, sizeof(char*) * cap);
                    if (!declared) { free(all_vars); return; }
                }
                declared[n++] = decl->u.destruct_decl.names[k];
            }
        }
        // 其余语句（函数定义 / import / 表达式语句）不登记
    }
    free(declared);
    free(all_vars);
}

// ----------------------------------------------------------------------------
// switch 重复 case 值检查（2026-11-05）
//   此前 `case 1 { … } case 1 { … }` 静默通过，第二个 case 永远不可能命中，
//   但编译器一声不吭（与"case 类型不可达"那条同族的静默坑）。
//   这里只比**编译期可确定的常量**（整数 / 浮点 / 字符串 / bool / null，含负数字面量）；
//   区间 case（AST_RANGE）与运行期表达式跳过 —— 不做猜测，宁漏勿误报。
//   用**警告**而非错误：与 WARN_SWITCH_CASE_TYPE 同口径，不打断既有代码。
// ----------------------------------------------------------------------------
typedef struct {
    int kind;       // 1=整数(含枚举/bigint) 2=浮点 3=字符串 4=bool 5=null
    long long iv;
    double fv;
    const char* sv; // 字符串正文 / bigint 字符串
    int line;
    int col;
} SwCaseKey;

static int sw_case_key(Ast* v, SwCaseKey* k) {
    if (!v || !k) return 0;
    memset(k, 0, sizeof(*k));
    k->line = v->line;
    k->col = v->column;
    if (v->kind == AST_UNARY) {
        long long iv = 0;
        if (ast_const_int(v, &iv)) { k->kind = 1; k->iv = iv; return 1; }
        return 0;
    }
    if (v->kind == AST_NUM) {
        if (v->u.num.is_bigint) {
            if (!v->u.num.bigint_str) return 0;
            k->kind = 1;
            k->sv = v->u.num.bigint_str;
            return 1;
        }
        if (v->u.num.is_float) { k->kind = 2; k->fv = v->u.num.value; return 1; }
        k->kind = 1;
        k->iv = (long long)v->u.num.value;
        return 1;
    }
    if (v->kind == AST_STRING) {
        if (!v->u.string.value) return 0;
        k->kind = 3;
        k->sv = v->u.string.value;
        return 1;
    }
    if (v->kind == AST_BOOL) { k->kind = 4; k->iv = v->u.boolean; return 1; }
    if (v->kind == AST_NULL) { k->kind = 5; return 1; }
    return 0;
}

static int sw_key_same(const SwCaseKey* a, const SwCaseKey* b) {
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
        case 1:   // 整数 / bigint
            if (a->sv || b->sv) return a->sv && b->sv && strcmp(a->sv, b->sv) == 0;
            return a->iv == b->iv;
        case 2: return a->fv == b->fv;
        case 3: return a->sv && b->sv && strcmp(a->sv, b->sv) == 0;
        case 4: return a->iv == b->iv;
        case 5: return 1;   // null
        default: return 0;
    }
}

static void sw_check_dup_cases(Ast* sw) {
    if (!sw || sw->kind != AST_SWITCH) return;
    int cap = 64, n = 0;
    SwCaseKey* seen = (SwCaseKey*)calloc((size_t)cap, sizeof(SwCaseKey));
    if (!seen) return;
    // 区间 case 的去重：只比**两端都是整数字面量**的区间（变量端点无法静态确定 ⇒ 不猜 ✓）
    typedef struct { long long lo, hi; int inclusive; int line; } SwRangeKey;
    int rcap = 32, rn = 0;
    SwRangeKey* rseen = (SwRangeKey*)calloc((size_t)rcap, sizeof(SwRangeKey));
    for (int i = 0; i < sw->u.switch_.case_count; i++) {
        for (int j = 0; j < sw->u.switch_.cases[i].values.count; j++) {
            Ast* v = sw->u.switch_.cases[i].values.items[j];
            if (!v) continue;
            if (v->kind == AST_RANGE && rseen) {
                // ★ 2026-11-07：重复的**区间** case（`case 1:3` 写两遍）此前零诊断 ✗
                long long lo = 0, hi = 0;
                if (ast_const_int(v->u.range.start, &lo) &&
                    ast_const_int(v->u.range.end, &hi)) {
                    int hit = -1;
                    for (int p = 0; p < rn; p++) {
                        if (rseen[p].lo == lo && rseen[p].hi == hi &&
                            rseen[p].inclusive == v->u.range.inclusive) { hit = p; break; }
                    }
                    if (hit >= 0) {
                        warning_add_at_fmt(WARN_DUPLICATE_CASE, v->line, v->column,
                            "重复的区间 case（与第 %d 行的 case 相同）—— switch 命中前面的分支后即跳出，"
                            "这个分支**永远不会执行**；删掉重复项", rseen[hit].line);
                    } else {
                        // ★ 2026-11-07：区间**部分重叠**此前也零诊断 ✗
                        //   `case 1:4 { } case 3:6 { }` ⇒ 3、4 会被前一个分支先命中，
                        //   后一个分支在这段区间内不可达。switch 区间是闭区间（`tmp >= lo && tmp <= hi`）。
                        for (int p = 0; p < rn; p++) {
                            if (lo <= rseen[p].hi && rseen[p].lo <= hi) {
                                warning_add_at_fmt(WARN_OVERLAP_CASE, v->line, v->column,
                                    "case 区间 %lld:%lld 与第 %d 行的区间重叠 —— switch 按顺序匹配，"
                                    "重叠部分会被前面的分支先命中，后一个分支在这段区间内**永远不会执行**；"
                                    "请把两个区间改成互不重叠，或合并成一个",
                                    lo, hi, rseen[p].line);
                                break;
                            }
                        }
                    }
                    if (rn >= rcap) {
                        rcap *= 2;
                        SwRangeKey* grown = (SwRangeKey*)realloc(rseen, sizeof(SwRangeKey) * (size_t)rcap);
                        if (grown) rseen = grown;
                    }
                    if (rn < rcap) {
                        rseen[rn].lo = lo; rseen[rn].hi = hi;
                        rseen[rn].inclusive = v->u.range.inclusive;
                        rseen[rn].line = v->line;
                        rn++;
                    }
                }
                continue;
            }
            SwCaseKey k;
            if (!sw_case_key(v, &k)) continue;
            int hit = -1;
            for (int p = 0; p < n; p++) {
                if (sw_key_same(&seen[p], &k)) { hit = p; break; }
            }
            if (hit >= 0) {
                if (seen[hit].line == k.line) {
                    warning_add_at_fmt(WARN_DUPLICATE_CASE, k.line, k.col,
                        "同一个 case 的取值列表里重复了相同的值 —— 一个 case 只匹配一次，"
                        "重复项没有意义；删掉重复项");
                } else {
                    warning_add_at_fmt(WARN_DUPLICATE_CASE, k.line, k.col,
                        "重复的 case 值（与第 %d 行的 case 相同）—— switch 命中前面的分支后即跳出，"
                        "这个 case **永远不会执行**；删掉重复项，或合并成 `case a, b { … }`",
                        seen[hit].line);
                }
            }
            if (n >= cap) {
                cap *= 2;
                SwCaseKey* grown = (SwCaseKey*)realloc(seen, sizeof(SwCaseKey) * (size_t)cap);
                if (!grown) { free(seen); return; }
                seen = grown;
            }
            seen[n++] = k;
        }
    }
    free(seen);
    free(rseen);

    // ★ 2026-11-06：`case is Type` 的重复分支同样不可达（此前只查了值 case ✗）
    //   `case is P { … } case is P { … }` 实测静默取第一个、第二个永远不执行。
    //   与值 case 同口径：用警告（不打断既有代码），只比**编译期类型名**。
    typedef struct { char name[128]; int line; } SwSeenType;
    int tcap = 32, tn = 0;
    SwSeenType* tseen = (SwSeenType*)calloc((size_t)tcap, sizeof(SwSeenType));
    if (!tseen) return;
    for (int i = 0; i < sw->u.switch_.case_count; i++) {
        struct SwitchCase* c = &sw->u.switch_.cases[i];
        if (!c->is_type_match) continue;
        for (int t = 0; t < c->match_type_count; t++) {
            TypeInfo* ti = (c->match_types && t < c->match_type_count)
                               ? c->match_types[t]
                               : (t == 0 ? c->match_type : NULL);
            if (!ti || ti->kind == TYPE_ANY || ti->kind == TYPE_INFER) continue;
            char nm[128];
            // 诊断里用**源码里的写法**（struct 用 struct_name，如 `P`）；
            //   type_to_string 对 struct 会加 "struct " 前缀，出现在 `case is …` 里反而不像源码。
            if (ti->kind == TYPE_STRUCT && ti->struct_name) {
                snprintf(nm, sizeof(nm), "%s", ti->struct_name);
            } else {
                snprintf(nm, sizeof(nm), "%s", type_to_string(ti));
            }
            int hit = -1;
            for (int p = 0; p < tn; p++) {
                if (strcmp(tseen[p].name, nm) == 0) { hit = p; break; }
            }
            if (hit >= 0) {
                int line = c->line > 0 ? c->line : sw->line;
                int col = c->column > 0 ? c->column : sw->column;
                if (tseen[hit].line == line) {
                    warning_add_at_fmt(WARN_DUPLICATE_CASE, line, col,
                        "同一个 case 的类型列表里重复了 '%s' —— 同一分支只匹配一次，"
                        "重复项没有意义；删掉重复项",
                        nm);
                } else {
                    warning_add_at_fmt(WARN_DUPLICATE_CASE, line, col,
                        "重复的 case 类型 '%s'（与第 %d 行的 case 相同）—— switch 命中前面的分支后即跳出，"
                        "这个分支**永远不会执行**；删掉重复项，或合并成 `case is %s, …`",
                        nm, tseen[hit].line, nm);
                }
            }
            if (tn >= tcap) {
                tcap *= 2;
                SwSeenType* grown = (SwSeenType*)realloc(tseen, sizeof(SwSeenType) * (size_t)tcap);
                if (!grown) { free(tseen); return; }
                tseen = grown;
            }
            snprintf(tseen[tn].name, sizeof(tseen[tn].name), "%s", nm);
            tseen[tn].line = c->line > 0 ? c->line : sw->line;
            tn++;
        }
    }
    free(tseen);
}

// ----------------------------------------------------------------------------
// 递归深度守护（2026-10-30）
//   超长表达式（`a + b + …` 几千项）会让 AST 深度线性增长；visit 是纯递归，
//   实测 ~4800 项时默认 8MB 栈被耗尽 ⇒ SIGSEGV（不是错误诊断）。
//   超过上限：报一条明确诊断并停止下探；调用方仍会正常收尾/退出，不会崩溃。
//   2000 层远高于真实代码（现有全部用例远低于它），同时低于实测崩溃阈值。
// ----------------------------------------------------------------------------
#define SEM_MAX_AST_DEPTH 2000

static void visit_node(Semantic* s, Ast* ast);

void visit(Semantic* s, Ast* ast) {
    if (!ast) return;
    if (s->ast_depth >= SEM_MAX_AST_DEPTH) {
        // 只在刚越过上限时报一次（外层每层都会走到这里，不抑制会刷屏）
        if (s->ast_depth == SEM_MAX_AST_DEPTH) {
            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                             "表达式/语句嵌套过深（超过 %d 层）：请把超长表达式拆成多个中间变量",
                             SEM_MAX_AST_DEPTH);
        }
        return;
    }
    s->ast_depth++;
    visit_node(s, ast);
    s->ast_depth--;
}

static void visit_node(Semantic* s, Ast* ast) {
    if (!ast) return;

    switch (ast->kind) {
        // 块处理
        #include "visitinc/visit_block.inc"

        // 函数定义
        #include "visitinc/visit_func_def.inc"

        // 变量和赋值
        #include "visitinc/visit_var.inc"

        // 控制流语句
        #include "visitinc/visit_control.inc"

        // 表达式
        #include "visitinc/visit_expr.inc"

        // 简单语句
        #include "visitinc/visit_stmt.inc"

        // 模块相关
        #include "visitinc/visit_module.inc"

        // 异常处理
        #include "visitinc/visit_exception.inc"

        // 类型检查
        #include "visitinc/visit_type_check.inc"

        // 类型定义
        #include "visitinc/visit_type_def.inc"

        // FFI 相关
        #include "visitinc/visit_ffi.inc"

        // 枚举和别名
        #include "visitinc/visit_enum.inc"

        // struct 初始化
        #include "visitinc/visit_struct_init.inc"

        // await
        #include "visitinc/visit_await.inc"

        // 字段访问
        #include "visitinc/visit_field_access.inc"

        default:
            break;
    }
}

// ============================================================================
// visit_list 函数
// ============================================================================
#include "visitinc/visit_list.inc"
