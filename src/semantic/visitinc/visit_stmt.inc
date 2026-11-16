        case AST_RETURN: {
            // ★ 2026-11-05：模块顶层的 return 此前**静默中止模块初始化** ——
            //   后面的 main 不会被调用、进程直接以非 0 退出，而且一个字都不报 ✗
            //   （实测 `var x = 1` + `return x` + `main(){print("after")}`：
            //    零输出、exit=1）。这里编译期直接拦下。
            if (!s->current_func) {
                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                    "return 只能写在函数体内 —— 模块顶层的 return 会**中止整个模块的初始化**"
                    "（后面的 main 不会被调用）；请把这段逻辑放进函数里");
                visit(s, ast->u.ret);
                break;
            }
            // ★ 2026-11-10（泛型载荷 enum 步 3/3）：把**函数返回类型**作为"期望类型"传下去，
            //   这样 `return Res.ok(42)` 能从签名推出类型实参（与 var 声明同一个通道）。
            {
                TypeInfo* saved_expected = s->expected_type;
                TypeInfo* rt = s->current_func ? s->current_func->u.func.return_type : NULL;
                s->expected_type = (rt && rt->kind != TYPE_INFER) ? rt : NULL;
                visit(s, ast->u.ret);
                s->expected_type = saved_expected;
            }

            // ★ 2026-11-15：**裸 return（不返回值）** 在声明了返回类型的函数里要定向报错。
            //   此前走下面的 infer_expr_type(NULL) ⇒ 报「期望 int，实际 any」——
            //   文案既没说"你没写返回值"，也看不出是 return 少写了值 ✗
            //   （新解析规则下 `return` 换行不再吞下一行，这条路径会更常见）。
            if (!ast->u.ret) {
                TypeInfo* rt0 = s->current_func->u.func.return_type;
                if (rt0 && rt0->kind != TYPE_INFER && rt0->kind != TYPE_ANY &&
                    rt0->kind != TYPE_NULL) {   // TYPE_NULL = 声明成 void
                    error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                        "return 没有返回值，但函数声明返回 %s —— 写 `return 值`，"
                        "或把返回类型改成 void / 省略返回类型",
                        type_to_string(rt0));
                }
                break;
            }

            if (s->current_func) {
                TypeInfo* return_type_info = infer_expr_type(s, ast->u.ret);
                if (return_type_info) {
                    TypeInfo* expected_type = s->current_func->u.func.return_type;

                    // main 未声明返回类型时，返回值作为进程退出码，按 int 检查
                    // （声明为 int 时走下方通用检查；声明为其他类型已在 visit_func_impl 报错）
                    int main_exit_check = s->in_main_func &&
                        (!expected_type || expected_type->kind == TYPE_INFER);

                    // TYPE_INFER 表示函数声明时未指定返回类型，保持为 any（不推断）
                    if (main_exit_check) {
                        if (return_type_info->kind != TYPE_ANY &&
                            return_type_info->kind != TYPE_INFER &&
                            return_type_info->kind != TYPE_INT) {
                            error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                "main 函数的返回值将作为进程退出码，只能返回 int，实际 %s",
                                type_kind_to_string(return_type_info->kind));
                        }
                    } else if (!expected_type || expected_type->kind == TYPE_INFER) {
                        // 省略返回类型的函数返回 any，不进行推断
                        // 保持 TYPE_INFER（在类型系统中等同于 any）
                    } else if (!type_is_compatible(expected_type, return_type_info)) {
                        // 返回类型不匹配
                        const char* expected_str = type_to_string(expected_type);
                        char expected_buf[64];
                        strncpy(expected_buf, expected_str, 63);
                        expected_buf[63] = '\0';
                        const char* actual_str = type_to_string(return_type_info);
                        // ★ 2026-10-24（round6 复核 N3）：**函数返回函数**时最常见的原因是
                        //   "签名没写全"—— `func(int)`（无返回类型）与 `func(int):int` 是两个
                        //   不同类型，而报错只摆两个长得几乎一样的字符串，看不出差在哪 ✗
                        //   ⇒ 两侧都是函数类型时补一句口径说明（不改变判据，只加提示 ✓）
                        char fn_hint[256];
                        fn_hint[0] = '\0';
                        if (expected_type->kind == TYPE_FUNCTION &&
                            return_type_info->kind == TYPE_FUNCTION) {
                            snprintf(fn_hint, sizeof(fn_hint),
                                "（函数类型按**完整签名**比较：`func(int)` 与 `func(int):int` 是两个不同类型 —— "
                                "返回闭包时括号右边那截返回类型也要写全）");
                        }
                        // ★ 2026-11-16：多返回值按**位置**逐项比较 —— 个数不一样时把个数说出来。
                        //   不然 `[int, int]` vs `[int, int, int]` 只能靠眼睛数逗号（而且两串很像 ✗）
                        if (expected_type->kind == TYPE_MULTI_RET &&
                            return_type_info->kind == TYPE_MULTI_RET) {
                            if (expected_type->param_count != return_type_info->param_count) {
                                snprintf(fn_hint, sizeof(fn_hint),
                                    "（多返回值按**位置**逐项比较：声明 %d 个，实际 %d 个）",
                                    expected_type->param_count, return_type_info->param_count);
                            } else {
                                // 个数一样 ⇒ 逐项找出**第几个**不同，别让人对着两串相似的
                                // `[int, string]` / `[int, int]` 用眼睛数位置 ✓
                                int bad = -1;
                                for (int i = 0; i < expected_type->param_count; i++) {
                                    if (!type_is_compatible(expected_type->param_types ? expected_type->param_types[i] : NULL,
                                                            return_type_info->param_types ? return_type_info->param_types[i] : NULL)) {
                                        bad = i;
                                        break;
                                    }
                                }
                                if (bad >= 0) {
                                    snprintf(fn_hint, sizeof(fn_hint),
                                        "（多返回值按**位置**逐项比较：第 %d 个不同）", bad + 1);
                                }
                            }
                        }
                        error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                 "返回类型不匹配：期望 %s，实际 %s%s",
                                 expected_buf, actual_str, fn_hint);
                    } else if (expected_type->kind != TYPE_MULTI_RET &&
                               return_type_info->kind == TYPE_MULTI_RET &&
                               return_type_info->param_count > 1) {
                        // ★ 2026-11-16：**静默截断** —— `func f(): int { return two() }`
                        //   （two 声明返回 [int,int]）走的是 type_is_compatible 的
                        //   "非解构上下文退化成首元素"规则 ⇒ 只交出第 1 个值，其余**默默丢掉**。
                        //   这条规则本身要保留（`int a = two()` 取首值是合法写法），但在 return
                        //   处把**别的函数的多返回值**当单值交出去，几乎必然是漏写/抄错
                        //   ⇒ 补一条警告（不改语义：仍是取首值，运行结果与以前逐位一致 ✓）
                        //   ⚠ 只在"声明了具体单值返回类型"时报：声明成 any / 省略返回类型
                        //     那种写法太随意，逐个报就是噪音 ✗
                        warning_add_at_fmt(WARN_MULTI_RET_DEGRADE, ast->line, ast->column,
                            "`return` 拿到 %d 个返回值，但函数声明返回 %s ⇒ 只取**第 1 个**，"
                            "其余 %d 个被静默丢掉（要全部交出去：把返回类型写成多返回值"
                            "`[T1, T2]`，或先解构 `var[T1, T2](a, b) = …` 再用 a/b）",
                            return_type_info->param_count, type_to_string(expected_type),
                            return_type_info->param_count - 1);
                    }
                    type_free(return_type_info);
                }
            }
            break;
        }
            
        case AST_RETURN_MULTI: {
            // 多值返回: return a, b, c
            int count = ast->u.ret_multi.count;

            // ★ 2026-11-05：与 AST_RETURN 同口径 —— 模块顶层的 return 会静默中止模块初始化
            if (!s->current_func) {
                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                    "return 只能写在函数体内 —— 模块顶层的 return 会**中止整个模块的初始化**"
                    "（后面的 main 不会被调用）；请把这段逻辑放进函数里");
                for (int i = 0; i < count; i++) visit(s, ast->u.ret_multi.exprs[i]);
                break;
            }

            // main 的返回值作为进程退出码，不支持多值返回
            if (s->in_main_func) {
                error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                    "main 函数的返回值将作为进程退出码，不支持多值返回");
            }
            
            // 访问每个返回值表达式
            for (int i = 0; i < count; i++) {
                visit(s, ast->u.ret_multi.exprs[i]);
            }
            
            if (s->current_func) {
                TypeInfo* expected_type = s->current_func->u.func.return_type;
                
                if (expected_type && expected_type->kind == TYPE_MULTI_RET) {
                    // 函数声明了多返回值类型，检查数量和类型
                    if (count != expected_type->param_count) {
                        error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                            "多返回值数量不匹配：期望 %d 个，实际 %d 个",
                            expected_type->param_count, count);
                    } else {
                        // 逐个检查类型兼容性
                        for (int i = 0; i < count; i++) {
                            TypeInfo* actual = infer_expr_type(s, ast->u.ret_multi.exprs[i]);
                            if (actual) {
                                if (!type_is_compatible(expected_type->param_types[i], actual)) {
                                    // type_to_string 使用线程本地单缓冲区，
                                    // 连续调用会覆盖，必须先拷贝到独立缓冲区
                                    char expected_buf[64];
                                    char actual_buf[64];
                                    strncpy(expected_buf, type_to_string(expected_type->param_types[i]), 63);
                                    expected_buf[63] = '\0';
                                    strncpy(actual_buf, type_to_string(actual), 63);
                                    actual_buf[63] = '\0';
                                    error_add_at_fmt(ERR_TYPE_MISMATCH, ast->u.ret_multi.exprs[i]->line, ast->u.ret_multi.exprs[i]->column,
                                        "返回值 %d 类型不匹配：期望 %s，实际 %s",
                                        i + 1, expected_buf, actual_buf);
                                }
                                type_free(actual);
                            }
                        }
                    }
                } else if (expected_type && expected_type->kind != TYPE_INFER) {
                    // 函数声明了单返回值类型但 return 了多个值
                    error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                        "函数声明返回单个值，但 return 语句返回了 %d 个值", count);
                }
                // TYPE_INFER（无注解）允许多返回值，不报错
            }
            break;
        }
            
        case AST_EXPR_STMT:
            visit(s, ast->u.expr_stmt.expr);
            break;

        case AST_ARRAY:
            for (int i = 0; i < ast->u.array.count; i++) {
                visit(s, ast->u.array.items[i]);
            }
            break;

        case AST_DICT:
            for (int i = 0; i < ast->u.dict.count; i++) {
                visit(s, ast->u.dict.entries[i].key);
                visit(s, ast->u.dict.entries[i].value);
            }
            break;

        case AST_INTERP_STRING:
            for (int i = 0; i < ast->u.interp_string.count - 1; i++) {
                if (ast->u.interp_string.exprs[i]) {
                    visit(s, ast->u.interp_string.exprs[i]);
                }
            }
            break;

        case AST_BREAK:
        case AST_CONTINUE: {
            // B3：循环外使用 break/continue——此前静默通过（编译不报错、运行期什么都不做）
            // 沿作用域链向上找循环作用域；先碰到函数边界（闭包体）即视为不在循环内
            // ★ 2026-11-11：`break` 在 switch 的 case 里**合法**（codegen 把 switch 当
            //   break 目标 ⇒ 离开该 switch）；此前只认 is_loop ⇒ `switch x { case 1 { break } }`
            //   在不带循环的函数里被误报「break 不能用在循环外」✗。`continue` 仍只认循环。
            int in_loop = 0;
            int in_switch = 0;
            for (Scope* sc = s->current; sc; sc = sc->parent) {
                if (sc->is_loop) { in_loop = 1; break; }
                if (sc->is_switch) { in_switch = 1; continue; }   // 不 break：continue 要越过 switch 找循环
                if (sc->is_func) break;
            }
            if (ast->kind == AST_CONTINUE) {
                if (!in_loop) {
                    error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                             "'continue' 不能用在循环外（只能出现在 for/while 循环体内）");
                }
            } else if (!in_loop && !in_switch) {
                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                         "'break' 不能用在循环或 switch 外（只能出现在 for/while 循环体内，"
                         "或 switch 的 case 分支里）");
            }
            break;
        }