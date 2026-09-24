#include "semantic_internal.h"

// ============================================================================
// 类型推断工具函数
// ============================================================================

// ============================================================================
// 把模块符号表里的一条 struct/cstruct 符号，按**完整精度**搬进当前作用域的符号
// ----------------------------------------------------------------------------
// 这是"怎么把模块里的 struct 字段搬进当前作用域"的**唯一实现**。此前有两份：
//   · visit_module.inc 的 AST_USE  —— 优先用 type_info（正确）
//   · semantic_visit_ast.c 的 import_type_deps —— 只用扁平字段重建（**丢嵌套泛型**）
// 实测差异（assert/test_alias_type_deps_struct_fields.leno）：声明方 `Array[Array[int]] grid`
//   经 import_type_deps 进来 → `Array[Array]`，取一层元素后更是**裸 Array**；
//   经 AST_USE 进来          → `Array[Array[int]]` / `Array[int]` ✓
// ⇒ 同一个类型因"怎么被导入"得到不同精度，且丢失方向是**静默变粗**。
//
// 规则：字段类型优先用 type_info（扫描器对含 `[` 的类型会存完整 TypeInfo，支持
// Array[Array[int]] / Dict[K,V] / Ptr[T] 等嵌套）；没有 type_info 时才按扁平字段重建
// （此时才用得上 element_type/element_struct_name）。nullable 必须单独传播 ——
// type_info 里可能没带（另一处漏过这一条，见 visit_module.inc 的注释）。
//
// 不负责：把 struct_def / 方法占位符注册到全局表与 func_table —— 那部分两个调用点
// 各有差异（dup 处理、错误分支不同），属另一轮收敛；本函数只填"符号自己的类型与字段"。
// ============================================================================
void semantic_attach_struct_fields(Symbol* sym, const ModuleStructSymbol* ssym) {
    if (!sym || !ssym) return;

    TypeKind tk = ssym->is_cstruct ? TYPE_CSTRUCT : TYPE_STRUCT;
    sym->type = type_new(tk);
    sym->type->struct_name = strdup(ssym->name ? ssym->name : "");

    sym->struct_field_count = ssym->field_count;
    sym->struct_field_names = (char**)malloc(sizeof(char*) * ssym->field_count);
    sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ssym->field_count);
    for (int i = 0; i < ssym->field_count; i++) {
        sym->struct_field_names[i] = strdup(ssym->fields[i].name);
        // ★ 优先完整类型信息（嵌套泛型靠它）
        if (ssym->fields[i].type_info) {
            sym->struct_field_types[i] = type_copy(ssym->fields[i].type_info);
            if (ssym->fields[i].nullable) {
                sym->struct_field_types[i]->nullable = 1;
            }
        } else {
            // 向后兼容：从扁平字段重建类型
            sym->struct_field_types[i] = type_new(ssym->fields[i].type);
            if (ssym->fields[i].struct_name) {
                sym->struct_field_types[i]->struct_name = strdup(ssym->fields[i].struct_name);
            }
            if (ssym->fields[i].nullable) {
                sym->struct_field_types[i]->nullable = 1;
            }
            // 重建 Array[T]/Dict[K,V] 的**第一层**元素类型（嵌套层靠上面的 type_info）
            if ((ssym->fields[i].type == TYPE_ARRAY || ssym->fields[i].type == TYPE_DICT)
                && ssym->fields[i].element_type != TYPE_PTR) {
                TypeInfo* elem_type = type_new(ssym->fields[i].element_type);
                if (ssym->fields[i].element_struct_name) {
                    elem_type->struct_name = strdup(ssym->fields[i].element_struct_name);
                }
                sym->struct_field_types[i]->element_type = elem_type;
            }
        }
    }

    // 泛型类型参数（如 Box[T] 的 [T]）
    sym->struct_type_param_count = ssym->type_param_count;
    if (ssym->type_param_count > 0 && ssym->type_param_names) {
        sym->struct_type_params = (char**)malloc(sizeof(char*) * ssym->type_param_count);
        for (int i = 0; i < ssym->type_param_count; i++) {
            sym->struct_type_params[i] = strdup(ssym->type_param_names[i]);
        }
    }
}

// ============================================================================
// 把模块符号表里的一条 struct/cstruct 符号，**完整注册**进当前编译
// ----------------------------------------------------------------------------
// 与 semantic_attach_struct_fields 的分工：那个只管"符号自己的类型与字段"；这里管两件事：
//   ① 全局 struct_def 注册（类型检查 / face 实现检查用；cstruct 有自己的表，不注册）
//   ② 方法占位符注册到 func_table（方法调用解析用）
//
// 这是这件事的**唯一实现**。此前 visit_module.inc 的 AST_USE 与
// semantic_visit_ast.c 的 import_type_deps 各写一份，**后者贫**：
//   方法占位符只填 name / pcnt / 返回类型，**不填 param_types 与 type_params，
//   也不带返回类型的泛型实参** ⇒ 泛型方法经那条路进来时 resolve_generic_in_type 拿不到
//   类型参数表 ⇒ 返回类型停在**未替换的 `T`**，而未约束的泛型参数与任何类型都兼容
//   ⇒ 类型检查被**静默跳过**。
// 实测（assert/test_generic_type_param_paths.leno 判据 3）：
//   `func probe(Box[int] b): string { return b.get() }`
//     直接 `use m.Box`   ⇒ 正确报「返回类型不匹配：期望 string，实际 int」
//     经 `use m.F`（alias）⇒ **什么都不报**、exit 0
// ============================================================================
void semantic_register_struct_from_module(Semantic* s, const ModuleStructSymbol* ssym) {
    if (!s || !ssym || !ssym->name) return;
    const char* symbol_name = ssym->name;

    // ---- ① 全局 struct_def（用于类型检查 / face 实现检查）----
    if (!ssym->is_cstruct && !struct_def_find(symbol_name)) {
        ObjStructDef* sdef = struct_def_new(symbol_name, ssym->field_count, ssym->method_count);
        if (sdef) {
            // 泛型类型参数数量与名称
            sdef->type_param_count = ssym->type_param_count;
            if (ssym->type_param_count > 0 && ssym->type_param_names) {
                sdef->type_param_names = (char**)malloc(sizeof(char*) * ssym->type_param_count);
                for (int tpi = 0; tpi < ssym->type_param_count; tpi++) {
                    sdef->type_param_names[tpi] = strdup(ssym->type_param_names[tpi]);
                }
            }
            // 方法名（模块符号表里是 "StructName::method_name" 格式 ⇒ 剥成纯方法名）
            for (int i = 0; i < ssym->method_count; i++) {
                const char* full_method_name = ssym->methods[i].name;
                const char* method_name = full_method_name;
                const char* sep = strstr(full_method_name, "::");
                if (sep) method_name = sep + 2;
                sdef->methods[i].name = strdup(method_name);
            }
            // impl 信息（face 实现）
            if (ssym->impl_count > 0) {
                sdef->impl_count = ssym->impl_count;
                sdef->impl_names = (char**)malloc(sizeof(char*) * ssym->impl_count);
                for (int i = 0; i < ssym->impl_count; i++) {
                    sdef->impl_names[i] = strdup(ssym->impl_names[i]);
                }
            }
            struct_def_register(sdef);
        }
    }

    // ---- ② 方法占位符 → func_table（即使 struct_def 已存在也要注册方法）----
    for (int mi = 0; mi < ssym->method_count; mi++) {
        const char* full_method_name = ssym->methods[mi].name;
        Ast* existing = func_table_find(&s->func_table, full_method_name);
        if (existing) continue;
        Ast* placeholder = ast_new(AST_FUNC_DEF, 0);
        if (!placeholder) continue;
        placeholder->u.func.name = strdup(full_method_name);
        // pcnt 含 self
        placeholder->u.func.pcnt = 1 + ssym->methods[mi].param_count;
        // 返回类型：优先完整 return_type_info（支持 TYPE_MULTI_RET 等复杂类型）
        if (ssym->methods[mi].return_type_info) {
            placeholder->u.func.return_type = type_copy(ssym->methods[mi].return_type_info);
        } else {
            placeholder->u.func.return_type = type_new(ssym->methods[mi].return_type);
            if (ssym->methods[mi].return_struct_name) {
                placeholder->u.func.return_type->struct_name = strdup(ssym->methods[mi].return_struct_name);
            }
            if (ssym->methods[mi].return_type == TYPE_GENERIC_PARAM && ssym->methods[mi].return_type_param_name) {
                placeholder->u.func.return_type->type_param_name = strdup(ssym->methods[mi].return_type_param_name);
            }
            // 返回类型的泛型实参（如 Holder[K] 里的 [K]）
            if (ssym->methods[mi].return_generic_count > 0 && ssym->methods[mi].return_generic_param_names) {
                placeholder->u.func.return_type->generic_count = ssym->methods[mi].return_generic_count;
                placeholder->u.func.return_type->generic_args = (TypeInfo**)malloc(sizeof(TypeInfo*) * ssym->methods[mi].return_generic_count);
                for (int gi = 0; gi < ssym->methods[mi].return_generic_count; gi++) {
                    // 是类型参数名（K/V/T）还是具体类型（int/string…）
                    int is_type_param = 0;
                    for (int tpi = 0; tpi < ssym->type_param_count; tpi++) {
                        if (ssym->type_param_names[tpi] &&
                            strcmp(ssym->methods[mi].return_generic_param_names[gi], ssym->type_param_names[tpi]) == 0) {
                            is_type_param = 1;
                            placeholder->u.func.return_type->generic_args[gi] = type_new(TYPE_GENERIC_PARAM);
                            placeholder->u.func.return_type->generic_args[gi]->type_param_name =
                                strdup(ssym->methods[mi].return_generic_param_names[gi]);
                            break;
                        }
                    }
                    if (!is_type_param) {
                        // 具体类型（简单判断）
                        TypeInfo* arg_type = type_new(TYPE_ANY);
                        if (strcmp(ssym->methods[mi].return_generic_param_names[gi], "int") == 0) arg_type = type_new(TYPE_INT);
                        else if (strcmp(ssym->methods[mi].return_generic_param_names[gi], "float") == 0) arg_type = type_new(TYPE_FLOAT);
                        else if (strcmp(ssym->methods[mi].return_generic_param_names[gi], "string") == 0) arg_type = type_new(TYPE_STRING);
                        else if (strcmp(ssym->methods[mi].return_generic_param_names[gi], "bool") == 0) arg_type = type_new(TYPE_BOOL);
                        placeholder->u.func.return_type->generic_args[gi] = arg_type;
                    }
                }
            }
        }
        // 参数类型（self + 各参数，含泛型参数名）
        int total_pcnt = 1 + ssym->methods[mi].param_count;
        placeholder->u.func.param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * total_pcnt);
        placeholder->u.func.param_types[0] = type_new(TYPE_STRUCT);
        placeholder->u.func.param_types[0]->struct_name = strdup(ssym->name);
        if (ssym->type_param_count > 0 && ssym->type_param_names) {
            placeholder->u.func.param_types[0]->generic_count = ssym->type_param_count;
            placeholder->u.func.param_types[0]->generic_args = (TypeInfo**)malloc(sizeof(TypeInfo*) * ssym->type_param_count);
            for (int tpi = 0; tpi < ssym->type_param_count; tpi++) {
                placeholder->u.func.param_types[0]->generic_args[tpi] = type_new(TYPE_GENERIC_PARAM);
                placeholder->u.func.param_types[0]->generic_args[tpi]->type_param_name = strdup(ssym->type_param_names[tpi]);
            }
        }
        for (int pi = 0; pi < ssym->methods[mi].param_count; pi++) {
            if (ssym->methods[mi].param_generic_names && ssym->methods[mi].param_generic_names[pi]) {
                placeholder->u.func.param_types[1 + pi] = type_new(TYPE_GENERIC_PARAM);
                placeholder->u.func.param_types[1 + pi]->type_param_name = strdup(ssym->methods[mi].param_generic_names[pi]);
            } else {
                placeholder->u.func.param_types[1 + pi] = type_new(ssym->methods[mi].param_types[pi]);
                // ⑯：聚合类型（struct/face/cstruct/clib）参数带上名字 —— 此前扫描器算出了名字
                // 却没处存（ModuleStructMethod 没有该字段）⇒ 跨模块方法的聚合参数不可判，只能跳过。
                if (ssym->methods[mi].param_struct_names && ssym->methods[mi].param_struct_names[pi]) {
                    placeholder->u.func.param_types[1 + pi]->struct_name =
                        strdup(ssym->methods[mi].param_struct_names[pi]);
                }
            }
        }
        // 泛型类型参数表（供 resolve_generic_in_type 用）—— 缺了它泛型替换就做不了
        if (ssym->type_param_count > 0 && ssym->type_param_names) {
            placeholder->u.func.type_param_count = ssym->type_param_count;
            placeholder->u.func.type_params = (char**)malloc(sizeof(char*) * ssym->type_param_count);
            for (int tpi = 0; tpi < ssym->type_param_count; tpi++) {
                placeholder->u.func.type_params[tpi] = strdup(ssym->type_param_names[tpi]);
            }
        }
        placeholder->u.func.default_count = 0;
        // ★ C2：把扫描器记下的 async 标记带到占位 AST 上 —— gen_method_call 就是靠
        //   `mdef->u.func.is_async` 决定发 OP_ASYNC_CALL 的（跨模块 struct 的 async 方法
        //   否则会退化成同步 CALL；虽然有运行期兜底，静态发出更明确）。
        placeholder->u.func.is_async = ssym->methods[mi].is_async;
        func_table_add(&s->func_table, full_method_name, placeholder);
    }
}

// ============================================================================
// 跨模块 struct 方法的实参检查（**唯一实现**）
// ----------------------------------------------------------------------------
// 为什么需要它：方法调用的实参检查在本仓有**两份拷贝**，而且两份犯**同一个**错 ——
//   都去**本编译单元的 root AST** 里找 struct 声明：
//     · visit_expr.inc（AST_CALL 形态）
//     · visit_module.inc（MODULE_CALL 形态 —— 实测 `pa.set(...)` 走的是这条）
//   而跨模块的 struct **不在本文件里** ⇒ struct_def_ast 恒为 NULL ⇒ 整块检查被跳过
//   ⇒ 跨模块方法的实参个数与类型**完全不检查**（而且与是否 use 无关）。
//   实测：`pa.set("oops")`（`set(int v)`）零诊断、exit=0，还照跑打印 `R=oops`；
//   而同文件同写法**会**报「set 第 1 个参数类型不匹配: 期望 int, 实际 string」。
// 数据来源 = semantic_register_struct_from_module 注册的**方法占位符**（key = "Struct::method"）：
//   pcnt = 1 + 参数数、param_types[0] = self、param_types[1+i] = 第 i 个参数 ⇒ 跨模块也拿得到。
// 只查"参数过多"与"逐参类型"，**不查参数不足**：占位符的 default_count 固定为 0
//   （扫描器不记方法默认值）⇒ 查少了会对"带默认值的方法"误报。
// 泛型参数（TYPE_GENERIC_PARAM，未约束）与 any 一律跳过 ⇒ 不误报。
// 返回：1 = 找到占位符并已按它检查；0 = 没有占位符（无从检查）。
int semantic_check_method_args_from_placeholder(Semantic* s, const char* struct_name,
                                                const char* method_name, AstList* args,
                                                int line, int column) {
    if (!s || !struct_name || !method_name || !args) return 0;

    char method_key[BUFFER_MEDIUM];
    snprintf(method_key, sizeof(method_key), "%s::%s", struct_name, method_name);
    Ast* ph = func_table_find(&s->func_table, method_key);
    int arity = args->count;

    if (ph && ph->u.func.param_types) {

    int param_count = ph->u.func.pcnt - 1;   // 去掉 self

    if (arity > param_count) {
        char msg[BUFFER_MEDIUM];
        snprintf(msg, sizeof(msg), "方法 '%s' 参数过多: 最多 %d 个, 实际 %d",
                 method_name, param_count, arity);
        error_add_at(ERR_SEMANTIC, line, column, msg);
        return 1;
    }

    for (int i = 0; i < arity; i++) {
        TypeInfo* expected_type = ph->u.func.param_types[i + 1];   // +1 跳过 self
        if (!expected_type || expected_type->kind == TYPE_ANY ||
            expected_type->kind == TYPE_GENERIC_PARAM) continue;

        // ---- 保守闸门：宁漏不误报（误报会破坏合法代码）----
        // 占位符的参数类型来自扫描器，**聚合类型（struct/face/cstruct/clib）的名字常不全**
        // （实测：`期望 struct`（无名） vs `实际 struct TreeNode`；`期望 face` vs 实现该 face 的
        //  `struct WidgetA`）—— 这类比较会造出一批**假报错**（套件里 7 个用例被误伤）。
        // 故：两边都是聚合类型时，只判"kind 相同 + 名字都齐"；否则（跨 kind 或名字缺失）跳过。
        int exp_agg = (expected_type->kind == TYPE_STRUCT || expected_type->kind == TYPE_FACE ||
                       expected_type->kind == TYPE_CSTRUCT || expected_type->kind == TYPE_CLIB);
        if (exp_agg) {
            TypeInfo* probe_type = infer_expr_type(s, args->items[i]);
            int arg_agg = probe_type && (probe_type->kind == TYPE_STRUCT || probe_type->kind == TYPE_FACE ||
                                         probe_type->kind == TYPE_CSTRUCT || probe_type->kind == TYPE_CLIB);
            // ★ 只在"**两边都是聚合**"时保守：那种情况下名字可能缺失或带模块前缀，比不出来会假报错。
            //   而"一边聚合、一边是基本类型"（如把 1 传给 Pair 参数）是**真**不符，必须报
            //   （第一版闸门把这类也跳过了 ⇒ 判据 ⑦ 实测静默）。
            if (arg_agg) {
                if (probe_type->kind != expected_type->kind ||
                    !expected_type->struct_name || !probe_type->struct_name ||
                    strchr(expected_type->struct_name, '.') || strchr(probe_type->struct_name, '.')) {
                    type_free(probe_type);
                    continue;
                }
            }
            if (probe_type) type_free(probe_type);
        }

        TypeInfo* arg_type = infer_expr_type(s, args->items[i]);
        if (arg_type && arg_type->kind != TYPE_ANY &&
            !type_is_compatible(expected_type, arg_type)) {
            char msg[BUFFER_MEDIUM];
            char idx_str[16];
            snprintf(idx_str, sizeof(idx_str), "%d", i + 1);
            format_type_error(msg, sizeof(msg),
                "%s3 第 %s4 个参数类型不匹配: 期望 %s1, 实际 %s2",
                expected_type, arg_type, method_name, idx_str);
            error_add_at(ERR_SEMANTIC, line, column, msg);
        }
        if (arg_type) type_free(arg_type);
    }
        return 1;
    }

    // ---- 没有占位符时（典型：只 `import ... as a`、**没** use ⇒ 谁都没注册占位符）----
    // 直接查**导入模块的符号表**（同一份扫描器数据的源头）。注意 `ModuleStructMethod` **没有
    // `param_struct_names`**（clib 那边有）⇒ 聚合类型参数的 struct 名拿不到，那类一律跳过
    // （保守闸门：宁漏不误报）；C 布局类型也跳过 —— Leno 侧的隐式转换规则这里没重建。
    for (int mi = 0; mi < s->imported_module_count; mi++) {
        ImportedModuleInfo* mod = &s->imported_modules[mi];
        if (!mod->sym_table) continue;
        ModuleStructSymbol* ssym = module_symbol_table_find_struct(mod->sym_table, struct_name);
        if (!ssym) continue;

        for (int k = 0; k < ssym->method_count; k++) {
            const char* full = ssym->methods[k].name;
            const char* sep = full ? strstr(full, "::") : NULL;
            const char* bare = sep ? sep + 2 : full;
            if (!bare || strcmp(bare, method_name) != 0) continue;

            int m_param_count = ssym->methods[k].param_count;
            if (arity > m_param_count) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "方法 '%s' 参数过多: 最多 %d 个, 实际 %d",
                         method_name, m_param_count, arity);
                error_add_at(ERR_SEMANTIC, line, column, msg);
                return 1;
            }

            for (int i = 0; i < arity; i++) {
                if (!ssym->methods[k].param_types) break;
                TypeKind ek = ssym->methods[k].param_types[i];
                if (ek == TYPE_ANY) continue;
                // ⑯：聚合类型现在带名字了（param_struct_names）⇒ 可以判；只有"聚合类型但没名字"才跳过。
                //    模块限定名（含 '.'）也跳过 —— 期望侧是裸名、实际侧可能是 `a.TreeNode`，
                //    两边字符串不等但其实是同一类型 ⇒ 直接比会假报错。
                char* ek_name = ssym->methods[k].param_struct_names ? ssym->methods[k].param_struct_names[i] : NULL;
                int ek_agg = (ek == TYPE_STRUCT || ek == TYPE_FACE || ek == TYPE_CSTRUCT || ek == TYPE_CLIB);
                if (ek_agg && (!ek_name || strchr(ek_name, '.'))) continue;
                if ((ek >= TYPE_I8 && ek <= TYPE_C_SSIZE) || ek == TYPE_F32 || ek == TYPE_F64 ||
                    ek == TYPE_STR8 || ek == TYPE_STR16) continue;
                if (ssym->methods[k].param_generic_names && ssym->methods[k].param_generic_names[i]) continue;

                TypeInfo* expected_type = type_new(ek);
                if (ek_agg && ek_name) expected_type->struct_name = strdup(ek_name);
                TypeInfo* arg_type = infer_expr_type(s, args->items[i]);
                if (arg_type && arg_type->kind != TYPE_ANY &&
                    !type_is_compatible(expected_type, arg_type)) {
                    char msg[BUFFER_MEDIUM];
                    char idx_str[16];
                    snprintf(idx_str, sizeof(idx_str), "%d", i + 1);
                    format_type_error(msg, sizeof(msg),
                        "%s3 第 %s4 个参数类型不匹配: 期望 %s1, 实际 %s2",
                        expected_type, arg_type, method_name, idx_str);
                    error_add_at(ERR_SEMANTIC, line, column, msg);
                }
                if (arg_type) type_free(arg_type);
                type_free(expected_type);
            }
            return 1;
        }
    }
    return 0;
}

// ============================================================================
// face 方法的实参检查（唯一实现）
// ----------------------------------------------------------------------------
// 位置由来（2026-09-18 打点定位，不靠猜）：`an.feed("oops")` 走的是 visit_module.inc 的
//   MODULE_CALL 那条路，且实测 `is_struct_method == 1`（`obj_kind == 14 == TYPE_FACE`）——
//   ⇒ 它被带进 struct 那套逻辑（按 "Animal::feed" 找占位符 ⇒ 找不到 ⇒ **整块跳过**）⇒
//   实参个数/类型全不检查（同文件也静默）。
// 数据来源：
//   · 同文件：**解析器的 face AST 有 `method_param_types`** ⇒ 可判逐参类型；
//   · 跨模块：`ModuleFaceMethodSymbol` 现在有 `param_types` / `param_struct_names`（⑰-2 补的，
//     同一次 `.lenosymc` bump：v27 → v28）⇒ 类型也判得动；旧缓存里没有这段 ⇒ 只判个数。
// 返回：1 = 找到该 face 并已判；0 = 没有该 face 的信息。
int semantic_check_face_method_args(Semantic* s, const char* face_name, const char* method_name,
                                    AstList* args, int line, int column) {
    if (!s || !face_name || !method_name || !args) return 0;
    int arity = args->count;

    // ① 同文件：face AST 里有每个方法的参数个数与类型
    if (s->root && s->root->kind == AST_BLOCK) {
        for (int i = 0; i < s->root->u.block.count; i++) {
            Ast* stmt = s->root->u.block.items[i];
            if (stmt->kind != AST_FACE_DEF || !stmt->u.face_def.name ||
                strcmp(stmt->u.face_def.name, face_name) != 0) continue;
            for (int j = 0; j < stmt->u.face_def.method_count; j++) {
                if (!stmt->u.face_def.method_names || !stmt->u.face_def.method_names[j]) continue;
                if (strcmp(stmt->u.face_def.method_names[j], method_name) != 0) continue;
                int pc = stmt->u.face_def.method_param_counts ? stmt->u.face_def.method_param_counts[j] : -1;
                if (pc >= 0 && arity > pc) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "方法 '%s' 参数过多: 最多 %d 个, 实际 %d",
                             method_name, pc, arity);
                    error_add_at(ERR_SEMANTIC, line, column, msg);
                    return 1;
                }
                TypeInfo*** pts = stmt->u.face_def.method_param_types;
                if (!pts || !pts[j]) return 1;
                for (int ai = 0; ai < arity; ai++) {
                    TypeInfo* expected_type = pts[j][ai];
                    if (!expected_type || expected_type->kind == TYPE_ANY ||
                        expected_type->kind == TYPE_GENERIC_PARAM) continue;
                    TypeInfo* arg_type = infer_expr_type(s, args->items[ai]);
                    if (!arg_type || arg_type->kind == TYPE_ANY) {
                        if (arg_type) type_free(arg_type);
                        continue;
                    }
                    // 聚合类型保守（与 struct 那支同规矩：kind 不同 / 名字缺 / 含模块限定名 ⇒ 不判）
                    int exp_agg = (expected_type->kind == TYPE_STRUCT || expected_type->kind == TYPE_FACE ||
                                   expected_type->kind == TYPE_CSTRUCT || expected_type->kind == TYPE_CLIB);
                    int arg_agg = (arg_type->kind == TYPE_STRUCT || arg_type->kind == TYPE_FACE ||
                                   arg_type->kind == TYPE_CSTRUCT || arg_type->kind == TYPE_CLIB);
                    if (exp_agg && arg_agg &&
                        (expected_type->kind != arg_type->kind ||
                         !expected_type->struct_name || !arg_type->struct_name ||
                         strchr(expected_type->struct_name, '.') || strchr(arg_type->struct_name, '.'))) {
                        type_free(arg_type);
                        continue;
                    }
                    if (!type_is_compatible(expected_type, arg_type)) {
                        char msg[BUFFER_MEDIUM];
                        char idx_str[16];
                        snprintf(idx_str, sizeof(idx_str), "%d", ai + 1);
                        format_type_error(msg, sizeof(msg),
                            "%s3 第 %s4 个参数类型不匹配: 期望 %s1, 实际 %s2",
                            expected_type, arg_type, method_name, idx_str);
                        error_add_at(ERR_SEMANTIC, line, column, msg);
                    }
                    type_free(arg_type);
                }
                return 1;
            }
            return 0;
        }
    }

    // ② 跨模块：模块符号表里的 face 方法符号
    //    ⑰-2 之前它只有 param_count ⇒ 只判"参数过多"；现在扫描器补上了 param_types /
    //    param_struct_names（同样一次 `.lenosymc` bump，v27 → v28）⇒ 类型也判得动了。
    for (int i = 0; i < s->imported_module_count; i++) {
        ImportedModuleInfo* m = &s->imported_modules[i];
        if (!m->sym_table) continue;
        ModuleFaceSymbol* fs = module_symbol_table_find_face(m->sym_table, face_name);
        if (!fs) continue;
        for (int fi = 0; fi < fs->method_count; fi++) {
            if (!fs->methods[fi].name || strcmp(fs->methods[fi].name, method_name) != 0) continue;
            if (arity > fs->methods[fi].param_count) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "方法 '%s' 参数过多: 最多 %d 个, 实际 %d",
                         method_name, fs->methods[fi].param_count, arity);
                error_add_at(ERR_SEMANTIC, line, column, msg);
                return 1;
            }
            if (!fs->methods[fi].param_types) return 1;   // 旧缓存/未记录 ⇒ 只判个数
            for (int ai = 0; ai < arity; ai++) {
                TypeKind ek = fs->methods[fi].param_types[ai];
                if (ek == TYPE_ANY || ek == TYPE_UNKNOWN) continue;
                // C 布局类型：Leno 侧的隐式转换规则不在此重建 ⇒ 跳过（宁漏不误报）
                if ((ek >= TYPE_I8 && ek <= TYPE_C_SSIZE) || ek == TYPE_F32 || ek == TYPE_F64 ||
                    ek == TYPE_STR8 || ek == TYPE_STR16) continue;
                char* ek_name = (fs->methods[fi].param_struct_names)
                                ? fs->methods[fi].param_struct_names[ai] : NULL;
                int exp_agg = (ek == TYPE_STRUCT || ek == TYPE_FACE ||
                               ek == TYPE_CSTRUCT || ek == TYPE_CLIB);
                // 聚合类型但名字缺/含模块限定名 ⇒ 判不准，跳过
                if (exp_agg && (!ek_name || strchr(ek_name, '.'))) continue;

                TypeInfo* expected_type = type_new(ek);
                if (exp_agg && ek_name) expected_type->struct_name = strdup(ek_name);
                TypeInfo* arg_type = infer_expr_type(s, args->items[ai]);
                if (arg_type && arg_type->kind != TYPE_ANY) {
                    int arg_agg = (arg_type->kind == TYPE_STRUCT || arg_type->kind == TYPE_FACE ||
                                   arg_type->kind == TYPE_CSTRUCT || arg_type->kind == TYPE_CLIB);
                    int skip = 0;
                    if (exp_agg && arg_agg &&
                        (arg_type->kind != expected_type->kind ||
                         !arg_type->struct_name || strchr(arg_type->struct_name, '.'))) {
                        skip = 1;   // 两边都是聚合：保守（与同文件那支同规矩）
                    }
                    if (!skip && !type_is_compatible(expected_type, arg_type)) {
                        char msg[BUFFER_MEDIUM];
                        char idx_str[16];
                        snprintf(idx_str, sizeof(idx_str), "%d", ai + 1);
                        format_type_error(msg, sizeof(msg),
                            "%s3 第 %s4 个参数类型不匹配: 期望 %s1, 实际 %s2",
                            expected_type, arg_type, method_name, idx_str);
                        error_add_at(ERR_SEMANTIC, line, column, msg);
                    }
                }
                if (arg_type) type_free(arg_type);
                type_free(expected_type);
            }
            return 1;
        }
    }
    return 0;
}

// 检查方法名是否是数组元素修改方法
// 返回：1 = 是，0 = 否
int type_utils_is_array_element_mutator(const char* method_name) {
    return (strcmp(method_name, "add") == 0 ||
            strcmp(method_name, "insert") == 0 ||
            strcmp(method_name, "pop") == 0 ||
            strcmp(method_name, "remove") == 0 ||
            strcmp(method_name, "clear") == 0 ||
            strcmp(method_name, "reverse") == 0 ||
            strcmp(method_name, "sort") == 0);
}

// 获取数组元素修改方法中，元素参数的位置
// 参数：
//   method_name - 方法名
//   is_module_call - 是否是模块调用（arrays.add）还是实例调用（arr.add）
// 返回：元素参数的索引，-1 表示不是元素修改方法
int type_utils_get_array_element_param_index(const char* method_name, int is_module_call) {
    if (strcmp(method_name, "add") == 0) {
        // add(arr, value) 或 arr.add(value)
        // value 是最后一个参数
        return is_module_call ? 1 : 0;
    }
    if (strcmp(method_name, "insert") == 0) {
        // insert(arr, index, value) 或 arr.insert(index, value)
        // value 是最后一个参数
        return is_module_call ? 2 : 1;
    }
    return -1;
}

// 尝试更新空数组的元素类型
// 当向 Array（元素类型未指定）添加第一个具体类型元素时，更新数组类型
// 注意：Array[any]（明确指定为 any）不会更新，保持 any 类型
//
// 参数：
//   s - 语义分析器
//   arr_sym - 数组变量符号
//   elem_type - 要设置的元素类型
// 返回：
//   1 = 更新了类型，0 = 未更新（数组已有具体类型或不是数组）
int type_utils_try_update_array_element_type(Symbol* arr_sym, TypeInfo* elem_type) {
    if (!arr_sym || !arr_sym->type || arr_sym->type->kind != TYPE_ARRAY) {
        return 0;
    }
    
    TypeInfo* current_elem = arr_sym->type->element_type;
    
    // 只有当前元素类型为空（未指定）时才更新
    // Array[any]（明确指定为 any）保持 any 类型，不更新
    if (!current_elem) {
        if (elem_type && elem_type->kind != TYPE_ANY) {
            arr_sym->type->element_type = type_copy(elem_type);
            return 1;
        }
    }
    
    return 0;
}

// 尝试更新嵌套数组的元素类型（递归版本）
// 用于 arr[0].add(1)、arr[0][0].add(1) 等任意层级嵌套情况
//
// 参数：
//   arr_sym - 外层数组变量符号
//   depth - 嵌套深度（arr[0] 是 1，arr[0][0] 是 2）
//   elem_type - 要设置的最内层元素类型
// 返回：
//   1 = 更新了类型，0 = 未更新
static int type_utils_try_update_nested_array_element_type_recursive(Symbol* arr_sym, int depth, TypeInfo* elem_type) {
    if (!arr_sym || !arr_sym->type || arr_sym->type->kind != TYPE_ARRAY) {
        return 0;
    }
    
    TypeInfo* current_type = arr_sym->type;
    
    // 根据深度逐层进入嵌套数组类型
    for (int i = 0; i < depth; i++) {
        if (!current_type->element_type || current_type->element_type->kind != TYPE_ARRAY) {
            return 0;
        }
        current_type = current_type->element_type;
    }
    
    // 现在 current_type 是最内层的数组类型，更新其元素类型
    TypeInfo* inner_elem = current_type->element_type;
    if (!inner_elem || inner_elem->kind == TYPE_ANY) {
        if (elem_type && elem_type->kind != TYPE_ANY) {
            if (current_type->element_type) {
                type_free(current_type->element_type);
            }
            current_type->element_type = type_copy(elem_type);
            return 1;
        }
    }
    
    return 0;
}

// 计算索引表达式的嵌套深度
// arr[0] -> 1, arr[0][0] -> 2, arr[0][0][0] -> 3
static int type_utils_get_index_depth(Ast* ast) {
    if (!ast || ast->kind != AST_INDEX) {
        return 0;
    }
    
    // 递归计算深度
    // 如果 obj 也是索引表达式，继续深入
    int parent_depth = type_utils_get_index_depth(ast->u.index.obj);
    
    // 检查当前索引是否是整数（数组访问）还是字符串（方法名）
    // 对于 arr[0][0].add() 这种情况，我们只计算整数索引
    if (ast->u.index.index && ast->u.index.index->kind == AST_NUM) {
        return parent_depth + 1;
    }
    
    return parent_depth;
}

// 尝试更新嵌套数组的元素类型（对外接口）
// 用于 arr[0].add(1) 这种情况，自动计算嵌套深度
//
// 参数：
//   arr_sym - 外层数组变量符号
//   index_ast - 索引表达式 AST（用于计算嵌套深度）
//   elem_type - 要设置的内部元素类型
// 返回：
//   1 = 更新了类型，0 = 未更新
int type_utils_try_update_nested_array_element_type_ex(Symbol* arr_sym, Ast* index_ast, TypeInfo* elem_type) {
    int depth = type_utils_get_index_depth(index_ast);
    if (depth <= 0) {
        return 0;
    }
    return type_utils_try_update_nested_array_element_type_recursive(arr_sym, depth, elem_type);
}

// 旧的接口，保持兼容性（只处理一层嵌套）
int type_utils_try_update_nested_array_element_type(Symbol* arr_sym, TypeInfo* elem_type) {
    return type_utils_try_update_nested_array_element_type_recursive(arr_sym, 1, elem_type);
}

// 从 AST 节点解析变量符号
// 支持：AST_VAR（变量名）、AST_INDEX（数组索引，返回数组变量）
// 返回：符号指针，未找到返回 NULL
Symbol* type_utils_resolve_var_symbol(Semantic* s, Ast* ast) {
    if (!ast) return NULL;
    
    if (ast->kind == AST_VAR) {
        return scope_resolve(s->current, ast->u.var.name);
    }
    
    // 对于索引表达式 arr[0]，返回 arr 的符号
    if (ast->kind == AST_INDEX) {
        return type_utils_resolve_var_symbol(s, ast->u.index.obj);
    }
    
    return NULL;
}

// ============================================================================
// 字典类型检查工具函数
// ============================================================================

// 检查方法名是否是字典元素修改方法
// 返回：1 = 是，0 = 否
int type_utils_is_dict_element_mutator(const char* method_name) {
    return (strcmp(method_name, "set") == 0 ||
            strcmp(method_name, "remove") == 0 ||
            strcmp(method_name, "clear") == 0);
}



// 获取字典元素修改方法中，元素参数的位置
// 参数：
//   method_name - 方法名
// 返回：元素参数的索引，-1 表示不是元素修改方法
int type_utils_get_dict_element_param_index(const char* method_name) {
    if (strcmp(method_name, "set") == 0) {
        // set(key, value) - value 是第2个参数，索引1
        return 1;
    }
    return -1;
}

// 尝试更新空字典的值类型
// 当向 Dict（值类型未指定）添加第一个具体类型值时，更新字典值类型
// 注意：Dict[string, any]（明确指定为 any）保持 any 类型
//
// 参数：
//   dict_sym - 字典变量符号
//   value_type - 要设置的值类型
// 返回：
//   1 = 更新了类型，0 = 未更新（字典已有具体类型或不是字典）
int type_utils_try_update_dict_value_type(Symbol* dict_sym, TypeInfo* value_type) {
    if (!dict_sym || !dict_sym->type || dict_sym->type->kind != TYPE_DICT) {
        return 0;
    }
    
    TypeInfo* current_value = dict_sym->type->value_type;
    
    // 只有当前值类型为空（未指定）时才更新
    // Dict[string, any]（明确指定为 any）保持 any 类型，不更新
    if (!current_value) {
        if (value_type && value_type->kind != TYPE_ANY) {
            dict_sym->type->value_type = type_copy(value_type);
            return 1;
        }
    }

    return 0;
}

// ============================================================================
// 安全格式化类型错误信息（避免 type_to_string 缓冲区覆盖）
// ============================================================================

// 安全格式化类型错误信息
// 参数：
//   buf - 输出缓冲区
//   buf_size - 缓冲区大小
//   fmt - 格式字符串，支持 %s 占位符（会被替换为 type1/type2/str1/str2）
//   type1, type2 - 要格式化的类型（可为 NULL）
//   str1, str2 - 额外的字符串参数（可为 NULL）
void format_type_error(char* buf, size_t buf_size, const char* fmt,
                       TypeInfo* type1, TypeInfo* type2,
                       const char* str1, const char* str2) {
    char type1_buf[128] = "";
    char type2_buf[128] = "";

    // 先保存 type1 的字符串表示
    if (type1) {
        const char* type1_str = type_to_string(type1);
        strncpy(type1_buf, type1_str, sizeof(type1_buf) - 1);
        type1_buf[sizeof(type1_buf) - 1] = '\0';
    }

    // 再获取 type2 的字符串表示（避免覆盖 type1 的缓冲区）
    if (type2) {
        const char* type2_str = type_to_string(type2);
        strncpy(type2_buf, type2_str, sizeof(type2_buf) - 1);
        type2_buf[sizeof(type2_buf) - 1] = '\0';
    }

    // 使用 snprintf 格式化输出
    // 简单的占位符替换：%s1 -> type1_buf, %s2 -> type2_buf, %s3 -> str1, %s4 -> str2
    const char* p = fmt;
    size_t offset = 0;
    buf[0] = '\0';

    while (*p && offset < buf_size - 1) {
        if (*p == '%' && *(p + 1) == 's') {
            // 检查是否有数字后缀
            if (*(p + 2) == '1') {
                // %s1 -> type1_buf
                size_t len = strlen(type1_buf);
                if (offset + len < buf_size - 1) {
                    memcpy(buf + offset, type1_buf, len);
                    offset += len;
                }
                p += 3;
            } else if (*(p + 2) == '2') {
                // %s2 -> type2_buf
                size_t len = strlen(type2_buf);
                if (offset + len < buf_size - 1) {
                    memcpy(buf + offset, type2_buf, len);
                    offset += len;
                }
                p += 3;
            } else if (*(p + 2) == '3') {
                // %s3 -> str1
                if (str1) {
                    size_t len = strlen(str1);
                    if (offset + len < buf_size - 1) {
                        memcpy(buf + offset, str1, len);
                        offset += len;
                    }
                }
                p += 3;
            } else if (*(p + 2) == '4') {
                // %s4 -> str2
                if (str2) {
                    size_t len = strlen(str2);
                    if (offset + len < buf_size - 1) {
                        memcpy(buf + offset, str2, len);
                        offset += len;
                    }
                }
                p += 3;
            } else {
                // 普通的 %s，使用 type1_buf
                size_t len = strlen(type1_buf);
                if (offset + len < buf_size - 1) {
                    memcpy(buf + offset, type1_buf, len);
                    offset += len;
                }
                p += 2;
            }
        } else {
            buf[offset++] = *p++;
        }
    }
    buf[offset] = '\0';
}

// ============================================================================
// 生成详细的类型错误提示（包含转换建议）
// ============================================================================

// 获取类型转换建议
// 根据期望类型和实际类型，返回转换建议字符串
// 简单编辑距离（Levenshtein），用于拼写建议
static int levenshtein(const char* a, const char* b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 50 || lb > 50) return 999;
    int d[51][51];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++)
            d[i][j] = (a[i-1] == b[j-1]) ? d[i-1][j-1]
                     : 1 + ((d[i-1][j] < d[i][j-1]) ?
                        (d[i-1][j] < d[i-1][j-1] ? d[i-1][j] : d[i-1][j-1]) :
                        (d[i][j-1] < d[i-1][j-1] ? d[i][j-1] : d[i-1][j-1]));
    return d[la][lb];
}

// G2: Python 习惯的大写首字面量（True/False/Null）——true/false/null 是关键字
// 不在任何符号表，符号表扫描给不出候选；大小写不敏感比对直接给出写法提示。
// 命中且大小写不一致时返回提示串，否则返回空串。
static const char* literal_case_hint(const char* name) {
    static char hint[128];
    hint[0] = '\0';
    if (!name || !name[0]) return hint;
    static const char* literal_keywords[] = {"true", "false", "null"};
    for (int i = 0; i < 3; i++) {
        if (_stricmp(name, literal_keywords[i]) == 0 && strcmp(name, literal_keywords[i]) != 0) {
            snprintf(hint, sizeof(hint),
                     "\n  提示: 字面量写法是小写 '%s'（Leno 大小写敏感）", literal_keywords[i]);
            return hint;
        }
    }
    return hint;
}

// 在当前作用域查找最相似的变量名，返回提示字符串（静态缓冲区）
const char* get_similar_name_hint(Scope* scope, const char* name) {
    static char hint[256];
    hint[0] = '\0';
    if (!scope || !name || !name[0]) return hint;

    // G2: 大写首字面量优先提示（True/False/Null → true/false/null）
    {
        const char* kw = literal_case_hint(name);
        if (kw[0]) return kw;
    }

    const char* best = NULL;
    int best_dist = 3;  // 最多允许 3 个编辑距离

    for (Scope* s = scope; s; s = s->parent) {
        for (int i = 0; i < s->sym_cnt; i++) {
            Symbol* sym = s->syms[i];
            if (!sym || !sym->name) continue;
            int dist = levenshtein(name, sym->name);
            if (dist < best_dist) {
                best_dist = dist;
                best = sym->name;
                if (dist == 0) break;
            }
        }
        if (best_dist == 0) break;
    }

    if (best && strcmp(best, name) != 0) {
        snprintf(hint, sizeof(hint), "\n  提示: 是否想输入 '%s'？", best);
    }
    return hint;
}

// 在给定名字集合中找最相似的（C1 struct 方法提示用），返回提示串（静态缓冲区）
const char* get_similar_in_names(const char** names, int count, const char* name) {
    static char hint[160];
    hint[0] = '\0';
    if (!names || count <= 0 || !name || !name[0]) return hint;
    const char* best = NULL;
    int best_dist = 3;
    for (int i = 0; i < count; i++) {
        if (!names[i]) continue;
        int dist = levenshtein(name, names[i]);
        if (dist < best_dist) {
            best_dist = dist;
            best = names[i];
            if (dist == 0) break;
        }
    }
    if (best && strcmp(best, name) != 0) {
        snprintf(hint, sizeof(hint), "\n  提示: 是否想用 '%s'？", best);
    }
    return hint;
}

// C2：未定义函数的相似名提示——依次在 函数表（用户函数）/ 内置 native 函数 /
// 作用域变量 中找最相似的名字（变量兜底覆盖"把变量当函数调用"的手误）
const char* get_undefined_func_hint(Semantic* s, const char* name) {
    static char hint[192];
    hint[0] = '\0';
    if (!s || !name || !name[0]) return hint;

    // G2: Python 习惯的大写首字面量（True/False/Null）优先提示
    {
        const char* kw = literal_case_hint(name);
        if (kw[0]) return kw;
    }

    const char* best = NULL;
    int best_dist = 3;  // 最多允许 2 次编辑距离

    // 1. 用户函数表
    if (s->func_table.entries) {
        for (int i = 0; i < s->func_table.capacity; i++) {
            for (FuncEntry* e = s->func_table.entries[i]; e; e = e->next) {
                if (!e->name) continue;
                int dist = levenshtein(name, e->name);
                if (dist < best_dist) {
                    best_dist = dist;
                    best = e->name;
                    if (dist == 0) break;
                }
            }
            if (best_dist == 0) break;
        }
    }

    // 2. 内置 native 函数（print/len/str 等）
    if (best_dist > 0) {
        int nc = native_get_name_count();
        for (int i = 0; i < nc; i++) {
            const char* nn = native_get_name(i);
            if (!nn) continue;
            int dist = levenshtein(name, nn);
            if (dist < best_dist) {
                best_dist = dist;
                best = nn;
                if (dist == 0) break;
            }
        }
    }

    // 3. 作用域变量（把变量当函数调用的手误）
    if (best_dist > 0) {
        for (Scope* sc = s->current; sc; sc = sc->parent) {
            for (int i = 0; i < sc->sym_cnt; i++) {
                Symbol* sym = sc->syms[i];
                if (!sym || !sym->name) continue;
                int dist = levenshtein(name, sym->name);
                if (dist < best_dist) {
                    best_dist = dist;
                    best = sym->name;
                    if (dist == 0) break;
                }
            }
            if (best_dist == 0) break;
        }
    }

    if (best && strcmp(best, name) != 0) {
        snprintf(hint, sizeof(hint), "\n  提示: 是否想输入 '%s'？", best);
    }
    return hint;
}

// C1：用户 struct 方法的相似名提示——扫函数表里 "Struct::method" 方法占位符。
// 前向定义的 struct（定义在使用点之后）占位符尚未注册，返回空串（优雅降级）
const char* get_similar_struct_method_hint(Semantic* s, const char* struct_name, const char* method_name) {
    static char hint[160];
    hint[0] = '\0';
    if (!s || !struct_name || !struct_name[0] || !method_name || !method_name[0]) return hint;
    char prefix[128];
    snprintf(prefix, sizeof(prefix), "%s::", struct_name);
    int plen = (int)strlen(prefix);
    const char* best = NULL;
    int best_dist = 3;
    if (s->func_table.entries) {
        for (int i = 0; i < s->func_table.capacity; i++) {
            for (FuncEntry* e = s->func_table.entries[i]; e; e = e->next) {
                if (!e->name || strncmp(e->name, prefix, plen) != 0) continue;
                const char* mname = e->name + plen;
                int dist = levenshtein(method_name, mname);
                if (dist < best_dist) {
                    best_dist = dist;
                    best = mname;
                    if (dist == 0) break;
                }
            }
            if (best_dist == 0) break;
        }
    }
    if (best && strcmp(best, method_name) != 0) {
        snprintf(hint, sizeof(hint), "\n  提示: 是否想用 '%s'？", best);
    }
    return hint;
}

// C1：内置类型方法调用的相似名提示——TypeKind 映射为注册表类型名（native_get_type_name
// 即注册时使用的名字），委托编译期实例方法元信息表
const char* semantic_method_hint(TypeInfo* type, const char* method_name) {
    if (!type || !method_name) return "";
    const char* tn = native_get_type_name(type->kind);
    if (!tn && type->kind == TYPE_BIGINT) tn = "number";
    if (!tn) return "";
    return native_instance_method_hint(tn, method_name);
}

// E5：未定义的 struct 类型若是某**已导入模块**的导出类型，提示先 use 导入
// （漏 use 是最高频的跨模块手误：模块里有 Point，宿主直接 new Point()）
const char* get_module_with_struct_hint(Semantic* s, const char* struct_name) {
    static char hint[192];
    hint[0] = '\0';
    if (!s || !struct_name || !struct_name[0]) return hint;
    for (int i = 0; i < s->imported_module_count; i++) {
        ImportedModuleInfo* mi = &s->imported_modules[i];
        if (!mi->alias || !mi->sym_table) continue;
        if (module_symbol_table_find_struct(mi->sym_table, struct_name)) {
            snprintf(hint, sizeof(hint),
                     "\n  提示: 模块 '%s' 中存在类型 '%s'，请先 'use %s.%s' 导入后裸名使用",
                     mi->alias, struct_name, mi->alias, struct_name);
            return hint;
        }
    }
    return hint;
}

const char* get_type_conversion_hint(TypeKind expected, TypeKind actual) {
    // any 转具体类型
    if (actual == TYPE_ANY) {
        switch (expected) {
            case TYPE_INT: return "提示：使用 _int(value) 进行显式转换";
            case TYPE_FLOAT: return "提示：使用 _float(value) 进行显式转换";
            case TYPE_STRING: return "提示：使用 _str(value) 进行显式转换";
            case TYPE_BOOL: return "提示：使用 _bool(value) 进行显式转换";
            // ⚠ 这条会被追加进调用方的 msg 缓冲（BUFFER_MEDIUM 级）⇒ 必须**短**：
            //   第一版写长了，实测被截断成"…（如" ✗（报文尾部丢字比没有提示更糟）。
            // ★ P1 的要点：**把用户引到语言既有的 `x as T` 安全转换**（`leno_ast.h` 原话：
            //   "安全类型转换" —— 匹配返回目标类型、**不匹配返回 null**、静态类型即目标类型）。
            //   此前只说"需要显式转换"却没点名语法 ⇒ 用户只能自己猜（本次移植最费时的一处 ✗）。
            case TYPE_DICT: return "提示：用 x as Dict[string, any] 安全转换（不匹配给 null），"
                                   "或 if x is Dict[string, any] => d {} 收窄";
            case TYPE_ARRAY: return "提示：用 x as Array[any] 安全转换（不匹配给 null），"
                                    "或 if x is Array[any] => a {} 收窄";
            default: return "提示：用 x as T 安全转换（不匹配给 null），或 if x is T => v {} 收窄";
        }
    }
    
    // float 转 int（需要截断）
    if (actual == TYPE_FLOAT && expected == TYPE_INT) {
        return "提示：float 转 int 会截断小数部分，使用 _int(value) 显式转换";
    }

    // bool 转 int/float（自动提升，但这里报错说明可能需要显式处理）
    if (actual == TYPE_BOOL && (expected == TYPE_INT || expected == TYPE_FLOAT || expected == TYPE_BIGINT)) {
        return "提示：bool 可以自动升级为 int/float，检查是否有其他类型问题";
    }

    // int 转 float（自动升级，但这里报错说明可能需要显式处理）
    if (actual == TYPE_INT && expected == TYPE_FLOAT) {
        return "提示：int 可以自动升级为 float，检查是否有其他类型问题";
    }
    
    // string 转数值
    if (actual == TYPE_STRING) {
        if (expected == TYPE_INT) return "提示：字符串转 int 使用 _int(value)，失败会报错";
        if (expected == TYPE_FLOAT) return "提示：字符串转 float 使用 _float(value)，失败会报错";
    }
    
    // 数组类型不匹配
    if (expected == TYPE_ARRAY && actual == TYPE_ARRAY) {
        return "提示：数组类型是不变的，Array[int] 不能赋给 Array 或其他元素类型的数组";
    }
    
    // Dict 类型不匹配
    if (expected == TYPE_DICT && actual == TYPE_DICT) {
        return "提示：字典类型是不变的，确保键值类型完全匹配";
    }
    
    return NULL;  // 没有特定提示
}

// 生成详细的类型错误信息
// 参数：
//   buf - 输出缓冲区
//   buf_size - 缓冲区大小
//   expected - 期望类型
//   actual - 实际类型
//   context - 错误上下文（如"变量赋值"、"函数参数"等）
//   var_name - 相关变量名（可为NULL，用于在错误信息中显示具体变量名）
void format_detailed_type_error(char* buf, size_t buf_size,
                                TypeInfo* expected, TypeInfo* actual,
                                const char* context) {
    format_detailed_type_error_ex(buf, buf_size, expected, actual, context, NULL);
}

// 扩展版本：支持变量名
void format_detailed_type_error_ex(char* buf, size_t buf_size,
                                TypeInfo* expected, TypeInfo* actual,
                                const char* context, const char* var_name) {
    char expected_buf[128] = "";
    char actual_buf[128] = "";
    
    if (expected) {
        const char* str = type_to_string(expected);
        strncpy(expected_buf, str, sizeof(expected_buf) - 1);
        expected_buf[sizeof(expected_buf) - 1] = '\0';
    }
    
    if (actual) {
        const char* str = type_to_string(actual);
        strncpy(actual_buf, str, sizeof(actual_buf) - 1);
        actual_buf[sizeof(actual_buf) - 1] = '\0';
    }
    
    // 构建基础错误信息（包含变量名）
    int offset;
    if (var_name) {
        offset = snprintf(buf, buf_size, "类型错误：%s '%s'\n  期望类型: %s\n  实际类型: %s",
                          context ? context : "类型不匹配", var_name,
                          expected_buf, actual_buf);
    } else {
        offset = snprintf(buf, buf_size, "类型错误：%s\n  期望类型: %s\n  实际类型: %s",
                          context ? context : "类型不匹配",
                          expected_buf, actual_buf);
    }
    
    // 添加转换建议
    if (expected && actual) {
        const char* hint = get_type_conversion_hint(expected->kind, actual->kind);
        if (hint && (size_t)offset < buf_size - 1) {
            snprintf(buf + offset, buf_size - offset, "\n  %s", hint);
        }
    }
}

// ============================================================================
// 数组索引赋值类型检查工具函数
// ============================================================================

// 检查数组索引赋值的元素类型兼容性
// 参数：
//   obj_type - 数组对象的类型
//   value_type - 要赋值的类型
//   line - 行号（用于错误报告）
// 返回：
//   1 = 类型兼容，0 = 类型不兼容（已报告错误）
int type_utils_check_array_index_assignment(TypeInfo* obj_type, TypeInfo* value_type, int line, int column) {
    if (!obj_type || obj_type->kind != TYPE_ARRAY) {
        return 1;  // 不是数组，不检查
    }
    
    TypeInfo* elem_type = obj_type->element_type;
    if (!elem_type || elem_type->kind == TYPE_ANY) {
        return 1;  // 数组元素类型未指定或为 any，不检查
    }
    
    if (!value_type) {
        return 1;  // 无法推断类型，不检查
    }
    
    // any 不能赋值给具体类型（从不确定到确定需要显式转换）
    if (value_type->kind == TYPE_ANY) {
        char msg[BUFFER_MEDIUM];
        format_detailed_type_error(msg, sizeof(msg),
            elem_type, value_type, "数组元素类型不匹配");
        error_add_at(ERR_SEMANTIC, line, column, msg);
        return 0;
    }
    
    if (value_type->kind == elem_type->kind) {
        return 1;  // 类型相同，允许
    }

    // int 可以隐式转为 float，但 float 不能转为 int
    if (elem_type->kind == TYPE_FLOAT && value_type->kind == TYPE_INT) {
        return 1;  // int -> float 允许
    }

    // bool 可以隐式转为 int/float/bigint
    if ((elem_type->kind == TYPE_INT || elem_type->kind == TYPE_FLOAT || elem_type->kind == TYPE_BIGINT) &&
        value_type->kind == TYPE_BOOL) {
        return 1;
    }

    // 类型不兼容，报告错误
    char msg[BUFFER_MEDIUM];
    format_detailed_type_error(msg, sizeof(msg),
        elem_type, value_type, "数组元素类型不匹配");
    error_add_at(ERR_SEMANTIC, line, column, msg);
    
    return 0;
}

// ============================================================================
// 字典索引赋值类型检查工具函数
// ============================================================================

// 检查字典索引赋值的值类型兼容性
// 参数：
//   dict_sym - 字典变量符号
//   assign_type - 要赋值的类型
//   line - 行号（用于错误报告）
// 返回：
//   1 = 类型兼容或已更新类型，0 = 类型不兼容（已报告错误）
int type_utils_check_dict_index_assignment(Symbol* dict_sym, TypeInfo* assign_type, int line, int column) {
    if (!dict_sym || !dict_sym->type || dict_sym->type->kind != TYPE_DICT) {
        return 1;  // 不是字典，不检查
    }
    
    if (!assign_type) {
        return 1;  // 无法推断类型，不检查
    }
    
    // 首先尝试更新字典值类型（如果是第一次赋值）
    int type_updated = type_utils_try_update_dict_value_type(dict_sym, assign_type);
    if (type_updated) {
        return 1;  // 类型已更新，不需要进一步检查
    }
    
    // 检查字典值类型
    TypeInfo* value_type = dict_sym->type->value_type;
    if (!value_type || value_type->kind == TYPE_ANY) {
        return 1;  // 字典值类型未指定或为 any，不检查
    }
    
    // any 不能赋值给具体类型（从不确定到确定需要显式转换）
    if (assign_type->kind == TYPE_ANY) {
        char msg[BUFFER_MEDIUM];
        format_detailed_type_error(msg, sizeof(msg),
            value_type, assign_type, "字典值类型不匹配");
        error_add_at(ERR_SEMANTIC, line, column, msg);
        return 0;
    }
    
    if (assign_type->kind == value_type->kind) {
        return 1;  // 类型相同，允许
    }

    // 允许 impl face 的 struct 赋给 face 类型的 Dict 值
    if (value_type->kind == TYPE_FACE && assign_type->kind == TYPE_STRUCT && assign_type->struct_name) {
        ObjFaceDef* fdef = face_def_find(value_type->struct_name);
        ObjStructDef* sdef = struct_def_find(assign_type->struct_name);
        if (fdef && sdef && struct_implements_face(sdef, fdef)) {
            return 1;
        }
    }

    // int 可以隐式转为 float
    if (value_type->kind == TYPE_FLOAT && assign_type->kind == TYPE_INT) {
        return 1;  // int -> float 允许
    }

    // bool 可以隐式转为 int/float/bigint
    if ((value_type->kind == TYPE_INT || value_type->kind == TYPE_FLOAT || value_type->kind == TYPE_BIGINT) &&
        assign_type->kind == TYPE_BOOL) {
        return 1;
    }

    // 类型不兼容，报告错误
    char msg[BUFFER_MEDIUM];
    format_detailed_type_error(msg, sizeof(msg),
        value_type, assign_type, "字典值类型不匹配");
    error_add_at(ERR_SEMANTIC, line, column, msg);
    
    return 0;
}
