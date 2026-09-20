/**
 * 文本文档管理
 * 管理打开的文档和增量更新
 */

#include "leno_lsp.h"

// 打开文档
LspTextDocument* lsp_document_open(LspServer* server, const char* uri, 
                                   const char* content, int version) {
    if (!server || !uri) return NULL;
    
    // 检查是否已存在
    LspTextDocument* existing = lsp_document_get(server, uri);
    if (existing) {
        // 更新现有文档
        lsp_document_update(server, uri, content, version);
        return existing;
    }
    
    // 创建新文档
    LspTextDocument* doc = (LspTextDocument*)malloc(sizeof(LspTextDocument));
    if (!doc) return NULL;
    
    doc->uri = strdup(uri);
    doc->content = content ? strdup(content) : strdup("");
    doc->version = version;
    doc->next = server->documents;
    
    server->documents = doc;
    
    lsp_log(server, LSP_LOG_INFO, "Document opened: %s (version %d)", uri, version);
    
    return doc;
}

// 更新文档
void lsp_document_update(LspServer* server, const char* uri, 
                         const char* content, int version) {
    if (!server || !uri) return;
    
    LspTextDocument* doc = lsp_document_get(server, uri);
    if (!doc) return;
    
    free(doc->content);
    doc->content = content ? strdup(content) : strdup("");
    doc->version = version;
    
    lsp_log(server, LSP_LOG_DEBUG, "Document updated: %s (version %d)", uri, version);
}

// 增量更新文档
void lsp_document_update_incremental(LspServer* server, const char* uri,
                                      int start_line, int start_char,
                                      int end_line, int end_char,
                                      const char* new_text, int version) {
    if (!server || !uri) return;
    
    LspTextDocument* doc = lsp_document_get(server, uri);
    if (!doc) return;
    
    // 安全检查
    if (!doc->content) {
        doc->content = strdup("");
    }
    
    int old_len = strlen(doc->content);
    
    // 将行列位置转换为偏移量
    LspPosition start_pos = {start_line, start_char};
    LspPosition end_pos = {end_line, end_char};
    
    int start_offset = lsp_position_to_offset(doc->content, start_pos);
    int end_offset = lsp_position_to_offset(doc->content, end_pos);
    
    // 边界检查
    if (start_offset < 0) start_offset = 0;
    if (start_offset > old_len) start_offset = old_len;
    if (end_offset < start_offset) end_offset = start_offset;
    if (end_offset > old_len) end_offset = old_len;
    
    int new_text_len = new_text ? strlen(new_text) : 0;
    int replaced_len = end_offset - start_offset;
    
    // 计算新长度（防止溢出）
    long long new_len_ll = (long long)old_len - replaced_len + new_text_len;
    if (new_len_ll < 0) new_len_ll = 0;
    if (new_len_ll > 100000000) { // 100MB 限制
        lsp_log(server, LSP_LOG_ERROR, "Document too large: %s", uri);
        return;
    }
    int new_len = (int)new_len_ll;
    
    char* new_content = (char*)malloc(new_len + 1);
    if (!new_content) return;
    
    // 复制开始部分
    if (start_offset > 0) {
        memcpy(new_content, doc->content, start_offset);
    }
    
    // 插入新文本
    if (new_text_len > 0) {
        memcpy(new_content + start_offset, new_text, new_text_len);
    }
    
    // 复制结束部分
    int tail_len = old_len - end_offset;
    if (tail_len > 0) {
        memcpy(new_content + start_offset + new_text_len, 
               doc->content + end_offset, 
               tail_len);
    }
    
    new_content[new_len] = '\0';
    
    free(doc->content);
    doc->content = new_content;
    doc->version = version;
    
    lsp_log(server, LSP_LOG_DEBUG, "Document updated incrementally: %s (version %d)", 
            uri, version);
}

// 关闭文档
void lsp_document_close(LspServer* server, const char* uri) {
    if (!server || !uri) return;
    
    LspTextDocument** current = &server->documents;
    while (*current) {
        if (strcmp((*current)->uri, uri) == 0) {
            LspTextDocument* to_remove = *current;
            *current = to_remove->next;
            
            free(to_remove->uri);
            free(to_remove->content);
            free(to_remove);
            
            lsp_log(server, LSP_LOG_INFO, "Document closed: %s", uri);
            return;
        }
        current = &(*current)->next;
    }
}

// 获取文档
LspTextDocument* lsp_document_get(LspServer* server, const char* uri) {
    if (!server || !uri) return NULL;
    
    LspTextDocument* doc = server->documents;
    while (doc) {
        if (strcmp(doc->uri, uri) == 0) {
            return doc;
        }
        doc = doc->next;
    }
    return NULL;
}

// 将行号列号转换为偏移量
int lsp_position_to_offset(const char* content, LspPosition pos) {
    if (!content) return 0;
    
    int line = 0;
    int col = 0;
    int offset = 0;
    
    while (content[offset] != '\0') {
        if (line == (int)pos.line) {
            if (col == (int)pos.character) {
                return offset;
            }
            col++;
        }
        
        if (content[offset] == '\n') {
            line++;
            col = 0;
        }
        
        offset++;
    }
    
    return offset;
}

// 将偏移量转换为行号列号
LspPosition lsp_offset_to_position(const char* content, int offset) {
    LspPosition pos = {0, 0};
    if (!content) return pos;
    
    int current_offset = 0;
    
    while (content[current_offset] != '\0' && current_offset < offset) {
        if (content[current_offset] == '\n') {
            pos.line++;
            pos.character = 0;
        } else {
            pos.character++;
        }
        current_offset++;
    }
    
    return pos;
}

// 获取指定行的内容
char* lsp_get_line_content(const char* content, int line) {
    if (!content) return NULL;
    
    int current_line = 0;
    int offset = 0;
    
    // 找到目标行
    while (content[offset] != '\0' && current_line < line) {
        if (content[offset] == '\n') {
            current_line++;
        }
        offset++;
    }
    
    if (current_line != line) return NULL;
    
    // 找到行尾
    int start = offset;
    while (content[offset] != '\0' && content[offset] != '\n') {
        offset++;
    }
    
    int len = offset - start;
    char* line_content = (char*)malloc(len + 1);
    if (!line_content) return NULL;
    
    memcpy(line_content, content + start, len);
    line_content[len] = '\0';
    
    return line_content;
}
