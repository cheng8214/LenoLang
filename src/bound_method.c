#include "include/lenolang.h"

// ============================================================================
// 绑定方法支持
// ============================================================================

ObjBoundMethod* bound_method_new(Value receiver, ObjNative* method) {
    ObjBoundMethod* bound = (ObjBoundMethod*)gc_alloc(sizeof(ObjBoundMethod), OBJ_BOUND_METHOD);
    if (!bound) return NULL;
    
    bound->receiver = receiver;
    bound->method = method;
    bound->closure = NULL;
    return bound;
}

// 创建绑定闭包方法（将 receiver 和闭包方法绑定在一起）
ObjBoundMethod* bound_closure_method_new(Value receiver, ObjClosure* closure) {
    ObjBoundMethod* bound = (ObjBoundMethod*)gc_alloc(sizeof(ObjBoundMethod), OBJ_BOUND_METHOD);
    if (!bound) return NULL;
    
    bound->receiver = receiver;
    bound->method = NULL;
    bound->closure = closure;
    return bound;
}
