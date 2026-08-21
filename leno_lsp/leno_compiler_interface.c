/**
 * LenoC Compiler Interface for LSP
 * Reuses LenoC lexer, parser and semantic analyzer
 */

#include "leno_lsp.h"
#include "../src/include/lenolang.h"
#include "../src/include/leno_error.h"

// Global variables needed by LenoC compiler
int debugMode = 0;
// runtime_type_check is defined in vm.c

/**
 * Compile source and collect errors
 * Reuses LenoC lexer, parser, and semantic analyzer
 */
LspDiagnostic* leno_compile_and_collect_errors(const char* content, const char* filename, int* count) {
    *count = 0;
    
    if (!content) return NULL;
    
    // Set error collector filename
    error_set_filename(filename ? filename : "<memory>");
    error_clear();
    
    // 1. Lexical analysis
    Lexer lexer;
    lexer_init(&lexer, content);
    
    // Check for lexer errors
    if (error_has_any()) {
        goto collect_errors;
    }
    
    // 2. Syntax analysis
    Parser parser;
    parser_init(&parser, content);
    
    if (!parser_parse(&parser)) {
        // Parser errors already collected
        goto collect_errors;
    }
    
    // 3. Semantic analysis
    if (parser.root) {
        Semantic semantic;
        semantic_init(&semantic, parser.root);
        semantic_analyze(&semantic, parser.root);
        semantic_cleanup(&semantic);
        // Semantic errors collected
    }
    
    // 4. Cleanup AST (LSP doesn't need to keep it)
    if (parser.root) {
        ast_free(parser.root);
    }
    
collect_errors:
    // Collect error information
    if (errors.count == 0) {
        return NULL;
    }
    
    LspDiagnostic* diags = (LspDiagnostic*)malloc(sizeof(LspDiagnostic) * errors.count);
    if (!diags) {
        error_clear();
        return NULL;
    }
    
    for (int i = 0; i < errors.count; i++) {
        Error* err = &errors.list[i];
        
        // Convert error type to LSP severity
        switch (err->type) {
            case ERR_SYNTAX:
            case ERR_SEMANTIC:
            case ERR_UNDEFINED_VAR:
            case ERR_UNDEFINED_FUNC:
            case ERR_DUPLICATE_VAR:
            case ERR_TYPE_MISMATCH:
                diags[i].severity = LSP_DIAG_ERROR;
                break;
            case ERR_RUNTIME:
                diags[i].severity = LSP_DIAG_WARNING;
                break;
            default:
                diags[i].severity = LSP_DIAG_INFORMATION;
        }
        
        // Set position (LenoC uses 1-based line numbers, LSP uses 0-based)
        diags[i].range.start.line = err->line > 0 ? err->line - 1 : 0;
        // 利用编译器的列号精确定位
        if (err->column > 0) {
            diags[i].range.start.character = err->column - 1;
            diags[i].range.end.character = err->column;
        } else {
            diags[i].range.start.character = 0;
            diags[i].range.end.character = 100;
        }
        diags[i].range.end.line = diags[i].range.start.line;
        
        // Error code
        diags[i].code = (char*)malloc(16);
        if (diags[i].code) {
            snprintf(diags[i].code, 16, "E%d", i + 1);
        }
        
        // Error message
        diags[i].message = strdup(err->msg);
        
        // Source
        diags[i].source = strdup("leno");
    }
    
    *count = errors.count;
    error_clear();
    
    return diags;
}

/**
 * Get type info at position (for hover)
 */
char* leno_get_type_at_position(const char* content, LspPosition pos) {
    (void)pos;  // Unused for now
    if (!content) return NULL;
    
    // TODO: Implement type inference
    // This requires more complex analysis
    
    return NULL;
}

/**
 * Get definition location (for go-to-definition)
 */
LspLocation* leno_get_definition_location(const char* content, const char* symbol_name, int* count) {
    *count = 0;
    
    if (!content || !symbol_name) return NULL;
    
    // TODO: Implement definition lookup
    // Need to find symbol definition in AST
    
    return NULL;
}
