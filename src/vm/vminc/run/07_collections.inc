    LABEL(OP_NEWARRAY): {
        int a = READ_A(), c = READ_C();
        ObjArray* arr = arr_new(c);
        if (!arr) { runtime_error("OP_NEWARRAY: arr_new 失败", GET_CURRENT_LINE()); return -1; }
        for (int i = 0; i < c; i++) {
            arr->elements[i] = R(a + 1 + i);
        }
        arr->count = c;

        // ★ 设置运行时类型信息（照原栈式 OP_ARRAY —— 栈式已弃用，只作来源注记）：
        //   元素类型全一致时记 Array[T]，空数组/混合类型留 NULL。`type()`、空数组的类型名、
        //   add 的类型检查都依赖它 —— 不设置的话 `arr.clear()` 之后 type(arr) 只剩 "Array"
        //   （元素类型信息丢失；本仓库这里要报 Array[int] ✓）。
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
    LABEL(OP_ARRAY_APPEND): {
        int a = READ_A(), b = READ_B();
        ObjArray* arr = (ObjArray*)val_as_obj(R(b));
        if (arr->count >= arr->capacity) { if (!arr_grow(arr)) { runtime_error("数组扩容失败：内存不足或元素数达上限（若在 for 遍历中不断 add，数组会无限增长）", GET_CURRENT_LINE()); return -1; } }
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
        // 长度规则见 vm_helpers.inc 的 vm_container_len（单一事实来源；本处原先是它的内联副本）
        R(a) = val_int(vm_container_len(R(b)));
        DISPATCH();
    }

    // OP_CMPJMP_ITER A B C —— for 容器迭代的条件融合（T13）
    // ----------------------------------------------------------------------------
    // 等价于 codegen 原先发的 `OP_LEN + OP_LT + OP_JMP_IF_FALSE`
    // （回边另有独立的 `OP_INC + OP_JMP`）—— 合并后每轮固定开销由 6 条降到 2 条：
    //     原先每轮: LEN, LT, JMP_IF_FALSE, ITER_GET, [体], INC, JMP
    //     现在每轮: ITER_GET, [体], CMPJMP_ITER(先自增 + 比较 + 真则跳回)
    // 操作数：A = 索引寄存器；B = 容器寄存器；
    //         C 只当标志：bit6 = 真则跳（回边用），bit5 = 先自增索引再测试（回边用）；
    //         紧随 4 字节字的低 16 位 = 跳转偏移（offset + 32768，与 CMPJMP 同约定）。
    // ★ 语义关键：长度仍在**每次测试时**读取（`vm_container_len`，与 OP_LEN 逐支同口径）
    //   ⇒ `for a to x { a.add(..) }` 这类"迭代中增删容器"的行为一字不变
    //   （实测：2 元素起跑、每轮追加一个 ⇒ 6 轮结束、最终 len=6）。
    //   把长度提到循环外才是语义改动（那会变成 2 轮），不做。
    // ★ 索引寄存器的值由本指令自己维护：codegen 用 `LOADI idx,0` 初始化，此后**只有**
    //   这条指令会写它（用户可见的索引变量是另一个槽）⇒ 恒为 int，无需类型检查。
    LABEL(OP_CMPJMP_ITER): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        int bx = ((int)frame->ip[0] << 8) | (int)frame->ip[1];
        ADVANCE();                          /* 跳过携带偏移的第二个字 */
        int64_t idx = val_as_int(R(a));
        if (c & 0x20) {                     /* 回边：先自增（位置与原先那条 OP_INC 一致） */
            idx++;
            R(a) = VM_INT_RESULT(idx);      /* 与 OP_INC 同口径（超 int48 提升 bigint） */
        }
        int truthy = (idx < vm_container_len(R(b)));
        if (truthy == ((c & 0x40) != 0)) frame->ip += (bx - 32768);
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
    // OP_DICT_SET: 仅 AST_DICT 字面量 >255 项 / 寄存器不够时的兜底路径发（codegen_expr.c）；
    //   常规 `d[k]=v` 走 OP_INDEX_SET 的字典分支（内联缓存接在那边），`d[k]` 读走 OP_INDEX。
    //   接收者来自刚 OP_NEWDICT 的全新字典 ⇒ 恒为 OBJ_DICT，信任之，不做额外探测。
    LABEL(OP_DICT_SET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjDict* dict = (ObjDict*)val_as_obj(R(b));
        dict_set(dict, R(c), R(a));
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
                // ★ 2026-10-03 快路径：**非负 int 键 → 字典的数组部分**（O(1) 直取，同 Lua 的 luaH_getint）。
                //   为什么必须专门处理：IC 对这类键**结构性无效** —— 回填走 `dict_slot_for`，
                //   而它对数组部分的键恒返回 -1（object_dict.c:184）⇒ 把 dic->dict 清空 ⇒
                //   每次访问都"探 IC（必 miss）+ 跨 TU dict_get + 再 dict_slot_for" ✗
                //   口径与 dict_get 的数组分支**逐条对齐**：越界 / 洞 ⇒ 落到下面（哈希里必然没有 ⇒ null ✓）；
                //   值合法性过滤同 IC 命中路径（对象类型越界 ⇒ 视为不存在 ✓）
                if (likely(val_is_int(idx))) {
                    int64_t ii = val_as_int(idx);
                    if (ii >= 0 && ii < dd->asize) {
                        Value dv = dd->array[ii];
                        if (!val_is_null(dv)) {
                            R(a) = (!val_is_obj(dv) ||
                                    (val_as_obj(dv)->type >= OBJ_STRING && val_as_obj(dv)->type < OBJ_NONE))
                                       ? dv
                                       : val_null();
                            DISPATCH();
                        }
                    }
                    // 越界 / 洞：数组部分不会有 ⇒ 结果必为 null（仍走下面统一取一次，保持单点语义 ✓）
                    R(a) = dict_get_slot(dd, idx, NULL);
                    DISPATCH();
                }
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
                // ★ 2026-10-03：改调 dict_get_slot —— 一次查找同时给值与槽位，
                //   省掉原先 miss 之后**又查一遍**的 dict_slot_for（同一次访问两次查找 ✗）
                int dslot = -1;
                R(a) = dict_get_slot(dd, idx, &dslot);
                if (!dic->dead && dic->miss_streak < IC_DICT_MISS_LIMIT) {
                    if (dslot >= 0) {
                        dic->dict = dd;
                        dic->cap = (uint32_t)dd->capacity;
                        dic->slot = dslot;
                        dic->miss_streak++;
                    } else {
                        dic->dict = NULL;   // 键不存在 ⇒ 不缓存
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
    //
    // ⚠⚠ v3.2.8：上面那句"信任 codegen"**有个洞** —— "静态类型是 `Array[T]`"并不保证
    //   运行期不是 null（`Array[int] arr = null` 是**合法**写法）。实测 `arr[0]` 走这条
    //   特化路径 ⇒ `val_as_obj(null)` + `arr->count` 野指针读 ⇒ **0xC0000005 崩溃**
    //   （用例 examples/改进测试/test_null_index.leno 就是为验证这个而存在的）。
    //   ⇒ 4 条特化指令（INT / FLOAT / IMM / SET_INT）进来先挡一次 null/非数组。
    //   用宏收敛：4 处各写一遍判断，迟早漏一条，而漏掉的那条就是一个崩溃。
    #define ARRAY_SPECIALIZED_NULL_GUARD(reg)                                                       \
        if (unlikely(!val_is_obj(R(reg)) || val_as_obj(R(reg))->type != OBJ_ARRAY)) {               \
            int gline = GET_CURRENT_LINE();                                                         \
            if (!throw_runtime_error("下标访问: 对象是 null 或不是数组 —— 请先判空", gline)) { return -1; } \
            frame = &vm.frames[vm.frame_cnt - 1];                                                   \
            DISPATCH();                                                                             \
        }
    LABEL(OP_INDEX_ARRAY_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ARRAY_SPECIALIZED_NULL_GUARD(b);
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
        ARRAY_SPECIALIZED_NULL_GUARD(b);
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
    // —— 静态类型特化 + **立即数**下标：Array[int]/Array[float] 的字面量下标读 ——
    //   与上面 OP_INDEX_ARRAY_INT/_FLOAT 逐条等价（那两条本来就只有带不带类型名之分），
    //   唯一区别是 C 字段是**无符号** 0..255 的字面量下标 ⇒ 少一条"把常量装进寄存器"的 LOADI。
    //   ★ 为什么是无符号而不是 int8：口径对齐 Lua 5.5 的 `OP_GETI`
    //   （`lcode.c` 的 `isCint()` 用 `l_castS2U` 比 `MAXARG_C` = 255 ⇒ **负字面量下标
    //   直接落回寄存器路径**，`GETI` 压根不表示负数）。对 Leno 而言负半边更是浪费：
    //   `arr[-1]` 在本语言里**必然越界**（下面的无符号比较恒假）⇒ 用立即数只是把
    //   注定报错的路径省一条指令，却把 128..255 这段**真会用到的**（查找表 / 定长数组）
    //   挤回两条指令。故取 [0,255] 与 Lua 同口径。
    //   越界处理与寄存器版**完全一致**（同一个无符号比较 + 同一句报错文案）。
    LABEL(OP_INDEX_ARRAY_IMM): {
        int a = READ_A(), b = READ_B();
        int i2 = (int)READ_C();   // 无符号 0..255
        ARRAY_SPECIALIZED_NULL_GUARD(b);
        ObjArray* arr = (ObjArray*)val_as_obj(R(b));
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
    // 与 dict_get 的 is_valid_obj_value 同口径（值对象类型越界 ⇒ 视为不存在）
#define DICT_VALUE_OR_NULL(dv)                                                         \
    (((!val_is_obj(dv)) ||                                                             \
      (val_as_obj(dv)->type >= OBJ_STRING && val_as_obj(dv)->type < OBJ_NONE))         \
         ? (dv)                                                                        \
         : val_null())

    // ★ 2026-10-03 字典 int 键专用指令（对齐数组的 OP_INDEX_ARRAY_INT / OP_INDEX_SET_ARRAY_INT）：
    //   codegen 已核实 R[B] 静态是 Dict、R[C] 静态是 int ⇒ 省掉 `val_is_obj` + OBJ_* 判型 +
    //   `val_is_int` 检查。语义与 OP_INDEX / OP_INDEX_SET 的字典分支**逐条同口径**：
    //     非负 int 且落在**数组部分** ⇒ 直取/直写；越界、洞、负数 ⇒ 走通用查找/写入 ✓
    //   （`(uint64_t)` 无符号比较一并挡掉负数，与既有数组特化同一手法 ✓）
    LABEL(OP_INDEX_DICT_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjDict* dd = (ObjDict*)val_as_obj(R(b));
        int64_t ii = val_as_int(R(c));
        if ((uint64_t)ii < (uint64_t)dd->asize) {
            Value dv = dd->array[ii];
            if (!val_is_null(dv)) {
                R(a) = DICT_VALUE_OR_NULL(dv);
                DISPATCH();
            }
        }
        R(a) = dict_get_slot(dd, R(c), NULL);   // 越界 / 洞 / 负数 ⇒ 通用（哈希部分）✓
        DISPATCH();
    }

    LABEL(OP_INDEX_SET_DICT_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ObjDict* dd = (ObjDict*)val_as_obj(R(b));
        Value nv = R(a);
        int64_t ii = val_as_int(R(c));
        if ((uint64_t)ii < (uint64_t)dd->asize && !val_is_null(dd->array[ii])) {
            dd->array[ii] = nv;                 // 键已存在 ⇒ 与 dict_set 的"更新现有键"分支等价 ✓
            gc_write_barrier((Object*)dd, nv);
            DISPATCH();
        }
        dict_set(dd, R(c), nv);                 // 新键 / 越界 / 负数 ⇒ 通用（扩容 + 记插入顺序）✓
        DISPATCH();
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
                // ★ 2026-10-03 快路径：**非负 int 键 → 数组部分**。IC 对这类键结构性无效
                //   （`dict_slot_for` 对数组部分的键恒 -1 ⇒ 回填时把 dic->dict 清空 ⇒ 每次必 miss ✗）
                //   ⇒ 直接处理、不探 IC、不回填：
                //     ① 槽位**已有非 null 值**（键已存在）⇒ 直写数组 + 写屏障 —— 与 dict_set 的
                //        "更新现有键"分支逐条等价（`is_new = val_is_null(...)` 为假 ⇒ 不动 acount、
                //        不记插入顺序 ✓）
                //     ② 否则（新键 / 越界 / 洞）⇒ 交给 dict_set（要判稀疏、扩容、acount++、记插入顺序 ✓）
                if (likely(val_is_int(idx))) {
                    int64_t ii = val_as_int(idx);
                    if (ii >= 0 && ii < dd->asize && !val_is_null(dd->array[ii])) {
                        dd->array[ii] = nv2;
                        gc_write_barrier((Object*)dd, nv2);
                        DISPATCH();
                    }
                    dict_set(dd, idx, nv2);
                    DISPATCH();
                }
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
    // OP_INDEX_SET_ARRAY_INT A B C: R[B][R[C]] = R[A]
    //   静态类型特化版（对齐读路径的 OP_INDEX_ARRAY_INT）：codegen 已核实 R[B] 是
    //   Array[int]、R[C] 是 int48 ⇒ 跳过 obj 判型（val_is_obj + `->type` 分流）与下标
    //   数字判型，下标直接 val_as_int 解码 —— 通用版那句 `(int)value_to_double(idx)`
    //   对 int48 是「符号扩展 → cvtsi2sd → cvttsd2si」两次浮点往返，这里一次都不用。
    //   ★ 越界检查与 GC 写屏障与通用版**逐条一致**：下标为负时经 (uint64_t) 转型必然
    //     ≥ count ⇒ 落到同一个「数组索引越界」分支（报错文本、行号来源都相同）。
    //     口径对齐读路径的 OP_INDEX_ARRAY_INT（也是 (uint64_t) 无符号比较）。
    LABEL(OP_INDEX_SET_ARRAY_INT): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        ARRAY_SPECIALIZED_NULL_GUARD(b);
        ObjArray* arr = (ObjArray*)val_as_obj(R(b));
        int i2 = (int)val_as_int(R(c));
        if (unlikely((uint64_t)i2 >= (uint64_t)arr->count)) {
            int line = GET_CURRENT_LINE();
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
    // OP_INDEX_SET_ARRAY_IMM A B C: R[B][C] = R[A]（C 是**无符号立即数** 0..255）
    //   与读路径 OP_INDEX_ARRAY_IMM 完全对称：`a[0] = v` 原先是
    //   「OP_LOADI 装下标 0 → OP_INDEX_SET_ARRAY_INT」两条派发，下标是编译期字面量时
    //   那条 LOADI 纯属浪费（Lua 的 SETI 同样把小下标编在指令里）。
    //   ★ 越界检查 / GC 写屏障 / 报错文案与寄存器版 OP_INDEX_SET_ARRAY_INT **逐条一致**
    //     （同一个 (uint64_t) 无符号比较 ⇒ 报错文本、行号来源都相同）。
    LABEL(OP_INDEX_SET_ARRAY_IMM): {
        int a = READ_A(), b = READ_B();
        int i2 = (int)READ_C();   // 无符号 0..255
        ARRAY_SPECIALIZED_NULL_GUARD(b);
        ObjArray* arr = (ObjArray*)val_as_obj(R(b));
        if (unlikely((uint64_t)i2 >= (uint64_t)arr->count)) {
            int line = GET_CURRENT_LINE();
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
    #undef ARRAY_SPECIALIZED_NULL_GUARD
    // OP_SLICE A B C: R[A] = R[B][R[A+1] : R[A+2]]（结束索引**包含**；null 表示省略）
