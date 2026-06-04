// ============================================================================
// 内联缓存 (Inline Caching) 辅助函数 - 从 vm_ic.c 迁移
// ============================================================================

// 计算缓存索引：使用指令地址的哈希值
static inline int ic_cache_index(uint8_t* ip) {
    // 使用指针值的低 12 位作为索引（假设 IC_CACHE_SIZE = 4096）
    return ((uintptr_t)ip >> 2) & (IC_CACHE_SIZE - 1);
}

// 获取 receiver 的对象类型
static inline ObjType get_receiver_type(Value receiver) {
    if (val_is_int(receiver)) return OBJ_INT;
    if (val_is_float(receiver)) return OBJ_FLOAT;
    if (val_is_obj(receiver)) return val_as_obj(receiver)->type;
    return OBJ_NONE;
}

// 根据 receiver 类型和方法名查找方法
static inline ObjNative* find_method_by_type(ObjType type, const char* name) {
    (void)name;
    switch (type) {
        case OBJ_ARRAY:   return array_find_method(name);
        case OBJ_STRING:  return string_find_method(name);
        case OBJ_DICT:    return dict_find_method(name);
        case OBJ_FILE:    return file_find_method(name);
        case OBJ_GUI_RENDERER: return draw_find_method(name);
        case OBJ_GUI_WINDOW: return window_find_method(name);
        case OBJ_GUI_EVENT: return event_find_method(name);
        case OBJ_GUI_IMAGE: return image_find_method(name);
        case OBJ_GUI_FONT: return font_find_method(name);
        case OBJ_STRUCT:  return struct_find_method(name);
        case OBJ_CSTRUCT_DEF:
        case OBJ_CSTRUCT: return cstruct_find_method(name);
        case OBJ_THREAD:  return thread_find_method(name);
        case OBJ_CHANNEL: return channel_find_method(name);
        case OBJ_INT:
        case OBJ_FLOAT:
        case OBJ_BIGINT:  return number_find_method(name);
        default:          return NULL;
    }
}

// 检查类型是否支持内联缓存
static inline int ic_type_supported(ObjType type) {
    return type == OBJ_ARRAY || type == OBJ_STRING ||
           type == OBJ_DICT || type == OBJ_FILE ||
           type == OBJ_INT || type == OBJ_FLOAT ||
           type == OBJ_BIGINT ||
           type == OBJ_STRUCT ||
           type == OBJ_CSTRUCT_DEF || type == OBJ_CSTRUCT ||
           type == OBJ_THREAD || type == OBJ_CHANNEL ||
           type == OBJ_GUI_WINDOW || type == OBJ_GUI_RENDERER || type == OBJ_GUI_EVENT || type == OBJ_GUI_IMAGE || type == OBJ_GUI_FONT;
}
