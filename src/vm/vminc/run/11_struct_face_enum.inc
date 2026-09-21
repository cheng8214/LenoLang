    LABEL(OP_STRUCT_DEF): {
        int line = GET_CURRENT_LINE();
        int name_idx = READ_Bx();
        if (unlikely(name_idx < 0 || name_idx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("结构体名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[name_idx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("结构体名称必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* sname = (ObjString*)val_as_obj(name_val);

        uint8_t* p = frame->ip;
        uint8_t field_count = *p++;
        uint8_t method_count = *p++;

        ObjStructDef* def = struct_def_new(sname->chars, field_count, method_count);
        if (unlikely(!def)) {
            runtime_error("结构体定义内存分配失败", line);
            return -1;
        }
        if (!def->owner) def->owner = module_frame_current_module();

        uint8_t impl_count = *p++;
        if (impl_count > 0) {
            def->impl_names = (char**)calloc(impl_count, sizeof(char*));
            def->impl_count = impl_count;
            for (int i = 0; i < impl_count; i++) {
                int idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                Value v = frame->chunk->constants[idx];
                if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
                    def->impl_names[i] = strdup(((ObjString*)val_as_obj(v))->chars);
                }
            }
        }

        uint8_t type_param_count = *p++;
        def->type_param_count = type_param_count;
        if (type_param_count > 0) {
            def->type_param_names = (char**)malloc(sizeof(char*) * type_param_count);
            def->type_param_constraints = (char**)calloc(type_param_count, sizeof(char*));
            for (int i = 0; i < type_param_count; i++) {
                int idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                Value v = frame->chunk->constants[idx];
                if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
                    def->type_param_names[i] = strdup(((ObjString*)val_as_obj(v))->chars);
                } else {
                    def->type_param_names[i] = strdup("T");
                }
            }
        }

        for (int i = 0; i < field_count; i++) {
            int field_name_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            uint8_t field_type = *p++;
            uint8_t is_nullable = *p++;

            char* struct_type_name = NULL;
            if (field_type == TYPE_STRUCT) {
                uint8_t has_struct_type_name = *p++;
                if (has_struct_type_name) {
                    int tn_idx = ((int)p[0] << 8) | (int)p[1];
                    p += 2;
                    Value tv = frame->chunk->constants[tn_idx];
                    if (val_is_obj(tv) && val_as_obj(tv)->type == OBJ_STRING) {
                        struct_type_name = ((ObjString*)val_as_obj(tv))->chars;
                    }
                }
            }

            TypeKind element_type = TYPE_PTR;
            if (field_type == TYPE_PTR_GENERIC) {
                element_type = (TypeKind)(*p++);
            }

            uint8_t has_default = *p++;
            Value field_name_val = frame->chunk->constants[field_name_idx];
            if (unlikely(!val_is_obj(field_name_val) || val_as_obj(field_name_val)->type != OBJ_STRING)) {
                if (!throw_runtime_error("结构体字段名必须是字符串", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            ObjString* field_name = (ObjString*)val_as_obj(field_name_val);

            Value default_value = val_null();
            if (has_default) {
                int d_idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                default_value = frame->chunk->constants[d_idx];
            }

            struct_def_set_field(def, i, field_name->chars, (TypeKind)field_type,
                                 struct_type_name, default_value, has_default, element_type, is_nullable);
        }

        for (int i = 0; i < method_count; i++) {
            int mn_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            Value mn_val = frame->chunk->constants[mn_idx];
            if (unlikely(!val_is_obj(mn_val) || val_as_obj(mn_val)->type != OBJ_STRING)) {
                if (!throw_runtime_error("结构体方法名必须是字符串", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            def->methods[i].name = strdup(((ObjString*)val_as_obj(mn_val))->chars);

            int fn_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            Value fn_val = frame->chunk->constants[fn_idx];
            if (unlikely(!val_is_obj(fn_val) || val_as_obj(fn_val)->type != OBJ_FUNCTION)) {
                if (!throw_runtime_error("结构体方法必须是函数对象", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            def->methods[i].func = (ObjFunction*)val_as_obj(fn_val);
            ObjFunction* mfunc = def->methods[i].func;
            if (mfunc->upvalue_count == 0) {
                ObjClosure* cl = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                if (unlikely(!cl)) {
                    runtime_error("结构体方法闭包内存分配失败", line);
                    return -1;
                }
                cl->function = mfunc;
                cl->upvalue_count = 0;
                cl->type_param_count = 0;
                cl->type_param_args = NULL;
                def->methods[i].closure = cl;
            } else {
                // 需要 upvalue 的方法：运行时动态创建闭包
                def->methods[i].closure = NULL;
            }
        }

        uint8_t ctor_dtor_flags = *p++;
        def->has_ctor = (ctor_dtor_flags & 1) != 0;
        def->has_dtor = (ctor_dtor_flags & 2) != 0;
        if (def->has_ctor) def->ctor_index = *p++;
        if (def->has_dtor) def->dtor_index = *p++;

        uint8_t const_count = *p++;
        def->const_count = const_count;
        if (const_count > 0) {
            def->const_names = (char**)malloc(sizeof(char*) * const_count);
            def->const_values = (Value*)calloc(const_count, sizeof(Value));
            for (int i = 0; i < const_count; i++) {
                int cn_idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                Value cn_val = frame->chunk->constants[cn_idx];
                if (val_is_obj(cn_val) && val_as_obj(cn_val)->type == OBJ_STRING) {
                    def->const_names[i] = strdup(((ObjString*)val_as_obj(cn_val))->chars);
                } else {
                    def->const_names[i] = strdup("");
                }
                int cv_idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                def->const_values[i] = frame->chunk->constants[cv_idx];
            }
        }

        frame->ip = p;
        struct_def_register(def);
        DISPATCH();
    }
    // OP_FACE_DEF A Bx: 定义接口（与栈式同一数据布局）
    LABEL(OP_FACE_DEF): {
        int line = GET_CURRENT_LINE();
        int name_idx = READ_Bx();
        if (unlikely(name_idx < 0 || name_idx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("接口名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[name_idx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("接口名称必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* fname = (ObjString*)val_as_obj(name_val);

        uint8_t* p = frame->ip;
        uint8_t method_count = *p++;
        uint8_t type_param_count = *p++;

        ObjFaceDef* def = face_def_new(fname->chars, method_count);
        if (unlikely(!def)) {
            runtime_error("接口定义内存分配失败", line);
            return -1;
        }
        if (!def->owner) def->owner = module_frame_current_module();

        if (type_param_count > 0) {
            char** tp_names = (char**)malloc(sizeof(char*) * type_param_count);
            for (int tp = 0; tp < type_param_count; tp++) {
                int idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                Value v = frame->chunk->constants[idx];
                if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
                    tp_names[tp] = strdup(((ObjString*)val_as_obj(v))->chars);
                } else {
                    tp_names[tp] = strdup("T");
                }
            }
            face_def_set_type_params(def, type_param_count, tp_names, NULL);
            for (int tp = 0; tp < type_param_count; tp++) free(tp_names[tp]);
            free(tp_names);
        }

        for (int i = 0; i < method_count; i++) {
            int mn_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            uint8_t param_count = *p++;
            uint8_t return_type_kind = *p++;
            Value mn_val = frame->chunk->constants[mn_idx];
            if (unlikely(!val_is_obj(mn_val) || val_as_obj(mn_val)->type != OBJ_STRING)) {
                if (!throw_runtime_error("接口方法名必须是字符串", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            def->methods[i].name = strdup(((ObjString*)val_as_obj(mn_val))->chars);
            def->methods[i].param_count = param_count;
            def->methods[i].return_type = (return_type_kind != TYPE_INFER)
                                              ? type_new((TypeKind)return_type_kind) : NULL;
            def->methods[i].param_types = NULL;
            if (param_count > 0) {
                def->methods[i].param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * param_count);
                for (int j = 0; j < param_count; j++) {
                    uint8_t ptk = *p++;
                    def->methods[i].param_types[j] = (ptk != TYPE_INFER) ? type_new((TypeKind)ptk) : NULL;
                }
            }
        }

        frame->ip = p;
        face_def_register(def);
        DISPATCH();
    }
    // OP_ENUM_DEF A Bx: R[A] = enum 定义对象；数据区 member_count(1) + (名idx(2) 值idx(2))×n
    LABEL(OP_ENUM_DEF): {
        int a = READ_A();
        int line = GET_CURRENT_LINE();
        int name_idx = READ_Bx();
        if (unlikely(name_idx < 0 || name_idx >= frame->chunk->const_cnt)) {
            if (!throw_runtime_error("枚举名常量索引越界", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        Value name_val = frame->chunk->constants[name_idx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("enum 名称必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* ename = (ObjString*)val_as_obj(name_val);

        uint8_t* p = frame->ip;
        uint8_t member_count = *p++;
        ObjEnumDef* def = enum_def_new(ename->chars, member_count);
        if (unlikely(!def)) {
            runtime_error("枚举定义内存分配失败", line);
            return -1;
        }
        if (!def->owner) def->owner = module_frame_current_module();

        for (int i = 0; i < member_count; i++) {
            int mn_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            int mv_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            Value mn_val = frame->chunk->constants[mn_idx];
            if (unlikely(!val_is_obj(mn_val) || val_as_obj(mn_val)->type != OBJ_STRING)) {
                if (!throw_runtime_error("enum 成员名必须是字符串", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            Value mv_val = frame->chunk->constants[mv_idx];
            int64_t member_value;
            if (val_is_obj(mv_val) && val_as_obj(mv_val)->type == OBJ_BIGINT) {
                member_value = bigint_to_int64((ObjBigInt*)val_as_obj(mv_val));
            } else {
                member_value = (int64_t)val_as_num(mv_val);
            }
            enum_def_set_member(def, i, ((ObjString*)val_as_obj(mn_val))->chars, member_value);
        }

        frame->ip = p;
        enum_def_register(def);
        R(a) = val_obj((Object*)def);
        DISPATCH();
    }
    // OP_STRUCT_INIT A B C: R[A] = new Struct(...)（B = 实参个数）
    //   数据区：name_idx(2) generic_count(1) [generic_name_idx(2)]×n
    //           [field_name_idx(2)]×arg_count
    //           mod_space(1) + mod_slot(2)                     ← S2/2b-2，见下方"窥探"
    //   构造实参在 R[A+1 .. A+B]
    LABEL(OP_STRUCT_INIT): {
        // 调用点标识（读操作数之前取）—— 内联缓存的索引用它
        uint8_t* ip_before = frame->ip;
        int a = READ_A();
        int arg_count = READ_B();
        int line = GET_CURRENT_LINE();

        uint8_t* p = frame->ip;
        int name_idx = ((int)p[0] << 8) | (int)p[1];
        p += 2;
        int generic_count = *p++;

        char** generic_args = NULL;
        if (generic_count > 0) {
            generic_args = (char**)malloc(sizeof(char*) * generic_count);
            for (int gi = 0; gi < generic_count; gi++) {
                int idx = ((int)p[0] << 8) | (int)p[1];
                p += 2;
                Value v = frame->chunk->constants[idx];
                if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
                    generic_args[gi] = strdup(((ObjString*)val_as_obj(v))->chars);
                } else {
                    generic_args[gi] = strdup("unknown");
                }
            }
        }

        Value name_val = frame->chunk->constants[name_idx];
        if (unlikely(!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)) {
            if (!throw_runtime_error("结构体名称必须是字符串", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        ObjString* sname = (ObjString*)val_as_obj(name_val);

        // ★ 泛型实参替换（`new Holder[K](...)` 里的 K 要换成具体类型名）：
        //   ① 先看调用栈里的 self 实例（方法内 new 泛型 struct）；
        //   ② 再看闭包的 type_param_args（泛型函数调用时传入的精准实参）；
        //   ③ 最后按函数参数运行时值反推（T val ⇒ int/string/...）。
        //   缺失此步会让 `type(h)` 得到 "Holder[K]" 而不是 "Holder[string]" ✗
        if (generic_count > 0 && generic_args) {
            int resolved = 0;
            for (int fi = vm.frame_cnt - 1; fi >= 1 && !resolved; fi--) {
                CallFrame* cf = &vm.frames[fi];
                if (!cf->locals || cf->local_count <= 0) continue;
                Value self_val = cf->locals[0];
                if (val_is_obj(self_val) && val_as_obj(self_val)->type == OBJ_STRUCT) {
                    ObjStruct* self_obj = (ObjStruct*)val_as_obj(self_val);
                    ObjStructDef* self_def = self_obj->def;
                    if (self_def && self_def->type_param_count > 0 && self_def->type_param_names &&
                        self_obj->generic_type_arg_count > 0 && self_obj->generic_type_args) {
                        int any = 0;
                        for (int gi = 0; gi < generic_count; gi++) {
                            const char* arg_name = generic_args[gi];
                            const char* pure = (strncmp(arg_name, "struct ", 7) == 0)
                                                   ? arg_name + 7 : arg_name;
                            for (int pi = 0; pi < self_def->type_param_count &&
                                            pi < self_obj->generic_type_arg_count; pi++) {
                                if (self_def->type_param_names[pi] && pure &&
                                    strcmp(self_def->type_param_names[pi], pure) == 0) {
                                    free(generic_args[gi]);
                                    generic_args[gi] = strdup(self_obj->generic_type_args[pi]);
                                    any = 1;
                                    break;
                                }
                            }
                        }
                        if (any) resolved = 1;
                    }
                }
            }

            if (!resolved) {
                for (int fi = vm.frame_cnt - 1; fi >= 1 && !resolved; fi--) {
                    CallFrame* cf = &vm.frames[fi];
                    if (!cf->closure || !cf->closure->function) continue;
                    ObjFunction* func = cf->closure->function;
                    ObjClosure* clo = cf->closure;

                    // ② 闭包携带的精准类型实参
                    if (clo->type_param_count > 0 && clo->type_param_args &&
                        func->type_param_count > 0 && func->type_param_names) {
                        int any = 0;
                        for (int gi = 0; gi < generic_count; gi++) {
                            const char* arg_name = generic_args[gi];
                            const char* pure = (strncmp(arg_name, "struct ", 7) == 0)
                                                   ? arg_name + 7 : arg_name;
                            for (int pi = 0; pi < func->type_param_count &&
                                            pi < clo->type_param_count; pi++) {
                                if (func->type_param_names[pi] && pure &&
                                    strcmp(func->type_param_names[pi], pure) == 0) {
                                    free(generic_args[gi]);
                                    generic_args[gi] = strdup(clo->type_param_args[pi]);
                                    any = 1;
                                    break;
                                }
                            }
                        }
                        if (any) resolved = 1;
                    }

                    // ③ 按函数参数的运行时值反推
                    if (!resolved && func->type_param_count > 0 && func->type_param_names &&
                        cf->locals && cf->local_count > 0) {
                        int any = 0;
                        for (int gi = 0; gi < generic_count; gi++) {
                            const char* arg_name = generic_args[gi];
                            const char* pure = (strncmp(arg_name, "struct ", 7) == 0)
                                                   ? arg_name + 7 : arg_name;
                            int matched = -1;
                            for (int pi = 0; pi < func->type_param_count; pi++) {
                                if (func->type_param_names[pi] && pure &&
                                    strcmp(func->type_param_names[pi], pure) == 0) {
                                    matched = pi;
                                    break;
                                }
                            }
                            if (matched < 0) continue;
                            for (int ai = 0; ai < func->arity && ai < cf->local_count; ai++) {
                                Value arg_val = cf->locals[ai];
                                if (val_is_null(arg_val)) continue;
                                if (func->param_generic_names && func->param_generic_names[ai] &&
                                    pure && strcmp(func->param_generic_names[ai], pure) == 0) {
                                    const char* inferred = vm_value_to_generic_type_name(arg_val);
                                    if (inferred) {
                                        free(generic_args[gi]);
                                        generic_args[gi] = strdup(inferred);
                                        any = 1;
                                        break;
                                    }
                                }
                                if (val_is_obj(arg_val) && val_as_obj(arg_val)->type == OBJ_ARRAY) {
                                    ObjArray* arr = (ObjArray*)val_as_obj(arg_val);
                                    if (arr->type_info && arr->type_info->element_type) {
                                        const char* inferred =
                                            vm_typeinfo_to_generic_type_name(arr->type_info->element_type);
                                        if (inferred) {
                                            free(generic_args[gi]);
                                            generic_args[gi] = strdup(inferred);
                                            any = 1;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        if (any) resolved = 1;
                    }
                }
            }
        }

        // ---- 模块限定操作数（S2/2b-2：mod_space8 + mod_slot16）----
        // 位置在**字段名数据之后**（p 现在正指着字段名数据 ⇒ 偏移 2*arg_count），此处只
        // "窥探"不消费：真正的越过得等字段循环跑完（下面的 `p += 3`）。
        //   含义：0 = 无（模块内裸名 / 旧字节码）；1 = vm.globals[slot]（入口程序里
        //   `import ... as a` 的别名是 SYM_GLOBAL）；2 = frame->module->globals[slot]
        //   （模块文件里是 SYM_MODULE）。槽位里的对象是 import 存进去的**模块对象**。
        // 为什么必须按身份而不是按名字：别名与运行期 ObjModule.name 并不总相等
        //   （模块按路径去重、**首次加载定名**）—— 实测 test_cross_module_type_conflict
        //   的 D 段：p.leno 先被 `as first` 加载过，入口 `new p2.Point()` 按前缀比名字
        //   必然落空 ⇒ 回退裸名 ⇒ 拿到先注册的 r.Point（A=3，期望 A=1）。
        // 旧字节码没有这 3 字节时读到的是后续指令的字节 ⇒ 靠 OBJ_MODULE 类型检查兜底
        //   （取不到就退化为原有的名字查找，不会错值）。
        ObjModule* mod_owner = NULL;
        {
            const uint8_t* mod_ops = p + (size_t)arg_count * 2;
            uint8_t mod_space = mod_ops[0];
            uint16_t mod_slot = (uint16_t)(((uint16_t)mod_ops[1] << 8) | mod_ops[2]);
            if (mod_space == 1) {
                if (mod_slot < vm.global_count) {
                    Value mv = vm.globals[mod_slot];
                    if (val_is_obj(mv) && val_as_obj(mv)->type == OBJ_MODULE) {
                        mod_owner = (ObjModule*)val_as_obj(mv);
                    }
                }
            } else if (mod_space == 2) {
                ObjModule* self_mod = (ObjModule*)frame->module;
                if (self_mod && mod_slot < self_mod->global_count) {
                    Value mv = self_mod->globals[mod_slot];
                    if (val_is_obj(mv) && val_as_obj(mv)->type == OBJ_MODULE) {
                        mod_owner = (ObjModule*)val_as_obj(mv);
                    }
                }
            }
        }

        // 四级查找（**不比旧行为更差**：逐级放宽，最后一级就是原来的裸名查找）：
        //   ① 字节码带的导入模块槽位 → 该模块声明的那份（最精确，与名字无关）
        //   ② 当前模块自己声明的那份（模块内 `new Point()`；不带前缀时靠它）
        //   ③ 名字带前缀且某定义 owner->name == 前缀 → 那份
        //   ④ 裸名（旧字节码 / 原生桥接 / 反射的既有行为，在 ③ 内部回退）
        // ★ 内联缓存：此前 vm.ic_structdef_cache 也是**死字段**（定义了、初始化了，
        //   handler 从没读写）⇒ 每次 `new X()` 都要走最多三级的按名字查找
        //   （strchr + 逐级 struct_def_find*，全是字符串比较）。
        //   键 =（调用点、名字哈希、struct 定义表代数、mod_owner）；
        //   **owner 一起比** 是为了让"同名不同 owner"的调用点各自命中自己那份、不互相污染
        //   （与栈式 op_struct.inc 同口径：它同样存 mod_owner 并比 struct_def_generation()）。
        //   安全性：ObjStructDef 由 struct_def_mark_all() 作为 GC 根标记，裸指针不会悬空。
        int cache_idx = (int)(((uintptr_t)ip_before >> 2) & (IC_STRUCTDEF_CACHE_SIZE - 1));
        InlineStructDefCacheEntry* sd_cache = &vm.ic_structdef_cache[cache_idx];
        ObjStructDef* def = NULL;
        if (likely(sd_cache->valid && sd_cache->name_hash == sname->hash &&
                   sd_cache->gen == struct_def_generation() && sd_cache->owner == mod_owner)) {
            vm.ic_hits++;
            def = sd_cache->def;
        } else {
            vm.ic_misses++;
            const char* bare = sname->chars;
            const char* dotp = strchr(bare, '.');
            if (dotp) bare = dotp + 1;
            if (mod_owner) def = struct_def_find_in_module(mod_owner, bare);
            if (!def) {
                ObjModule* owner = module_frame_current_module();
                if (owner) def = struct_def_find_in_module(owner, bare);
            }
            if (!def) def = struct_def_find_qualified(sname->chars);
            if (def) {
                sd_cache->valid = 1;
                sd_cache->name_hash = sname->hash;
                sd_cache->gen = struct_def_generation();
                sd_cache->owner = mod_owner;
                sd_cache->def = def;
            }
        }
        if (unlikely(!def)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "未定义的结构体 '%s'", sname->chars);
            if (generic_args) {
                for (int gi = 0; gi < generic_count; gi++) free(generic_args[gi]);
                free(generic_args);
            }
            if (!throw_runtime_error(msg, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        ObjStruct* obj = struct_instance_new(def);
        if (unlikely(!obj)) {
            runtime_error("结构体实例内存分配失败", line);
            return -1;
        }
        if (generic_count > 0) {
            obj->generic_type_args = generic_args;
            obj->generic_type_arg_count = generic_count;
        }

        // 按字段名逐个赋值（编译期给名字，运行时查索引；fn_idx == 0 表示按位置）
        for (int i = 0; i < arg_count; i++) {
            int fn_idx = ((int)p[0] << 8) | (int)p[1];
            p += 2;
            int fidx;
            if (fn_idx == 0) {
                fidx = i;
            } else {
                Value fn_val = frame->chunk->constants[fn_idx];
                if (unlikely(!val_is_obj(fn_val) || val_as_obj(fn_val)->type != OBJ_STRING)) {
                    if (!throw_runtime_error("结构体字段名必须是字符串", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                fidx = struct_get_field_index(def, ((ObjString*)val_as_obj(fn_val))->chars);
            }
            if (unlikely(fidx < 0 || fidx >= def->field_count)) {
                if (!throw_runtime_error("结构体字段不存在", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            Value v = R(a + 1 + i);
            TypeKind et = def->fields[fidx].type;
            if (et == TYPE_FLOAT && val_is_int(v)) {
                v = val_float((double)val_as_int(v));
            } else if (et == TYPE_FLOAT && val_is_bigint(v)) {
                v = val_float(bigint_to_double(val_as_bigint(v)));
            }
            struct_set_field(obj, fidx, v);
        }
        p += 3;   // 跳过模块限定操作数（mod_space8 + mod_slot16，见上面的"窥探"）
        frame->ip = p;

        // 结果先落到寄存器（GC 根），构造器可能在其内部触发分配
        R(a) = val_obj((Object*)obj);

        // 调用构造函数（如果有）：self = 实例，实参紧随其后
        if (def->has_ctor && def->ctor_index >= 0 && def->ctor_index < def->method_count) {
            StructMethodInfo* mi = &def->methods[def->ctor_index];
            ObjClosure* cl = mi->closure;
            if (!cl && mi->func) {
                cl = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                if (cl) {
                    cl->function = mi->func;
                    cl->upvalue_count = mi->func->upvalue_count;
                    cl->type_param_count = 0;
                    cl->type_param_args = NULL;
                    for (int ui = 0; ui < mi->func->upvalue_count; ui++) cl->upvalues[ui] = NULL;
                }
            }
            if (cl) {
                if (call_reg(cl, 1, R(a), a + 1, arg_count, a, 1, line)) {
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                if (!vm.has_exception) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
        }
        DISPATCH();
    }
    // OP_GET_FIELD A B C: R[A] = R[B].field(C)（C = 字段索引）
