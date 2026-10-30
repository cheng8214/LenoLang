#include "semantic_internal.h"

// 名字是不是**未被遮蔽的用户模块别名** —— 是则返回它的 ImportedModuleInfo，否则 NULL。
//   与 visitinc/visit_module.inc 的 is_module 判据同源（别名表 + 作用域里解析得到的符号），
//   只是多判了一步"符号种类必须是全局/模块级"：局部变量遮蔽别名时（md09 那种情况）
//   该名字就是变量，不能再当模块用。
ImportedModuleInfo* semantic_module_alias_info(Semantic* s, const char* name) {
    if (!s || !name || !name[0]) return NULL;
    ImportedModuleInfo* info = find_imported_module(s, name);
    if (!info || info->load_failed) return NULL;
    Symbol* sym = scope_resolve(s->current, name);
    if (!sym) return NULL;
    if (sym->kind != SYM_GLOBAL && sym->kind != SYM_MODULE) return NULL;
    return info;
}

// 两个模块路径是否指向**同一个模块文件**（N7 的判据）。
//   按文件名比较（去掉目录）：同别名重复 import 同一个文件是合法写法（md07 就是 a/b 两次），
//   只有"同别名指向不同文件"才是冲突。⚠ 保守方向：判据只用于**放过**，宁漏报不误报 ✓
int semantic_same_module_file(const char* a, const char* b) {
    if (!a || !b) return 0;
    const char* ba = a;
    const char* pb = strpbrk(a, "/\\");
    while (pb) { ba = pb + 1; pb = strpbrk(ba, "/\\"); }
    const char* bb = b;
    pb = strpbrk(b, "/\\");
    while (pb) { bb = pb + 1; pb = strpbrk(bb, "/\\"); }
    return strcmp(ba, bb) == 0;
}

// ★ 2026-10-24（round8 N14）：**const 的深层保护** —— 表达式的"根对象"是不是 const？
// ----------------------------------------------------------------------------
// 症状：const 检查原来只覆盖"目标就是那个变量"的几种形态：
//     `const A = [1,2]; A[0] = 1`      ✔ 拦（AST_INDEX 的 obj 是 AST_VAR）
//     `const A = [1,2]; A.add(1)`      ✔ 拦（修改方法 + obj 是 AST_VAR）
// 而"根是 const、路径多拐一层"的全漏：
//     `const s = new S8(); s.v = 42`         ✗ 字段写（AST_FIELD 写路径没有 const 检查）
//     `const s9 = new S9(); s9.arr.add(1)`   ✗ 字段上的修改方法
//     `const s9 = new S9(); s9.arr[0] = 9`   ✗ 字段元素的写
// const 的语义是**整体不可变**（docs：深不可变）⇒ 根不可变就够了。
// 返回 1 = 根是 const，并把根名字写进 buf（`s` / `m.CFG`），调用方拼文案用。
// ⚠ 只走 AST_VAR / AST_MODULE_ACCESS / AST_FIELD_ACCESS / AST_INDEX 这条链：
//   parser 把 `a.b` 一律建成 AST_MODULE_ACCESS（可能指模块成员，也可能指 struct 字段），
//   所以这条分支要**两种都认**（先按模块别名查，不是再按变量查）。
int sem_const_root_name(Semantic* s, Ast* e, char* buf, int bufsz) {
    if (!e || !buf || bufsz <= 0) return 0;
    switch (e->kind) {
        case AST_VAR: {
            SymRef tmp;
            memset(&tmp, 0, sizeof(tmp));
            Symbol* sym = resolve_variable_with_upvalue(s, e->u.var.name, &tmp);
            if (sym && sym->is_const) {
                snprintf(buf, (size_t)bufsz, "%s", e->u.var.name);
                return 1;
            }
            return 0;
        }
        case AST_FIELD_ACCESS:
            return sem_const_root_name(s, e->u.field_access.obj, buf, bufsz);
        case AST_INDEX:
            return sem_const_root_name(s, e->u.index.obj, buf, bufsz);
        case AST_MODULE_ACCESS: {
            const char* base = e->u.module_access.module_name;
            ImportedModuleInfo* mi = semantic_module_alias_info(s, base);
            if (mi && mi->sym_table) {
                ModuleVarSymbol* mv = module_symbol_table_find_var(mi->sym_table,
                                                                   e->u.module_access.member_name);
                if (mv && mv->is_const) {
                    snprintf(buf, (size_t)bufsz, "%s.%s", base, e->u.module_access.member_name);
                    return 1;
                }
                return 0;
            }
            // 不是模块别名 ⇒ 是 `变量.字段`（parser 的形态）⇒ 看基变量
            SymRef tmp;
            memset(&tmp, 0, sizeof(tmp));
            Symbol* bsym = resolve_variable_with_upvalue(s, base, &tmp);
            if (bsym && bsym->is_const) {
                snprintf(buf, (size_t)bufsz, "%s", base);
                return 1;
            }
            return 0;
        }
        default:
            return 0;
    }
}

// N1（2026-10-25）：跨模块写导出变量**已经打通**（读 10-24、写 10-25），
//   原来这里那个 semantic_report_module_var_write（"跨模块写没打通，请写 setter"）
//   已删除 —— 现在 `m.var = v` 会真的写进 module->globals 并同步 exports。
//   留下的两条硬约束在调用点：const 变量不可写、非导出变量不可写
//   （见 visit_var.inc / visit_expr.inc 的 AST_MODULE_ACCESS 分支）。

// ============================================================================
// Upvalue 管理
// ============================================================================

// 当前正在分析的函数在 func_max_index / func_pinned 里的下标（顶层代码没有函数 ⇒ 0）
int sem_local_level(Semantic* s) {
    return s->func_stack_depth > 0 ? s->func_stack_depth - 1 : 0;
}

// 在函数作用域中分配局部变量索引
//   ★ 槽位回收：local_index 现在**可回退**（见 sem_local_release），所以这里必须同时记
//     历史高水位 —— func->local_count 只能取高水位（回退后的当前值会偏小 ⇒ codegen 的
//     临时寄存器起点偏低 ⇒ 临时值盖掉变量，静默错值）。
int allocate_local_index(Semantic* s) {
    int idx = s->local_index++;
    int lv = sem_local_level(s);
    if (s->local_index > s->func_max_index[lv]) s->func_max_index[lv] = s->local_index;
    return idx;
}

// 槽位回收标记：在**语句/作用域开始处**取，结束处交给 sem_local_release 回退。
//   返回当前计数器，本身不做任何修改（回退时机由调用方负责）。
int sem_local_mark(Semantic* s) {
    return s->local_index;
}

// 槽位回收：把计数器回退到 max(mark, 该函数"钉住下界")，只降不升。
//   ⚠ 钉住下界的来源：被闭包**按引用捕获**的槽位（upvalue 持有 frame->locals+slot 裸指针）
//     —— 它们必须独占槽位到函数结束，否则闭包读到的是后来复用该槽位的变量的值。
//   ⚠ 只用于"该范围内分配的槽位此刻都已失效"的边界：语句结束（for/if/switch/try 的隐藏槽位）
//     或控制结构体作用域结束。**变量声明本身不可回收**（它在本块内后续仍可读）。
void sem_local_release(Semantic* s, int mark) {
    int floor = s->func_pinned[sem_local_level(s)];
    int target = mark > floor ? mark : floor;
    if (target < s->local_index) s->local_index = target;
}

// 查找导入的模块信息
ImportedModuleInfo* find_imported_module(Semantic* s, const char* alias) {
    for (int i = 0; i < s->imported_module_count; i++) {
        if (strcmp(s->imported_modules[i].alias, alias) == 0) {
            return &s->imported_modules[i];
        }
    }
    return NULL;
}

// 添加 upvalue 到函数，返回索引
int add_upvalue(Ast* func_ast, const char* name, int index, int is_local, int is_value_capture) {
    if (!func_ast || !name) return -1;
    
    // 首次分配（检查指针是否为NULL）
    if (func_ast->u.func.upvalue_names == NULL) {
        func_ast->u.func.upvalue_names = (char**)calloc(MAX_UPVALUES, sizeof(char*));
        func_ast->u.func.upvalue_indices = (int*)calloc(MAX_UPVALUES, sizeof(int));
        func_ast->u.func.upvalue_is_local = (int*)calloc(MAX_UPVALUES, sizeof(int));
        func_ast->u.func.upvalue_is_value_capture = (int*)calloc(MAX_UPVALUES, sizeof(int));
        func_ast->u.func.upvalue_count = 0;
    }
    
    int count = func_ast->u.func.upvalue_count;
    
    // 检查是否已存在（只比较名字，因为同一函数内同名变量应该是同一个upvalue）
    for (int i = 0; i < count; i++) {
        if (func_ast->u.func.upvalue_names[i] && 
            strcmp(func_ast->u.func.upvalue_names[i], name) == 0) {
            return i;  // 返回已存在的索引
        }
    }
    
    // 扩容检查
    if (count >= MAX_UPVALUES) {
        return -1;
    }
    
    func_ast->u.func.upvalue_names[count] = strdup(name);
    func_ast->u.func.upvalue_indices[count] = index;
    func_ast->u.func.upvalue_is_local[count] = is_local;
    func_ast->u.func.upvalue_is_value_capture[count] = is_value_capture;
    func_ast->u.func.upvalue_count = count + 1;
    
    return count;
}

// 解析变量并处理 upvalue
// 返回符号，并在需要时自动注册 upvalue
// ★ 2026-10-10：本函数改成"**读路径统一置位**"的外壳，原实现改名到 `_raw` ✓
//   为什么非得在这里补一刀（实测踩到的 ✗）：函数式变量被**调用**时（`var c = func(){...}`
//   然后 `c()` ），被调方的解析**不走 `visit_var.inc` 的 AST_VAR 分支** ✗ ⇒ 只在外层置位的话
//   `c` 会被误报成"从未被读取" ✗ —— 一个字都没写错的代码被报出来 ✗（我第一版就报了 ✓）。
//   本函数是"按名字取值"的总入口 ✓（读变量 / 调函数变量 / 插值 / 传参… 都过这里 ✓）
//   ⇒ 在此置位覆盖最全 ✓；而**纯赋值**路径不走这里 ✓（实测 `written = 5` 仍会被正确报成
//   "只写没读" ✓）⇒ 不会把"只写过"洗成"读过" ✓。
Symbol* resolve_variable_with_upvalue(Semantic* s, const char* name, SymRef* ref) {
    Symbol* sym = resolve_variable_with_upvalue_raw(s, name, ref);
    if (sym) sym->is_read = 1;
    return sym;
}

Symbol* resolve_variable_with_upvalue_raw(Semantic* s, const char* name, SymRef* ref) {
    if (!s->current) {
        return scope_resolve(s->current, name);
    }
    
    // 先在当前作用域查找
    Symbol* local_sym = scope_resolve_local(s->current, name);
    if (local_sym) {
        ref->kind = local_sym->kind;
        ref->index = local_sym->index;
        ref->name = strdup(local_sym->name);
        ref->type_kind = local_sym->type ? local_sym->type->kind : TYPE_ANY;
        ref->element_type_kind = (local_sym->type && local_sym->type->element_type) ? local_sym->type->element_type->kind : TYPE_ANY;
        ref->struct_name = (local_sym->type && local_sym->type->struct_name) ? strdup(local_sym->type->struct_name) : NULL;
        return local_sym;
    }
    
    // 找到当前函数的函数作用域
    Scope* current_func_scope = s->current;
    while (current_func_scope && !current_func_scope->is_func) {
        current_func_scope = current_func_scope->parent;
    }
    
    // 向上层查找，确定变量在哪一层
    Scope* scope = s->current->parent;
    Scope* var_scope = NULL;  // 变量定义所在的作用域
    Symbol* target_sym = NULL;
    int func_boundary_count = 0;  // 经过的函数边界数
    
    while (scope) {
        Symbol* sym = scope_resolve_local(scope, name);
        if (sym) {
            target_sym = sym;
            var_scope = scope;
            // 如果找到的是全局变量、全局函数、native函数、模块变量或类型定义，直接返回，不作为 upvalue 处理
            if (sym->kind == SYM_GLOBAL || sym->kind == SYM_GLOBAL_FUNC || sym->kind == SYM_NATIVE || sym->kind == SYM_MODULE ||
                sym->kind == SYM_TYPE || sym->kind == SYM_STRUCT || sym->kind == SYM_CSTRUCT || sym->kind == SYM_ENUM) {
                ref->kind = sym->kind;
                ref->index = sym->index;
                ref->name = strdup(sym->name);
                ref->type_kind = sym->type ? sym->type->kind : TYPE_ANY;
                ref->element_type_kind = (sym->type && sym->type->element_type) ? sym->type->element_type->kind : TYPE_ANY;
                ref->struct_name = (sym->type && sym->type->struct_name) ? strdup(sym->type->struct_name) : NULL;
                return sym;
            }
            // 如果变量定义在当前函数的函数作用域中，不是 upvalue
            if (scope == current_func_scope) {
                ref->kind = target_sym->kind;
                ref->index = target_sym->index;
                ref->name = strdup(target_sym->name);
                ref->type_kind = target_sym->type ? target_sym->type->kind : TYPE_ANY;
                ref->element_type_kind = (target_sym->type && target_sym->type->element_type) ? target_sym->type->element_type->kind : TYPE_ANY;
                ref->struct_name = (target_sym->type && target_sym->type->struct_name) ? strdup(target_sym->type->struct_name) : NULL;
                return target_sym;
            }
            // 检查变量定义的作用域是否在当前函数内部（如while/for循环中）
            // 从var_scope向上查找，如果找到current_func_scope，说明变量在当前函数内
            if (var_scope && current_func_scope) {
                Scope* check_scope = var_scope->parent;
                int crossed_func = 0;
                while (check_scope) {
                    if (check_scope == current_func_scope) {
                        ref->kind = target_sym->kind;
                        ref->index = target_sym->index;
                        ref->name = strdup(target_sym->name);
                        ref->type_kind = target_sym->type ? target_sym->type->kind : TYPE_ANY;
                        ref->element_type_kind = (target_sym->type && target_sym->type->element_type) ? target_sym->type->element_type->kind : TYPE_ANY;
                        ref->struct_name = (target_sym->type && target_sym->type->struct_name) ? strdup(target_sym->type->struct_name) : NULL;
                        return target_sym;
                    }
                    if (check_scope->is_func) {
                        crossed_func++;
                        break;  // 跨越了函数边界
                    }
                    check_scope = check_scope->parent;
                }
            }
            // 变量在外层函数中，需要增加 func_boundary_count
            // 但只有当当前作用域是函数作用域时才需要补偿，
            // 因为遍历从 s->current->parent 开始，如果 s->current 不是函数作用域（如 if 块），
            // 遍历已经经过了当前函数的边界并正确计数了
            if (scope->is_func && s->current->is_func) {
                func_boundary_count++;
            }
            break;
        }
        // 检查是否跨越函数边界
        if (scope->is_func) {
            func_boundary_count++;
        }
        scope = scope->parent;
    }
    
    // 如果变量定义在非函数作用域（如while/for循环），需要继续向上找到函数作用域
    if (var_scope && !var_scope->is_func) {
        scope = var_scope->parent;
        while (scope) {
            if (scope->is_func) {
                func_boundary_count++;
                break;
            }
            scope = scope->parent;
        }
    }
    
    if (!target_sym) {
        // 最后尝试全局查找
        Symbol* global_sym = scope_resolve(s->current, name);
        if (global_sym) {
            ref->kind = global_sym->kind;
            ref->index = global_sym->index;
            ref->name = strdup(global_sym->name);
            ref->type_kind = global_sym->type ? global_sym->type->kind : TYPE_ANY;
            ref->element_type_kind = (global_sym->type && global_sym->type->element_type) ? global_sym->type->element_type->kind : TYPE_ANY;
            ref->struct_name = (global_sym->type && global_sym->type->struct_name) ? strdup(global_sym->type->struct_name) : NULL;
            return global_sym;
        }
        return NULL;
    }
    
    // 标记为被捕获
    target_sym->is_captured = 1;
    
    // 建立 upvalue 链
    // func_boundary_count 表示变量定义在往上第几层函数中
    // 0 = 直接外层, 1 = 外两层, 等等
    
    // 计算起始函数在栈中的位置
    // start_level 是变量定义的函数层级
    // upvalue 应该从 start_level+1 开始建立（外层函数引用内层变量）
    int start_level = s->func_stack_depth - 1 - func_boundary_count;
    if (start_level < 0) start_level = 0;
    
    // 从变量定义层的下一层开始，向外到当前层，逐层建立 upvalue
    int upvalue_index = target_sym->index;
    int is_local = 1;  // 第一层引用的是局部变量
    int upvalue_added = 0;  // 标记是否添加了 upvalue
    // 如果变量在循环作用域内定义，使用值捕获
    int is_value_capture = target_sym->is_in_loop;
    
    // 从 start_level+1 开始，因为 start_level 是变量定义的函数，不需要 upvalue
    for (int i = start_level + 1; i < s->func_stack_depth; i++) {
        Ast* func = s->func_stack[i];
        // ⚠ **struct 方法不是闭包 ⇒ 不能拥有 upvalue**（v3.2.8；修 0xC0000005）
        //   机理：方法调用走 `self["method"](self, args)` 绑定的方法对象，**没有闭包环境**；
        //   可本函数会照常把它需要的东西挂成 upvalue ⇒ 方法体内 `OP_GET_UPVALUE`
        //   去读一个并不存在的 upvalue 数组项 ⇒ **访问违例崩溃**（实测 exit=0xC0000005）。
        //   判据（方法 = 首参是 self）只认"链上被挂 upvalue 的那一层"，不误伤：
        //   · 文件级 struct 的方法读**全局** ⇒ 上面早就按 SYM_GLOBAL 返回了，走不到这里；
        //   · 方法读**自己的**参数/局部 ⇒ 不是 upvalue，也走不到这里；
        //   · 普通函数/闭包捕获外层 ⇒ func_stack 上那一层不是方法 ⇒ 不受影响（套件守着）。
        //   取舍：**响亮报错 > 静默崩溃**（与 S2、实例二十三/二十六 同一哲学）。
        //   若将来真要支持"方法捕获"，得让 `new` 时把外层帧的局部绑进方法闭包（另一条路，见 docs 实例三十二）。
        if (func && func->kind == AST_FUNC_DEF && func->u.func.pcnt > 0 && func->u.func.params &&
            func->u.func.params[0] && strcmp(func->u.func.params[0], "self") == 0) {
            error_add_at_fmt(ERR_SEMANTIC, func->line, func->column,
                "struct 方法不能捕获外层局部变量 '%s'（方法不是闭包、没有捕获能力）—— "
                "请改用参数传入，或用字段在 new 时承接该值", name);
            return target_sym;   // 不再建立 upvalue（编译已失败，避免继续发射崩溃代码）
        }
        int idx = add_upvalue(func, name, upvalue_index, is_local, is_value_capture);
        if (idx < 0) return NULL;
        
        // 下一层将引用这一层的 upvalue
        upvalue_index = idx;
        is_local = 0;  // 后续层引用的是 upvalue
        upvalue_added = 1;
        // 后续层继承值捕获标记
        is_value_capture = is_value_capture;
    }
    
    // ★ 槽位回收（寄存器号 8 位上限）
    //   按**引用**捕获（is_value_capture=0）⇒ 闭包持有 &frame->locals[slot]：该槽位必须钉住到
    //   **定义它的那个函数**结束，否则被复用后闭包读到的是后来变量的值。
    //   值捕获（循环体内定义 ⇒ is_value_capture=1）在建立闭包时就把值拷进 uv->closed，
    //   不持有槽位指针 ⇒ 不需要钉住（这也是循环里每轮重新创建闭包的语义基础）。
    //   ⚠ 钉住记账必须记在**定义该变量的函数层级**（start_level）上：此刻 s 正在分析内层
    //     函数，s->local_index 是内层的计数器，记错层会污染内层函数的回收下界。
    if (upvalue_added && !is_value_capture && target_sym->index >= 0) {
        int lv = (start_level >= 0 && start_level < 64) ? start_level : 0;
        int need = target_sym->index + 1;
        if (need > s->func_pinned[lv]) s->func_pinned[lv] = need;
    }

    // 只有实际建立了 upvalue 链，才设置为 SYM_UPVALUE
    // 否则保持原始类型（局部变量或参数）
    if (upvalue_added) {
        ref->kind = SYM_UPVALUE;
        ref->index = upvalue_index;
    } else {
        ref->kind = target_sym->kind;
        ref->index = target_sym->index;
    }
    // ★ 2026-10-30：`name` 可能**就是 ref->name 本身**（调用点常见写法是
    //   `resolve_variable_with_upvalue(s, ref.name, &ref)`，如闭包里的 `x++`）⇒
    //   必须**先复制后释放**；否则 strdup 读的是刚 free 的 12 字节（ASAN 实测 UAF）。
    char* new_name = strdup(name);
    free(ref->name);
    ref->name = new_name;
    free(ref->struct_name);
    ref->type_kind = target_sym->type ? target_sym->type->kind : TYPE_ANY;
    ref->element_type_kind = (target_sym->type && target_sym->type->element_type) ? target_sym->type->element_type->kind : TYPE_ANY;
    ref->struct_name = (target_sym->type && target_sym->type->struct_name) ? strdup(target_sym->type->struct_name) : NULL;
    return target_sym;
}
