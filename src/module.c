#include "include/lenolang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// 模块系统实现
// ============================================================================

static inline VM* get_current_vm(void) {
    extern THREAD_LOCAL VM* current_exec_vm;
    extern VM vm;
    return current_exec_vm ? current_exec_vm : &vm;
}

// 创建模块对象
ObjModule* module_new(const char* name) {
    ObjModule* module = (ObjModule*)gc_alloc(sizeof(ObjModule), OBJ_MODULE);
    if (!module) return NULL;
    
    module->name = strdup(name);
    module->source_path = NULL;
    module->globals = NULL;
    module->global_count = 0;
    module->global_capacity = 0;
    module->exports = dict_new(16);
    module->frame = NULL;
    module->native_imports = NULL;
    module->native_import_count = 0;
    module->init_chunk = NULL;
    module->initialized = 0;
    module->export_mappings = NULL;
    module->export_mapping_count = 0;
    
    return module;
}

// 创建模块帧
ModuleFrame* module_frame_new(ObjModule* module) {
    ModuleFrame* frame = (ModuleFrame*)malloc(sizeof(ModuleFrame));
    if (!frame) return NULL;
    
    frame->module = module;
    frame->globals = module->globals;
    frame->global_count = module->global_count;
    frame->global_capacity = module->global_capacity;
    frame->parent = NULL;
    
    return frame;
}

// 进入模块帧
void module_frame_enter(ModuleFrame* frame) {
    if (!frame) return;
    
    VM* target_vm = get_current_vm();
    frame->parent = target_vm->current_module_frame;
    target_vm->current_module_frame = frame;
}

// 退出模块帧
void module_frame_exit(void) {
    VM* target_vm = get_current_vm();
    if (!target_vm->current_module_frame) return;
    target_vm->current_module_frame = target_vm->current_module_frame->parent;
}

// 确保模块变量数组容量
int module_ensure_var_capacity(int slot) {
    VM* target_vm = get_current_vm();
    if (!target_vm->current_module_frame) return 0;
    
    if (slot >= target_vm->current_module_frame->global_capacity) {
        int new_capacity = target_vm->current_module_frame->global_capacity;
        if (new_capacity == 0) new_capacity = 8;
        while (new_capacity <= slot) {
            new_capacity = new_capacity * 2;
        }
        
        Value* new_globals = (Value*)realloc(target_vm->current_module_frame->globals, 
                                              new_capacity * sizeof(Value));
        if (!new_globals) return 0;
        
        for (int i = target_vm->current_module_frame->global_capacity; i < new_capacity; i++) {
            new_globals[i] = val_null();
        }
        
        target_vm->current_module_frame->globals = new_globals;
        target_vm->current_module_frame->global_capacity = new_capacity;
        
        if (target_vm->current_module_frame->module) {
            target_vm->current_module_frame->module->globals = new_globals;
            target_vm->current_module_frame->module->global_capacity = new_capacity;
        }
    }
    
    if (slot >= target_vm->current_module_frame->global_count) {
        target_vm->current_module_frame->global_count = slot + 1;
        if (target_vm->current_module_frame->module) {
            target_vm->current_module_frame->module->global_count = slot + 1;
        }
    }
    
    return 1;
}

// 获取当前模块帧中的变量
Value module_get_var(int index) {
    VM* target_vm = get_current_vm();
    if (!target_vm->current_module_frame) {
        return val_null();
    }
    if (index < 0 || index >= target_vm->current_module_frame->global_count) {
        return val_null();
    }
    return target_vm->current_module_frame->globals[index];
}

// 设置当前模块帧中的变量
void module_set_var(int index, Value value) {
    VM* target_vm = get_current_vm();
    if (!target_vm->current_module_frame || index < 0) return;
    
    if (!module_ensure_var_capacity(index)) return;
    
    target_vm->current_module_frame->globals[index] = value;
}

// 模块函数定义
void module_define_func(int index, Value func) {
    module_set_var(index, func);
}

// 获取模块函数
Value module_get_func(int index) {
    return module_get_var(index);
}

// 扫描 chunk 常量池中的 cstruct/face/struct/enum 定义并注册到全局表
void register_defs_from_chunk(Chunk* chunk) {
    if (!chunk) return;
    for (int i = 0; i < chunk->const_cnt; i++) {
        Value* val = &chunk->constants[i];
        if (val_is_obj(*val) && val_as_obj(*val)->type == OBJ_CSTRUCT_DEF) {
            ObjCStructDef* def = (ObjCStructDef*)val_as_obj(*val);
            if (!cstruct_def_find(def->name)) {
                cstruct_def_register(def);
            }
        } else if (val_is_obj(*val) && val_as_obj(*val)->type == OBJ_FACE_DEF) {
            ObjFaceDef* def = (ObjFaceDef*)val_as_obj(*val);
            if (!face_def_find(def->name)) {
                face_def_register(def);
            }
        } else if (val_is_obj(*val) && val_as_obj(*val)->type == OBJ_STRUCT_DEF) {
            ObjStructDef* def = (ObjStructDef*)val_as_obj(*val);
            if (!struct_def_find(def->name)) {
                struct_def_register(def);
            }
        } else if (val_is_obj(*val) && val_as_obj(*val)->type == OBJ_ENUM_DEF) {
            ObjEnumDef* def = (ObjEnumDef*)val_as_obj(*val);
            if (!enum_def_find(def->name)) {
                enum_def_register(def);
            }
        } else if (val_is_obj(*val) && val_as_obj(*val)->type == OBJ_FUNCTION) {
            ObjFunction* func = (ObjFunction*)val_as_obj(*val);
            if (func->chunk) {
                register_defs_from_chunk(func->chunk);
            }
        }
    }
}
