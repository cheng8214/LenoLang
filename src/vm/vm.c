#include "../include/leno_vm_runtime.h"
#include "../include/string_table.h"
#include "../include/native.h"
#include "../module/ffi/ffi_clib.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

// ============================================================================
// 跳转表优化配置（Computed Goto）
// 1. GCC/Clang：自动启用跳转表（VM_USE_JUMPTABLE = 1）
// 2. MSVC：自动回退到 switch 模式（VM_USE_JUMPTABLE = 0）
// 3. 强制切换：编译时添加 -DVM_FORCE_SWITCH 或 -DVM_FORCE_JUMPTABLE
//
#if defined(__GNUC__) || defined(__clang__)
    #define VM_USE_JUMPTABLE 1
    #define unlikely(x) __builtin_expect(!!(x), 0)
    #define likely(x)   __builtin_expect(!!(x), 1)
#else
    #define VM_USE_JUMPTABLE 0
    #define unlikely(x) (x)
    #define likely(x)   (x)
#endif

// 手动覆盖（用于测试）
#if defined(VM_FORCE_JUMPTABLE)
    #undef VM_USE_JUMPTABLE
    #define VM_USE_JUMPTABLE 1
#endif
#if defined(VM_FORCE_SWITCH)
    #undef VM_USE_JUMPTABLE
    #define VM_USE_JUMPTABLE 0
#endif

// 主线程使用全局 VM 以获得最佳性能
VM vm = {0};
int vm_initialized = 0;

// 主程序 VM 全局变量（效率第一）
// 子线程使用独立的局部 VM，不共享此全局变量


// ============================================================================
// INC/DEC 操作码公共辅助函数
// ============================================================================

// 执行增减计算（delta: +1 表示 INC, -1 表示 DEC）
// 返回新的 Value，保持原操作数类型
static inline Value do_inc_dec(Value val, int delta, int* error) {
    *error = 0;

    if (!val_is_num(val)) {
        *error = 1;
        return val_null();
    }

    // 如果值是 BigInt，直接使用 BigInt 运算
    if (val_is_bigint(val)) {
        ObjBigInt* big_val = val_as_bigint(val);
        ObjBigInt* big_delta = bigint_from_int64(delta);
        Value result = bigint_add(big_val, big_delta);
        return result;
    }

    // 获取旧值
    int64_t old_int = (int64_t)val_as_num(val);

    // 计算新值
    int64_t new_int = old_int + delta;

    // 检查 int64 溢出
    if ((delta > 0 && old_int > INT64_MAX - delta) ||
        (delta < 0 && old_int < INT64_MIN - delta)) {
        // int64 溢出，使用 BigInt
        ObjBigInt* big_val = bigint_from_int64(old_int);
        ObjBigInt* big_delta = bigint_from_int64(delta);
        return bigint_add(big_val, big_delta);
    }

    // 检查是否超出 int32 范围
    if (new_int < INT32_MIN || new_int > INT32_MAX) {
        return val_bigint_from_int64(new_int);
    }

    // 保持操作数的类型
    if (val_is_float(val)) {
        return val_float((double)new_int);
    } else if (val_is_bigint(val)) {
        // 原来是 BigInt，保持为 BigInt（但值已经在 int32 范围内）
        return val_bigint_from_int64(new_int);
    } else {
        return val_int((int)new_int);
    }
}

// 注意：bigint_to_double 和 value_to_double 现在定义在 leno_value.h 中作为 static inline
// 这样所有模块都可以内联使用这些函数，避免函数调用开销

// 将 Value 转换为数组/字符串索引（int）
// 返回 1 表示成功，0 表示失败（非数字类型）
static inline int value_to_index(Value val, int* index) {
    if (val_is_int(val)) {
        *index = val_as_int(val);
        return 1;
    } else if (val_is_float(val)) {
        *index = (int)val_as_num(val);
        return 1;
    } else if (val_is_bigint(val)) {
        // 使用 value_to_double 处理超出 int64 范围的 BigInt
        *index = (int)value_to_double(val);
        return 1;
    }
    return 0;
}

// ============================================================================
// 类型特化指令 BigInt 运算通用辅助函数
// ============================================================================

// 检查两个操作数是否需要使用 BigInt 运算（当任一操作数超出 int64 范围时）
static inline int need_bigint_arith(Value va, Value vb) {
    if (val_is_bigint(va) && !bigint_fits_in_int64(val_as_bigint(va))) return 1;
    if (val_is_bigint(vb) && !bigint_fits_in_int64(val_as_bigint(vb))) return 1;
    return 0;
}

// 获取操作数的 BigInt 指针（如果是 int 则创建临时 BigInt）
static inline ObjBigInt* get_bigint_operand(Value v) {
    if (val_is_bigint(v)) {
        return val_as_bigint(v);
    }
    return bigint_from_int64(val_as_int(v));
}

// 根据 int64 结果推送 Value（自动选择 int 或 BigInt）
// 注意：此函数不处理 int64 溢出，调用前需要确保不会溢出
// 定义在 vminc/vm_helpers.inc 中，以便在 vm_run_with_vm 的 #define vm (*vm_ptr) 上下文中正确展开

// 比较两个 Value（支持 BigInt 超出 int64 范围的情况）
// 返回值: -1(a<b), 0(a==b), 1(a>b)
static inline int compare_values(Value va, Value vb) {
    // 如果都不是超大 BigInt，使用 int64 比较
    if (bigint_fits_in_int64(get_bigint_operand(va)) && 
        bigint_fits_in_int64(get_bigint_operand(vb))) {
        int64_t a = val_is_bigint(va) ? bigint_to_int64(val_as_bigint(va)) : val_as_int(va);
        int64_t b = val_is_bigint(vb) ? bigint_to_int64(val_as_bigint(vb)) : val_as_int(vb);
        return (a < b) ? -1 : (a > b) ? 1 : 0;
    }
    // 使用 BigInt 比较
    return bigint_compare(get_bigint_operand(va), get_bigint_operand(vb));
}

// 比较 Value 和 int64 立即数
static inline int compare_value_imm(Value va, int64_t imm) {
    if (val_is_bigint(va) && !bigint_fits_in_int64(val_as_bigint(va))) {
        // 超大 BigInt 与立即数比较
        ObjBigInt* bigint_imm = bigint_from_int64(imm);
        int result = bigint_compare(val_as_bigint(va), bigint_imm);
        return result;
    }
    int64_t a = val_is_bigint(va) ? bigint_to_int64(val_as_bigint(va)) : val_as_int(va);
    return (a < imm) ? -1 : (a > imm) ? 1 : 0;
}

// ============================================================================
// 辅助函数实现 - 从各 .c 文件合并（需要在 OPCODE 之前定义）
// ============================================================================
// 注意：按依赖顺序包含，被依赖的在前
// 使用 #define vm (*current_exec_vm) 确保子线程使用正确的 VM

THREAD_LOCAL VM* current_exec_vm = NULL;

// 异常处理（最基础，被其他函数依赖）
#define vm (*current_exec_vm)
#include "vminc/vm_exception.inc"
#undef vm

// 基础工具函数（依赖 exception）
#define vm (*current_exec_vm)
#include "vminc/vm_utils.inc"
#undef vm

// 字节码块操作（独立，不使用 vm）
#include "vminc/vm_chunk.inc"

// 内联缓存（独立）
#define vm (*current_exec_vm)
#include "vminc/vm_ic.inc"
#undef vm

// Upvalue 管理（独立）
#define vm (*current_exec_vm)
#include "vminc/vm_upvalue.inc"
#undef vm

// 函数调用（依赖 exception）
#define vm (*current_exec_vm)
#include "vminc/vm_call.inc"
#undef vm

int vm_run(void) {
    // 主线程直接使用全局 vm，零开销
    current_exec_vm = &vm;

    // vm_run.inc 中直接使用 vm 变量（全局）
    #include "vminc/vm_run.inc"
}

// 使用指定的 VM 执行字节码（供子线程使用）
// 注意：vm_run.inc 中的代码使用 vm.xxx 访问 VM 成员，
// 并且所有栈操作函数都使用 &vm 作为第一个参数
// 所以我们需要将全局 vm 变量替换为 vm_ptr 指向的 VM
int vm_run_with_vm(VM* vm_ptr) {
    if (!vm_ptr) return -1;

    current_exec_vm = vm_ptr;

    // 使用宏将 vm_run.inc 中的 vm 替换为 (*vm_ptr)
    // 这样 vm.xxx 会变成 (*vm_ptr).xxx，即 vm_ptr->xxx
    // 而 &vm 会变成 &(*vm_ptr)，即 vm_ptr
    #define vm (*vm_ptr)
    
    // 执行字节码
    int result;
    #include "vminc/vm_run.inc"
    #undef vm
    
    return result;
}

int vm_call_value(Value callee, int arg_count, int line) {
    VM* vm_ptr = current_exec_vm ? current_exec_vm : &vm;
    int saved_frame_cnt = vm_ptr->frame_cnt;

    current_exec_vm = vm_ptr;
    #define vm (*current_exec_vm)
    if (!call_value(callee, arg_count, line)) {
        #undef vm
        return 0;
    }
    vm.stop_frame_cnt = saved_frame_cnt;
    #undef vm

    int r = vm_run_with_vm(vm_ptr);

    vm_ptr->stop_frame_cnt = 0;
    return (r == 0) ? 1 : 0;
}

int vm_run_coroutine_with_vm(ObjCoroutine* co, VM* vm_ptr) {
    // 直接使用传入的 VM 指针，不再使用宏
    if (!vm_ptr) {
        fprintf(stderr, "[ERROR] vm_ptr is NULL!\n");
        fflush(stderr);
        return -1;
    }

    // 设置当前执行 VM，确保全局变量访问正确
    extern THREAD_LOCAL VM* current_exec_vm;
    VM* saved_vm = current_exec_vm;
    current_exec_vm = vm_ptr;

    // 保存当前的 current_coroutine，防止嵌套调用时覆盖
    ObjCoroutine* saved_current = vm_ptr->current_coroutine;
    
    // 检查协程状态
    if (co->state == COROUTINE_COMPLETED || co->state == COROUTINE_FAILED) {
        vm_ptr->current_coroutine = saved_current;
        return 0;
    }
    
    // 如果协程是NEW状态，需要初始化执行环境
    if (co->state == COROUTINE_NEW) {
        co->state = COROUTINE_RUNNING;

        // 确保 current_coroutine 被设置
        vm_ptr->current_coroutine = co;

        // 创建初始调用帧
        if (vm_ptr->frame_cnt >= vm_ptr->frame_capacity) {
            if (!vm_grow_frames(vm_ptr)) {
                vm_ptr->current_coroutine = saved_current;
                return -1;
            }
        }
        
        CallFrame* frame = &vm_ptr->frames[vm_ptr->frame_cnt++];
        ObjFunction* func = co->closure->function;
        int local_count = func->local_count;
        
        // 分配locals数组
        if (local_count <= INLINE_LOCALS_MAX) {
            frame->locals = frame->inline_locals;
            frame->locals_is_dynamic = 0;
        } else {
            frame->locals = (Value*)malloc(local_count * sizeof(Value));
            frame->locals_is_dynamic = 1;
        }
        
        // 初始化所有 locals 为 null
        for (int i = 0; i < local_count; i++) {
            frame->locals[i] = val_null();
        }
        
        // 设置初始参数到 locals（参数对应前几个 locals）
        if (co->initial_args && co->initial_arg_count > 0) {
            int param_count = func->arity;  // 函数定义的参数数量
            int args_to_copy = (co->initial_arg_count < param_count) ? co->initial_arg_count : param_count;
            for (int i = 0; i < args_to_copy; i++) {
                frame->locals[i] = co->initial_args[i];
            }
        }
        
        frame->closure = co->closure;
        frame->chunk = func->chunk;
        frame->ip = func->chunk->code;
        frame->stack_base = vm_ptr->sp;
        frame->slot_count = local_count;
        frame->local_count = local_count;
        frame->catch_ip = NULL;
        frame->finally_ip = NULL;
        frame->prev_catch_ip = NULL;
        frame->prev_finally_ip = NULL;
        frame->in_finally = 0;
        frame->try_return_value = val_null();
        frame->has_try_return = 0;
        frame->has_captures = (co->closure->upvalue_count > 0) ? 1 : 0;
        
        // 保存创建此协程 frame 之前的 frame_cnt（其他协程/主程序的）
        int saved_frame_cnt = vm_ptr->frame_cnt - 1;  // 减去新创建的 frame
        // 设置协程的初始 frame_cnt（新创建的 frame 属于此协程）
        co->saved_frame_cnt = vm_ptr->frame_cnt;
        
        // 执行协程直到完成或挂起
        // 使用传入的 VM 执行（子线程使用自己的 VM）
        int result = vm_run_with_vm(vm_ptr);
        
        // 恢复之前的 frame_cnt（移除本协程添加的 frame）
        if (co->state == COROUTINE_SUSPENDED) {
            // 挂起时 saved_frames 拥有动态 locals，不释放 vm.frames 中的
            vm_ptr->frame_cnt = saved_frame_cnt;
        } else {
            // 完成或失败时，清理残留帧的动态 locals
            for (int i = saved_frame_cnt; i < vm_ptr->frame_cnt; i++) {
                CallFrame* f = &vm_ptr->frames[i];
                if (f->locals_is_dynamic && f->locals) {
                    free(f->locals);
                    f->locals = NULL;
                }
            }
            vm_ptr->frame_cnt = saved_frame_cnt;
        }
        
        // 检查协程是否完成
        // 如果 result == 0 且状态不是 SUSPENDED（即没有被再次挂起），则协程已完成
        if (result == 0 && co->state != COROUTINE_SUSPENDED) {
            co->state = COROUTINE_COMPLETED;
            // 完成 task_future（这是 task 返回给调用者的 Future）
            if (co->task_future && !co->task_future->completed) {
                future_complete(co->task_future, co->result);
            }
            // 释放初始参数内存
            if (co->initial_args) {
                free(co->initial_args);
                co->initial_args = NULL;
                co->initial_arg_count = 0;
            }
        } else if (vm_ptr->has_exception) {
            co->state = COROUTINE_FAILED;
            co->result = vm_ptr->exception;
            if (co->task_future && !co->task_future->completed) {
                // 如果有等待者，标记错误已传播
                if (co->task_future->waiter) {
                    co->error_propagated = 1;
                }
                future_fail(co->task_future, vm_ptr->exception);
            }
            // 释放初始参数内存
            if (co->initial_args) {
                free(co->initial_args);
                co->initial_args = NULL;
                co->initial_arg_count = 0;
            }
        }
        
        vm_ptr->current_coroutine = saved_current;
        return result;
    }
    
    // 如果协程是SUSPENDED或RUNNING状态，恢复执行
    // 注意：被Future完成的协程会被设为RUNNING状态，然后被加入就绪队列
    if (co->state == COROUTINE_SUSPENDED || co->state == COROUTINE_RUNNING) {
        co->state = COROUTINE_RUNNING;
        
        // 确保 current_coroutine 被设置
        vm_ptr->current_coroutine = co;
        
        // 保存当前的 frame_cnt（可能属于其他协程或主程序）
        int saved_frame_cnt = vm_ptr->frame_cnt;
        
        // 恢复所有保存的 frame 到 vm.frames
        if (co->saved_frames && co->saved_frame_count > 0) {
            // 确保有足够的空间
            while (vm_ptr->frame_cnt + co->saved_frame_count >= vm_ptr->frame_capacity) {
                if (!vm_grow_frames(vm_ptr)) {
                    vm_ptr->current_coroutine = saved_current;
                    return -1;
                }
            }
            // 将所有保存的 frame 拷贝到 vm.frames
            for (int i = 0; i < co->saved_frame_count; i++) {
                CallFrame* dst = &vm_ptr->frames[vm_ptr->frame_cnt + i];
                CallFrame* src = &co->saved_frames[i];
                memcpy(dst, src, sizeof(CallFrame));
                // inline_locals 帧的 locals 指针需重定向到 vm.frames 中的 inline_locals
                if (!dst->locals_is_dynamic && dst->local_count > 0) {
                    dst->locals = dst->inline_locals;
                }
                // 动态分配的 locals 指针直接拷贝即可，所有权转回 vm.frames
            }
            vm_ptr->frame_cnt += co->saved_frame_count;
            // 释放 saved_frames 数组本身（不释放动态 locals，所有权已转移到 vm.frames）
            free(co->saved_frames);
            co->saved_frames = NULL;
            co->saved_frame_count = 0;
        }
        
        // 更新协程的 saved_frame_cnt，使 OP_RETURN 能正确判断协程顶层函数返回
        // 恢复后的 frame_cnt 就是此协程当前拥有的帧数量上界
        co->saved_frame_cnt = vm_ptr->frame_cnt;
        
        // 恢复栈指针到挂起时的位置，确保压入 Future 结果的位置正确
        vm_ptr->sp = co->saved_sp;
        
        // 如果协程正在等待一个已完成的 Future，推送结果到栈或抛出异常
        if (co->waiting_for && co->waiting_for->completed) {
            // 检查 Future 是否有错误
            if (!val_is_null(co->waiting_for->error)) {
                // Future 失败了，抛出异常
                vm_ptr->exception = co->waiting_for->error;
                vm_ptr->has_exception = 1;
                // 查找 catch/finally 并跳转
                for (int i = vm_ptr->frame_cnt - 1; i >= 0; i--) {
                    CallFrame* frame = &vm_ptr->frames[i];
                    if (frame->closure && frame->closure->function && !frame->closure->function->has_try) {
                        continue;
                    }
                    if (frame->catch_ip || frame->finally_ip) {
                        // 展开调用栈
                        for (int j = vm_ptr->frame_cnt - 1; j > i; j--) {
                            CallFrame* unwind_frame = &vm_ptr->frames[j];
                            if (unwind_frame->locals && unwind_frame->locals_is_dynamic) {
                                free(unwind_frame->locals);
                                unwind_frame->locals = NULL;
                            }
                        }
                        vm_ptr->frame_cnt = i + 1;
                        vm_ptr->sp = frame->stack_base;
                        if (frame->catch_ip) {
                            frame->ip = frame->catch_ip;
                            // 手动压入异常到栈
                            if (vm_ptr->sp >= vm_ptr->stack_capacity) {
                                int new_capacity = vm_ptr->stack_capacity < 8 ? 8 : vm_ptr->stack_capacity * 2;
                                Value* new_stack = (Value*)realloc(vm_ptr->stack, new_capacity * sizeof(Value));
                                if (new_stack) {
                                    vm_ptr->stack = new_stack;
                                    vm_ptr->stack_capacity = new_capacity;
                                }
                            }
                            vm_ptr->stack[vm_ptr->sp++] = vm_ptr->exception;
                        } else {
                            vm_ptr->pending_exception = 1;
                            frame->ip = frame->finally_ip;
                        }
                        co->waiting_for = NULL;
                        // 异常已处理，清除异常状态
                        vm_ptr->has_exception = 0;
                        goto resume_done;
                    }
                }
                // 没有找到异常处理
                if (co->saved_frames && co->saved_frame_count > 0) {
                    error_add_at(ERR_RUNTIME, co->saved_frames[0].chunk->lines[0], 0, "未捕获的异步异常");
                } else {
                    error_add_at(ERR_RUNTIME, 0, 0, "未捕获的异步异常");
                }
            } else {
                // 手动压入结果到栈（sp 已恢复到挂起时的位置）
                if (vm_ptr->sp >= vm_ptr->stack_capacity) {
                    int new_capacity = vm_ptr->stack_capacity < 8 ? 8 : vm_ptr->stack_capacity * 2;
                    Value* new_stack = (Value*)realloc(vm_ptr->stack, new_capacity * sizeof(Value));
                    if (new_stack) {
                        vm_ptr->stack = new_stack;
                        vm_ptr->stack_capacity = new_capacity;
                    }
                }
                vm_ptr->stack[vm_ptr->sp++] = co->waiting_for->result;
            }
            co->waiting_for = NULL;
        }
        resume_done:;
        
        // 执行协程直到完成或再次挂起
        int result = vm_run_with_vm(vm_ptr);
        
        // 如果协程再次挂起，OP_AWAIT 已经保存了所有 frame
        // 如果协程完成，需要清理 saved_frames（如果有的话）
        // OP_AWAIT 会释放旧的 saved_frames 并重新分配，所以这里不需要处理
        
        // 恢复之前的 frame_cnt（移除本协程添加的 frame）
        // 注意：
        // - 协程挂起时: OP_AWAIT 已将帧保存到 saved_frames，动态 locals 所有权也转移了
        //   此时 vm.frames 中协程帧的 locals 指针不应被释放（所有权已转给 saved_frames）
        //   所以只需重置 frame_cnt，不释放 locals
        // - 协程完成时: OP_RETURN 已逐帧释放动态 locals 并减少 frame_cnt
        //   正常情况下 frame_cnt 已等于 saved_frame_cnt，无需清理
        // - 异常完成时: 可能有部分帧未正常 OP_RETURN，需清理这些帧的动态 locals
        //   但此时 saved_frames 应为空（恢复时已转出所有权）
        if (co->state == COROUTINE_SUSPENDED) {
            // 挂起时 saved_frames 拥有动态 locals，不释放 vm.frames 中的
            vm_ptr->frame_cnt = saved_frame_cnt;
        } else {
            // 完成或失败时，清理残留帧的动态 locals
            for (int i = saved_frame_cnt; i < vm_ptr->frame_cnt; i++) {
                CallFrame* f = &vm_ptr->frames[i];
                if (f->locals_is_dynamic && f->locals) {
                    free(f->locals);
                    f->locals = NULL;
                }
            }
            vm_ptr->frame_cnt = saved_frame_cnt;
        }
        
        // 检查协程是否完成
        // 如果 result == 0 且状态不是 SUSPENDED（即没有被再次挂起），则协程已完成
        if (result == 0 && co->state != COROUTINE_SUSPENDED) {
            co->state = COROUTINE_COMPLETED;
            // 完成 task_future（这是 task 返回给调用者的 Future）
            if (co->task_future && !co->task_future->completed) {
                future_complete(co->task_future, co->result);
            }
            // 释放初始参数内存
            if (co->initial_args) {
                free(co->initial_args);
                co->initial_args = NULL;
                co->initial_arg_count = 0;
            }
        } else if (vm_ptr->has_exception) {
            co->state = COROUTINE_FAILED;
            co->result = vm_ptr->exception;
            if (co->task_future && !co->task_future->completed) {
                // 如果有等待者，标记错误已传播
                if (co->task_future->waiter) {
                    co->error_propagated = 1;
                }
                future_fail(co->task_future, vm_ptr->exception);
            }
            // 释放初始参数内存
            if (co->initial_args) {
                free(co->initial_args);
                co->initial_args = NULL;
                co->initial_arg_count = 0;
            }
        }
        
        vm_ptr->current_coroutine = saved_current;
        current_exec_vm = saved_vm;  // 恢复之前的 VM
        return result;
    }
    
    vm_ptr->current_coroutine = saved_current;
    current_exec_vm = saved_vm;  // 恢复之前的 VM
    return 0;
}

// 包装函数：主线程使用全局 vm
int vm_run_coroutine(ObjCoroutine* co) {
    return vm_run_coroutine_with_vm(co, &vm);
}

// 获取当前协程
ObjCoroutine* vm_current_coroutine(void) {
    VM* target_vm = current_exec_vm ? current_exec_vm : &vm;
    return target_vm->current_coroutine;
}

int vm_in_async_context(void) {
    VM* target_vm = current_exec_vm ? current_exec_vm : &vm;
    return target_vm->current_coroutine != NULL;
}

// VM 初始化和栈操作（放在最后，因为它依赖前面的函数）
#include "vminc/vm_init.inc"