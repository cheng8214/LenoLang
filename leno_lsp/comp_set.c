/**
 * CompletionSet 实现
 * 
 * 核心数据结构：优先级 + 去重 + 排序
 * 
 * 去重策略：相同 label 的项只保留优先级最高（数值最小）的
 * 排序策略：priority 升序 → label 字母序
 */

#include "lsp_completion.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ========== 内部辅助函数 ========== */

static char* safe_strdup(const char* s) {
    if (!s) return NULL;
    return strdup(s);
}


/* ========== CompletionSet API ========== */

CompletionSet* comp_set_create(void) {
    CompletionSet* set = (CompletionSet*)malloc(sizeof(CompletionSet));
    if (!set) return NULL;
    
    set->capacity = 128;
    set->count = 0;
    set->has_dup = 0;
    set->items = (CompItem*)malloc(sizeof(CompItem) * set->capacity);
    
    if (!set->items) {
        free(set);
        return NULL;
    }
    
    return set;
}

void comp_set_destroy(CompletionSet* set) {
    if (!set) return;
    
    for (int i = 0; i < set->count; i++) {
        free(set->items[i].label);
        free(set->items[i].filterText);
        free(set->items[i].insertText);
        free(set->items[i].detail);
        free(set->items[i].documentation);
        free(set->items[i].sortText);
    }
    
    free(set->items);
    free(set);
}

static void ensure_capacity(CompletionSet* set) {
    if (set->count >= set->capacity) {
        set->capacity *= 2;
        set->items = (CompItem*)realloc(set->items, sizeof(CompItem) * set->capacity);
    }
}

static int find_item_by_label(CompletionSet* set, const char* label) {
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->items[i].label, label) == 0) {
            return i;
        }
    }
    return -1;
}

int comp_set_add(CompletionSet* set,
                 const char* label,
                 int kind,
                 int priority,
                 const char* detail,
                 const char* documentation,
                 const char* insertText,
                 const char* filterText) {
    if (!set || !label) return -1;
    
    // 检查是否已存在相同 label
    int existing = find_item_by_label(set, label);
    if (existing >= 0) {
        // 已存在，保留优先级更高的
        if (priority < set->items[existing].priority) {
            set->items[existing].priority = priority;
            set->items[existing].kind = kind;
            free(set->items[existing].detail);
            set->items[existing].detail = safe_strdup(detail);
            free(set->items[existing].documentation);
            set->items[existing].documentation = safe_strdup(documentation);
            free(set->items[existing].insertText);
            set->items[existing].insertText = safe_strdup(insertText);
            free(set->items[existing].filterText);
            set->items[existing].filterText = safe_strdup(filterText);
        } else {
            set->has_dup = 1;
        }
        return 0;  // 重复，跳过
    }
    
    ensure_capacity(set);
    
    CompItem* item = &set->items[set->count];
    item->label         = safe_strdup(label);
    item->filterText    = safe_strdup(filterText ? filterText : label);
    item->insertText    = safe_strdup(insertText);
    item->detail        = safe_strdup(detail);
    item->documentation = safe_strdup(documentation);
    item->kind          = kind;
    item->priority      = priority;
    item->sortText      = NULL;  // 稍后统一计算
    
    set->count++;
    return 1;
}

int comp_set_add_unique(CompletionSet* set,
                        const char* label,
                        int kind,
                        int priority,
                        const char* detail,
                        const char* documentation,
                        const char* insertText,
                        const char* filterText) {
    // 同 comp_set_add（这是语义别名）
    return comp_set_add(set, label, kind, priority, detail, documentation, insertText, filterText);
}

/* ========== 排序和输出 ========== */

static int item_compare(const void* a, const void* b) {
    const CompItem* ia = (const CompItem*)a;
    const CompItem* ib = (const CompItem*)b;
    
    // 优先级升序（数值小的在前）
    if (ia->priority != ib->priority) {
        return ia->priority - ib->priority;
    }
    
    // 同优先级按 label 字母序
    return strcmp(ia->label, ib->label);
}

LspCompletionItem* comp_set_to_lsp_array(CompletionSet* set, int* out_count) {
    if (!set || !out_count) return NULL;
    
    *out_count = 0;
    
    if (set->count == 0) {
        return NULL;
    }
    
    // 排序
    qsort(set->items, set->count, sizeof(CompItem), item_compare);
    
    // 生成 sortText
    for (int i = 0; i < set->count; i++) {
        char sort_buf[32];
        // sortText 格式: "优先级_序号" （保证稳定排序）
        snprintf(sort_buf, sizeof(sort_buf), "%04d_%04d", set->items[i].priority, i);
        sort_buf[sizeof(sort_buf) - 1] = '\0';
        set->items[i].sortText = strdup(sort_buf);
    }
    
    // 转换
    LspCompletionItem* result = (LspCompletionItem*)malloc(
        sizeof(LspCompletionItem) * set->count
    );
    
    if (!result) return NULL;
    
    for (int i = 0; i < set->count; i++) {
        result[i].label         = safe_strdup(set->items[i].label);
        result[i].kind          = set->items[i].kind;
        result[i].detail        = safe_strdup(set->items[i].detail);
        result[i].documentation = safe_strdup(set->items[i].documentation);
        result[i].insertText    = safe_strdup(set->items[i].insertText);
    }
    
    *out_count = set->count;
    return result;
}
