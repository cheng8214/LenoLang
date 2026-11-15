#include "include/lenolang.h"
#include "include/native.h"
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
    module->slot_export_names = NULL;   // 槽位→导出名反查表（初始化末尾建，见 10_vars_module.inc）
    module->slot_export_cap = 0;
    module->use_reexport_names = NULL;
    module->use_reexport_kinds = NULL;
    module->use_reexport_count = 0;
    module->tls_slots = NULL;           // thread_local var 槽位表（OP_TLS_DECL 登记时才建）
    module->tls_slot_cap = 0;
    
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

// 当前模块帧所属的模块（不在任何模块 init 中时返回 NULL）。
// 供 OP_DEFINE_STRUCT / ENUM / FACE / CSTRUCT 记录"声明来源"（跨模块同名检测，见 S2）：
// 这些操作码是在模块自己的 init_chunk 里执行的，此时当前模块就在模块帧里。
ObjModule* module_frame_current_module(void) {
    VM* target_vm = get_current_vm();
    if (!target_vm || !target_vm->current_module_frame) return NULL;
    return target_vm->current_module_frame->module;
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
    // ★ 2026-11-15：thread_local var 的槽位要读**当前线程**的值（不在共享的 globals 里）
    if (module_tls_id_of(target_vm->current_module_frame->module, index) >= 0) {
        return module_tls_get(target_vm, target_vm->current_module_frame->module, index);
    }
    return target_vm->current_module_frame->globals[index];
}

// ★ 2026-11-15（P0 跨堆写入检测）：**子线程把本线程堆的对象写进模块级 var** ⇒ 打一次警告
//
// 背景（Aria 服务端 CRC/QR 两次实测）：模块级 `var`（ObjModule.globals）是**全线程共享的
//   同一份槽**（module_frame_new 直接把 module->globals 交给帧），而每个子线程有**独立的
//   堆 / GC**。子线程写进去的对象归属它自己的堆 ⇒ 那个线程退出或 GC 之后，共享槽指向
//   已回收内存，别的线程读到的是**残值**（实测 CRC 表返回 185 / -1 / 0）。
// 判定：对象头里记了分配它的线程序号（Object.owner_thread，见 gc.c / leno_value.h）；
//   与**模块对象自己的** owner 不同 ⇒ 跨堆引用。
// 去重：每个线程只提示一次（同一个人写的同一类错，刷屏没有意义）⇒ 写完第一个就能改。
static THREAD_LOCAL int g_cross_heap_warned = 0;

// 跨堆写入检测的**唯一实现**（两个调用点：module_set_var 与 VM 的 OP_SET_MODULE_VAR ——
// 编译后的 `m.g = ...` 走后者，只改前者会漏掉最常见的路径）。
void module_warn_cross_heap_write(ObjModule* mod, int slot, Value value) {
    if (g_cross_heap_warned) return;
    if (!mod || !val_is_obj(value)) return;
    uint8_t value_owner = ((Object*)val_as_obj(value))->owner_thread;
    uint8_t mod_owner = ((Object*)mod)->owner_thread;
    if (value_owner == mod_owner) return;
    g_cross_heap_warned = 1;
    fprintf(stderr,
        "[警告] 子线程把**本线程堆上的对象**写进了模块级变量（模块 '%s' 槽 %d，"
        "线程序号 %u / 模块属于 %u）——\n"
        "       模块级 var 是所有线程**共享的同一份槽**，而对象属于写它的那个线程堆："
        "该线程退出/GC 之后这个槽会指向已回收内存（读到 185 / -1 / 0 这类残值）。\n"
        "       建议：① 每线程状态改用 threads.local_set/get；"
        "② 或由主线程构建后**只读**（别在子线程写模块级 var）。\n"
        "       （本条每线程只提示一次）\n",
        mod->name ? mod->name : "?", slot,
        (unsigned)value_owner, (unsigned)mod_owner);
    fflush(stderr);
}

// 设置当前模块帧中的变量
void module_set_var(int index, Value value) {
    VM* target_vm = get_current_vm();
    if (!target_vm->current_module_frame || index < 0) return;

    // ★ 2026-11-15：thread_local var ⇒ 写到**当前线程**的槽位（不碰共享的 globals，
    //   因此也不会触发跨堆写入警告 —— 这正是 thread_local 要解决的问题）。
    ObjModule* mod = target_vm->current_module_frame->module;
    if (module_tls_id_of(mod, index) >= 0) {
        module_tls_set(target_vm, mod, index, value);
        return;
    }

    if (!module_ensure_var_capacity(index)) return;

    module_warn_cross_heap_write(mod, index, value);

    target_vm->current_module_frame->globals[index] = value;
}

// ============================================================================
// `thread_local var`（每线程一份的模块级变量，2026-11-15）
// ----------------------------------------------------------------------------
// 语义：`thread_local var g: Dict = {}` ⇒ 每个线程各一份，首次访问时按声明初值新建。
// 为什么需要（Aria 服务端两次实测的共同根因）：模块级 `var` 是**全线程共享的同一份槽**
//   （ObjModule.globals），子线程往里写"本线程堆上的对象"= 跨堆引用 ⇒ 线程退出/GC 后
//   读到残值（CRC 表 185/-1/0）。正确做法原本只有 `threads.local_set/get`（字符串 key +
//   无类型 + 手加前缀），这个特性把它升格成语言语法。
// 存储：值放 **VM.tls_slots**（每线程一份，随 VM 结构体走，GC 已在 mark_roots 里标记），
//   模块只保存"这个槽是 thread_local + 别的线程怎么建默认值"（TlsSlotInfo）。
// ============================================================================

// 全局 TLS 槽位号（进程内唯一；模块是进程级共享对象，所以号也只分配一次）
static volatile long g_tls_next_id = 0;

static int tls_alloc_id(void) {
#ifdef _MSC_VER
    return (int)_InterlockedIncrement(&g_tls_next_id) - 1;
#else
    return (int)__sync_add_and_fetch(&g_tls_next_id, 1) - 1;
#endif
}

// 本线程的槽位数组按需扩容（冷路径：只在首次访问 / 新槽位号更大时走）
static int tls_ensure_capacity(VM* vm, int id) {
    if (!vm || id < 0) return 0;
    if (id < vm->tls_slot_capacity) {
        if (id >= vm->tls_slot_count) vm->tls_slot_count = id + 1;
        return 1;
    }
    int cap = vm->tls_slot_capacity < 8 ? 8 : vm->tls_slot_capacity;
    while (cap <= id) cap *= 2;
    Value* slots = (Value*)realloc(vm->tls_slots, sizeof(Value) * (size_t)cap);
    if (!slots) return 0;
    vm->tls_slots = slots;
    unsigned char* ready = (unsigned char*)realloc(vm->tls_slot_ready, (size_t)cap);
    if (!ready) return 0;
    vm->tls_slot_ready = ready;
    for (int i = vm->tls_slot_capacity; i < cap; i++) {
        slots[i] = val_null();
        ready[i] = 0;
    }
    vm->tls_slot_capacity = cap;
    if (id >= vm->tls_slot_count) vm->tls_slot_count = id + 1;
    return 1;
}

// 按 TlsSlotInfo 给**本线程**建默认值
//   ⚠ Array / Dict 必须新建空容器：不能把声明处那个对象跨堆共享（那正是要避免的事）
static Value tls_make_default(const TlsSlotInfo* info) {
    switch (info->kind) {
        case TLS_DEF_INT:
        case TLS_DEF_FLOAT:
        case TLS_DEF_BOOL:
        case TLS_DEF_STRING:
            return info->def;
        case TLS_DEF_ARRAY: {
            ObjArray* arr = arr_new(4);
            return arr ? val_obj((Object*)arr) : val_null();
        }
        case TLS_DEF_DICT: {
            ObjDict* d = dict_new(8);
            return d ? val_obj((Object*)d) : val_null();
        }
        default:
            return val_null();
    }
}

// 登记一个 thread_local 槽（OP_TLS_DECL 执行时调用一次；模块顶层）
int module_tls_declare(ObjModule* mod, int slot, Value init) {
    if (!mod || slot < 0) return -1;

    // 1. 槽位表按需扩容（与 globals 分开，绝大多数模块这里始终是 NULL）
    if (slot >= mod->tls_slot_cap) {
        int cap = mod->tls_slot_cap < 8 ? 8 : mod->tls_slot_cap;
        while (cap <= slot) cap *= 2;
        TlsSlotInfo* info = (TlsSlotInfo*)realloc(mod->tls_slots, sizeof(TlsSlotInfo) * (size_t)cap);
        if (!info) return -1;
        mod->tls_slots = info;
        for (int i = mod->tls_slot_cap; i < cap; i++) {
            info[i].tls_id = -1;
            info[i].kind = TLS_DEF_NULL;
            info[i].def = val_null();
        }
        mod->tls_slot_cap = cap;
    }

    TlsSlotInfo* info = &mod->tls_slots[slot];
    if (info->tls_id < 0) {
        info->tls_id = tls_alloc_id();
        // 2. 默认值怎么建：按**运行期种类**（codegen 已保证初值只能是字面量/空容器）
        if (val_is_null(init)) {
            info->kind = TLS_DEF_NULL;
            info->def = val_null();
        } else if (val_is_int(init)) {
            info->kind = TLS_DEF_INT;
            info->def = init;
        } else if (val_is_bool(init)) {
            info->kind = TLS_DEF_BOOL;
            info->def = init;
        } else if (val_is_float(init)) {
            info->kind = TLS_DEF_FLOAT;
            info->def = init;
        } else if (val_is_obj(init)) {
            ObjType t = val_as_obj(init)->type;
            if (t == OBJ_STRING) {
                info->kind = TLS_DEF_STRING;
                info->def = init;
            } else if (t == OBJ_ARRAY) {
                info->kind = TLS_DEF_ARRAY;   // 每线程新建空数组
                info->def = val_null();
            } else if (t == OBJ_DICT) {
                info->kind = TLS_DEF_DICT;
                info->def = val_null();
            } else {
                info->kind = TLS_DEF_NULL;
                info->def = val_null();
            }
        } else {
            info->kind = TLS_DEF_NULL;
            info->def = val_null();
        }
    }

    // 3. 写入**执行声明的这个线程**的槽（声明处的初值）
    VM* vm = get_current_vm();
    if (!tls_ensure_capacity(vm, info->tls_id)) return -1;
    vm->tls_slots[info->tls_id] = init;
    vm->tls_slot_ready[info->tls_id] = 1;
    return info->tls_id;
}

Value module_tls_get(VM* vm, ObjModule* mod, int slot) {
    int id = module_tls_id_of(mod, slot);
    if (id < 0 || !vm) return val_null();
    if (!tls_ensure_capacity(vm, id)) return val_null();
    if (!vm->tls_slot_ready[id]) {
        vm->tls_slots[id] = tls_make_default(&mod->tls_slots[slot]);
        vm->tls_slot_ready[id] = 1;
    }
    return vm->tls_slots[id];
}

void module_tls_set(VM* vm, ObjModule* mod, int slot, Value value) {
    int id = module_tls_id_of(mod, slot);
    if (id < 0 || !vm) return;
    if (!tls_ensure_capacity(vm, id)) return;
    vm->tls_slots[id] = value;
    vm->tls_slot_ready[id] = 1;
}

// 模块函数定义
void module_define_func(int index, Value func) {
    module_set_var(index, func);
}// 获取模块函数
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

// ============================================================================
// 模块函数指针修复（反序列化后需要调用）
// ============================================================================

static ObjModule** fix_visited = NULL;
static int fix_visited_count = 0;
static int fix_visited_capacity = 0;

static int is_module_visited(ObjModule* mod) {
    for (int i = 0; i < fix_visited_count; i++) {
        if (fix_visited[i] == mod) return 1;
    }
    return 0;
}

static void mark_module_visited(ObjModule* mod) {
    if (fix_visited_count >= fix_visited_capacity) {
        fix_visited_capacity = fix_visited_capacity == 0 ? 16 : fix_visited_capacity * 2;
        fix_visited = (ObjModule**)realloc(fix_visited, fix_visited_capacity * sizeof(ObjModule*));
    }
    fix_visited[fix_visited_count++] = mod;
}

static void fix_single_module_impl(ObjModule* mod) {
    if (!mod) return;
    if (is_module_visited(mod)) return;
    mark_module_visited(mod);
    if (mod->globals) {
        for (int j = 0; j < mod->global_count; j++) {
            Value* gv = &mod->globals[j];
            if (val_is_obj(*gv)) {
                Object* obj = val_as_obj(*gv);
                if (obj->type == OBJ_FUNCTION) {
                    ((ObjFunction*)obj)->module = mod;
                    if (((ObjFunction*)obj)->chunk) {
                        register_defs_from_chunk(((ObjFunction*)obj)->chunk);
                    }
                } else if (obj->type == OBJ_CLOSURE) {
                    ObjClosure* cl = (ObjClosure*)obj;
                    if (cl->function) {
                        cl->function->module = mod;
                        if (cl->function->chunk) {
                            register_defs_from_chunk(cl->function->chunk);
                        }
                    }
                } else if (obj->type == OBJ_STRUCT_DEF) {
                    ObjStructDef* def = (ObjStructDef*)obj;
                    if (def->methods) {
                        for (int m = 0; m < def->method_count; m++) {
                            if (def->methods[m].func) {
                                def->methods[m].func->module = mod;
                                if (def->methods[m].func->chunk) {
                                    register_defs_from_chunk(def->methods[m].func->chunk);
                                }
                            }
                            if (def->methods[m].closure && def->methods[m].closure->function) {
                                def->methods[m].closure->function->module = mod;
                                if (def->methods[m].closure->function->chunk) {
                                    register_defs_from_chunk(def->methods[m].closure->function->chunk);
                                }
                            }
                        }
                    }
                    if (!struct_def_find(def->name)) {
                        struct_def_register(def);
                    }
                } else if (obj->type == OBJ_ENUM_DEF) {
                    ObjEnumDef* def = (ObjEnumDef*)obj;
                    if (!enum_def_find(def->name)) {
                        enum_def_register(def);
                    }
                } else if (obj->type == OBJ_CSTRUCT_DEF) {
                    ObjCStructDef* def = (ObjCStructDef*)obj;
                    if (!cstruct_def_find(def->name)) {
                        cstruct_def_register(def);
                    }
                } else if (obj->type == OBJ_FACE_DEF) {
                    ObjFaceDef* def = (ObjFaceDef*)obj;
                    if (!face_def_find(def->name)) {
                        face_def_register(def);
                    }
                } else if (obj->type == OBJ_MODULE) {
                    fix_single_module_impl((ObjModule*)obj);
                }
            }
        }
    }
    if (mod->exports) {
        for (int j = 0; j < mod->exports->order_count; j++) {
            Value key = mod->exports->order[j];
            if (val_is_null(key)) continue;
            Value ev = dict_get(mod->exports, key);
            if (val_is_obj(ev)) {
                Object* obj = val_as_obj(ev);
                if (obj->type == OBJ_FUNCTION) {
                    ((ObjFunction*)obj)->module = mod;
                    if (((ObjFunction*)obj)->chunk) {
                        register_defs_from_chunk(((ObjFunction*)obj)->chunk);
                    }
                } else if (obj->type == OBJ_CLOSURE) {
                    ObjClosure* cl = (ObjClosure*)obj;
                    if (cl->function) {
                        cl->function->module = mod;
                        if (cl->function->chunk) {
                            register_defs_from_chunk(cl->function->chunk);
                        }
                    }
                } else if (obj->type == OBJ_STRUCT_DEF) {
                    ObjStructDef* def = (ObjStructDef*)obj;
                    if (def->methods) {
                        for (int m = 0; m < def->method_count; m++) {
                            if (def->methods[m].func) {
                                def->methods[m].func->module = mod;
                                if (def->methods[m].func->chunk) {
                                    register_defs_from_chunk(def->methods[m].func->chunk);
                                }
                            }
                            if (def->methods[m].closure && def->methods[m].closure->function) {
                                def->methods[m].closure->function->module = mod;
                                if (def->methods[m].closure->function->chunk) {
                                    register_defs_from_chunk(def->methods[m].closure->function->chunk);
                                }
                            }
                        }
                    }
                    if (!struct_def_find(def->name)) {
                        struct_def_register(def);
                    }
                } else if (obj->type == OBJ_ENUM_DEF) {
                    ObjEnumDef* def = (ObjEnumDef*)obj;
                    if (!enum_def_find(def->name)) {
                        enum_def_register(def);
                    }
                } else if (obj->type == OBJ_CSTRUCT_DEF) {
                    ObjCStructDef* def = (ObjCStructDef*)obj;
                    if (!cstruct_def_find(def->name)) {
                        cstruct_def_register(def);
                    }
                } else if (obj->type == OBJ_FACE_DEF) {
                    ObjFaceDef* def = (ObjFaceDef*)obj;
                    if (!face_def_find(def->name)) {
                        face_def_register(def);
                    }
                } else if (obj->type == OBJ_MODULE) {
                    fix_single_module_impl((ObjModule*)obj);
                }
            }
        }
    }
    if (mod->native_imports && mod->native_import_count > 0) {
        for (int j = 0; j < mod->native_import_count; j++) {
            native_init_module(mod->native_imports[j]);
        }
    }
    if (mod->init_chunk) {
        register_defs_from_chunk(mod->init_chunk);
    }
}

void fix_single_module(ObjModule* mod) {
    fix_visited_count = 0;
    fix_single_module_impl(mod);
}

void fix_module_function_ptrs(Chunk* chunk) {
    if (!chunk) return;
    for (int i = 0; i < chunk->const_cnt; i++) {
        Value* val = &chunk->constants[i];
        if (val_is_obj(*val) && val_as_obj(*val)->type == OBJ_MODULE) {
            ObjModule* mod = (ObjModule*)val_as_obj(*val);
            fix_single_module(mod);
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
        }
        if (val_is_obj(*val) && val_as_obj(*val)->type == OBJ_FUNCTION) {
            ObjFunction* func = (ObjFunction*)val_as_obj(*val);
            if (func->chunk) {
                fix_module_function_ptrs(func->chunk);
            }
        }
    }
}
