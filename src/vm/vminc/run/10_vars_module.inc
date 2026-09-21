    LABEL(OP_LOAD_NATIVE_MODULE): {
        int a = READ_A();
        int bx = READ_Bx();
        int line = GET_CURRENT_LINE();
        if (unlikely(bx < 0 || bx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("模块名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value mv = frame->chunk->constants[bx];
        if (unlikely(!val_is_obj(mv) || val_as_obj(mv)->type != OBJ_STRING)) {
            if (!throw_runtime_error("模块名必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        const char* module_name = ((ObjString*)val_as_obj(mv))->chars;
        if (native_init_module(module_name) != 0) {
            char nmsg[256];
            snprintf(nmsg, sizeof(nmsg), "未知的原生模块: %s", module_name);
            if (!throw_runtime_error(nmsg, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = val_null();
        DISPATCH();
    }
    // OP_MODULE_CALL A B C: R[A] = 模块方法(R[A+1 .. A+C])
    //   B = 常量表中 "模块名\0方法名" 组合串的索引（0/>255 时紧随 EXTRAARG）
    LABEL(OP_MODULE_CALL): {
        // 调用点标识（读操作数**之前**取，保证每个调用点一个稳定值）——
        // 内联缓存的索引用它，见下面 native_find_module_method 那段的说明。
        uint8_t* ip_before = frame->ip;
        int a = READ_A(), b = READ_B();
        int nargs = READ_C();
        int line = GET_CURRENT_LINE();
        int cidx = READ_CONST_IDX(b);
        if (unlikely(cidx < 0 || cidx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("模块方法名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value nv = frame->chunk->constants[cidx];
        if (unlikely(!val_is_obj(nv) || val_as_obj(nv)->type != OBJ_STRING)) {
            if (!throw_runtime_error("模块方法名必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* combo = (ObjString*)val_as_obj(nv);

        // ★ 内联缓存：按**调用点**缓存 "module\0method" → ModuleMethodMeta*。
        //   此前这里每次调用都直接 native_find_module_method()（哈希 + 两次 strcmp）——
        //   实测 examples/性能测试/函数调用数学计算.leno（5000 万轮 × 3 次模块调用）
        //   因此比栈式慢 **1.68x**（≈22ns/次；同一份样例里纯循环与浮点算术
        //   寄存器式反而更快，差距全集中在这一处）。
        //   字段与初始化（vm.ic_module_cache / IC_MODULE_CACHE_SIZE）早就随移植带过来了
        //   （leno_vm.h 的注释就写着"避免每次调用都做哈希表查找"），
        //   只是本 handler 从没读写过它 —— 一直是个**死字段**；
        //   栈式的 op_module_call.inc 正是靠这份缓存。
        //   安全性：`meta` 指向 native 方法表里**单独分配、永不释放**的 entry，
        //   表扩容只是重新分桶、复用同一批 entry 对象 ⇒ 指针稳定（GC 也不管它）。
        int cache_idx = (int)(((uintptr_t)ip_before >> 2) & (IC_MODULE_CACHE_SIZE - 1));
        InlineModuleCallCacheEntry* cache = &vm.ic_module_cache[cache_idx];
        uint32_t name_hash = combo->hash;   // 合并串的哈希同时覆盖模块名与方法名

        ModuleMethodMeta* meta;
        if (likely(cache->valid && cache->combined_hash == name_hash)) {
            vm.ic_hits++;
            meta = (ModuleMethodMeta*)cache->meta;
        } else {
            vm.ic_misses++;
            // 未命中才解析名字：命中路径连 strlen / strcmp 都省掉
            const char* module_name = combo->chars;
            int mlen = (int)strlen(module_name);
            if (unlikely(mlen + 1 > combo->len)) {
                if (!throw_runtime_error("模块方法名格式错误", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            const char* method_name = combo->chars + mlen + 1;
            meta = native_find_module_method(module_name, method_name);
            if (unlikely(!meta)) {
                char msg[512];
                snprintf(msg, sizeof(msg), "未找到模块方法: %s.%s", module_name, method_name);
                if (!throw_runtime_error(msg, line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            cache->valid = 1;
            cache->combined_hash = name_hash;
            cache->meta = (void*)meta;
        }

        int base = vm.sp;
        for (int i = 0; i < nargs; i++) vm_stack_push(&vm, R(a + 1 + i));
        Value result = meta->function(nargs, vm.stack + base);
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
    LABEL(OP_GET_MODULE_CONST): DISPATCH();
    // 模块变量 / 函数：以**当前帧所属模块**为上下文，索引到 module->globals[]
    // OP_GET_MODULE_VAR A Bx: R[A] = 模块全局[Bx]
    LABEL(OP_GET_MODULE_VAR): {
        int a = READ_A();
        int bx = READ_Bx();
        int line = GET_CURRENT_LINE();
        ObjModule* module = (ObjModule*)frame->module;
        if (unlikely(!module)) {
            if (!throw_runtime_error("模块变量访问: 不在模块上下文中", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (unlikely(bx < 0 || bx >= module->global_count)) {
            // 尚未写入过的槽位：视为 null（与全局变量未初始化一致）
            R(a) = val_null();
            DISPATCH();
        }
        R(a) = module->globals[bx];
        DISPATCH();
    }
    // OP_SET_MODULE_VAR A Bx: 模块全局[Bx] = R[A]（必要时扩容，见 DEFINE_MODULE_FUNC）
    LABEL(OP_SET_MODULE_VAR): {
        int a = READ_A();
        int bx = READ_Bx();
        int line = GET_CURRENT_LINE();
        ObjModule* module = (ObjModule*)frame->module;
        if (unlikely(!module)) {
            if (!throw_runtime_error("模块变量设置: 不在模块上下文中", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (bx >= module->global_capacity) {
            int new_capacity = module->global_capacity == 0 ? 8 : module->global_capacity * 2;
            while (new_capacity <= bx) new_capacity *= 2;
            Value* new_globals = (Value*)realloc(module->globals, sizeof(Value) * (size_t)new_capacity);
            if (unlikely(!new_globals)) {
                runtime_error("模块变量设置: 内存分配失败", line);
                return -1;
            }
            for (int i = module->global_capacity; i < new_capacity; i++) {
                new_globals[i] = val_null();
            }
            module->globals = new_globals;
            module->global_capacity = new_capacity;
        }
        module->globals[bx] = R(a);
        if (bx >= module->global_count) module->global_count = bx + 1;
        gc_write_barrier((Object*)module, R(a));
        DISPATCH();
    }
    // OP_GET_MODULE_FUNC A Bx: 同 GET_MODULE_VAR（模块函数与变量共用 globals 槽）
    LABEL(OP_GET_MODULE_FUNC): {
        int a = READ_A();
        int bx = READ_Bx();
        int line = GET_CURRENT_LINE();
        ObjModule* module = (ObjModule*)frame->module;
        if (unlikely(!module)) {
            if (!throw_runtime_error("模块函数访问: 不在模块上下文中", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (unlikely(bx < 0 || bx >= module->global_count)) {
            if (!throw_runtime_error("模块函数索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = module->globals[bx];
        DISPATCH();
    }
    // OP_DEFINE_MODULE_FUNC A Bx: 模块全局[Bx] = R[A]（必要时扩容）
    LABEL(OP_DEFINE_MODULE_FUNC): {
        int a = READ_A();
        int bx = READ_Bx();
        int line = GET_CURRENT_LINE();
        ObjModule* module = (ObjModule*)frame->module;
        if (unlikely(!module)) {
            if (!throw_runtime_error("模块函数定义: 不在模块上下文中", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        if (bx >= module->global_capacity) {
            int new_capacity = module->global_capacity == 0 ? 8 : module->global_capacity * 2;
            while (new_capacity <= bx) new_capacity *= 2;
            Value* new_globals = (Value*)realloc(module->globals, sizeof(Value) * (size_t)new_capacity);
            if (unlikely(!new_globals)) {
                runtime_error("模块函数定义: 内存分配失败", line);
                return -1;
            }
            for (int i = module->global_capacity; i < new_capacity; i++) {
                new_globals[i] = val_null();
            }
            module->globals = new_globals;
            module->global_capacity = new_capacity;
        }
        module->globals[bx] = R(a);
        if (bx >= module->global_count) module->global_count = bx + 1;
        gc_write_barrier((Object*)module, R(a));
        DISPATCH();
    }
    // OP_INIT_LENOMODULE A: 若 R[A] 是未初始化的 .leno 模块，执行其 init_chunk
    //   执行完毕 R[A] 仍是模块对象（供别名变量持有）
    LABEL(OP_INIT_LENOMODULE): {
        int a = READ_A();
        int line = GET_CURRENT_LINE();
        Value module_val = R(a);

        if (unlikely(!val_is_obj(module_val) || val_as_obj(module_val)->type != OBJ_MODULE)) {
            if (!throw_runtime_error("模块初始化: 目标不是模块对象", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        ObjModule* module = (ObjModule*)val_as_obj(module_val);

        if (!module->initialized) {
            module->initialized = 1;

            // 模块依赖的原生模块
            if (module->native_imports && module->native_import_count > 0) {
                for (int i = 0; i < module->native_import_count; i++) {
                    if (native_init_module(module->native_imports[i]) != 0) {
                        char nmsg[256];
                        snprintf(nmsg, sizeof(nmsg), "未知的原生模块: %s", module->native_imports[i]);
                        if (!throw_runtime_error(nmsg, line)) return -1;
                        frame = &vm.frames[vm.frame_cnt - 1];
                        DISPATCH();
                    }
                }
            }

            if (module->init_chunk) {
                int saved_sp = vm.sp;
                ModuleFrame* mod_frame = module_frame_new(module);
                module_frame_enter(mod_frame);
                int result = vm_run_chunk(module->init_chunk);
                module_frame_exit();
                vm.sp = saved_sp;
                // 递归执行后 vm.frames 可能已 realloc，frame 必须重新取
                frame = &vm.frames[vm.frame_cnt - 1];
                if (result < 0) return -1;
            }

            // 导出映射：把模块内全局变量/类型补进 exports
            if (module->export_mappings && module->export_mapping_count > 0) {
                for (int i = 0; i < module->export_mapping_count; i++) {
                    ExportGlobalMapping* mapping = &module->export_mappings[i];
                    ObjString* key = str_copy(mapping->name, (int)strlen(mapping->name));
                    Value current_val = dict_get(module->exports, val_obj((Object*)key));
                    if (val_is_null(current_val)) {
                        if (mapping->global_index >= 0 && mapping->global_index < module->global_count) {
                            Value actual_val = module->globals[mapping->global_index];
                            if (!val_is_null(actual_val)) {
                                dict_set(module->exports, val_obj((Object*)key), actual_val);
                            }
                        } else if (mapping->global_index == -2) {
                            ObjStructDef* def = struct_def_find(mapping->name);
                            if (def) {
                                dict_set(module->exports, val_obj((Object*)key), val_obj((Object*)def));
                            }
                        }
                    }
                }
            }

            // use 再导出的类型
            if (module->use_reexport_names && module->use_reexport_count > 0) {
                for (int i = 0; i < module->use_reexport_count; i++) {
                    const char* name = module->use_reexport_names[i];
                    TypeKind kind = (TypeKind)module->use_reexport_kinds[i];
                    ObjString* key = str_copy(name, (int)strlen(name));
                    Value current_val = dict_get(module->exports, val_obj((Object*)key));
                    if (val_is_null(current_val)) {
                        if (kind == TYPE_ENUM) {
                            ObjEnumDef* def = enum_def_find(name);
                            if (def) dict_set(module->exports, val_obj((Object*)key), val_obj((Object*)def));
                        } else if (kind == TYPE_STRUCT) {
                            ObjStructDef* def = struct_def_find(name);
                            if (def) dict_set(module->exports, val_obj((Object*)key), val_obj((Object*)def));
                        }
                    }
                }
            }
        }

        R(a) = module_val;
        DISPATCH();
    }

    // --- struct ---
    // OP_STRUCT_DEF A Bx: Bx = 结构体名常量索引；其余元数据紧随指令（布局与栈式一致）：
    //   field_count(1) method_count(1)
    //   impl_count(1) + impl_name_idx(2)×n
    //   type_param_count(1) + param_name_idx(2)×n
    //   每个字段: name_idx(2) type(1) nullable(1)
    //             [type==STRUCT: has_name(1) [name_idx(2)]]
    //             [type==PTR_GENERIC: elem_type(1)]
    //             has_default(1) [default_idx(2)]
    //   每个方法: method_name_idx(2) func_idx(2)
    //   ctor_dtor_flags(1) [ctor_index(1)] [dtor_index(1)]
    //   const_count(1) + (cname_idx(2) cval_idx(2))×n
