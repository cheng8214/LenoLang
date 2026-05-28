#include "include/lenolang.h"

// ============================================================================
// 绑定方法支持
// ============================================================================

ObjBoundMethod* bound_method_new(Value receiver, ObjNative* method) {
    ObjBoundMethod* bound = (ObjBoundMethod*)gc_alloc(sizeof(ObjBoundMethod), OBJ_BOUND_METHOD);
    if (!bound) return NULL;
    
    bound->receiver = receiver;
    bound->method = method;
    return bound;
}
