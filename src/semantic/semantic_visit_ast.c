#include "semantic_internal.h"

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
                                      const char* field_name, int* out_line) {
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
                    return 1;
                }
                return 0;   // 找到但没标 pri ⇒ 公有 ✓
            }
        }
    }
    return 0;
}

static int pri_imported_method_private(Semantic* s, const char* struct_name,
                                       const char* method_name, int* out_line) {
    if (!s || !struct_name || !method_name) return 0;
    for (int mi = 0; mi < s->imported_module_count; mi++) {
        ModuleSymbolTable* t = s->imported_modules[mi].sym_table;
        if (!t) continue;
        ModuleStructMethod* m = module_symbol_table_find_struct_method(t, struct_name, method_name);
        if (m) {
            if (m->is_private) {
                if (out_line) *out_line = m->line;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

// 统一的报错文案（字段/方法共用 ✓ —— 这套东西值钱的地方就在这句话 ✓）
static void pri_report(Semantic* s, Ast* ast, const char* kind, const char* struct_name,
                       const char* member, int decl_line) {
    (void)s;
    char msg[BUFFER_MEDIUM];
    snprintf(msg, sizeof(msg),
        "'%s.%s' 是 pri 私有%s（%s 第 %d 行）—— 只有 %s 自己的方法内部可以访问它；"
        "要对外开放就把 'pri' 去掉（默认全公有）",
        struct_name, member, kind, error_get_filename(), decl_line, struct_name);
    error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
}

// 字段访问检查（读 ✓ 写 ✓ 都走 AST_FIELD_ACCESS ✓ ⇒ 一处即可 ✓）
// ⚠ 非 static：semantic_type.c 的 infer_expr_type 里也要用（跨模块的表那时才加载好 ✓）
void pri_check_field_access(Semantic* s, Ast* ast, TypeInfo* obj_type, const char* field_name) {
    if (!obj_type || obj_type->kind != TYPE_STRUCT || !obj_type->struct_name) return;
    if (pri_inside_own_method(s, obj_type->struct_name)) return;
    int line = 0;
    if (pri_field_is_private(s, obj_type->struct_name, field_name, &line)) {
        pri_report(s, ast, "字段", obj_type->struct_name, field_name, line);
        return;
    }
    // 本文件里没有这个 struct 的定义（= 它是**导入**进来的 ✓）⇒ 查模块符号表 ✓
    if (pri_find_struct_def(s, obj_type->struct_name) == NULL &&
        pri_imported_field_private(s, obj_type->struct_name, field_name, &line)) {
        pri_report(s, ast, "字段", obj_type->struct_name, field_name, line);
    }
}

// 方法调用检查（obj.method() ✓）
// ⚠ 非 static：semantic_type.c 的表达式侧方法解析里也要用（同字段侧的理由 ✓）
void pri_check_method_access(Semantic* s, Ast* ast, TypeInfo* obj_type, const char* method_name) {
    if (!obj_type || obj_type->kind != TYPE_STRUCT || !obj_type->struct_name) return;
    if (pri_inside_own_method(s, obj_type->struct_name)) return;
    int line = 0;
    if (pri_method_is_private(s, obj_type->struct_name, method_name, &line)) {
        pri_report(s, ast, "方法", obj_type->struct_name, method_name, line);
        return;
    }
    // 跨模块兜底（同字段侧 ✓）
    if (pri_find_struct_def(s, obj_type->struct_name) == NULL &&
        pri_imported_method_private(s, obj_type->struct_name, method_name, &line)) {
        pri_report(s, ast, "方法", obj_type->struct_name, method_name, line);
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
        if (!original_sym || !original_sym->is_null_value) { if (ref.name) free(ref.name); continue; }
        if (!ref.name) continue;

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
        free(ref.name);
    }
}

// ============================================================================
// 访问者模式 - 单遍处理
// ============================================================================
void visit_list(Semantic* s, AstList* list);

void visit(Semantic* s, Ast* ast) {
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