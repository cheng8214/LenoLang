    // OP_TRY A Bx
    //   A=0（默认）：Bx 指向 **catch 块**，登记为 catch handler
    //   A=1：**无 catch 体、有 finally** ⇒ Bx 指向 **finally 块**，登记为 finally handler
    //     （异常派发里走"有 finally、无 catch ⇒ pending=1 先跑 finally、之后继续向外抛"那一支；
    //      若登记成 catch，异常会被 OP_FINALLY 清掉 ⇒ 静默消失，见 codegen_stmt.c 的说明）
    LABEL(OP_TRY): {
        int a = READ_A();
        int bx = READ_Bx();
        uint8_t* try_start = frame->ip - 4;
        // ★ 2026-10-30：外层处理状态**压栈**（不再是单槽 prev_*）⇒ 任意嵌套深度都能还原
        if (!vm_try_push(frame)) {
            if (!throw_runtime_error("try/defer 嵌套过深（超过 24 层）", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (a == 1) {
            // 无 catch、只有 finally：**保留外层 catch**（不覆盖），本层标记 has_catch=0
            frame->has_catch = 0;
            frame->finally_ip = try_start + bx;
        } else {
            frame->catch_ip = try_start + bx;
            frame->has_catch = 1;
            frame->finally_ip = NULL;
        }
        // 新 try 区域是**独立**的上下文：即便它写在某个 finally 体内，本层自己的
        // catch/finally 也必须先被派发到（出栈时再还原外层的 in_finally）✓
        frame->in_finally = 0;
        DISPATCH();
    }
    // OP_CATCH A Bx: R[A] = 当前异常（Bx 保留兼容，运行期已不需要：catch 体内的
    //   "再次 throw 先跑本层 finally"由 has_catch=0 + finally_ip 自然处理）
    LABEL(OP_CATCH): {
        int a = READ_A();
        R(a) = vm.exception;
        vm.exception = val_null();
        vm.has_exception = 0;
        frame->catch_ip = NULL;
        frame->has_catch = 0;
        DISPATCH();
    }
    // OP_FINALLY Bx: Bx = finally 块相对本指令起始的字节偏移
    LABEL(OP_FINALLY): {
        int a = READ_A();
        int bx = READ_Bx();
        uint8_t* fin_start = frame->ip - 4;
        uint8_t* target = fin_start + bx;
        if (a == 1) {
            // ★ 2026-10-30：**预备登记**（`try{...}catch{...}finally{...}` 的 try 体入口）。
            //   A=0 的 OP_TRY 只登记 catch（finally_ip = NULL）⇒ try 体内的 return / break /
            //   continue 根本看不见本层 finally（被静默跳过）✗ 实测。
            //   这里只补一条 finally_ip；本层处理状态已由 OP_TRY 压栈，不动异常/返回值状态。
            frame->finally_ip = target;
            DISPATCH();
        }
        if (!vm.pending_exception) {
            vm.exception = val_null();
        }
        frame->finally_ip = target;
        frame->in_finally = 1;
        DISPATCH();
    }
    LABEL(OP_END_TRY): {
        int a = READ_A();
        // 记下"刚刚结束的是哪一层 finally"（出栈前捕获）：break/continue 续跳要靠它区分
        // 嵌套 try 在 finally 体内正常结束时的 A=4。
        uint8_t* ending_fin = frame->finally_ip;

        // A=2：**离开 finally 块**（finally 体内的 break/continue 跳出，codegen 补发）——
        //   出栈还原外层 + 丢弃在飞异常，**不做任何跳转**（控制流交给随后的 OP_JMP）
        if (a == 2) {
            vm_try_pop(frame);
            frame->finally_goto_target = NULL;
            frame->finally_goto_owner = NULL;
            frame->finally_goto_layers = 0;
            vm.pending_exception = 0;
            vm.has_exception = 0;
            vm.exception = val_null();
            vm.exception_line = 0;
            DISPATCH();
        }
        // A=3：**catch 块入口**、且本层另有 finally
        //   catch 已消费 ⇒ has_catch=0；本层 finally 保留（catch 体内的 return / throw
        //   仍要先跑它）。catch 体结束自然落入 finally，由 finally 末尾的 END_TRY A=4 出栈。
        if (a == 3) {
            frame->catch_ip = NULL;
            frame->has_catch = 0;
            frame->in_finally = 0;
            vm.has_exception = 0;
            vm.exception = val_null();
            vm.exception_line = 0;
            DISPATCH();
        }
        // A=4：**finally 体结束** ⇒ 出栈还原外层
        // A=1：**catch 入口（无 finally）** ⇒ 整个 try 结束，出栈还原外层
        // A=0：try 体正常结束 —— 有 finally 时**不出栈**（还要跑 finally，仅露出外层 catch）；
        //        只有 catch 时整个 try 结束，出栈。
        if (a == 4 || a == 1) {
            vm_try_pop(frame);
        } else if (!frame->finally_ip) {
            vm_try_pop(frame);
        } else {
            vm_try_peek_outer_catch(frame);
        }

        // 需要在 finally 之后继续传播的异常
        if (vm.pending_exception) {
            // 异常覆盖 break/continue 的续跳（JS/Python 同语义）
            frame->finally_goto_target = NULL;
            frame->finally_goto_owner = NULL;
            frame->finally_goto_layers = 0;
            vm.pending_exception = 0;
            // 外层若**有 catch** ⇒ catch 优先（异常在它 try 体内）；
            // 否则先跑外层的 finally（finally-only 的 finally 比更外层的 catch 更近）
            if (!frame->has_catch && frame->finally_ip) {
                vm.pending_exception = 1;
                frame->ip = frame->finally_ip;
                vm.has_exception = 0;
                DISPATCH();
            } else if (frame->has_catch && frame->catch_ip) {
                frame->ip = frame->catch_ip;
                frame->catch_ip = NULL;
                frame->has_catch = 0;
                vm.has_exception = 0;
                DISPATCH();
            } else if (frame->finally_ip) {
                vm.pending_exception = 1;
                frame->ip = frame->finally_ip;
                vm.has_exception = 0;
                DISPATCH();
            } else {
                // 当前帧已无处理器：把**当前异常原样**继续向外抛
                //   ★ 2026-10-02 统一：修前这里 `throw_runtime_error("未捕获的异常", …)` 会
                //     **换掉**原异常 —— 调用方 catch 到的是"未捕获的异常"，真正的原因丢了 ✗
                //     （`try { throw "boom" } finally { }` 走的正是这条路径）
                vm.has_exception = 1;   // 跳进 finally 时它被清成了 0
                if (!dispatch_exception(vm.exception_line ? vm.exception_line : GET_CURRENT_LINE())) {
                    return -1;
                }
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
        }

        // 离开 try/catch/finally 范围：异常状态不再需要保留。
        // 若不清理，has_exception 会一直挂着，后续 native 调用（OP_CALL/OP_CALL_NATIVE
        // 里 "if (vm.has_exception) 转成 throw_runtime_error"）会把**早已处理过的旧异常**
        // 重新抛出（表现为 `} catch { }` 之后 add() 报前面那次越界的错）。
        vm.has_exception = 0;
        vm.exception = val_null();
        vm.exception_line = 0;

        // try 块中的 return 在此收尾（finally 已执行完）
        if (frame->has_try_return) {
            // return 覆盖 break/continue 的续跳
            frame->finally_goto_target = NULL;
            frame->finally_goto_owner = NULL;
            frame->finally_goto_layers = 0;
            // 还有外层 finally ⇒ 先跳过去，返回值继续暂存（has_try_return 不清）。
            if (FRAME_TRY_VALID(frame) && frame->finally_ip && !frame->in_finally) {
                frame->in_finally = 1;
                frame->ip = frame->finally_ip;
                DISPATCH();
            }
            frame->has_try_return = 0;
            Value saved = frame->try_return_value;
            REG_FINISH_RETURN(saved);
            DISPATCH();
        }

        // ★ 2026-10-30：break/continue 穿过 finally 的续跳（OP_FIN_GOTO 布置，A=4 收尾）。
        //   ending_fin == owner 才算"轮到我等的这一层" —— 嵌套 try 在 finally 体内正常
        //   结束时也会走 A=4，不能让它把外层 break 的续跳提前消费掉 ✗
        if (a == 4 && frame->finally_goto_target &&
            ending_fin == frame->finally_goto_owner) {
            if (frame->finally_goto_layers > 1 && !frame->in_finally && frame->finally_ip) {
                frame->finally_goto_layers--;
                frame->finally_goto_owner = frame->finally_ip;
                frame->in_finally = 1;
                frame->ip = frame->finally_ip;
                DISPATCH();
            }
            uint8_t* gt = frame->finally_goto_target;
            frame->finally_goto_target = NULL;
            frame->finally_goto_owner = NULL;
            frame->finally_goto_layers = 0;
            frame->ip = gt;
            DISPATCH();
        }
        DISPATCH();
    }
    // OP_FIN_GOTO A Bx: break/continue 穿过 try-finally
    //   A = 还要执行几层 finally；Bx = 续跳目标（原始 16 位偏移，相对本指令起点）。
    //   记录 (target, layers) 后跳进当前最内层 finally；每层 finally 末尾的 END_TRY A=4
    //   递减层数，减到 0 时续跳 target（见上面 A=4 分支）。
    LABEL(OP_FIN_GOTO): {
        int layers = READ_A();
        int bx = READ_Bx();
        frame->finally_goto_target = (frame->ip - 4) + bx;
        frame->finally_goto_layers = layers;
        frame->finally_goto_owner = NULL;
        if (FRAME_TRY_VALID(frame) && frame->finally_ip && !frame->in_finally && layers > 0) {
            frame->finally_goto_owner = frame->finally_ip;
            frame->in_finally = 1;
            frame->ip = frame->finally_ip;
        } else {
            frame->ip = frame->finally_goto_target;
            frame->finally_goto_target = NULL;
            frame->finally_goto_layers = 0;
        }
        DISPATCH();
    }
    // OP_THROW A: 抛出 R[A]（null 表示重新抛出当前异常）
    LABEL(OP_THROW): {
        int a = READ_A();
        int line = GET_CURRENT_LINE();
        Value exception = R(a);
        if (val_is_null(exception)) exception = vm.exception;
        // ★★ 2026-10-10 修：**裸值也要统一包成异常对象**
        //   原先只有字符串走 wrap_string_exception ✗ ⇒ `throw 42` 时 vm.exception 里存的是
        //   那个**裸 int** ✗，而 `catch e` 的静态类型是 Dict{msg,line,file,stack}
        //   （见 visit_exception.inc 的说明 ✓）⇒ **类型撒谎** ✗：紧接着的 `e.len()` / `e.msg`
        //   报的是「类型 object 上不存在方法 'len'」✗ —— 与实际完全对不上 ✓（实测 2026-10-10）。
        //   口径与 throw_existing_exception / throw_pending_exception **逐条对齐** ✓：
        //     · 已是异常对象（Dict）⇒ **原样**（位置是错误真正发生的地方 ✓ 不该被改写成"接住它的那行"）✓
        //     · 字符串            ⇒ wrap_string_exception（老行为 ✓）
        //     · 其它裸值          ⇒ 先字符串化，再走同一包装 ✓（这样任何 throw 出来的都是同一个形状 ✓）
        if (val_is_string(exception)) {
            exception = wrap_string_exception(exception, line);
        } else if (!(val_is_obj(exception) && val_as_obj(exception)->type == OBJ_DICT)) {
            char* m = value_to_string(exception);
            const char* mm = m ? m : "未知异常";
            ObjString* ms = str_copy(mm, (int)strlen(mm));
            if (ms) exception = wrap_string_exception(val_obj((Object*)ms), line);
            free(m);
        }
        if (vm.pending_exception) vm.pending_exception = 0;
        // 抛异常覆盖 break/continue 的续跳（JS/Python 同语义）
        frame->finally_goto_target = NULL;
        frame->finally_goto_owner = NULL;
        frame->finally_goto_layers = 0;
        vm.exception = exception;
        vm.has_exception = 1;
        vm.exception_line = line;

        // finally 里抛异常：该 finally 放弃执行。**逐帧**在派发循环里出栈到非 finally
        //   上下文（见下面 for 循环开头）—— 不能只处理当前帧：异常从被调函数（如
        //   finally 里调用的 boom()）抛出时，正在执行 finally 的是**外层帧** tf。
        int handled = 0;
        // ★ 协程边界：协程帧与调用方帧共用同一个 vm.frames，扫到 0 就会拿**调用方**的
        //   try-catch 来接管协程里的异常（实测主线里的 catch 会在协程执行期间打印），
        //   随后帧计数/执行流全乱 ⇒ 跑过 main 末尾、进垃圾内存（F1）。
        //   边界 = 当前协程的首帧索引（语义见 vm.c 里 saved_frame_cnt 的注释）。
        int throw_floor = vm.current_coroutine ? vm.current_coroutine->saved_frame_cnt : 0;
        if (throw_floor < 0) throw_floor = 0;
        // ★ 宿主回调边界（f6）：与 dispatch_exception 同款 —— 跑在回调里时不许跳到
        //   调用方的 catch（否则 catch 在内层循环里跑完、外层再用同一批帧继续跑 ⇒ 崩）
        if (vm.host_floor > throw_floor) throw_floor = vm.host_floor;
        for (int i = vm.frame_cnt - 1; i >= throw_floor; i--) {
            CallFrame* tf = &vm.frames[i];
            if (tf->closure && tf->closure->function && !tf->closure->function->has_try) continue;
            // finally 体内抛出的异常：该 finally 放弃执行 ⇒ 先出栈到非 finally 上下文
            if (tf->in_finally) {
                while (tf->try_top > 0 && tf->in_finally) vm_try_pop(tf);
                if (tf->in_finally) tf->in_finally = 0;
            }
            // ★ 2026-10-30：处理栈语义 —— 当前层要么有自己的 catch（has_catch），
            //   要么有 finally；有 catch 时 catch 优先（异常就发生在它的 try 体内），
            //   否则先跑 finally（finally-only 的 finally 比更外层 catch 更近）。
            if (!tf->has_catch && tf->finally_ip) {
                vm.pending_exception = 1;
                tf->ip = tf->finally_ip;
                vm.has_exception = 0;
            } else if (tf->has_catch && tf->catch_ip) {
                tf->ip = tf->catch_ip;
                tf->catch_ip = NULL;
                tf->has_catch = 0;
                vm.has_exception = 0;
            } else if (tf->finally_ip) {
                vm.pending_exception = 1;
                tf->ip = tf->finally_ip;
                vm.has_exception = 0;
            } else {
                continue;   // 本帧当前层没有处理器 ⇒ 继续向外层帧找
            }

            for (int j = vm.frame_cnt - 1; j > i; j--) {
                CallFrame* uw = &vm.frames[j];
                if (uw->locals && uw->locals_is_dynamic) {
                    free(uw->locals);
                    uw->locals = NULL;
                }
            }
            vm.frame_cnt = i + 1;
            vm.sp = tf->stack_base;
            handled = 1;
            break;
        }

        if (!handled) {
            // ★ 协程内未捕获的异常：**不要**在这里打印/报错 —— 直接返回 -1，
            //   由 vm_run_coroutine 检查 vm.has_exception 把错误写进协程的 task_future
            //   （await 侧再抛出 / asyncs.all 收集）。否则 `risky(-1)` 这类
            //   "协程里抛、调用方 await 捕获"的用例会在最后多出一条未处理错误 ✗
            if (vm.current_coroutine) return -1;
            // ★ 宿主回调边界（f6）：同理不报"未捕获"（接它的人在外层）⇒ 保持
            //   has_exception 交回 C 侧，外层解释器在原生边界处重新派发
            if (vm.host_floor > 0) return -1;
            // ★ 统一出口：与 dispatch_exception（vm_exception.inc）里的未捕获报告**同一份**
            //   实现（异常值就在 vm.exception 里，见上面 `vm.exception = exception;`）
            report_unhandled_exception(line);
            return -1;
        }
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }

    // --- 模块 ---
    // OP_LOAD_NATIVE_MODULE A Bx: 运行时加载原生模块，R[A] = null（占位）
