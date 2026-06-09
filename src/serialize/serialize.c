#include "include/leno_serialize.h"
#include "include/lenolang.h"
#include "include/module_loader.h"
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#endif

static char** serialized_modules = NULL;
static int serialized_module_count = 0;
static int serialized_module_capacity = 0;

static int is_module_serialized(const char* path) {
    for (int i = 0; i < serialized_module_count; i++) {
        if (serialized_modules[i] && strcmp(serialized_modules[i], path) == 0) return 1;
    }
    return 0;
}

static void mark_module_serialized(const char* path) {
    if (serialized_module_count >= serialized_module_capacity) {
        serialized_module_capacity = serialized_module_capacity == 0 ? 16 : serialized_module_capacity * 2;
        serialized_modules = (char**)realloc(serialized_modules, serialized_module_capacity * sizeof(char*));
    }
    serialized_modules[serialized_module_count++] = strdup(path);
}

static void clear_serialized_modules(void) {
    for (int i = 0; i < serialized_module_count; i++) {
        free(serialized_modules[i]);
    }
    free(serialized_modules);
    serialized_modules = NULL;
    serialized_module_count = 0;
    serialized_module_capacity = 0;
}

// ============================================================================
// XOR 字符串编码密钥
// ============================================================================

static const uint8_t XOR_KEY[] = {0x4C, 0x45, 0x4E, 0x4F}; // "LENO"
#define XOR_KEY_LEN 4

static void xor_encode_decode(uint8_t* data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        data[i] ^= XOR_KEY[i % XOR_KEY_LEN];
    }
}

// ============================================================================
// 内部辅助：写入缓冲区
// ============================================================================

typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
} WriteBuffer;

static void wb_init(WriteBuffer* wb) {
    wb->capacity = 4096;
    wb->data = (uint8_t*)malloc(wb->capacity);
    wb->size = 0;
}

static void wb_ensure(WriteBuffer* wb, size_t additional) {
    while (wb->size + additional > wb->capacity) {
        wb->capacity *= 2;
        wb->data = (uint8_t*)realloc(wb->data, wb->capacity);
    }
}

static void wb_write(WriteBuffer* wb, const void* data, size_t len) {
    wb_ensure(wb, len);
    memcpy(wb->data + wb->size, data, len);
    wb->size += len;
}

static void wb_write_u8(WriteBuffer* wb, uint8_t val) {
    wb_write(wb, &val, 1);
}

static void wb_write_u16(WriteBuffer* wb, uint16_t val) {
    uint8_t buf[2];
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
    wb_write(wb, buf, 2);
}

static void wb_write_u32(WriteBuffer* wb, uint32_t val) {
    uint8_t buf[4];
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
    wb_write(wb, buf, 4);
}

static void wb_write_i32(WriteBuffer* wb, int32_t val) {
    wb_write_u32(wb, (uint32_t)val);
}

static void wb_write_u64(WriteBuffer* wb, uint64_t val) {
    uint8_t buf[8];
    for (int i = 7; i >= 0; i--) {
        buf[i] = val & 0xFF;
        val >>= 8;
    }
    wb_write(wb, buf, 8);
}

static void wb_write_double(WriteBuffer* wb, double val) {
    uint8_t buf[8];
    memcpy(buf, &val, 8);
    wb_write(wb, buf, 8);
}

static void wb_write_string(WriteBuffer* wb, const char* str, uint32_t len) {
    wb_write_u32(wb, len);
    if (len > 0) {
        // XOR 编码字符串
        uint8_t* encoded = (uint8_t*)malloc(len);
        if (encoded) {
            memcpy(encoded, str, len);
            xor_encode_decode(encoded, len);
            wb_write(wb, encoded, len);
            free(encoded);
        } else {
            wb_write(wb, str, len);
        }
    }
}

static void wb_free(WriteBuffer* wb) {
    free(wb->data);
    wb->data = NULL;
    wb->size = 0;
    wb->capacity = 0;
}

// ============================================================================
// 内部辅助：读取缓冲区
// ============================================================================

static int ctx_read(DeserializeCtx* ctx, void* out, size_t len) {
    if (ctx->pos + len > ctx->size) return 0;
    memcpy(out, ctx->data + ctx->pos, len);
    ctx->pos += len;
    return 1;
}

static int ctx_read_u8(DeserializeCtx* ctx, uint8_t* out) {
    return ctx_read(ctx, out, 1);
}

static int ctx_read_u16(DeserializeCtx* ctx, uint16_t* out) {
    uint8_t buf[2];
    if (!ctx_read(ctx, buf, 2)) return 0;
    *out = ((uint16_t)buf[0] << 8) | buf[1];
    return 1;
}

static int ctx_read_u32(DeserializeCtx* ctx, uint32_t* out) {
    uint8_t buf[4];
    if (!ctx_read(ctx, buf, 4)) return 0;
    *out = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | buf[3];
    return 1;
}

static int ctx_read_i32(DeserializeCtx* ctx, int32_t* out) {
    uint32_t u;
    if (!ctx_read_u32(ctx, &u)) return 0;
    *out = (int32_t)u;
    return 1;
}

static int ctx_read_u64(DeserializeCtx* ctx, uint64_t* out) {
    uint8_t buf[8];
    if (!ctx_read(ctx, buf, 8)) return 0;
    *out = 0;
    for (int i = 0; i < 8; i++) {
        *out = (*out << 8) | buf[i];
    }
    return 1;
}

static int ctx_read_double(DeserializeCtx* ctx, double* out) {
    return ctx_read(ctx, out, 8);
}

static char* ctx_read_string(DeserializeCtx* ctx, uint32_t* out_len) {
    uint32_t len;
    if (!ctx_read_u32(ctx, &len)) return NULL;
    if (out_len) *out_len = len;
    if (len == 0) return strdup("");
    if (ctx->pos + len > ctx->size) return NULL;
    char* str = (char*)malloc(len + 1);
    if (!str) return NULL;
    memcpy(str, ctx->data + ctx->pos, len);
    str[len] = '\0';
    ctx->pos += len;
    // XOR 解码字符串
    xor_encode_decode((uint8_t*)str, len);
    return str;
}

// ============================================================================
// FNV-1a 哈希
// ============================================================================

uint64_t serialize_source_hash(const char* source, size_t len) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)source[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// ============================================================================
// 前向声明
// ============================================================================

static int serialize_constant(WriteBuffer* wb, Value val);
static int deserialize_constant(DeserializeCtx* ctx, Value* out_val);
static int serialize_chunk(WriteBuffer* wb, Chunk* chunk);
static int deserialize_chunk_data(DeserializeCtx* ctx, Chunk* chunk);
static int serialize_scope_data(WriteBuffer* wb, Scope* scope);
static Scope* deserialize_scope_data(DeserializeCtx* ctx);

// ============================================================================
// 常量序列化
// ============================================================================

static int serialize_constant(WriteBuffer* wb, Value val) {
    if (val_is_null(val)) {
        wb_write_u8(wb, CONST_TAG_NULL);
        return 1;
    }
    if (val_is_bool(val)) {
        wb_write_u8(wb, val == TRUE_VAL ? CONST_TAG_TRUE : CONST_TAG_FALSE);
        return 1;
    }
    if (val_is_int(val)) {
        wb_write_u8(wb, CONST_TAG_INT);
        wb_write_i32(wb, (int32_t)val_as_int(val));
        return 1;
    }
    if (val_is_float(val)) {
        wb_write_u8(wb, CONST_TAG_FLOAT);
        wb_write_double(wb, val_as_double(val));
        return 1;
    }
    if (val_is_obj(val)) {
        Object* obj = val_as_obj(val);
        switch (obj->type) {
        case OBJ_STRING: {
            ObjString* str = (ObjString*)obj;
            wb_write_u8(wb, CONST_TAG_STRING);
            wb_write_string(wb, str->chars, (uint32_t)str->len);
            return 1;
        }
        case OBJ_FUNCTION: {
            ObjFunction* func = (ObjFunction*)obj;
            wb_write_u8(wb, CONST_TAG_FUNCTION);
            wb_write_string(wb, func->name, (uint32_t)strlen(func->name));
            wb_write_u32(wb, (uint32_t)func->arity);
            wb_write_u32(wb, (uint32_t)func->upvalue_count);
            wb_write_u32(wb, (uint32_t)func->local_count);
            wb_write_u8(wb, (uint8_t)func->has_try);
            uint32_t param_count = 0;
            if (func->arity > 0 && func->param_types) {
                param_count = (uint32_t)func->arity;
            }
            wb_write_u32(wb, param_count);
            for (uint32_t i = 0; i < param_count; i++) {
                wb_write_u8(wb, (uint8_t)func->param_types[i]);
            }
            if (!serialize_chunk(wb, func->chunk)) return 0;
            return 1;
        }
        case OBJ_BIGINT: {
            ObjBigInt* bigint = (ObjBigInt*)obj;
            wb_write_u8(wb, CONST_TAG_BIGINT);
            wb_write_u32(wb, (uint32_t)bigint->limb_count);
            wb_write_u8(wb, (uint8_t)bigint->is_negative);
            for (int i = 0; i < bigint->limb_count; i++) {
                uint32_t limb = bigint->limbs[i];
                uint8_t buf[4];
                buf[0] = (limb >> 24) & 0xFF;
                buf[1] = (limb >> 16) & 0xFF;
                buf[2] = (limb >> 8) & 0xFF;
                buf[3] = limb & 0xFF;
                wb_write(wb, buf, 4);
            }
            return 1;
        }
        case OBJ_RANGE: {
            ObjRange* range = (ObjRange*)obj;
            wb_write_u8(wb, CONST_TAG_RANGE);
            wb_write_i32(wb, (int32_t)range->start);
            wb_write_i32(wb, (int32_t)range->end);
            wb_write_u8(wb, (uint8_t)range->inclusive);
            return 1;
        }
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;
            wb_write_u8(wb, CONST_TAG_ARRAY);
            wb_write_u32(wb, (uint32_t)arr->count);
            for (int i = 0; i < arr->count; i++) {
                if (!serialize_constant(wb, arr->elements[i])) return 0;
            }
            return 1;
        }
        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)obj;
            wb_write_u8(wb, CONST_TAG_DICT);
            uint32_t entry_count = 0;
            for (int i = 0; i < dict->order_count; i++) {
                if (dict->order[i]) entry_count++;
            }
            wb_write_u32(wb, entry_count);
            for (int i = 0; i < dict->order_count; i++) {
                Value key = dict->order[i];
                if (val_is_null(key)) continue;
                if (!serialize_constant(wb, key)) return 0;
                Value value = dict_get(dict, key);
                if (!serialize_constant(wb, value)) return 0;
            }
            return 1;
        }
        case OBJ_ENUM_DEF: {
            ObjEnumDef* enum_def = (ObjEnumDef*)obj;
            wb_write_u8(wb, CONST_TAG_ENUM_DEF);
            wb_write_string(wb, enum_def->name, (uint32_t)strlen(enum_def->name));
            wb_write_u32(wb, (uint32_t)enum_def->member_count);
            for (int i = 0; i < enum_def->member_count; i++) {
                wb_write_string(wb, enum_def->members[i].name,
                                (uint32_t)strlen(enum_def->members[i].name));
                int64_t val = enum_def->members[i].value;
                uint8_t buf[8];
                for (int j = 7; j >= 0; j--) {
                    buf[j] = val & 0xFF;
                    val >>= 8;
                }
                wb_write(wb, buf, 8);
            }
            return 1;
        }
        case OBJ_STRUCT_DEF: {
            ObjStructDef* struct_def = (ObjStructDef*)obj;
            wb_write_u8(wb, CONST_TAG_STRUCT_DEF);
            wb_write_string(wb, struct_def->name, (uint32_t)strlen(struct_def->name));
            wb_write_u32(wb, (uint32_t)struct_def->field_count);
            for (int i = 0; i < struct_def->field_count; i++) {
                StructFieldInfo* f = &struct_def->fields[i];
                wb_write_string(wb, f->name, (uint32_t)strlen(f->name));
                wb_write_u8(wb, (uint8_t)f->type);
                if (f->struct_type_name) {
                    wb_write_u8(wb, 1);
                    wb_write_string(wb, f->struct_type_name,
                                    (uint32_t)strlen(f->struct_type_name));
                } else {
                    wb_write_u8(wb, 0);
                }
                wb_write_u8(wb, (uint8_t)f->has_default);
                if (f->has_default) {
                    if (!serialize_constant(wb, f->default_value)) return 0;
                }
                wb_write_u8(wb, (uint8_t)f->element_type);
            }
            wb_write_u32(wb, (uint32_t)struct_def->method_count);
            for (int i = 0; i < struct_def->method_count; i++) {
                StructMethodInfo* m = &struct_def->methods[i];
                wb_write_string(wb, m->name, (uint32_t)strlen(m->name));
                if (m->func) {
                    if (!serialize_constant(wb, val_obj((Object*)m->func))) return 0;
                } else {
                    wb_write_u8(wb, CONST_TAG_NULL);
                }
            }
            wb_write_u32(wb, (uint32_t)struct_def->impl_count);
            for (int i = 0; i < struct_def->impl_count; i++) {
                wb_write_string(wb, struct_def->impl_names[i],
                                (uint32_t)strlen(struct_def->impl_names[i]));
            }
            return 1;
        }
        case OBJ_CSTRUCT_DEF: {
            ObjCStructDef* cdef = (ObjCStructDef*)obj;
            wb_write_u8(wb, CONST_TAG_CSTRUCT_DEF);
            wb_write_string(wb, cdef->name, (uint32_t)strlen(cdef->name));
            wb_write_u32(wb, (uint32_t)cdef->field_count);
            wb_write_u32(wb, (uint32_t)cdef->total_size);
            wb_write_u32(wb, (uint32_t)cdef->alignment);
            for (int i = 0; i < cdef->field_count; i++) {
                CStructFieldInfo* f = &cdef->fields[i];
                wb_write_string(wb, f->name, (uint32_t)strlen(f->name));
                wb_write_u8(wb, (uint8_t)f->type);
                wb_write_u32(wb, (uint32_t)f->offset);
                wb_write_u32(wb, (uint32_t)f->size);
                wb_write_u32(wb, (uint32_t)f->array_dim);
                if (f->struct_name) {
                    wb_write_u8(wb, 1);
                    wb_write_string(wb, f->struct_name,
                                    (uint32_t)strlen(f->struct_name));
                } else {
                    wb_write_u8(wb, 0);
                }
                wb_write_u8(wb, (uint8_t)f->element_type);
            }
            return 1;
        }
        case OBJ_FACE_DEF: {
            ObjFaceDef* face = (ObjFaceDef*)obj;
            wb_write_u8(wb, CONST_TAG_FACE_DEF);
            wb_write_string(wb, face->name, (uint32_t)strlen(face->name));
            wb_write_u32(wb, (uint32_t)face->method_count);
            for (int i = 0; i < face->method_count; i++) {
                FaceMethodInfo* m = &face->methods[i];
                wb_write_string(wb, m->name, (uint32_t)strlen(m->name));
                wb_write_u32(wb, (uint32_t)m->param_count);
                if (m->return_type) {
                    wb_write_u8(wb, (uint8_t)m->return_type->kind);
                } else {
                    wb_write_u8(wb, (uint8_t)TYPE_INFER);
                }
                for (int j = 0; j < m->param_count; j++) {
                    if (m->param_types[j]) {
                        wb_write_u8(wb, (uint8_t)m->param_types[j]->kind);
                    } else {
                        wb_write_u8(wb, (uint8_t)TYPE_INFER);
                    }
                }
            }
            return 1;
        }
        case OBJ_MODULE: {
            ObjModule* mod = (ObjModule*)obj;
            if (mod->source_path && is_module_serialized(mod->source_path)) {
                wb_write_u8(wb, CONST_TAG_MODULE_REF);
                wb_write_string(wb, mod->name, (uint32_t)strlen(mod->name));
                if (mod->source_path) {
                    wb_write_u8(wb, 1);
                    wb_write_string(wb, mod->source_path, (uint32_t)strlen(mod->source_path));
                } else {
                    wb_write_u8(wb, 0);
                }
                return 1;
            }
            if (mod->source_path) {
                mark_module_serialized(mod->source_path);
            }
            wb_write_u8(wb, CONST_TAG_MODULE);
            wb_write_string(wb, mod->name, (uint32_t)strlen(mod->name));
            if (mod->source_path) {
                wb_write_u8(wb, 1);
                wb_write_string(wb, mod->source_path, (uint32_t)strlen(mod->source_path));
            } else {
                wb_write_u8(wb, 0);
            }
            wb_write_u32(wb, (uint32_t)mod->global_count);
            for (int i = 0; i < mod->global_count; i++) {
                if (!serialize_constant(wb, mod->globals[i])) return 0;
            }
            {
                ObjDict* dict = mod->exports;
                uint32_t entry_count = 0;
                for (int i = 0; i < dict->order_count; i++) {
                    if (dict->order[i]) entry_count++;
                }
                wb_write_u32(wb, entry_count);
                for (int i = 0; i < dict->order_count; i++) {
                    Value key = dict->order[i];
                    if (val_is_null(key)) continue;
                    if (!serialize_constant(wb, key)) return 0;
                    Value value = dict_get(dict, key);
                    if (!serialize_constant(wb, value)) return 0;
                }
            }
            wb_write_u32(wb, (uint32_t)mod->native_import_count);
            for (int i = 0; i < mod->native_import_count; i++) {
                wb_write_string(wb, mod->native_imports[i], (uint32_t)strlen(mod->native_imports[i]));
            }
            // 序列化 init_chunk
            if (mod->init_chunk) {
                wb_write_u8(wb, 1);
                if (!serialize_chunk(wb, mod->init_chunk)) return 0;
            } else {
                wb_write_u8(wb, 0);
            }
            // 序列化 export_mappings
            wb_write_u32(wb, (uint32_t)mod->export_mapping_count);
            for (int i = 0; i < mod->export_mapping_count; i++) {
                wb_write_string(wb, mod->export_mappings[i].name, (uint32_t)strlen(mod->export_mappings[i].name));
                wb_write_u32(wb, (uint32_t)mod->export_mappings[i].global_index);
            }
            return 1;
        }
        case OBJ_FFI_POINTER: {
            ObjFFIPointer* p = (ObjFFIPointer*)obj;
            wb_write_u8(wb, CONST_TAG_FFI_PTR);
            wb_write_u64(wb, (uint64_t)(uintptr_t)p->ptr);
            wb_write_u64(wb, (uint64_t)p->size);
            wb_write_u8(wb, (uint8_t)p->owned);
            wb_write_u8(wb, (uint8_t)p->freed);
            wb_write_u8(wb, (uint8_t)p->element_type);
            return 1;
        }
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            wb_write_u8(wb, CONST_TAG_CLOSURE);
            if (!serialize_constant(wb, val_obj((Object*)closure->function))) return 0;
            wb_write_u32(wb, (uint32_t)closure->upvalue_count);
            return 1;
        }
        case OBJ_FFI_LIBRARY: {
            extern char* ffi_library_get_path(Object* obj);
            char* path = ffi_library_get_path(obj);
            wb_write_u8(wb, CONST_TAG_FFI_LIB);
            if (path) {
                wb_write_u8(wb, 1);
                wb_write_string(wb, path, (uint32_t)strlen(path));
            } else {
                wb_write_u8(wb, 0);
            }
            return 1;
        }
        default:
            return 0;
        }
    }
    return 0;
}

// ============================================================================
// Chunk 序列化
// ============================================================================

static int serialize_chunk(WriteBuffer* wb, Chunk* chunk) {
    if (!chunk) {
        wb_write_u32(wb, 0);
        wb_write_u32(wb, 0);
        wb_write_u32(wb, 0);
        wb_write_u32(wb, 0);
        wb_write_u32(wb, 0);
        return 1;
    }

    uint32_t filename_len = chunk->filename ? (uint32_t)strlen(chunk->filename) : 0;
    wb_write_string(wb, chunk->filename, filename_len);

    wb_write_u32(wb, (uint32_t)chunk->local_count);
    wb_write_u32(wb, (uint32_t)chunk->const_cnt);

    for (int i = 0; i < chunk->const_cnt; i++) {
        if (!serialize_constant(wb, chunk->constants[i])) return 0;
    }

    wb_write_u32(wb, (uint32_t)chunk->len);
    if (chunk->len > 0) {
        wb_write(wb, chunk->code, (size_t)chunk->len);
    }

    if (chunk->len > 0 && chunk->lines) {
        wb_write_u8(wb, 1);
        for (int i = 0; i < chunk->len; i++) {
            wb_write_u16(wb, (uint16_t)(chunk->lines[i] & 0xFFFF));
        }
    } else {
        wb_write_u8(wb, 0);
    }

    return 1;
}

// ============================================================================
// Scope 序列化（仅序列化全局作用域的符号信息）
// ============================================================================

static int serialize_scope_data(WriteBuffer* wb, Scope* scope) {
    if (!scope) {
        wb_write_u32(wb, 0);
        wb_write_u32(wb, 0);
        return 1;
    }

    wb_write_u32(wb, (uint32_t)scope->global_var_index);
    wb_write_u32(wb, (uint32_t)scope->global_func_index);

    int sym_count = 0;
    for (int i = 0; i < scope->sym_cnt; i++) {
        Symbol* sym = scope->syms[i];
        if (sym->kind == SYM_GLOBAL || sym->kind == SYM_GLOBAL_FUNC ||
            sym->kind == SYM_NATIVE || sym->kind == SYM_TYPE ||
            sym->kind == SYM_STRUCT || sym->kind == SYM_CSTRUCT ||
            sym->kind == SYM_ENUM || sym->kind == SYM_CLIB ||
            sym->kind == SYM_CFUNC) {
            sym_count++;
        }
    }
    wb_write_u32(wb, (uint32_t)sym_count);

    for (int i = 0; i < scope->sym_cnt; i++) {
        Symbol* sym = scope->syms[i];
        if (sym->kind != SYM_GLOBAL && sym->kind != SYM_GLOBAL_FUNC &&
            sym->kind != SYM_NATIVE && sym->kind != SYM_TYPE &&
            sym->kind != SYM_STRUCT && sym->kind != SYM_CSTRUCT &&
            sym->kind != SYM_ENUM && sym->kind != SYM_CLIB &&
            sym->kind != SYM_CFUNC) {
            continue;
        }

        wb_write_u8(wb, (uint8_t)sym->kind);
        wb_write_string(wb, sym->name, (uint32_t)strlen(sym->name));
        wb_write_i32(wb, sym->index);
        wb_write_u8(wb, (uint8_t)sym->is_captured);

        if (sym->type) {
            wb_write_u8(wb, (uint8_t)sym->type->kind);
        } else {
            wb_write_u8(wb, (uint8_t)TYPE_INFER);
        }

        if (sym->dict_keys && sym->dict_key_count > 0) {
            wb_write_u8(wb, 1);
            wb_write_u32(wb, (uint32_t)sym->dict_key_count);
            for (int j = 0; j < sym->dict_key_count; j++) {
                wb_write_string(wb, sym->dict_keys[j],
                                (uint32_t)strlen(sym->dict_keys[j]));
            }
        } else {
            wb_write_u8(wb, 0);
        }

        // 序列化 clib 函数签名信息
        if (sym->kind == SYM_CLIB) {
            wb_write_u32(wb, (uint32_t)sym->clib_func_count);
            for (int j = 0; j < sym->clib_func_count; j++) {
                wb_write_string(wb, sym->clib_func_names[j],
                                (uint32_t)strlen(sym->clib_func_names[j]));
                // 返回类型
                if (sym->clib_func_return_types[j]) {
                    wb_write_u8(wb, (uint8_t)sym->clib_func_return_types[j]->kind);
                } else {
                    wb_write_u8(wb, (uint8_t)TYPE_INFER);
                }
                // 参数数量
                wb_write_u32(wb, (uint32_t)sym->clib_func_param_counts[j]);
                // 参数类型
                for (int k = 0; k < sym->clib_func_param_counts[j]; k++) {
                    if (sym->clib_func_param_types[j][k]) {
                        wb_write_u8(wb, (uint8_t)sym->clib_func_param_types[j][k]->kind);
                    } else {
                        wb_write_u8(wb, (uint8_t)TYPE_INFER);
                    }
                }
            }
        }

        // 序列化 cfunc 回调签名信息
        if (sym->kind == SYM_CFUNC) {
            wb_write_u32(wb, (uint32_t)sym->cfunc_param_count);
            // 返回类型
            if (sym->cfunc_return_type) {
                wb_write_u8(wb, (uint8_t)sym->cfunc_return_type->kind);
            } else {
                wb_write_u8(wb, (uint8_t)TYPE_INFER);
            }
            // 参数类型
            for (int j = 0; j < sym->cfunc_param_count; j++) {
                if (sym->cfunc_param_types[j]) {
                    wb_write_u8(wb, (uint8_t)sym->cfunc_param_types[j]->kind);
                } else {
                    wb_write_u8(wb, (uint8_t)TYPE_INFER);
                }
            }
        }
    }

    return 1;
}

// ============================================================================
// 常量反序列化
// ============================================================================

static int deserialize_constant(DeserializeCtx* ctx, Value* out_val) {
    uint8_t tag;
    if (!ctx_read_u8(ctx, &tag)) return 0;

    switch (tag) {
    case CONST_TAG_NULL:
        *out_val = val_null();
        return 1;
    case CONST_TAG_TRUE:
        *out_val = val_bool(1);
        return 1;
    case CONST_TAG_FALSE:
        *out_val = val_bool(0);
        return 1;
    case CONST_TAG_INT: {
        int32_t ival;
        if (!ctx_read_i32(ctx, &ival)) return 0;
        *out_val = val_int((int)ival);
        return 1;
    }
    case CONST_TAG_FLOAT: {
        double dval;
        if (!ctx_read_double(ctx, &dval)) return 0;
        *out_val = val_float(dval);
        return 1;
    }
    case CONST_TAG_STRING: {
        uint32_t len;
        char* str = ctx_read_string(ctx, &len);
        if (!str) return 0;
        ObjString* obj = str_new(str, (int)len);
        free(str);
        *out_val = val_obj((Object*)obj);
        return 1;
    }
    case CONST_TAG_FUNCTION: {
        uint32_t name_len;
        char* name = ctx_read_string(ctx, &name_len);
        if (!name) return 0;

        uint32_t arity, upvalue_count, local_count, param_count;
        uint8_t has_try;
        if (!ctx_read_u32(ctx, &arity) ||
            !ctx_read_u32(ctx, &upvalue_count) ||
            !ctx_read_u32(ctx, &local_count) ||
            !ctx_read_u8(ctx, &has_try) ||
            !ctx_read_u32(ctx, &param_count)) {
            free(name);
            return 0;
        }

        ObjFunction* func = (ObjFunction*)gc_alloc(sizeof(ObjFunction), OBJ_FUNCTION);
        func->arity = (int)arity;
        func->name = name;
        func->upvalue_count = (int)upvalue_count;
        func->local_count = (int)local_count;
        func->has_try = has_try;
        func->module = NULL;

        if (param_count > 0) {
            func->param_types = (TypeKind*)malloc(sizeof(TypeKind) * param_count);
            for (uint32_t i = 0; i < param_count; i++) {
                uint8_t tk;
                if (!ctx_read_u8(ctx, &tk)) {
                    free(func->param_types);
                    func->param_types = NULL;
                    return 0;
                }
                func->param_types[i] = (TypeKind)tk;
            }
        } else {
            func->param_types = NULL;
        }

        func->chunk = (Chunk*)malloc(sizeof(Chunk));
        chunk_init(func->chunk);
        if (!deserialize_chunk_data(ctx, func->chunk)) return 0;

        *out_val = val_obj((Object*)func);
        return 1;
    }
    case CONST_TAG_BIGINT: {
        uint32_t limb_count;
        uint8_t is_negative;
        if (!ctx_read_u32(ctx, &limb_count) || !ctx_read_u8(ctx, &is_negative)) return 0;
        uint32_t* limbs = NULL;
        if (limb_count > 0) {
            limbs = (uint32_t*)malloc(sizeof(uint32_t) * limb_count);
            for (uint32_t i = 0; i < limb_count; i++) {
                uint8_t buf[4];
                if (!ctx_read(ctx, buf, 4)) {
                    free(limbs);
                    return 0;
                }
                limbs[i] = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                           ((uint32_t)buf[2] << 8) | buf[3];
            }
        }
        ObjBigInt* bigint = bigint_new(limbs, (int)limb_count, is_negative);
        free(limbs);
        *out_val = val_obj((Object*)bigint);
        return 1;
    }
    case CONST_TAG_RANGE: {
        int32_t start, end;
        uint8_t inclusive;
        if (!ctx_read_i32(ctx, &start) || !ctx_read_i32(ctx, &end) ||
            !ctx_read_u8(ctx, &inclusive)) return 0;
        ObjRange* range = range_new((int)start, (int)end, inclusive);
        *out_val = val_obj((Object*)range);
        return 1;
    }
    case CONST_TAG_ARRAY: {
        uint32_t count;
        if (!ctx_read_u32(ctx, &count)) return 0;
        ObjArray* arr = arr_new((int)count > 0 ? (int)count : 8);
        arr->count = (int)count;
        for (uint32_t i = 0; i < count; i++) {
            if (!deserialize_constant(ctx, &arr->elements[i])) return 0;
        }
        *out_val = val_obj((Object*)arr);
        return 1;
    }
    case CONST_TAG_DICT: {
        uint32_t entry_count;
        if (!ctx_read_u32(ctx, &entry_count)) return 0;
        ObjDict* dict = dict_new((int)entry_count > 0 ? (int)entry_count * 2 : 8);
        for (uint32_t i = 0; i < entry_count; i++) {
            Value key_val, val;
            if (!deserialize_constant(ctx, &key_val)) return 0;
            if (!deserialize_constant(ctx, &val)) return 0;
            dict_set(dict, key_val, val);
        }
        *out_val = val_obj((Object*)dict);
        return 1;
    }
    case CONST_TAG_ENUM_DEF: {
        uint32_t name_len;
        char* name = ctx_read_string(ctx, &name_len);
        if (!name) return 0;
        uint32_t member_count;
        if (!ctx_read_u32(ctx, &member_count)) { free(name); return 0; }
        ObjEnumDef* enum_def = enum_def_new(name, (int)member_count);
        free(name);
        for (uint32_t i = 0; i < member_count; i++) {
            uint32_t mname_len;
            char* mname = ctx_read_string(ctx, &mname_len);
            if (!mname) return 0;
            uint8_t buf[8];
            if (!ctx_read(ctx, buf, 8)) { free(mname); return 0; }
            int64_t val = 0;
            for (int j = 0; j < 8; j++) {
                val = (val << 8) | buf[j];
            }
            enum_def_set_member(enum_def, (int)i, mname, val);
            free(mname);
        }
        *out_val = val_obj((Object*)enum_def);
        return 1;
    }
    case CONST_TAG_STRUCT_DEF: {
        uint32_t name_len;
        char* name = ctx_read_string(ctx, &name_len);
        if (!name) return 0;
        uint32_t field_count;
        if (!ctx_read_u32(ctx, &field_count)) { free(name); return 0; }

        ObjStructDef* sdef = struct_def_new(name, (int)field_count, 0);
        free(name);

        for (uint32_t i = 0; i < field_count; i++) {
            uint32_t fname_len;
            char* fname = ctx_read_string(ctx, &fname_len);
            if (!fname) return 0;
            uint8_t type_kind;
            if (!ctx_read_u8(ctx, &type_kind)) { free(fname); return 0; }

            uint8_t has_struct_type;
            char* struct_type_name = NULL;
            if (!ctx_read_u8(ctx, &has_struct_type)) { free(fname); return 0; }
            if (has_struct_type) {
                uint32_t stn_len;
                struct_type_name = ctx_read_string(ctx, &stn_len);
                if (!struct_type_name) { free(fname); return 0; }
            }

            uint8_t has_default;
            if (!ctx_read_u8(ctx, &has_default)) { free(fname); free(struct_type_name); return 0; }
            Value default_val = val_null();
            if (has_default) {
                if (!deserialize_constant(ctx, &default_val)) {
                    free(fname); free(struct_type_name); return 0;
                }
            }

            uint8_t element_type;
            if (!ctx_read_u8(ctx, &element_type)) { free(fname); free(struct_type_name); return 0; }

            struct_def_set_field(sdef, (int)i, fname, (TypeKind)type_kind,
                                 struct_type_name, default_val, has_default,
                                 (TypeKind)element_type);
            free(fname);
            free(struct_type_name);
        }

        uint32_t method_count;
        if (!ctx_read_u32(ctx, &method_count)) return 0;
        if (method_count > 0) {
            sdef->methods = (StructMethodInfo*)calloc(method_count, sizeof(StructMethodInfo));
            sdef->method_count = (int)method_count;
        }

        for (uint32_t i = 0; i < method_count; i++) {
            uint32_t mname_len;
            char* mname = ctx_read_string(ctx, &mname_len);
            if (!mname) return 0;

            Value func_val;
            if (!deserialize_constant(ctx, &func_val)) { free(mname); return 0; }

            ObjFunction* func = NULL;
            if (val_is_obj(func_val) && val_as_obj(func_val)->type == OBJ_FUNCTION) {
                func = (ObjFunction*)val_as_obj(func_val);
            }
            sdef->methods[i].name = mname;
            sdef->methods[i].func = func;
            sdef->methods[i].closure = NULL;
            // 预创建闭包（与 OP_STRUCT_DEF 运行时逻辑一致）
            if (func && func->upvalue_count == 0) {
                ObjClosure* closure = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                if (closure) {
                    closure->function = func;
                    closure->upvalue_count = 0;
                    sdef->methods[i].closure = closure;
                }
            }
        }

        uint32_t impl_count;
        if (!ctx_read_u32(ctx, &impl_count)) return 0;
        sdef->impl_count = (int)impl_count;
        if (impl_count > 0) {
            sdef->impl_names = (char**)malloc(sizeof(char*) * impl_count);
            for (uint32_t i = 0; i < impl_count; i++) {
                uint32_t iname_len;
                sdef->impl_names[i] = ctx_read_string(ctx, &iname_len);
                if (!sdef->impl_names[i]) return 0;
            }
        }

        *out_val = val_obj((Object*)sdef);
        return 1;
    }
    case CONST_TAG_CSTRUCT_DEF: {
        uint32_t name_len;
        char* name = ctx_read_string(ctx, &name_len);
        if (!name) return 0;
        uint32_t field_count, total_size, alignment;
        if (!ctx_read_u32(ctx, &field_count) ||
            !ctx_read_u32(ctx, &total_size) ||
            !ctx_read_u32(ctx, &alignment)) { free(name); return 0; }

        ObjCStructDef* cdef = cstruct_def_new(name, (int)field_count,
                                               (int)total_size, (int)alignment);
        free(name);

        for (uint32_t i = 0; i < field_count; i++) {
            uint32_t fname_len;
            char* fname = ctx_read_string(ctx, &fname_len);
            if (!fname) return 0;
            uint8_t type_kind;
            uint32_t offset, size, array_dim;
            if (!ctx_read_u8(ctx, &type_kind) ||
                !ctx_read_u32(ctx, &offset) ||
                !ctx_read_u32(ctx, &size) ||
                !ctx_read_u32(ctx, &array_dim)) { free(fname); return 0; }

            uint8_t has_struct_name;
            char* struct_name = NULL;
            if (!ctx_read_u8(ctx, &has_struct_name)) { free(fname); return 0; }
            if (has_struct_name) {
                uint32_t sn_len;
                struct_name = ctx_read_string(ctx, &sn_len);
                if (!struct_name) { free(fname); return 0; }
            }

            uint8_t element_type;
            if (!ctx_read_u8(ctx, &element_type)) { free(fname); free(struct_name); return 0; }

            cstruct_def_set_field(cdef, (int)i, fname, (TypeKind)type_kind,
                                  (int)offset, (int)size, (int)array_dim,
                                  struct_name, (TypeKind)element_type);
            free(fname);
            free(struct_name);
        }

        *out_val = val_obj((Object*)cdef);
        return 1;
    }
    case CONST_TAG_FACE_DEF: {
        uint32_t name_len;
        char* name = ctx_read_string(ctx, &name_len);
        if (!name) return 0;
        uint32_t method_count;
        if (!ctx_read_u32(ctx, &method_count)) { free(name); return 0; }

        ObjFaceDef* face = face_def_new(name, (int)method_count);
        free(name);

        for (uint32_t i = 0; i < method_count; i++) {
            uint32_t mname_len;
            char* mname = ctx_read_string(ctx, &mname_len);
            if (!mname) return 0;

            uint32_t param_count;
            uint8_t return_type_kind;
            if (!ctx_read_u32(ctx, &param_count) ||
                !ctx_read_u8(ctx, &return_type_kind)) { free(mname); return 0; }

            face->methods[i].name = mname;
            face->methods[i].param_count = (int)param_count;
            if (return_type_kind != TYPE_INFER) {
                face->methods[i].return_type = type_new((TypeKind)return_type_kind);
            } else {
                face->methods[i].return_type = NULL;
            }
            face->methods[i].param_types = NULL;
            if (param_count > 0) {
                face->methods[i].param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * param_count);
                for (uint32_t j = 0; j < param_count; j++) {
                    uint8_t ptk;
                    if (!ctx_read_u8(ctx, &ptk)) return 0;
                    if (ptk != TYPE_INFER) {
                        face->methods[i].param_types[j] = type_new((TypeKind)ptk);
                    } else {
                        face->methods[i].param_types[j] = NULL;
                    }
                }
            }
        }

        *out_val = val_obj((Object*)face);
        return 1;
    }
    case CONST_TAG_MODULE_REF: {
        uint32_t name_len;
        char* name = ctx_read_string(ctx, &name_len);
        if (!name) return 0;
        uint8_t has_source_path;
        if (!ctx_read_u8(ctx, &has_source_path)) { free(name); return 0; }
        char* source_path = NULL;
        if (has_source_path) {
            uint32_t sp_len;
            source_path = ctx_read_string(ctx, &sp_len);
            if (!source_path) { free(name); return 0; }
        }
        ObjModule* mod = NULL;
        if (source_path) {
            mod = find_loaded_module(source_path);
            if (!mod) {
                mod = (ObjModule*)gc_alloc(sizeof(ObjModule), OBJ_MODULE);
                mod->name = strdup(name);
                mod->source_path = strdup(source_path);
                mod->exports = dict_new(4);
                mod->frame = NULL;
                mod->native_imports = NULL;
                mod->native_import_count = 0;
                mod->globals = NULL;
                mod->global_count = 0;
                mod->global_capacity = 0;
                mod->init_chunk = NULL;
                mod->initialized = 0;
                mod->export_mappings = NULL;
                mod->export_mapping_count = 0;
                add_loaded_module_public(source_path, mod);
            }
        }
        free(name);
        free(source_path);
        if (mod) {
            *out_val = val_obj((Object*)mod);
        } else {
            *out_val = val_null();
        }
        return 1;
    }
    case CONST_TAG_MODULE: {
        uint32_t name_len;
        char* name = ctx_read_string(ctx, &name_len);
        if (!name) return 0;
        uint8_t has_source_path;
        if (!ctx_read_u8(ctx, &has_source_path)) { free(name); return 0; }
        char* source_path = NULL;
        if (has_source_path) {
            uint32_t sp_len;
            source_path = ctx_read_string(ctx, &sp_len);
            if (!source_path) { free(name); return 0; }
        }
        ObjModule* mod = NULL;
        if (source_path) {
            mod = find_loaded_module(source_path);
        }
        if (!mod) {
            mod = (ObjModule*)gc_alloc(sizeof(ObjModule), OBJ_MODULE);
            mod->name = name;
            mod->source_path = source_path;
            mod->exports = dict_new(4);
            mod->frame = NULL;
            mod->native_imports = NULL;
            mod->native_import_count = 0;
            mod->globals = NULL;
            mod->global_count = 0;
            mod->global_capacity = 0;
            mod->init_chunk = NULL;
            mod->initialized = 0;
            mod->export_mappings = NULL;
            mod->export_mapping_count = 0;
            if (source_path) {
                add_loaded_module_public(source_path, mod);
            }
        } else {
            free(name);
            free(source_path);
        }

        uint32_t global_count;
        if (!ctx_read_u32(ctx, &global_count)) return 0;
        mod->global_count = (int)global_count;
        mod->global_capacity = (int)global_count > 0 ? (int)global_count : 0;
        mod->globals = NULL;
        if (global_count > 0) {
            mod->globals = (Value*)malloc(sizeof(Value) * global_count);
            for (uint32_t i = 0; i < global_count; i++) {
                if (!deserialize_constant(ctx, &mod->globals[i])) return 0;
            }
        }

        uint32_t export_count;
        if (!ctx_read_u32(ctx, &export_count)) return 0;
        for (uint32_t i = 0; i < export_count; i++) {
            Value key_val, val;
            if (!deserialize_constant(ctx, &key_val)) return 0;
            if (!deserialize_constant(ctx, &val)) return 0;
            dict_set(mod->exports, key_val, val);
        }

        {
            uint32_t ni_count;
            if (!ctx_read_u32(ctx, &ni_count)) return 0;
            mod->native_import_count = (int)ni_count;
            mod->native_imports = NULL;
            if (ni_count > 0) {
                mod->native_imports = (char**)malloc(ni_count * sizeof(char*));
                for (uint32_t i = 0; i < ni_count; i++) {
                    uint32_t name_len;
                    mod->native_imports[i] = ctx_read_string(ctx, &name_len);
                    if (!mod->native_imports[i]) return 0;
                }
            }
        }

        // 反序列化 init_chunk
        {
            uint8_t has_init_chunk;
            if (!ctx_read_u8(ctx, &has_init_chunk)) return 0;
            if (has_init_chunk) {
                Chunk* chunk = (Chunk*)malloc(sizeof(Chunk));
                if (!chunk) return 0;
                chunk_init(chunk);
                if (!deserialize_chunk_data(ctx, chunk)) {
                    chunk_free(chunk);
                    free(chunk);
                    return 0;
                }
                mod->init_chunk = chunk;
            }
        }

        // 反序列化 export_mappings
        {
            uint32_t em_count;
            if (!ctx_read_u32(ctx, &em_count)) return 0;
            mod->export_mapping_count = (int)em_count;
            mod->export_mappings = NULL;
            if (em_count > 0) {
                mod->export_mappings = (ExportGlobalMapping*)malloc(em_count * sizeof(ExportGlobalMapping));
                for (uint32_t i = 0; i < em_count; i++) {
                    uint32_t name_len;
                    mod->export_mappings[i].name = ctx_read_string(ctx, &name_len);
                    if (!mod->export_mappings[i].name) return 0;
                    uint32_t gidx;
                    if (!ctx_read_u32(ctx, &gidx)) return 0;
                    mod->export_mappings[i].global_index = (int)gidx;
                }
            }
        }

        // 反序列化后，将模块内所有函数/闭包的 module 指针指向此模块
        // 因为序列化时 func->module 未保存，反序列化后为 NULL
        {
            extern void update_module_function_ptrs(ObjModule* src_module, ObjModule* dst_module);
            update_module_function_ptrs(mod, mod);
        }

        *out_val = val_obj((Object*)mod);
        return 1;
    }
    case CONST_TAG_FFI_PTR: {
        uint64_t ptr_val, size_val;
        uint8_t owned, freed, element_type;
        if (!ctx_read_u64(ctx, &ptr_val) ||
            !ctx_read_u64(ctx, &size_val) ||
            !ctx_read_u8(ctx, &owned) ||
            !ctx_read_u8(ctx, &freed) ||
            !ctx_read_u8(ctx, &element_type)) return 0;
        ObjFFIPointer* p = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
        p->ptr = (void*)(uintptr_t)ptr_val;
        p->size = (size_t)size_val;
        p->owned = owned;
        p->freed = freed;
        p->element_type = (TypeKind)element_type;
        *out_val = val_obj((Object*)p);
        return 1;
    }
    case CONST_TAG_CLOSURE: {
        Value func_val;
        if (!deserialize_constant(ctx, &func_val)) return 0;
        uint32_t upvalue_count;
        if (!ctx_read_u32(ctx, &upvalue_count)) return 0;
        ObjFunction* func = NULL;
        if (val_is_obj(func_val) && val_as_obj(func_val)->type == OBJ_FUNCTION) {
            func = (ObjFunction*)val_as_obj(func_val);
        }
        ObjClosure* closure = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
        closure->function = func;
        closure->upvalue_count = (int)upvalue_count;
        for (int i = 0; i < (int)upvalue_count && i < MAX_UPVALUES; i++) {
            closure->upvalues[i] = NULL;
        }
        *out_val = val_obj((Object*)closure);
        return 1;
    }
    case CONST_TAG_FFI_LIB: {
        uint8_t has_path;
        if (!ctx_read_u8(ctx, &has_path)) return 0;
        if (has_path) {
            uint32_t path_len;
            char* path = ctx_read_string(ctx, &path_len);
            if (!path) return 0;
            extern Value ffi_reload_library(const char* path);
            Value lib_val = ffi_reload_library(path);
            free(path);
            *out_val = lib_val;
            return 1;
        }
        *out_val = val_null();
        return 1;
    }
    default:
        return 0;
    }
}

// ============================================================================
// Chunk 反序列化
// ============================================================================

static int deserialize_chunk_data(DeserializeCtx* ctx, Chunk* chunk) {
    uint32_t filename_len;
    char* filename = ctx_read_string(ctx, &filename_len);
    if (!filename) return 0;
    if (chunk->filename) free(chunk->filename);
    chunk->filename = filename;

    uint32_t local_count, const_count;
    if (!ctx_read_u32(ctx, &local_count) || !ctx_read_u32(ctx, &const_count)) return 0;
    chunk->local_count = (int)local_count;

    if (const_count > 0) {
        chunk->const_capacity = (int)const_count * 2;
        if (chunk->const_capacity < 8) chunk->const_capacity = 8;
        chunk->constants = (Value*)malloc(sizeof(Value) * chunk->const_capacity);
        chunk->const_cnt = 0;
        for (uint32_t i = 0; i < const_count; i++) {
            if (!deserialize_constant(ctx, &chunk->constants[i])) return 0;
            chunk->const_cnt++;
        }
    }

    uint32_t code_len;
    if (!ctx_read_u32(ctx, &code_len)) return 0;
    chunk->len = 0;
    chunk->code_capacity = (int)code_len > 0 ? (int)code_len * 2 : 8;
    chunk->code = (uint8_t*)malloc(chunk->code_capacity);
    if (code_len > 0) {
        if (ctx->pos + code_len > ctx->size) return 0;
        memcpy(chunk->code, ctx->data + ctx->pos, code_len);
        ctx->pos += code_len;
        chunk->len = (int)code_len;
    }

    uint8_t has_lines;
    if (!ctx_read_u8(ctx, &has_lines)) return 0;
    if (has_lines && chunk->len > 0) {
        chunk->lines = (int*)malloc(sizeof(int) * chunk->code_capacity);
        for (int i = 0; i < chunk->len; i++) {
            uint16_t line;
            if (!ctx_read_u16(ctx, &line)) return 0;
            chunk->lines[i] = (int)line;
        }
    }

    return 1;
}

// ============================================================================
// Scope 反序列化
// ============================================================================

static Scope* deserialize_scope_data(DeserializeCtx* ctx) {
    uint32_t global_var_count, global_func_count;
    if (!ctx_read_u32(ctx, &global_var_count) ||
        !ctx_read_u32(ctx, &global_func_count)) return NULL;

    Scope* scope = scope_new(NULL, 0);
    if (!scope) return NULL;
    scope->global_var_index = (int)global_var_count;
    scope->global_func_index = (int)global_func_count;

    uint32_t sym_count;
    if (!ctx_read_u32(ctx, &sym_count)) return NULL;

    for (uint32_t i = 0; i < sym_count; i++) {
        uint8_t kind;
        if (!ctx_read_u8(ctx, &kind)) return NULL;

        uint32_t name_len;
        char* name = ctx_read_string(ctx, &name_len);
        if (!name) return NULL;

        int32_t index;
        uint8_t is_captured, type_kind;
        if (!ctx_read_i32(ctx, &index) ||
            !ctx_read_u8(ctx, &is_captured) ||
            !ctx_read_u8(ctx, &type_kind)) { free(name); return NULL; }

        Symbol* sym = scope_define(scope, name, (SymKind)kind);
        free(name);
        if (sym) {
            sym->index = index;
            sym->is_captured = is_captured;
            if ((TypeKind)type_kind != TYPE_INFER) {
                sym->type = type_new((TypeKind)type_kind);
            }
        }

        uint8_t has_dict_keys;
        if (!ctx_read_u8(ctx, &has_dict_keys)) return NULL;
        if (has_dict_keys) {
            uint32_t dk_count;
            if (!ctx_read_u32(ctx, &dk_count)) return NULL;
            for (uint32_t j = 0; j < dk_count; j++) {
                uint32_t dk_len;
                char* dk = ctx_read_string(ctx, &dk_len);
                if (!dk) return NULL;
                if (sym) symbol_add_dict_key(sym, dk);
                free(dk);
            }
        }

        // 反序列化 clib 函数签名信息
        if ((SymKind)kind == SYM_CLIB && sym) {
            uint32_t func_count;
            if (!ctx_read_u32(ctx, &func_count)) return NULL;
            sym->clib_func_count = (int)func_count;
            sym->clib_func_names = (char**)malloc(func_count * sizeof(char*));
            sym->clib_func_return_types = (TypeInfo**)malloc(func_count * sizeof(TypeInfo*));
            sym->clib_func_param_counts = (int*)malloc(func_count * sizeof(int));
            sym->clib_func_param_types = (TypeInfo***)malloc(func_count * sizeof(TypeInfo**));
            for (uint32_t j = 0; j < func_count; j++) {
                uint32_t fname_len;
                char* fname = ctx_read_string(ctx, &fname_len);
                if (!fname) return NULL;
                sym->clib_func_names[j] = fname;
                uint8_t ret_kind;
                if (!ctx_read_u8(ctx, &ret_kind)) return NULL;
                if ((TypeKind)ret_kind != TYPE_INFER) {
                    sym->clib_func_return_types[j] = type_new((TypeKind)ret_kind);
                } else {
                    sym->clib_func_return_types[j] = NULL;
                }
                uint32_t param_count;
                if (!ctx_read_u32(ctx, &param_count)) return NULL;
                sym->clib_func_param_counts[j] = (int)param_count;
                sym->clib_func_param_types[j] = (TypeInfo**)malloc(param_count * sizeof(TypeInfo*));
                for (uint32_t k = 0; k < param_count; k++) {
                    uint8_t pkind;
                    if (!ctx_read_u8(ctx, &pkind)) return NULL;
                    if ((TypeKind)pkind != TYPE_INFER) {
                        sym->clib_func_param_types[j][k] = type_new((TypeKind)pkind);
                    } else {
                        sym->clib_func_param_types[j][k] = NULL;
                    }
                }
            }
        }

        // 反序列化 cfunc 回调签名信息
        if ((SymKind)kind == SYM_CFUNC && sym) {
            uint32_t param_count;
            if (!ctx_read_u32(ctx, &param_count)) return NULL;
            sym->cfunc_param_count = (int)param_count;
            uint8_t ret_kind;
            if (!ctx_read_u8(ctx, &ret_kind)) return NULL;
            if ((TypeKind)ret_kind != TYPE_INFER) {
                sym->cfunc_return_type = type_new((TypeKind)ret_kind);
            } else {
                sym->cfunc_return_type = NULL;
            }
            sym->cfunc_param_types = (TypeInfo**)malloc(param_count * sizeof(TypeInfo*));
            for (uint32_t j = 0; j < param_count; j++) {
                uint8_t pkind;
                if (!ctx_read_u8(ctx, &pkind)) return NULL;
                if ((TypeKind)pkind != TYPE_INFER) {
                    sym->cfunc_param_types[j] = type_new((TypeKind)pkind);
                } else {
                    sym->cfunc_param_types[j] = NULL;
                }
            }
        }
    }

    return scope;
}

// ============================================================================
// 公共 API
// ============================================================================

SerializeResult chunk_serialize(const char* path, Chunk* chunk, Scope* global_scope) {
    clear_serialized_modules();
    WriteBuffer wb;
    wb_init(&wb);

    uint32_t magic = LENO_BIN_MAGIC;
    wb_write_u32(&wb, magic);
    wb_write_u32(&wb, LENO_BIN_VERSION);
    wb_write_u32(&wb, 0);

    uint64_t src_hash = 0;
    if (chunk && chunk->filename) {
#ifdef _WIN32
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, chunk->filename, -1, NULL, 0);
        wchar_t* widePath = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, chunk->filename, -1, widePath, wideLen);
        FILE* f = _wfopen(widePath, L"rb");
        free(widePath);
#else
        FILE* f = fopen(chunk->filename, "rb");
#endif
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            char* src = (char*)malloc(sz + 1);
            if (src) {
                size_t rd = fread(src, 1, sz, f);
                src[rd] = '\0';
                src_hash = serialize_source_hash(src, rd);
                free(src);
            }
            fclose(f);
        }
    }
    wb_write_u64(&wb, src_hash);

    if (!serialize_scope_data(&wb, global_scope)) {
        wb_free(&wb);
        return SERIALIZE_ERR_FORMAT;
    }

    if (!serialize_chunk(&wb, chunk)) {
        wb_free(&wb);
        return SERIALIZE_ERR_FORMAT;
    }

#ifdef _WIN32
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t* widePath = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath, wideLen);
    FILE* file = _wfopen(widePath, L"wb");
    free(widePath);
#else
    FILE* file = fopen(path, "wb");
#endif

    if (!file) {
        wb_free(&wb);
        return SERIALIZE_ERR_FILE;
    }

    size_t total_size = wb.size;
    size_t written = fwrite(wb.data, 1, total_size, file);
    fclose(file);
    wb_free(&wb);

    if (written != total_size) {
        return SERIALIZE_ERR_WRITE;
    }
    return SERIALIZE_OK;
}

SerializeResult chunk_deserialize(const char* path, Chunk* out_chunk, Scope** out_scope) {
#ifdef _WIN32
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t* widePath = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath, wideLen);
    FILE* file = _wfopen(widePath, L"rb");
    free(widePath);
#else
    FILE* file = fopen(path, "rb");
#endif

    if (!file) return SERIALIZE_ERR_FILE;

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size < 20) {
        fclose(file);
        return SERIALIZE_ERR_FORMAT;
    }

    uint8_t* data = (uint8_t*)malloc(file_size);
    if (!data) {
        fclose(file);
        return SERIALIZE_ERR_MEMORY;
    }

    size_t read = fread(data, 1, file_size, file);
    fclose(file);

    if ((long)read != file_size) {
        free(data);
        return SERIALIZE_ERR_READ;
    }

    DeserializeCtx ctx;
    ctx.data = data;
    ctx.size = (size_t)file_size;
    ctx.pos = 0;

    uint32_t magic, version, flags;
    uint64_t src_hash;
    if (!ctx_read_u32(&ctx, &magic) ||
        !ctx_read_u32(&ctx, &version) ||
        !ctx_read_u32(&ctx, &flags) ||
        !ctx_read_u64(&ctx, &src_hash)) {
        free(data);
        return SERIALIZE_ERR_FORMAT;
    }

    if (magic != LENO_BIN_MAGIC) {
        free(data);
        return SERIALIZE_ERR_MAGIC;
    }

    if (version != LENO_BIN_VERSION) {
        free(data);
        return SERIALIZE_ERR_VERSION;
    }

    Scope* scope = deserialize_scope_data(&ctx);
    if (!scope) {
        free(data);
        return SERIALIZE_ERR_FORMAT;
    }

    chunk_init(out_chunk);
    if (!deserialize_chunk_data(&ctx, out_chunk)) {
        scope_free(scope);
        chunk_free(out_chunk);
        free(data);
        return SERIALIZE_ERR_FORMAT;
    }

    *out_scope = scope;
    free(data);
    return SERIALIZE_OK;
}

int serialize_is_binary_file(const char* path) {
    if (!path) return 0;
    size_t len = strlen(path);
    size_t ext_len = strlen(LENO_BIN_EXT);
    if (len < ext_len) return 0;
    return strcmp(path + len - ext_len, LENO_BIN_EXT) == 0;
}

char* serialize_get_bin_path(const char* source_path) {
    if (!source_path) return NULL;

    size_t len = strlen(source_path);
    const char* ext = strrchr(source_path, '.');

    char* bin_path;
    if (ext && (strcmp(ext, ".leno") == 0 || strcmp(ext, ".lenb") == 0)) {
        size_t base_len = ext - source_path;
        bin_path = (char*)malloc(base_len + strlen(LENO_BIN_EXT) + 1);
        memcpy(bin_path, source_path, base_len);
        strcpy(bin_path + base_len, LENO_BIN_EXT);
    } else {
        bin_path = (char*)malloc(len + strlen(LENO_BIN_EXT) + 1);
        strcpy(bin_path, source_path);
        strcat(bin_path, LENO_BIN_EXT);
    }
    return bin_path;
}

int serialize_cache_is_valid(const char* source_path, const char* bin_path) {
    struct stat src_stat, bin_stat;
    if (stat(source_path, &src_stat) != 0 || stat(bin_path, &bin_stat) != 0) return 0;
    return bin_stat.st_mtime >= src_stat.st_mtime;
}
