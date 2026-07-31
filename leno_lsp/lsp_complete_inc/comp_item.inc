// 前置声明
static void add_completion_item(LspCompletionItem** items, int* count, int* capacity,
                                const char* label, int kind, const char* detail, const char* documentation);

// 添加补全项
static void add_completion_item(LspCompletionItem** items, int* count, int* capacity,
                                 const char* label, int kind, const char* detail,
                                 const char* prefix) {
    if (!label) return;
    
    // 前缀匹配检查
    if (prefix && prefix[0] != '\0') {
        if (strncmp(label, prefix, strlen(prefix)) != 0) {
            return;
        }
    }
    
    // 检查是否已存在
    for (int i = 0; i < *count; i++) {
        if (strcmp((*items)[i].label, label) == 0) {
            return;
        }
    }
    
    // 扩容
    if (*count >= *capacity) {
        *capacity *= 2;
        *items = (LspCompletionItem*)realloc(*items,
            sizeof(LspCompletionItem) * (*capacity));
    }
    
    (*items)[*count].label = strdup(label);
    (*items)[*count].kind = kind;
    (*items)[*count].detail = detail ? strdup(detail) : NULL;
    (*items)[*count].documentation = NULL;
    (*items)[*count].insertText = NULL;
    (*count)++;
}
