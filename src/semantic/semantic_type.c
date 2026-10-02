#include "semantic_internal.h"
#include "include/module_symbol_table.h"

// ============================================================================
// 递归修正类型名：`Shape`（face）/ `Color`（enum）被 parser 记成 TYPE_STRUCT + 名字，
//   这里按"当前作用域 → 导入模块符号表"把**整棵类型树**里的名字纠正过来。
// ----------------------------------------------------------------------------
// 为什么要递归：v3.2.2 起 `is/as` 的泛型实参会带进运行期做**名字校验**（类型规格）。
//   只修最外层的话，`Array[Shape]` 的元素仍带着 TYPE_STRUCT + "Shape"，运行期就不知道
//   那是 face ⇒ 拿 struct 名字去比 "Rect" ⇒ 误判为不匹配（实测踩到过）。
//   历史上这段逻辑在 visit_type_check / visit_control（守卫、case）/ visit_expr 里
//   各写了一份，且只修"最外层"或"数组元素"—— 这里收成唯一实现。
// 注意：不处理 TYPE_FUNCTION 的签名（那里的名字由其它路径解析），只处理
//   Array/Dict/Ptr 三种实参位置 + 最外层。
// ============================================================================
// ★ 2026-10-02：改收 `TypeInfo**` —— **别名必须换指针**（`type_free` + `type_copy`，见
//   resolve_alias_in_type）：`alias IntList = Array[int]` 这种只改 kind 会把元素类型丢掉 ✗。
//   此前只收 `TypeInfo*` ⇒ 别名这一类整个漏掉 ⇒ `x as 跨模块别名` / `x is 跨模块别名` 的
//   目标类型停在 `TYPE_STRUCT + 别名名` ⇒ 运行期按一个**不存在的 struct 名**比对 ⇒
//   **静默给 null / 静默 false**（实测 `"hi" as AliasStr` = null、`is IntList` 不进分支）✗
void resolve_type_names(Semantic* s, TypeInfo** type_ptr) {
    if (!type_ptr || !*type_ptr) return;

    // 别名 / face / enum / cstruct / clib 的解析走**唯一实现**（与变量声明、struct 字段类型、
    //   函数签名同一路）⇒ 不在这里重写一份，避免两处口径漂移 ✓
    resolve_alias_in_type(s, type_ptr, (*type_ptr)->line);

    TypeInfo* type = *type_ptr;
    if (!type) return;

    if (type->kind == TYPE_STRUCT && type->struct_name) {
        Symbol* sym = scope_resolve(s->current, type->struct_name);
        if (sym && sym->type && sym->type->kind == TYPE_FACE) {
            type->kind = TYPE_FACE;
        } else if (sym && sym->type && sym->type->kind == TYPE_ENUM) {
            type->kind = TYPE_ENUM;
        } else if (face_def_find(type->struct_name)) {
            // 兜底：编译期已注册的运行期 face 注册表（既有 fix_struct_to_face 同一判据）
            type->kind = TYPE_FACE;
        } else {
            // 当前作用域没有 ⇒ 查导入模块的符号表（face / enum）
            for (int mi = 0; mi < s->imported_module_count; mi++) {
                ImportedModuleInfo* m = &s->imported_modules[mi];
                if (!m || !m->sym_table) continue;
                if (module_symbol_table_find_face(m->sym_table, type->struct_name)) {
                    type->kind = TYPE_FACE;
                    break;
                }
                if (module_symbol_table_find_enum(m->sym_table, type->struct_name)) {
                    type->kind = TYPE_ENUM;
                    break;
                }
            }
        }
    }

    // 泛型实参递归（三种带实参的类型）
    if (type->kind == TYPE_ARRAY) {
        resolve_type_names(s, &type->element_type);
    } else if (type->kind == TYPE_DICT) {
        resolve_type_names(s, &type->key_type);
        resolve_type_names(s, &type->value_type);
    } else if (type->kind == TYPE_PTR_GENERIC) {
        resolve_type_names(s, &type->element_type);
    }
}

// 修正 TypeInfo：如果类型是 TYPE_STRUCT 但名称实际是 face 定义，改为 TYPE_FACE
// 这是因为 parser 解析字段类型时，face 可能尚未注册到全局表，导致被误判为 struct
static void fix_struct_to_face(TypeInfo* type) {
    if (type && type->kind == TYPE_STRUCT && type->struct_name) {
        if (face_def_find(type->struct_name)) {
            type->kind = TYPE_FACE;
        }
    }
    // 递归修正 Array[StructThatIsActuallyFace] 的元素类型
    if (type && type->kind == TYPE_ARRAY && type->element_type) {
        fix_struct_to_face(type->element_type);
    }
    // 递归修正 Dict[K, V] 的值类型
    if (type && type->kind == TYPE_DICT && type->value_type) {
        fix_struct_to_face(type->value_type);
    }
    // 递归修正 func(P1, P2, ...): R 的参数类型和返回类型
    if (type && type->kind == TYPE_FUNCTION) {
        if (type->param_types) {
            for (int i = 0; i < type->param_count; i++) {
                fix_struct_to_face(type->param_types[i]);
            }
        }
        if (type->return_type) {
            fix_struct_to_face(type->return_type);
        }
    }
}



// ============================================================================
// 从函数体推断返回类型
// ============================================================================

// 深扫一棵语句树，把**所有**"带值 return"的表达式类型合并进 *acc：
//   · 结束时 *acc == NULL ⇒ 树里没有任何带值 return（⇒ 调用方按 void 处理 ✓）
//   · 多个 return 类型互不兼容 ⇒ 合并成 TYPE_ANY（"推不出"，调用方自行决定宽容还是跳过 ✓）
//   ★ 2026-09-30：以前只看 **块内顶层** 的 return ⇒ `func(){ if c { return 5 } }` 推不出来 ✗
//     实测症状：回调字面量被当成 `func():any`，赋给 `func():void` 变量/字段时报
//     「变量 'x' 声明类型与初始化值类型不匹配」（日常写回调最常踩的一条 ✗）
//   ⚠ **不下钻嵌套的函数字面量**（AST_FUNC_DEF 落 default）：那里面 return 属于它自己 ✓
static void merge_return_expr_types(Semantic* s, Ast* node, TypeInfo** acc) {
    if (!node) return;

    switch (node->kind) {
        case AST_RETURN:
            if (node->u.ret) {
                TypeInfo* rt = infer_expr_type(s, node->u.ret);
                if (rt) {
                    if (!*acc) {
                        *acc = rt;
                    } else if (type_is_compatible(*acc, rt)) {
                        type_free(rt);
                    } else {
                        // 类型不兼容 ⇒ 当作"推不出"（TYPE_ANY，与旧行为一致 ✓）
                        type_free(*acc);
                        type_free(rt);
                        *acc = type_new(TYPE_ANY);
                    }
                }
            }
            return;

        // 容器语句：下钻各分支（eif 链挂在 else_ 上，递归自然覆盖 ✓）
        case AST_BLOCK:
            for (int i = 0; i < node->u.block.count; i++) {
                merge_return_expr_types(s, node->u.block.items[i], acc);
            }
            return;
        case AST_IF:
            merge_return_expr_types(s, node->u.if_.then, acc);
            merge_return_expr_types(s, node->u.if_.else_, acc);
            return;
        case AST_WHILE:
            merge_return_expr_types(s, node->u.while_.body, acc);
            return;
        case AST_FOR:
            merge_return_expr_types(s, node->u.for_.body, acc);
            return;
        case AST_SWITCH:
            for (int i = 0; i < node->u.switch_.case_count; i++) {
                merge_return_expr_types(s, node->u.switch_.cases[i].body, acc);
            }
            merge_return_expr_types(s, node->u.switch_.default_body, acc);
            return;
        case AST_TRY:
            merge_return_expr_types(s, node->u.try_.try_body, acc);
            merge_return_expr_types(s, node->u.try_.catch_body, acc);
            merge_return_expr_types(s, node->u.try_.finally_body, acc);
            return;

        // 其它语句（含嵌套函数字面量）不下钻 ✓
        default:
            return;
    }
}

// ⚠ v3.2.8 起**非 static**：`visit_type_def.inc`（编进 semantic_visit_ast.c，另一个 TU）也要用它
//   —— 判"impl 方法没标返回类型"时的实际返回类型（见该文件里 face 兼容性检查的注释）。
//   契约不变：**NULL = 推不出**（调用方见 NULL/TYPE_ANY 一律按"宁漏不误报"跳过 ✓）；
//   变的是**能推出来的场合变多了**（嵌套分支里的 return 现在也算 ✓，见上面的深扫 ✓）
TypeInfo* infer_return_type_from_body(Semantic* s, Ast* body) {
    if (!body) return NULL;

    TypeInfo* inferred_type = NULL;
    merge_return_expr_types(s, body, &inferred_type);
    return inferred_type;
}

// ============================================================================
// 回调返回类型推断（**唯一实现**：map / reduce 的实例形态与模块形态共用）
// ============================================================================
// 为什么需要这个助手（v3.2.8）：`map` / `reduce` 的返回类型都取决于**回调的返回类型**，
//   而这件事有**四种写法**：`arr.map(fn)` / `arr.reduce(fn, init)`（实例形态）与
//   `arrays.map(arr, fn)` / `arrays.reduce(arr, fn, init)`（模块形态）。
//   此前实例形态有一条"类型守卫"实现、模块形态**没有** ⇒ 同一操作两种写法精度不一致
//   （实测：`Array[int] m = arrays.map(xs, fn)` 报 `Array[any]`，而 `xs.map(fn)` 是 `Array[int]`）。
//   各自修一份就等于把守卫逻辑抄两遍（参数符号的保存/恢复最容易抄漏）⇒ 收敛到这一个函数。
//
// 做法：临时把回调的**参数符号**类型设成实际类型（数组元素 / 累加器），推完函数体返回类型再恢复。
//   · acc_type == NULL（map 语义）：只守第 0 个参数 = 元素类型，其余参数保持原样；
//   · acc_type != NULL（reduce 语义）：第 0 个 = 累加器（拿不到就用元素类型兜底）、第 1 个 = 元素类型。
//   ⚠ 守卫只改写**符号类型**：回调若把形参写成 `any`，函数体表达式的类型可能已被更早的遍历
//     缓存（`ast->cached_type`）⇒ 拿不到结果（返回 any / NULL）。实用建议：回调把形参类型写出来。
//
// 参数：elem_type 数组元素类型（只读，内部复制）；acc_type 可为 NULL；cb 回调 AST。
// 返回：回调返回类型的**新副本**（调用方 type_free）；推不出返回 NULL。
static TypeInfo* infer_callback_ret_type(Semantic* s, TypeInfo* elem_type,
                                         TypeInfo* acc_type, Ast* cb) {
    if (!s || !elem_type || !elem_type->element_type) return NULL;
    if (!cb || cb->kind != AST_FUNC_DEF || cb->u.func.pcnt < 1) return NULL;

    TypeInfo* guard_type = type_copy(elem_type->element_type);
    TypeInfo* orig_types[8] = {NULL};   // 保存原始类型（恢复时用）
    int guard_count = cb->u.func.pcnt < 8 ? cb->u.func.pcnt : 8;
    for (int gi = 0; gi < guard_count; gi++) {
        Symbol* param_sym = scope_resolve(s->current, cb->u.func.params[gi]);
        if (!param_sym) continue;
        orig_types[gi] = param_sym->type;
        if (gi == 0) {
            param_sym->type = (acc_type && acc_type->kind != TYPE_ANY)
                ? type_copy(acc_type) : type_copy(guard_type);
        } else if (gi == 1 && acc_type) {
            param_sym->type = type_copy(guard_type);
        } else {
            param_sym->type = type_copy(orig_types[gi]);
        }
    }
    TypeInfo* inferred = infer_return_type_from_body(s, cb->u.func.body);
    for (int gi = 0; gi < guard_count; gi++) {
        Symbol* param_sym = scope_resolve(s->current, cb->u.func.params[gi]);
        if (param_sym && orig_types[gi]) {
            type_free(param_sym->type);
            param_sym->type = orig_types[gi];
        }
    }
    type_free(guard_type);
    return inferred;
}

// ============================================================================
// ⓪ 族通用推断点（**语义侧唯一实现**，2026-09-28 声明化）
// ============================================================================
// 规格（含 NTYPE_ARG_CB_RET 节点）+ 调用实参 ⇒ 推回调返回类型并代回规格形状。
//   此前 map（模块/实例两条路径各一份）、reduce、threads.start 各自硬编码 if 链
//  （最多时四份手写守卫拷贝、精度互不一致）⇒ 收敛后**注册即数据**：
//     arrays.map    = Array[ARG_CB_RET(1)]      threads.start = Thread[ARG_CB_RET(0)]
//     arrays.reduce = ARG_CB_RET(1, acc=2)      （实例形态下标同：接收者 = 0）
//   推断规则：内联闭包（AST_FUNC_DEF）⇒ infer_callback_ret_type（有 acc ⇒ reduce 守卫；
//   无 acc 但 arg0 是带元素的 Array ⇒ map 守卫；否则直推体）；命名函数 ⇒ TYPE_FUNCTION 的
//   return_type。推不出 ⇒ NULL（调用方回落 Kind 槽，宁漏勿误报 ✓）。
// 参数：arg0_type = 接收者/首实参的类型（ARG0 约定，map/reduce 的容器就是它）；
//   args/arg_count = **实参 AST 数组**（模块形态不含接收者；实例形态下标 - has_receiver 对齐）；
//   has_receiver = 1 ⇒ 实例形态（下标 0 是接收者，不在 args 里）。
static const NativeTypeSpec* spec_find_cb_ret(const NativeTypeSpec* spec) {
    if (!spec) return NULL;
    if (spec->tag == NTYPE_ARG_CB_RET) return spec;
    const NativeTypeSpec* f = spec_find_cb_ret(spec->sub);
    return f ? f : spec_find_cb_ret(spec->sub2);
}

static TypeInfo* spec_resolve_with_cb_ret(Semantic* s, const NativeTypeSpec* spec,
                                          TypeInfo* arg0_type,
                                          Ast* const* args, int arg_count, int has_receiver) {
    const NativeTypeSpec* cb_node = spec_find_cb_ret(spec);
    if (!cb_node) return NULL;
    int cb_i = cb_node->arg_index - has_receiver;
    if (cb_i < 0 || cb_i >= arg_count) return NULL;
    Ast* cb_ast = args[cb_i];

    TypeInfo* acc_type = NULL;
    if (cb_node->acc_index >= 0) {
        int acc_i = cb_node->acc_index - has_receiver;
        if (acc_i >= 0 && acc_i < arg_count) {
            acc_type = infer_expr_type(s, args[acc_i]);
        }
    }

    TypeInfo* ret = NULL;
    if (cb_ast && cb_ast->kind == AST_FUNC_DEF) {
        bool guarded = false;
        if (acc_type) {
            ret = infer_callback_ret_type(s, arg0_type, acc_type, cb_ast);   // reduce 守卫 ✓
            guarded = true;
        } else if (arg0_type && arg0_type->kind == TYPE_ARRAY &&
                   arg0_type->element_type && arg0_type->element_type->kind != TYPE_ANY) {
            ret = infer_callback_ret_type(s, arg0_type, NULL, cb_ast);       // map 守卫 ✓
            guarded = true;
        }
        if (!guarded || !ret) {
            // 无容器可守（threads.start）或守卫路径没推出来 ⇒ 直推函数体 ✓
            ret = infer_return_type_from_body(s, cb_ast->u.func.body);
        }
    } else {
        // 命名函数等：取其函数类型的 return_type（声明即所得 ✓）
        TypeInfo* ft = infer_expr_type(s, cb_ast);
        if (ft) {
            if (ft->kind == TYPE_FUNCTION && ft->return_type) {
                ret = type_copy(ft->return_type);
            }
            type_free(ft);
        }
    }
    if (acc_type) type_free(acc_type);
    if (!ret || ret->kind == TYPE_ANY) {
        if (ret) type_free(ret);
        return NULL;
    }
    TypeInfo* out = native_type_spec_to_info_with_cb(spec, arg0_type, ret);   // T 代回规格形状
    type_free(ret);
    return out;
}

// ----------------------------------------------------------------------------
// Dict.get 返回类型推断（**两条调用路径共享**，2026-09-28 收敛）：
//   2 参形态 d.get(k, def) ⇒ 第 2 实参（默认值）的类型
//     （opts.get("x", 0) → int / ("x", 0.0) → float / ("z", true) → bool / ("name", "") → string）；
//   1 参形态 d.get(k)      ⇒ 字典值类型 V（`Dict[string,string].get("k")` ⇒ string）。
//   ⚠ 诚实性说明：1 参形态在**键不存在**时返回 null ⇒ 严格说该是 `V?`；但本语言的 null 可
//     隐式赋给具体类型，且 2 参形态（返回默认值类型）同样是这个口径 ⇒ 与 2 参一致取 V。
//   命中并推出 ⇒ 返回 TypeInfo（**所有权转移给调用方**）；推不出/非 get ⇒ NULL。
//   receiver_type 不夺取所有权。此前 AST_CALL / obj_sym 两条路径各留一份拷贝、且 obj_sym
//   缺 1 参形态 ⇒ 同一语法两处深度不一致（arity 分支逻辑规格词汇表表达不了，收敛为函数共享）。
// Dict.get 的返回类型 —— **唯一判定点**（方法名判断也在这里，调用点不做名字分支 ✓）
//   · 2 参形态：按**默认值实参**的类型（`get("y", 0.0)` ⇒ float ✓）
//   · 1 参形态：字典的值类型 V ✓（缺键时运行期给 null ⇒ 沿用 d4c3e0e 的"乐观"口径 ✓）
//   ⚠ **只对 `get` 成立**：别的 Dict 方法（`len` ⇒ int、`has` ⇒ bool）在编译期元信息表里
//     本来就有正确类型（注册桥接见 method_table.c:108 ✓）⇒ 必须在这里**留空**，
//     让它们落到元信息表 ✓；否则会被本函数返回的 V 覆盖 ✗ ——
//     LenoWeb `web_html.leno:26 return attrs.has(name)` 正是被推成 string 而编译不过，
//     导致该包 35 个文件连锁失败 ✓（2026-09-28 修 LenoWeb）
//   名字判断放这里的理由：与 83ff614「Dict.get 三处特判收敛为共享 infer_dict_get_return」
//     同一路数 ✓ —— 保持**单一判定点**，别在各调用点又长出 if 链 ✗
static TypeInfo* infer_dict_get_return(Semantic* s, TypeInfo* receiver_type, const char* method_name,
                                       Ast* const* args, int arg_count) {
    if (!method_name || strcmp(method_name, "get") != 0) return NULL;
    if (!receiver_type || receiver_type->kind != TYPE_DICT) return NULL;
    if (arg_count >= 2) {
        TypeInfo* default_type = infer_expr_type(s, args[1]);
        if (default_type && default_type->kind != TYPE_ANY) {
            return default_type;
        }
        if (default_type) type_free(default_type);
        return NULL;
    }
    if (receiver_type->value_type && receiver_type->value_type->kind != TYPE_ANY) {
        return type_copy(receiver_type->value_type);
    }
    return NULL;
}

// ============================================================================
// 类型推断辅助函数
// ============================================================================

// 推断 struct/cstruct/face 对象调用 method_name 的返回类型
// 包含完整的 6 步查找 + 泛型替换 + return_type_info 回退
// 返回推断出的类型（需调用者 type_free），未找到返回 NULL
// 注意：不设置 ast->cached_type，不 type_free(obj_type)，均由调用方管理
TypeInfo* infer_method_return_type(Semantic* s, TypeInfo* obj_type, const char* method_name) {
    if (!obj_type || !method_name) return NULL;

    if (obj_type->kind != TYPE_STRUCT && obj_type->kind != TYPE_CSTRUCT && obj_type->kind != TYPE_FACE) {
        return NULL;
    }

    // 1. 检查函数类型字段（如 func(int,int):int op）
    if (obj_type->struct_name && (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT)) {
        Symbol* struct_sym = scope_resolve(s->current, obj_type->struct_name);
        if (struct_sym && struct_sym->struct_field_names && struct_sym->struct_field_types) {
            for (int fi = 0; fi < struct_sym->struct_field_count; fi++) {
                if (strcmp(struct_sym->struct_field_names[fi], method_name) == 0 &&
                    struct_sym->struct_field_types[fi]->kind == TYPE_FUNCTION &&
                    struct_sym->struct_field_types[fi]->return_type) {
                    return type_copy(struct_sym->struct_field_types[fi]->return_type);
                }
            }
        }
    }

    // 2. Face 本地定义
    if (obj_type->kind == TYPE_FACE) {
        ObjFaceDef* fdef = face_def_find(obj_type->struct_name);
        if (fdef) {
            for (int mi = 0; mi < fdef->method_count; mi++) {
                if (strcmp(fdef->methods[mi].name, method_name) == 0) {
                    if (fdef->methods[mi].return_type) {
                        return type_copy(fdef->methods[mi].return_type);
                    } else {
                        return type_new(TYPE_ANY);
                    }
                }
            }
        }
        // 3. Face 可能定义在导入的模块中
        for (int mi = 0; mi < s->imported_module_count; mi++) {
            ImportedModuleInfo* m = &s->imported_modules[mi];
            if (m && m->sym_table) {
                ModuleFaceSymbol* face_sym = module_symbol_table_find_face(m->sym_table, obj_type->struct_name);
                if (face_sym) {
                    for (int fi = 0; fi < face_sym->method_count; fi++) {
                        if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                            TypeInfo* ret_type = type_new(face_sym->methods[fi].return_type);
                            if (face_sym->methods[fi].return_type == TYPE_STRUCT && face_sym->methods[fi].return_struct_name) {
                                ret_type->struct_name = strdup(face_sym->methods[fi].return_struct_name);
                            }
                            // 检查返回类型是否是 face
                            if (face_sym->methods[fi].return_struct_name) {
                                ModuleFaceSymbol* ret_face = module_symbol_table_find_face(m->sym_table, face_sym->methods[fi].return_struct_name);
                                if (ret_face) {
                                    ret_type->kind = TYPE_FACE;
                                    ret_type->struct_name = strdup(ret_face->name);
                                }
                            }
                            return ret_type;
                        }
                    }
                }
            }
        }
        return NULL;
    }

    // 4. 从函数表查找方法定义（struct_name::method_name）
    char method_key[256];
    if (obj_type->struct_name) {
        snprintf(method_key, sizeof(method_key), "%s::%s", obj_type->struct_name, method_name);
    } else {
        strncpy(method_key, method_name, sizeof(method_key) - 1);
        method_key[sizeof(method_key) - 1] = '\0';
    }
    Ast* func_def = func_table_find(&s->func_table, method_key);
    if (func_def && func_def->kind == AST_FUNC_DEF) {
        TypeInfo* ret = NULL;
        if (func_def->u.func.return_type && func_def->u.func.return_type->kind != TYPE_INFER) {
            ret = type_copy(func_def->u.func.return_type);
        } else if (func_def->u.func.body) {
            ret = infer_return_type_from_body(s, func_def->u.func.body);
        }
        if (!ret) {
            ret = type_new(TYPE_ANY);
        }

        // 泛型参数替换：如果 obj_type 有具体泛型参数，替换返回类型中的泛型参数
        if (obj_type->generic_count > 0 && obj_type->generic_args && obj_type->struct_name) {
            for (int mi = 0; mi < s->imported_module_count; mi++) {
                ImportedModuleInfo* info = &s->imported_modules[mi];
                if (info->sym_table) {
                    ModuleStructSymbol* ssym = module_symbol_table_find_struct(info->sym_table, obj_type->struct_name);
                    if (ssym && ssym->type_param_count > 0 && ssym->type_param_names) {
                        TypeInfo* substituted = ret;
                        for (int gi = 0; gi < ssym->type_param_count && gi < obj_type->generic_count; gi++) {
                            TypeInfo* new_ret = type_substitute(substituted, ssym->type_param_names[gi], obj_type->generic_args[gi]);
                            type_free(substituted);
                            substituted = new_ret;
                        }
                        ret = substituted;
                        break;
                    }
                }
            }
        }

        // 如果返回类型缺少泛型信息，从模块符号表的 return_type_info 补充
        if (obj_type->struct_name &&
            ((ret->kind == TYPE_DICT && !ret->value_type) ||
             (ret->kind == TYPE_ARRAY && !ret->element_type) ||
             (ret->kind == TYPE_PTR_GENERIC && !ret->element_type) ||
             (ret->kind == TYPE_STRUCT && !ret->struct_name))) {
            for (int mi = 0; mi < s->imported_module_count; mi++) {
                ImportedModuleInfo* info = &s->imported_modules[mi];
                if (info->sym_table) {
                    ModuleStructMethod* mod_method = module_symbol_table_find_struct_method(
                        info->sym_table, obj_type->struct_name, method_name);
                    if (mod_method && mod_method->return_type_info) {
                        type_free(ret);
                        ret = type_copy(mod_method->return_type_info);
                        break;
                    }
                }
            }
        }

        return ret;
    }

    // 5. 原生方法
    {
        int arity;
        const char* type_name = (obj_type->kind == TYPE_CSTRUCT) ? "cstruct" : "struct";
        TypeKind return_type = native_get_instance_method_return_type(type_name, method_name, &arity);

        if ((return_type == TYPE_STRUCT || return_type == TYPE_CSTRUCT) &&
            (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT)) {
            return type_copy(obj_type);
        }
        if (return_type != TYPE_ANY) {
            return type_new(return_type);
        }
    }

    // 6. 从导入模块的符号表查找
    if (obj_type->struct_name) {
        for (int i = 0; i < s->imported_module_count; i++) {
            ImportedModuleInfo* info = &s->imported_modules[i];
            if (info->sym_table) {
                ModuleStructMethod* method = module_symbol_table_find_struct_method(
                    info->sym_table, obj_type->struct_name, method_name);
                if (method) {
                    TypeInfo* result = NULL;
                    // 如果返回类型是泛型参数，用 type_substitute 替换
                    if (method->return_type == TYPE_GENERIC_PARAM && method->return_type_param_name &&
                        obj_type->generic_count > 0 && obj_type->generic_args) {
                        ModuleStructSymbol* ssym = module_symbol_table_find_struct(info->sym_table, obj_type->struct_name);
                        if (ssym && ssym->type_param_names) {
                            for (int gi = 0; gi < ssym->type_param_count && gi < obj_type->generic_count; gi++) {
                                if (strcmp(ssym->type_param_names[gi], method->return_type_param_name) == 0) {
                                    result = type_copy(obj_type->generic_args[gi]);
                                    break;
                                }
                            }
                        }
                    }
                    if (!result) {
                        if (method->return_type_info) {
                            result = type_copy(method->return_type_info);
                        } else {
                            result = type_new(method->return_type);
                        }
                        if (method->return_type == TYPE_STRUCT && method->return_struct_name) {
                            result->struct_name = strdup(method->return_struct_name);
                        }
                    }
                    return result;
                }
            }
        }
    }

    return NULL;
}

// 泛型形参在字段类型里常被解析成 TYPE_STRUCT 占位（struct_name=形参名，如 "T"），
// type_substitute 只认 TYPE_GENERIC_PARAM ⇒ 对这类占位补替换（对齐 visit_expr.inc
// 自定义方法检查里的 is_generic 特判）。语义与 type_substitute 完全一致：
// **不释放输入**（输入可能是方法 AST 拥有的 param_types[]），返回新类型由调用方释放。
TypeInfo* semantic_substitute_generic_param(TypeInfo* type, const char* param_name, TypeInfo* concrete) {
    if (!type) return type;
    if (type->kind == TYPE_GENERIC_PARAM && type->type_param_name &&
        strcmp(type->type_param_name, param_name) == 0) {
        TypeInfo* replaced = type_copy(concrete);
        // ★ 别丢**占位符自身**的可空标记（同 type_substitute 的说明）：`T?` 代入后仍是可空
        if (replaced && type->nullable) replaced->nullable = 1;
        return replaced;
    }
    if (type->kind == TYPE_STRUCT && type->struct_name &&
        strcmp(type->struct_name, param_name) == 0) {
        TypeInfo* replaced = type_copy(concrete);
        if (replaced && type->nullable) replaced->nullable = 1;
        return replaced;
    }
    TypeInfo* result = type_copy(type);
    if (result->element_type) {
        result->element_type = semantic_substitute_generic_param(result->element_type, param_name, concrete);
    }
    if (result->key_type) {
        result->key_type = semantic_substitute_generic_param(result->key_type, param_name, concrete);
    }
    if (result->value_type) {
        result->value_type = semantic_substitute_generic_param(result->value_type, param_name, concrete);
    }
    // v31：**泛型实参**也要递归 —— 否则"占位符藏在 generic_args 里"的类型替换等于没做：
    //   `Pair[K, V]` 的 K/V 就在 generic_args[] 里 ⇒ `gm.makePair[string, int]` 的静态类型
    //   仍停在 `Pair[K, V]`（实测：`string k = p.getKey()` 报"期望 string，实际 struct K"）。
    if (result->generic_count > 0 && result->generic_args) {
        for (int gi = 0; gi < result->generic_count; gi++) {
            TypeInfo* sub = semantic_substitute_generic_param(result->generic_args[gi],
                                                             param_name, concrete);
            type_free(result->generic_args[gi]);   // 释放 type_copy 造出来的那份
            result->generic_args[gi] = sub;
        }
    }
    return result;
}

// 推断 struct/cstruct/clib/dict 对象访问 field_name 的字段类型
// 包含泛型替换、cstruct 特殊处理（c_layout_type_to_leno）、变量符号回退、全局 cstruct 表查找
// out_field_index: 输出字段索引（可为 NULL 表示不需要）
// 返回推断出的类型（需调用者 type_free），未找到返回 NULL
// 注意：不设置 ast 字段，不 type_free(obj_type)，均由调用方管理
// ============================================================================
// native struct 兜底（v3.2.5）：让 `DirEntry` / `DirInfo` 这类名字能当**类型标注**
// ----------------------------------------------------------------------------
// 为什么需要：native 模块用 `native_register_struct_spec()` 声明字段表，但这些名字
//   **不在符号表里**（native 模块没有 sym_table）⇒ 类型名校验四处都判"未定义的类型"
//   （实测：`DirInfo d = dirs.stat(p)` 报 `[语义错误] 未定义的类型: DirInfo`，还建议一个
//   native 根本不存在的 `use module.DirInfo`），只写 `var d = ...` 才躲得过。
// 判据来源与其它环节**同源**：`native_find_struct_spec()` —— 编译期字段解析
//   （下面 infer_field_type 的 native 分支）与运行期 `ObjStructDef`（native_struct_def_for）
//   用的都是这份规格 ⇒ 这里认了它，字段访问与运行期就一定对得上。
// ⚠ 只判"合法性"，不改 kind：它本来就是普通 struct（face/enum 由 resolve_type_names 纠正）。
// ============================================================================
int semantic_native_struct_known(const char* name) {
    if (!name || !name[0]) return 0;
    return native_find_struct_spec(name) != NULL;
}

TypeInfo* infer_field_type(Semantic* s, TypeInfo* obj_type, const char* field_name, int* out_field_index) {
    if (!obj_type || !field_name) return NULL;
    if (out_field_index) *out_field_index = -1;

    if (obj_type->kind == TYPE_STRUCT) {
        // 从对象类型获取 struct 类型名称，查找 struct 定义
        if (obj_type->struct_name) {
            Symbol* struct_def_sym = scope_resolve(s->current, obj_type->struct_name);
            if (struct_def_sym && struct_def_sym->struct_field_count > 0) {
                for (int i = 0; i < struct_def_sym->struct_field_count; i++) {
                    if (strcmp(struct_def_sym->struct_field_names[i], field_name) == 0) {
                        TypeInfo* result = type_copy(struct_def_sym->struct_field_types[i]);
                        if (out_field_index) *out_field_index = i;

                        // 泛型参数替换
                        if (obj_type->generic_count > 0 && obj_type->generic_args &&
                            struct_def_sym->struct_type_param_count > 0 && struct_def_sym->struct_type_params) {
                            for (int j = 0; j < struct_def_sym->struct_type_param_count && j < obj_type->generic_count; j++) {
                                TypeInfo* substituted = semantic_substitute_generic_param(result,
                                    struct_def_sym->struct_type_params[j], obj_type->generic_args[j]);
                                type_free(result);
                                result = substituted;
                            }
                        }
                        return result;
                    }
                }
            }

            // native 结构体兜底（v3.2.3）：native 模块用 NativeStructSpec 声明字段表
            //   ⇒ 既不在作用域（不是脚本符号）、也没有符号表（native 模块 sym_table 恒 NULL），
            //   只能查这份规格。字段顺序与运行期 ObjStructDef **同源**（同一 spec）✓
            {
                const NativeStructSpec* nss = native_find_struct_spec(obj_type->struct_name);
                if (nss) {
                    for (int fi = 0; fi < nss->field_count; fi++) {
                        if (strcmp(nss->field_names[fi], field_name) == 0) {
                            if (out_field_index) *out_field_index = fi;
                            return native_type_spec_to_info(nss->field_types[fi]);
                        }
                    }
                }
            }

            // 跨模块 struct 查找：当 struct 定义在导入模块中时（如 use SDL3.Font），
            // scope_resolve 可能找不到（或找到的符号没有字段信息），需要从导入模块的符号表查找。
            // 这与 TYPE_CSTRUCT 分支的跨模块查找逻辑一致。
            for (int mi = 0; mi < s->imported_module_count; mi++) {
                ImportedModuleInfo* mod = &s->imported_modules[mi];
                if (mod->sym_table) {
                    ModuleStructSymbol* ssym = module_symbol_table_find_struct(mod->sym_table, obj_type->struct_name);
                    if (ssym && !ssym->is_cstruct && ssym->field_count > 0) {
                        for (int fi = 0; fi < ssym->field_count; fi++) {
                            if (strcmp(ssym->fields[fi].name, field_name) == 0) {
                                if (out_field_index) *out_field_index = fi;
                                // 从模块符号表的字段类型构建 TypeInfo
                                // ★ pri：跨模块私有字段 —— **在这里报**，因为只有这里才拿得到
                    //   已加载（非 NULL）的模块符号表 ✓
                    //   为什么不在 visit_field_access 里报：那边跑得更早，表的懒加载还没发生
                    //   ⇒ 实测 imported_modules[].sym_table 仍是 NULL，检查静默失效 ✗
                    //   放行条件：当前在该 struct 自己的方法内（看 self 的类型 ✓）
                    if (ssym->fields[fi].is_private) {
                        int inside_own = 0;
                        Symbol* self_sym = scope_resolve(s->current, "self");
                        if (self_sym && self_sym->type && self_sym->type->struct_name &&
                            strcmp(self_sym->type->struct_name, obj_type->struct_name) == 0) {
                            inside_own = 1;
                        }
                        if (!inside_own) {
                            char pri_msg[BUFFER_MEDIUM];
                            snprintf(pri_msg, sizeof(pri_msg),
                                "'%s.%s' 是 pri 私有字段（%s 第 %d 行）—— 只有 %s 自己的方法内部可以访问它；"
                                "要对外开放就把 'pri' 去掉（默认全公有）",
                                obj_type->struct_name, field_name, error_get_filename(),
                                ssym->fields[fi].line, obj_type->struct_name);
                            error_add_at(ERR_SEMANTIC, ssym->fields[fi].line, 1, pri_msg);
                        }
                    }
                    TypeInfo* result = type_new(ssym->fields[fi].type);
                                if (ssym->fields[fi].struct_name) {
                                    result->struct_name = strdup(ssym->fields[fi].struct_name);
                                }
                                // 如果字段有元素类型（如 Array[T], Ptr[T]），构建 element_type
                                if (ssym->fields[fi].element_type != TYPE_ANY && ssym->fields[fi].element_type != 0) {
                                    result->element_type = type_new(ssym->fields[fi].element_type);
                                    if (ssym->fields[fi].element_struct_name) {
                                        result->element_type->struct_name = strdup(ssym->fields[fi].element_struct_name);
                                    }
                                }
                                // 泛型参数替换
                                if (obj_type->generic_count > 0 && obj_type->generic_args &&
                                    ssym->type_param_count > 0 && ssym->type_param_names) {
                                    for (int j = 0; j < ssym->type_param_count && j < obj_type->generic_count; j++) {
                                        TypeInfo* substituted = type_substitute(result,
                                            ssym->type_param_names[j], obj_type->generic_args[j]);
                                        type_free(result);
                                        result = substituted;
                                    }
                                }
                                return result;
                            }
                        }
                    }
                }
            }
        }
        // 变量符号回退查找
        return NULL;
    }
    else if (obj_type->kind == TYPE_CSTRUCT) {
        if (obj_type->struct_name) {
            Symbol* cstruct_def_sym = scope_resolve(s->current, obj_type->struct_name);
            if (cstruct_def_sym && cstruct_def_sym->struct_field_count > 0) {
                for (int i = 0; i < cstruct_def_sym->struct_field_count; i++) {
                    if (strcmp(cstruct_def_sym->struct_field_names[i], field_name) == 0) {
                        TypeInfo* result = type_copy(cstruct_def_sym->struct_field_types[i]);
                        // cstruct 字段类型从 C 布局类型映射为 Leno 类型
                        TypeKind leno_kind = c_layout_type_to_leno(result->kind);
                        if (leno_kind != result->kind) {
                            result->kind = leno_kind;
                        }
                        if (out_field_index) *out_field_index = i;
                        return result;
                    }
                }
            }

            // 从全局 cstruct 定义表查找（跨模块导入的 cstruct）
            ObjCStructDef* cdef = cstruct_def_find(obj_type->struct_name);
            if (cdef) {
                int idx = cstruct_get_field_index(cdef, field_name);
                if (idx >= 0) {
                    if (out_field_index) *out_field_index = idx;
                    return type_new(c_layout_type_to_leno(cdef->fields[idx].type));
                }
            } else {
                // 全局表可能还没注册，从导入模块的符号表查找
                for (int mi = 0; mi < s->imported_module_count; mi++) {
                    ImportedModuleInfo* mod = &s->imported_modules[mi];
                    if (mod->sym_table) {
                        ModuleStructSymbol* ssym = module_symbol_table_find_struct(mod->sym_table, obj_type->struct_name);
                        if (ssym && ssym->is_cstruct) {
                            for (int fi = 0; fi < ssym->field_count; fi++) {
                                if (strcmp(ssym->fields[fi].name, field_name) == 0) {
                                    if (out_field_index) *out_field_index = fi;
                                    return type_new(c_layout_type_to_leno(ssym->fields[fi].type));
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
        return NULL;
    }
    else if (obj_type->kind == TYPE_CLIB) {
        if (obj_type->struct_name) {
            Symbol* clib_sym = scope_resolve(s->current, obj_type->struct_name);
            if (clib_sym && clib_sym->clib_func_count > 0) {
                for (int i = 0; i < clib_sym->clib_func_count; i++) {
                    if (strcmp(clib_sym->clib_func_names[i], field_name) == 0) {
                        TypeInfo* ret_type = clib_sym->clib_func_return_types[i];
                        TypeKind rk = ret_type ? ret_type->kind : TYPE_NULL;
                        TypeKind leno_rk = c_layout_type_to_leno(rk);
                        if (leno_rk != rk) {
                            return type_new(leno_rk);
                        }
                        switch (rk) {
                            case TYPE_PTR:
                                { TypeInfo* t = type_new(TYPE_PTR); t->struct_name = strdup("Ptr"); return t; }
                            case TYPE_PTR_GENERIC:
                                if (ret_type && ret_type->element_type) {
                                    return type_ptr_generic(type_copy(ret_type->element_type));
                                } else {
                                    TypeInfo* t = type_new(TYPE_PTR); t->struct_name = strdup("Ptr"); return t;
                                }
                            case TYPE_BOOL:
                                return type_new(TYPE_BOOL);
                            case TYPE_NULL:
                                return type_new(TYPE_NULL);
                            default:
                                return ret_type ? type_copy(ret_type) : type_new(TYPE_ANY);
                        }
                    }
                }
            }
        }
        return NULL;
    }
    else if (obj_type->kind == TYPE_DICT) {
        return obj_type->value_type ? type_copy(obj_type->value_type) : type_new(TYPE_ANY);
    }

    return NULL;
}

// ============================================================================
// 辅助：检查 nullable 值类型是否参与算术运算（发出警告）
// nullable 的值类型（如 int?, float?, bool?, bigint?）参与算术运算时，
// 运行时可能为 null 导致错误，编译时发出警告提醒用户先做 null 检查
// ============================================================================
static void check_nullable_arith(TypeInfo* left, TypeInfo* right, int line, int column) {
    int warned = 0;
    if (left && left->nullable &&
        (left->kind == TYPE_INT || left->kind == TYPE_FLOAT ||
         left->kind == TYPE_BOOL || left->kind == TYPE_BIGINT)) {
        warning_add_at(WARN_NULLABLE_ARITH, line, column,
            "nullable 值类型参与算术运算，运行时可能为 null，请先检查变量是否为 null（如 if a != null）");
        warned = 1;
    }
    if (!warned && right && right->nullable &&
        (right->kind == TYPE_INT || right->kind == TYPE_FLOAT ||
         right->kind == TYPE_BOOL || right->kind == TYPE_BIGINT)) {
        warning_add_at(WARN_NULLABLE_ARITH, line, column,
            "nullable 值类型参与算术运算，运行时可能为 null，请先检查变量是否为 null（如 if a != null）");
    }
}

// ============================================================================
// 类型推断
// ============================================================================

// ★ T11：「确定是 null」的操作数 ⇒ **编译错误**（教程明写「null 不能参与算术运算」）
// ----------------------------------------------------------------------------
// `null + 1`（字面量）一直是编译错误 —— 操作数类型就是 TYPE_NULL。但另两种写法在
// **编译期同样能确定**是 null，此前只能等运行期才报错：
//   ① `var a = null`     —— 未锁定类型的变量，初值是 null 字面量
//   ② `int? a = null`    —— 可空值类型变量，初值是 null 字面量
// 只要该变量**此后没被写过**（任何写操作都会清掉 Symbol.is_null_value），
// 用到它时值就确定为 null ⇒ 直接报错，不必等到运行期（那里报的是同一句话的运行时版本）。
// 教程对 `int? a = null; a + 1` 的承诺原本只是"编译警告 + 运行时报错"，这里把它提到编译期。
//
// ⚠ 判据是「**宁漏勿误报**」：误报会让本来能跑的程序编译不过，比漏报严重得多。
//   ⇒ 只在**能确定**时置位（见 visit_var.inc 的置位点，以及每个写变量的清位点）；
//     拿不准（被别处赋过值、字段、全局、参数…）一律不报，
//     由 VM 的运行期检查兜底（src/vm/vminc/run/03_arith.inc 的 OP_ADD_INT 等）。
void report_known_null_name(Semantic* s, const char* name, int line, int column) {
    (void)s; // 避免未使用警告
    if (!name) return;
    char msg[BUFFER_MEDIUM];
    snprintf(msg, sizeof(msg),
             "变量 '%s' 的值确定为 null（声明为 null 之后未重新赋值），不能参与算术运算"
             " —— 请先赋初值，或在 `if %s != null` 分支内使用",
             name, name);
    error_add_at(ERR_TYPE_MISMATCH, line, column, msg);
}

// 表达式形态的入口：只有**普通变量**才可能带这个标志（见 Symbol.is_null_value 的说明）
static void report_known_null_operand(Semantic* s, Ast* operand, Ast* report_at) {
    if (!operand || operand->kind != AST_VAR || !operand->u.var.name) return;
    Symbol* sym = scope_resolve(s->current, operand->u.var.name);
    if (!sym || !sym->is_null_value) return;
    report_known_null_name(s, operand->u.var.name, report_at->line, report_at->column);
}

// 与下面各分支里 `TYPE_NULL ⇒ 报错` 的那组运算符**严格同集**（多/少一个都会跟既有口径打架）
static int is_arith_or_bitop_for_null_check(LenoTokenType op) {
    switch (op) {
        case TOK_PLUS: case TOK_MINUS: case TOK_STAR: case TOK_SLASH: case TOK_MOD:
        case TOK_BITAND: case TOK_BITOR: case TOK_BITXOR:
        case TOK_SHL: case TOK_SHR: case TOK_USHR:
            return 1;
        default:
            return 0;
    }
}

TypeInfo* infer_expr_type(Semantic* s, Ast* ast) {
    if (!ast) return type_new(TYPE_ANY);
    
    // 检查类型缓存，避免重复推断同一表达式
    if (ast->cached_type) {
        // clib 调用的 cached_type 是 TYPE_CLIB（供 codegen 识别），
        // 但表达式实际类型应该是映射后的 Leno 类型（零摩擦）
        if (ast->kind == AST_MODULE_CALL && ast->cached_type->kind == TYPE_CLIB && ast->cached_type->struct_name) {
            // 优先使用 visit 阶段缓存的 clib 返回类型（已映射为 Leno 类型）
            if (ast->u.module_call.clib_return_type) {
                return type_copy(ast->u.module_call.clib_return_type);
            }
            // 降级：从作用域查找 clib 符号（可能函数信息为空，因为 clib 定义在其他模块中）
            Symbol* clib_sym = scope_resolve(s->root_scope, ast->cached_type->struct_name);
            if (!clib_sym) {
                clib_sym = scope_resolve(s->current, ast->cached_type->struct_name);
            }
            // 如果作用域中的符号没有函数信息，从导入模块的符号表查找
            if (!clib_sym || clib_sym->clib_func_count == 0) {
                const char* clib_name = ast->cached_type->struct_name;
                for (int mi = 0; mi < s->imported_module_count; mi++) {
                    ImportedModuleInfo* info = &s->imported_modules[mi];
                    if (info->sym_table) {
                        ModuleClibSymbol* mod_clib = module_symbol_table_find_clib(info->sym_table, clib_name);
                        if (mod_clib && mod_clib->func_count > 0) {
                            const char* func_name = ast->u.module_call.method_name;
                            for (int fi = 0; fi < mod_clib->func_count; fi++) {
                                if (strcmp(mod_clib->funcs[fi].name, func_name) == 0) {
                                    TypeKind rk = mod_clib->funcs[fi].return_type;
                                    // C 类型 → Leno 类型映射
                                    TypeKind leno_rk = c_layout_type_to_leno(rk);
                                    if (leno_rk != rk) {
                                        return type_new(leno_rk);
                                    }
                                    switch (rk) {
                                        case TYPE_PTR: {
                                            TypeInfo* t = type_new(TYPE_PTR);
                                            t->struct_name = strdup("Ptr");
                                            return t;
                                        }
                                        case TYPE_PTR_GENERIC: {
                                            if (mod_clib->funcs[fi].return_element_type != TYPE_UNKNOWN) {
                                                return type_ptr_generic(type_new(mod_clib->funcs[fi].return_element_type));
                                            } else {
                                                TypeInfo* t = type_new(TYPE_PTR);
                                                t->struct_name = strdup("Ptr");
                                                return t;
                                            }
                                        }
                                        case TYPE_BOOL:
                                            return type_new(TYPE_BOOL);
                                        case TYPE_NULL:
                                            return type_new(TYPE_NULL);
                                        default:
                                            return type_new(rk);
                                    }
                                }
                            }
                            break; // 找到了 clib 但没有匹配的函数，不需要继续搜索其他模块
                        }
                    }
                }
            }
            if (clib_sym && clib_sym->clib_func_count > 0) {
                const char* func_name = ast->u.module_call.method_name;
                for (int i = 0; i < clib_sym->clib_func_count; i++) {
                    if (strcmp(clib_sym->clib_func_names[i], func_name) == 0) {
                        TypeInfo* ret_type = clib_sym->clib_func_return_types[i];
                        TypeKind rk = ret_type ? ret_type->kind : TYPE_NULL;
                        // C 类型 → Leno 类型映射（使用 c_layout_type_to_leno 统一处理）
                        TypeKind leno_rk = c_layout_type_to_leno(rk);
                        if (leno_rk != rk) {
                            return type_new(leno_rk);
                        }
                        switch (rk) {
                            case TYPE_PTR: {
                                TypeInfo* t = type_new(TYPE_PTR);
                                t->struct_name = strdup("Ptr");
                                return t;
                            }
                            case TYPE_PTR_GENERIC: {
                                // 保留元素类型信息，使类型安全检查生效
                                if (ret_type->element_type) {
                                    return type_ptr_generic(type_copy(ret_type->element_type));
                                } else {
                                    TypeInfo* t = type_new(TYPE_PTR);
                                    t->struct_name = strdup("Ptr");
                                    return t;
                                }
                            }
                            case TYPE_BOOL:
                                return type_new(TYPE_BOOL);
                            case TYPE_NULL:
                                return type_new(TYPE_NULL);
                            default:
                                return type_copy(ret_type);
                        }
                    }
                }
            }
        }
        // AST_INDEX 的 TYPE_ANY 缓存可能被类型守卫收窄，需要重新检查
        if (ast->cached_type && ast->cached_type->kind != TYPE_ANY) {
            return type_copy(ast->cached_type);
        }
        if (ast->cached_type && ast->kind != AST_INDEX) {
            return type_copy(ast->cached_type);
        }
    }
    
    TypeInfo* result = NULL;
    
    switch (ast->kind) {
        case AST_NUM: {
            if (ast->u.num.is_bigint) {
                ast->cached_type = type_new(TYPE_BIGINT);
                return type_copy(ast->cached_type);
            }
            // 根据原始字面量是否有小数点来判断类型
            if (ast->u.num.is_float) {
                ast->cached_type = type_new(TYPE_FLOAT);
            } else {
                ast->cached_type = type_new(TYPE_INT);
            }
            return type_copy(ast->cached_type);
        }
        case AST_STRING:
            ast->cached_type = type_new(TYPE_STRING);
            return type_copy(ast->cached_type);
        case AST_INTERP_STRING:
            for (int i = 0; i < ast->u.interp_string.count - 1; i++) {
                if (ast->u.interp_string.exprs[i]) {
                    TypeInfo* expr_type = infer_expr_type(s, ast->u.interp_string.exprs[i]);
                    if (expr_type) {
                        // 检查插值表达式类型是否可转换为字符串
                        // 允许：string, int, float, bool, null, bigint, array, dict
                        // 所有类型都可以通过运行时 toString 转换为字符串
                        (void)expr_type; // 避免未使用警告
                        type_free(expr_type);
                    }
                }
            }
            ast->cached_type = type_new(TYPE_STRING);
            return type_copy(ast->cached_type);
        case AST_BOOL:
            ast->cached_type = type_new(TYPE_BOOL);
            return type_copy(ast->cached_type);
        case AST_NULL:
            ast->cached_type = type_new(TYPE_NULL);
            return type_copy(ast->cached_type);
        case AST_ARRAY: {
            TypeInfo* result = NULL;
            if (ast->u.array.count == 0) {
                // 空数组推断为 Array（元素类型未指定）
                result = type_array(NULL);
            } else {
                TypeInfo* element_type = NULL;
                // 记录当前元素类型已知的公共 face 名称（用于 struct 类型推断）
                char* common_face_name = NULL;
                for (int i = 0; i < ast->u.array.count; i++) {
                    TypeInfo* elem_type = infer_expr_type(s, ast->u.array.items[i]);
                    if (!element_type) {
                        element_type = elem_type;
                        // 如果第一个元素是 struct，初始化公共 face 集合
                        if (element_type && element_type->kind == TYPE_STRUCT && element_type->struct_name) {
                            ObjStructDef* sdef = struct_def_find(element_type->struct_name);
                            if (sdef && sdef->impl_count > 0) {
                                common_face_name = strdup(sdef->impl_names[0]);
                            }
                        }
                    } else if (type_equals(element_type, elem_type)) {
                        // 类型相同，继续
                        type_free(elem_type);
                    } else {
                        // 类型不同，尝试类型提升
                        int promoted = 0;
                        
                        // int + float -> float
                        if ((element_type->kind == TYPE_INT && elem_type->kind == TYPE_FLOAT) ||
                            (element_type->kind == TYPE_FLOAT && elem_type->kind == TYPE_INT)) {
                            TypeInfo* new_type = type_new(TYPE_FLOAT);
                            type_free(element_type);
                            type_free(elem_type);
                            element_type = new_type;
                            promoted = 1;
                        }
                        // int/float + bigint -> bigint
                        else if ((element_type->kind == TYPE_INT && elem_type->kind == TYPE_BIGINT) ||
                                 (element_type->kind == TYPE_BIGINT && elem_type->kind == TYPE_INT) ||
                                 (element_type->kind == TYPE_FLOAT && elem_type->kind == TYPE_BIGINT) ||
                                 (element_type->kind == TYPE_BIGINT && elem_type->kind == TYPE_FLOAT)) {
                            TypeInfo* new_type = type_new(TYPE_BIGINT);
                            type_free(element_type);
                            type_free(elem_type);
                            element_type = new_type;
                            promoted = 1;
                        }
                        // float/bigint + bigint/float 已经是 BIGINT
                        else if (element_type->kind == TYPE_BIGINT && elem_type->kind == TYPE_BIGINT) {
                            type_free(elem_type);
                            promoted = 1;
                        }
                        // struct + struct：查找公共 face
                        else if (element_type->kind == TYPE_STRUCT && elem_type->kind == TYPE_STRUCT
                                 && element_type->struct_name && elem_type->struct_name) {
                            ObjStructDef* cur_sdef = struct_def_find(elem_type->struct_name);
                            if (cur_sdef && cur_sdef->impl_count > 0 && common_face_name) {
                                // 检查当前 struct 是否也实现了公共 face
                                int found = 0;
                                for (int fi = 0; fi < cur_sdef->impl_count; fi++) {
                                    if (strcmp(cur_sdef->impl_names[fi], common_face_name) == 0) {
                                        found = 1;
                                        break;
                                    }
                                }
                                if (found) {
                                    // 公共 face 仍有效，提升为 face 类型
                                    type_free(element_type);
                                    type_free(elem_type);
                                    element_type = type_new(TYPE_FACE);
                                    element_type->struct_name = strdup(common_face_name);
                                    promoted = 1;
                                } else {
                                    // 公共 face 不匹配，尝试找新的公共 face
                                    // 遍历当前 struct 的 impl，看是否与之前所有 struct 都匹配
                                    free(common_face_name);
                                    common_face_name = NULL;
                                }
                            } else {
                                // 没有 impl 或没有公共 face，无法提升
                                free(common_face_name);
                                common_face_name = NULL;
                            }
                        }
                        // face + struct：检查 struct 是否实现该 face
                        else if (element_type->kind == TYPE_FACE && elem_type->kind == TYPE_STRUCT
                                 && element_type->struct_name && elem_type->struct_name) {
                            ObjStructDef* cur_sdef = struct_def_find(elem_type->struct_name);
                            if (cur_sdef && struct_implements_face(cur_sdef, face_def_find(element_type->struct_name))) {
                                type_free(elem_type);
                                promoted = 1;
                            } else {
                                free(common_face_name);
                                common_face_name = NULL;
                            }
                        }
                        // struct + face：检查 struct 是否实现该 face，提升为 face 类型
                        else if (element_type->kind == TYPE_STRUCT && elem_type->kind == TYPE_FACE
                                 && element_type->struct_name && elem_type->struct_name) {
                            ObjStructDef* cur_sdef = struct_def_find(element_type->struct_name);
                            ObjFaceDef* fdef = face_def_find(elem_type->struct_name);
                            if (cur_sdef && struct_implements_face(cur_sdef, fdef)) {
                                free(common_face_name);
                                common_face_name = strdup(elem_type->struct_name);
                                type_free(element_type);
                                type_free(elem_type);
                                element_type = type_new(TYPE_FACE);
                                element_type->struct_name = strdup(common_face_name);
                                promoted = 1;
                            } else {
                                free(common_face_name);
                                common_face_name = NULL;
                            }
                        }
                        // face + face：检查是否同一个 face
                        else if (element_type->kind == TYPE_FACE && elem_type->kind == TYPE_FACE
                                 && element_type->struct_name && elem_type->struct_name) {
                            if (strcmp(element_type->struct_name, elem_type->struct_name) == 0) {
                                type_free(elem_type);
                                promoted = 1;
                            } else {
                                free(common_face_name);
                                common_face_name = NULL;
                            }
                        }
                        
                        if (!promoted) {
                            // 无法类型提升，返回 any[]
                            type_free(elem_type);
                            type_free(element_type);
                            free(common_face_name);
                            common_face_name = NULL;
                            result = type_array(type_new(TYPE_ANY));
                            break;
                        }
                    }
                }
                if (!result) {
                    if (!element_type) {
                        element_type = type_new(TYPE_ANY);
                    }
                    result = type_array(element_type);
                }
                free(common_face_name);
            }
            ast->cached_type = type_copy(result);
            return result;
        }
        case AST_DICT: {
            TypeInfo* result = NULL;
            if (ast->u.dict.count == 0) {
                // 空字典返回 NULL 键值类型（类似空数组），允许后续类型推断
                result = type_dict(NULL, NULL);
            } else {
                TypeInfo* key_type = NULL;
                TypeInfo* value_type = NULL;
                for (int i = 0; i < ast->u.dict.count; i++) {
                    // 推断键类型
                    Ast* key_ast = ast->u.dict.entries[i].key;
                    TypeInfo* curr_key = NULL;
                    if (key_ast->kind == AST_NUM) {
                        curr_key = type_new(TYPE_INT);
                    } else {
                        // AST_STRING（标识符也转为字符串）或其他
                        curr_key = type_new(TYPE_STRING);
                    }
                    if (!key_type) {
                        key_type = curr_key;
                    } else if (!type_equals(key_type, curr_key)) {
                        type_free(key_type);
                        type_free(curr_key);
                        key_type = type_new(TYPE_ANY);
                    } else {
                        type_free(curr_key);
                    }

                    // 推断值类型
                    TypeInfo* curr_value = infer_expr_type(s, ast->u.dict.entries[i].value);
                    if (!value_type) {
                        value_type = curr_value;
                    } else {
                        if (!type_equals(value_type, curr_value)) {
                            type_free(value_type);
                            value_type = type_new(TYPE_ANY);
                        }
                        type_free(curr_value);
                    }
                }
                if (!key_type) key_type = type_new(TYPE_STRING);
                if (!value_type) value_type = type_new(TYPE_ANY);
                result = type_dict(key_type, value_type);
            }
            ast->cached_type = type_copy(result);
            return result;
        }
        case AST_VAR: {
            if (ast->cached_type) {
                return type_copy(ast->cached_type);
            }
            Symbol* sym = scope_resolve(s->current, ast->u.var.name);
            if (sym && sym->type) {
                ast->cached_type = type_copy(sym->type);
                return type_copy(sym->type);
            }
            // 函数名引用：从函数表构造 TYPE_FUNCTION 类型
            if (sym && (sym->kind == SYM_GLOBAL_FUNC || sym->kind == SYM_LOCAL)) {
                Ast* func_def = func_table_find(&s->func_table, ast->u.var.name);
                if (func_def && func_def->kind == AST_FUNC_DEF) {
                    TypeInfo* ret_type = func_def->u.func.return_type ? type_copy(func_def->u.func.return_type) : type_new(TYPE_ANY);
                    TypeInfo* func_type = type_function(ret_type, func_def->u.func.param_types, func_def->u.func.pcnt);
                    ast->cached_type = func_type;
                    return type_copy(func_type);
                }
            }
            if (ast->u.var.ref.type_kind != TYPE_ANY && ast->u.var.ref.type_kind != TYPE_INFER) {
                ast->cached_type = type_new(ast->u.var.ref.type_kind);
                if (ast->u.var.ref.struct_name &&
                    (ast->u.var.ref.type_kind == TYPE_STRUCT ||
                     ast->u.var.ref.type_kind == TYPE_FACE ||
                     ast->u.var.ref.type_kind == TYPE_CSTRUCT ||
                     ast->u.var.ref.type_kind == TYPE_ENUM)) {
                    ast->cached_type->struct_name = strdup(ast->u.var.ref.struct_name);
                }
            } else {
                ast->cached_type = type_new(TYPE_ANY);
            }
            return type_copy(ast->cached_type);
        }
        case AST_BINOP: {
            TypeInfo* left = infer_expr_type(s, ast->u.binop.l);
            TypeInfo* right = infer_expr_type(s, ast->u.binop.r);
            TypeInfo* result = NULL;

            // ★ T11：**确定是 null** 的操作数 ⇒ 编译错误
            //   （`var a = null; a + 1` / `int? a = null; a + 1`；不确定的走下面的警告 + VM 运行期检查）
            if (is_arith_or_bitop_for_null_check(ast->u.binop.op)) {
                report_known_null_operand(s, ast->u.binop.l, ast);
                report_known_null_operand(s, ast->u.binop.r, ast);
            }

            switch (ast->u.binop.op) {
                case TOK_PLUS: {
                    // 字符串拼接：任一操作数是 string 时结果为 string
                    if (left && right &&
                        (left->kind == TYPE_STRING || right->kind == TYPE_STRING)) {
                        result = type_new(TYPE_STRING);
                    }
                    // null 参与加法 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_NULL || right->kind == TYPE_NULL)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "null 类型不能参与算术运算，请先检查变量是否为 null");
                        result = type_new(TYPE_ANY);
                    }
                    // nullable 值类型参与算术运算 -> 警告
                    else if (left && right &&
                        ((left->nullable && (left->kind == TYPE_INT || left->kind == TYPE_FLOAT || left->kind == TYPE_BOOL || left->kind == TYPE_BIGINT)) ||
                         (right->nullable && (right->kind == TYPE_INT || right->kind == TYPE_FLOAT || right->kind == TYPE_BOOL || right->kind == TYPE_BIGINT)))) {
                        check_nullable_arith(left, right, ast->line, ast->column);
                        // 继续走数值类型推导
                        if (left->kind == TYPE_GENERIC_PARAM) {
                            result = type_copy(left);
                        } else if (right->kind == TYPE_GENERIC_PARAM) {
                            result = type_copy(left);
                        } else if (left->kind == TYPE_BIGINT || right->kind == TYPE_BIGINT) {
                            result = type_new(TYPE_BIGINT);
                        } else if (left->kind == TYPE_INT && right->kind == TYPE_INT) {
                            result = type_new(TYPE_INT);
                        } else if (left->kind == TYPE_FLOAT || right->kind == TYPE_FLOAT) {
                            result = type_new(TYPE_FLOAT);
                        } else {
                            result = type_new(TYPE_ANY);
                        }
                    }
                    // 如果任一操作数是 ANY，结果是 ANY
                    else if (left && right &&
                        (left->kind == TYPE_ANY || right->kind == TYPE_ANY)) {
                        result = type_new(TYPE_ANY);
                    }
                    // bool 参与加法 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_BOOL || right->kind == TYPE_BOOL)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "bool 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // 数值类型推导：int + int = int, int + float = float, float + float = float
                    // bigint + int/float/bigint = bigint
                    // 泛型参数：T + T = T（运行时确定具体类型）
                    else if (left && right) {
                        // 泛型参数参与运算：T + 1 = T（保持泛型类型）
                        if (left->kind == TYPE_GENERIC_PARAM) {
                            result = type_copy(left);
                        } else if (right->kind == TYPE_GENERIC_PARAM) {
                            result = type_copy(left);
                        } else if (left->kind == TYPE_BIGINT || right->kind == TYPE_BIGINT) {
                            result = type_new(TYPE_BIGINT);
                        } else if (left->kind == TYPE_INT && right->kind == TYPE_INT) {
                            result = type_new(TYPE_INT);
                        } else if (type_equals(left, right)) {
                            // 相同类型（包括泛型参数 TYPE_GENERIC_PARAM），结果保持该类型
                            result = type_copy(left);
                        } else if (left->kind == TYPE_CLIB || right->kind == TYPE_CLIB) {
                            // clib 类型：返回的是 C 类型（i32/i64/f32 等），需要 as int/as float 显式转换
                            error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "clib 返回的 C 类型不能直接参与运算，请用 as int 或 as float 显式转换");
                            result = type_new(TYPE_ANY);
                        } else {
                            result = type_new(TYPE_FLOAT);
                        }
                    } else {
                        result = type_new(TYPE_ANY);
                    }
                    break;
                }
                case TOK_MINUS:
                case TOK_STAR: {
                    // null 参与减法/乘法 -> 编译错误
                    if (left && right &&
                        (left->kind == TYPE_NULL || right->kind == TYPE_NULL)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "null 类型不能参与算术运算，请先检查变量是否为 null");
                        result = type_new(TYPE_ANY);
                    }
                    // nullable 值类型参与算术运算 -> 警告
                    else if (left && right &&
                        ((left->nullable && (left->kind == TYPE_INT || left->kind == TYPE_FLOAT || left->kind == TYPE_BOOL || left->kind == TYPE_BIGINT)) ||
                         (right->nullable && (right->kind == TYPE_INT || right->kind == TYPE_FLOAT || right->kind == TYPE_BOOL || right->kind == TYPE_BIGINT)))) {
                        check_nullable_arith(left, right, ast->line, ast->column);
                        if (left->kind == TYPE_GENERIC_PARAM) {
                            result = type_copy(left);
                        } else if (right->kind == TYPE_GENERIC_PARAM) {
                            result = type_copy(left);
                        } else if (left->kind == TYPE_BIGINT || right->kind == TYPE_BIGINT) {
                            result = type_new(TYPE_BIGINT);
                        } else if (left->kind == TYPE_INT && right->kind == TYPE_INT) {
                            result = type_new(TYPE_INT);
                        } else if (left->kind == TYPE_FLOAT || right->kind == TYPE_FLOAT) {
                            result = type_new(TYPE_FLOAT);
                        } else {
                            result = type_new(TYPE_ANY);
                        }
                    }
                    // 如果任一操作数是 ANY，结果是 ANY
                    else if (left && right &&
                        (left->kind == TYPE_ANY || right->kind == TYPE_ANY)) {
                        result = type_new(TYPE_ANY);
                    }
                    // bool 参与减法/乘法 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_BOOL || right->kind == TYPE_BOOL)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "bool 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // string 参与减法/乘法 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_STRING || right->kind == TYPE_STRING)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "string 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // 数值类型推导
                    else if (left && right) {
                        // 泛型参数参与运算：T - 1 = T, T * 2 = T（保持泛型类型）
                        if (left->kind == TYPE_GENERIC_PARAM) {
                            result = type_copy(left);
                        } else if (right->kind == TYPE_GENERIC_PARAM) {
                            result = type_copy(left);
                        } else if (left->kind == TYPE_BIGINT || right->kind == TYPE_BIGINT) {
                            result = type_new(TYPE_BIGINT);
                        } else if (left->kind == TYPE_INT && right->kind == TYPE_INT) {
                            result = type_new(TYPE_INT);
                        } else if (type_equals(left, right)) {
                            result = type_copy(left);
                        } else if (left->kind == TYPE_CLIB || right->kind == TYPE_CLIB) {
                            error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "clib 返回的 C 类型不能直接参与运算，请用 as int 或 as float 显式转换");
                            result = type_new(TYPE_ANY);
                        } else {
                            result = type_new(TYPE_FLOAT);
                        }
                    } else {
                        result = type_new(TYPE_ANY);
                    }
                    break;
                }
                case TOK_SLASH:
                    // null 参与除法 -> 编译错误
                    if (left && right &&
                        (left->kind == TYPE_NULL || right->kind == TYPE_NULL)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "null 类型不能参与算术运算，请先检查变量是否为 null");
                        result = type_new(TYPE_ANY);
                    }
                    // nullable 值类型参与算术运算 -> 警告
                    else if (left && right &&
                        ((left->nullable && (left->kind == TYPE_INT || left->kind == TYPE_FLOAT || left->kind == TYPE_BOOL || left->kind == TYPE_BIGINT)) ||
                         (right->nullable && (right->kind == TYPE_INT || right->kind == TYPE_FLOAT || right->kind == TYPE_BOOL || right->kind == TYPE_BIGINT)))) {
                        check_nullable_arith(left, right, ast->line, ast->column);
                        if (left->kind == TYPE_INT && right->kind == TYPE_INT) {
                            result = type_new(TYPE_INT);
                        } else if (left->kind == TYPE_FLOAT || right->kind == TYPE_FLOAT) {
                            result = type_new(TYPE_FLOAT);
                        } else if (left->kind == TYPE_BIGINT || right->kind == TYPE_BIGINT) {
                            result = type_new(TYPE_BIGINT);
                        } else {
                            result = type_new(TYPE_ANY);
                        }
                    }
                    // 如果任一操作数是 ANY，结果是 ANY
                    else if (left && right &&
                        (left->kind == TYPE_ANY || right->kind == TYPE_ANY)) {
                        result = type_new(TYPE_ANY);
                    }
                    // bool 参与除法 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_BOOL || right->kind == TYPE_BOOL)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "bool 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // string 参与除法 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_STRING || right->kind == TYPE_STRING)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "string 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // 泛型参数参与运算：T / 1 = T（保持泛型类型）
                    else if (left && right &&
                             (left->kind == TYPE_GENERIC_PARAM || right->kind == TYPE_GENERIC_PARAM)) {
                        result = (left->kind == TYPE_GENERIC_PARAM) ? type_copy(left) : type_copy(left);
                    }
                    // int / int = int
                    else if (left && right &&
                             (left->kind == TYPE_INT && right->kind == TYPE_INT)) {
                        result = type_new(TYPE_INT);
                    }
                    // float involved → float (int/float, bigint/float, etc.)
                    else if (left && right &&
                             (left->kind == TYPE_FLOAT || right->kind == TYPE_FLOAT)) {
                        result = type_new(TYPE_FLOAT);
                    }
                    // bigint involved → bigint (bigint/int, bigint/bigint)
                    else if (left && right &&
                             (left->kind == TYPE_BIGINT || right->kind == TYPE_BIGINT)) {
                        result = type_new(TYPE_BIGINT);
                    }
                    // 相同类型（包括泛型参数），结果保持该类型
                    else if (left && right && type_equals(left, right)) {
                        result = type_copy(left);
                    }
                    else if (left && right &&
                             (left->kind == TYPE_CLIB || right->kind == TYPE_CLIB)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "clib 返回的 C 类型不能直接参与运算，请用 as int 或 as float 显式转换");
                        result = type_new(TYPE_ANY);
                    }
                    else {
                        result = type_new(TYPE_FLOAT);
                    }
                    break;
                case TOK_MOD:
                case TOK_BITAND:
                case TOK_BITOR:
                case TOK_BITXOR:
                case TOK_SHL:
                case TOK_SHR:
                case TOK_USHR:
                    // null 参与位运算/取模 -> 编译错误
                    if (left && right &&
                        (left->kind == TYPE_NULL || right->kind == TYPE_NULL)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "null 类型不能参与算术运算，请先检查变量是否为 null");
                        result = type_new(TYPE_ANY);
                    }
                    // nullable 值类型参与位运算/取模 -> 警告
                    else if (left && right &&
                        ((left->nullable && (left->kind == TYPE_INT || left->kind == TYPE_FLOAT || left->kind == TYPE_BOOL || left->kind == TYPE_BIGINT)) ||
                         (right->nullable && (right->kind == TYPE_INT || right->kind == TYPE_FLOAT || right->kind == TYPE_BOOL || right->kind == TYPE_BIGINT)))) {
                        check_nullable_arith(left, right, ast->line, ast->column);
                        if (left->kind == TYPE_BIGINT || right->kind == TYPE_BIGINT) {
                            result = type_new(TYPE_BIGINT);
                        } else {
                            result = type_new(TYPE_INT);
                        }
                    }
                    // bool 参与位运算/取模 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_BOOL || right->kind == TYPE_BOOL)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "bool 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // string 参与位运算/取模 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_STRING || right->kind == TYPE_STRING)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "string 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // 位运算和取模：如果任一操作数是 bigint，返回 bigint
                    else if (left && right &&
                        (left->kind == TYPE_BIGINT || right->kind == TYPE_BIGINT)) {
                        result = type_new(TYPE_BIGINT);
                    }
                    // 否则返回整数
                    else {
                        result = type_new(TYPE_INT);
                    }
                    break;
                case TOK_LT:
                case TOK_GT:
                case TOK_LE:
                case TOK_GE:
                    // 大小比较：检查类型兼容性
                    if (left && right &&
                        left->kind != TYPE_ANY && right->kind != TYPE_ANY) {
                        // 允许：int/float 之间比较
                        int left_is_num = (left->kind == TYPE_INT || left->kind == TYPE_FLOAT || left->kind == TYPE_BIGINT);
                        int right_is_num = (right->kind == TYPE_INT || right->kind == TYPE_FLOAT || right->kind == TYPE_BIGINT);
                        // 允许：string 和 string 比较
                        int left_is_string = (left->kind == TYPE_STRING);
                        int right_is_string = (right->kind == TYPE_STRING);
                        // 允许：泛型参数参与比较（运行时确定具体类型）
                        int left_is_generic = (left->kind == TYPE_GENERIC_PARAM);
                        int right_is_generic = (right->kind == TYPE_GENERIC_PARAM);
                        // 检查不兼容的组合
                        if ((left_is_num && right_is_string) || (left_is_string && right_is_num)) {
                            error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "数值类型不能和 string 类型进行大小比较");
                        } else if (left_is_string && right_is_string) {
                            // string 和 string 比较是允许的
                        } else if (!left_is_num && !right_is_num && !left_is_string && !right_is_string
                                   && !left_is_generic && !right_is_generic) {
                            // 其他不兼容类型（排除泛型参数）
                            error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "不兼容的类型不能进行大小比较");
                        }
                    }
                    // any 类型不能参与大小比较（如无类型参数的 Array 元素）：
                    // 编译期能确定的类型问题不放过到运行时
                    else if (left && right &&
                               (left->kind == TYPE_ANY || right->kind == TYPE_ANY)) {
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column,
                            "any 类型不能参与大小比较，请指定数组元素类型（如 Array[int]）");
                    }
                    result = type_new(TYPE_BOOL);
                    break;
                case TOK_EQEQ:
                case TOK_NEQ:
                case TOK_AND:
                case TOK_OR:
                case TOK_IN:
                case TOK_NOT_IN:
                    result = type_new(TYPE_BOOL);
                    break;
                case TOK_NULL_COALESCE: {
                    // left ?? right → 结果类型 = left 去可空化
                    if (left && left->nullable) {
                        result = type_copy(left);
                        result->nullable = 0;
                    } else if (left) {
                        result = type_copy(left);
                    } else {
                        result = type_new(TYPE_ANY);
                    }
                    break;
                }
                default:
                    result = type_new(TYPE_ANY);
                    break;
            }
            type_free(left);
            type_free(right);
            ast->cached_type = type_copy(result);
            return result;
        }
        case AST_UNARY: {
            TypeInfo* operand = infer_expr_type(s, ast->u.unary.operand);
            TypeInfo* result = NULL;
            if (ast->u.unary.op == TOK_NOT) {
                type_free(operand);
                result = type_new(TYPE_BOOL);
            } else if (ast->u.unary.op == TOK_MINUS) {
                report_known_null_operand(s, ast->u.unary.operand, ast);   // ★ T11：同口径
                // 负号：bool 参与 -> 编译错误
                if (operand && operand->kind == TYPE_BOOL) {
                    error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "bool 类型不能参与算术运算");
                    type_free(operand);
                    result = type_new(TYPE_ANY);
                }
                // 负号：string 参与 -> 编译错误
                else if (operand && operand->kind == TYPE_STRING) {
                    error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "string 类型不能参与算术运算");
                    type_free(operand);
                    result = type_new(TYPE_ANY);
                }
                // 负号：null 参与 -> 编译错误
                else if (operand && operand->kind == TYPE_NULL) {
                    error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, "null 类型不能参与算术运算，请先检查变量是否为 null");
                    type_free(operand);
                    result = type_new(TYPE_ANY);
                }
                // 负号：nullable 值类型参与 -> 警告
                else if (operand && operand->nullable &&
                    (operand->kind == TYPE_INT || operand->kind == TYPE_FLOAT ||
                     operand->kind == TYPE_BOOL || operand->kind == TYPE_BIGINT)) {
                    warning_add_at(WARN_NULLABLE_ARITH, ast->line, ast->column,
                        "nullable 值类型参与算术运算，运行时可能为 null，请先检查变量是否为 null（如 if a != null）");
                    result = operand;
                }
                // 保持原有数值类型
                else {
                    result = operand;
                }
            } else {
                result = operand;
            }
            ast->cached_type = type_copy(result);
            return result;
        }
        case AST_CALL: {
            // 先对所有参数进行类型推断（触发参数中的类型检查）
            for (int i = 0; i < ast->u.call.args.count; i++) {
                TypeInfo* arg_type = infer_expr_type(s, ast->u.call.args.items[i]);
                if (arg_type) type_free(arg_type);
            }

            if (ast->u.call.callee && ast->u.call.callee->kind == AST_VAR) {
                const char* func_name = ast->u.call.callee->u.var.name;

                // 首先检查变量符号的类型（可能是函数类型）
                // 同时检查是否是 async 函数（async 函数返回 Future）
                Ast* func_def_for_async = func_table_find(&s->func_table, func_name);
                Symbol* sym = scope_resolve(s->current, func_name);
                if (sym && sym->type && sym->type->kind == TYPE_FUNCTION && sym->type->return_type) {
                    TypeInfo* ret = type_copy(sym->type->return_type);
                    
                    // async 函数返回 Future 而非声明的返回类型
                    if (func_def_for_async && func_def_for_async->kind == AST_FUNC_DEF &&
                        func_def_for_async->u.func.is_async) {
                        type_free(ret);
                        ret = type_new(TYPE_FUTURE);
                    }
                    
                    // 泛型函数调用：用推断的类型参数替换泛型参数
                    if (ast->u.call.generic_type_count > 0 && ast->u.call.generic_type_names && ast->u.call.generic_type_args) {
                        for (int p = 0; p < ast->u.call.generic_type_count; p++) {
                            TypeInfo* sub = type_substitute(ret,
                                ast->u.call.generic_type_names[p],
                                ast->u.call.generic_type_args[p]);
                            if (sub != ret) {
                                type_free(ret);
                                ret = sub;
                            }
                        }
                    }
                    
                    ast->cached_type = type_copy(ret);
                    return ret;
                }

                // 使用哈希表 O(1) 查找函数定义
                Ast* func_def = func_table_find(&s->func_table, func_name);

                if (func_def && func_def->kind == AST_FUNC_DEF) {
                    TypeInfo* ret = NULL;
                    // async 函数返回 Future 而非声明的返回类型
                    if (func_def->u.func.is_async) {
                        ret = type_new(TYPE_FUTURE);
                    } else if (func_def->u.func.return_type &&
                               func_def->u.func.return_type->kind != TYPE_INFER) {
                        ret = type_copy(func_def->u.func.return_type);
                    } else {
                        // 无返回类型注解（TYPE_INFER）或无返回类型：统一返回 any
                        ret = type_new(TYPE_ANY);
                    }

                    // 泛型函数调用：用推断的类型参数替换泛型参数
                    if (ast->u.call.generic_type_count > 0 && ast->u.call.generic_type_names && ast->u.call.generic_type_args) {
                        for (int p = 0; p < ast->u.call.generic_type_count; p++) {
                            TypeInfo* sub = type_substitute(ret,
                                ast->u.call.generic_type_names[p],
                                ast->u.call.generic_type_args[p]);
                            type_free(ret);
                            ret = sub;
                        }
                    }

                    ast->cached_type = type_copy(ret);
                    return ret;
                }

                // 找不到函数定义时，检查变量符号的类型
                if (sym && sym->type) {
                    ast->cached_type = type_copy(sym->type);
                    return type_copy(ast->cached_type);
                }

                // ① 完整规格优先（v3.2.8）：内置函数通道此前只有 Kind 槽，表达不了
                //    `ExecResult{...}` 这类**带名字/带字段**的返回类型（`_exec` 的
                //    `[output, code]` 只能表达成 `Array[any]`）⇒ 有规格就用规格
                //    （与模块方法的 return_spec 同一优先级规则）。
                //    NTYPE_BY_ARITY（双形态内置，如 `_env` 读/写返回不同类型）按
                //    **实参个数**先定型（2026-09-28，注册即数据，语义侧无名字特判）。
                {
                    const NativeTypeSpec* fn_spec = native_get_return_spec(func_name);
                    if (fn_spec) {
                        fn_spec = native_type_spec_resolve_for_call(fn_spec, ast->u.call.args.count);
                        TypeInfo* spec_ret = native_type_spec_to_info(fn_spec);
                        if (spec_ret) {
                            ast->cached_type = type_copy(spec_ret);
                            return spec_ret;
                        }
                    }
                }

                TypeKind return_type = native_get_return_type(func_name);
                TypeInfo* ret_type = type_new(return_type);
                // 如果是数组类型，补充元素类型信息
                if (return_type == TYPE_ARRAY) {
                    TypeKind elem = native_get_return_element_type(func_name);
                    if (elem != TYPE_UNKNOWN) {
                        ret_type->element_type = type_new(elem);
                    }
                }
                ast->cached_type = type_copy(ret_type);
                return ret_type;
            }
            // 处理实例方法调用：obj.method()
            if (ast->u.call.callee && ast->u.call.callee->kind == AST_INDEX) {
                Ast* index_ast = ast->u.call.callee;
                if (index_ast->u.index.index && index_ast->u.index.index->kind == AST_STRING) {
                    const char* method_name = index_ast->u.index.index->u.string.value;
                    
                    // 推断对象类型
                    TypeInfo* obj_type = infer_expr_type(s, index_ast->u.index.obj);
                    if (obj_type) {
                        // 处理 clib 方法调用
                        if (obj_type->kind == TYPE_CLIB && obj_type->struct_name) {
                            Symbol* clib_sym = scope_resolve(s->current, obj_type->struct_name);
                            if (clib_sym && clib_sym->clib_func_count > 0) {
                                for (int ci = 0; ci < clib_sym->clib_func_count; ci++) {
                                    if (strcmp(clib_sym->clib_func_names[ci], method_name) == 0) {
                                        TypeInfo* ret_type = clib_sym->clib_func_return_types[ci];
                                        TypeInfo* result2 = NULL;
                                        if (ret_type) {
                                            TypeKind rk = ret_type->kind;
                                            switch (rk) {
                                                case TYPE_I8: case TYPE_U8:
                                                case TYPE_I16: case TYPE_U16:
                                                case TYPE_I32: case TYPE_U32:
                                                case TYPE_I64: case TYPE_U64:
                                                    result2 = type_new(TYPE_INT); break;
                                                case TYPE_F32: case TYPE_F64:
                                                    result2 = type_new(TYPE_FLOAT); break;
                                                case TYPE_STR8: case TYPE_STR16:
                                                    result2 = type_new(TYPE_STRING); break;
                                                case TYPE_PTR:
                                                    result2 = type_new(TYPE_PTR);
                                                    result2->struct_name = strdup("Ptr"); break;
                                                case TYPE_PTR_GENERIC:
                                                    // 保留元素类型信息
                                                    if (ret_type->element_type) {
                                                        result2 = type_ptr_generic(type_copy(ret_type->element_type));
                                                    } else {
                                                        result2 = type_new(TYPE_PTR);
                                                        result2->struct_name = strdup("Ptr");
                                                    }
                                                    break;
                                                case TYPE_BOOL:
                                                    result2 = type_new(TYPE_BOOL); break;
                                                case TYPE_NULL:
                                                    result2 = type_new(TYPE_NULL); break;
                                                default:
                                                    result2 = type_copy(ret_type); break;
                                            }
                                        } else {
                                            result2 = type_new(TYPE_ANY);
                                        }
                                        ast->cached_type = type_copy(result2);
                                        type_free(obj_type);
                                        return result2;
                                    }
                                }
                            }
                            type_free(obj_type);
                            return type_new(TYPE_ANY);
                        }
                        // 处理泛型约束类型参数的方法调用（如 T: Printable，调用 T.format()）
                        if (obj_type->kind == TYPE_GENERIC_PARAM && obj_type->constraint_name) {
                            ObjFaceDef* constraint_face = face_def_find(obj_type->constraint_name);
                            if (constraint_face) {
                                for (int mi = 0; mi < constraint_face->method_count; mi++) {
                                    if (strcmp(constraint_face->methods[mi].name, method_name) == 0) {
                                        if (constraint_face->methods[mi].return_type) {
                                            ast->cached_type = type_copy(constraint_face->methods[mi].return_type);
                                        } else {
                                            ast->cached_type = type_new(TYPE_ANY);
                                        }
                                        type_free(obj_type);
                                        return type_copy(ast->cached_type);
                                    }
                                }
                            }
                            // 约束 face 可能在导入的模块中
                            for (int mi = 0; mi < s->imported_module_count; mi++) {
                                ImportedModuleInfo* m = &s->imported_modules[mi];
                                if (m && m->sym_table) {
                                    ModuleFaceSymbol* face_sym = module_symbol_table_find_face(m->sym_table, obj_type->constraint_name);
                                    if (face_sym) {
                                        for (int fi = 0; fi < face_sym->method_count; fi++) {
                                            if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                                                TypeInfo* ret_type = type_new(face_sym->methods[fi].return_type);
                                                if (face_sym->methods[fi].return_type == TYPE_STRUCT && face_sym->methods[fi].return_struct_name) {
                                                    ret_type->struct_name = strdup(face_sym->methods[fi].return_struct_name);
                                                }
                                                ast->cached_type = ret_type;
                                                type_free(obj_type);
                                                return type_copy(ast->cached_type);
                                            }
                                        }
                                    }
                                }
                            }
                            type_free(obj_type);
                            return type_new(TYPE_ANY);
                        }

                        // 处理 struct/cstruct/face 类型的方法调用
                        if (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT || obj_type->kind == TYPE_FACE) {
                            TypeInfo* ret = infer_method_return_type(s, obj_type, method_name);
                            if (ret) {
                                ast->cached_type = type_copy(ret);
                                type_free(obj_type);
                                return ret;
                            }
                            type_free(obj_type);
                            return type_new(TYPE_ANY);
                        }
                        
                        const char* type_name = native_get_type_name(obj_type->kind);
                        
                        // 特殊处理 cstruct 数组类型
                        if (obj_type->kind == TYPE_CSTRUCT && obj_type->struct_name && 
                            strcmp(obj_type->struct_name, "__CSTRUCT_ARRAY__") == 0) {
                            // cstruct 数组类型使用 cstruct 的方法表
                            type_name = "cstruct";
                        }
                        
                        if (type_name) {
                            // ① 规格优先（2026-09-27）：规格里可以含**关系型标签**，实例形式下
                            //    "第 0 个实参"就是**接收者** —— `Array[ARG0_ELEM]` = 元素类型跟接收者
                            //    走（copy/clear/reverse/sort/filter）、裸 `ARG0_ELEM` = pop/remove、
                            //    Dict 的 `Array[ARG0_KEY]` / `Array[ARG0_VALUE]` = keys/values。
                            //    规格是**声明式**的唯一来源 ⇒ 优先于下面按 Kind 猜的老规则。
                            //    ⚠ `NTYPE_ARG_CB_RET`（map/reduce，2026-09-28 声明化）需要**推函数体**
                            //    ⇒ 走语义侧通用推断点 spec_resolve_with_cb_ret（下标 0 = 接收者 ✓）
                            {
                                const NativeTypeSpec* inst_spec =
                                    native_get_instance_method_return_spec(type_name, method_name);
                                if (inst_spec) {
                                    TypeInfo* spec_ret = NULL;
                                    if (spec_find_cb_ret(inst_spec)) {
                                        spec_ret = spec_resolve_with_cb_ret(s, inst_spec, obj_type,
                                            ast->u.call.args.items, ast->u.call.args.count, 1);
                                        if (spec_ret) {
                                            ast->cached_type = type_copy(spec_ret);
                                            type_free(obj_type);
                                            return spec_ret;
                                        }
                                    } else {
                                        spec_ret = native_type_spec_to_info_with_args(inst_spec, obj_type);
                                        if (spec_ret) {
                                            ast->cached_type = type_copy(spec_ret);
                                            type_free(obj_type);
                                            return spec_ret;
                                        }
                                    }
                                }
                            }

                            // 获取实例方法的返回类型（毯式「ARRAY ⇒ 照抄接收者」规则已删：
                            //   meta 表里返回 TYPE_ARRAY 的 Array 方法全部已声明规格，
                            //   规格路径在上面先命中 ✓）
                            int arity;
                            TypeKind return_type = native_get_instance_method_return_type(type_name, method_name, &arity);

                            // 如果返回类型是数组，从元信息获取元素类型（规格缺失时的兜底）
                            if (return_type == TYPE_ARRAY) {
                                TypeKind elem_type = native_get_instance_method_return_element_type(type_name, method_name);
                                if (elem_type != TYPE_UNKNOWN) {
                                    type_free(obj_type);
                                    TypeInfo* arr_type = type_new(TYPE_ARRAY);
                                    arr_type->element_type = type_new(elem_type);
                                    return arr_type;
                                }
                            }

                            //（map/reduce 的推断特判已删：⓪ 族声明化后走上面的实例规格路径 ✓）

                            // Dict.get 返回类型：2 参 ⇒ 默认值类型、1 参 ⇒ 值类型 V
                            //（与 obj_sym 路径共享 infer_dict_get_return ✓）
                            // ⚠ 方法名判断**已收敛进 infer_dict_get_return 内部**（唯一判定点 ✓）
                            //   —— 别的 Dict 方法（len ⇒ int / has ⇒ bool）在元信息表里本就有
                            //   正确类型，必须让它们跳过本块 ✓（2026-09-28 修 LenoWeb）
                            {
                                TypeInfo* get_ret = infer_dict_get_return(s, obj_type, method_name,
                                    ast->u.call.args.items, ast->u.call.args.count);
                                if (get_ret) {
                                    type_free(obj_type);
                                    ast->cached_type = type_copy(get_ret);
                                    return get_ret;
                                }
                            }

                            type_free(obj_type);
                            return type_new(return_type);
                        }
                        type_free(obj_type);
                    }
                }
            }
            // 处理函数类型字段调用：self["op"](a, b)（callee 是 AST_INDEX，且推断类型为 TYPE_FUNCTION）
            if (ast->u.call.callee && ast->u.call.callee->kind == AST_INDEX) {
                TypeInfo* callee_type = infer_expr_type(s, ast->u.call.callee);
                if (callee_type && callee_type->kind == TYPE_FUNCTION && callee_type->return_type) {
                    TypeInfo* ret = type_copy(callee_type->return_type);
                    ast->cached_type = type_copy(ret);
                    type_free(callee_type);
                    return ret;
                }
                if (callee_type) type_free(callee_type);
            }
            // 处理实例方法调用：obj.method()（callee 是 AST_FIELD_ACCESS）
            if (ast->u.call.callee && ast->u.call.callee->kind == AST_FIELD_ACCESS) {
                Ast* field_access = ast->u.call.callee;
                const char* method_name = field_access->u.field_access.field_name;
                TypeInfo* obj_type = infer_expr_type(s, field_access->u.field_access.obj);
                if (obj_type) {
                    // 处理 struct/cstruct/face 类型的方法调用
                    if (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT || obj_type->kind == TYPE_FACE) {
                        // ★ pri：私有方法检查（表达式侧 `obj.method()` 的唯一必经点 ✓）
                        //   与字段侧同理：放在这里表已懒加载 ✓（visit_module 里查得太早会拿到 NULL ✗）
                        pri_check_method_access(s, ast, obj_type, method_name);
                        // 从函数表查找方法定义
                        char method_key[256];
                        if (obj_type->struct_name) {
                            snprintf(method_key, sizeof(method_key), "%s::%s", obj_type->struct_name, method_name);
                        } else {
                            strncpy(method_key, method_name, sizeof(method_key) - 1);
                            method_key[sizeof(method_key) - 1] = '\0';
                        }
                        Ast* func_def = func_table_find(&s->func_table, method_key);
                        if (func_def && func_def->kind == AST_FUNC_DEF) {
                            TypeInfo* ret = NULL;
                            if (func_def->u.func.return_type && func_def->u.func.return_type->kind != TYPE_INFER) {
                                ret = type_copy(func_def->u.func.return_type);
                            } else if (func_def->u.func.body) {
                                ret = infer_return_type_from_body(s, func_def->u.func.body);
                            }
                            if (!ret) ret = type_new(TYPE_ANY);
                            // 泛型参数替换
                            if (obj_type->generic_count > 0 && obj_type->generic_args && obj_type->struct_name) {
                                for (int mi = 0; mi < s->imported_module_count; mi++) {
                                    ImportedModuleInfo* info = &s->imported_modules[mi];
                                    if (info->sym_table) {
                                        ModuleStructSymbol* ssym = module_symbol_table_find_struct(info->sym_table, obj_type->struct_name);
                                        if (ssym && ssym->type_param_count > 0 && ssym->type_param_names) {
                                            TypeInfo* substituted = ret;
                                            for (int gi = 0; gi < ssym->type_param_count && gi < obj_type->generic_count; gi++) {
                                                TypeInfo* new_ret = type_substitute(substituted, ssym->type_param_names[gi], obj_type->generic_args[gi]);
                                                type_free(substituted);
                                                substituted = new_ret;
                                            }
                                            ret = substituted;
                                            break;
                                        }
                                    }
                                }
                            }
                            ast->cached_type = type_copy(ret);
                            type_free(obj_type);
                            return ret;
                        }
                        // 检查是否是原生方法
                        int arity;
                        const char* type_name = (obj_type->kind == TYPE_CSTRUCT) ? "cstruct" : "struct";
                        TypeKind native_ret = native_get_instance_method_return_type(type_name, method_name, &arity);
                        if (native_ret != TYPE_ANY) {
                            type_free(obj_type);
                            return type_new(native_ret);
                        }
                        // 检查是否是从模块导入的 struct 的方法
                        if (obj_type->struct_name) {
                            for (int i = 0; i < s->imported_module_count; i++) {
                                ImportedModuleInfo* info = &s->imported_modules[i];
                                if (info->sym_table) {
                                    ModuleStructMethod* method = module_symbol_table_find_struct_method(
                                        info->sym_table, obj_type->struct_name, method_name);
                                    if (method) {
                                        TypeInfo* result = NULL;
                                        // 如果返回类型是泛型参数，用 type_substitute 替换
                                        if (method->return_type == TYPE_GENERIC_PARAM && method->return_type_param_name &&
                                            obj_type->generic_count > 0 && obj_type->generic_args) {
                                            ModuleStructSymbol* ssym = module_symbol_table_find_struct(info->sym_table, obj_type->struct_name);
                                            if (ssym && ssym->type_param_names) {
                                                for (int gi = 0; gi < ssym->type_param_count && gi < obj_type->generic_count; gi++) {
                                                    if (strcmp(ssym->type_param_names[gi], method->return_type_param_name) == 0) {
                                                        result = type_copy(obj_type->generic_args[gi]);
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                        if (!result) {
                                            if (method->return_type_info) {
                                                result = type_copy(method->return_type_info);
                                            } else {
                                                result = type_new(method->return_type);
                                            }
                                            if (method->return_type == TYPE_STRUCT && method->return_struct_name) {
                                                result->struct_name = strdup(method->return_struct_name);
                                            }
                                        }
                                        type_free(obj_type);
                                        return result;
                                    }
                                }
                            }
                        }
                    }
                    // 处理 clib 类型的方法调用
                    else if (obj_type->kind == TYPE_CLIB && obj_type->struct_name) {
                        Symbol* clib_sym = scope_resolve(s->current, obj_type->struct_name);
                        if (clib_sym && clib_sym->clib_func_count > 0) {
                            for (int ci = 0; ci < clib_sym->clib_func_count; ci++) {
                                if (strcmp(clib_sym->clib_func_names[ci], method_name) == 0) {
                                    TypeInfo* ret_type = clib_sym->clib_func_return_types[ci];
                                    TypeInfo* result2 = NULL;
                                    if (ret_type) {
                                        TypeKind rk = ret_type->kind;
                                        switch (rk) {
                                            case TYPE_I8: case TYPE_U8:
                                            case TYPE_I16: case TYPE_U16:
                                            case TYPE_I32: case TYPE_U32:
                                            case TYPE_I64: case TYPE_U64:
                                                result2 = type_new(TYPE_INT); break;
                                            case TYPE_F32: case TYPE_F64:
                                                result2 = type_new(TYPE_FLOAT); break;
                                            case TYPE_STR8: case TYPE_STR16:
                                                result2 = type_new(TYPE_STRING); break;
                                            case TYPE_PTR:
                                                result2 = type_new(TYPE_PTR);
                                                result2->struct_name = strdup("Ptr"); break;
                                            case TYPE_PTR_GENERIC:
                                                // 保留元素类型信息
                                                if (ret_type->element_type) {
                                                    result2 = type_ptr_generic(type_copy(ret_type->element_type));
                                                } else {
                                                    result2 = type_new(TYPE_PTR);
                                                    result2->struct_name = strdup("Ptr");
                                                }
                                                break;
                                            case TYPE_BOOL:
                                                result2 = type_new(TYPE_BOOL); break;
                                            case TYPE_NULL:
                                                result2 = type_new(TYPE_NULL); break;
                                            default:
                                                result2 = type_copy(ret_type); break;
                                        }
                                    } else {
                                        result2 = type_new(TYPE_ANY);
                                    }
                                    type_free(obj_type);
                                    return result2;
                                }
                            }
                        }
                    }
                    type_free(obj_type);
                }
            }
            return type_new(TYPE_ANY);
        }
        
        case AST_MODULE_CALL: {
            const char* actual_module = native_resolve_module_alias(ast->u.module_call.module_name);

            // 检查是否是原生模块
            int is_native_module = native_is_module(actual_module);

            if (is_native_module) {
                const char* method_name = ast->u.module_call.method_name;

                // ⓪ 族（map/reduce/threads.start 的回调返回类型）已**声明化**（2026-09-28）：
                //   注册规格带 NTYPE_ARG_CB_RET 节点 ⇒ 下面的规格解析路径经
                //   spec_resolve_with_cb_ret（语义侧唯一通用推断点）解析，特判 if 链已删 ✓

                // ① 优先用**完整返回类型规格**：它才能表达 `Array[DirEntry]` / `Dict[string,string]`
                //    这类参数化、带名字的类型（Kind 槽表达不了，见 leno_types.h 的 NativeTypeSpec）。
                //    ⚠ 规格里可能有**关系型标签**（`NTYPE_ARG0_ELEM` / `NTYPE_ARG_CB_RET`）——
                //    例如 `arrays.copy(xs)` 的真实类型是 `Array[T]`（T = xs 的元素类型）、
                //    `arrays.map(xs, fn)` 是 `Array[回调返回]` ⇒ 这类规格必须拿实参来解析。
                //    只为"带实参引用"的规格去推实参：另外 230+ 个方法的规格与实参无关，
                //    不该为它们白推一遍。返回的 TypeInfo 归调用方释放 ✓
                const NativeTypeSpec* ret_spec =
                    native_get_module_method_return_spec(actual_module, method_name);
                if (ret_spec) {
                    TypeInfo* arg0_type = NULL;
                    if (native_type_spec_has_arg_ref(ret_spec) && ast->u.module_call.args.count > 0) {
                        arg0_type = infer_expr_type(s, ast->u.module_call.args.items[0]);
                    }
                    TypeInfo* spec_type = NULL;
                    if (spec_find_cb_ret(ret_spec)) {
                        // ⓪ 族：回调返回类型需要**推函数体** ⇒ 语义侧通用推断点 ✓
                        spec_type = spec_resolve_with_cb_ret(s, ret_spec, arg0_type,
                            ast->u.module_call.args.items, ast->u.module_call.args.count, 0);
                        if (spec_type) {
                            ast->cached_type = type_copy(spec_type);
                        }
                    } else {
                        spec_type = native_type_spec_to_info_with_args(ret_spec, arg0_type);
                    }
                    if (arg0_type) type_free(arg0_type);
                    if (spec_type) return spec_type;
                }

                TypeKind return_type = native_get_module_method_return_type(
                    actual_module,
                    method_name);
                
                // 如果返回类型是数组，从元信息获取元素类型
                if (return_type == TYPE_ARRAY) {
                    TypeKind elem_type = native_get_module_method_return_element_type(actual_module, method_name);
                    if (elem_type != TYPE_UNKNOWN) {
                        TypeInfo* arr_type = type_new(TYPE_ARRAY);
                        arr_type->element_type = type_new(elem_type);
                        return arr_type;
                    }
                }
                
                return type_new(return_type);
            }

            // 检查是否是导入的用户模块
            ImportedModuleInfo* module_info = find_imported_module(s, ast->u.module_call.module_name);
            if (module_info && module_info->file_path && module_info->sym_table) {
                // 模块加载失败时，错误已注册，返回 NULL 避免下游误报 any 错误
                if (module_info->load_failed) return NULL;
                const char* method_name = ast->u.module_call.method_name;

                ModuleFuncSymbol* func = module_symbol_table_find_func(module_info->sym_table, method_name);
                if (func) {
                    TypeInfo* type;
                    if (func->return_type_info) {
                        // 有完整类型信息（如 Dict[string,int]），直接使用
                        type = type_copy(func->return_type_info);
                    } else {
                        type = type_new(func->return_type);
                    }
                    if (func->return_struct_name) {
                        // 检查返回类型是否是模块中定义的 face
                        ModuleFaceSymbol* face_sym = module_symbol_table_find_face(module_info->sym_table, func->return_struct_name);
                        if (face_sym) {
                            type->kind = TYPE_FACE;
                            type->struct_name = strdup(face_sym->name);
                        } else if (func->return_type == TYPE_STRUCT) {
                            type->struct_name = strdup(func->return_struct_name);
                        }
                    }
                    return type;
                }

                // 检查是否是 struct 初始化（如 new module.Point()）
                ModuleStructSymbol* struct_sym = module_symbol_table_find_struct(module_info->sym_table, method_name);
                if (struct_sym) {
                    TypeInfo* type = type_new(TYPE_STRUCT);
                    type->struct_name = strdup(struct_sym->name);
                    return type;
                }

                // 如果没有找到，返回 TYPE_ANY
                return type_new(TYPE_ANY);
            }

            // 不是模块调用，可能是实例方法调用（如 arr1.copy()）
            // 查找变量
            Symbol* obj_sym = scope_resolve(s->current, ast->u.module_call.module_name);
            if (obj_sym && obj_sym->type) {
                // clib 类型实例方法调用（如 ttf.TTF_OpenFont(...)）
                // native_get_type_name 不支持 TYPE_CLIB，需专用路径处理
                if (obj_sym->type->kind == TYPE_CLIB && obj_sym->type->struct_name) {
                    Symbol* clib_sym = scope_resolve(s->current, obj_sym->type->struct_name);
                    if (clib_sym && clib_sym->clib_func_count > 0) {
                        const char* method_name = ast->u.module_call.method_name;
                        for (int ci = 0; ci < clib_sym->clib_func_count; ci++) {
                            if (strcmp(clib_sym->clib_func_names[ci], method_name) == 0) {
                                TypeInfo* ret_type = clib_sym->clib_func_return_types[ci];
                                TypeInfo* result2 = NULL;
                                if (ret_type) {
                                    TypeKind rk = ret_type->kind;
                                    switch (rk) {
                                        case TYPE_I8: case TYPE_U8:
                                        case TYPE_I16: case TYPE_U16:
                                        case TYPE_I32: case TYPE_U32:
                                        case TYPE_I64: case TYPE_U64:
                                            result2 = type_new(TYPE_INT); break;
                                        case TYPE_F32: case TYPE_F64:
                                            result2 = type_new(TYPE_FLOAT); break;
                                        case TYPE_STR8: case TYPE_STR16:
                                            result2 = type_new(TYPE_STRING); break;
                                        case TYPE_PTR:
                                            result2 = type_new(TYPE_PTR);
                                            result2->struct_name = strdup("Ptr"); break;
                                        case TYPE_PTR_GENERIC:
                                            // 保留元素类型信息
                                            if (ret_type->element_type) {
                                                result2 = type_ptr_generic(type_copy(ret_type->element_type));
                                            } else {
                                                result2 = type_new(TYPE_PTR);
                                                result2->struct_name = strdup("Ptr");
                                            }
                                            break;
                                        case TYPE_BOOL:
                                            result2 = type_new(TYPE_BOOL); break;
                                        case TYPE_NULL:
                                            result2 = type_new(TYPE_NULL); break;
                                        case TYPE_C_INT: case TYPE_C_UINT:
                                        case TYPE_C_LONG: case TYPE_C_ULONG:
                                        case TYPE_C_LONGLONG: case TYPE_C_ULONGLONG:
                                        case TYPE_C_SIZE: case TYPE_C_SSIZE:
                                            result2 = type_new(TYPE_INT); break;
                                        default:
                                            result2 = type_copy(ret_type); break;
                                    }
                                } else {
                                    result2 = type_new(TYPE_ANY);
                                }
                                ast->cached_type = type_copy(result2);
                                return result2;
                            }
                        }
                    }
                    return type_new(TYPE_ANY);
                }

                const char* type_name = native_get_type_name(obj_sym->type->kind);
                if (type_name) {
                    const char* method_name = ast->u.module_call.method_name;

                    // ① 规格优先（与 AST_CALL 实例路径**同一实现**）：关系型标签 + CB_RET
                    //    都由规格声明；接收者 = obj_sym->type（下标 0）。毯式「ARRAY/STRUCT
                    //    ⇒ 照抄接收者」老规则已删——meta 表里返回 TYPE_ARRAY 的 Array 方法
                    //    （copy/clear/reverse/sort/map/filter）全部已声明规格 ✓
                    {
                        const NativeTypeSpec* inst_spec =
                            native_get_instance_method_return_spec(type_name, method_name);
                        if (inst_spec) {
                            TypeInfo* spec_ret = NULL;
                            if (spec_find_cb_ret(inst_spec)) {
                                spec_ret = spec_resolve_with_cb_ret(s, inst_spec, obj_sym->type,
                                    ast->u.module_call.args.items, ast->u.module_call.args.count, 1);
                            } else {
                                spec_ret = native_type_spec_to_info_with_args(inst_spec, obj_sym->type);
                            }
                            if (spec_ret) {
                                ast->cached_type = type_copy(spec_ret);
                                return spec_ret;
                            }
                            // 推不出 ⇒ 落到下面的 Kind 槽兜底（宁漏勿误报）
                        }
                    }

                    int arity;
                    TypeKind return_type = native_get_instance_method_return_type(type_name, method_name, &arity);

                    // Dict.get 返回类型：2 参 ⇒ 默认值类型、1 参 ⇒ 值类型 V
                    //（与 AST_CALL 路径共享 infer_dict_get_return；此前这里只有 2 参拷贝、
                    //  缺 1 参形态 ⇒ `string s = d.get("k")` 在此路径推不出 ✗ 已统一 ✓）
                    // 方法名判断已收敛进 infer_dict_get_return 内部（唯一判定点 ✓，2026-09-28）
                    {
                        TypeInfo* get_ret = infer_dict_get_return(s, obj_sym->type, method_name,
                            ast->u.module_call.args.items, ast->u.module_call.args.count);
                        if (get_ret) {
                            ast->cached_type = type_copy(get_ret);
                            return get_ret;
                        }
                    }

                    return type_new(return_type);
                }
            }

            return type_new(TYPE_ANY);
        }
        case AST_MODULE_ACCESS: {
            // 模块成员访问：尝试查找导入的模块
            ImportedModuleInfo* module_info = find_imported_module(s, ast->u.module_access.module_name);
            if (module_info) {
                // 模块加载失败时，错误已注册，返回 NULL 避免下游误报 any 错误
                if (module_info->load_failed) return NULL;
                // 检查是否是 enum 成员访问（如 math_enum.Color.RED）
                // module_name 是模块别名，member_name 是 enum 名或函数名
                if (module_info->sym_table) {
                    // 检查是否是 enum 名
                    ModuleEnumSymbol* enum_sym = module_symbol_table_find_enum(module_info->sym_table, ast->u.module_access.member_name);
                    if (enum_sym) {
                        // 这是 enum 名，返回 enum 类型
                        // 实际的成员访问会通过 AST_INDEX 处理
                        TypeInfo* enum_type = type_new(TYPE_ENUM);
                        enum_type->struct_name = strdup(enum_sym->name);
                        ast->cached_type = enum_type;
                        return type_copy(ast->cached_type);
                    }

                    // 检查是否是变量名
                    ModuleVarSymbol* var_sym = module_symbol_table_find_var(module_info->sym_table, ast->u.module_access.member_name);
                    if (var_sym) {
                        // ★ 优先使用完整类型信息（支持 Array[int] 等泛型变量）
                        if (var_sym->type_info) {
                            ast->cached_type = type_copy(var_sym->type_info);
                            return type_copy(ast->cached_type);
                        }
                        // 向后兼容：从扁平字段重建类型
                        TypeInfo* var_type = type_new(var_sym->type);
                        if (var_sym->type == TYPE_STRUCT && var_sym->struct_name) {
                            var_type->struct_name = strdup(var_sym->struct_name);
                        } else if (var_sym->type == TYPE_CSTRUCT && var_sym->struct_name) {
                            var_type->struct_name = strdup(var_sym->struct_name);
                        }
                        ast->cached_type = var_type;
                        return type_copy(ast->cached_type);
                    }

                    // 检查是否是 struct 名（如 cs.Point）
                    ModuleStructSymbol* struct_sym = module_symbol_table_find_struct(module_info->sym_table, ast->u.module_access.member_name);
                    if (struct_sym) {
                        TypeInfo* st_type = type_new(struct_sym->is_cstruct ? TYPE_CSTRUCT : TYPE_STRUCT);
                        st_type->struct_name = strdup(ast->u.module_access.member_name);
                        ast->cached_type = st_type;
                        return type_copy(ast->cached_type);
                    }

                    // 检查是否是 face 名（如 cs.Shape）
                    ModuleFaceSymbol* face_sym = module_symbol_table_find_face(module_info->sym_table, ast->u.module_access.member_name);
                    if (face_sym) {
                        TypeInfo* face_type = type_new(TYPE_FACE);
                        face_type->struct_name = strdup(ast->u.module_access.member_name);
                        ast->cached_type = face_type;
                        return type_copy(ast->cached_type);
                    }
                }
                // 用户模块的其他成员，无法静态推导类型，返回 ANY
                return type_new(TYPE_ANY);
            }
            // 原生模块的成员访问
            const char* actual_module = native_resolve_module_alias(ast->u.module_access.module_name);
            if (native_is_module(actual_module)) {
                // 原生模块成员（如 maths.PI），返回 ANY（无法静态确定）
                return type_new(TYPE_ANY);
            }
            // 检查是否是当前文件中定义的 enum 访问（如 Color.green）
            Symbol* enum_type_sym = scope_resolve(s->current, ast->u.module_access.module_name);
            if (enum_type_sym && enum_type_sym->kind == SYM_ENUM) {
                // 这是当前文件中定义的 enum 类型，enum 成员值是 int 类型
                // 检查 member_name 是否是该 enum 的有效成员
                TypeInfo* enum_type = type_new(TYPE_ENUM);
                enum_type->struct_name = strdup(ast->u.module_access.module_name);
                ast->cached_type = enum_type;
                return type_copy(ast->cached_type);
            }
            // 可能是变量属性访问
            Symbol* var_sym = scope_resolve(s->current, ast->u.module_access.module_name);
            if (var_sym && var_sym->type) {
                // 检查 any 类型：需要类型守卫收窄后才能访问字段
                if (var_sym->type->kind == TYPE_ANY) {
                    // 尝试查找类型守卫（如 if v is Dict { v.field }）
                    const char* var_name = ast->u.module_access.module_name;
                    const char* member_name = ast->u.module_access.member_name;
                    char guard_name[256];
                    snprintf(guard_name, sizeof(guard_name), "%s.%s", var_name, member_name);
                    Symbol* guard_sym = scope_resolve(s->current, guard_name);
                    if (guard_sym && guard_sym->type) {
                        ast->cached_type = type_copy(guard_sym->type);
                        return type_copy(ast->cached_type);
                    }
                    // 无类型守卫，报错（但如果已有模块加载失败，抑制级联 any 错误）
                    if (!s->has_module_load_failure) {
                        char msg[512];
                        // ★ 2026-09-18（P1）：原提示只说"用 if x is Type 收窄"，但**收窄到裸容器**
                        //   （如 Dict）取元素仍是 any ⇒ 照提示改完还是失败（实测 5~6 轮编译失败）。
                        //   补两句关键：① 容器要**带泛型参数**；② 语言既有 `x as T` **安全转换**
                        //   （不匹配返回 null、静态类型即目标类型）—— 这才是最省事的那条路 ✓。
                        snprintf(msg, sizeof(msg),
                            "不能在 any 类型 '%s' 上直接访问字段 '%s'\n"
                            "  提示: 用 if %s is Type { %s.%s } 收窄，"
                            "或用 x as T 安全转换（如 raw as Dict[string, any]，不匹配给 null）\n"
                            "  注意: 收窄到**裸**容器（如 Dict）取出的元素仍是 any；"
                            "要按类型取元素请收窄到带泛型参数的容器（如 Dict[string, any]、Array[string]），"
                            "见 docs/类型收窄与绑定语法改进.md",
                            var_name, member_name, var_name, var_name, member_name);
                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                    }
                    ast->cached_type = type_new(TYPE_ANY);
                    return type_copy(ast->cached_type);
                }
                if (var_sym->type->kind == TYPE_DICT && var_sym->type->value_type) {
                    return type_copy(var_sym->type->value_type);
                }
            }
            return type_new(TYPE_ANY);
        }
        case AST_INDEX: {
            // ★★ 字面量键 + struct 接收者 ⇒ 编译期就能查字段表（2026-09-30 补 ✓✓）
            //   为什么必须放这里：方法体里的**裸字段名**成员访问（`b.no_such_field` ✓
            //   —— file_manager:346 `btnBack._loaded` 的形态 ✓）实测**走的就是这条索引路**
            //   ⇒ 上面 AST_FIELD_ACCESS 那段永远看不到它 ✗ ⇒ 编译期一句不报 ✗
            //     ⇒ 一路漏到**运行期**才炸 ✗（"struct 不存在字段 '_loaded'" + 调用栈 ✗）
            //   判定保守（宁少报、不误报 ✓）：
            //     · 键是**字符串字面量** ✓（动态键一律不管 ✓）
            //     · 接收者静态类型是 TYPE_STRUCT ✓
            //     · 能在作用域里**确知字段表** ✓
            //     · 名字既不是字段、也不是同名方法 ⇒ 才报 ✓
            if (ast->u.index.obj && ast->u.index.index &&
                ast->u.index.index->kind == AST_STRING) {
                const char* key = ast->u.index.index->u.string.value;
                if (key) {
                    TypeInfo* recv = infer_expr_type(s, ast->u.index.obj);
                    if (recv && recv->kind == TYPE_STRUCT && recv->struct_name) {
                        Symbol* def_sym = scope_resolve(s->current, recv->struct_name);
                        if (def_sym && def_sym->struct_field_count > 0) {
                            int known = 0;
                            for (int fi = 0; fi < def_sym->struct_field_count; fi++) {
                                if (def_sym->struct_field_names[fi] &&
                                    strcmp(def_sym->struct_field_names[fi], key) == 0) {
                                    known = 1;
                                    break;
                                }
                            }
                            if (!known) {
                                Ast* sd2 = (Ast*)def_sym->type_decl_ast;
                                if (sd2 && sd2->kind == AST_STRUCT_DEF) {
                                    for (int mi = 0; mi < sd2->u.struct_def.method_count; mi++) {
                                        Ast* m = sd2->u.struct_def.methods[mi];
                                        if (m && m->kind == AST_FUNC_DEF && m->u.func.name &&
                                            strcmp(m->u.func.name, key) == 0) {
                                            known = 1;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (!known) {
                                char msg[256];
                                snprintf(msg, sizeof(msg), "struct '%s' 没有字段 '%s'",
                                         recv->struct_name, key);
                                // 同样先去重 ✓（与上面 AST_FIELD_ACCESS 那条同一理由 ✓）
                                if (!error_has_at(ast->line, ast->column, msg)) {
                                    error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                                }
                            }

                            // ★ 顺带把 **pri 私有** 也在索引路上拦住（2026-09-30 ✓）
                            //   同一条根因：裸字段名的成员访问走索引路 ✗ ⇒ 访问器那条的 pri
                            //   检查也看不到它 ✗ ⇒ 私有成员能被裸名读走 ⇒ 只剩运行期才炸 ✗
                            //   实测（assert/test_pri_cross_module.leno 的 bare_pri_field 一例）：
                            //   裸名读 pri 字段**编译通过并成功打印出私密值** ✗
                            //   两个来源都查：同文件看 struct AST 的 field_private[] ✓、
                            //   跨模块看导入模块符号表（v32 起带 is_private ✓）；都不知道就沉默 ✓
                            int priv = -1;   // -1 = 拿不到信息 ✓（一律不动 ✓）
                            Ast* sd3 = (Ast*)def_sym->type_decl_ast;
                            if (sd3 && sd3->kind == AST_STRUCT_DEF &&
                                sd3->u.struct_def.field_private) {
                                for (int fi = 0; fi < sd3->u.struct_def.field_count; fi++) {
                                    if (sd3->u.struct_def.field_names[fi] &&
                                        strcmp(sd3->u.struct_def.field_names[fi], key) == 0) {
                                        priv = sd3->u.struct_def.field_private[fi] ? 1 : 0;
                                        break;
                                    }
                                }
                            }
                            if (priv < 0) {
                                for (int mi = 0; mi < s->imported_module_count && priv < 0; mi++) {
                                    ImportedModuleInfo* mod = &s->imported_modules[mi];
                                    if (!mod->sym_table) continue;
                                    ModuleStructSymbol* ssym =
                                        module_symbol_table_find_struct(mod->sym_table, recv->struct_name);
                                    if (!ssym || ssym->is_cstruct) continue;
                                    for (int fi = 0; fi < ssym->field_count; fi++) {
                                        if (ssym->fields[fi].name &&
                                            strcmp(ssym->fields[fi].name, key) == 0) {
                                            priv = ssym->fields[fi].is_private ? 1 : 0;
                                            break;
                                        }
                                    }
                                }
                            }
                            // 自己方法内部访问自己的私有成员是合法的 ✓（与访问器那条同一判定 ✓）
                            if (priv == 1) {
                                int own = 0;
                                Symbol* self_sym2 = scope_resolve(s->current, "self");
                                if (self_sym2 && self_sym2->type && self_sym2->type->struct_name &&
                                    strcmp(self_sym2->type->struct_name, recv->struct_name) == 0) {
                                    own = 1;
                                }
                                if (!own) {
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                        "'%s.%s' 是 pri 私有字段 —— 只有 %s 自己的方法内部可以访问它；"
                                        "要对外开放就把 'pri' 去掉（默认全公有）",
                                        recv->struct_name, key, recv->struct_name);
                                    error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                                }
                            }
                        }
                    }
                    if (recv) type_free(recv);
                }
            }

            // 如果有缓存的类型且不是 any，直接返回（any 可能被守卫收窄，需要重新检查）
            if (ast->cached_type && ast->cached_type->kind != TYPE_ANY) {
                return type_copy(ast->cached_type);
            }
            // 检查是否有索引类型守卫收窄（如 if arr[0] is int）
            // 支持任意深度连续索引：a[0]["key"] → "a[0][key]"，config["items"][0]["url"] → "config[items][0][url]"
            // 递归收集所有索引键，构造完整守卫符号名
            if (ast->u.index.obj && ast->u.index.index) {
                // 递归收集变量名和索引键链
                char* keys[16];   // 最多支持 16 层嵌套
                int key_count = 0;
                const char* root_var_name = NULL;
                Ast* cur = ast;
                while (cur && cur->kind == AST_INDEX && key_count < 16) {
                    Ast* idx = cur->u.index.index;
                    char* key = NULL;
                    if (idx) {
                        if (idx->kind == AST_NUM) {
                            if (idx->u.num.is_bigint && idx->u.num.bigint_str) {
                                key = strdup(idx->u.num.bigint_str);
                            } else if (idx->u.num.is_float) {
                                char buf[32];
                                snprintf(buf, sizeof(buf), "%g", idx->u.num.value);
                                key = strdup(buf);
                            } else {
                                char buf[32];
                                snprintf(buf, sizeof(buf), "%d", (int)idx->u.num.value);
                                key = strdup(buf);
                            }
                        } else if (idx->kind == AST_STRING && idx->u.string.value) {
                            key = strdup(idx->u.string.value);
                        } else if (idx->kind == AST_VAR && idx->u.var.name) {
                            key = strdup(idx->u.var.name);
                        }
                    }
                    if (key) {
                        keys[key_count++] = key;
                    } else {
                        break;  // 无法识别的索引类型，停止
                    }
                    cur = cur->u.index.obj;
                    if (cur && cur->kind == AST_VAR) {
                        root_var_name = cur->u.var.name;
                        break;
                    }
                }
                // 如果找到了根变量名和至少一个索引键，构造守卫符号名查找
                if (root_var_name && key_count > 0) {
                    // keys[0] 是最外层，keys[key_count-1] 是最内层
                    // 守卫符号格式: "var[innermost][...][outermost]"
                    // 构造方式：var + [ + innermost + ] + [ + ... + [ + outermost + ]
                    int total_len = strlen(root_var_name) + 1;  // "var" + null
                    for (int i = 0; i < key_count; i++) {
                        total_len += strlen(keys[i]) + 2;  // "[key]"
                    }
                    char* guard_name = (char*)malloc(total_len + 1);
                    guard_name[0] = '\0';
                    strcat(guard_name, root_var_name);
                    // 从最内层(key_count-1)到最外层(0)拼接 "[key]"
                    for (int i = key_count - 1; i >= 0; i--) {
                        strcat(guard_name, "[");
                        strcat(guard_name, keys[i]);
                        strcat(guard_name, "]");
                    }
                    Symbol* guard_sym = scope_resolve(s->current, guard_name);
                    free(guard_name);
                    for (int i = 0; i < key_count; i++) free(keys[i]);
                    if (guard_sym && guard_sym->type) {
                        if (ast->cached_type) type_free(ast->cached_type);
                        ast->cached_type = type_copy(guard_sym->type);
                        return type_copy(ast->cached_type);
                    }
                } else {
                    for (int i = 0; i < key_count; i++) free(keys[i]);
                }
            }

            // 简单索引守卫查找：arr[0] 或 d["name"]（上面的递归已覆盖，但保留作为回退）
            if (ast->u.index.obj && ast->u.index.obj->kind == AST_VAR &&
                ast->u.index.index) {
                const char* var_name = ast->u.index.obj->u.var.name;
                Ast* idx = ast->u.index.index;
                char* idx_key_str = NULL;  // 动态分配的索引键字符串
                if (idx->kind == AST_NUM) {
                    if (idx->u.num.is_bigint && idx->u.num.bigint_str) {
                        idx_key_str = strdup(idx->u.num.bigint_str);
                    } else if (idx->u.num.is_float) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%g", idx->u.num.value);
                        idx_key_str = strdup(buf);
                    } else {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%d", (int)idx->u.num.value);
                        idx_key_str = strdup(buf);
                    }
                } else if (idx->kind == AST_STRING && idx->u.string.value) {
                    idx_key_str = strdup(idx->u.string.value);
                } else if (idx->kind == AST_VAR && idx->u.var.name) {
                    idx_key_str = strdup(idx->u.var.name);
                }
                if (idx_key_str) {
                    int guard_name_len = strlen(var_name) + strlen(idx_key_str) + 4;
                    char* guard_name = (char*)malloc(guard_name_len);
                    snprintf(guard_name, guard_name_len, "%s[%s]", var_name, idx_key_str);
                    Symbol* guard_sym = scope_resolve(s->current, guard_name);
                    free(guard_name);
                    free(idx_key_str);
                    if (guard_sym && guard_sym->type) {
                        if (ast->cached_type) type_free(ast->cached_type);
                        ast->cached_type = type_copy(guard_sym->type);
                        return type_copy(ast->cached_type);
                    }
                }
            }

            // 检查是否有字段类型守卫收窄（如 if s.age is int）
            // dict 的 s.age 语法被解析为 AST_INDEX（obj=AST_VAR("s"), index=AST_STRING("age")）
            if (ast->u.index.obj && ast->u.index.obj->kind == AST_VAR &&
                ast->u.index.index && ast->u.index.index->kind == AST_STRING) {
                const char* var_name = ast->u.index.obj->u.var.name;
                const char* field_name = ast->u.index.index->u.string.value;
                int guard_name_len = strlen(var_name) + strlen(field_name) + 2;
                char* guard_name = (char*)malloc(guard_name_len);
                snprintf(guard_name, guard_name_len, "%s.%s", var_name, field_name);
                Symbol* guard_sym = scope_resolve(s->current, guard_name);
                free(guard_name);
                if (guard_sym && guard_sym->type) {
                    if (ast->cached_type) type_free(ast->cached_type);
                    ast->cached_type = type_copy(guard_sym->type);
                    return type_copy(ast->cached_type);
                }
            }

            TypeInfo* obj_type = infer_expr_type(s, ast->u.index.obj);
            if (obj_type) {
                // 检查 any 类型：需要类型守卫收窄后才能访问字段/索引
                if (obj_type->kind == TYPE_ANY) {
                    const char* field_name = NULL;
                    if (ast->u.index.index && ast->u.index.index->kind == AST_STRING) {
                        field_name = ast->u.index.index->u.string.value;
                    }
                    const char* var_name = NULL;
                    if (ast->u.index.obj && ast->u.index.obj->kind == AST_VAR) {
                        var_name = ast->u.index.obj->u.var.name;
                    }
                    // 如果已有模块加载失败，抑制级联 any 错误
                    if (!s->has_module_load_failure) {
                        char msg[768];
                        // ★ 2026-09-18（P1）：同上一处 —— 补"裸容器收窄后取元素仍是 any"这句，
                        //   否则用户照提示改完仍然失败（这是本次 TraeSign 移植最费时的一处 ✗）。
                        const char* caveat =
                            "\n  提示: 也可用 x as T **安全转换**（不匹配给 null、静态类型即目标类型），"
                            "如 raw as Dict[string, any]\n"
                            "  注意: 收窄到**裸**容器（如 Dict/Array）取出的元素仍是 any；"
                            "要按类型取元素请收窄到带泛型参数的容器（如 Dict[string, any]、Array[string]），"
                            "见 docs/类型收窄与绑定语法改进.md";
                        if (var_name && field_name) {
                            snprintf(msg, sizeof(msg),
                                "不能在 any 类型 '%s' 上直接访问字段 '%s'\n"
                                "  提示: 使用 if %s is Type { %s.%s } 进行类型收窄%s",
                                var_name, field_name, var_name, var_name, field_name, caveat);
                        } else if (field_name) {
                            snprintf(msg, sizeof(msg),
                                "不能在 any 类型上直接访问字段 '%s'\n"
                                "  提示: 使用 if x is Type { x.%s } 进行类型收窄%s",
                                field_name, field_name, caveat);
                        } else {
                            snprintf(msg, sizeof(msg),
                                "不能在 any 类型上进行索引访问\n"
                                "  提示: 使用 if x is Array { x[i] } 进行类型收窄%s",
                                caveat);
                        }
                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                    }
                    type_free(obj_type);
                    ast->cached_type = type_new(TYPE_ANY);
                    return type_copy(ast->cached_type);
                }
                // 检查：基础类型不支持属性访问（如 float.w）
                if (ast->u.index.index && ast->u.index.index->kind == AST_STRING) {
                    TypeKind ok = obj_type->kind;
                    if (ok != TYPE_ARRAY && ok != TYPE_DICT && ok != TYPE_STRING &&
                        ok != TYPE_STRUCT && ok != TYPE_CSTRUCT && ok != TYPE_FACE &&
                        ok != TYPE_ENUM && ok != TYPE_CLIB && ok != TYPE_PTR_GENERIC &&
                        ok != TYPE_ANY && ok != TYPE_INFER && ok != TYPE_UNKNOWN &&
                        ok != TYPE_GENERIC_PARAM && ok != TYPE_MULTI_RET) {
                        const char* field_name = ast->u.index.index->u.string.value;
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                            "类型 '%s' 不支持属性访问 '.%s'（只有 struct、dict、array 等类型才能访问属性）",
                            type_kind_to_string(ok), field_name);
                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                    }
                }
                if (obj_type->kind == TYPE_ARRAY) {
                    // 返回数组的元素类型
                    if (obj_type->element_type) {
                        fix_struct_to_face(obj_type->element_type);
                        ast->cached_type = type_copy(obj_type->element_type);
                        type_free(obj_type);
                        return type_copy(ast->cached_type);
                    } else {
                        // 数组没有指定元素类型，返回 any
                        type_free(obj_type);
                        ast->cached_type = type_new(TYPE_ANY);
                        return type_copy(ast->cached_type);
                    }
                } else if (obj_type->kind == TYPE_DICT && obj_type->value_type) {
                    ast->cached_type = type_copy(obj_type->value_type);
                    type_free(obj_type);
                    return type_copy(ast->cached_type);
                } else if (obj_type->kind == TYPE_PTR_GENERIC && obj_type->element_type) {
                    // 泛型指针 Ptr[T] 的索引访问返回元素类型 T
                    // 例如 Cv.malloc_array(3) 返回 Ptr[Cv]，v[0] 返回 Cv
                    fix_struct_to_face(obj_type->element_type);
                    ast->cached_type = type_copy(obj_type->element_type);
                    type_free(obj_type);
                    return type_copy(ast->cached_type);
                } else if (obj_type->kind == TYPE_STRING) {
                    type_free(obj_type);
                    ast->cached_type = type_new(TYPE_STRING);
                    return type_copy(ast->cached_type);
                } else if ((obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT) && obj_type->struct_name) {
                    // 处理 struct/cstruct 字段访问：走**统一**的字段类型推断（infer_field_type）
                    // ⚠ 2026-09-18 修：此前这里只用 `scope_resolve(struct_name)` **一级**查找
                    //   ⇒ 消费方没有 use 该 struct/cstruct 类型名时（如 `h.v.x`，h.v 是别的模块
                    //   的 cstruct）取不到字段、**静默返回 any**；而同一件事在 AST_FIELD_ACCESS
                    //   那条路（infer_field_type）是**三级**查找：作用域 → 全局定义表 →
                    //   导入模块的符号表。两处深度不一致 ⇒ 同一个表达式仅因写法不同得到不同结论
                    //   （实测：`h.v.x` 在没 use `Vec` 时是 any、use 了才是 int）。
                    //   顺带对齐 cstruct 字段的 C 布局类型映射（i32→int，见 c_layout_type_to_leno）。
                    if (ast->u.index.index && ast->u.index.index->kind == AST_STRING) {
                        const char* field_name = ast->u.index.index->u.string.value;
                        TypeInfo* field_type = infer_field_type(s, obj_type, field_name, NULL);
                        if (field_type) {
                            ast->cached_type = field_type;   // 所有权转移（调用方负责 type_free）
                            fix_struct_to_face(ast->cached_type);
                            type_free(obj_type);
                            return type_copy(ast->cached_type);
                        }
                    }

                    // cstruct 数组索引：cstruct 指针可通过整数索引访问数组元素
                    // 例如 Cv.malloc_array(3) 返回的指针可通过 v[0], v[1] 等访问
                    if (obj_type->kind == TYPE_CSTRUCT && ast->u.index.index &&
                        ast->u.index.index->kind != AST_STRING) {
                        ast->cached_type = type_copy(obj_type);
                        type_free(obj_type);
                        return type_copy(ast->cached_type);
                    }

                    type_free(obj_type);
                    ast->cached_type = type_new(TYPE_ANY);
                    return type_copy(ast->cached_type);
                } else if (obj_type->kind == TYPE_ENUM) {
                    // enum 成员访问返回 int 或 bigint（根据值的大小）
                    // 这里我们统一返回 int，因为大多数 enum 值都在 int 范围内
                    // 如果值超过 int 范围，运行时会自动处理为 bigint
                    type_free(obj_type);
                    ast->cached_type = type_new(TYPE_INT);
                    return type_copy(ast->cached_type);
                } else if (obj_type->kind == TYPE_CLIB && ast->u.index.index && ast->u.index.index->kind == AST_STRING) {
                    // clib 方法调用：obj.method()
                    const char* func_name = ast->u.index.index->u.string.value;
                    if (obj_type->struct_name) {
                        Symbol* clib_sym = scope_resolve(s->current, obj_type->struct_name);
                        if (clib_sym && clib_sym->clib_func_count > 0) {
                            for (int i = 0; i < clib_sym->clib_func_count; i++) {
                                if (strcmp(clib_sym->clib_func_names[i], func_name) == 0) {
                                    TypeInfo* ret_type = clib_sym->clib_func_return_types[i];
                                    TypeKind rk = ret_type ? ret_type->kind : TYPE_NULL;
                                    switch (rk) {
                                        case TYPE_I8: case TYPE_U8:
                                        case TYPE_I16: case TYPE_U16:
                                        case TYPE_I32: case TYPE_U32:
                                        case TYPE_I64: case TYPE_U64:
                                            result = type_new(TYPE_INT); break;
                                        case TYPE_F32: case TYPE_F64:
                                            result = type_new(TYPE_FLOAT); break;
                                        case TYPE_STR8: case TYPE_STR16:
                                            result = type_new(TYPE_STRING); break;
                                        case TYPE_PTR:
                                            result = type_new(TYPE_PTR);
                                            result->struct_name = strdup("Ptr"); break;
                                        case TYPE_PTR_GENERIC:
                                            // 保留元素类型信息
                                            if (ret_type && ret_type->element_type) {
                                                result = type_ptr_generic(type_copy(ret_type->element_type));
                                            } else {
                                                result = type_new(TYPE_PTR);
                                                result->struct_name = strdup("Ptr");
                                            }
                                            break;
                                        case TYPE_BOOL:
                                            result = type_new(TYPE_BOOL); break;
                                        case TYPE_NULL:
                                            result = type_new(TYPE_NULL); break;
                                        default:
                                            result = ret_type ? type_copy(ret_type) : type_new(TYPE_ANY); break;
                                    }
                                    type_free(obj_type);
                                    return result;
                                }
                            }
                        }
                    }
                    type_free(obj_type);
                    ast->cached_type = type_new(TYPE_ANY);
                    return type_copy(ast->cached_type);
                } else if (obj_type->kind == TYPE_GENERIC_PARAM && obj_type->constraint_name
                           && ast->u.index.index && ast->u.index.index->kind == AST_STRING) {
                    // 泛型约束类型参数的方法引用（如 T: Comparable，T.compare 作为值）
                    // 返回绑定方法类型（不含 self，因为 self 已绑定到 receiver）
                    const char* method_name = ast->u.index.index->u.string.value;
                    ObjFaceDef* constraint_face = face_def_find(obj_type->constraint_name);
                    if (constraint_face) {
                        for (int mi = 0; mi < constraint_face->method_count; mi++) {
                            if (strcmp(constraint_face->methods[mi].name, method_name) == 0) {
                                // 绑定方法类型：func(param1, ...) -> returnType（不含 self）
                                TypeInfo* func_type = type_new(TYPE_FUNCTION);
                                func_type->param_count = constraint_face->methods[mi].param_count;
                                func_type->param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * (func_type->param_count > 0 ? func_type->param_count : 1));
                                for (int pi = 0; pi < constraint_face->methods[mi].param_count; pi++) {
                                    if (constraint_face->methods[mi].param_types && constraint_face->methods[mi].param_types[pi]) {
                                        func_type->param_types[pi] = type_copy(constraint_face->methods[mi].param_types[pi]);
                                    } else {
                                        // 参数类型未指定时，使用约束名称作为 struct 类型
                                        // （face 名称在函数签名中被解析为 TYPE_STRUCT）
                                        func_type->param_types[pi] = type_new(TYPE_STRUCT);
                                        func_type->param_types[pi]->struct_name = strdup(obj_type->constraint_name);
                                    }
                                }
                                func_type->return_type = constraint_face->methods[mi].return_type
                                    ? type_copy(constraint_face->methods[mi].return_type)
                                    : type_new(TYPE_ANY);
                                ast->cached_type = func_type;
                                type_free(obj_type);
                                return type_copy(ast->cached_type);
                            }
                        }
                    }
                    type_free(obj_type);
                    ast->cached_type = type_new(TYPE_ANY);
                    return type_copy(ast->cached_type);
                }
                type_free(obj_type);
            }
            ast->cached_type = type_new(TYPE_ANY);
            return type_copy(ast->cached_type);
        }
        case AST_SLICE: {
            // 切片操作：数组切片返回数组类型，字符串切片返回字符串类型
            TypeInfo* obj_type = infer_expr_type(s, ast->u.slice.obj);
            TypeInfo* result = NULL;
            if (obj_type && obj_type->kind == TYPE_ARRAY) {
                // 返回相同类型的数组
                result = type_copy(obj_type);
            } else if (obj_type && obj_type->kind == TYPE_STRING) {
                // 字符串切片返回字符串类型
                result = type_new(TYPE_STRING);
            } else {
                result = type_new(TYPE_ANY);
            }
            if (obj_type) type_free(obj_type);
            ast->cached_type = type_copy(result);
            return result;
        }
        case AST_COMPOUND_ASSIGN: {
            // 使用resolve_variable_with_upvalue处理复合赋值，支持闭包
            SymRef ref;
            memset(&ref, 0, sizeof(ref));
            Symbol* sym = resolve_variable_with_upvalue(s, ast->u.compound_assign.name, &ref);
            // ★ T11：**确定是 null** 的变量做复合赋值（`int? a = null; a += 1`）⇒ 同一口径的编译错误
            if (sym && sym->is_null_value) {
                report_known_null_name(s, ast->u.compound_assign.name, ast->line, ast->column);
            }
            if (sym && sym->type) {
                result = type_new(sym->type->kind);
            } else {
                result = infer_expr_type(s, ast->u.compound_assign.value);
            }
            break;
        }
        case AST_ASSIGN: {
            // 赋值表达式返回被赋值的类型
            // 对于连续赋值如 x = y = 0，需要推断赋值目标的类型
            if (ast->u.assign.name_count > 0 && ast->u.assign.names[0]) {
                Symbol* sym = scope_resolve(s->current, ast->u.assign.names[0]);
                if (sym && sym->type) {
                    result = type_copy(sym->type);
                } else {
                    result = infer_expr_type(s, ast->u.assign.value);
                }
            } else {
                // 如果无法确定目标类型，返回右侧表达式的类型
                result = infer_expr_type(s, ast->u.assign.value);
            }
            break;
        }
        case AST_INDEX_ASSIGN: {
            // 索引赋值表达式返回被赋值的类型
            // 例如 self["y"] = 0，返回字段 y 的类型
            TypeInfo* obj_type = infer_expr_type(s, ast->u.index_assign.obj);
            if (obj_type && obj_type->kind == TYPE_STRUCT && obj_type->struct_name) {
                // 获取字段名
                if (ast->u.index_assign.index->kind == AST_STRING) {
                    const char* field_name = ast->u.index_assign.index->u.string.value;
                    // 查找 struct 定义
                    Symbol* struct_sym = scope_resolve(s->current, obj_type->struct_name);
                    if (struct_sym && struct_sym->struct_field_count > 0) {
                        for (int i = 0; i < struct_sym->struct_field_count; i++) {
                            if (strcmp(struct_sym->struct_field_names[i], field_name) == 0) {
                                result = type_copy(struct_sym->struct_field_types[i]);
                                fix_struct_to_face(result);
                                type_free(obj_type);
                                break;
                            }
                        }
                    }
                }
            }
            if (!result) {
                if (obj_type) type_free(obj_type);
                // 如果无法确定字段类型，返回右侧表达式的类型
                result = infer_expr_type(s, ast->u.index_assign.value);
            }
            break;
        }
        case AST_IF: {
            // if 表达式类型推断：根据 then 和 else 分支推断
            TypeInfo* then_type = infer_expr_type(s, ast->u.if_.then);
            TypeInfo* else_type = infer_expr_type(s, ast->u.if_.else_);

            if (then_type && else_type) {
                if (type_equals(then_type, else_type)) {
                    // 两个分支类型相同，直接返回该类型
                    result = type_copy(then_type);
                } else {
                    // 类型不同，尝试类型提升（复用数组推断中的逻辑）
                    int promoted = 0;

                    // int + float -> float
                    if ((then_type->kind == TYPE_INT && else_type->kind == TYPE_FLOAT) ||
                        (then_type->kind == TYPE_FLOAT && else_type->kind == TYPE_INT)) {
                        result = type_new(TYPE_FLOAT);
                        promoted = 1;
                    }
                    // int/float + bigint -> bigint
                    else if ((then_type->kind == TYPE_INT && else_type->kind == TYPE_BIGINT) ||
                             (then_type->kind == TYPE_BIGINT && else_type->kind == TYPE_INT) ||
                             (then_type->kind == TYPE_FLOAT && else_type->kind == TYPE_BIGINT) ||
                             (then_type->kind == TYPE_BIGINT && else_type->kind == TYPE_FLOAT)) {
                        result = type_new(TYPE_BIGINT);
                        promoted = 1;
                    }
                    // bigint + bigint
                    else if (then_type->kind == TYPE_BIGINT && else_type->kind == TYPE_BIGINT) {
                        result = type_new(TYPE_BIGINT);
                        promoted = 1;
                    }

                    if (!promoted) {
                        // 兜底：如果 kind 相同，直接取该类型（如两个 bool、两个 int）
                        if (then_type->kind == else_type->kind) {
                            result = type_copy(then_type);
                        } else {
                            result = type_new(TYPE_ANY);
                        }
                    }
                }
            } else {
                result = type_new(TYPE_ANY);
            }

            if (then_type) type_free(then_type);
            if (else_type) type_free(else_type);
            break;
        }
        case AST_AWAIT: {
            // await 表达式：解包 Future 类型，返回内部类型
            // 例如 await async_func() → async_func 返回 Future[T]，await 返回 T
            if (ast->u.await.expr) {
                TypeInfo* inner_type = infer_expr_type(s, ast->u.await.expr);
                if (inner_type) {
                    if (inner_type->kind == TYPE_FUTURE) {
                        type_free(inner_type);
                        // Future 类型：尝试从被等待的函数调用中提取声明返回类型
                        // 例如 await inner_task(x) → inner_task 声明返回 int → await 返回 int
                        if (ast->u.await.expr->kind == AST_CALL &&
                            ast->u.await.expr->u.call.callee &&
                            ast->u.await.expr->u.call.callee->kind == AST_VAR) {
                            const char* fn = ast->u.await.expr->u.call.callee->u.var.name;
                            Ast* fdef = func_table_find(&s->func_table, fn);
                            if (fdef && fdef->kind == AST_FUNC_DEF && fdef->u.func.is_async && fdef->u.func.return_type) {
                                result = type_copy(fdef->u.func.return_type);
                            } else {
                                result = type_new(TYPE_ANY);
                            }
                        } else if (ast->u.await.expr->kind == AST_CALL &&
                                   ast->u.await.expr->u.call.callee &&
                                   ast->u.await.expr->u.call.callee->kind == AST_INDEX &&
                                   ast->u.await.expr->u.call.callee->u.index.obj &&
                                   ast->u.await.expr->u.call.callee->u.index.obj->kind == AST_VAR &&
                                   ast->u.await.expr->u.call.callee->u.index.index &&
                                   ast->u.await.expr->u.call.callee->u.index.index->kind == AST_STRING) {
                            // await obj.method() — AST_MODULE_CALL 已被 visit_module.inc 转为 AST_CALL(INDEX(obj, "method"))
                            Ast* index_node = ast->u.await.expr->u.call.callee;
                            const char* obj_name = index_node->u.index.obj->u.var.name;
                            const char* method_name = index_node->u.index.index->u.string.value;
                            // 查找变量以获取对象类型
                            Symbol* obj_sym = scope_resolve(s->current, obj_name);
                            TypeInfo* obj_type = NULL;
                            int obj_type_owned = 0;
                            if (obj_sym && obj_sym->type) {
                                obj_type = obj_sym->type;
                            } else {
                                obj_type = infer_expr_type(s, index_node->u.index.obj);
                                obj_type_owned = 1;
                            }
                            if (obj_type) {
                                const char* type_name = NULL;
                                if (obj_type->kind == TYPE_CSTRUCT) {
                                    type_name = "cstruct";
                                } else if (obj_type->kind == TYPE_STRUCT) {
                                    type_name = "struct";
                                } else {
                                    type_name = native_get_type_name(obj_type->kind);
                                }
                                if (type_name) {
                                    int arity;
                                    TypeKind ret = native_get_instance_method_return_type(type_name, method_name, &arity);
                                    if (ret == TYPE_FUTURE) {
                                        TypeKind elem = native_get_instance_method_return_element_type(type_name, method_name);
                                        if (elem != TYPE_UNKNOWN && elem != TYPE_ANY) {
                                            result = type_new(elem);
                                            if (elem == TYPE_STRUCT) {
                                                result->struct_name = strdup("Socket");
                                            }
                                        } else {
                                            result = type_new(TYPE_ANY);
                                        }
                                    } else if (ret != TYPE_UNKNOWN) {
                                        result = type_new(ret);
                                    }
                                }
                                if (obj_type_owned) type_free(obj_type);
                            }
                            if (!result) result = type_new(TYPE_ANY);
                        } else if (ast->u.await.expr->kind == AST_MODULE_CALL) {
                            // 兼容旧版：模块实例方法调用（不应到达此分支）
                            const char* method_name = ast->u.await.expr->u.module_call.method_name;
                            Symbol* obj_sym = scope_resolve(s->current, ast->u.await.expr->u.module_call.module_name);
                            if (obj_sym && obj_sym->type && (obj_sym->type->kind == TYPE_STRUCT || obj_sym->type->kind == TYPE_CSTRUCT || obj_sym->type->kind == TYPE_FACE)) {
                                const char* type_name = (obj_sym->type->kind == TYPE_CSTRUCT) ? "cstruct" : "struct";
                                int arity;
                                TypeKind ret = native_get_instance_method_return_type(type_name, method_name, &arity);
                                if (ret == TYPE_FUTURE) {
                                    TypeKind elem = native_get_instance_method_return_element_type(type_name, method_name);
                                    if (elem != TYPE_UNKNOWN && elem != TYPE_ANY) {
                                        result = type_new(elem);
                                        if (elem == TYPE_STRUCT) {
                                            result->struct_name = strdup("Socket");
                                        }
                                    } else {
                                        result = type_new(TYPE_ANY);
                                    }
                                } else {
                                    result = type_new(ret);
                                }
                            } else {
                                result = type_new(TYPE_ANY);
                            }
                            if (!result) result = type_new(TYPE_ANY);
                        } else {
                            result = type_new(TYPE_ANY);
                        }
                    } else {
                        // 非 Future 类型（直接返回值），保持原类型
                        result = inner_type;
                    }
                } else {
                    result = type_new(TYPE_ANY);
                }
            } else {
                result = type_new(TYPE_ANY);
            }
            break;
        }
        case AST_SAFE_ACCESS: {
            // 安全访问：expr?.field / expr?.method()
            // 结果类型为字段/方法的类型，但总是可空的
            TypeInfo* obj_type = infer_expr_type(s, ast->u.safe_access.obj);
            const char* safe_name = ast->u.safe_access.name;

            if (ast->u.safe_access.is_call && obj_type &&
                (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT || obj_type->kind == TYPE_FACE)) {
                // 方法调用：复用统一的方法返回类型推断
                result = infer_method_return_type(s, obj_type, safe_name);
            } else if (obj_type) {
                // 字段访问：复用统一的字段类型推断
                result = infer_field_type(s, obj_type, safe_name, NULL);
            }
            if (!result) result = type_new(TYPE_ANY);
            // 安全访问的结果总是可空的
            result->nullable = 1;
            if (obj_type) type_free(obj_type);
            break;
        }
        case AST_TYPE_CHECK:
            // is 表达式返回 bool
            result = type_new(TYPE_BOOL);
            break;
        case AST_AS_CAST: {
            // as 转型表达式：匹配时返回目标类型，不匹配时返回 null
            // 类型推断返回目标类型（与 is 不同，as 的结果类型是目标类型）
            if (ast->u.type_check.type) {
                result = type_copy(ast->u.type_check.type);
            } else {
                result = type_new(TYPE_ANY);
            }
            break;
        }
        case AST_STRUCT_INIT: {
            // struct 构造函数调用返回对应的 struct 类型
            TypeInfo* struct_type = type_new(TYPE_STRUCT);
            // 处理模块限定的 struct 名称（如 "math.Point"），提取实际的 struct 名称
            const char* dot_pos = strchr(ast->u.struct_init.struct_name, '.');
            struct_type->struct_name = strdup(dot_pos ? dot_pos + 1 : ast->u.struct_init.struct_name);
            // 携带泛型类型参数（如 Box[int] 的 int）
            if (ast->u.struct_init.generic_type_count > 0 && ast->u.struct_init.generic_type_args) {
                struct_type->generic_count = ast->u.struct_init.generic_type_count;
                struct_type->generic_args = (TypeInfo**)malloc(sizeof(TypeInfo*) * struct_type->generic_count);
                if (struct_type->generic_args) {
                    for (int i = 0; i < struct_type->generic_count; i++) {
                        struct_type->generic_args[i] = type_copy(ast->u.struct_init.generic_type_args[i]);
                    }
                }
            }
            result = struct_type;
            break;
        }
        case AST_FIELD_ACCESS: {
            // 字段访问：需要知道对象的类型和字段的类型
            TypeInfo* obj_type = infer_expr_type(s, ast->u.field_access.obj);

            // ★★ 字段存在性检查（2026-09-30 补 ✓）—— 放在**推断这条路**上，而不是只在
            //   visit_field_access.inc（访问器那条 ✗）
            //   为什么：实测方法体里的**裸字段名**成员访问（`b.no_such_field` ✓ —— 前处理会把它
            //   换成 `self.b.no_such_field` ✓）只走推断这条路 ✓，访问器那条覆盖不到 ⇒
            //   编译期**一句不报** ✗ ⇒ 一路漏到运行期才炸 ✗（file_manager:346 `btnBack._loaded`
            //   就是这么炸的 ✓ —— 那句在运行期才报"struct 不存在字段 '_loaded'" ✗）
            //   判定保守（宁少报、不误报 ✓）：
            //     · 接收者是 **TYPE_STRUCT** 且结构体名已知 ✓
            //     · 能在当前作用域**确知字段表**（struct_field_count > 0 ✓）
            //     · 名字既不是字段、也不是同名方法 ⇒ 才报 ✓
            //     · 拿不到字段表（跨模块没符号 ✓、泛型 any ✓）⇒ 一律沉默 ✓
            if (obj_type && obj_type->kind == TYPE_STRUCT && obj_type->struct_name &&
                ast->u.field_access.field_name) {
                const char* fname = ast->u.field_access.field_name;
                Symbol* def_sym = scope_resolve(s->current, obj_type->struct_name);
                if (def_sym && def_sym->struct_field_count > 0) {
                    int known = 0;
                    for (int fi = 0; fi < def_sym->struct_field_count; fi++) {
                        if (def_sym->struct_field_names[fi] &&
                            strcmp(def_sym->struct_field_names[fi], fname) == 0) {
                            known = 1;
                            break;
                        }
                    }
                    if (!known) {
                        // 同名方法也算存在（方法调用另有语法 ✓；这里只兜"名字写错" ✓）
                        //   走 struct 定义 AST 的 methods[]（与 pri 检查同款写法 ✓ 准确 ✓）
                        Ast* sd = (Ast*)def_sym->type_decl_ast;
                        if (sd && sd->kind == AST_STRUCT_DEF) {
                            for (int mi = 0; mi < sd->u.struct_def.method_count; mi++) {
                                Ast* m = sd->u.struct_def.methods[mi];
                                if (m && m->kind == AST_FUNC_DEF && m->u.func.name &&
                                    strcmp(m->u.func.name, fname) == 0) {
                                    known = 1;
                                    break;
                                }
                            }
                        }
                    }
                    if (!known) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "struct '%s' 没有字段 '%s'",
                                 obj_type->struct_name, fname);
                        // ★ 先去重再报 ✓：显式 `self.b.xxx` 形态会被**访问器那条**先报一遍
                        //   （visit_field_access.inc ✓）⇒ 这里再报就是同一处、同一句话的第二遍 ✗
                        //   （实测：不去重会显示 "(重复 2 次)" ✗ —— 那只是计数不是折叠 ✓）
                        if (!error_has_at(ast->line, ast->column, msg)) {
                            error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                        }
                    }
                }
            }

            // 检查：基础类型不支持点号属性访问（如 float.w）
            if (obj_type) {
                TypeKind ok = obj_type->kind;
                if (ok != TYPE_ARRAY && ok != TYPE_DICT && ok != TYPE_STRING &&
                    ok != TYPE_STRUCT && ok != TYPE_CSTRUCT && ok != TYPE_FACE &&
                    ok != TYPE_ENUM && ok != TYPE_CLIB && ok != TYPE_PTR_GENERIC &&
                    ok != TYPE_ANY && ok != TYPE_INFER && ok != TYPE_UNKNOWN &&
                    ok != TYPE_GENERIC_PARAM && ok != TYPE_MULTI_RET) {
                    const char* field_name = ast->u.field_access.field_name;
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "类型 '%s' 不支持属性访问 '.%s'（只有 struct、dict、array 等类型才能访问属性）",
                        type_kind_to_string(ok), field_name);
                    error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                }
            }

            // 初始化字段索引为 -1（未确定）
            ast->u.field_access.field_index = -1;

            // 检查是否有字段类型守卫收窄（如 if s.age is int）
            if (ast->u.field_access.obj->kind == AST_VAR) {
                const char* var_name = ast->u.field_access.obj->u.var.name;
                const char* field_name = ast->u.field_access.field_name;
                int guard_name_len = strlen(var_name) + strlen(field_name) + 2;
                char* guard_name = (char*)malloc(guard_name_len);
                snprintf(guard_name, guard_name_len, "%s.%s", var_name, field_name);
                Symbol* guard_sym = scope_resolve(s->current, guard_name);
                free(guard_name);
                if (guard_sym && guard_sym->type) {
                    result = type_copy(guard_sym->type);
                    if (obj_type) type_free(obj_type);
                    break;
                }
            }

            // 使用统一的字段类型推断
            if (obj_type) {
                const char* field_name = ast->u.field_access.field_name;
                result = infer_field_type(s, obj_type, field_name, &ast->u.field_access.field_index);

                // ★ pri：私有成员的统一检查点（字段访问的**表达式**侧）
                //   为什么放这里：infer_field_type 刚跑完 ⇒ 导入模块的符号表**已懒加载** ✓
                //   （放在 visit_field_access 里查得太早，实测表还是 NULL ⇒ 静默失效 ✗）
                //   同文件 / 跨模块两支都由 pri_check_field_access 内部处理 ✓
                pri_check_field_access(s, ast, obj_type, field_name);

                // struct 类型：infer_field_type 可能未找到，尝试从变量符号回退查找
                if (!result && obj_type->kind == TYPE_STRUCT && ast->u.field_access.obj->kind == AST_VAR) {
                    const char* var_name = ast->u.field_access.obj->u.var.name;
                    Symbol* var_sym = scope_resolve(s->current, var_name);
                    if (var_sym && var_sym->struct_field_count > 0) {
                        for (int i = 0; i < var_sym->struct_field_count; i++) {
                            if (strcmp(var_sym->struct_field_names[i], field_name) == 0) {
                                result = type_copy(var_sym->struct_field_types[i]);
                                ast->u.field_access.field_index = i;
                                break;
                            }
                        }
                    }
                }
                // cstruct 类型：infer_field_type 可能未找到，尝试从变量符号回退查找
                if (!result && obj_type->kind == TYPE_CSTRUCT && ast->u.field_access.field_index < 0 && ast->u.field_access.obj->kind == AST_VAR) {
                    const char* var_name = ast->u.field_access.obj->u.var.name;
                    Symbol* var_sym = scope_resolve(s->current, var_name);
                    if (var_sym && var_sym->struct_field_count > 0) {
                        for (int i = 0; i < var_sym->struct_field_count; i++) {
                            if (strcmp(var_sym->struct_field_names[i], field_name) == 0) {
                                result = type_copy(var_sym->struct_field_types[i]);
                                TypeKind leno_kind = c_layout_type_to_leno(result->kind);
                                if (leno_kind != result->kind) {
                                    result->kind = leno_kind;
                                }
                                ast->u.field_access.field_index = i;
                                break;
                            }
                        }
                    }
                }
            }

            if (!result) result = type_new(TYPE_ANY);
            if (obj_type) type_free(obj_type);
            // 修正：如果字段类型被误推断为 struct 但实际是 face，修正为 face
            fix_struct_to_face(result);
            break;
        }
        case AST_ADDRESS_OF: {
            // &expr 取地址，结果类型为 TYPE_PTR
            // 尝试推断 element_type：如果操作数是 cstruct 字段访问，取字段类型
            result = type_new(TYPE_PTR);
            if (ast->u.address_of.operand && ast->u.address_of.operand->kind == AST_FIELD_ACCESS) {
                Ast* fa = ast->u.address_of.operand;
                TypeInfo* field_type = infer_expr_type(s, fa);
                if (field_type) {
                    result->element_type = field_type;  // 转移所有权，不 type_free
                }
            }
            break;
        }
        case AST_FUNC_DEF: {
            // 匿名函数表达式：根据返回类型注解构建函数类型
            TypeInfo* return_type;
            if (ast->u.func.return_type && ast->u.func.return_type->kind != TYPE_INFER) {
                return_type = type_copy(ast->u.func.return_type);
            } else {
                // 无显式返回类型注解 ⇒ **按函数体推断**（深扫，含嵌套分支里的 return ✓）
                //   ★ 2026-09-30 修正：推不出（函数体里没有任何"带值 return"）时按 **void** 处理。
                //     以前回落 `TYPE_ANY` ✗ ⇒ 回调字面量被当成 `func():any`，而 `func():void`
                //     形参/变量是 `TYPE_NULL` ⇒ `type_equals` 两侧都非空时走严格比 ⇒ 不相等 ⇒
                //     `func():void f = func() { ... }` 编译期报"声明类型与初始化值类型不匹配" ✗
                //     （实测：`Button.on_click(func(){...})` 这类日常回调最常踩；当时只能绕成
                //      `func():any f = func(){...}` 再包一层字面量喂进去 ✗）
                //   ⚠ 这不等于"宽松放行"：`func():int f = func() { }` 照样报错（void ≠ int ✓）
                TypeInfo* inferred = infer_return_type_from_body(s, ast->u.func.body);
                return_type = inferred ? inferred : type_new(TYPE_NULL);
            }
            
            // 构建参数类型数组
            TypeInfo** param_types = NULL;
            if (ast->u.func.pcnt > 0) {
                param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.func.pcnt);
                for (int i = 0; i < ast->u.func.pcnt; i++) {
                    if (ast->u.func.param_types && ast->u.func.param_types[i]) {
                        param_types[i] = type_copy(ast->u.func.param_types[i]);
                    } else {
                        param_types[i] = type_new(TYPE_ANY);
                    }
                }
            }
            
            result = type_function(return_type, param_types, ast->u.func.pcnt);
            break;
        }
        case AST_CLIB_DEF:
        default:
            result = type_new(TYPE_ANY);
            break;
    }
    
    // 缓存推断结果（简单类型直接缓存，复杂类型不缓存以避免内存问题）
    if (result && ast->kind != AST_CALL && ast->kind != AST_INDEX && ast->kind != AST_SLICE) {
        ast->cached_type = type_copy(result);
    }

    return result;
}
