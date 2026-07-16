static int module_symbol_table_scan_depth(ModuleSymbolTable* table, const char* current_file, int depth) {
    if (depth > MODULE_SCAN_MAX_DEPTH) return -1;
    if (!table || !table->module_path) return -1;

    // Pass 1: 读取文件、规范化路径、收集所有 struct/cstruct/clib/face/enum 名称
    #include "scan/scan_pass1.inc"

    // Pass 2: 初始化本地别名表、进入主循环、跳过空白/注释、处理 use 导入
    #include "scan/scan_pass2_init.inc"

        // 查找 export 关键字
        if (strncmp(p, "export", 6) == 0 && (p[6] == ' ' || p[6] == '\t' || p[6] == '\n' || p[6] == '\r' || p[6] == '\0')) {
            char* after_export = p + 6;
            while (*after_export && (*after_export == ' ' || *after_export == '\t' || *after_export == '\n' || *after_export == '\r')) after_export++;

            #include "scan/scan_struct.inc"
            #include "scan/scan_cstruct.inc"
            #include "scan/scan_face.inc"
            #include "scan/scan_enum.inc"
            #include "scan/scan_func.inc"
            #include "scan/scan_var.inc"
            #include "scan/scan_alias.inc"
        }


        p++;
    }


    // 清理局部别名表、名称数组，返回结果
    #include "scan/scan_cleanup.inc"
