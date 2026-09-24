    LABEL(OP_GET_FIELD): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj = R(b);
        int line = GET_CURRENT_LINE();
        if (unlikely(!val_is_obj(obj))) {
            if (!throw_runtime_error("字段访问需要结构体对象", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        // cstruct（C 布局）字段读：数组/嵌套字段返回视图（见 vm_cstruct_field_read）
        if (val_as_obj(obj)->type == OBJ_CSTRUCT) {
            ObjCStruct* co = (ObjCStruct*)val_as_obj(obj);
            if (unlikely(c < 0 || c >= co->def->field_count)) {
                if (!throw_runtime_error("cstruct 字段索引越界", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = vm_cstruct_field_read(co, c);
            DISPATCH();
        }
        if (unlikely(val_as_obj(obj)->type != OBJ_STRUCT)) {
            if (!throw_runtime_error("字段访问需要结构体对象", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjStruct* so = (ObjStruct*)val_as_obj(obj);
        if (unlikely(c < 0 || c >= so->def->field_count)) {
            if (!throw_runtime_error("字段索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = struct_get_field(so, c);
        DISPATCH();
    }
    // OP_GET_FIELD_FAST A B C: R[A] = R[B].field(C)
    //   —— 静态类型特化的字段读（对齐栈式 OP_GET_FIELD_FAST）：codegen 已核实 R[B] 的静态
    //   类型是 struct（非 cstruct）且 C 合法 ⇒ 省掉 cstruct 分流与 struct_get_field() 调用，
    //   直接读 field_values。仍保留 tag/类型/越界三项轻检查 ⇒ 报错口径与 OP_GET_FIELD 一致。
    LABEL(OP_GET_FIELD_FAST): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj = R(b);
        int line = GET_CURRENT_LINE();
        if (unlikely(!val_is_obj(obj) || val_as_obj(obj)->type != OBJ_STRUCT)) {
            if (!throw_runtime_error("字段访问需要结构体对象", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjStruct* so = (ObjStruct*)val_as_obj(obj);
        if (unlikely(c < 0 || c >= so->def->field_count)) {
            if (!throw_runtime_error("字段索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = so->field_values[c];
        DISPATCH();
    }
    // OP_ACC_FIELDS A B C: R[A] = R[B].field(idx[0]) + ... + R[B].field(idx[C-1])
    //   —— 多字段累加融合（对齐栈式 OP_ACC_FIELDS）：紧随指令 C 个字节的字段索引。
    //   语义与「逐字段 OP_ADD_F 连加」逐位等价：double 左到右累加 + val_float。
    //   ⚠ 尾随字节必须在**任何分支之前**走完（否则错误路径会把索引当指令执行）。
    LABEL(OP_ACC_FIELDS): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj = R(b);
        int line = GET_CURRENT_LINE();
        frame->ip += c;                       // 跳过尾随的字段索引字节
        const uint8_t* idx = frame->ip - c;
        if (unlikely(!val_is_obj(obj) || val_as_obj(obj)->type != OBJ_STRUCT)) {
            if (!throw_runtime_error("字段访问需要结构体对象", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjStruct* so = (ObjStruct*)val_as_obj(obj);
        double sum = 0.0;
        for (int i = 0; i < c; i++) {
            int fi = (int)idx[i];
            if (unlikely(fi >= so->def->field_count)) {
                if (!throw_runtime_error("字段索引越界", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            sum += val_as_num_ex(so->field_values[fi]);
        }
        R(a) = val_float(sum);
        DISPATCH();
    }
    // OP_SET_FIELD A B C: R[B].field(C) = R[A]
    LABEL(OP_SET_FIELD): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj = R(b);
        int line = GET_CURRENT_LINE();
        if (unlikely(!val_is_obj(obj))) {
            if (!throw_runtime_error("字段赋值需要结构体对象", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        // cstruct（C 布局）字段写：数值类型自动提升
        if (val_as_obj(obj)->type == OBJ_CSTRUCT) {
            ObjCStruct* co = (ObjCStruct*)val_as_obj(obj);
            if (unlikely(c < 0 || c >= co->def->field_count)) {
                if (!throw_runtime_error("cstruct 字段索引越界", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            Value nv = R(a);
            TypeKind ft = co->def->fields[c].type;
            if (val_is_int(nv) && (ft == TYPE_F32 || ft == TYPE_F64)) {
                nv = val_float((double)val_as_int(nv));
            } else if (val_is_bigint(nv) && (ft == TYPE_F32 || ft == TYPE_F64)) {
                nv = val_float(bigint_to_double(val_as_bigint(nv)));
            } else if (val_is_float(nv) && (ft >= TYPE_I8 && ft <= TYPE_U64)) {
                nv = val_num(val_as_num(nv));
            }
            cstruct_set_field_value(co, c, nv);
            DISPATCH();
        }
        if (unlikely(val_as_obj(obj)->type != OBJ_STRUCT)) {
            if (!throw_runtime_error("字段赋值需要结构体对象", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjStruct* so = (ObjStruct*)val_as_obj(obj);
        if (unlikely(c < 0 || c >= so->def->field_count)) {
            if (!throw_runtime_error("字段索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        struct_set_field(so, c, R(a));
        DISPATCH();
    }
    // OP_GET_METHOD A B C: R[A] = R[B].方法名(K[C]) —— 内置类型 → 绑定原生方法；
    //   struct → 绑定脚本方法闭包；dict → 退化为按键取属性。
    LABEL(OP_GET_METHOD): {
        // 调用点标识（读操作数之前取）—— 内联缓存的索引用它
        uint8_t* ip_before = frame->ip;
        int a = READ_A(), b = READ_B();
        int cidx = READ_CONST_IDX(READ_C());
        int line = GET_CURRENT_LINE();
        if (unlikely(cidx < 0 || cidx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("方法名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[cidx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("方法名必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* name = (ObjString*)val_as_obj(name_val);
        Value receiver = R(b);
        ObjType rt = get_receiver_type(receiver);

        if (ic_type_supported(rt)) {
            // ★ 内联缓存：此前 vm.ic_cache 是**死字段** —— leno_vm.h 里定义了、
            //   vm_init.inc 里初始化了、vm_ic.inc 里连 ic_cache_index() 都写好了，
            //   但**全仓库没有一处读写它**。于是每次 `arr.len()` / `str.len()` /
            //   `d.has(x)` 都要 find_method_by_type() 查一次方法表（哈希 + strcmp）。
            //   键 =（调用点、receiver 类型、方法名哈希），值 = ObjNative*。
            //   与栈式 op_property.inc 同口径（那边主线程分支也是按调用点缓存 method）。
            //   安全性：方法表里的 ObjNative 由各自 *_mark_methods() 作为 GC 根标记，
            //   且本 GC 非移动式 ⇒ 裸指针稳定。
            InlineCacheEntry* ic = &vm.ic_cache[ic_cache_index(ip_before)];
            uint32_t mhash = name->hash;
            ObjNative* method;
            if (likely(ic->valid && ic->receiver_type == rt && ic->name_hash == mhash)) {
                vm.ic_hits++;
                method = ic->method;
            } else {
                vm.ic_misses++;
                method = find_method_by_type(rt, name->chars);
                if (method) {
                    ic->valid = 1;
                    ic->receiver_type = (int)rt;
                    ic->name_hash = mhash;
                    ic->method = method;
                }
            }
            if (method) {
                // ★ 窥孔优化：若**下一条就是 OP_CALL、且调用的正是本寄存器**，
                //   就不必构造 ObjBoundMethod（每次调用一次 GC 分配），
                //   直接把 receiver 当第 0 个实参调原生函数，并就地吃掉那条 OP_CALL。
                //   与栈式 op_property.inc 的窥孔同口径（那边同样检测 OP_CALL 后直接
                //   `method->function(arg_count + 1, ...)`，不经过 bound method）。
                //   实测 examples/性能测试/内联缓存 (Inline Caching) 性能对比测试.leno：
                //   只接 ic_cache 时 671ms（栈式 92ms，7.3x），窥孔后见提交信息。
                //   边界：只在无 EXTEND 前缀（a <= 255）、单返回值（C == 2，即
                //   nresults == 1）时才走这条快路；其余形态原样退回分配 bound method。
                if (likely(a <= 255 && *frame->ip == (uint8_t)OP_CALL &&
                           frame->ip[1] == (uint8_t)a && frame->ip[3] == 2)) {
                    int nargs = (int)frame->ip[2] - 1;
                    frame->ip += 4;              // 吃掉 OP_CALL
                    int base = vm.sp;
                    vm_stack_push(&vm, receiver);
                    for (int i = 0; i < nargs; i++) vm_stack_push(&vm, R(a + 1 + i));
                    Value result = method->function(nargs + 1, vm.stack + base);
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
                ObjBoundMethod* bound = bound_method_new(receiver, method);
                if (unlikely(!bound)) { runtime_error("内存分配失败", line); return -1; }
                R(a) = val_obj((Object*)bound);
                DISPATCH();
            }
        }

        if (rt == OBJ_STRUCT) {
            ObjStruct* so = (ObjStruct*)val_as_obj(receiver);
            ObjStructDef* def = so->def;
            ObjClosure* pre = NULL;
            ObjFunction* fn = NULL;

            // ★ 内联缓存：此前 vm.ic_method_cache 是**死字段**（leno_vm.h 里定义了、
            //   vm_init.inc 里初始化了，本 handler 从没读写过）⇒ 每次方法调用都要
            //   struct_method_lookup() **线性 strcmp 遍历方法表**。
            //   键 =（调用点、def 指针、方法名哈希、struct 定义表代数）；
            //   值 = **预创建方法闭包**。与栈式 op_struct.inc 同口径
            //   （它同样按调用点索引 + 比 struct_def_generation()）。
            //   只缓存预创建闭包（无 upvalue 的方法）；带 upvalue 的方法每次都要新建
            //   闭包，不缓存 —— 与 leno_vm.h 里该结构体的注释一致。
            //   安全性：ObjStructDef 是 GC 根（struct_def_mark_all），
            //   def->methods[i].closure 经 def 可达，且本 GC 非移动式 ⇒ 裸指针稳定。
            InlineMethodCacheEntry* mc = NULL;
            uint32_t mgen = 0;
            ObjClosure* cl = NULL;   // ★ 取到的方法闭包（IC 命中 / 查表结果），收尾时统一用
            if (def) {
                mgen = struct_def_generation();
                int cache_idx = (int)(((uintptr_t)ip_before >> 2) & (IC_METHOD_CACHE_SIZE - 1));
                mc = &vm.ic_method_cache[cache_idx];
                if (likely(mc->valid && mc->def == def && mc->name_hash == name->hash &&
                           mc->gen == mgen)) {
                    vm.ic_hits++;
                    cl = mc->closure;
                } else {
                    vm.ic_misses++;
                }
            }

            if (!cl && def && struct_method_lookup(def, name, &pre, &fn)) {
                if (pre && mc) {   // 只缓存预创建闭包（upvalue 方法不缓存）
                    mc->valid = 1;
                    mc->def = def;
                    mc->name_hash = name->hash;
                    mc->gen = mgen;
                    mc->closure = pre;
                }
                cl = pre;
                if (!cl) {
                    cl = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                    if (unlikely(!cl)) { runtime_error("内存分配失败", line); return -1; }
                    cl->function = fn;
                    cl->upvalue_count = fn->upvalue_count;
                    cl->type_param_count = 0;
                    cl->type_param_args = NULL;
                    for (int ui = 0; ui < fn->upvalue_count; ui++) cl->upvalues[ui] = NULL;
                }
            }

            if (cl) {
                // ★ struct 方法调用的**窥孔**（⑤-ab）—— 与上文"内置类型方法"那条窥孔
                //   同口径（条件逐字一致：a <= 255 即无 EXTEND 前缀、下一条就是 OP_CALL、
                //   同一基址 a、C == 2 即单返回值）：
                //   紧邻的 OP_CALL 一定会消费 R[a] ⇒ 无需把方法值"物化"成绑定对象，
                //   直接按 OP_INVOKE_METHOD_TYPED 那套做（receiver 去重 + call_reg）。
                //   为什么值得：face / 静态类型未知的接收者走的就是 GET_METHOD + CALL 这条
                //   两指令形状（codegen 不为它们发融合指令），实测 **95.7ns/次**，而每轮那次
                //   ObjBoundMethod 分配（64B + GC 压力）是主要开销；融合路径只要 38.2ns。
                //   ⚠ async 方法**不走窥孔**：它要"调用即建协程"，退回下面分配绑定对象，
                //     由 OP_CALL 的绑定分支走 vm_start_async_call（见 06_call.inc）。
                if (likely(a <= 255 && !(cl->function && cl->function->is_async) &&
                           *frame->ip == (uint8_t)OP_CALL && frame->ip[1] == (uint8_t)a &&
                           frame->ip[3] == 2)) {
                    int nargs = (int)frame->ip[2] - 1;
                    frame->ip += 4;              // 吃掉那条 OP_CALL
                    // 实参取法与 OP_INVOKE_METHOD_TYPED / OP_CALL 绑定分支**逐字同口径**：
                    //   语义分析对 struct 方法调用会把 receiver 放进实参首位，而"值相等"
                    //   会被自引用骗到（`a.add(a)`）⇒ 再用形参个数佐证。
                    int self_in_args = 0;
                    int expect_arity = cl->function ? cl->function->arity : 0;
                    if (nargs > 0) {
                        Value a0 = R(a + 1);
                        if (val_is_obj(a0) && val_is_obj(receiver)) {
                            self_in_args = (val_as_obj(a0) == val_as_obj(receiver));
                        } else if (!val_is_obj(a0) && !val_is_obj(receiver)) {
                            self_in_args = (a0 == receiver);
                        }
                        if (self_in_args && expect_arity > 0 && nargs != expect_arity &&
                            nargs + 1 == expect_arity) {
                            self_in_args = 0;   // 个数表明调用方没传 receiver ⇒ 自引用误判
                        }
                    }
                    // 窥孔条件已保证 nresults == 1（C == 2）⇒ 结果直接写回 R[a]
                    if (likely(call_reg(cl, self_in_args ? 0 : 1, receiver, a + 1, nargs, a, 1, line))) {
                        frame = &vm.frames[vm.frame_cnt - 1];
                        DISPATCH();
                    }
                    if (!vm.has_exception) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                // 窥孔不适用（多返回值 / 跨 255 寄存器 / async）⇒ 照旧物化绑定对象
                ObjBoundMethod* bound = bound_closure_method_new(receiver, cl);
                if (unlikely(!bound)) { runtime_error("内存分配失败", line); return -1; }
                R(a) = val_obj((Object*)bound);
                DISPATCH();
            }
            // ★ 没有同名方法：退化为"取同名字段"—— 支持 `func():int cb` 这种**函数类型字段**
            //   的 `obj.cb()` 调用（字段里就是一等函数值，CALL 直接调它）。
            //   否则 `self.on_click()` 会误报「类型 object 上不存在方法 'on_click'」✗
            if (def) {
                int fi = struct_get_field_index(def, name->chars);
                if (fi >= 0) {
                    R(a) = struct_get_field(so, fi);
                    DISPATCH();
                }
            }
        }

        if (rt == OBJ_DICT) {
            R(a) = dict_get((ObjDict*)val_as_obj(receiver), name_val);
            DISPATCH();
        }

        // cstruct 定义 / 实例 / 实例数组：malloc / size / free 等方法来自 C 侧方法表
        //   （`v.free()` 里 v 是 malloc_array 得到的 ObjCStructArray，也要能取到方法）
        if (rt == OBJ_CSTRUCT_DEF || rt == OBJ_CSTRUCT || rt == OBJ_CSTRUCT_ARRAY ||
            rt == OBJ_CSTRUCT_ARRAY_VIEW) {
            extern ObjNative* cstruct_find_method(const char* name);
            ObjNative* native_method = cstruct_find_method(name->chars);
            if (unlikely(!native_method)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "cstruct 不存在方法 '%s'", name->chars);
                if (!throw_runtime_error(msg, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            // 实例方法需要绑定 receiver（native 调用约定：args[0] = receiver）
            ObjType receiver_type = val_as_obj(receiver)->type;
            if (receiver_type != OBJ_CSTRUCT_DEF) {
                ObjBoundMethod* bound = (ObjBoundMethod*)gc_alloc(sizeof(ObjBoundMethod), OBJ_BOUND_METHOD);
                if (unlikely(!bound)) { runtime_error("内存分配失败", line); return -1; }
                bound->receiver = receiver;
                bound->method = native_method;
                bound->closure = NULL;
                R(a) = val_obj((Object*)bound);
                DISPATCH();
            }
            R(a) = val_obj((Object*)native_method);
            DISPATCH();
        }

        // .leno 模块：成员（变量 / 函数 / 类型）都在 exports 字典里
        if (rt == OBJ_MODULE) {
            Value member = dict_get(((ObjModule*)val_as_obj(receiver))->exports, name_val);
            if (unlikely(val_is_null(member))) {
                char msg[256];
                snprintf(msg, sizeof(msg), "模块中不存在成员 '%s'", name->chars);
                if (!throw_runtime_error(msg, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = member;
            DISPATCH();
        }

        {
            char msg[256];
            snprintf(msg, sizeof(msg), "类型 %s 上不存在方法 '%s'", obj_type_to_name(rt), name->chars);
            if (!throw_runtime_error(msg, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
    }
    // OP_INVOKE_METHOD_TYPED A B C —— struct 方法「取方法 + 调用」融合
    //   （对齐栈式同名指令）。等价于「OP_GET_METHOD + OP_CALL」，但省掉：
    //     ① 一次指令派发；
    //     ② 每次调用一次 ObjBoundMethod 分配（老路径 bound_closure_method_new）；
    //     ③ OP_CALL 里"receiver 是否已在实参首位"的去重判断。
    //   操作数：A = 基址（R[A] = receiver，实参在 R[A+1 .. A+B]），
    //           B = 实参个数（与 OP_CALL 的 B 同口径），C = 保留；
    //           紧随 4 字节：方法名常量(16) + 静态类型名常量(16)。
    //   VM 侧仍按**运行时实际类型**分发 —— 类型名常量只读掉、不参与查找（与栈式一致），
    //   语义与老路径一致；它留着给将来的编译期去虚拟化。
    //   ⚠ codegen 只在「接收者静态类型是 struct、该方法是脚本方法且非 async」时发本指令。
    LABEL(OP_INVOKE_METHOD_TYPED): {
        uint8_t* ip_before = frame->ip;      // 调用点标识（IC 索引用，指向本指令的 op 字节）
        int a = READ_A(), argc = READ_B();
        int cidx = ((int)frame->ip[0] << 8) | (int)frame->ip[1];
        frame->ip += 4;                      // 走完"方法名 + 类型名"两个字
        int line = GET_CURRENT_LINE();
        if (unlikely(cidx < 0 || cidx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("方法名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[cidx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("方法名必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* name = (ObjString*)val_as_obj(name_val);
        Value receiver = R(a);
        ObjType rt = get_receiver_type(receiver);
        // 静态类型已由 codegen 核实是 struct；运行期不是 struct 时报错口径与老路径一致
        // （老路径走 OP_GET_METHOD 的兜底分支，报的就是这句话）。
        if (unlikely(rt != OBJ_STRUCT)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "类型 %s 上不存在方法 '%s'", obj_type_to_name(rt), name->chars);
            if (!throw_runtime_error(msg, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjStruct* so = (ObjStruct*)val_as_obj(receiver);
        ObjStructDef* def = so->def;
        ObjClosure* cl = NULL;

        // 与 OP_GET_METHOD 共用同一张 ic_method_cache（键含 def 指针 + 方法名哈希 + 定义表代数）
        uint32_t mgen = struct_def_generation();
        int cache_idx = (int)(((uintptr_t)ip_before >> 2) & (IC_METHOD_CACHE_SIZE - 1));
        InlineMethodCacheEntry* mc = &vm.ic_method_cache[cache_idx];
        if (likely(def && mc->valid && mc->def == def && mc->name_hash == name->hash &&
                   mc->gen == mgen)) {
            vm.ic_hits++;
            cl = mc->closure;
        } else {
            vm.ic_misses++;
            ObjClosure* pre = NULL;
            ObjFunction* fn = NULL;
            if (unlikely(!def || !struct_method_lookup(def, name, &pre, &fn))) {
                char msg[256];
                snprintf(msg, sizeof(msg), "类型 '%s' 没有方法 '%s'",
                         (def && def->name) ? def->name : "?", name->chars);
                if (!throw_runtime_error(msg, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            if (pre) {
                // 只缓存预创建闭包（带 upvalue 的方法每次新建，与 OP_GET_METHOD 同口径）
                mc->valid = 1;
                mc->def = def;
                mc->name_hash = name->hash;
                mc->gen = mgen;
                mc->closure = pre;
                cl = pre;
            } else {
                cl = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                if (unlikely(!cl)) { runtime_error("内存分配失败", line); return -1; }
                cl->function = fn;
                cl->upvalue_count = fn->upvalue_count;
                cl->type_param_count = 0;
                cl->type_param_args = NULL;
                for (int ui = 0; ui < fn->upvalue_count; ui++) cl->upvalues[ui] = NULL;
            }
        }

        // ★ 实参取法与 OP_CALL 的绑定方法分支**逐字同口径**（见 06_call.inc）：
        //   语义分析对 struct 方法调用**并不统一**地把 receiver 放进实参首位
        //   （MODULE_CALL 改写的调用不含、AST_CALL 形态含），而且"值相等"会被自引用骗到
        //   （`a.add(a)` 只传 1 个实参却被当成 [receiver]）⇒ 光靠"首实参 == receiver"
        //   不够，还要用形参个数佐证。
        //   ⚠ 这里绝不能按"R[a+1] 就是 self 副本"的假设直接跳过 —— 实测那样会让被调方
        //     实参整体错位（光线追踪 Phase B/C 的 hits 从 900000/585500 变成 0）。
        int nargs = argc;
        int self_in_args = 0;
        int expect_arity = cl->function ? cl->function->arity : 0;
        if (nargs > 0) {
            Value a0 = R(a + 1);
            if (val_is_obj(a0) && val_is_obj(receiver)) {
                self_in_args = (val_as_obj(a0) == val_as_obj(receiver));
            } else if (!val_is_obj(a0) && !val_is_obj(receiver)) {
                self_in_args = (a0 == receiver);
            }
            if (self_in_args && expect_arity > 0 && nargs != expect_arity &&
                nargs + 1 == expect_arity) {
                self_in_args = 0;   // 个数表明调用方没传 receiver ⇒ 自引用误判
            }
        }
        // （codegen 不为 async 方法发本指令，故这里没有协程分支；nresults 固定 1。）
        int call_ok = call_reg(cl, self_in_args ? 0 : 1, receiver, a + 1, nargs, a, 1, line);
        if (likely(call_ok)) {
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (!vm.has_exception) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    // OP_GET_FIELD_ADDR A B C: R[A] = &R[B].field(C)（仅 cstruct 字段）
    LABEL(OP_GET_FIELD_ADDR): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        int line = GET_CURRENT_LINE();
        Value obj = R(b);
        if (unlikely(!val_is_obj(obj) || val_as_obj(obj)->type != OBJ_CSTRUCT)) {
            if (!throw_runtime_error("& 取地址仅支持 cstruct 字段", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjCStruct* co = (ObjCStruct*)val_as_obj(obj);
        if (unlikely(c < 0 || c >= co->def->field_count)) {
            if (!throw_runtime_error("cstruct 字段索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjFFIPointer* ptr = cstruct_field_addr_new(co, c);
        if (unlikely(!ptr)) {
            runtime_error("内存不足", line);
            return -1;
        }
        R(a) = val_obj((Object*)ptr);
        DISPATCH();
    }
    // OP_SET_PTR_ELEM_TYPE A B: R[A] 若是 FFI 指针，设置其 element_type = (TypeKind)B
    //   由 `Ptr[u32] p = ffi.malloc(...)` 这类**声明**发射（gen_var_decl）。ffi.read_at /
    //   write_at 靠 element_type 算元素宽度，读不到就报「需要 Ptr[T] 类型指针」
    //   （examples/clib/test_read_write_at.leno、test_cfunc_qsort.leno 整份跑不出来）。
    LABEL(OP_SET_PTR_ELEM_TYPE): {
        int a = READ_A(), b = READ_B();
        Value v = R(a);
        if (val_is_obj(v) && val_as_obj(v)->type == OBJ_FFI_POINTER) {
            ObjFFIPointer* p = (ObjFFIPointer*)val_as_obj(v);
            if (!p->freed) p->element_type = (TypeKind)b;
        }
        DISPATCH();
    }
    // OP_SET_DECLARED_FACE A Bx: R[A] 若是 struct 实例，设置 declared_face = K[Bx]
    //   face 声明的变量（`Speaker s = ...`）：type()/数组元素类型推断会把实例当 face 看
    //   （types.c 读 obj->declared_face）。
    LABEL(OP_SET_DECLARED_FACE): {
        int a = READ_A(), bx = READ_Bx();
        Value v = R(a);
        if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRUCT &&
            bx >= 0 && bx < frame->chunk->const_cnt) {
            Value nv = frame->chunk->constants[bx];
            if (val_is_obj(nv) && val_as_obj(nv)->type == OBJ_STRING) {
                ((ObjStruct*)val_as_obj(v))->declared_face = (ObjString*)val_as_obj(nv);
            }
        }
        DISPATCH();
    }

    // --- cstruct ---
    // OP_CSTRUCT_DEF A Bx: R[A] = C 布局结构体定义；Bx = 名字常量索引
    //   紧随元数据（与 codegen 的 gen_cstruct_def 严格同序）：
    //   field_count(1) total_size(2) alignment(1) is_packed(1) explicit_align(1)
    //   fields: name_const(2) type(1) offset(2) array_dim(2) struct_name_const(2)
    //           + PTR_GENERIC 时 elem_type(1)
    LABEL(OP_CSTRUCT_DEF): {
        int a = READ_A();
        int name_idx = READ_Bx();
        uint8_t* ip = frame->ip;
        int field_count = ip[0]; ip += 1;
        int total_size = (ip[0] << 8) | ip[1]; ip += 2;
        int alignment = ip[0]; ip += 1;
        int is_packed = ip[0]; ip += 1;
        int explicit_align = ip[0]; ip += 1;

        if (unlikely(name_idx < 0 || name_idx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("cstruct 名字常量越界", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[name_idx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("cstruct 名称必须是字符串", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* cname = (ObjString*)val_as_obj(name_val);

        ObjCStructDef* def = cstruct_def_new(cname->chars, field_count, total_size, alignment);
        if (unlikely(!def)) {
            runtime_error("cstruct 定义分配失败", GET_CURRENT_LINE());
            return -1;
        }
        if (!def->owner) def->owner = module_frame_current_module();
        def->is_packed = (is_packed != 0);
        def->explicit_align = explicit_align;

        for (int i = 0; i < field_count; i++) {
            int fname_idx = (ip[0] << 8) | ip[1]; ip += 2;
            TypeKind ftype = (TypeKind)ip[0]; ip += 1;
            int foffset = (ip[0] << 8) | ip[1]; ip += 2;
            int fdim = (ip[0] << 8) | ip[1]; ip += 2;
            int sname_idx = (ip[0] << 8) | ip[1]; ip += 2;

            const char* fname = "";
            if (fname_idx >= 0 && fname_idx < frame->chunk->const_cnt) {
                Value fnv = frame->chunk->constants[fname_idx];
                if (val_is_obj(fnv) && val_as_obj(fnv)->type == OBJ_STRING) {
                    fname = ((ObjString*)val_as_obj(fnv))->chars;
                }
            }
            const char* sname = NULL;
            if (ftype == TYPE_CSTRUCT && sname_idx != 0xFFFF &&
                sname_idx >= 0 && sname_idx < frame->chunk->const_cnt) {
                Value snv = frame->chunk->constants[sname_idx];
                if (val_is_obj(snv) && val_as_obj(snv)->type == OBJ_STRING) {
                    sname = ((ObjString*)val_as_obj(snv))->chars;
                }
            }
            int fsize = c_layout_type_size(ftype);
            if (ftype == TYPE_CSTRUCT && sname) {
                ObjCStructDef* nested = cstruct_def_find(sname);
                if (nested) fsize = nested->total_size;
            }
            TypeKind elem = TYPE_PTR;
            if (ftype == TYPE_PTR_GENERIC) { elem = (TypeKind)ip[0]; ip += 1; }

            cstruct_def_set_field(def, i, fname, ftype, foffset, fsize, fdim, sname, elem);
        }
        // 元数据是变长的，ip 必须由我们自己推进
        frame->ip = ip;

        cstruct_def_register(def);
        R(a) = val_obj((Object*)def);
        DISPATCH();
    }
    // OP_GET_CSTRUCT_DEF A Bx: R[A] = cstruct_def_find(K[Bx])
    LABEL(OP_GET_CSTRUCT_DEF): {
        int a = READ_A();
        int name_idx = READ_Bx();
        if (unlikely(name_idx < 0 || name_idx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("cstruct 名字常量越界", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[name_idx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("cstruct 名称必须是字符串", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjCStructDef* def = cstruct_def_find(((ObjString*)val_as_obj(name_val))->chars);
        if (unlikely(!def)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "找不到 cstruct 定义 '%s'",
                     ((ObjString*)val_as_obj(name_val))->chars);
            if (!throw_runtime_error(msg, GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = val_obj((Object*)def);
        DISPATCH();
    }

    // --- 类型检查 / 转换 ---
    // OP_TYPE_CHECK A B C: R[A] = (R[A] is 类型B[元素C])
    //   需要名字的类型（struct/face/enum）在指令后紧跟 2 字节名字常量索引
