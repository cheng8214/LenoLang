#include "include/lenolang.h"
#include "include/leno_ast.h"
#include "include/leno_lexer.h"
#include "include/leno_parser.h"
#include "include/leno_semantic.h"
#include "include/leno_codegen.h"
#include "include/leno_optimize.h"
#include "include/leno_serialize.h"
#include "include/native.h"
#include "include/module_compiler.h"
#include "include/module_loader.h"
#include "include/leno_package.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

// 版本信息
#define LENO_VERSION "1.0.0"

// 全局标志
int debugMode = 0;
static int pauseMode = 0;
static int compileMode = 0;
static int packMode = 0;
static int initMode = 0;
static int installMode = 0;

// 命令行参数（供 _args() 全局函数使用）
int g_argc = 0;
char** g_argv = NULL;

// 设置控制台UTF-8编码（Windows）
static void setupConsole(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

// 打印版本信息
static void printVersion(void) {
    printf("LenoLang Compiler %s\n", LENO_VERSION);
    printf("Copyright (c) 2025 LenoLang Team\n");
}

// 打印帮助信息
static void printHelp(const char* program) {
    printf("LenoLang Compiler %s\n\n", LENO_VERSION);
    printf("用法: %s [选项] <文件>\n\n", program);
    printf("选项:\n");
    printf("  -h, --help        显示帮助信息\n");
    printf("  -v, --version     显示版本信息\n");
    printf("  --pause           执行完毕后暂停\n");
    printf("  --debug           启用调试模式（输出字节码）\n");
    printf("  -c, --compile     编译为二进制文件（.lenb），不执行\n");
    printf("  -p, --pack        编译并打包为独立 exe（嵌入 leno_vm.exe）\n");
    printf("  --init [路径]     在当前目录创建新 Leno 包项目\n");
    printf("  --install         安装包或依赖到全局缓存\n");
    printf("\n");
    printf("示例:\n");
    printf("  %s script.leno       运行脚本\n", program);
    printf("  %s script.lenb       运行编译后的二进制\n", program);
    printf("  %s -c test.leno      编译为二进制\n", program);
    printf("  %s -p test.leno      打包为独立 exe\n", program);
    printf("  %s --debug test.leno 调试模式运行\n", program);
    printf("  %s --init my-package 创建新包\n", program);
    printf("  %s --install         安装当前项目依赖\n", program);
    printf("  %s --install <路径>  安装指定包目录\n", program);
    printf("  %s --install <git源> 从 Git 远程安装包\n", program);
    printf("                       如: gitee:user/repo/pkg-a\n");
}

// 主执行流程
int lenolang_run(const char* source) {
      if (debugMode) {
         printf("debug模式:进入主执行流程\n");
     }
    // 0. 清空错误
    error_clear();
    warning_clear();
     if (debugMode) {
         printf("debug模式:进入语法分析阶段\n");
     }
    // 1. 词法分析 + 语法分析
    Parser parser;
    parser_init(&parser, source);
    if (parser_parse(&parser) < 0) {
        // 语法分析失败，只释放 AST
        error_print_all();
        warning_print_all();
        ast_free(parser.root);
        return -1;
    }
   if (debugMode) {
         printf("debug模式:进入语义分析\n");
     }
    // 2. 语义分析（单遍）
    Semantic sem;
    semantic_init(&sem, parser.root);
    // 提前初始化 CodeGen，确保 fail 路径上 codegen_cleanup 安全
    Chunk chunk;
    chunk_init(&chunk);
    CodeGen gen;
    codegen_init(&gen, &chunk, &sem);
    semantic_analyze(&sem, parser.root);
    if (error_has_any()) goto fail;

    // 2.5 常量折叠优化
    optimize_constant_fold(parser.root);

    // 2.6 死代码消除
    optimize_dead_code_elimination(parser.root);

  if (debugMode) {
         printf("debug模式:开始生成字节码\n");
     }
    // 3. 生成字节码
    codegen(&gen, parser.root);
    if (error_has_any()) goto fail;

    // 调试模式：输出字节码
    if (debugMode) {
        disassembleChunk(&chunk, "主程序");
    }

    // 4. VM 执行
    gc_init();
    vm_init_with_scope(sem.root_scope);  // 使用语义分析的 scope，确保索引一致
    vm_load(&chunk);
    int ret = vm_run();

    // 5. 释放
    codegen_cleanup(&gen);
    ast_free(parser.root);
    // 释放语义分析中的资源（函数名列表）
    semantic_cleanup(&sem);
    // 释放字节码块
    chunk_free(&chunk);
    // gc_free_all 会释放 VM 的 global_scope
    gc_free_all();
    
    // 如果有运行时错误，打印错误信息
    if (ret != 0 || error_has_any()) {
        error_print_all();
        warning_print_all();
        return -1;
    }

    warning_print_all();
    return ret;

fail:
    if (debugMode) {
        printf("debug模式:编译失败\n");
    }
    error_print_all();
    warning_print_all();
    codegen_cleanup(&gen);
    ast_free(parser.root);
    // 编译失败时释放 root_scope（VM 未初始化）
    if (sem.root_scope) {
        scope_free(sem.root_scope);
        sem.root_scope = NULL;
    }
    // 释放语义分析中的其他资源
    semantic_cleanup(&sem);
    return -1;
}


// fix_module_function_ptrs 等函数已移至 module.c

int lenolang_run_binary(const char* path) {
    Chunk chunk;
    Scope* scope = NULL;

    SerializeResult result = chunk_deserialize(path, &chunk, &scope);
    if (result != SERIALIZE_OK) {
        const char* err_msg = "未知错误";
        switch (result) {
            case SERIALIZE_ERR_FILE:    err_msg = "无法打开文件"; break;
            case SERIALIZE_ERR_MAGIC:   err_msg = "不是有效的 .lenb 文件"; break;
            case SERIALIZE_ERR_VERSION: err_msg = "二进制文件版本不兼容"; break;
            case SERIALIZE_ERR_FORMAT:  err_msg = "文件格式错误"; break;
            case SERIALIZE_ERR_MEMORY:  err_msg = "内存不足"; break;
            case SERIALIZE_ERR_READ:    err_msg = "读取文件失败"; break;
            default: break;
        }
        fprintf(stderr, "加载二进制文件失败: %s (%s)\n", path, err_msg);
        return -1;
    }

    if (debugMode) {
        printf("debug模式:从二进制文件加载成功\n");
        disassembleChunk(&chunk, "主程序");
    }

    gc_init();
    vm_init_with_scope(scope);
    fix_module_function_ptrs(&chunk);
    vm_load(&chunk);
    int ret = vm_run();

    chunk_free(&chunk);
    gc_free_all();

    if (ret != 0 || error_has_any()) {
        error_print_all();
        warning_print_all();
        return -1;
    }

    warning_print_all();
    return ret;
}

// 编译源代码到二进制文件
int lenolang_compile(const char* source, const char* output_path) {
    clock_t compile_t0 = clock();
    error_clear();
    warning_clear();

    gc_init();
    vm_init();

    Parser parser;
    parser_init(&parser, source);
    if (parser_parse(&parser) < 0) {
        error_print_all();
        warning_print_all();
        ast_free(parser.root);
        gc_free_all();
        return -1;
    }

    Semantic sem;
    semantic_init(&sem, parser.root);
    Chunk chunk;
    chunk_init(&chunk);
    CodeGen gen;
    codegen_init(&gen, &chunk, &sem);
    semantic_analyze(&sem, parser.root);
    if (error_has_any()) goto compile_fail;

    optimize_constant_fold(parser.root);
    optimize_dead_code_elimination(parser.root);

    codegen(&gen, parser.root);
    if (error_has_any()) goto compile_fail;

    if (debugMode) {
        disassembleChunk(&chunk, "主程序");
    }

    SerializeResult result = chunk_serialize(output_path, &chunk, sem.root_scope);
    if (result != SERIALIZE_OK) {
        fprintf(stderr, "写入二进制文件失败: %s (错误码: %d)\n", output_path, result);
        codegen_cleanup(&gen);
        ast_free(parser.root);
        semantic_cleanup(&sem);
        chunk_free(&chunk);
        gc_free_all();
        return -1;
    }

    printf("编译成功: %s -> %s\n", chunk.filename ? chunk.filename : "stdin", output_path);
    {
        clock_t compile_t1 = clock();
        double compile_ms = (double)(compile_t1 - compile_t0) / CLOCKS_PER_SEC * 1000.0;
        printf("编译耗时: %.1f ms\n", compile_ms);
    }

    codegen_cleanup(&gen);
    ast_free(parser.root);
    semantic_cleanup(&sem);
    chunk_free(&chunk);
    gc_free_all();
    warning_print_all();
    return 0;

compile_fail:
    error_print_all();
    warning_print_all();
    codegen_cleanup(&gen);
    ast_free(parser.root);
    if (sem.root_scope) {
        scope_free(sem.root_scope);
        sem.root_scope = NULL;
    }
    semantic_cleanup(&sem);
    gc_free_all();
    return -1;
}

// 从文件运行
int lenolang_run_file(const char* path) {
    // 检查是否是 .lenb 二进制文件
    if (serialize_is_binary_file(path)) {
        return lenolang_run_binary(path);
    }

    // 将路径转换为绝对路径，确保模块导入时相对路径能正确解析
    char abs_path[MAX_PATH_LEN];
#ifdef _WIN32
    // Windows: _fullpath 使用 ANSI 代码页，不支持 UTF-8 中文路径
    // 需要先将 UTF-8 转为宽字符，用 _wfullpath，再转回 UTF-8
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        if (wlen > 0) {
            wchar_t* wpath = (wchar_t*)malloc(wlen * sizeof(wchar_t));
            if (wpath) {
                MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
                wchar_t wabs[MAX_PATH_LEN];
                if (_wfullpath(wabs, wpath, MAX_PATH_LEN) != NULL) {
                    int abs_len = WideCharToMultiByte(CP_UTF8, 0, wabs, -1, abs_path, MAX_PATH_LEN, NULL, NULL);
                    if (abs_len > 0) {
                        error_set_filename(abs_path);
                    } else {
                        error_set_filename(path);
                    }
                } else {
                    error_set_filename(path);
                }
                free(wpath);
            } else {
                error_set_filename(path);
            }
        } else {
            error_set_filename(path);
        }
    }
#else
    if (realpath(path, abs_path) != NULL) {
        error_set_filename(abs_path);
    } else {
        error_set_filename(path);
    }
#endif
    
#ifdef _WIN32
    // Windows: 使用宽字符支持中文路径
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t* widePath = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
    if (widePath == NULL) {
        fprintf(stderr, "内存不足\n");
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath, wideLen);
    
    FILE* file = _wfopen(widePath, L"rb");
    free(widePath);
#else
    FILE* file = fopen(path, "rb");
#endif
    
    if (!file) {
        fprintf(stderr, "无法打开文件: %s\n", path);
        return -1;
    }
    
    // 获取文件大小
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // 读取文件内容
    char* source = (char*)malloc(size + 1);
    if (!source) {
        fprintf(stderr, "内存分配失败\n");
        fclose(file);
        return -1;
    }
    
    size_t read = fread(source, 1, size, file);
    source[read] = '\0';
    fclose(file);

    // 设置模块搜索路径（根据项目根目录 + 全局缓存）
    // 必须在编译/打包/运行之前设置，确保 import 能解析到包
    {
        package_search_path_clear();
        const char* abs_file = error_get_filename();
        if (abs_file) {
            char* proj_root = package_find_project_root(abs_file);
            if (proj_root) {
                // 添加 <项目根>/lib/ 到搜索路径
                char lib_path[MAX_PATH_LEN];
                snprintf(lib_path, sizeof(lib_path), "%slib%c", proj_root, 
#ifdef _WIN32
                    '\\'
#else
                    '/'
#endif
                );
                package_search_path_add(lib_path);

                // 从 leno.toml 读取依赖，添加依赖包的 lib/ 到搜索路径
                char toml_path[MAX_PATH_LEN];
                snprintf(toml_path, sizeof(toml_path), "%sleno.toml", proj_root);
                PackageConfig* pkg_cfg = package_config_parse(toml_path);
                if (pkg_cfg) {
                    for (int di = 0; di < pkg_cfg->dep_count; di++) {
                        const char* dn = pkg_cfg->dependencies[di].name;
                        if (!dn) continue;
                        const char* cache = package_cache_dir();
                        char dep_lib[MAX_PATH_LEN];
                        snprintf(dep_lib, sizeof(dep_lib), "%s%s%clib%c",
                                 cache, dn,
#ifdef _WIN32
                                 '\\',
#else
                                 '/',
#endif
#ifdef _WIN32
                                 '\\'
#else
                                 '/'
#endif
                        );
                        package_search_path_add(dep_lib);
                    }
                    package_config_free(pkg_cfg);
                }
                free(proj_root);
            }
            // 始终添加源文件所在目录作为搜索路径
            char file_dir[MAX_PATH_LEN];
            strncpy(file_dir, abs_file, MAX_PATH_LEN - 1);
            file_dir[MAX_PATH_LEN - 1] = '\0';
            char* last_sep = strrchr(file_dir, 
#ifdef _WIN32
                '\\'
#else
                '/'
#endif
            );
            if (last_sep) *(last_sep + 1) = '\0';
            package_search_path_add(file_dir);
        }

        // 添加全局缓存中所有已安装包的 lib/ 到搜索路径
        package_cache_add_to_search_paths();

        // 设置模块编译缓存目录（优先项目根，fallback 到 entry 文件目录）
        if (module_loader_is_cache_enabled()) {
            const char* abs_f = error_get_filename();
            char* proot = abs_f ? package_find_project_root(abs_f) : NULL;
            const char* base_dir = NULL;
            char dir_buf[MAX_PATH_LEN];
            if (proot) {
                base_dir = proot;
            } else if (abs_f) {
                strncpy(dir_buf, abs_f, MAX_PATH_LEN - 1);
                dir_buf[MAX_PATH_LEN - 1] = '\0';
                char* ls = strrchr(dir_buf,
#ifdef _WIN32
                    '\\'
#else
                    '/'
#endif
                );
                if (ls) *(ls + 1) = '\0';
                base_dir = dir_buf;
            }
            if (base_dir) {
                char cache_dir[MAX_PATH_LEN];
                snprintf(cache_dir, sizeof(cache_dir), "%s.lenocache%c", base_dir,
#ifdef _WIN32
                    '\\'
#else
                    '/'
#endif
                );
                module_loader_set_cache_dir(cache_dir);
            }
            if (proot) free(proot);
        }
    }

    // 编译模式：编译为 .lenb 文件
    if (compileMode) {
        char* bin_path = serialize_get_bin_path(path);
        if (!bin_path) {
            fprintf(stderr, "无法生成输出路径\n");
            free(source);
            return -1;
        }
        int result = lenolang_compile(source, bin_path);
        free(source);
        free(bin_path);

        if (pauseMode) {
            printf("\n按任意键继续...");
            getchar();
        }
        return result;
    }

    // 打包模式：编译为 .lenb 并嵌入 leno_vm.exe 尾部
    if (packMode) {
        char* bin_path = serialize_get_bin_path(path);
        if (!bin_path) {
            fprintf(stderr, "无法生成输出路径\n");
            free(source);
            return -1;
        }
        clock_t pack_t0 = clock();
        int result = lenolang_compile(source, bin_path);
        free(source);
        if (result != 0) {
            free(bin_path);
            return result;
        }
        clock_t pack_compile_end = clock();

        // 生成输出 exe 路径：与源文件同目录，扩展名改为 .exe
        char out_exe[MAX_PATH_LEN];
        strncpy(out_exe, path, MAX_PATH_LEN - 1);
        out_exe[MAX_PATH_LEN - 1] = '\0';
        char* dot = strrchr(out_exe, '.');
        if (dot) {
            strcpy(dot, ".exe");
        } else {
            strcat(out_exe, ".exe");
        }

        // 查找 leno_vm：先在与 leno同目录的 build/ 下找
        char vm_exe[MAX_PATH_LEN];
#ifdef _WIN32
        // 获取当前 exe 所在目录
        char exe_dir[MAX_PATH_LEN];
        GetModuleFileNameA(NULL, exe_dir, MAX_PATH_LEN);
        exe_dir[MAX_PATH_LEN - 1] = '\0';
        char* last_sep = strrchr(exe_dir, '\\');
        if (last_sep) {
            *(last_sep + 1) = '\0';
            // 检查路径长度，防止缓冲区溢出
            size_t dir_len = strlen(exe_dir);
            const char* vm_name = "leno_vm.exe";
            size_t vm_name_len = strlen(vm_name);
            if (dir_len + vm_name_len < MAX_PATH_LEN) {
                memcpy(vm_exe, exe_dir, dir_len);
                memcpy(vm_exe + dir_len, vm_name, vm_name_len + 1);
            } else {
                // 路径太长，使用默认名称
                strcpy(vm_exe, "leno_vm.exe");
            }
        } else {
            strcpy(vm_exe, "leno_vm.exe");
        }
#else
        strcpy(vm_exe, "leno_vm");
#endif

        // 读取 leno_vm 文件
#ifdef _WIN32
        wchar_t wvm_exe[MAX_PATH_LEN];
        MultiByteToWideChar(CP_UTF8, 0, vm_exe, -1, wvm_exe, MAX_PATH_LEN);
        FILE* vm_fp = _wfopen(wvm_exe, L"rb");
#else
        FILE* vm_fp = fopen(vm_exe, "rb");
#endif
        if (!vm_fp) {
            fprintf(stderr, "[错误] 找不到 leno_vm.exe: %s\n", vm_exe);
            fprintf(stderr, "请先运行 build_vm.bat 构建 VM 运行时\n");
#ifdef _WIN32
            { wchar_t wp[MAX_PATH_LEN]; MultiByteToWideChar(CP_UTF8, 0, bin_path, -1, wp, MAX_PATH_LEN); _wremove(wp); }
#else
            remove(bin_path);
#endif
            free(bin_path);
            return -1;
        }
        fseek(vm_fp, 0, SEEK_END);
        long vm_size = ftell(vm_fp);
        fseek(vm_fp, 0, SEEK_SET);
        unsigned char* vm_data = (unsigned char*)malloc(vm_size);
        if (!vm_data) {
            fclose(vm_fp);
            free(bin_path);
            return -1;
        }
        fread(vm_data, 1, vm_size, vm_fp);
        fclose(vm_fp);

        // 读取编译好的 .lenb 文件
#ifdef _WIN32
        wchar_t wlenb_path[MAX_PATH_LEN];
        MultiByteToWideChar(CP_UTF8, 0, bin_path, -1, wlenb_path, MAX_PATH_LEN);
        FILE* lenb_fp = _wfopen(wlenb_path, L"rb");
#else
        FILE* lenb_fp = fopen(bin_path, "rb");
#endif
        if (!lenb_fp) {
            fprintf(stderr, "[错误] 无法读取编译产物: %s\n", bin_path);
            free(vm_data);
#ifdef _WIN32
            { wchar_t wp[MAX_PATH_LEN]; MultiByteToWideChar(CP_UTF8, 0, bin_path, -1, wp, MAX_PATH_LEN); _wremove(wp); }
#else
            remove(bin_path);
#endif
            free(bin_path);
            return -1;
        }
        fseek(lenb_fp, 0, SEEK_END);
        long lenb_size = ftell(lenb_fp);
        fseek(lenb_fp, 0, SEEK_SET);
        unsigned char* lenb_data = (unsigned char*)malloc(lenb_size);
        if (!lenb_data) {
            fclose(lenb_fp);
            free(vm_data);
#ifdef _WIN32
            { wchar_t wp[MAX_PATH_LEN]; MultiByteToWideChar(CP_UTF8, 0, bin_path, -1, wp, MAX_PATH_LEN); _wremove(wp); }
#else
            remove(bin_path);
#endif
            free(bin_path);
            return -1;
        }
        fread(lenb_data, 1, lenb_size, lenb_fp);
        fclose(lenb_fp);

        // 删除临时 .lenb 文件（已读入内存）
#ifdef _WIN32
        { wchar_t wp[MAX_PATH_LEN]; MultiByteToWideChar(CP_UTF8, 0, bin_path, -1, wp, MAX_PATH_LEN); _wremove(wp); }
#else
        remove(bin_path);
#endif

        // 写入输出文件: [vm 数据] [lenb 数据] [4字节 lenb_size] [4字节 LENB_MAGIC]
#ifdef _WIN32
        wchar_t wout_exe[MAX_PATH_LEN];
        MultiByteToWideChar(CP_UTF8, 0, out_exe, -1, wout_exe, MAX_PATH_LEN);
        FILE* out_fp = _wfopen(wout_exe, L"wb");
#else
        FILE* out_fp = fopen(out_exe, "wb");
#endif
        if (!out_fp) {
            fprintf(stderr, "[错误] 无法创建输出文件: %s\n", out_exe);
            free(lenb_data);
            free(vm_data);
            free(bin_path);
            return -1;
        }
        fwrite(vm_data, 1, vm_size, out_fp);
        fwrite(lenb_data, 1, lenb_size, out_fp);
        uint32_t lenb_size_le = (uint32_t)lenb_size;
        fwrite(&lenb_size_le, 4, 1, out_fp);
        uint32_t magic = 0x424E454C; // "LENB"
        fwrite(&magic, 4, 1, out_fp);
        fclose(out_fp);

        free(vm_data);
        free(lenb_data);
        free(bin_path);

        {
            clock_t pack_end = clock();
            double embed_ms = (double)(pack_end - pack_compile_end) / CLOCKS_PER_SEC * 1000.0;
            double total_ms = (double)(pack_end - pack_t0) / CLOCKS_PER_SEC * 1000.0;
            printf("打包成功: %s -> %s (%.1f KB)\n", path, out_exe,
                   (vm_size + lenb_size + 8) / 1024.0);
            printf("打包嵌入耗时: %.1f ms\n", embed_ms);
            printf("总耗时: %.1f ms\n", total_ms);
        }

        if (pauseMode) {
            printf("\n按任意键继续...");
            getchar();
        }
        return 0;
    }

    // 输出源代码
   if (!debugMode) 
    {
        // printf("===== 源代码 =====\n");
        //  printf("%s", source);
        //  printf("\n===== 执行结果 =====\n\n");
    }
    
    int result = lenolang_run(source);
    free(source);
    
    // 暂停模式
    if (pauseMode) {
        printf("\n按任意键继续...");
        getchar();
    }
    
    return result;
}


// 主函数逻辑
static int main_logic(int argc, char** argv) {
    setupConsole();

    // 注册模块编译器函数指针（解耦 module_loader 和编译器）
    set_module_compile_func(compile_module_new);

    // 环境变量禁用缓存（LENO_NO_CACHE=1）
    if (getenv("LENO_NO_CACHE") != NULL) {
        module_loader_set_cache_enabled(0);
    }

    // 保存命令行参数，供 _args() 全局函数使用
    g_argc = argc;
    g_argv = argv;

    const char* filePath = NULL;
    
    // 解析参数
    // Leno 内置选项（--pause, --debug 等）在任何位置都生效
    // 第一个非选项参数作为脚本路径，之后的非选项参数传给脚本
    for (int i = 1; i < argc; i++) {
        // 先检查是否是 Leno 内置选项（在任何位置都处理）
        if (strcmp(argv[i], "--pause") == 0) {
            pauseMode = 1;
            continue;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debugMode = 1;
            continue;
        } else if (strcmp(argv[i], "--compile") == 0 || strcmp(argv[i], "-c") == 0) {
            compileMode = 1;
            continue;
        } else if (strcmp(argv[i], "--pack") == 0 || strcmp(argv[i], "-p") == 0) {
            packMode = 1;
            continue;
        } else if (strcmp(argv[i], "--no-cache") == 0) {
            module_loader_set_cache_enabled(0);
            continue;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printVersion();
            return 0;
        } else if (strcmp(argv[i], "--init") == 0) {
            initMode = 1;
            continue;
        } else if (strcmp(argv[i], "--install") == 0) {
            installMode = 1;
            continue;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printHelp(argv[0]);
            return 0;
        }

        // 非选项参数
        if (argv[i][0] != '-') {
            if (filePath == NULL) {
                // 第一个非选项参数是脚本路径
                filePath = argv[i];
            }
            // 后续非选项参数保留给脚本（由 _args() 获取）
            continue;
        }

        // 未知的 '-' 开头选项
        fprintf(stderr, "未知选项: %s\n", argv[i]);
        printHelp(argv[0]);
        return 64;
    }
    
    if (initMode) {
        if (filePath) {
            /* 如果参数包含路径分隔符，当作目录路径；否则在当前目录下创建子目录 */
            int is_path = strchr(filePath, '/') || strchr(filePath, '\\');
            if (is_path) {
                return package_init(filePath, NULL);
            } else {
                return package_init(filePath, filePath);
            }
        } else {
            return package_init(".", NULL);
        }
    } else if (installMode) {
        package_cache_ensure();
        if (filePath) {
            /* leno --install <git-url> 或 <本地目录路径> */
            if (strncmp(filePath, "gitee:", 6) == 0 ||
                strncmp(filePath, "github:", 7) == 0 ||
                strncmp(filePath, "gitlab:", 7) == 0 ||
                strncmp(filePath, "git:", 4) == 0 ||
                strncmp(filePath, "https://", 8) == 0 ||
                strncmp(filePath, "http://", 7) == 0 ||
                strstr(filePath, "git@")) {
                /* git 源 → 远程安装 */
                return package_install_from_git(filePath);
            } else {
                /* 本地目录路径 → 本地安装 */
                return package_install_from_dir(filePath);
            }
        } else {
            /* leno --install - 从当前目录的 leno.toml 安装所有依赖 */
            char toml_path[MAX_PATH_LEN];
            snprintf(toml_path, sizeof(toml_path), "leno.toml");
            return package_install_deps(toml_path);
        }
    } else if (filePath == NULL) {
        // 没有文件参数，显示帮助信息
        printHelp(argv[0]);
        // 暂停以便用户查看帮助信息
        printf("\n按任意键继续...");
        getchar();
        return 0;
    } else {
        // 文件模式
        return lenolang_run_file(filePath);
    }
    
    return 0;
}

#ifdef _WIN32
// Windows: 使用 wmain 支持 Unicode 命令行
int wmain(int argc, wchar_t* wargv[]) {
    // 注册模块编译器函数指针（解耦 module_loader 和编译器）
    set_module_compile_func(compile_module_new);

    // 将宽字符参数转换为 UTF-8
    char** argv = (char**)malloc((argc + 1) * sizeof(char*));
    if (!argv) {
        fprintf(stderr, "内存分配失败\n");
        return 1;
    }
    
    for (int i = 0; i < argc; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = (char*)malloc(len);
        if (!argv[i]) {
            fprintf(stderr, "内存分配失败\n");
            return 1;
        }
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], len, NULL, NULL);
    }
    argv[argc] = NULL;
    
    int result = main_logic(argc, argv);
    
    // 释放内存
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
    
    return result;
}
#else
// Linux/macOS: 使用标准 main
int main(int argc, char* argv[]) {
    return main_logic(argc, argv);
}
#endif
