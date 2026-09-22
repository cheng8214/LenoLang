    LABEL(OP_NEWARRAY): {
        int a = READ_A(), c = READ_C();
        ObjArray* arr = arr_new(c);
        if (!arr) { runtime_error("OP_NEWARRAY: arr_new 失败", GET_CURRENT_LINE()); return -1; }
        for (int i = 0; i < c; i++) {
            arr->elements[i] = R(a + 1 + i);
        }
        arr->count = c;

        // ★ 设置运行时类型信息（照栈式 OP_ARRAY）：元素类型全一致时记 Array[T]，
        //   空数组/混合类型留 NULL。`type()`、空数组的类型名、add 的类型检查都依赖它 ——
        //   不设置的话 `arr.clear()` 之后 type(arr) 只剩 "Array"（元素类型信息丢失，
        //   栈式仍报 Array[int]；examples/array/基本 clear.leno 的差异）。
        //   用驻留缓存 type_get_array_cached，避免每个数组字面量都分配 TypeInfo。
        if (c > 0) {
            Value first = arr->elements[0];
            int all_same = 1;
            for (int i = 1; i < c; i++) {
                if (val_get_type(arr->elements[i]) != val_get_type(first)) { all_same = 0; break; }
            }
            if (all_same) {
                TypeKind elem_kind = TYPE_ANY;
                switch (val_get_type(first)) {
                    case VAL_NULL:  elem_kind = TYPE_NULL; break;
                    case VAL_BOOL:  elem_kind = TYPE_BOOL; break;
                    case VAL_INT:   elem_kind = TYPE_INT; break;
                    case VAL_FLOAT: elem_kind = TYPE_FLOAT; break;
                    case VAL_OBJ:
                        switch (val_as_obj(first)->type) {
                            case OBJ_STRING: elem_kind = TYPE_STRING; break;
                            case OBJ_ARRAY:  elem_kind = TYPE_ARRAY; break;
                            case OBJ_DICT:   elem_kind = TYPE_DICT; break;
                            default: break;   // 其他对象类型不设类型信息
                        }
                        break;
                    default: break;
                }
                if (elem_kind != TYPE_ANY) {
                    arr->type_info = type_get_array_cached(elem_kind);
                }
            }
        }

        R(a) = val_obj((Object*)arr);
        DISPATCH();
    }
    LABEL(OP_ARRAY_GET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjArray* arr = (ObjArray*)val_as_obj(R(b));
        int idx = (int)val_as_int(R(c));
        R(a) = arr->elements[idx];
        DISPATCH();
    }
    LABEL(OP_ARRAY_SET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjArray* arr = (ObjArray*)val_as_obj(R(b));
        int idx = (int)val_as_int(R(c));
        arr->elements[idx] = R(a);
        DISPATCH();
    }
    LABEL(OP_ARRAY_APPEND): {
        int a = READ_A(), b = READ_B();
        ObjArray* arr = (ObjArray*)val_as_obj(R(b));
        if (arr->count >= arr->capacity) { if (!arr_grow(arr)) { runtime_error("OP_ARRAY_APPEND: arr_grow 失败", GET_CURRENT_LINE()); return -1; } }
        arr->elements[arr->count++] = R(a);
        // ★ 写屏障（补漏）：本 GC 是**分代 + 记住集**——老年代对象引用年轻代对象时
        //   必须把老对象入集，否则年轻代回收漏扫该引用（见 leno_value.h 的 REMEMBERED 位
        //   与 gc.c 开头的说明）。原生 arr_add 一直有这一句，这条融合指令此前漏了：
        //   数组字面量场景恰好是 young→young 所以没暴露，但 `arr += x` 落在老数组上就有风险。
        gc_write_barrier((Object*)arr, R(a));
        // C = "要不要结果"标志（codegen 的 need_result）：语句位置没人要这个新长度
        // ⇒ 跳过写回（T10-②，等价于栈式的 OP_ARRAY_APPEND_NOPUSH，省一条写）。
        if (READ_C()) R(a) = val_int(arr->count);
        DISPATCH();
    }
    LABEL(OP_LEN): {
        int a = READ_A(), b = READ_B();
        Value v = R(b);
        if (val_is_obj(v)) {
            if (val_as_obj(v)->type == OBJ_ARRAY) R(a) = val_int(((ObjArray*)val_as_obj(v))->count);
            else if (val_as_obj(v)->type == OBJ_STRING) R(a) = val_int(((ObjString*)val_as_obj(v))->len);
            // ⚠ 字典的条目分两处存：**连续整数键**在数组部（acount），
            //   其余键在哈希部（count）。只读 count 会让整数键字典的长度恒为 0
            //   （实测 `{1:..,2:..,3:..}` 的 len 得 0，for 迭代一次都不进 ✗）
            else if (val_as_obj(v)->type == OBJ_DICT) {
                ObjDict* d = (ObjDict*)val_as_obj(v);
                R(a) = val_int(d->acount + d->count);
            }
            // struct：字段数（`for p to k` 遍历字段名 / `p.len()`）
            else if (val_as_obj(v)->type == OBJ_STRUCT) {
                ObjStruct* so = (ObjStruct*)val_as_obj(v);
                R(a) = val_int(so->def ? so->def->field_count : 0);
            }
            // enum：成员数（`for EV to v` 遍历成员值；OP_ITER_GET 的 enum 分支配套）
            //   ⚠ 缺这一支时 len 恒为 0 ⇒ 循环体一次都不进（实测 sum_all() 得 0 ✗）
            else if (val_as_obj(v)->type == OBJ_ENUM_DEF) {
                ObjEnumDef* ed = (ObjEnumDef*)val_as_obj(v);
                R(a) = val_int(ed->member_count);
            }
            else R(a) = val_int(0);
        } else {
            R(a) = val_int(0);
        }
        DISPATCH();
    }

    // --- 字典 ---
    // OP_NEWDICT A B C: 用 R[A+1..A+2C] 交替的 (键, 值) 建字典
    LABEL(OP_NEWDICT): {
        int a = READ_A(), c = READ_C();
        ObjDict* dict = dict_new(c > 0 ? c : 8);
        if (unlikely(!dict)) {
            runtime_error("OP_NEWDICT: 内存分配失败", GET_CURRENT_LINE());
            return -1;
        }
        for (int i = 0; i < c; i++) {
            dict_set(dict, R(a + 1 + i * 2), R(a + 2 + i * 2));
        }
        R(a) = val_obj((Object*)dict);
        DISPATCH();
    }
    // 字典读写：这两条 opcode 目前 codegen 不发（实测 dump 里 `d[k]` / `d[k]=v` 走的是
    //   OP_INDEX / OP_INDEX_SET 的字典分支）⇒ 内联缓存接在那边（见 OP_INDEX_SET）。
    //   这里保持最简实现，不做额外探测。
    LABEL(OP_DICT_GET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjDict* dict = (ObjDict*)val_as_obj(R(b));
        R(a) = dict_get(dict, R(c));
        DISPATCH();
    }
    LABEL(OP_DICT_SET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjDict* dict = (ObjDict*)val_as_obj(R(b));
        dict_set(dict, R(c), R(a));
        DISPATCH();
    }
    LABEL(OP_DICT_GET_KEY): {
        // TODO: 字典键迭代
        DISPATCH();
    }

    // --- 通用索引 ---
    // OP_INDEX A B C: R[A] = R[B][R[C]]（数组 / 字符串 / 字典）
    LABEL(OP_INDEX): {
        uint8_t* ip_before = frame->ip;   // 调用点标识（字典内联缓存的索引）
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj = R(b);
        Value idx = R(c);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_obj(obj))) {
            ObjType ot = val_as_obj(obj)->type;
            if (ot == OBJ_ARRAY) {
                ObjArray* arr = (ObjArray*)val_as_obj(obj);
                if (unlikely(!val_is_num(idx))) {
                    if (!throw_runtime_error("数组索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                int i2 = (int)value_to_double(idx);
                if (unlikely((uint64_t)i2 >= (uint64_t)arr->count)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "数组索引越界: 索引 %d, 长度 %d", i2, arr->count);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                R(a) = arr->elements[i2];
                DISPATCH();
            }
            if (ot == OBJ_STRING) {
                ObjString* s = (ObjString*)val_as_obj(obj);
                if (unlikely(!val_is_num(idx))) {
                    // 非数字索引：可能是 `EnumType.Member` —— 类型名（字符串）+ 成员名，
                    // 走全局枚举表查找。语义分析把 `Color.RED` 编译成 obj["RED"]，
                    // 而 obj 是枚举类型名常量。
                    if (val_is_string(idx)) {
                        ObjEnumDef* ed = enum_def_find(s->chars);
                        if (ed) {
                            const char* mname = ((ObjString*)val_as_obj(idx))->chars;
                            for (int mi = 0; mi < ed->member_count; mi++) {
                                if (ed->members[mi].name &&
                                    strcmp(ed->members[mi].name, mname) == 0) {
                                    // val_int_safe：超出 int48 时自动装箱成 bigint
                                    // （枚举里 1<<62 这类值很常见）
                                    R(a) = val_int_safe(ed->members[mi].value);
                                    DISPATCH();
                                }
                            }
                            char emsg[256];
                            snprintf(emsg, sizeof(emsg), "枚举 '%s' 不存在成员 '%s'",
                                     s->chars, mname);
                            if (!throw_runtime_error(emsg, line)) return -1;
                            frame = &vm.frames[vm.frame_cnt - 1];
                            DISPATCH();
                        }
                    }
                    if (!throw_runtime_error("字符串索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                int i2 = (int)value_to_double(idx);
                if (unlikely(i2 < 0 || i2 >= s->char_len)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "字符串索引越界: 索引 %d, 长度 %d", i2, s->char_len);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                int off = utf8_char_offset(s->chars, s->len, i2);
                int cb = utf8_char_byte_len(s->chars, s->len, off);
                R(a) = val_obj((Object*)str_new(&s->chars[off], cb));
                DISPATCH();
            }
            if (ot == OBJ_DICT) {
                // ★ 调用点内联缓存（见 leno_vm.h 的 InlineDictCacheEntry）：
                //   `d[k]` 原先每次都要走 dict_get 那 5~8 层跨 TU 调用。
                //   校验用**内容相等**（value_shallow_equal，与 dict_find_entry 同一判据）——
                //   编译期字面量不在内化表里、两处 `"k"` 是两个对象，用 == 会永不命中 ✗。
                ObjDict* dd = (ObjDict*)val_as_obj(obj);
                InlineDictCacheEntry* dic =
                    &vm.ic_dict_cache[ic_cache_index(ip_before) & (IC_DICT_CACHE_SIZE - 1)];
                if (likely(!dic->dead && dic->dict == dd && dic->slot >= 0 &&
                           dic->cap == (uint32_t)dd->capacity && dic->slot < dd->capacity &&
                           dict_ic_key_match(dd->entries[dic->slot].key, idx))) {
                    vm.ic_hits++;
                    dic->miss_streak = 0;
                    Value dv = dd->entries[dic->slot].value;
                    // 与 dict_get 的 is_valid_obj_value 同口径（值对象类型越界 ⇒ 视为不存在）
                    R(a) = (!val_is_obj(dv) ||
                            (val_as_obj(dv)->type >= OBJ_STRING && val_as_obj(dv)->type < OBJ_NONE))
                               ? dv
                               : val_null();
                    DISPATCH();
                }
                vm.ic_misses++;
                R(a) = dict_get(dd, idx);
                if (!dic->dead && dic->miss_streak < IC_DICT_MISS_LIMIT) {
                    int dslot = dict_slot_for(dd, idx);
                    if (dslot >= 0) {
                        dic->dict = dd;
                        dic->cap = (uint32_t)dd->capacity;
                        dic->slot = dslot;
                        dic->miss_streak++;
                    } else {
                        dic->dict = NULL;   // 数组部分的键 / 键不存在 ⇒ 不缓存
                    }
                } else if (!dic->dead) {
                    dic->dead = 1;   // 键在轮换 ⇒ 永久放弃（理由见 OP_INDEX_SET 处的注释）
                    dic->dict = NULL;
                }
                DISPATCH();
            }
            // .leno 模块：成员在 exports 字典里
            if (ot == OBJ_MODULE) {
                Value member = dict_get(((ObjModule*)val_as_obj(obj))->exports, idx);
                // ★ 兜底：exports 里没登记的类型名（cstruct / struct / enum）按名字查全局类型表。
                //   跨模块写 `a.Vec.malloc()` 时既没 use 类型名、exports 也没这条 ⇒
                //   此前拿到 null，报「类型 object 上不存在方法 'malloc'」✗
                if (val_is_null(member) && val_is_string(idx)) {
                    const char* nm = ((ObjString*)val_as_obj(idx))->chars;
                    ObjCStructDef* cdef = cstruct_def_find(nm);
                    if (cdef) {
                        member = val_obj((Object*)cdef);
                    } else {
                        ObjStructDef* sdef = struct_def_find(nm);
                        if (sdef) {
                            member = val_obj((Object*)sdef);
                        } else {
                            ObjEnumDef* edef = enum_def_find(nm);
                            if (edef) member = val_obj((Object*)edef);
                        }
                    }
                }
                R(a) = member;
                DISPATCH();
            }
            // 枚举定义对象：按成员名取成员值（`EnumName.Member`，含模块限定形态）
            if (ot == OBJ_ENUM_DEF) {
                if (unlikely(!val_is_string(idx))) {
                    if (!throw_runtime_error("枚举成员名必须是字符串", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjEnumDef* ed = (ObjEnumDef*)val_as_obj(obj);
                const char* mname = ((ObjString*)val_as_obj(idx))->chars;
                for (int mi = 0; mi < ed->member_count; mi++) {
                    if (ed->members[mi].name && strcmp(ed->members[mi].name, mname) == 0) {
                        // val_int_safe：超出 int48 时自动装箱成 bigint
                        R(a) = val_int_safe(ed->members[mi].value);
                        DISPATCH();
                    }
                }
                {
                    char emsg[256];
                    snprintf(emsg, sizeof(emsg), "枚举 '%s' 不存在成员 '%s'",
                             ed->name ? ed->name : "?", mname);
                    if (!throw_runtime_error(emsg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
            }
            // struct 实例：按**字段名**索引。
            //   语义分析把 `s.field` 生成成 obj["field"]（通用索引），因此字段读
            //   最终走到这里；缺了这个分支就报「下标访问: 对象不支持索引」。
            if (ot == OBJ_STRUCT) {
                if (unlikely(!val_is_string(idx))) {
                    if (!throw_runtime_error("struct 字段名必须是字符串", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjStruct* so = (ObjStruct*)val_as_obj(obj);
                int fi = struct_get_field_index(so->def, ((ObjString*)val_as_obj(idx))->chars);
                if (unlikely(fi < 0)) {
                    // ★ 不是字段 → 再试**方法名**：泛型函数里 `a.compareTo(b)`（a 的静态类型
                    //   只是受 face 约束的 T）编译成 obj["compareTo"] 访问，field_index 无从确定。
                    //   这里返回绑定方法（含 receiver），随后 CALL 即可正确调用。
                    ObjClosure* pre = NULL;
                    ObjFunction* mfn = NULL;
                    if (so->def && struct_method_lookup(so->def, (ObjString*)val_as_obj(idx), &pre, &mfn)) {
                        ObjClosure* cl = pre;
                        if (!cl) {
                            cl = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                            if (unlikely(!cl)) { runtime_error("内存分配失败", line); return -1; }
                            cl->function = mfn;
                            cl->upvalue_count = mfn->upvalue_count;
                            cl->type_param_count = 0;
                            cl->type_param_args = NULL;
                            for (int ui = 0; ui < mfn->upvalue_count; ui++) cl->upvalues[ui] = NULL;
                        }
                        ObjBoundMethod* bound = bound_closure_method_new(obj, cl);
                        if (unlikely(!bound)) { runtime_error("内存分配失败", line); return -1; }
                        R(a) = val_obj((Object*)bound);
                        DISPATCH();
                    }
                    char msg[256];
                    snprintf(msg, sizeof(msg), "struct 不存在字段 '%s'",
                             ((ObjString*)val_as_obj(idx))->chars);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                R(a) = struct_get_field(so, fi);
                DISPATCH();
            }
            // cstruct（C 布局）实例：同样按字段名索引（`v["x"]`）
            if (ot == OBJ_CSTRUCT) {
                if (unlikely(!val_is_string(idx))) {
                    if (!throw_runtime_error("cstruct 字段名必须是字符串", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjCStruct* co = (ObjCStruct*)val_as_obj(obj);
                int fi = cstruct_get_field_index(co->def, ((ObjString*)val_as_obj(idx))->chars);
                if (unlikely(fi < 0)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "cstruct 不存在字段 '%s'",
                             ((ObjString*)val_as_obj(idx))->chars);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                R(a) = vm_cstruct_field_read(co, fi);
                DISPATCH();
            }
            // cstruct 数组（malloc_array）：按整数索引取元素视图
            if (ot == OBJ_CSTRUCT_ARRAY) {
                if (unlikely(!val_is_num(idx))) {
                    if (!throw_runtime_error("cstruct 数组索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjCStructArray* ca = (ObjCStructArray*)val_as_obj(obj);
                int ci = (int)value_to_double(idx);
                if (unlikely(ci < 0 || ci >= ca->count)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "cstruct 数组索引越界: 索引 %d, 长度 %d", ci, ca->count);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjCStruct* elem = cstruct_array_get(ca, ci);
                if (unlikely(!elem)) {
                    if (!throw_runtime_error("cstruct 数组元素获取失败", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                R(a) = val_obj((Object*)elem);
                DISPATCH();
            }
            // cstruct 数组字段视图：按整数索引取元素视图
            if (ot == OBJ_CSTRUCT_ARRAY_VIEW) {
                if (unlikely(!val_is_num(idx))) {
                    if (!throw_runtime_error("cstruct 数组索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjCStructArrayView* view = (ObjCStructArrayView*)val_as_obj(obj);
                int ci = (int)value_to_double(idx);
                if (unlikely(ci < 0 || ci >= view->array_dim)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "cstruct 数组索引越界: 索引 %d, 长度 %d", ci, view->array_dim);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjCStruct* parent = view->cstruct;
                CStructFieldInfo* vf = &parent->def->fields[view->field_index];
                uint8_t* elem_addr = parent->data + vf->offset + (ci * view->element_size);
                // 元素本身是嵌套 cstruct ⇒ 非拥有视图；否则按元素类型取值
                if (view->element_type == TYPE_CSTRUCT) {
                    ObjCStructDef* edef = vf->struct_name ? cstruct_def_find(vf->struct_name) : NULL;
                    if (unlikely(!edef)) {
                        if (!throw_runtime_error("找不到 cstruct 数组元素类型定义", line)) return -1;
                        frame = &vm.frames[vm.frame_cnt - 1];
                        DISPATCH();
                    }
                    ObjCStruct* elem = (ObjCStruct*)gc_alloc(sizeof(ObjCStruct), OBJ_CSTRUCT);
                    if (unlikely(!elem)) { runtime_error("内存不足", line); return -1; }
                    elem->def = edef;
                    elem->data = elem_addr;
                    elem->owns_memory = 0;
                    R(a) = val_obj((Object*)elem);
                } else {
                    R(a) = vm_c_layout_read_scalar(elem_addr, view->element_type);
                }
                DISPATCH();
            }
        }

        if (!throw_runtime_error("下标访问: 对象不支持索引", line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    // —— 静态类型特化：Array[int] 索引 ——
    // 信任 codegen 守卫：R[B] 恒为 OBJ_ARRAY、R[C] 恒为 int48（codegen 只在能确认这两个
    // 条件时才发此指令）。故跳过 obj 判型 / 下标数字判型，直接 bounds-check + 取元素，
    // 比 OP_INDEX 的热路径少 2~3 次判型。仅越界时走错误兜底。
    LABEL(OP_INDEX_ARRAY_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjArray* arr = (ObjArray*)val_as_obj(R(b));
        int i2 = (int)val_as_int(R(c));
        if (likely((uint64_t)i2 < (uint64_t)arr->count)) {
            R(a) = arr->elements[i2];
            DISPATCH();
        }
        {
            int line = GET_CURRENT_LINE();
            char msg[128];
            snprintf(msg, sizeof(msg), "数组索引越界: 索引 %d, 长度 %d", i2, arr->count);
            if (!throw_runtime_error(msg, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
    }
    // —— 静态类型特化：Array[float] 索引 —— 语义同上
    LABEL(OP_INDEX_ARRAY_FLOAT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjArray* arr = (ObjArray*)val_as_obj(R(b));
        int i2 = (int)val_as_int(R(c));
        if (likely((uint64_t)i2 < (uint64_t)arr->count)) {
            R(a) = arr->elements[i2];
            DISPATCH();
        }
        {
            int line = GET_CURRENT_LINE();
            char msg[128];
            snprintf(msg, sizeof(msg), "数组索引越界: 索引 %d, 长度 %d", i2, arr->count);
            if (!throw_runtime_error(msg, line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
    }
    // OP_INDEX_SET A B C: R[B][R[C]] = R[A]
    LABEL(OP_INDEX_SET): {
        uint8_t* ip_before = frame->ip;   // 调用点标识（字典内联缓存的索引）
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj = R(b);
        Value idx = R(c);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_obj(obj))) {
            ObjType ot = val_as_obj(obj)->type;
            if (ot == OBJ_ARRAY) {
                ObjArray* arr = (ObjArray*)val_as_obj(obj);
                int i2 = val_is_num(idx) ? (int)value_to_double(idx) : -1;
                if (unlikely(i2 < 0 || i2 >= arr->count)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "数组索引越界: 索引 %d, 长度 %d", i2, arr->count);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                arr->elements[i2] = R(a);
                gc_write_barrier((Object*)arr, R(a));
                DISPATCH();
            }
            if (ot == OBJ_DICT) {
                // ★ 调用点内联缓存（见 leno_vm.h 的 InlineDictCacheEntry）：
                //   `d[k]=v` 原先每次都要走 dict_set 那 5~8 层跨 TU 调用。
                ObjDict* dd = (ObjDict*)val_as_obj(obj);
                Value nv2 = R(a);
                InlineDictCacheEntry* dic =
                    &vm.ic_dict_cache[ic_cache_index(ip_before) & (IC_DICT_CACHE_SIZE - 1)];
                if (likely(!dic->dead && dic->dict == dd && dic->slot >= 0 &&
                           dic->cap == (uint32_t)dd->capacity && dic->slot < dd->capacity &&
                           dict_ic_key_match(dd->entries[dic->slot].key, idx))) {
                    vm.ic_hits++;
                    dic->miss_streak = 0;
                    dd->entries[dic->slot].value = nv2;
                    gc_write_barrier((Object*)dd, nv2);
                    DISPATCH();
                }
                vm.ic_misses++;
                dict_set(dd, idx, nv2);
                if (!dic->dead && dic->miss_streak < IC_DICT_MISS_LIMIT) {
                    int dslot = dict_slot_for(dd, idx);
                    if (dslot >= 0) {
                        dic->dict = dd;
                        dic->cap = (uint32_t)dd->capacity;
                        dic->slot = dslot;
                        dic->miss_streak++;
                    } else {
                        dic->dict = NULL;
                    }
                } else if (!dic->dead) {
                    // 连续多次未命中 ⇒ 该调用点的键在轮换，永久放弃（dead 不可逆，
                    // 否则"每 N 次命中一次"会把计数清零、导致一直反复回填）
                    dic->dead = 1;
                    dic->dict = NULL;
                }
                DISPATCH();
            }
            // struct 实例：按字段名赋值（`s.field = v` 的通用索引形态）
            if (ot == OBJ_STRUCT) {
                if (unlikely(!val_is_string(idx))) {
                    if (!throw_runtime_error("struct 字段名必须是字符串", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjStruct* so = (ObjStruct*)val_as_obj(obj);
                int fi = struct_get_field_index(so->def, ((ObjString*)val_as_obj(idx))->chars);
                if (unlikely(fi < 0)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "struct 不存在字段 '%s'",
                             ((ObjString*)val_as_obj(idx))->chars);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                Value nv = R(a);
                TypeKind et = so->def->fields[fi].type;
                if (et == TYPE_FLOAT && val_is_int(nv)) {
                    nv = val_float((double)val_as_int(nv));
                } else if (et == TYPE_FLOAT && val_is_bigint(nv)) {
                    nv = val_float(bigint_to_double(val_as_bigint(nv)));
                }
                struct_set_field(so, fi, nv);
                gc_write_barrier((Object*)so, nv);
                DISPATCH();
            }
            // cstruct（C 布局）实例：按字段名赋值（`v["x"] = 1`）
            if (ot == OBJ_CSTRUCT) {
                if (unlikely(!val_is_string(idx))) {
                    if (!throw_runtime_error("cstruct 字段名必须是字符串", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjCStruct* co = (ObjCStruct*)val_as_obj(obj);
                int fi = cstruct_get_field_index(co->def, ((ObjString*)val_as_obj(idx))->chars);
                if (unlikely(fi < 0)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "cstruct 不存在字段 '%s'",
                             ((ObjString*)val_as_obj(idx))->chars);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                Value nv = R(a);
                TypeKind ft = co->def->fields[fi].type;
                if (val_is_int(nv) && (ft == TYPE_F32 || ft == TYPE_F64)) {
                    nv = val_float((double)val_as_int(nv));
                } else if (val_is_bigint(nv) && (ft == TYPE_F32 || ft == TYPE_F64)) {
                    nv = val_float(bigint_to_double(val_as_bigint(nv)));
                } else if (val_is_float(nv) && (ft >= TYPE_I8 && ft <= TYPE_U64)) {
                    nv = val_num(val_as_num(nv));
                }
                cstruct_set_field_value(co, fi, nv);
                DISPATCH();
            }
            // cstruct 数组字段视图：按整数索引写元素（`v.arr[0] = 1`）
            if (ot == OBJ_CSTRUCT_ARRAY_VIEW) {
                ObjCStructArrayView* view = (ObjCStructArrayView*)val_as_obj(obj);
                int ci = val_is_num(idx) ? (int)value_to_double(idx) : -1;
                if (unlikely(ci < 0 || ci >= view->array_dim)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "cstruct 数组索引越界: 索引 %d, 长度 %d", ci, view->array_dim);
                    if (!throw_runtime_error(msg, line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                ObjCStruct* parent = view->cstruct;
                CStructFieldInfo* vf = &parent->def->fields[view->field_index];
                uint8_t* elem_addr = parent->data + vf->offset + (ci * view->element_size);
                vm_c_layout_write_scalar(elem_addr, view->element_type, R(a));
                DISPATCH();
            }
        }

        if (!throw_runtime_error("下标赋值: 对象不支持索引", line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }
    // OP_SLICE A B C: R[A] = R[B][R[A+1] : R[A+2]]（结束索引**包含**；null 表示省略）
