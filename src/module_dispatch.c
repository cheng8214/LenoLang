#include "include/lenolang.h"
#include "include/module_dispatch.h"
#include <stdio.h>

static ModuleCompileFunc g_compile_func = NULL;

void set_module_compile_func(ModuleCompileFunc func) {
    g_compile_func = func;
}

ModuleCompileFunc get_module_compile_func(void) {
    return g_compile_func;
}

// 递归扫描 chunk 常量池，设置所有函数/闭包的 module 指针
static void update_chunk_function_module(Chunk* chunk, ObjModule* dst_module) {
    if (!chunk || chunk->const_cnt <= 0) return;
    for (int i = 0; i < chunk->const_cnt; i++) {
        Value val = chunk->constants[i];
        if (!val_is_obj(val)) continue;
        Object* obj = val_as_obj(val);
        if (obj->type == OBJ_FUNCTION) {
            ObjFunction* func = (ObjFunction*)obj;
            func->module = dst_module;
            // 递归扫描函数的 chunk
            if (func->chunk) {
                update_chunk_function_module(func->chunk, dst_module);
            }
        } else if (obj->type == OBJ_CLOSURE) {
            ObjClosure* closure = (ObjClosure*)obj;
            if (closure->function) {
                closure->function->module = dst_module;
                if (closure->function->chunk) {
                    update_chunk_function_module(closure->function->chunk, dst_module);
                }
            }
        } else if (obj->type == OBJ_STRUCT_DEF) {
            ObjStructDef* sdef = (ObjStructDef*)obj;
            for (int m = 0; m < sdef->method_count; m++) {
                if (sdef->methods[m].func) {
                    sdef->methods[m].func->module = dst_module;
                    if (sdef->methods[m].func->chunk) {
                        update_chunk_function_module(sdef->methods[m].func->chunk, dst_module);
                    }
                }
            }
        }
    }
}

void update_module_function_ptrs(ObjModule* src_module, ObjModule* dst_module) {
    // 更新导出表
    if (dst_module->exports) {
        for (int i = 0; i < dst_module->exports->capacity; i++) {
            ObjDictEntry* entry = &dst_module->exports->entries[i];
            if (!val_is_null(entry->key) && entry->key != DICT_TOMBSTONE_VAL && !val_is_null(entry->value)) {
                if (val_is_obj(entry->value)) {
                    Object* obj = val_as_obj(entry->value);
                    if (obj->type == OBJ_CLOSURE) {
                        ObjClosure* closure = (ObjClosure*)obj;
                        if (closure->function) {
                            closure->function->module = dst_module;
                            // 递归修复嵌套闭包的 module 指针
                            if (closure->function->chunk) {
                                update_chunk_function_module(closure->function->chunk, dst_module);
                            }
                        }
                    } else if (obj->type == OBJ_FUNCTION) {
                        ObjFunction* func = (ObjFunction*)obj;
                        func->module = dst_module;
                        if (func->chunk) {
                            update_chunk_function_module(func->chunk, dst_module);
                        }
                    }
                }
            }
        }
    }
    // 更新全局变量表
    if (dst_module->globals && dst_module->global_count > 0) {
        for (int i = 0; i < dst_module->global_count; i++) {
            Value val = dst_module->globals[i];
            if (val_is_obj(val)) {
                Object* obj = val_as_obj(val);
                if (obj->type == OBJ_CLOSURE) {
                    ObjClosure* closure = (ObjClosure*)obj;
                    if (closure->function) {
                        closure->function->module = dst_module;
                        if (closure->function->chunk) {
                            update_chunk_function_module(closure->function->chunk, dst_module);
                        }
                    }
                } else if (obj->type == OBJ_FUNCTION) {
                    ObjFunction* func = (ObjFunction*)obj;
                    func->module = dst_module;
                    if (func->chunk) {
                        update_chunk_function_module(func->chunk, dst_module);
                    }
                }
            }
        }
    }
    // 递归扫描 init_chunk
    if (dst_module->init_chunk) {
        update_chunk_function_module(dst_module->init_chunk, dst_module);
    }
    // 更新 struct 方法的 module 指针
    struct_def_update_method_modules(src_module, dst_module);
}
