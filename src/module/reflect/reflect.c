// reflect —— 运行期**只读**反射
// ============================================================================
// 为什么有这个模块（2026-11-23）
// ----------------------------------------------------------------------------
// 编译器早就把类型的完整元数据写进了字节码与运行期定义：
//   · ObjStructDef：字段名、字段声明类型、Array/Dict 的完整类型树（ContainerTypeMeta）、
//     可空标记、泛型实参名、方法表、impl 的 face 名；
//   · ObjCStructDef：C 布局字段名、类型、offset、数组维度、位域宽、packed 对齐；
//   · ObjFaceDef：方法签名。
// `jsons.fill` / `jsons.encode` 就是纯元数据驱动的通用逻辑。但那层元数据此前
// **只有 C 侧能读**，脚本里拿不到 ⇒ 每个 struct 都要手抄一份 to_dict / 字段名列表 /
// 表单标签 / 表格列映射，抄漏一处不报错、只是行为不对。
// 本模块把那层已经存在的元数据**原样开放**出来，不引入任何新机制。
//
// 为什么同时覆盖 cstruct（数据导向那一半）
// ----------------------------------------------------------------------------
// cstruct 是"需要缓存局部性时显式下探"的那条路（连续内存 + offset + 位域 + packed）。
// 但此前要用它就得把字段名硬编码：泛型缓冲、批量读写、列式统计只能一类一写。
// cstruct 的 offset/宽度/类型本来就在 ObjCStructDef 里 ⇒ 开放出来之后，
// "按字段名取一列 / 按声明类型分派"就能写一次给所有 cstruct 用。
//
// 设计口径（v1 有意收窄 —— 宁可少给，也不给会静默错的）
// ----------------------------------------------------------------------------
// · **只有只读**。写一律走 `jsons.fill(target, dict)`（按字段**声明类型**转换后写入）
//   或直接赋值 `s.field = x`（编译期检查）。`reflect.set` 会绕过类型检查
//   （`set(s, "age", "abc")` 要跑到很远才炸，甚至不炸）——与本项目"写错就要报出来"
//   的口径相反，故不提供。
// · **不做 enum 值反查**。本语言的 enum 成员运行期就是 int 常量（不带载荷时连对象
//   都没有）⇒ `reflect.enum_members(1)` 无从知道 1 属于哪个 enum。给一个靠猜的 API
//   比不给更糟；要按名取成员请直接用 `Color.RED` 这类编译期常量。
//   带载荷 enum 在运行期是"降级 struct"（ObjStructDef::enum_sig 非空），其字段是
//   tag/p0/p1 这类**内部表示** ⇒ fields() 对它们返回空数组，避免把实现细节当契约泄露。
// · **参数形状错就报错，字段名不存在只在 has_field 里返回 false**。
//   `fields(123)` / `get(s, 123)` 一定是写错了 ⇒ 抛明确错误；
//   而"这个 struct 有没有这个字段"是正常的探测需求 ⇒ 用 has_field，它不抛。
// · cstruct 字段读取**必须**先过 `cstruct_is_alive`：直接调 cstruct_get_field_value
//   会读 obj->data + offset —— 释放后就是 use-after-free。VM 的字段指令都做了这个
//   判定，reflect 是新入口，不能漏（否则等于给压测补上的 UAF 防线开了个后门）。
// ============================================================================

#include "include/native.h"
#include "include/leno_value.h"
#include <stdio.h>
#include <string.h>

// ==================== 内部辅助 ====================

// 反射目标：GC struct（含带载荷 enum 的降级 struct）或 C 布局 cstruct。
typedef struct {
    int is_cstruct;         // 0 = ObjStruct，1 = ObjCStruct
    ObjStruct* s;           // is_cstruct == 0 时有效
    ObjStructDef* sdef;
    ObjCStruct* c;          // is_cstruct == 1 时有效
    ObjCStructDef* cdef;
} Target;

// 解析反射目标；不是 struct/cstruct ⇒ 已报错，返回 0。
//   ⚠ 对 int/array/dict 调 fields()/get() 一定是写错了 ⇒ 明确报错，不静默给空结果。
static int resolve_target(int argc, Value* args, const char* who, Target* t) {
    memset(t, 0, sizeof(*t));
    if (argc < 1) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: 需要一个 struct 或 cstruct 实例", who);
        native_throw_error(msg);
        return 0;
    }
    Value v = args[0];
    if (val_is_obj(v)) {
        Object* o = val_as_obj(v);
        if (o->type == OBJ_STRUCT) {
            ObjStruct* s = (ObjStruct*)o;
            if (s->def) {
                t->is_cstruct = 0;
                t->s = s;
                t->sdef = s->def;
                return 1;
            }
        } else if (o->type == OBJ_CSTRUCT) {
            ObjCStruct* c = (ObjCStruct*)o;
            if (c->def) {
                t->is_cstruct = 1;
                t->c = c;
                t->cdef = c->def;
                return 1;
            }
        }
    }
    {
        char msg[200];
        const char* kind = "unknown";
        if (val_is_null(v)) kind = "null";
        else if (val_is_bool(v)) kind = "bool";
        else if (val_is_int(v)) kind = "int";
        else if (val_is_float(v)) kind = "float";
        else if (val_is_string(v)) kind = "string";
        else if (val_is_obj(v)) {
            switch (val_as_obj(v)->type) {
            case OBJ_ARRAY: kind = "array"; break;
            case OBJ_DICT: kind = "dict"; break;
            default: kind = "object"; break;
            }
        }
        snprintf(msg, sizeof(msg), "%s: 需要一个 struct 或 cstruct 实例，实际是 %s",
                 who, kind);
        native_throw_error(msg);
    }
    return 0;
}

// 有效字段数。带载荷 enum 的降级 struct 返回 0（tag/p0 是内部表示，不当契约暴露）。
static int target_field_count(const Target* t) {
    if (t->is_cstruct) return t->cdef->field_count;
    return t->sdef->enum_sig ? 0 : t->sdef->field_count;
}

static const char* target_field_name(const Target* t, int i) {
    if (t->is_cstruct) return t->cdef->fields[i].name;
    return t->sdef->fields[i].name;
}

static int target_field_index(const Target* t, const char* name) {
    if (t->is_cstruct) return cstruct_get_field_index(t->cdef, name);
    if (t->sdef->enum_sig) return -1;
    return struct_get_field_index(t->sdef, name);
}

static const char* target_type_name(const Target* t) {
    if (t->is_cstruct) return t->cdef->name ? t->cdef->name : "cstruct";
    return t->sdef->name ? t->sdef->name : "struct";
}

// 运行期宽类别名。**对任意 Value 都有定义** —— kind() 是纯查询，不该因为
// "值长得怪"就抛错。
static const char* runtime_kind_of(Value v) {
    if (val_is_null(v)) return "null";
    if (val_is_bool(v)) return "bool";
    if (val_is_int(v)) return "int";
    if (val_is_float(v)) return "float";
    if (!val_is_obj(v)) return "unknown";
    switch (val_as_obj(v)->type) {
    case OBJ_STRING: return "string";
    case OBJ_ARRAY: return "array";
    case OBJ_DICT: return "dict";
    case OBJ_BIGINT: return "bigint";
    case OBJ_RANGE: return "range";
    case OBJ_STRUCT: {
        ObjStructDef* def = ((ObjStruct*)val_as_obj(v))->def;
        // 带载荷 enum 的降级 struct：对外叫 enum，不叫 struct
        return (def && def->enum_sig) ? "enum" : "struct";
    }
    case OBJ_CSTRUCT: return "cstruct";
    case OBJ_STRUCT_DEF: return "struct_def";
    case OBJ_ENUM_DEF: return "enum_def";
    case OBJ_FACE_DEF: return "face_def";
    case OBJ_CSTRUCT_DEF: return "cstruct_def";
    case OBJ_FILE: return "File";
    case OBJ_SOCKET: return "Socket";
    case OBJ_CHANNEL: return "Channel";
    case OBJ_THREAD: return "Thread";
    case OBJ_FUTURE: return "Future";
    case OBJ_COROUTINE: return "coroutine";
    case OBJ_MODULE: return "module";
    case OBJ_FFI_POINTER: return "Ptr";
    case OBJ_CLOSURE:
    case OBJ_FUNCTION:
    case OBJ_BOUND_METHOD:
    case OBJ_NATIVE: return "func";
    default: return "object";
    }
}

// 声明类型的可读写法 —— 风格与 type.c 的 type_to_string **一致**：
//   `Array[struct Item]` / `Dict[string, int]` / `int?`（同一个类型在报错、LSP hover
//   与这里看到的是同一种写法，免得又多一套方言）。
//   C 布局那几种宽度类型也在这里给名字 —— 数据导向场景下 `i32` 与 `int`
//   的区分是**要紧的**（前者是 4 字节定宽，后者是 int48 装箱）。
static const char* decl_kind_desc(TypeKind k) {
    switch (k) {
    case TYPE_INT: return "int";
    case TYPE_FLOAT: return "float";
    case TYPE_STRING: return "string";
    case TYPE_BOOL: return "bool";
    case TYPE_BIGINT: return "bigint";
    case TYPE_NULL: return "null";
    case TYPE_ANY: return "any";
    case TYPE_ARRAY: return "Array";
    case TYPE_DICT: return "Dict";
    case TYPE_FILE: return "File";
    case TYPE_PTR: return "Ptr";
    case TYPE_PTR_GENERIC: return "Ptr";
    case TYPE_STRUCT: return "struct";
    case TYPE_FACE: return "face";
    case TYPE_ENUM: return "enum";
    case TYPE_SOCKET: return "Socket";
    case TYPE_CHANNEL: return "Channel";
    case TYPE_THREAD: return "Thread";
    case TYPE_FUTURE: return "Future";
    // ---- C 布局（cstruct 字段）----
    case TYPE_CSTRUCT: return "cstruct";
    case TYPE_I8: return "i8";
    case TYPE_U8: return "u8";
    case TYPE_I16: return "i16";
    case TYPE_U16: return "u16";
    case TYPE_I32: return "i32";
    case TYPE_U32: return "u32";
    case TYPE_I64: return "i64";
    case TYPE_U64: return "u64";
    case TYPE_F32: return "f32";
    case TYPE_F64: return "f64";
    case TYPE_C_INT: return "c_int";
    case TYPE_C_UINT: return "c_uint";
    case TYPE_C_LONG: return "c_long";
    case TYPE_C_ULONG: return "c_ulong";
    case TYPE_C_LONGLONG: return "c_longlong";
    case TYPE_C_ULONGLONG: return "c_ulonglong";
    case TYPE_C_SIZE: return "c_size";
    case TYPE_C_SSIZE: return "c_ssize";
    case TYPE_STR8: return "str8";
    case TYPE_STR16: return "str16";
    case TYPE_CFUNC: return "cfunc";
    case TYPE_CLIB: return "clib";
    default: return "unknown";
    }
}

typedef struct {
    char* buf;
    size_t cap;
    size_t pos;
} TypeBuf;

static void tb_put(TypeBuf* b, const char* s) {
    if (!s) return;
    size_t n = strlen(s);
    if (b->pos + n >= b->cap) n = (b->cap > 0 && b->pos < b->cap) ? (b->cap - 1 - b->pos) : 0;
    if (n > 0) {
        memcpy(b->buf + b->pos, s, n);
        b->pos += n;
    }
    b->buf[b->pos] = '\0';
}

// 容器声明类型树（2026-11-15 起字段带完整树）：逐层展开。
static void tb_container(TypeBuf* b, const ContainerTypeMeta* m) {
    if (!m) { tb_put(b, "unknown"); return; }
    switch (m->type) {
    case TYPE_ARRAY:
        tb_put(b, "Array[");
        tb_container(b, m->element);
        tb_put(b, "]");
        break;
    case TYPE_DICT:
        tb_put(b, "Dict[");
        tb_container(b, m->key);
        tb_put(b, ", ");
        tb_container(b, m->value);
        tb_put(b, "]");
        break;
    case TYPE_STRUCT:
        tb_put(b, "struct ");
        tb_put(b, m->struct_name ? m->struct_name : "?");
        break;
    default:
        tb_put(b, decl_kind_desc(m->type));
        break;
    }
}

// GC struct 单个字段的声明类型串。
static void field_type_desc(const StructFieldInfo* f, TypeBuf* b) {
    if (!f) return;
    if (f->container) {
        tb_container(b, f->container);
    } else if (f->type == TYPE_ARRAY) {
        // 元素类型未指定（老字节码 / 裸 Array）⇒ 只写 `Array`，不编一个假的元素类型
        if (f->element_type == TYPE_UNKNOWN && !f->element_struct_name) {
            tb_put(b, "Array");
        } else {
            tb_put(b, "Array[");
            if (f->element_type == TYPE_STRUCT && f->element_struct_name) {
                tb_put(b, "struct ");
                tb_put(b, f->element_struct_name);
            } else {
                tb_put(b, decl_kind_desc(f->element_type));
            }
            tb_put(b, "]");
        }
    } else if (f->type == TYPE_DICT) {
        if (f->dict_key_type == TYPE_UNKNOWN && f->dict_value_type == TYPE_UNKNOWN) {
            tb_put(b, "Dict");
        } else {
            tb_put(b, "Dict[");
            tb_put(b, decl_kind_desc(f->dict_key_type));
            tb_put(b, ", ");
            if (f->dict_value_type == TYPE_STRUCT && f->dict_value_struct_name) {
                tb_put(b, "struct ");
                tb_put(b, f->dict_value_struct_name);
            } else {
                tb_put(b, decl_kind_desc(f->dict_value_type));
            }
            tb_put(b, "]");
        }
    } else if (f->type == TYPE_STRUCT && f->struct_type_name) {
        tb_put(b, "struct ");
        tb_put(b, f->struct_type_name);
    } else {
        tb_put(b, decl_kind_desc(f->type));
    }
    if (f->nullable) tb_put(b, "?");
}

// cstruct 单个字段的声明类型串：宽度值类型本身就是契约（i32 ≠ int），
// 数组写成 `i32[4]`、位域保留 `: 3`、Ptr[T] 写出元素类型。
static void cstruct_field_type_desc(const CStructFieldInfo* f, TypeBuf* b) {
    if (!f) return;
    if (f->array_dim > 0) {
        tb_put(b, decl_kind_desc(f->type));
        char dim[24];
        snprintf(dim, sizeof(dim), "[%d]", f->array_dim);
        tb_put(b, dim);
    } else if (f->type == TYPE_CSTRUCT && f->struct_name) {
        tb_put(b, "struct ");
        tb_put(b, f->struct_name);
    } else if (f->type == TYPE_PTR_GENERIC) {
        tb_put(b, "Ptr[");
        tb_put(b, decl_kind_desc(f->element_type));
        tb_put(b, "]");
    } else {
        tb_put(b, decl_kind_desc(f->type));
    }
    if (f->bit_width > 0) {
        char w[24];
        snprintf(w, sizeof(w), " : %d", f->bit_width);
        tb_put(b, w);
    }
}

// 往数组尾部追加（写屏障与 object_array.c 同口径，防止老年代数组漏 mark 年轻代）。
static void arr_push_value(ObjArray* arr, Value v) {
    if (arr->count >= arr->capacity) {
        if (!arr_grow(arr)) return;
    }
    arr->elements[arr->count++] = v;
    gc_write_barrier((Object*)arr, v);
}

// 参数校验：第 idx 个实参必须是字符串，取它的 chars。
//   返回 NULL 表示已报错（native_throw_error 不跳转，调用点仍要 return）。
static const char* arg_as_cstr(int argc, Value* args, int idx, const char* who, const char* what) {
    if (idx >= argc || !val_is_string(args[idx])) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: %s必须是字符串", who, what);
        native_throw_error(msg);
        return NULL;
    }
    return ((ObjString*)val_as_obj(args[idx]))->chars;
}

// 字段名不存在时的统一报错（带可用字段，比单纯"没有该字段"更能自纠）。
static void throw_no_such_field(const Target* t, const char* who, const char* name) {
    char msg[512];
    int pos = snprintf(msg, sizeof(msg), "%s: %s 没有字段 '%s'（可用: ",
                       who, target_type_name(t), name);
    if (!t->is_cstruct && t->sdef->enum_sig) {
        pos += snprintf(msg + pos, sizeof(msg) - pos, "无 —— 带载荷 enum 请用 case 匹配");
    } else {
        int n = target_field_count(t);
        for (int i = 0; i < n && i < 8; i++) {
            const char* fn = target_field_name(t, i);
            if (!fn) continue;
            pos += snprintf(msg + pos, sizeof(msg) - pos, "%s%s",
                            i > 0 ? ", " : "", fn);
        }
        if (n > 8) snprintf(msg + pos, sizeof(msg) - pos, ", …");
    }
    native_throw_error(msg);
}

// ==================== 模块方法 ====================

// reflect.kind(v) -> string
static Value reflect_kind_func(int argc, Value* args) {
    (void)argc;
    const char* k = runtime_kind_of(args[0]);
    return val_obj((Object*)str_new(k, (int)strlen(k)));
}

// reflect.type_name(v) -> string
//   有声明名的值给出声明名（struct Song → "Song"、cstruct Vec2 → "Vec2"、
//   带载荷 enum Msg → "Msg"）；其余值与 kind 同名（int → "int"）。
static Value reflect_type_name_func(int argc, Value* args) {
    (void)argc;
    Value v = args[0];
    const char* name = NULL;
    if (val_is_obj(v)) {
        Object* o = val_as_obj(v);
        if (o->type == OBJ_STRUCT) {
            ObjStructDef* def = ((ObjStruct*)o)->def;
            if (def && def->name) name = def->name;
        } else if (o->type == OBJ_CSTRUCT) {
            ObjCStructDef* def = ((ObjCStruct*)o)->def;
            if (def && def->name) name = def->name;
        }
    }
    if (!name) name = runtime_kind_of(v);
    return val_obj((Object*)str_new(name, (int)strlen(name)));
}

// reflect.fields(v) -> Array[string]   （按**声明顺序**，与 jsons 的编码顺序同源）
static Value reflect_fields_func(int argc, Value* args) {
    Target t;
    if (!resolve_target(argc, args, "reflect.fields", &t)) return val_null();

    int n = target_field_count(&t);
    ObjArray* arr = arr_new(n > 0 ? n : 1);
    if (!arr) return val_null();
    Value av = val_obj((Object*)arr);
    gc_push_root(&av);   // 后续 str_new 可能触发 GC
    for (int i = 0; i < n; i++) {
        const char* fn = target_field_name(&t, i);
        if (!fn) continue;
        ObjString* s = str_new(fn, (int)strlen(fn));
        if (!s) break;
        arr_push_value(arr, val_obj((Object*)s));
    }
    gc_pop_root();
    return av;
}

// reflect.field_count(v) -> int
static Value reflect_field_count_func(int argc, Value* args) {
    Target t;
    if (!resolve_target(argc, args, "reflect.field_count", &t)) return val_null();
    return val_int(target_field_count(&t));
}

// reflect.has_field(v, name) -> bool   （探测用：字段不存在返回 false，**不抛**）
static Value reflect_has_field_func(int argc, Value* args) {
    Target t;
    if (!resolve_target(argc, args, "reflect.has_field", &t)) return val_null();
    const char* name = arg_as_cstr(argc, args, 1, "reflect.has_field", "字段名");
    if (!name) return val_null();
    return val_bool(target_field_index(&t, name) >= 0);
}

// reflect.get(v, name) -> any
//   返回字段的**原值**（引用类型即同一对象）；字段名不存在 ⇒ 报错并列出可用字段。
//   为什么不存在要报错：拼错字段名是最常见的用法错误，静默给 null 会让
//   "读不到"和"值本来就是 null"混在一起（本项目反复踩过的同一类坑）。
static Value reflect_get_func(int argc, Value* args) {
    Target t;
    if (!resolve_target(argc, args, "reflect.get", &t)) return val_null();
    const char* name = arg_as_cstr(argc, args, 1, "reflect.get", "字段名");
    if (!name) return val_null();

    int idx = target_field_index(&t, name);
    if (idx < 0) {
        throw_no_such_field(&t, "reflect.get", name);
        return val_null();
    }
    if (t.is_cstruct) {
        // 必须先判存活：cstruct_get_field_value 直接 data + offset，
        // 释放后读就是 use-after-free（VM 字段指令都有这道检查，新入口不能漏）
        if (!cstruct_is_alive(t.c)) {
            native_throw_error("reflect.get: cstruct 数据已释放（free/free_all 之后不能再读字段）");
            return val_null();
        }
        return cstruct_get_field_value(t.c, idx);
    }
    return t.s->field_values[idx];
}

// reflect.field_type(v, name) -> string
//   字段的**声明类型**按 Leno 写法给串：`Array[int]` / `Dict[string, struct Item]` /
//   `int?` / cstruct 的 `i32` / `f32[4]` / `u32 : 3`。
//   用途：通用表单 / 表格列 / 文档 / 按类型分派，此前只能手写死。
static Value reflect_field_type_func(int argc, Value* args) {
    Target t;
    if (!resolve_target(argc, args, "reflect.field_type", &t)) return val_null();
    const char* name = arg_as_cstr(argc, args, 1, "reflect.field_type", "字段名");
    if (!name) return val_null();

    int idx = target_field_index(&t, name);
    if (idx < 0) {
        throw_no_such_field(&t, "reflect.field_type", name);
        return val_null();
    }

    char buf[256];
    TypeBuf tb = { buf, sizeof(buf), 0 };
    buf[0] = '\0';
    if (t.is_cstruct) {
        cstruct_field_type_desc(&t.cdef->fields[idx], &tb);
    } else {
        field_type_desc(&t.sdef->fields[idx], &tb);
    }
    return val_obj((Object*)str_new(buf, (int)tb.pos));
}

// reflect.methods(v) -> Array[string]
//   声明在 struct 体内的方法名（按声明顺序）。通用"有没有这个方法"判断 / 文档生成用。
//   cstruct / 带载荷 enum 没有用户方法 ⇒ 空数组（不是错误：它们是合法目标，
//   cstruct 的 free/size 之类是实例方法，不在这里）。
static Value reflect_methods_func(int argc, Value* args) {
    Target t;
    if (!resolve_target(argc, args, "reflect.methods", &t)) return val_null();

    int n = (t.is_cstruct || t.sdef->enum_sig) ? 0 : t.sdef->method_count;
    ObjArray* arr = arr_new(n > 0 ? n : 1);
    if (!arr) return val_null();
    Value av = val_obj((Object*)arr);
    gc_push_root(&av);
    for (int i = 0; i < n; i++) {
        const char* name = t.sdef->methods[i].name;
        if (!name) continue;
        ObjString* s = str_new(name, (int)strlen(name));
        if (!s) break;
        arr_push_value(arr, val_obj((Object*)s));
    }
    gc_pop_root();
    return av;
}

// ==================== 初始化 ====================

void reflect_init_module(void) {
    TypeKind one_any[] = { TYPE_ANY };
    TypeKind any_and_str[] = { TYPE_ANY, TYPE_STRING };

    native_register_module_method("reflect", "kind", reflect_kind_func, &NATIVE_T_STRING,
                                  NATIVE_FIXED(one_any));
    native_register_module_method("reflect", "type_name", reflect_type_name_func, &NATIVE_T_STRING,
                                  NATIVE_FIXED(one_any));
    native_register_module_method("reflect", "fields", reflect_fields_func, &NATIVE_T_ARR_STRING,
                                  NATIVE_FIXED(one_any));
    native_register_module_method("reflect", "field_count", reflect_field_count_func, &NATIVE_T_INT,
                                  NATIVE_FIXED(one_any));
    native_register_module_method("reflect", "has_field", reflect_has_field_func, &NATIVE_T_BOOL,
                                  NATIVE_FIXED(any_and_str));
    native_register_module_method("reflect", "get", reflect_get_func, &NATIVE_T_ANY,
                                  NATIVE_FIXED(any_and_str));
    native_register_module_method("reflect", "field_type", reflect_field_type_func,
                                  &NATIVE_T_STRING, NATIVE_FIXED(any_and_str));
    native_register_module_method("reflect", "methods", reflect_methods_func, &NATIVE_T_ARR_STRING,
                                  NATIVE_FIXED(one_any));
}
