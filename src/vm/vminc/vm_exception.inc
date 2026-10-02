// ============================================================================
// 异常处理 - 从 vm_exception.c 迁移
// ============================================================================

// 辅助函数：获取Value的类型描述
static inline const char* value_type_desc(Value val) {
    switch (val_get_type(val)) {
        case VAL_NULL: return "null";
        case VAL_BOOL: return "bool";
        case VAL_INT: return "int";
        case VAL_FLOAT: return "float";
        case VAL_OBJ: {
            switch (val_as_obj(val)->type) {
                case OBJ_STRING: return "string";
                case OBJ_ARRAY: return "array";
                case OBJ_DICT: return "dict";
                case OBJ_STRUCT: {
                    ObjStruct* s = (ObjStruct*)val_as_obj(val);
                    return s->def ? s->def->name : "struct";
                }
                case OBJ_FUNCTION: return "function";
                case OBJ_CLOSURE: return "closure";
                case OBJ_NATIVE: return "native";
                case OBJ_BIGINT: return "int";
                case OBJ_MODULE: return "module";
                case OBJ_SOCKET: return "socket";
                default: return "object";
            }
        }
        default: return "unknown";
    }
}

// 辅助函数：将 Value 转换为简短的调试字符串（用于报错信息）
// 截断长字符串、显示数组长度、显示结构体名称等，避免报错信息本身导致二次崩溃
static inline void value_to_short_str(Value val, char* buf, size_t buf_size) {
    switch (val_get_type(val)) {
        case VAL_NULL:
            snprintf(buf, buf_size, "null");
            break;
        case VAL_BOOL:
            snprintf(buf, buf_size, "%s", val_as_bool(val) ? "true" : "false");
            break;
        case VAL_INT:
            snprintf(buf, buf_size, "%lld", (long long)val_as_int(val));
            break;
        case VAL_FLOAT:
            snprintf(buf, buf_size, "%g", val_as_num(val));
            break;
        case VAL_OBJ: {
            Object* obj = val_as_obj(val);
            switch (obj->type) {
                case OBJ_STRING: {
                    ObjString* s = (ObjString*)obj;
                    size_t len = s->len;
                    if (len <= 32) {
                        snprintf(buf, buf_size, "\"%.*s\"", (int)len, s->chars);
                    } else {
                        snprintf(buf, buf_size, "\"%.*s...\" (len=%zu)", 28, s->chars, len);
                    }
                    break;
                }
                case OBJ_ARRAY: {
                    ObjArray* arr = (ObjArray*)obj;
                    snprintf(buf, buf_size, "array(len=%d)", arr->count);
                    break;
                }
                case OBJ_DICT: {
                    ObjDict* d = (ObjDict*)obj;
                    snprintf(buf, buf_size, "dict(count=%d)", d->count);
                    break;
                }
                case OBJ_STRUCT: {
                    ObjStruct* s = (ObjStruct*)obj;
                    snprintf(buf, buf_size, "struct(%s)", s->def ? s->def->name : "unknown");
                    break;
                }
                case OBJ_FUNCTION: {
                    ObjFunction* f = (ObjFunction*)obj;
                    snprintf(buf, buf_size, "func(%s)", f->name ? f->name : "<anonymous>");
                    break;
                }
                case OBJ_CLOSURE:
                    snprintf(buf, buf_size, "closure");
                    break;
                case OBJ_NATIVE:
                    snprintf(buf, buf_size, "native");
                    break;
                case OBJ_BIGINT: {
                    char* bs = bigint_to_string(val_as_bigint(val));
                    if (bs) {
                        snprintf(buf, buf_size, "%s", bs);
                        free(bs);
                    } else {
                        snprintf(buf, buf_size, "bigint(?)");
                    }
                    break;
                }
                case OBJ_MODULE:
                    snprintf(buf, buf_size, "module");
                    break;
                case OBJ_SOCKET:
                    snprintf(buf, buf_size, "socket");
                    break;
                case OBJ_FFI_POINTER:
                    snprintf(buf, buf_size, "ffi_ptr");
                    break;
                case OBJ_FFI_CALLBACK:
                    snprintf(buf, buf_size, "ffi_callback");
                    break;
                default:
                    snprintf(buf, buf_size, "object(type=%d)", obj->type);
                    break;
            }
            break;
        }
        default:
            snprintf(buf, buf_size, "unknown");
            break;
    }
}

// 运行时错误（不抛出异常，直接报错）
static inline void runtime_error(const char* msg, int line) {
    // 获取当前调用帧的 chunk 文件名
    const char* filename = NULL;
    if (vm.frame_cnt > 0) {
        CallFrame* frame = &vm.frames[vm.frame_cnt - 1];
        if (frame->chunk && frame->chunk->filename) {
            filename = frame->chunk->filename;
        }
    }
    
    // 临时设置文件名用于错误报告
    const char* old_filename = error_get_filename();
    if (filename) {
        error_set_filename(filename);
    }
    
    error_add_at(ERR_RUNTIME, line, 0, msg);
    
    // 恢复原来的文件名
    error_set_filename(old_filename);
}

// ============================================================================
// 调用栈追踪 (Stack Trace)
// ============================================================================

// 构建调用栈数组，每帧格式: "func_name (file:line)"
// 从最内层帧（当前执行帧）到最外层帧（main）排列
static ObjArray* build_stack_trace(void) {
    ObjArray* stack_arr = arr_new(vm.frame_cnt);
    if (!stack_arr) return NULL;

    // 调用栈只到**协程边界**为止（协程的栈不该带出调用方的帧）
    int st_floor = vm.current_coroutine ? vm.current_coroutine->saved_frame_cnt : 0;
    if (st_floor < 0) st_floor = 0;
    for (int i = vm.frame_cnt - 1; i >= st_floor; i--) {
        CallFrame* f = &vm.frames[i];

        // 获取函数名
        const char* func_name = NULL;
        if (f->closure && f->closure->function && f->closure->function->name) {
            func_name = f->closure->function->name;
        } else if (i == 0) {
            func_name = "<main>";
        } else {
            func_name = "<anonymous>";
        }

        // 获取文件名
        const char* filename = "unknown";
        if (f->chunk && f->chunk->filename) {
            filename = f->chunk->filename;
        }

        // 获取行号（当前 IP 对应的行）
        // ⚠ ip 指向**下一条**指令（DISPATCH 取指后已 +4，约定见 run/01_prologue.inc）
        //   ⇒ 本指令 = ip[-4..-1]，行号表必须按 ip-4 取。此前按 ip 取 ⇒ 报出来的
        //   是**下一条指令**所在行（实测：`var q = 1 / z` 在第 20 行，[位置] 与 #0
        //   都报 21）—— 与 `e.line`（= GET_CURRENT_LINE()，按 ip-4）自相矛盾 ✗
        int line = 0;
        if (f->chunk && f->chunk->lines && f->ip && f->chunk->code) {
            int offset = (int)(f->ip - f->chunk->code) - 4;
            if (offset < 0) offset = 0;
            if (offset < f->chunk->len) {
                line = f->chunk->lines[offset];
            }
        }

        // 拼接 "func_name (file:line)"
        char frame_desc[BUFFER_XLARGE];
        snprintf(frame_desc, sizeof(frame_desc), "%s (%s:%d)", func_name, filename, line);
        ObjString* frame_str = str_copy(frame_desc, strlen(frame_desc));
        if (frame_str) {
            arr_write(stack_arr, stack_arr->count, val_obj((Object*)frame_str));
        }
    }
    return stack_arr;
}

// 打印未捕获异常的调用栈
// exception: 异常值（可能是 Dict 或字符串）
// line: 异常发生时的行号
static void print_unhandled_exception(Value exception, int line) {
    const char* msg = "未知错误";
    const char* filename = "unknown";
    ObjArray* stack_arr = NULL;
    int has_stack = 0;

    // 尝试从异常 Dict 中提取信息
    if (val_is_obj(exception) && val_as_obj(exception)->type == OBJ_DICT) {
        ObjDict* err_dict = (ObjDict*)val_as_obj(exception);

        // 获取 msg
        ObjString* msg_key = str_copy("msg", 3);
        Value msg_val = dict_get(err_dict, val_obj((Object*)msg_key));
        if (val_is_string(msg_val)) {
            msg = ((ObjString*)val_as_obj(msg_val))->chars;
        }

        // 获取 file
        ObjString* file_key = str_copy("file", 4);
        Value file_val = dict_get(err_dict, val_obj((Object*)file_key));
        if (val_is_string(file_val)) {
            filename = ((ObjString*)val_as_obj(file_val))->chars;
        }

        // 获取 line
        ObjString* line_key = str_copy("line", 4);
        Value line_val = dict_get(err_dict, val_obj((Object*)line_key));
        if (val_is_int(line_val)) {
            line = (int)val_as_int(line_val);
        }

        // 获取 stack
        ObjString* stack_key = str_copy("stack", 5);
        Value stack_val = dict_get(err_dict, val_obj((Object*)stack_key));
        if (val_is_obj(stack_val) && val_as_obj(stack_val)->type == OBJ_ARRAY) {
            stack_arr = (ObjArray*)val_as_obj(stack_val);
            has_stack = 1;
        }
    } else {
        // 字符串异常：直接使用 value_to_string
        char* str = value_to_string(exception);
        if (str) {
            // 使用静态缓冲区避免内存管理问题
            static char msg_buf[BUFFER_XXLARGE];
            snprintf(msg_buf, sizeof(msg_buf), "%s", str);
            msg = msg_buf;
            free(str);
        }

        // 获取当前文件名
        if (vm.frame_cnt > 0) {
            CallFrame* f = &vm.frames[vm.frame_cnt - 1];
            if (f->chunk && f->chunk->filename) {
                filename = f->chunk->filename;
            }
        }
    }

    // 如果异常 Dict 中没有 stack（如 throw "string"），实时构建
    if (!has_stack) {
        stack_arr = build_stack_trace();
        has_stack = (stack_arr != NULL);
    }

    // 打印错误信息和调用栈
    fprintf(stderr, "[错误] %s:%d: %s\n", filename, line, msg);
    // ★ 2026-10-02：msg 里**已含** [调用栈]（统一文案，见 throw_runtime_error）
    //   ⇒ 不再在下面重复列一遍同样的帧（修前未捕获的原生/操作码错误会把同一份
    //   调用栈打两遍：一遍在 msg 里、一遍在这里 ✗）
    if (has_stack && stack_arr && stack_arr->count > 0 && !strstr(msg, "[调用栈]")) {
        fprintf(stderr, "调用栈:\n");
        for (int i = 0; i < stack_arr->count; i++) {
            if (val_is_string(stack_arr->elements[i])) {
                fprintf(stderr, "    at %s\n", ((ObjString*)val_as_obj(stack_arr->elements[i]))->chars);
            }
        }
    }
}

// 将字符串异常包装为异常 Dict 对象
// 用于 throw "string" 时自动包装为含 msg/line/file/stack 的异常对象
static Value wrap_string_exception(Value str_val, int line) {
    if (!val_is_string(str_val)) return str_val;

    ObjDict* err_dict = dict_new(8);
    if (!err_dict) return str_val;  // 内存不足时回退为原始字符串

    // msg 属性
    ObjString* msg_key = str_copy("msg", 3);
    if (msg_key) {
        dict_set(err_dict, val_obj((Object*)msg_key), str_val);
    }

    // line 属性
    ObjString* line_key = str_copy("line", 4);
    if (line_key) {
        dict_set(err_dict, val_obj((Object*)line_key), val_int(line));
    }

    // file 属性
    CallFrame* current_frame = &vm.frames[vm.frame_cnt - 1];
    const char* filename = (current_frame->chunk && current_frame->chunk->filename)
                           ? current_frame->chunk->filename : "unknown";
    ObjString* file_key = str_copy("file", 4);
    ObjString* file_val = str_copy(filename, (int)strlen(filename));
    if (file_key && file_val) {
        dict_set(err_dict, val_obj((Object*)file_key), val_obj((Object*)file_val));
    }

    // stack 属性
    ObjArray* stack_arr = build_stack_trace();
    if (stack_arr) {
        ObjString* stack_key = str_copy("stack", 5);
        if (stack_key) {
            dict_set(err_dict, val_obj((Object*)stack_key), val_obj((Object*)stack_arr));
        }
    }

    return val_obj((Object*)err_dict);
}

// 前置声明：两者定义在本函数**之后**（get_current_context / append_stack_trace），
// 统一错误文案要用到
static inline void get_current_context(
    const char** func_name, int* ip_offset,
    int* src_line, const char** filename);
static inline int append_stack_trace(char* buf, int pos, size_t buf_size);

// 取当前异常的文本 msg（是 Dict 就读它的 msg 字段，否则字符串化）—— 调用方负责 free()
static inline char* exception_msg_dup(void) {
    if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_DICT) {
        ObjString* mk = str_copy("msg", 3);
        if (mk) {
            Value mv = dict_get((ObjDict*)val_as_obj(vm.exception), val_obj((Object*)mk));
            if (val_is_string(mv)) return strdup(((ObjString*)val_as_obj(mv))->chars);
        }
    }
    return value_to_string(vm.exception);
}

// 取当前异常**自己**的行号（Dict 的 line 字段；拿不到回 0）
//   用途：异常跨过原生边界/协程边界被原样带回来时，位置要听它的，而不是听"接住它的那行"
static inline int exception_own_line(void) {
    if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_DICT) {
        ObjString* lk = str_copy("line", 4);
        if (lk) {
            Value lv = dict_get((ObjDict*)val_as_obj(vm.exception), val_obj((Object*)lk));
            if (val_is_int(lv)) return (int)val_as_int(lv);
        }
    }
    return 0;
}

// 报告一个**未捕获**的异常：`[错误] file:line: msg` + 调用栈，并登记一条 runtime 错误
//   ★ 统一出口：dispatch_exception 找不到 handler 时、协程恢复路径没人接时都走这里
static inline void report_unhandled_exception(int line) {
    char* msg = exception_msg_dup();
    print_unhandled_exception(vm.exception, line);
    runtime_error(msg ? msg : "未知异常", line);
    // S2：上面已带调用栈即时打印过，标记后退出时 error_print_all 不再重复展示
    if (errors.count > 0) errors.list[errors.count - 1].printed = 1;
    free(msg);
}

// 派发**当前** vm.exception：从当前帧向上找 catch/finally 并跳过去
// 返回：1 = 异常已被 catch/finally 接管（调用方重取 frame 后继续 DISPATCH）
//       0 = 未捕获（调用方应停机返回 -1）
// ★ 2026-10-02：这段走法**只此一份** —— throw_runtime_error / throw_existing_exception /
//   原生边界（throw_pending_exception）全部经它派发。此前只有 throw_runtime_error 一处，
//   想保留"已是异常对象"的异常（含它自己的 msg/line/file/stack）就只能复制这段，
//   而**复制这段曾经出过事**（见下面 ①②③ 的死循环记录）⇒ 先抽成一份再谈扩展 ✓
static inline int dispatch_exception(int line) {
    // 从当前调用帧开始向上查找有 catch/finally 的帧
    // ★ 协程边界（**必须有**）：协程帧与调用方帧共用同一个 vm.frames，
    //   如果一路扫到 0，协程里抛的异常会被**调用方**的 try-catch 接走 ——
    //   实测：协程 `throw` 被 main 的 catch 接住（`caught: boom` 在协程执行期间就打印了），
    //   随后帧计数与执行流全乱 ⇒ 程序跑过 main 末尾、进垃圾内存
    //   （非确定性报错 / 访问违例，即 F1）。协程的首帧索引存在
    //   `current_coroutine->saved_frame_cnt`（语义：协程第一个帧的下标），
    //   搜索到此为止 —— 协程里没接住的异常按文档"路径3"交给 asyncs.run() 收集。
    int frame_floor = vm.current_coroutine ? vm.current_coroutine->saved_frame_cnt : 0;
    if (frame_floor < 0) frame_floor = 0;
    // ★ 宿主回调边界（f6）：vm_call_value 跑回调（arr.map/filter、FFI 回调）期间会把
    //   host_floor 设成"调用前的帧数"。异常**不许**展开到它下面去 —— 跳进调用方的
    //   catch 会让 catch 在**内层**解释器循环里执行完，内层返回后外层又在同一批帧上
    //   继续跑 ⇒ 状态写坏（实测：filter 回调里 throw ⇒ catch 打印正常、进程随后
    //   0xC0000409/0xC0000005 崩，非确定性）。⇒ 撞界就交回 C 侧往外传，外层再派发。
    int host_floor = vm.host_floor;
    if (host_floor > frame_floor) frame_floor = host_floor;
    for (int i = vm.frame_cnt - 1; i >= frame_floor; i--) {
        CallFrame* frame = &vm.frames[i];
        // 快速路径：如果函数确定没有 try，直接跳过
        // 注意：如果 closure 为 NULL（如顶层代码），不能跳过，需要检查 catch_ip
        if (frame->closure && frame->closure->function && !frame->closure->function->has_try) {
            continue;
        }
        // ★ 与 OP_THROW 的派发**同口径**（src/vm/vminc/run/09_exception.inc）。
        //   ⚠ 这两处曾经分叉，代价是**挂死（死循环）**，实测（修复前）：
        //     `main() { try { .. } catch { c = c + 1 }  print("B c=" + c)  var z = 0  var boom = 1 / z }`
        //     ⇒ `B c=1 / B c=2 / B c=3 …` **永不退出** —— try **之外**的除零被上面的 catch
        //     反复接住，控制流一次次回到 catch 段。三处分叉各自都能造成它：
        //     ① 只看 `catch_ip`/`finally_ip`，不认 `prev_*` 兜底 ⇒ catch 体内抛错被当成"没人接"；
        //     ② 跳进 catch 时**不注销 handler**，而 `} catch {`（**无变量**）根本不发 OP_CATCH
        //        （见 codegen_stmt.c 的注释）⇒ `catch_ip` 永远挂着 ⇒ 本帧后续**每一个**运行期
        //        错误都被同一个 catch 接住（这正是 OP_THROW 侧同期修过、这里漏掉的那一条）；
        //     ③ 缺 `in_finally` 守卫与 `catch_finally_ip` 分支 ⇒ finally 体内抛错也绕回自己。
        uint8_t* tcatch = frame->catch_ip ? frame->catch_ip : frame->prev_catch_ip;
        uint8_t* tfin = frame->finally_ip ? frame->finally_ip : frame->prev_finally_ip;
        if (tcatch || tfin) {
            // 找到了有异常处理的帧，需要展开调用栈到这个帧
            // 释放当前帧到目标帧之间的所有帧的locals（仅当使用动态分配时才需要 free）
            for (int j = vm.frame_cnt - 1; j > i; j--) {
                CallFrame* unwind_frame = &vm.frames[j];
                if (unwind_frame->locals && unwind_frame->locals_is_dynamic) {
                    free(unwind_frame->locals);
                    unwind_frame->locals = NULL;
                }
            }
            // 设置帧计数为目标帧
            vm.frame_cnt = i + 1;

            // 恢复栈指针到目标帧的stack_base
            vm.sp = frame->stack_base;

            // 处理异常（分支顺序与 OP_THROW 逐条对齐）
            if (frame->catch_finally_ip && !frame->in_finally) {
                // catch 体内再次抛错：先执行**本层** finally，再向外传播
                vm.pending_exception = 1;
                frame->ip = frame->catch_finally_ip;
                frame->catch_finally_ip = NULL;
                vm.has_exception = 0;
            } else if (frame->finally_ip && !frame->in_finally && !frame->catch_ip) {
                // 有 finally、无 catch：先跑 finally（标记异常待续传播）
                vm.pending_exception = 1;
                frame->ip = frame->finally_ip;
                vm.has_exception = 0;
            } else if (tcatch) {
                // 跳转到 catch 块（寄存器式：异常值走 vm.exception，OP_CATCH 负责取值）
                frame->ip = tcatch;
                vm.has_exception = 0;
                // ★ 接住后注销所用 handler：否则它会把本帧后续的运行期错误反复接走（见上面 ②）
                if (frame->catch_ip) frame->catch_ip = NULL;
                else frame->prev_catch_ip = NULL;
            } else {
                vm.pending_exception = 1;
                frame->ip = tfin;
                vm.has_exception = 0;
            }
            return 1;
        }
    }

    // 没有找到任何异常处理
    if (host_floor > 0) {
        // ★ 被**宿主回调边界**拦住（f6）⇒ 这不是"没人接"，而是"接它的人在外层"：
        //   不派发也不报未捕获，保持 has_exception，交回 C 侧（arrays / FFI 宿主）
        //   沿 C 栈往外传；外层解释器在 OP_MODULE_CALL / OP_CLIB_CALL 的边界处
        //   再调一次本函数（那时 host_floor 已恢复）就能找到调用方的 catch ✓
        vm.has_exception = 1;
        return 0;
    }
    // ★ 在协程里**不打印**"未处理"：异常按设计交给 asyncs.run() 收集后
    //   再抛回调用方（谁捕获谁报，没捕获才由最外层报）。这里照打会出现
    //   "既说未处理、又被 run() 抛出来处理"的双重报告。
    if (!vm.current_coroutine) {
        report_unhandled_exception(line);
    } else {
        // 协程内：只登记一条（保持原行为 —— **不**标记 printed，交给 asyncs.run 报）
        char* m = exception_msg_dup();
        runtime_error(m ? m : "未知异常", line);
        free(m);
    }
    vm.has_exception = 1;
    return 0;
}

// 抛出运行时异常（用于 try-catch 捕获）
// 设置异常状态并跳转到 catch/finally 块
// 返回：1 = 异常已被 catch/finally 接管（调用方重取 frame 后继续 DISPATCH）
//       0 = 未捕获（调用方应停机返回 -1）
static inline int throw_runtime_error(const char* msg, int line) {
    // ★ 统一错误文案（2026-10-02）：**引擎**产生的错误一律自带「位置 + 调用栈」，
    //   与 throw_runtime_error_with_operand(s) 的丰富文案同形。此前原生模块
    //   （native_throw_error，如 `_int("12\r")`、数组越界）只登记一句**裸消息**，
    //   于是 `catch e` 拿到的 `e.msg` 没有位置 —— 而 `print(e)` / `"…" + e` 只渲染
    //   `e.msg` ⇒ 用户看到的是一句"凭空"的错（实测：PvZ 的「渲染异常: 无法将字符串
    //   转换为整数」，位置其实躺在 `e.line`/`e.file` 里却没人打印）。
    //   ⚠ 用户自己 `throw "xxx"` 的字符串**不动**（那是用户数据，位置同样在
    //     `e.line`/`e.file`/`e.stack` 里）—— 见 wrap_string_exception。
    char unified_buf[BUFFER_XXLARGE];
    if (!strstr(msg, "[位置]")) {
        const char* u_func = "<main>";
        const char* u_file = "unknown";
        int u_off = 0, u_src = 0;
        get_current_context(&u_func, &u_off, &u_src, &u_file);
        // 行号以调用方传入的 line 为准（它是 GET_CURRENT_LINE() 算出来的当前位置），
        // 只在 line 非法时才退回上下文里的行号
        int up = snprintf(unified_buf, sizeof(unified_buf),
                          "%s\n  [位置] %s:%d (func: %s)\n",
                          msg, u_file, line > 0 ? line : u_src, u_func);
        append_stack_trace(unified_buf, up, sizeof(unified_buf));
        msg = unified_buf;
    }

    // 创建异常对象（使用字典表示，包含 msg、line、file、stack 属性）
    ObjDict* err_dict = dict_new(8);
    if (!err_dict) {
        // 内存分配失败，直接报错
        runtime_error(msg, line);
        return 0;
    }

    // 添加 msg 属性
    ObjString* msg_key = str_copy("msg", 3);
    ObjString* msg_val = str_copy(msg, strlen(msg));
    if (!msg_key || !msg_val) {
        runtime_error(msg, line);
        return 0;
    }
    dict_set(err_dict, val_obj((Object*)msg_key), val_obj((Object*)msg_val));

    // 添加 line 属性
    ObjString* line_key = str_copy("line", 4);
    if (!line_key) {
        runtime_error(msg, line);
        return 0;
    }
    dict_set(err_dict, val_obj((Object*)line_key), val_int(line));

    // 添加 file 属性（当前文件名）
    CallFrame* current_frame = &vm.frames[vm.frame_cnt - 1];
    const char* filename = current_frame->chunk->filename ? current_frame->chunk->filename : "unknown";
    ObjString* file_key = str_copy("file", 4);
    ObjString* file_val = str_copy(filename, strlen(filename));
    if (file_key && file_val) {
        dict_set(err_dict, val_obj((Object*)file_key), val_obj((Object*)file_val));
    }

    // 添加 stack 属性（调用栈，每帧包含 "func_name (file:line)" 信息）
    ObjArray* stack_arr = build_stack_trace();
    if (stack_arr) {
        ObjString* stack_key = str_copy("stack", 5);
        if (stack_key) {
            dict_set(err_dict, val_obj((Object*)stack_key), val_obj((Object*)stack_arr));
        }
    }

    // 设置异常状态
    vm.exception = val_obj((Object*)err_dict);
    vm.has_exception = 1;
    vm.exception_line = line;  // 保存异常发生时的行号

    return dispatch_exception(line);
}

// 抛出一个**已有的**异常值（跨边界带回来的原始异常）
//   · 已是异常对象（Dict）：**原样**派发 —— 保留它自己的 msg/line/file/stack
//     （位置是错误**真正发生**的地方，不该被改写成"接住它的那一行"）
//   · 字符串：按统一文案处理（位置取这里；裸字符串本身没有位置可听）
//   · 其它裸值：字符串化后同上
// 返回同 throw_runtime_error：1 = 已接管；0 = 未捕获（调用方应 return -1）
static inline int throw_existing_exception(Value exc, int line) {
    if (val_is_string(exc)) {
        return throw_runtime_error(((ObjString*)val_as_obj(exc))->chars, line);
    }
    if (val_is_obj(exc) && val_as_obj(exc)->type == OBJ_DICT) {
        vm.exception = exc;
        vm.has_exception = 1;
        int own = exception_own_line();
        if (own <= 0) own = line;
        vm.exception_line = own;
        return dispatch_exception(own);
    }
    char* m = value_to_string(exc);
    int handled = throw_runtime_error(m ? m : "未知异常", line);
    free(m);
    return handled;
}

// 原生函数返回后若挂起了异常：把它接进统一的异常派发
//   ★ 06_call.inc 的三处 `if (vm.has_exception)` 共用这一份 —— 此前那三处各写一遍
//     "取字符串 → error_clear → throw_runtime_error"，把**已经是异常对象**的异常
//     （协程经 asyncs.run() 抛回、FFI 回调带回来的）也 flatten 成字符串再按"原生调用点"
//     重造 ⇒ 原始 msg/位置/栈全丢（实测：协程 throw 的位置被指到 asyncs.run() 那行）✗
//   · 已是异常对象 ⇒ 原样保留（见 throw_existing_exception）
//   · native_throw_error 的裸字符串 ⇒ 按**调用点**造统一文案（位置 + 调用栈）
// 返回同 throw_runtime_error：1 = 已接管；0 = 未捕获（调用方应 return -1）
static inline int throw_pending_exception(int line) {
    error_clear();
    if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_DICT) {
        return throw_existing_exception(vm.exception, line);
    }
    char* msg = NULL;
    if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
        msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
    } else {
        msg = value_to_string(vm.exception);
    }
    int handled = throw_runtime_error(msg ? msg : "未知异常", line);
    free(msg);
    return handled;
}

// ============================================================================
// 带操作数上下文的异常抛出函数
// ============================================================================

// 操作码人性化名称映射
// op_name 是调用方传入的内部操作码名称（如 "ADD (+)"、"INC_LOCAL_NOPUSH (++)"）
// 返回对应的中文人性化描述
static inline const char* op_name_to_human(const char* op_name) {
    if (!op_name) return "未知操作";
    // 按操作类型匹配关键词
    if (strstr(op_name, "ADD"))       return "加法 (+)";
    if (strstr(op_name, "SUB"))      return "减法 (-)";
    if (strstr(op_name, "MUL"))      return "乘法 (*)";
    if (strstr(op_name, "DIV_INT"))  return "整数除法 (/)";
    if (strstr(op_name, "DIV_FLOAT"))return "浮点除法 (/)";
    if (strstr(op_name, "DIV"))      return "除法 (/)";
    if (strstr(op_name, "MOD_INT"))  return "取模 (%)";
    if (strstr(op_name, "MOD"))      return "取模 (%)";
    if (strstr(op_name, "BITAND"))   return "按位与 (&)";
    if (strstr(op_name, "BITOR"))    return "按位或 (|)";
    if (strstr(op_name, "BITXOR"))   return "按位异或 (^)";
    if (strstr(op_name, "BITNOT"))   return "按位非 (~)";
    if (strstr(op_name, "SHL_IMM"))  return "左移 (<<)";
    if (strstr(op_name, "SHL"))      return "左移 (<<)";
    if (strstr(op_name, "SHR_IMM"))  return "右移 (>>)";
    if (strstr(op_name, "SHR"))      return "右移 (>>)";
    if (strstr(op_name, "USHR_IMM")) return "逻辑右移 (>>>)";
    if (strstr(op_name, "USHR"))     return "逻辑右移 (>>>)";
    if (strstr(op_name, "PRE_INC"))  return "前缀自增 (++)";
    if (strstr(op_name, "PRE_DEC"))  return "前缀自减 (--)";
    if (strstr(op_name, "INC_LOCAL_NOPUSH")) return "自增 (++)";
    if (strstr(op_name, "DEC_LOCAL_NOPUSH")) return "自减 (--)";
    if (strstr(op_name, "INC_LOCAL"))return "后缀自增 (++)";
    if (strstr(op_name, "DEC_LOCAL"))return "后缀自减 (--)";
    if (strstr(op_name, "INC"))      return "自增 (++)";
    if (strstr(op_name, "DEC"))      return "自减 (--)";
    if (strstr(op_name, "NEG"))      return "取负 (-)";
    if (strstr(op_name, "CAST_FLOAT")) return "类型转换 (→float)";
    if (strstr(op_name, "CAST_INT"))   return "类型转换 (→int)";
    return op_name;  // 未知操作码，原样返回
}

// 获取当前帧的源码位置信息
// 将 ip_offset 映射回 chunk->lines 的行号，并获取文件名和函数名
static inline void get_current_context(
    const char** func_name, int* ip_offset,
    int* src_line, const char** filename) {

    *func_name = "<main>";
    *ip_offset = 0;
    *src_line = 0;
    *filename = "unknown";

    if (vm.frame_cnt > 0) {
        CallFrame* frame = &vm.frames[vm.frame_cnt - 1];
        // 函数名
        if (frame->closure && frame->closure->function) {
            if (frame->closure->function->name) {
                *func_name = frame->closure->function->name;
            } else {
                *func_name = "<anonymous>";
            }
        }
        // 文件名
        if (frame->chunk && frame->chunk->filename) {
            *filename = frame->chunk->filename;
        }
        // IP 偏移和源码行号
        // ⚠ 同 build_stack_trace：ip 指向下一条指令 ⇒ 本指令按 ip-4 取，
        //   否则 [位置] 报的是下一条指令的行号（与 e.line 打架）
        if (frame->chunk && frame->chunk->code && frame->ip) {
            *ip_offset = (int)(frame->ip - frame->chunk->code) - 4;
            if (*ip_offset < 0) *ip_offset = 0;
            if (*ip_offset < frame->chunk->len && frame->chunk->lines) {
                *src_line = frame->chunk->lines[*ip_offset];
            }
        }
    }
}

// 构建调用栈回溯字符串（追加到 err_buf 中 pos 位置之后）
// 格式：
//   调用栈:
//     #0 main @ test.leno:12
//     #1 foo  @ test.leno:8
static inline int append_stack_trace(char* buf, int pos, size_t buf_size) {
    if (vm.frame_cnt <= 0) return pos;

    pos += snprintf(buf + pos, buf_size - pos, "  [调用栈]\n");

    for (int i = vm.frame_cnt - 1; i >= 0; i--) {
        CallFrame* f = &vm.frames[i];

        // 函数名
        const char* fn = "<anonymous>";
        if (f->closure && f->closure->function && f->closure->function->name) {
            fn = f->closure->function->name;
        } else if (i == 0) {
            fn = "<main>";
        }

        // 文件名
        const char* file = "unknown";
        if (f->chunk && f->chunk->filename) {
            file = f->chunk->filename;
        }

        // 行号（同 build_stack_trace：ip 指向下一条指令 ⇒ 按 ip-4 取本指令行，
        //   否则栈里的行号整体偏一行，与 e.line / [位置] 不一致 ✗）
        int line = 0;
        if (f->chunk && f->chunk->lines && f->chunk->code && f->ip) {
            int offset = (int)(f->ip - f->chunk->code) - 4;
            if (offset < 0) offset = 0;
            if (offset < f->chunk->len) {
                line = f->chunk->lines[offset];
            }
        }

        pos += snprintf(buf + pos, buf_size - pos,
            "    #%d %s @ %s:%d\n",
            vm.frame_cnt - 1 - i, fn, file, line);

        // 限制最多 16 层，避免极深递归时输出过长
        if (vm.frame_cnt - 1 - i >= 15) {
            pos += snprintf(buf + pos, buf_size - pos,
                "    ... (省略 %d 帧)\n", vm.frame_cnt - 1 - i - 15 + 1);
            break;
        }
    }
    return pos;
}

// 带两个操作数上下文的异常抛出
static inline int throw_runtime_error_with_operands(
    const char* msg, const char* op_name,
    Value a_val, Value b_val, int line) {

    char err_buf[BUFFER_XXLARGE];
    char a_str[128], b_str[128];
    const char* func_name = "<main>";
    const char* filename = "unknown";
    int ip_offset = 0, src_line = 0;
    get_current_context(&func_name, &ip_offset, &src_line, &filename);

    value_to_short_str(a_val, a_str, sizeof(a_str));
    value_to_short_str(b_val, b_str, sizeof(b_str));

    int pos = snprintf(err_buf, sizeof(err_buf),
        "%s\n"
        "  [操作] %s\n"
        "  [内部] %s\n"
        "  [位置] %s:%d (func: %s)\n"
        "  [操作数a] type=%s, value=%s\n"
        "  [操作数b] type=%s, value=%s\n",
        msg,
        op_name_to_human(op_name), op_name,
        filename, src_line > 0 ? src_line : line, func_name,
        value_type_desc(a_val), a_str,
        value_type_desc(b_val), b_str);

    // 添加调用栈回溯
    pos = append_stack_trace(err_buf, pos, sizeof(err_buf));

    return throw_runtime_error(err_buf, line);
}

// 带单个操作数上下文的异常抛出
static inline int throw_runtime_error_with_operand(
    const char* msg, const char* op_name,
    Value a_val, int line) {

    char err_buf[BUFFER_XXLARGE];
    char a_str[128];
    const char* func_name = "<main>";
    const char* filename = "unknown";
    int ip_offset = 0, src_line = 0;
    get_current_context(&func_name, &ip_offset, &src_line, &filename);

    value_to_short_str(a_val, a_str, sizeof(a_str));

    int pos = snprintf(err_buf, sizeof(err_buf),
        "%s\n"
        "  [操作] %s\n"
        "  [内部] %s\n"
        "  [位置] %s:%d (func: %s)\n"
        "  [操作数] type=%s, value=%s\n",
        msg,
        op_name_to_human(op_name), op_name,
        filename, src_line > 0 ? src_line : line, func_name,
        value_type_desc(a_val), a_str);

    // 添加调用栈回溯
    pos = append_stack_trace(err_buf, pos, sizeof(err_buf));

    return throw_runtime_error(err_buf, line);
}
