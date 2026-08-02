#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../include/module_symbol_table.h"
#include "../include/module_loader.h"
#include "../include/leno_serialize.h"
#include "../include/leno_error.h"
#include "../include/platform.h"
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#endif

// 来自 module_loader.c
extern int normalize_path(char* path, int max_len);

// 初始容量
#define INITIAL_CAPACITY 16

// ---- 模块符号表容量限制（命名常量，替代硬编码） ----
#define MOD_MAX_NAMES         64   // 标识符/名称缓冲区长度
#define MOD_MAX_TYPE_STR     256   // 类型字符串缓冲区长度
#define MOD_MAX_TYPES        128   // 单模块内最大 struct/cstruct/face 定义数
#define MOD_MAX_CLIBS         32   // 单模块内最大 clib 定义数
#define MOD_MAX_ENUMS         32   // 单模块内最大 enum 定义数
#define MOD_MAX_MEMBERS      128   // 单 enum 最大成员数
#define MOD_MAX_FIELDS       128   // 单 struct/cstruct 最大字段数
#define MOD_MAX_METHODS      128   // 单 struct/cstruct/face 最大方法数
#define MOD_MAX_PARAMS        64   // 单函数最大参数数
#define MOD_MAX_TYPE_PARAMS   16   // 单类型最大泛型参数数
#define MOD_MAX_ALIASES       64   // 单模块内最大 alias 定义数
#define MOD_MAX_CLIB_FUNCS   512   // 单 clib 最大函数数
#define MOD_MAX_GENERIC_RET   16   // 返回类型最大泛型参数数

// 创建/销毁
#include "inc/sym_table_create.inc"

// 添加/查找操作
#include "inc/sym_table_add.inc"

// 类型字符串解析器
#include "inc/sym_table_type_parse.inc"

// 导入别名类型依赖传导
#include "inc/sym_table_import_alias.inc"

// 符号表扫描（两遍扫描）
#include "inc/sym_table_scan.inc"

// 符号表缓存（.lenosymc 序列化/反序列化）
#include "inc/sym_table_cache.inc"

// 公开入口
#include "inc/sym_table_entry.inc"
