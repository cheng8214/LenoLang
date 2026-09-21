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
            ObjNative* method = find_method_by_type(rt, name->chars);
            if (method) {
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
            if (def && struct_method_lookup(def, name, &pre, &fn)) {
                ObjClosure* cl = pre;
                if (!cl) {
                    cl = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                    if (unlikely(!cl)) { runtime_error("内存分配失败", line); return -1; }
                    cl->function = fn;
                    cl->upvalue_count = fn->upvalue_count;
                    cl->type_param_count = 0;
                    cl->type_param_args = NULL;
                    for (int ui = 0; ui < fn->upvalue_count; ui++) cl->upvalues[ui] = NULL;
                }
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
