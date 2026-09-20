// ============================================================================
// 寄存器式 codegen：工具函数
// ============================================================================

#include "codegen.h"

// 工具函数已在 codegen_import.c 中定义，此文件保留用于未来扩展
// 大部分工具函数（is_string_expr 等）已移到 codegen_import.c

// ============================================================================
// find_main_function：在全局作用域查找 main 函数
// 返回 {has_main, main_index}，未找到时 has_main=0、main_index=-1
// 寄存器式与栈式语义一致（直接照搬栈式基线实现，仅注释保留）
// ============================================================================
MainFuncInfo find_main_function(Semantic* sem) {
    MainFuncInfo info = {0, -1};

    Scope* global = sem->root_scope;
    if (!global) return info;

    for (int i = 0; i < global->sym_cnt; i++) {
        Symbol* sym = global->syms[i];
        if (sym && sym->kind == SYM_GLOBAL_FUNC && strcmp(sym->name, "main") == 0) {
            info.has_main = 1;
            info.main_index = sym->index;
            break;
        }
    }

    return info;
}
