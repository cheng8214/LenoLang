    LABEL(OP_CALL): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        int nargs = b - 1;
        int nresults = c - 1;
        if (nresults < 0) nresults = 1;
        Value callee = R(a);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_obj(callee))) {
            ObjType ct = val_as_obj(callee)->type;

            // --- 脚本闭包：压入新帧，解释器继续在新帧执行 ---
            if (ct == OBJ_CLOSURE) {
                // ★ async 闭包：**调用即建协程**、返回 Future —— 与调用写法无关。
                //   编译器只在静态能确定 callee 是 async 时才发 OP_ASYNC_CALL，
                //   `var f = some_async; f()` / 把 async 函数当参数传 这些写法判不出来，
                //   必须在这里按运行期标记兜底；否则函数体被**同步执行**：返回值不是
                //   Future（`var r = f(3)` 拿到裸值），体里的 await 还会把当前协程的帧
                //   搞乱 ⇒ 整段**静默无输出**（C4）。
                ObjClosure* cl = (ObjClosure*)val_as_obj(callee);
                if (unlikely(cl->function && cl->function->is_async)) {
                    if (unlikely(vm_start_async_call(&vm, cl, frame->locals, a, nargs, a,
                                                     val_null(), 0, line) != 0)) {
                        if (!vm.has_exception) return -1;
                    }
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                if (likely(call_reg(cl, 0, val_null(),
                                    a + 1, nargs, a, nresults, line))) {
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                if (!vm.has_exception) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }

            // --- 裸函数对象：现场包装成闭包后调用 ---
            if (ct == OBJ_FUNCTION) {
                ObjFunction* f = (ObjFunction*)val_as_obj(callee);
                ObjClosure* cl = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                if (unlikely(!cl)) { runtime_error("内存分配失败", line); return -1; }
                cl->function = f;
                cl->upvalue_count = 0;
                cl->type_param_count = 0;
                cl->type_param_args = NULL;
                for (int i = 0; i < MAX_UPVALUES; i++) cl->upvalues[i] = NULL;
                // 同上：裸函数对象也要按 async 标记建协程（不是压帧同步调用）
                if (unlikely(f->is_async)) {
                    if (unlikely(vm_start_async_call(&vm, cl, frame->locals, a, nargs, a,
                                                     val_null(), 0, line) != 0)) {
                        if (!vm.has_exception) return -1;
                    }
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                if (call_reg(cl, 0, val_null(), a + 1, nargs, a, nresults, line)) {
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                if (!vm.has_exception) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }

            // --- 原生函数：同步调用，结果直接写 R[A] ---
            if (ct == OBJ_NATIVE) {
                ObjNative* native = (ObjNative*)val_as_obj(callee);
                int base = vm.sp;
                for (int i = 0; i < nargs; i++) vm_stack_push(&vm, R(a + 1 + i));
                Value result = native->function(nargs, vm.stack + base);
                vm.sp = base;
                if (unlikely(vm.has_exception)) {
                    char* msg = NULL;
                    if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
                        msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
                    } else {
                        msg = value_to_string(vm.exception);
                    }
                    error_clear();
                    if (!throw_runtime_error(msg, line)) { free(msg); return -1; }
                    free(msg);
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                if (nresults > 0) R(a) = result;
                DISPATCH();
            }

            // --- 绑定方法 ---
            // ⚠ receiver 去重：语义分析对 struct 方法调用会把 receiver 作为第 0 个
            //   实参放进 args（栈式对应 OP_INVOKE_METHOD_TYPED），而内置类型方法不带。
            //   若实参首项就是 receiver，则不重复插入，否则隐式补上。
            if (ct == OBJ_BOUND_METHOD) {
                ObjBoundMethod* bound = (ObjBoundMethod*)val_as_obj(callee);
                // receiver 去重判定：
                //   ① 首要仍是"首实参是否就是 receiver"（两处调用约定不同：MODULE_CALL
                //      路径**不含** receiver，语义改写出的 AST_CALL 路径**含** receiver）；
                //   ② 但值相等会被**自引用**骗到：`a.add(a)` 只传了 1 个实参却被当成
                //      [receiver] ⇒ 元素变成寄存器残留值 ✗。故再用形参个数佐证：
                //      实参个数 == arity ⇒ 含 receiver；实参个数 + 1 == arity ⇒ 不含。
                int self_in_args = 0;
                int expect_arity = 0;
                if (bound->closure && bound->closure->function) {
                    expect_arity = bound->closure->function->arity;
                } else if (bound->method) {
                    expect_arity = bound->method->arity;
                }
                if (nargs > 0) {
                    Value a0 = R(a + 1);
                    if (val_is_obj(a0) && val_is_obj(bound->receiver)) {
                        self_in_args = (val_as_obj(a0) == val_as_obj(bound->receiver));
                    } else if (!val_is_obj(a0) && !val_is_obj(bound->receiver)) {
                        self_in_args = (a0 == bound->receiver);
                    }
                    if (self_in_args && expect_arity > 0 && nargs != expect_arity &&
                        nargs + 1 == expect_arity) {
                        self_in_args = 0;   // 个数表明调用方没传 receiver ⇒ 自引用误判
                    }
                }
                if (bound->closure) {
                    // ★ async 方法经绑定对象调用（`var g = w.run; g()`）同样是"调用即建协程"
                    if (unlikely(bound->closure->function && bound->closure->function->is_async)) {
                        if (unlikely(vm_start_async_call(&vm, bound->closure, frame->locals, a, nargs, a,
                                                         bound->receiver, !self_in_args, line) != 0)) {
                            if (!vm.has_exception) return -1;
                        }
                        frame = &vm.frames[vm.frame_cnt - 1];
                        DISPATCH();
                    }
                    if (call_reg(bound->closure, self_in_args ? 0 : 1, bound->receiver,
                                 a + 1, nargs, a, nresults, line)) {
                        frame = &vm.frames[vm.frame_cnt - 1];
                        DISPATCH();
                    }
                    if (!vm.has_exception) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjNative* native = bound->method;
                if (native == NULL) {
                    if (!throw_runtime_error("绑定方法无效", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                int base = vm.sp;
                int pass_nargs;
                if (self_in_args) {
                    for (int i = 0; i < nargs; i++) vm_stack_push(&vm, R(a + 1 + i));
                    pass_nargs = nargs;
                } else {
                    vm_stack_push(&vm, bound->receiver);
                    for (int i = 0; i < nargs; i++) vm_stack_push(&vm, R(a + 1 + i));
                    pass_nargs = nargs + 1;
                }
                Value result = native->function(pass_nargs, vm.stack + base);
                vm.sp = base;
                if (unlikely(vm.has_exception)) {
                    char* msg = NULL;
                    if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
                        msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
                    } else {
                        msg = value_to_string(vm.exception);
                    }
                    error_clear();
                    if (!throw_runtime_error(msg, line)) { free(msg); return -1; }
                    free(msg);
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                if (nresults > 0) R(a) = result;
                DISPATCH();
            }

            // --- 结构体定义：按位置参数构造实例 ---
            if (ct == OBJ_STRUCT_DEF) {
                ObjStructDef* def = (ObjStructDef*)val_as_obj(callee);
                ObjStruct* obj = struct_instance_new(def);
                if (unlikely(!obj)) { runtime_error("内存不足，无法创建结构体实例", line); return -1; }
                for (int i = 0; i < nargs && i < def->field_count; i++) {
                    Value fv = R(a + 1 + i);
                    TypeKind et = def->fields[i].type;
                    if (et == TYPE_FLOAT && val_is_int(fv)) {
                        fv = val_float((double)val_as_int(fv));
                    } else if (et == TYPE_FLOAT && val_is_bigint(fv)) {
                        fv = val_float(bigint_to_double(val_as_bigint(fv)));
                    }
                    struct_set_field(obj, i, fv);
                }
                if (nresults > 0) R(a) = val_obj((Object*)obj);
                DISPATCH();
            }
        }

        // 不可调用
        {
            const char* msg = val_is_null(callee) ? "函数未定义" : "只能调用函数（不是对象类型）";
            if (!throw_runtime_error(msg, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
    }

    // OP_CALL_NATIVE A B C: R[A] = native(K[B])(R[A+1 .. A+C])
    LABEL(OP_CALL_NATIVE): {
        int a = READ_A(), b = READ_B();
        int nargs = READ_C();
        int line = GET_CURRENT_LINE();
        int cidx = READ_CONST_IDX(b);
        if (unlikely(cidx < 0 || cidx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("native 函数名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[cidx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("native 函数名必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        uint8_t* ip_before = frame->ip - 4;
        int cache_idx = (int)(((uintptr_t)ip_before >> 2) & (IC_NATIVE_CACHE_SIZE - 1));
        InlineNativeCacheEntry* cache = &vm.ic_native_cache[cache_idx];
        uint32_t name_hash = ((ObjString*)val_as_obj(name_val))->hash;

        ObjNative* native;
        if (likely(cache->valid && cache->name_hash == name_hash)) {
            vm.ic_hits++;
            native = cache->fn;
        } else {
            vm.ic_misses++;
            native = native_find_function(((ObjString*)val_as_obj(name_val))->chars);
            if (unlikely(!native)) {
                if (!throw_runtime_error("未找到 native 函数", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            cache->valid = 1;
            cache->name_hash = name_hash;
            cache->fn = native;
        }

        int base = vm.sp;
        for (int i = 0; i < nargs; i++) vm_stack_push(&vm, R(a + 1 + i));
        Value result = native->function(nargs, vm.stack + base);
        vm.sp = base;

        if (unlikely(vm.has_exception)) {
            char* msg = NULL;
            if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
                msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
            } else {
                msg = value_to_string(vm.exception);
            }
            error_clear();
            if (!throw_runtime_error(msg, line)) { free(msg); return -1; }
            free(msg);
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        R(a) = result;
        DISPATCH();
    }

    // OP_RETURN A B: 返回 R[A .. A+B-2]（B-1 个值）
    LABEL(OP_RETURN): {
        int a = READ_A(), b = READ_B();
        int nresults = b - 1;

        // 返回是天然 GC 安全点（与栈式一致：每 256 次 return 检查延迟 GC）
        if (unlikely(vm.gc_force_request)) {
            vm.gc_force_request = 0;
            gc_force_collect();
        } else if (unlikely(++vm.gc_return_counter >= 256)) {
            vm.gc_return_counter = 0;
            if (gc.deferred_gc && gc.enabled && !gc.running) {
                gc_try_collect_deferred();
            }
        }

        Value result = (nresults >= 1) ? R(a) : val_null();
        // 构造函数：返回 self（R0）
        if (frame->closure && frame->closure->function && frame->closure->function->is_ctor) {
            result = frame->locals[0];
        }

        // 有 finally 且尚未执行：先执行 finally，返回值暂存
        if (FRAME_TRY_VALID(frame) && frame->finally_ip != NULL && !frame->in_finally) {
            frame->try_return_value = result;
            frame->has_try_return = 1;
            frame->ip = frame->finally_ip;
            DISPATCH();
        }

        int ret_frame = frame->ret_frame;
        int ret_reg = frame->ret_reg;
        int ret_want = frame->ret_want;
        int stack_base = frame->stack_base;
        Value* locals = frame->locals;
        int local_count = frame->local_count;
        int locals_is_dynamic = frame->locals_is_dynamic;

        // 关闭指向本帧局部变量的所有 upvalue
        if (locals != NULL && local_count > 0 && vm.open_upvalues != NULL) {
            close_upvalues(locals, local_count, 1);
        }

        vm.frame_cnt--;

        // 宿主/回调停止条件（vm_call_value 路径）
        if (vm.stop_frame_cnt > 0 && vm.frame_cnt <= vm.stop_frame_cnt) {
            vm.sp = stack_base;
            vm_stack_push(&vm, result);
            vm.last_return_value = result;
            vm.last_return_count = 1;
            vm.last_return_values[0] = result;
            vm.stop_frame_cnt = 0;
            if (locals != NULL && locals_is_dynamic) free(locals);
            return 0;
        }

        // 协程顶层函数返回
        if (vm.current_coroutine) {
            if (vm.frame_cnt <= vm.current_coroutine->saved_frame_cnt) {
                vm.current_coroutine->result = result;
                gc_write_barrier((Object*)vm.current_coroutine, result);
                if (locals != NULL && locals_is_dynamic) free(locals);
                return 0;
            }
        }

        vm.last_return_value = result;
        vm.last_return_count = 1;
        vm.last_return_values[0] = result;

        // 结果写回调用方
        if (ret_frame >= 0 && ret_frame < vm.frame_cnt) {
            if (ret_want > 0) {
                Value* target = &vm.frames[ret_frame].locals[ret_reg];
                target[0] = result;
                for (int i = 1; i < ret_want; i++) target[i] = val_null();
            }
        } else if (ret_frame < 0 && vm.frame_cnt > 0) {
            // 宿主调用（栈式接口）：结果压回栈
            vm.sp = stack_base;
            vm_stack_push(&vm, result);
        }

        if (locals != NULL && locals_is_dynamic) free(locals);

        if (vm.frame_cnt == 0) {
            goto vm_exit;
        }

        frame = &vm.frames[vm.frame_cnt - 1];
        vm.sp = frame->stack_base;
        DISPATCH();
    }

    // OP_RETURN_MULTI A B: 返回 R[A .. A+B-1]，个数由 OP_RETURN_MULTI 的 B 寄存器给出
    //   编码：A = 结果起始寄存器，B = 存放个数的寄存器
    LABEL(OP_RETURN_MULTI): {
        int a = READ_A(), b = READ_B();
        int count = 0;
        if (val_is_int(R(b))) count = (int)val_as_int(R(b));
        if (count < 0) count = 0;
        if (count > VM_MAX_RETURNS) count = VM_MAX_RETURNS;

        if (unlikely(vm.gc_force_request)) {
            vm.gc_force_request = 0;
            gc_force_collect();
        } else if (unlikely(++vm.gc_return_counter >= 256)) {
            vm.gc_return_counter = 0;
            if (gc.deferred_gc && gc.enabled && !gc.running) {
                gc_try_collect_deferred();
            }
        }

        Value results[VM_MAX_RETURNS];
        for (int i = 0; i < count; i++) results[i] = R(a + i);
        if (count == 0) results[0] = val_null();

        if (FRAME_TRY_VALID(frame) && frame->finally_ip != NULL && !frame->in_finally) {
            frame->try_return_value = results[0];
            frame->has_try_return = 1;
            frame->ip = frame->finally_ip;
            DISPATCH();
        }

        int ret_frame = frame->ret_frame;
        int ret_reg = frame->ret_reg;
        int ret_want = frame->ret_want;
        int stack_base = frame->stack_base;
        Value* locals = frame->locals;
        int local_count = frame->local_count;
        int locals_is_dynamic = frame->locals_is_dynamic;

        if (locals != NULL && local_count > 0 && vm.open_upvalues != NULL) {
            close_upvalues(locals, local_count, 1);
        }

        vm.frame_cnt--;

        if (vm.stop_frame_cnt > 0 && vm.frame_cnt <= vm.stop_frame_cnt) {
            vm.sp = stack_base;
            for (int i = 0; i < count; i++) vm_stack_push(&vm, results[i]);
            vm.last_return_value = results[0];
            vm.last_return_count = count;
            for (int i = 0; i < count && i < VM_MAX_RETURNS; i++) vm.last_return_values[i] = results[i];
            vm.stop_frame_cnt = 0;
            if (locals != NULL && locals_is_dynamic) free(locals);
            return 0;
        }

        if (vm.current_coroutine) {
            if (vm.frame_cnt <= vm.current_coroutine->saved_frame_cnt) {
                vm.current_coroutine->result = results[0];
                gc_write_barrier((Object*)vm.current_coroutine, results[0]);
                if (locals != NULL && locals_is_dynamic) free(locals);
                return 0;
            }
        }

        vm.last_return_value = results[0];
        vm.last_return_count = count;
        for (int i = 0; i < count && i < VM_MAX_RETURNS; i++) vm.last_return_values[i] = results[i];

        if (ret_frame >= 0 && ret_frame < vm.frame_cnt) {
            if (ret_want > 0) {
                Value* target = &vm.frames[ret_frame].locals[ret_reg];
                for (int i = 0; i < ret_want; i++) {
                    target[i] = (i < count) ? results[i] : val_null();
                }
            }
        } else if (ret_frame < 0 && vm.frame_cnt > 0) {
            vm.sp = stack_base;
            for (int i = 0; i < count; i++) vm_stack_push(&vm, results[i]);
        }

        if (locals != NULL && locals_is_dynamic) free(locals);

        if (vm.frame_cnt == 0) {
            goto vm_exit;
        }

        frame = &vm.frames[vm.frame_cnt - 1];
        vm.sp = frame->stack_base;
        DISPATCH();
    }

    // OP_TAIL_CALL: 暂按普通调用执行（codegen 目前生成 CALL + RETURN）；
    // 真正的帧复用尾调用优化见 docs 的后续阶段。
    LABEL(OP_TAIL_CALL): {
        int a = READ_A(), b = READ_B();
        int nargs = b - 1;
        Value callee = R(a);
        int line = GET_CURRENT_LINE();
        if (likely(val_is_obj(callee) && val_as_obj(callee)->type == OBJ_CLOSURE)) {
            // 与 OP_CALL 同口径：async 闭包经尾调用也要建协程（返回值是 Future）
            ObjClosure* tcl = (ObjClosure*)val_as_obj(callee);
            if (unlikely(tcl->function && tcl->function->is_async)) {
                if (unlikely(vm_start_async_call(&vm, tcl, frame->locals, a, nargs, a,
                                                 val_null(), 0, line) != 0)) {
                    if (!vm.has_exception) return -1;
                }
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            if (call_reg(tcl, 0, val_null(),
                         a + 1, nargs, a, 1, line)) {
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            if (!vm.has_exception) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (!throw_runtime_error("尾调用目标必须是函数", line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }

    // --- 数组 ---
