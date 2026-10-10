    // OP_TRY A Bx
    //   A=0（默认）：Bx 指向 **catch 块**，登记为 catch handler
    //   A=1：**无 catch 体、有 finally** ⇒ Bx 指向 **finally 块**，登记为 finally handler
    //     （异常派发里走"有 finally、无 catch ⇒ pending=1 先跑 finally、之后继续向外抛"那一支；
    //      若登记成 catch，异常会被 OP_FINALLY 清掉 ⇒ 静默消失，见 codegen_stmt.c 的说明）
    LABEL(OP_TRY): {
        int a = READ_A();
        int bx = READ_Bx();
        uint8_t* try_start = frame->ip - 4;
        frame->prev_catch_ip = frame->catch_ip;
        frame->prev_finally_ip = frame->finally_ip;
        if (a == 1) {
            frame->catch_ip = NULL;
            frame->finally_ip = try_start + bx;
        } else {
            frame->catch_ip = try_start + bx;
            frame->finally_ip = NULL;
        }
        DISPATCH();
    }
    // OP_CATCH A Bx: R[A] = 当前异常；清除 catch 状态（寄存器式不再走栈传异常）
    //   Bx != 0：本层 finally 块相对本指令的偏移 —— 在这里就**预备**好 finally_ip，
    //   使 catch 体内再次 throw 时先执行本层 finally、再向外传播
    //   （`try { throw "first" } catch e { throw "second" } finally { ... }`）。
    LABEL(OP_CATCH): {
        int a = READ_A();
        int bx = READ_Bx();
        R(a) = vm.exception;
        vm.exception = val_null();
        vm.has_exception = 0;
        frame->catch_ip = NULL;
        // 预备本层 finally：catch 体内再次 throw 时要先跑它（见 catch_finally_ip 说明）
        frame->catch_finally_ip = bx ? ((frame->ip - 4) + bx) : NULL;
        DISPATCH();
    }
    // OP_FINALLY Bx: Bx = finally 块相对本指令起始的字节偏移
    LABEL(OP_FINALLY): {
        int bx = READ_Bx();
        uint8_t* fin_start = frame->ip - 4;
        if (!vm.pending_exception) {
            vm.exception = val_null();
        }
        frame->finally_ip = fin_start + bx;
        frame->in_finally = 1;
        DISPATCH();
    }
    LABEL(OP_END_TRY): {
        int a = READ_A();
        // A=2：**离开 finally 块**（finally 体内的 break/continue 跳出，codegen 补发）——
        //   只做"注销本层 try 注册 + 丢弃在飞异常"，**不做任何跳转**：
        //   控制流由紧随其后的 OP_JMP 决定 ✓（JS/Python 同语义：break/continue 覆盖在飞异常；
        //   不注销的话后面同一帧里的异常会跳回这个已经跑完的 finally 块 ✗）
        if (a == 2) {
            frame->catch_ip = frame->prev_catch_ip;
            frame->prev_catch_ip = NULL;
            frame->catch_finally_ip = NULL;
            frame->finally_ip = frame->prev_finally_ip;
            frame->prev_finally_ip = NULL;
            frame->in_finally = 0;
            vm.pending_exception = 0;
            vm.has_exception = 0;
            vm.exception = val_null();
            vm.exception_line = 0;
            DISPATCH();
        }
        // A=1：**catch 块入口**那次 END_TRY。
        //   catch 体期间必须保持 catch_ip = NULL 且 prev_catch_ip = 外层 catch：
        //     · catch 体内 throw ⇒ catch_finally_ip 分支先跑本层 finally，
        //       再由 finally 末尾的 END_TRY 用 prev_catch_ip 继续向外传播 ✓
        //     · catch 体内普通 throw（本层无 finally）⇒ 走 prev_catch_ip 兜底 ✓
        //   （A=0：正常收尾，把 prev_* 还原回 catch_ip/finally_ip 并清空 prev_*）
        if (!a) {
            frame->catch_ip = frame->prev_catch_ip;
            frame->prev_catch_ip = NULL;
            frame->catch_finally_ip = NULL;
        }
        frame->finally_ip = frame->prev_finally_ip;
        frame->prev_finally_ip = NULL;
        frame->in_finally = 0;

        // 需要在 finally 之后继续传播的异常
        if (vm.pending_exception) {
            vm.pending_exception = 0;
            if (frame->catch_ip) {
                frame->ip = frame->catch_ip;
                frame->catch_ip = NULL;
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
            frame->has_try_return = 0;
            Value saved = frame->try_return_value;
            REG_FINISH_RETURN(saved);
            DISPATCH();
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
        vm.exception = exception;
        vm.has_exception = 1;
        vm.exception_line = line;

        // 在 finally 中 throw：清掉本帧 finally 防止死循环
        if (FRAME_TRY_VALID(frame) && frame->in_finally && frame->finally_ip) {
            frame->finally_ip = NULL;
        }

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
            uint8_t* tcatch = tf->catch_ip ? tf->catch_ip : tf->prev_catch_ip;
            uint8_t* tfin = tf->finally_ip ? tf->finally_ip : tf->prev_finally_ip;
            if (!tcatch && !tfin) continue;

            for (int j = vm.frame_cnt - 1; j > i; j--) {
                CallFrame* uw = &vm.frames[j];
                if (uw->locals && uw->locals_is_dynamic) {
                    free(uw->locals);
                    uw->locals = NULL;
                }
            }
            vm.frame_cnt = i + 1;
            vm.sp = tf->stack_base;

            if (tf->catch_finally_ip && !tf->in_finally) {
                // catch 体内再次 throw：先执行**本层** finally，再向外传播
                vm.pending_exception = 1;
                tf->ip = tf->catch_finally_ip;
                tf->catch_finally_ip = NULL;
                vm.has_exception = 0;
            } else if (tf->finally_ip && !tf->in_finally && !tf->catch_ip) {
                vm.pending_exception = 1;
                tf->ip = tf->finally_ip;
                vm.has_exception = 0;
            } else if (tcatch) {
                tf->ip = tcatch;
                vm.has_exception = 0;
                if (tf->catch_ip) tf->catch_ip = NULL;
                else tf->prev_catch_ip = NULL;
            } else {
                vm.pending_exception = 1;
                tf->ip = tfin;
                vm.has_exception = 0;
            }
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
