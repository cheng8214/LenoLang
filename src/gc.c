// ============================================================================
// 分代垃圾回收器（Generational GC）
//
// 架构：
//   年轻代（Young Generation）：
//     - 新对象分配在年轻代
//     - Minor GC 只扫描年轻代，存活对象 survived 计数递增
//     - survived >= promote_age 的对象晋升到老年代
//
//   老年代（Old Generation）：
//     - 长寿命对象存储在老年代
//     - Major GC 扫描全部（年轻代 + 老年代）
//     - 触发频率远低于 Minor GC
//
//   写屏障（Write Barrier）：
//     - 当老年代对象引用年轻代对象时，将老年代对象加入 remembered set
//     - Minor GC 时额外扫描 remembered set，确保年轻代可达对象不被误回收
//
// 收集策略：
//   gc_alloc() → 年轻代分配超阈值 → Minor GC
//   Minor GC 后仍超阈值 → 总内存超老年代阈值 → Major GC
//   gc_collect() → 根据总内存自动选择 Minor/Major GC
// ============================================================================

#include "include/lenolang.h"
#include "include/string_table.h"
#include "include/native.h"
#include "include/leno_vm.h"
#include "module/guis/guis_internal.h"
#include "module/guis/leno_guis.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>



// GC 全局实例（线程局部存储）
THREAD_LOCAL GC gc = {0};

// 字符串字典的墓碑标记（用于区分空槽和已删除槽）
extern _Thread_local ObjString* tombstone;

// 安全的字符串长度计算（防止野指针导致崩溃）
static size_t safe_strlen(const char* str) {
    if (!str) return 0;
    // 简单的边界检查，防止野指针导致无限循环
    size_t len = 0;
    const char* p = str;
    while (len < 1024 * 1024) {  // 最大1MB
        if (*p == '\0') return len;
        p++;
        len++;
    }
    return 0;  // 超过边界，可能是野指针
}

// 检查对象类型是否有效
static int is_valid_obj_type(ObjType type) {
    return type >= OBJ_STRING && type < OBJ_NONE;
}

// ============================================================================
// 写屏障（Write Barrier）
// ============================================================================

// 将对象加入 remembered set（去重）
static void remembered_set_add(Object* obj) {
    // 动态扩容
    if (gc.remembered_count >= gc.remembered_capacity) {
        int new_cap = gc.remembered_capacity * 2;
        if (new_cap < GC_REMEMBERED_INIT) new_cap = GC_REMEMBERED_INIT;
        Object** new_set = (Object**)realloc(gc.remembered_set, new_cap * sizeof(Object*));
        if (!new_set) return;
        gc.remembered_set = new_set;
        gc.remembered_capacity = new_cap;
    }
    // 去重检查
    for (int i = 0; i < gc.remembered_count; i++) {
        if (gc.remembered_set[i] == obj) return;
    }
    gc.remembered_set[gc.remembered_count++] = obj;
}

// 写屏障：老年代对象 holder 的 Value 字段被写入年轻代引用时调用
void gc_write_barrier(Object* holder, Value value) {
    if (!val_is_obj(value)) return;
    if (holder->generation != GEN_OLD) return;
    Object* val_obj = val_as_obj(value);
    if (val_obj->generation == GEN_YOUNG) {
        remembered_set_add(holder);
    }
}

// 写屏障：老年代对象 holder 的 Object* 字段被写入年轻代引用时调用
void gc_write_barrier_obj(Object* holder, Object* value_obj) {
    if (holder->generation != GEN_OLD) return;
    if (value_obj->generation == GEN_YOUNG) {
        remembered_set_add(holder);
    }
}

// ============================================================================
// GC 初始化与控制
// ============================================================================

void gc_init(void) {
    gc.young_heap = NULL;
    gc.old_heap = NULL;
    gc.young_allocated = 0;
    gc.old_allocated = 0;
    gc.young_threshold = GC_YOUNG_THRESHOLD;
    gc.old_threshold = GC_OLD_THRESHOLD;
    gc.running = 0;
    gc.mode = GC_MODE_FULL;
    gc.vm = NULL;
    gc.enabled = 1;
    gc.remembered_set = NULL;
    gc.remembered_count = 0;
    gc.remembered_capacity = 0;
    gc.promote_age = GC_PROMOTE_AGE;
    gc.minor_gc_count = 0;
    gc.extra_roots = NULL;
    gc.extra_root_count = 0;
    gc.extra_root_capacity = 0;
}

// 设置 GC 开关（1=启用，0=禁用）
void gc_set_enabled(int enabled) {
    gc.enabled = enabled;
}

// 获取 GC 开关状态
int gc_get_enabled(void) {
    return gc.enabled;
}

void gc_push_root(Value* ptr) {
    if (gc.extra_root_count >= gc.extra_root_capacity) {
        int new_cap = gc.extra_root_capacity * 2;
        if (new_cap < 64) new_cap = 64;
        Value** new_roots = (Value**)realloc(gc.extra_roots, new_cap * sizeof(Value*));
        if (!new_roots) return;
        gc.extra_roots = new_roots;
        gc.extra_root_capacity = new_cap;
    }
    gc.extra_roots[gc.extra_root_count++] = ptr;
}

void gc_pop_root(void) {
    if (gc.extra_root_count > 0) {
        gc.extra_root_count--;
    }
}

// ============================================================================
// 对象分配
// ============================================================================

// 分配 GC 管理的对象，新对象分配到年轻代
Object* gc_alloc(size_t size, ObjType type) {
    // 年轻代分配超阈值时直接触发 GC
    // 此时 VM 状态一致：栈、帧、局部变量都是有效的，GC 可以正确标记所有根集合
    if (gc.enabled && gc.young_allocated + size > gc.young_threshold && !gc.running) {
        gc_minor_collect();
        // Minor GC 后仍超阈值，检查是否需要 Major GC
        if (gc.young_allocated > gc.young_threshold) {
            if (gc.old_allocated + gc.young_allocated > gc.old_threshold) {
                gc_major_collect();
            }
        }
    }

    Object* obj = (Object*)malloc(size);

    // 分配失败时尝试 Major GC 释放内存
    if (!obj && gc.enabled && !gc.running) {
        gc_major_collect();
        obj = (Object*)malloc(size);
        if (!obj) {
            // 放宽阈值，避免反复触发 GC
            gc.old_threshold = gc.old_allocated + gc.young_allocated + (1024 * 1024);
        }
    }

    if (!obj) {
        error_add(ERR_RUNTIME, 0, "内存分配失败");
        return NULL;
    }

    // 初始化对象头部，新对象默认为年轻代
    obj->type = type;
    // GC运行期间分配的对象自动标记为存活，避免被误回收
    obj->marked = gc.running ? 1 : 0;
    obj->flags = 0;
    obj->generation = GEN_YOUNG;
    obj->survived = 0;
    obj->size = size;
    obj->next = gc.young_heap;
    gc.young_heap = obj;
    gc.young_allocated += size;

    return obj;
}

// VM 安全点 GC 检查：在指令边界调用，确保 VM 状态一致
void gc_check_safe_point(void) {
    if (gc.enabled && !gc.running) {
        if (gc.young_allocated > gc.young_threshold) {
            gc_minor_collect();
            if (gc.young_allocated > gc.young_threshold) {
                if (gc.old_allocated + gc.young_allocated > gc.old_threshold) {
                    gc_major_collect();
                }
            }
        }
    }
}

// 跟踪非 GC 对象的内存变化（如动态数组扩容）
// 同时更新 obj->size，确保分代晋升时内存计数正确转移
void gc_track_memory(Object* obj, size_t old_size, size_t new_size) {
    if (new_size > old_size) {
        size_t diff = new_size - old_size;
        if (obj && obj->generation == GEN_OLD) {
            gc.old_allocated += diff;
        } else {
            gc.young_allocated += diff;
        }
        if (obj) obj->size += diff;
    } else if (old_size > new_size) {
        size_t diff = old_size - new_size;
        if (obj && obj->generation == GEN_OLD) {
            gc.old_allocated -= diff;
        } else {
            gc.young_allocated -= diff;
        }
        if (obj) obj->size -= diff;
    }
}

// ============================================================================
// 标记阶段（Mark Phase）
// ============================================================================

// 递归标记对象及其所有可达引用
void gc_mark_object(Object* obj) {
    if (!obj || obj->marked) return;

    // 安全检查：验证对象类型是否有效
    if (!is_valid_obj_type(obj->type)) {
        return;  // 无效的对象类型，跳过
    }

    obj->marked = 1;

    switch (obj->type) {
        // 闭包：标记函数和所有 upvalue
        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)obj;
            gc_mark_object((Object*)closure->function);
            for (int i = 0; i < closure->upvalue_count; i++) {
                gc_mark_object((Object*)closure->upvalues[i]);
            }
            break;
        }
        // 函数：标记常量池和所属模块
        case OBJ_FUNCTION: {
            ObjFunction* func = (ObjFunction*)obj;
            if (func->chunk) {
                for (int i = 0; i < func->chunk->const_cnt; i++) {
                    gc_mark_value(func->chunk->constants[i]);
                }
            }
            if (func->module) {
                gc_mark_object((Object*)func->module);
            }
            break;
        }
        // 原生函数：无子引用
        case OBJ_NATIVE:
            break;
        case OBJ_GUI_WINDOW:
            break;
        case OBJ_GUI_RENDERER: {
            extern void guis_mark_renderer_refs(Object* obj);
            guis_mark_renderer_refs(obj);
            break;
        }
        case OBJ_GUI_FONT:
            break;
        case OBJ_GUI_IMAGE:
            break;
        case OBJ_GUI_EVENT: {
            /* Event 对象内部持有 ObjDict*，需要标记 */
            ObjGUIEvent* ev = (ObjGUIEvent*)obj;
            if (ev->data) gc_mark_object((Object*)ev->data);
            break;
        }
        case OBJ_RGB:
            break;
        /* 数组：标记所有元素 */
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;
            for (int i = 0; i < arr->count; i++) {
                gc_mark_value(arr->elements[i]);
            }
            break;
        }
        // 字典：标记数组部分、哈希表部分和插入顺序数组中的键值
        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)obj;
            for (int i = 0; i < dict->asize; i++) {
                gc_mark_value(dict->array[i]);
            }
            for (int i = 0; i < dict->capacity; i++) {
                ObjString* key = dict->entries[i].key;
                if (key != NULL && key != tombstone) {
                    gc_mark_object((Object*)key);
                    gc_mark_value(dict->entries[i].value);
                }
            }
            for (int i = 0; i < dict->order_count; i++) {
                if (dict->order[i] != NULL) {
                    gc_mark_object((Object*)dict->order[i]);
                }
            }
            break;
        }
        // Upvalue：标记引用的值、关闭值和链表下一个节点
        case OBJ_UPVALUE: {
            Upvalue* upvalue = (Upvalue*)obj;
            gc_mark_value(*upvalue->location);
            gc_mark_value(upvalue->closed);
            if (upvalue->next) {
                gc_mark_object((Object*)upvalue->next);
            }
            break;
        }
        // 字符串：无子引用（字符数据由 free_object_resources 释放）
        case OBJ_STRING:
            break;
        // 大整数：无子引用（limbs 数组由 free_object_resources 释放）
        case OBJ_BIGINT:
            break;
        // 模块：标记导出字典和所有全局变量
        case OBJ_MODULE: {
            ObjModule* module = (ObjModule*)obj;
            if (module->exports) {
                gc_mark_object((Object*)module->exports);
            }
            for (int i = 0; i < module->global_count; i++) {
                gc_mark_value(module->globals[i]);
            }
            if (module->init_chunk) {
                for (int i = 0; i < module->init_chunk->const_cnt; i++) {
                    gc_mark_value(module->init_chunk->constants[i]);
                }
            }
            break;
        }
        // 绑定方法：标记接收者和方法
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod* bound = (ObjBoundMethod*)obj;
            gc_mark_value(bound->receiver);
            gc_mark_object((Object*)bound->method);
            break;
        }
        // 文件对象：标记路径和模式字符串
        case OBJ_FILE: {
            ObjFile* file = (ObjFile*)obj;
            gc_mark_object((Object*)file->path);
            gc_mark_object((Object*)file->mode);
            break;
        }
        // 结构体定义：标记字段默认值和方法（闭包或函数）
        case OBJ_STRUCT_DEF: {
            ObjStructDef* def = (ObjStructDef*)obj;
            for (int i = 0; i < def->field_count; i++) {
                if (def->fields[i].has_default) {
                    gc_mark_value(def->fields[i].default_value);
                }
            }
            for (int i = 0; i < def->method_count; i++) {
                if (def->methods[i].closure) {
                    gc_mark_object((Object*)def->methods[i].closure);
                } else if (def->methods[i].func) {
                    gc_mark_object((Object*)def->methods[i].func);
                }
            }
            break;
        }
        case OBJ_FACE_DEF:
            break;
        // 结构体实例：标记定义和所有字段值
        case OBJ_STRUCT: {
            ObjStruct* struct_obj = (ObjStruct*)obj;
            if (struct_obj->def) {
                gc_mark_object((Object*)struct_obj->def);
                for (int i = 0; i < struct_obj->def->field_count; i++) {
                    gc_mark_value(struct_obj->field_values[i]);
                }
            }
            break;
        }
        // 协程：标记闭包、结果、等待对象、链表下一个、保存的帧和初始参数
        case OBJ_COROUTINE: {
            ObjCoroutine* co = (ObjCoroutine*)obj;
            gc_mark_object((Object*)co->closure);
            gc_mark_value(co->result);
            gc_mark_object((Object*)co->waiting_for);
            gc_mark_object((Object*)co->task_future);
            if (co->next) {
                gc_mark_object((Object*)co->next);
            }
            // 标记保存的调用帧副本
            if (co->has_saved_frame && co->saved_frame_copy) {
                CallFrame* frame_copy = co->saved_frame_copy;
                gc_mark_object((Object*)frame_copy->closure);
                if (frame_copy->chunk) {
                    for (int j = 0; j < frame_copy->chunk->const_cnt; j++) {
                        gc_mark_value(frame_copy->chunk->constants[j]);
                    }
                }
                if (frame_copy->locals) {
                    for (int i = 0; i < frame_copy->local_count; i++) {
                        gc_mark_value(frame_copy->locals[i]);
                    }
                }
                if (frame_copy->has_try_return) {
                    gc_mark_value(frame_copy->try_return_value);
                }
                if (frame_copy->module) {
                    gc_mark_object((Object*)frame_copy->module);
                }
            }
            if (co->initial_args) {
                for (int i = 0; i < co->initial_arg_count; i++) {
                    gc_mark_value(co->initial_args[i]);
                }
            }
            break;
        }
        // Future：标记结果、错误和等待者
        case OBJ_FUTURE: {
            ObjFuture* future = (ObjFuture*)obj;
            gc_mark_value(future->result);
            gc_mark_value(future->error);
            gc_mark_object((Object*)future->waiter);
            break;
        }
        // 线程对象：标记返回值
        case OBJ_THREAD: {
            ObjThread* thread = (ObjThread*)obj;
            gc_mark_value(thread->result);
            break;
        }
        // 通道：标记缓冲区中的值
        case OBJ_CHANNEL: {
            ObjChannel* channel = (ObjChannel*)obj;
            if (channel->buffer && channel->count > 0) {
                for (int i = 0; i < channel->count; i++) {
                    int idx = (channel->head + i) % channel->capacity;
                    gc_mark_value(channel->buffer[idx]);
                }
            }
            break;
        }
        // C 结构体数组视图：标记所属 C 结构体
        case OBJ_CSTRUCT_ARRAY_VIEW: {
            ObjCStructArrayView* view = (ObjCStructArrayView*)obj;
            gc_mark_object((Object*)view->cstruct);
            break;
        }
        // C 结构体实例：标记结构体定义
        case OBJ_CSTRUCT: {
            ObjCStruct* cs = (ObjCStruct*)obj;
            gc_mark_object((Object*)cs->def);
            break;
        }
        // C 结构体数组：标记结构体定义
        case OBJ_CSTRUCT_ARRAY: {
            ObjCStructArray* csa = (ObjCStructArray*)obj;
            gc_mark_object((Object*)csa->def);
            break;
        }
        // 以下类型无 GC 子引用
        case OBJ_ENUM_DEF:
            break;
        case OBJ_CSTRUCT_DEF:
            break;
        case OBJ_RANGE:
            break;
        case OBJ_FFI_LIBRARY:
            break;
        case OBJ_FFI_POINTER:
            break;
        case OBJ_FFI_CALLBACK: {
            ObjFFICallback* cb = (ObjFFICallback*)obj;
            gc_mark_value(cb->func_val);
            break;
        }
        case OBJ_NONE:
        case OBJ_INT:
        case OBJ_FLOAT:
            break;
    }
}

// 标记 Value（如果是对象类型则递归标记）
void gc_mark_value(Value v) {
    if (val_is_obj(v)) {
        gc_mark_object(val_as_obj(v));
    }
}

// ============================================================================
// 根集合标记（Root Set Marking）
// ============================================================================

// 标记所有根集合：栈、帧、全局变量、upvalue 链、异常、协程、线程等
static void mark_roots(void) {
    if (!gc.vm) return;

    // 1. 标记操作数栈上的所有值
    // 安全检查：确保栈指针在有效范围内
    int sp = gc.vm->sp;
    if (sp < 0) sp = 0;
    if (sp > gc.vm->stack_capacity) sp = gc.vm->stack_capacity;
    for (int i = 0; i < sp; i++) {
        gc_mark_value(gc.vm->stack[i]);
    }

    // 2. 标记所有调用帧（闭包、常量池、局部变量、try 返回值、模块）
    for (int i = 0; i < gc.vm->frame_cnt; i++) {
        CallFrame* frame = &gc.vm->frames[i];
        if (frame->closure) {
            gc_mark_object((Object*)frame->closure);
        }
        if (frame->chunk) {
            for (int j = 0; j < frame->chunk->const_cnt; j++) {
                gc_mark_value(frame->chunk->constants[j]);
            }
        }
        if (frame->locals) {
            for (int j = 0; j < frame->local_count; j++) {
                gc_mark_value(frame->locals[j]);
            }
        }
        if (frame->has_try_return) {
            gc_mark_value(frame->try_return_value);
        }
        if (frame->module) {
            gc_mark_object((Object*)frame->module);
        }
    }

    // 3. 标记全局变量
    for (int i = 0; i < gc.vm->global_count; i++) {
        gc_mark_value(gc.vm->globals[i]);
    }

    // 3.5 标记当前模块帧及其全局变量
    ModuleFrame* module_frame = gc.vm->current_module_frame;
    while (module_frame) {
        if (module_frame->module) {
            gc_mark_object((Object*)module_frame->module);
        }
        if (module_frame->globals) {
            for (int i = 0; i < module_frame->global_count; i++) {
                gc_mark_value(module_frame->globals[i]);
            }
        }
        module_frame = module_frame->parent;
    }

    // 4. 标记全局函数
    for (int i = 0; i < gc.vm->global_func_count; i++) {
        gc_mark_value(gc.vm->global_funcs[i]);
    }

    // 5. 标记 open upvalue 链表（闭包捕获的栈上变量）
    Upvalue* upvalue = gc.vm->open_upvalues;
    while (upvalue) {
        gc_mark_object((Object*)upvalue);
        upvalue = upvalue->next;
    }

    // 6. 标记当前异常值（防止 GC 在异常处理期间回收异常对象）
    gc_mark_value(gc.vm->exception);

    // 7. 标记最后一次返回值
    gc_mark_value(gc.vm->last_return_value);

    // 8. 标记原生函数方法表
    native_mark_all_functions();

    // 9. 标记内置类型方法表
    extern void array_mark_methods(void);
    extern void string_mark_methods(void);
    extern void dict_mark_methods(void);
    extern void file_mark_methods(void);
    extern void draw_mark_methods(void);
    extern void window_mark_methods(void);
    extern void event_mark_methods(void);
    extern void image_mark_methods(void);
    extern void font_mark_methods(void);
    extern void number_mark_methods(void);
    extern void cstruct_mark_methods(void);
    extern void struct_mark_methods(void);
    array_mark_methods();
    string_mark_methods();
    dict_mark_methods();
    file_mark_methods();
    draw_mark_methods();
    window_mark_methods();
    event_mark_methods();
    image_mark_methods();
    font_mark_methods();
    number_mark_methods();
    thread_mark_methods();
    channel_mark_methods();
    cstruct_mark_methods();
    struct_mark_methods();

    // 10. 标记内化字符串表（仅 Minor GC 使用，Major GC 改用 intern_sweep_unmarked）

    // 11. 标记全局结构体定义表
    extern void struct_def_mark_all(void);
    struct_def_mark_all();

    // 12. 标记全局 face 定义表
    extern void face_def_mark_all(void);
    face_def_mark_all();

    // 13. 标记全局 C 结构体定义表
    extern void cstruct_def_mark_all(void);
    cstruct_def_mark_all();

    // 13.5 标记已加载模块缓存
    extern void loaded_modules_mark_all(void);
    loaded_modules_mark_all();

    // 14. 标记当前协程
    if (gc.vm->current_coroutine) {
        gc_mark_object((Object*)gc.vm->current_coroutine);
    }

    // 15. 标记所有协程链表（包括挂起等待的协程）
    ObjCoroutine* co = gc.vm->all_coroutines;
    while (co) {
        gc_mark_object((Object*)co);
        co = co->next;
    }

    // 15. 标记活动线程列表
    if (gc.vm->active_threads) {
        for (int i = 0; i < gc.vm->active_thread_count; i++) {
            if (gc.vm->active_threads[i]) {
                gc_mark_object((Object*)gc.vm->active_threads[i]);
            }
        }
    }

    // 16. 标记事件循环中的就绪队列和定时器协程
    if (gc.vm->event_loop) {
        for (int i = 0; i < gc.vm->event_loop->ready_count; i++) {
            gc_mark_object((Object*)gc.vm->event_loop->ready_queue[i]);
        }
        for (int i = 0; i < gc.vm->event_loop->timer_count; i++) {
            gc_mark_object((Object*)gc.vm->event_loop->timers[i].coroutine);
        }
    }

    // 17. 标记额外根集合（gc_push_root 注册的 Value 指针）
    for (int i = 0; i < gc.extra_root_count; i++) {
        gc_mark_value(*gc.extra_roots[i]);
    }

    // 18. 标记 GUI 模块的额外根（定时器回调等）
    extern void guis_mark_extra_roots(void);
    guis_mark_extra_roots();
}

// 标记 remembered set 中的老年代对象（Minor GC 时作为额外根集合）
static void mark_remembered_set(void) {
    for (int i = 0; i < gc.remembered_count; i++) {
        Object* obj = gc.remembered_set[i];
        // 安全检查：验证对象是否有效
        if (!obj || !is_valid_obj_type(obj->type)) continue;
        if (obj->generation == GEN_OLD && !obj->marked) {
            gc_mark_object(obj);
        }
    }
}

// ============================================================================
// 对象大小计算
// ============================================================================

// 计算对象的总内存占用（包含动态分配的子资源）
// 注：当前未使用，保留供调试和内存分析使用
__attribute__((unused))
static size_t get_object_size(Object* obj) {
    if (!obj) return sizeof(Object);
    switch (obj->type) {
        case OBJ_STRING: return sizeof(ObjString);
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;
            return sizeof(ObjArray) + (arr->capacity * sizeof(Value));
        }
        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)obj;
            return sizeof(ObjDict)
                 + dict->asize * sizeof(Value)
                 + dict->capacity * sizeof(ObjDictEntry)
                 + dict->order_capacity * sizeof(ObjString*);
        }
        case OBJ_CLOSURE: return sizeof(ObjClosure);
        case OBJ_FUNCTION: return sizeof(ObjFunction);
        case OBJ_NATIVE: return sizeof(ObjNative);
        case OBJ_GUI_WINDOW: return sizeof(Object) + sizeof(int) + sizeof(void*);
        case OBJ_GUI_RENDERER: return sizeof(Object) + sizeof(void*) + sizeof(void*);
        case OBJ_GUI_FONT: return sizeof(Object) + sizeof(void*);
        case OBJ_GUI_IMAGE: return sizeof(Object) + sizeof(void*);
        case OBJ_GUI_EVENT: return sizeof(Object) + sizeof(void*);
        case OBJ_BIGINT: {
            ObjBigInt* bigint = (ObjBigInt*)obj;
            return sizeof(ObjBigInt) + bigint->limb_count * sizeof(uint32_t);
        }
        case OBJ_MODULE: return sizeof(ObjModule);
        case OBJ_BOUND_METHOD: return sizeof(ObjBoundMethod);
        case OBJ_FILE: return sizeof(ObjFile);
        case OBJ_RANGE: return sizeof(ObjRange);
        case OBJ_UPVALUE: return sizeof(Upvalue);
        case OBJ_STRUCT_DEF: {
            ObjStructDef* def = (ObjStructDef*)obj;
            size_t size = sizeof(ObjStructDef);
            if (def->name) size += safe_strlen(def->name) + 1;
            if (def->fields) {
                size += def->field_count * sizeof(StructFieldInfo);
                for (int i = 0; i < def->field_count; i++) {
                    if (def->fields[i].name) size += safe_strlen(def->fields[i].name) + 1;
                    if (def->fields[i].struct_type_name) size += safe_strlen(def->fields[i].struct_type_name) + 1;
                }
            }
            if (def->methods) {
                size += def->method_count * sizeof(StructMethodInfo);
                for (int i = 0; i < def->method_count; i++) {
                    if (def->methods[i].name) size += safe_strlen(def->methods[i].name) + 1;
                }
            }
            return size;
        }
        case OBJ_FACE_DEF: {
            ObjFaceDef* def = (ObjFaceDef*)obj;
            size_t size = sizeof(ObjFaceDef);
            if (def->name) size += safe_strlen(def->name) + 1;
            if (def->methods) {
                size += def->method_count * sizeof(FaceMethodInfo);
                for (int i = 0; i < def->method_count; i++) {
                    if (def->methods[i].name) size += safe_strlen(def->methods[i].name) + 1;
                }
            }
            return size;
        }
        case OBJ_STRUCT: {
            ObjStruct* struct_obj = (ObjStruct*)obj;
            size_t size = sizeof(ObjStruct);
            if (struct_obj->def) {
                size += struct_obj->def->field_count * sizeof(Value);
            }
            return size;
        }
        case OBJ_COROUTINE: {
            ObjCoroutine* co = (ObjCoroutine*)obj;
            size_t size = sizeof(ObjCoroutine);
            if (co->saved_frame) {
                size += sizeof(CallFrame);
                if (co->saved_frame->locals_is_dynamic && co->saved_frame->locals) {
                    size += co->saved_frame->local_count * sizeof(Value);
                }
            }
            if (co->initial_args) {
                size += co->initial_arg_count * sizeof(Value);
            }
            return size;
        }
        case OBJ_FUTURE: {
            return sizeof(ObjFuture);
        }
        case OBJ_THREAD: {
            return sizeof(ObjThread);
        }
        case OBJ_CHANNEL: {
            ObjChannel* channel = (ObjChannel*)obj;
            return sizeof(ObjChannel) + channel->capacity * sizeof(Value);
        }
        case OBJ_CSTRUCT_DEF: {
            ObjCStructDef* def = (ObjCStructDef*)obj;
            size_t s = sizeof(ObjCStructDef);
            if (def->name) s += safe_strlen(def->name) + 1;
            if (def->fields) {
                s += def->field_count * sizeof(CStructFieldInfo);
                for (int i = 0; i < def->field_count; i++) {
                    if (def->fields[i].name) s += safe_strlen(def->fields[i].name) + 1;
                    if (def->fields[i].struct_name) s += safe_strlen(def->fields[i].struct_name) + 1;
                }
            }
            if (def->field_hash_table) {
                s += def->field_hash_capacity * sizeof(CStructFieldHashEntry*);
            }
            return s;
        }
        case OBJ_CSTRUCT: {
            ObjCStruct* cs = (ObjCStruct*)obj;
            size_t s = sizeof(ObjCStruct);
            if (cs->owns_memory && cs->data && cs->def) {
                s += cs->def->total_size;
            }
            return s;
        }
        case OBJ_CSTRUCT_ARRAY: {
            ObjCStructArray* csa = (ObjCStructArray*)obj;
            return sizeof(ObjCStructArray) + (size_t)csa->count * csa->element_size;
        }
        case OBJ_CSTRUCT_ARRAY_VIEW: return sizeof(ObjCStructArrayView);
        case OBJ_ENUM_DEF: {
            ObjEnumDef* edef = (ObjEnumDef*)obj;
            size_t s = sizeof(ObjEnumDef);
            if (edef->name) s += safe_strlen(edef->name) + 1;
            if (edef->members) {
                s += edef->member_count * sizeof(EnumMemberInfo);
                for (int i = 0; i < edef->member_count; i++) {
                    if (edef->members[i].name) s += safe_strlen(edef->members[i].name) + 1;
                }
            }
            return s;
        }
        case OBJ_FFI_LIBRARY: return sizeof(Object) + 256;
        case OBJ_FFI_POINTER: {
            extern size_t ffi_pointer_get_data_size(Object* obj);
            return sizeof(Object) + 64 + ffi_pointer_get_data_size(obj);
        }
        case OBJ_FFI_CALLBACK: return sizeof(ObjFFICallback);
        default: return sizeof(Object);
    }
}

// ============================================================================
// 对象资源释放
// ============================================================================

// 释放对象持有的动态资源（不释放对象本身）
static void free_object_resources(Object* obj) {
    if (!obj) return;

    switch (obj->type) {
        // 字符串：释放字符数组，并从字符串表中移除
        case OBJ_STRING: {
            ObjString* str = (ObjString*)obj;
            intern_remove(str);
            free(str->chars);
            break;
        }
        // 函数：释放名称和字节码块
        case OBJ_FUNCTION: {
            ObjFunction* func = (ObjFunction*)obj;
            free(func->name);
            if (func->chunk) {
                chunk_free(func->chunk);
                free(func->chunk);
            }
            break;
        }
        // 闭包：无额外资源
        case OBJ_CLOSURE:
            break;
        // 原生函数：释放名称
        case OBJ_NATIVE: {
            ObjNative* native = (ObjNative*)obj;
            free(native->name);
            break;
        }
        case OBJ_GUI_WINDOW: {
            ObjGUIWindow* w = (ObjGUIWindow*)obj;
            if (w->platform) {
                leno_gui_platform_destroy_window(w->platform);
                w->platform = NULL;
            }
            break;
        }
        case OBJ_GUI_RENDERER: {
            ObjGUIRenderer* r = (ObjGUIRenderer*)obj;
            if (r->platform) {
                leno_gui_platform_destroy_renderer(r->platform);
                r->platform = NULL;
            }
            break;
        }
        case OBJ_GUI_FONT: {
            ObjGUIFont* f = (ObjGUIFont*)obj;
            if (f->platform) {
                leno_gui_platform_destroy_font(f->platform);
                f->platform = NULL;
            }
            break;
        }
        case OBJ_GUI_IMAGE: {
            ObjGUIImage* img = (ObjGUIImage*)obj;
            if (img->platform) {
                leno_gui_platform_destroy_image(img->platform);
                img->platform = NULL;
            }
            break;
        }
        case OBJ_GUI_EVENT:
            break;
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)obj;
            type_free(arr->type_info);
            free(arr->elements);
            break;
        }
        // 字典：释放数组部分、哈希表和插入顺序数组
        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)obj;
            type_free(dict->type_info);
            free(dict->array);
            free(dict->entries);
            free(dict->order);
            break;
        }
        // 大整数：释放 limbs 数组
        case OBJ_BIGINT: {
            ObjBigInt* bigint = (ObjBigInt*)obj;
            free(bigint->limbs);
            break;
        }
        // 模块：释放名称和全局变量数组
        case OBJ_MODULE: {
            ObjModule* module = (ObjModule*)obj;
            free(module->name);
            free(module->source_path);
            free(module->globals);
            if (module->init_chunk) {
                chunk_free(module->init_chunk);
                free(module->init_chunk);
            }
            if (module->export_mappings) {
                for (int i = 0; i < module->export_mapping_count; i++) {
                    free(module->export_mappings[i].name);
                }
                free(module->export_mappings);
            }
            break;
        }
        // 绑定方法：无额外资源
        case OBJ_BOUND_METHOD:
            break;
        // 文件对象：关闭文件句柄
        case OBJ_FILE: {
            ObjFile* file = (ObjFile*)obj;
            if (file->fp && !file->is_closed) {
                fclose(file->fp);
            }
            break;
        }
        // 范围：无额外资源
        case OBJ_RANGE:
            break;
        // Upvalue：无额外资源
        case OBJ_UPVALUE:
            break;
        // 结构体定义：释放名称、字段信息和方法信息
        case OBJ_STRUCT_DEF: {
            ObjStructDef* def = (ObjStructDef*)obj;
            free(def->name);
            if (def->fields) {
                for (int i = 0; i < def->field_count; i++) {
                    free(def->fields[i].name);
                    free(def->fields[i].struct_type_name);
                }
                free(def->fields);
            }
            if (def->methods) {
                for (int i = 0; i < def->method_count; i++) {
                    free(def->methods[i].name);
                }
                free(def->methods);
            }
            break;
        }
        case OBJ_FACE_DEF: {
            ObjFaceDef* def = (ObjFaceDef*)obj;
            free(def->name);
            if (def->methods) {
                for (int i = 0; i < def->method_count; i++) {
                    free(def->methods[i].name);
                }
                free(def->methods);
            }
            break;
        }
        // 结构体实例：释放字段值数组
        case OBJ_STRUCT: {
            ObjStruct* struct_obj = (ObjStruct*)obj;
            free(struct_obj->field_values);
            break;
        }
        // 协程：释放保存的调用帧副本和初始参数
        case OBJ_COROUTINE: {
            ObjCoroutine* co = (ObjCoroutine*)obj;
            if (co->saved_frame) {
                if (co->saved_frame->locals_is_dynamic && co->saved_frame->locals) {
                    free(co->saved_frame->locals);
                }
                free(co->saved_frame);
            }
            if (co->initial_args) {
                free(co->initial_args);
            }
            break;
        }
        // Future：无额外资源
        case OBJ_FUTURE:
            break;
        // 线程对象：释放错误消息和同步原语
        case OBJ_THREAD: {
            ObjThread* thread = (ObjThread*)obj;
            if (thread->error_msg) {
                free(thread->error_msg);
            }
            platform_mutex_destroy(&thread->mutex);
            platform_cond_destroy(&thread->done_cond);
            break;
        }
        // 通道：释放缓冲区和同步原语
        case OBJ_CHANNEL: {
            ObjChannel* channel = (ObjChannel*)obj;
            if (channel->buffer) {
                free(channel->buffer);
            }
            platform_mutex_destroy(&channel->mutex);
            platform_cond_destroy(&channel->not_empty);
            platform_cond_destroy(&channel->not_full);
            break;
        }
        // C 结构体定义：释放名称、字段信息和哈希表
        case OBJ_CSTRUCT_DEF: {
            ObjCStructDef* def = (ObjCStructDef*)obj;
            free(def->name);
            if (def->fields) {
                for (int i = 0; i < def->field_count; i++) {
                    free(def->fields[i].name);
                    free(def->fields[i].struct_name);
                }
                free(def->fields);
            }
            if (def->field_hash_table) {
                for (int i = 0; i < def->field_hash_capacity; i++) {
                    CStructFieldHashEntry* entry = def->field_hash_table[i];
                    while (entry) {
                        CStructFieldHashEntry* next = entry->next;
                        free(entry->name);
                        free(entry);
                        entry = next;
                    }
                }
                free(def->field_hash_table);
            }
            break;
        }
        // C 结构体实例：释放拥有的内存
        case OBJ_CSTRUCT: {
            ObjCStruct* cs = (ObjCStruct*)obj;
            if (cs->owns_memory && cs->data) {
                free(cs->data);
            }
            break;
        }
        // C 结构体数组：释放数据缓冲区
        case OBJ_CSTRUCT_ARRAY: {
            ObjCStructArray* csa = (ObjCStructArray*)obj;
            free(csa->data);
            break;
        }
        // Enum 定义：释放名称和成员信息
        case OBJ_ENUM_DEF: {
            ObjEnumDef* edef = (ObjEnumDef*)obj;
            free(edef->name);
            if (edef->members) {
                for (int i = 0; i < edef->member_count; i++) {
                    free(edef->members[i].name);
                }
                free(edef->members);
            }
            break;
        }
        // FFI 库：释放路径和 OS 句柄（委托给 ffi.c）
        case OBJ_FFI_LIBRARY: {
            extern void ffi_library_free_resources(Object* obj);
            ffi_library_free_resources(obj);
            break;
        }
        // FFI 指针：释放拥有的内存（委托给 ffi.c）
        case OBJ_FFI_POINTER: {
            extern void ffi_pointer_free_resources(Object* obj);
            ffi_pointer_free_resources(obj);
            break;
        }
        case OBJ_FFI_CALLBACK: {
            extern void ffi_callback_free_resources(Object* obj);
            ffi_callback_free_resources(obj);
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// 清除阶段（Sweep Phase）
// ============================================================================

// 清除年轻代：回收未标记对象，晋升存活次数达标的对象到老年代
static void sweep_young(void) {
    Object** obj = &gc.young_heap;
    while (*obj) {
        // 安全检查：验证对象类型是否有效
        if (!is_valid_obj_type((*obj)->type)) {
            Object* invalid = *obj;
            *obj = invalid->next;
            gc.young_allocated -= invalid->size;
            free(invalid);
            continue;
        }
        if (!(*obj)->marked) {
            Object* unreached = *obj;
            *obj = unreached->next;
            gc.young_allocated -= unreached->size;
            free_object_resources(unreached);
            free(unreached);
        } else {
            (*obj)->survived++;
            if ((*obj)->survived >= gc.promote_age) {
                Object* promote = *obj;
                *obj = promote->next;
                // Major GC 时 sweep_old 紧接着执行，晋升对象必须保持 marked=1
                promote->marked = (gc.mode == GC_MODE_FULL) ? 1 : 0;
                promote->generation = GEN_OLD;
                promote->survived = 0;
                promote->next = gc.old_heap;
                gc.old_heap = promote;
                size_t obj_size = promote->size;
                gc.young_allocated -= obj_size;
                gc.old_allocated += obj_size;
            } else {
                (*obj)->marked = 0;
                obj = &(*obj)->next;
            }
        }
    }
}

// 清除老年代：回收未标记的对象
static void sweep_old(void) {
    Object** obj = &gc.old_heap;
    while (*obj) {
        if (!is_valid_obj_type((*obj)->type)) {
            Object* invalid = *obj;
            *obj = invalid->next;
            gc.old_allocated -= invalid->size;
            free(invalid);
            continue;
        }
        if (!(*obj)->marked) {
            Object* unreached = *obj;
            *obj = unreached->next;
            gc.old_allocated -= unreached->size;
            free_object_resources(unreached);
            free(unreached);
        } else {
            (*obj)->marked = 0;
            obj = &(*obj)->next;
        }
    }
}

// GC 后重建 remembered set（清除所有条目，由写屏障重新填充）
static void rebuild_remembered_set(void) {
    gc.remembered_count = 0;
}

// 清除老年代所有对象的 marked 标志（Minor GC 前调用）
// 确保老年代对象在 mark_roots 阶段能被重新扫描，
// 否则老年代对象保持 marked=1 会导致 gc_mark_object 跳过它们，
// 其引用的年轻代对象不会被标记，从而被误回收
static void clear_all_marks(void) {
    Object* obj = gc.young_heap;
    while (obj) {
        obj->marked = 0;
        obj = obj->next;
    }
    obj = gc.old_heap;
    while (obj) {
        obj->marked = 0;
        obj = obj->next;
    }
}

// ============================================================================
// GC 收集入口
// ============================================================================

// Minor GC：只收集年轻代
// 流程：标记根集合 → 标记 remembered set → 清除年轻代 → 清理未引用的内化字符串 → 重建 remembered set
void gc_minor_collect(void) {
    if (gc.running || !gc.enabled) return;

    gc.running = 1;
    gc.mode = GC_MODE_MINOR;
    gc.minor_gc_count++;

    clear_all_marks();
    mark_roots();
    mark_remembered_set();

    sweep_young();

    rebuild_remembered_set();

    gc.running = 0;

    // 动态调整年轻代阈值
    if (gc.young_threshold < gc.young_allocated * 2) {
        gc.young_threshold = gc.young_allocated * 2;
    }
    if (gc.young_threshold < GC_YOUNG_THRESHOLD) {
        gc.young_threshold = GC_YOUNG_THRESHOLD;
    }
}

// Major GC：收集全部（年轻代 + 老年代）
// 流程：标记根集合 → 标记 remembered set → 清除年轻代 → 清除老年代 → 重建 remembered set
void gc_major_collect(void) {
    if (gc.running || !gc.enabled) return;

    gc.running = 1;
    gc.mode = GC_MODE_FULL;

    clear_all_marks();
    mark_roots();
    mark_remembered_set();

    // Major GC：在 sweep 之前清理无引用的内化字符串（此时 marked 标志仍有效）
    intern_sweep_unmarked();

    sweep_young();
    sweep_old();

    rebuild_remembered_set();

    gc.running = 0;

    // 动态调整老年代阈值（上限 512MB）
    size_t total = gc.young_allocated + gc.old_allocated;
    size_t new_threshold = total * 2;
    if (new_threshold < GC_OLD_THRESHOLD) {
        new_threshold = GC_OLD_THRESHOLD;
    } else if (new_threshold > 1024 * 1024 * 512) {
        new_threshold = 1024 * 1024 * 512;
    }
    gc.old_threshold = new_threshold;

    // 动态调整年轻代阈值
    if (gc.young_threshold < gc.young_allocated * 2) {
        gc.young_threshold = gc.young_allocated * 2;
    }
    if (gc.young_threshold < GC_YOUNG_THRESHOLD) {
        gc.young_threshold = GC_YOUNG_THRESHOLD;
    }
}

// 自动选择 Minor 或 Major GC
void gc_collect(void) {
    if (gc.running || !gc.enabled) return;

    // 手动调用 gc_collect 时执行 Major GC，确保完整回收
    gc_major_collect();
}

// ============================================================================
// 程序退出时释放所有资源
// ============================================================================

// 释放所有 GC 对象和 VM 资源（程序退出时调用）
void gc_free_all(void) {
    // 等待所有活动线程完成
    if (gc.vm && gc.vm->active_threads) {
        for (int i = 0; i < gc.vm->active_thread_count; i++) {
            ObjThread* thread = gc.vm->active_threads[i];
            if (thread) {
                platform_mutex_lock(&thread->mutex);
                while (thread->state == THREAD_RUNNING) {
                    platform_cond_wait(&thread->done_cond, &thread->mutex);
                }
                platform_mutex_unlock(&thread->mutex);
                platform_thread_join(thread->os_thread, NULL);
            }
        }
        free(gc.vm->active_threads);
        gc.vm->active_threads = NULL;
        gc.vm->active_thread_count = 0;
        gc.vm->active_thread_capacity = 0;
    }

    // 释放全局作用域
    if (gc.vm && gc.vm->global_scope) {
        scope_free(gc.vm->global_scope);
        gc.vm->global_scope = NULL;
    }

    // 释放 VM 栈和帧
    if (gc.vm) {
        if (gc.vm->stack) {
            free(gc.vm->stack);
            gc.vm->stack = NULL;
        }
        gc.vm->stack_capacity = 0;
        if (gc.vm->frames) {
            free(gc.vm->frames);
            gc.vm->frames = NULL;
        }
        gc.vm->frame_capacity = 0;
    }

    // 释放年轻代所有对象
    Object* obj = gc.young_heap;
    while (obj) {
        Object* next = obj->next;
        free_object_resources(obj);
        free(obj);
        obj = next;
    }

    // 释放老年代所有对象
    obj = gc.old_heap;
    while (obj) {
        Object* next = obj->next;
        free_object_resources(obj);
        free(obj);
        obj = next;
    }

    // 释放内化字符串表
    intern_table_free();

    // 释放 remembered set
    free(gc.remembered_set);

    // 释放额外根集合
    free(gc.extra_roots);

    // 重置 GC 状态
    gc.young_heap = NULL;
    gc.old_heap = NULL;
    gc.young_allocated = 0;
    gc.old_allocated = 0;
    gc.young_threshold = GC_YOUNG_THRESHOLD;
    gc.old_threshold = GC_OLD_THRESHOLD;
    gc.running = 0;
    gc.mode = GC_MODE_FULL;
    gc.vm = NULL;
    gc.enabled = 1;
    gc.remembered_set = NULL;
    gc.remembered_count = 0;
    gc.remembered_capacity = 0;
    gc.promote_age = GC_PROMOTE_AGE;
    gc.minor_gc_count = 0;
    gc.extra_roots = NULL;
    gc.extra_root_count = 0;
    gc.extra_root_capacity = 0;
}
