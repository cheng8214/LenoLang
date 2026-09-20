#include "codegen.h"

void gen_import_inline(CodeGen* gen, Ast* ast) {
    // 原生模块（如 times, io 等）：生成 OP_LOAD_NATIVE_MODULE 操作码
    if (!strstr(ast->u.import.module_name, ".leno")) {
        // 生成字节码：OP_LOAD_NATIVE_MODULE <module_name_constant>
        int constant = make_constant(gen, val_obj((Object*)str_new(ast->u.import.module_name, strlen(ast->u.import.module_name))));
        emit_byte(gen, OP_LOAD_NATIVE_MODULE, ast->line);
        emit_byte(gen, (constant >> 8) & 0xff, ast->line);
        emit_byte(gen, constant & 0xff, ast->line);
        return;
    }

    const char* alias = ast->u.import.alias;
    char* extracted_name = NULL;
    if (!alias) {
        const char* base = strrchr(ast->u.import.module_name, '/');
        if (!base) base = strrchr(ast->u.import.module_name, '\\');
        if (!base) base = ast->u.import.module_name;
        else base++;

        const char* dot = strrchr(base, '.');
        if (dot) {
            extracted_name = (char*)malloc(dot - base + 1);
            strncpy(extracted_name, base, dot - base);
            extracted_name[dot - base] = '\0';
        } else {
            extracted_name = strdup(base);
        }
        alias = extracted_name;
    }

    const char* current_file = error_get_filename();
    ObjModule* module = load_module_file(ast->u.import.module_name, current_file, alias);

    if (!module) {
        // 模块加载失败时，检查是否已有前序语义错误
        // 如果已有错误（如 face/impl 缺少方法），则不报"无法加载模块"级联错误
        // 因为根因错误已经在模块自身的编译阶段报告过了，级联错误只会掩盖真正的问题
        if (!error_has_any()) {
            char err_msg[BUFFER_MEDIUM];
            snprintf(err_msg, sizeof(err_msg), "无法加载模块 '%s'", ast->u.import.module_name);
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, err_msg);
        }
        if (extracted_name) {
            free(extracted_name);
        }
        return;
    }

    // 将模块对象作为常量
    emit_constant(gen, val_obj((Object*)module), ast->line);

    // 生成 OP_INIT_LENOMODULE：在运行时初始化 .leno 模块（执行 init_chunk）
    emit_byte(gen, OP_INIT_LENOMODULE, ast->line);

    // 定义变量存储模块
    Symbol* sym = scope_resolve(gen->sem->root_scope, alias);
    if (sym) {
        if (sym->kind == SYM_GLOBAL) {
            emit_define_global(gen, sym->index, ast->line);
        } else if (sym->kind == SYM_MODULE) {
            // 模块级别的变量，使用 OP_SET_MODULE_VAR
            emit_bytes_2(gen, OP_SET_MODULE_VAR, sym->index, ast->line);
            emit_byte(gen, OP_POP, ast->line);
        }
    }

    if (extracted_name) {
        free(extracted_name);
    }
}
