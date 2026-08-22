#include "semantic_internal.h"

// ============================================================================
// 类型推断工具函数
// ============================================================================

// 检查方法名是否是数组元素修改方法
// 返回：1 = 是，0 = 否
int type_utils_is_array_element_mutator(const char* method_name) {
    return (strcmp(method_name, "add") == 0 ||
            strcmp(method_name, "insert") == 0);
}

// 获取数组元素修改方法中，元素参数的位置
// 参数：
//   method_name - 方法名
//   is_module_call - 是否是模块调用（arrays.add）还是实例调用（arr.add）
// 返回：元素参数的索引，-1 表示不是元素修改方法
int type_utils_get_array_element_param_index(const char* method_name, int is_module_call) {
    if (strcmp(method_name, "add") == 0) {
        // add(arr, value) 或 arr.add(value)
        // value 是最后一个参数
        return is_module_call ? 1 : 0;
    }
    if (strcmp(method_name, "insert") == 0) {
        // insert(arr, index, value) 或 arr.insert(index, value)
        // value 是最后一个参数
        return is_module_call ? 2 : 1;
    }
    return -1;
}

// 尝试更新空数组的元素类型
// 当向 Array（元素类型未指定）添加第一个具体类型元素时，更新数组类型
// 注意：Array[any]（明确指定为 any）不会更新，保持 any 类型
//
// 参数：
//   s - 语义分析器
//   arr_sym - 数组变量符号
//   elem_type - 要设置的元素类型
// 返回：
//   1 = 更新了类型，0 = 未更新（数组已有具体类型或不是数组）
int type_utils_try_update_array_element_type(Symbol* arr_sym, TypeInfo* elem_type) {
    if (!arr_sym || !arr_sym->type || arr_sym->type->kind != TYPE_ARRAY) {
        return 0;
    }
    
    TypeInfo* current_elem = arr_sym->type->element_type;
    
    // 只有当前元素类型为空（未指定）时才更新
    // Array[any]（明确指定为 any）保持 any 类型，不更新
    if (!current_elem) {
        if (elem_type && elem_type->kind != TYPE_ANY) {
            arr_sym->type->element_type = type_copy(elem_type);
            return 1;
        }
    }
    
    return 0;
}

// 尝试更新嵌套数组的元素类型（递归版本）
// 用于 arr[0].add(1)、arr[0][0].add(1) 等任意层级嵌套情况
//
// 参数：
//   arr_sym - 外层数组变量符号
//   depth - 嵌套深度（arr[0] 是 1，arr[0][0] 是 2）
//   elem_type - 要设置的最内层元素类型
// 返回：
//   1 = 更新了类型，0 = 未更新
static int type_utils_try_update_nested_array_element_type_recursive(Symbol* arr_sym, int depth, TypeInfo* elem_type) {
    if (!arr_sym || !arr_sym->type || arr_sym->type->kind != TYPE_ARRAY) {
        return 0;
    }
    
    TypeInfo* current_type = arr_sym->type;
    
    // 根据深度逐层进入嵌套数组类型
    for (int i = 0; i < depth; i++) {
        if (!current_type->element_type || current_type->element_type->kind != TYPE_ARRAY) {
            return 0;
        }
        current_type = current_type->element_type;
    }
    
    // 现在 current_type 是最内层的数组类型，更新其元素类型
    TypeInfo* inner_elem = current_type->element_type;
    if (!inner_elem || inner_elem->kind == TYPE_ANY) {
        if (elem_type && elem_type->kind != TYPE_ANY) {
            if (current_type->element_type) {
                type_free(current_type->element_type);
            }
            current_type->element_type = type_copy(elem_type);
            return 1;
        }
    }
    
    return 0;
}

// 计算索引表达式的嵌套深度
// arr[0] -> 1, arr[0][0] -> 2, arr[0][0][0] -> 3
static int type_utils_get_index_depth(Ast* ast) {
    if (!ast || ast->kind != AST_INDEX) {
        return 0;
    }
    
    // 递归计算深度
    // 如果 obj 也是索引表达式，继续深入
    int parent_depth = type_utils_get_index_depth(ast->u.index.obj);
    
    // 检查当前索引是否是整数（数组访问）还是字符串（方法名）
    // 对于 arr[0][0].add() 这种情况，我们只计算整数索引
    if (ast->u.index.index && ast->u.index.index->kind == AST_NUM) {
        return parent_depth + 1;
    }
    
    return parent_depth;
}

// 尝试更新嵌套数组的元素类型（对外接口）
// 用于 arr[0].add(1) 这种情况，自动计算嵌套深度
//
// 参数：
//   arr_sym - 外层数组变量符号
//   index_ast - 索引表达式 AST（用于计算嵌套深度）
//   elem_type - 要设置的内部元素类型
// 返回：
//   1 = 更新了类型，0 = 未更新
int type_utils_try_update_nested_array_element_type_ex(Symbol* arr_sym, Ast* index_ast, TypeInfo* elem_type) {
    int depth = type_utils_get_index_depth(index_ast);
    if (depth <= 0) {
        return 0;
    }
    return type_utils_try_update_nested_array_element_type_recursive(arr_sym, depth, elem_type);
}

// 旧的接口，保持兼容性（只处理一层嵌套）
int type_utils_try_update_nested_array_element_type(Symbol* arr_sym, TypeInfo* elem_type) {
    return type_utils_try_update_nested_array_element_type_recursive(arr_sym, 1, elem_type);
}

// 从 AST 节点解析变量符号
// 支持：AST_VAR（变量名）、AST_INDEX（数组索引，返回数组变量）
// 返回：符号指针，未找到返回 NULL
Symbol* type_utils_resolve_var_symbol(Semantic* s, Ast* ast) {
    if (!ast) return NULL;
    
    if (ast->kind == AST_VAR) {
        return scope_resolve(s->current, ast->u.var.name);
    }
    
    // 对于索引表达式 arr[0]，返回 arr 的符号
    if (ast->kind == AST_INDEX) {
        return type_utils_resolve_var_symbol(s, ast->u.index.obj);
    }
    
    return NULL;
}

// ============================================================================
// 字典类型检查工具函数
// ============================================================================

// 检查方法名是否是字典元素修改方法
// 返回：1 = 是，0 = 否
int type_utils_is_dict_element_mutator(const char* method_name) {
    return (strcmp(method_name, "set") == 0);
}



// 获取字典元素修改方法中，元素参数的位置
// 参数：
//   method_name - 方法名
// 返回：元素参数的索引，-1 表示不是元素修改方法
int type_utils_get_dict_element_param_index(const char* method_name) {
    if (strcmp(method_name, "set") == 0) {
        // set(key, value) - value 是第2个参数，索引1
        return 1;
    }
    return -1;
}

// 尝试更新空字典的值类型
// 当向 Dict（值类型未指定）添加第一个具体类型值时，更新字典值类型
// 注意：Dict[string, any]（明确指定为 any）保持 any 类型
//
// 参数：
//   dict_sym - 字典变量符号
//   value_type - 要设置的值类型
// 返回：
//   1 = 更新了类型，0 = 未更新（字典已有具体类型或不是字典）
int type_utils_try_update_dict_value_type(Symbol* dict_sym, TypeInfo* value_type) {
    if (!dict_sym || !dict_sym->type || dict_sym->type->kind != TYPE_DICT) {
        return 0;
    }
    
    TypeInfo* current_value = dict_sym->type->value_type;
    
    // 只有当前值类型为空（未指定）时才更新
    // Dict[string, any]（明确指定为 any）保持 any 类型，不更新
    if (!current_value) {
        if (value_type && value_type->kind != TYPE_ANY) {
            dict_sym->type->value_type = type_copy(value_type);
            return 1;
        }
    }

    return 0;
}

// ============================================================================
// 安全格式化类型错误信息（避免 type_to_string 缓冲区覆盖）
// ============================================================================

// 安全格式化类型错误信息
// 参数：
//   buf - 输出缓冲区
//   buf_size - 缓冲区大小
//   fmt - 格式字符串，支持 %s 占位符（会被替换为 type1/type2/str1/str2）
//   type1, type2 - 要格式化的类型（可为 NULL）
//   str1, str2 - 额外的字符串参数（可为 NULL）
void format_type_error(char* buf, size_t buf_size, const char* fmt,
                       TypeInfo* type1, TypeInfo* type2,
                       const char* str1, const char* str2) {
    char type1_buf[128] = "";
    char type2_buf[128] = "";

    // 先保存 type1 的字符串表示
    if (type1) {
        const char* type1_str = type_to_string(type1);
        strncpy(type1_buf, type1_str, sizeof(type1_buf) - 1);
        type1_buf[sizeof(type1_buf) - 1] = '\0';
    }

    // 再获取 type2 的字符串表示（避免覆盖 type1 的缓冲区）
    if (type2) {
        const char* type2_str = type_to_string(type2);
        strncpy(type2_buf, type2_str, sizeof(type2_buf) - 1);
        type2_buf[sizeof(type2_buf) - 1] = '\0';
    }

    // 使用 snprintf 格式化输出
    // 简单的占位符替换：%s1 -> type1_buf, %s2 -> type2_buf, %s3 -> str1, %s4 -> str2
    const char* p = fmt;
    size_t offset = 0;
    buf[0] = '\0';

    while (*p && offset < buf_size - 1) {
        if (*p == '%' && *(p + 1) == 's') {
            // 检查是否有数字后缀
            if (*(p + 2) == '1') {
                // %s1 -> type1_buf
                size_t len = strlen(type1_buf);
                if (offset + len < buf_size - 1) {
                    memcpy(buf + offset, type1_buf, len);
                    offset += len;
                }
                p += 3;
            } else if (*(p + 2) == '2') {
                // %s2 -> type2_buf
                size_t len = strlen(type2_buf);
                if (offset + len < buf_size - 1) {
                    memcpy(buf + offset, type2_buf, len);
                    offset += len;
                }
                p += 3;
            } else if (*(p + 2) == '3') {
                // %s3 -> str1
                if (str1) {
                    size_t len = strlen(str1);
                    if (offset + len < buf_size - 1) {
                        memcpy(buf + offset, str1, len);
                        offset += len;
                    }
                }
                p += 3;
            } else if (*(p + 2) == '4') {
                // %s4 -> str2
                if (str2) {
                    size_t len = strlen(str2);
                    if (offset + len < buf_size - 1) {
                        memcpy(buf + offset, str2, len);
                        offset += len;
                    }
                }
                p += 3;
            } else {
                // 普通的 %s，使用 type1_buf
                size_t len = strlen(type1_buf);
                if (offset + len < buf_size - 1) {
                    memcpy(buf + offset, type1_buf, len);
                    offset += len;
                }
                p += 2;
            }
        } else {
            buf[offset++] = *p++;
        }
    }
    buf[offset] = '\0';
}

// ============================================================================
// 生成详细的类型错误提示（包含转换建议）
// ============================================================================

// 获取类型转换建议
// 根据期望类型和实际类型，返回转换建议字符串
// 简单编辑距离（Levenshtein），用于拼写建议
static int levenshtein(const char* a, const char* b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 50 || lb > 50) return 999;
    int d[51][51];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++)
            d[i][j] = (a[i-1] == b[j-1]) ? d[i-1][j-1]
                     : 1 + ((d[i-1][j] < d[i][j-1]) ?
                        (d[i-1][j] < d[i-1][j-1] ? d[i-1][j] : d[i-1][j-1]) :
                        (d[i][j-1] < d[i-1][j-1] ? d[i][j-1] : d[i-1][j-1]));
    return d[la][lb];
}

// 在当前作用域查找最相似的变量名，返回提示字符串（静态缓冲区）
const char* get_similar_name_hint(Scope* scope, const char* name) {
    static char hint[256];
    hint[0] = '\0';
    if (!scope || !name || !name[0]) return hint;

    const char* best = NULL;
    int best_dist = 3;  // 最多允许 3 个编辑距离

    for (Scope* s = scope; s; s = s->parent) {
        for (int i = 0; i < s->sym_cnt; i++) {
            Symbol* sym = s->syms[i];
            if (!sym || !sym->name) continue;
            int dist = levenshtein(name, sym->name);
            if (dist < best_dist) {
                best_dist = dist;
                best = sym->name;
                if (dist == 0) break;
            }
        }
        if (best_dist == 0) break;
    }

    if (best && strcmp(best, name) != 0) {
        snprintf(hint, sizeof(hint), "\n  提示: 是否想输入 '%s'？", best);
    }
    return hint;
}

const char* get_type_conversion_hint(TypeKind expected, TypeKind actual) {
    // any 转具体类型
    if (actual == TYPE_ANY) {
        switch (expected) {
            case TYPE_INT: return "提示：使用 _int(value) 进行显式转换";
            case TYPE_FLOAT: return "提示：使用 _float(value) 进行显式转换";
            case TYPE_STRING: return "提示：使用 _str(value) 进行显式转换";
            case TYPE_BOOL: return "提示：使用 _bool(value) 进行显式转换";
            default: return "提示：any 类型需要显式转换后才能赋值给具体类型变量";
        }
    }
    
    // float 转 int（需要截断）
    if (actual == TYPE_FLOAT && expected == TYPE_INT) {
        return "提示：float 转 int 会截断小数部分，使用 _int(value) 显式转换";
    }
    
    // int 转 float（自动升级，但这里报错说明可能需要显式处理）
    if (actual == TYPE_INT && expected == TYPE_FLOAT) {
        return "提示：int 可以自动升级为 float，检查是否有其他类型问题";
    }
    
    // string 转数值
    if (actual == TYPE_STRING) {
        if (expected == TYPE_INT) return "提示：字符串转 int 使用 _int(value)，失败会报错";
        if (expected == TYPE_FLOAT) return "提示：字符串转 float 使用 _float(value)，失败会报错";
    }
    
    // 数组类型不匹配
    if (expected == TYPE_ARRAY && actual == TYPE_ARRAY) {
        return "提示：数组类型是不变的，Array[int] 不能赋给 Array 或其他元素类型的数组";
    }
    
    // Dict 类型不匹配
    if (expected == TYPE_DICT && actual == TYPE_DICT) {
        return "提示：字典类型是不变的，确保键值类型完全匹配";
    }
    
    return NULL;  // 没有特定提示
}

// 生成详细的类型错误信息
// 参数：
//   buf - 输出缓冲区
//   buf_size - 缓冲区大小
//   expected - 期望类型
//   actual - 实际类型
//   context - 错误上下文（如"变量赋值"、"函数参数"等）
//   var_name - 相关变量名（可为NULL，用于在错误信息中显示具体变量名）
void format_detailed_type_error(char* buf, size_t buf_size,
                                TypeInfo* expected, TypeInfo* actual,
                                const char* context) {
    format_detailed_type_error_ex(buf, buf_size, expected, actual, context, NULL);
}

// 扩展版本：支持变量名
void format_detailed_type_error_ex(char* buf, size_t buf_size,
                                TypeInfo* expected, TypeInfo* actual,
                                const char* context, const char* var_name) {
    char expected_buf[128] = "";
    char actual_buf[128] = "";
    
    if (expected) {
        const char* str = type_to_string(expected);
        strncpy(expected_buf, str, sizeof(expected_buf) - 1);
        expected_buf[sizeof(expected_buf) - 1] = '\0';
    }
    
    if (actual) {
        const char* str = type_to_string(actual);
        strncpy(actual_buf, str, sizeof(actual_buf) - 1);
        actual_buf[sizeof(actual_buf) - 1] = '\0';
    }
    
    // 构建基础错误信息（包含变量名）
    int offset;
    if (var_name) {
        offset = snprintf(buf, buf_size, "类型错误：%s '%s'\n  期望类型: %s\n  实际类型: %s",
                          context ? context : "类型不匹配", var_name,
                          expected_buf, actual_buf);
    } else {
        offset = snprintf(buf, buf_size, "类型错误：%s\n  期望类型: %s\n  实际类型: %s",
                          context ? context : "类型不匹配",
                          expected_buf, actual_buf);
    }
    
    // 添加转换建议
    if (expected && actual) {
        const char* hint = get_type_conversion_hint(expected->kind, actual->kind);
        if (hint && (size_t)offset < buf_size - 1) {
            snprintf(buf + offset, buf_size - offset, "\n  %s", hint);
        }
    }
}

// ============================================================================
// 数组索引赋值类型检查工具函数
// ============================================================================

// 检查数组索引赋值的元素类型兼容性
// 参数：
//   obj_type - 数组对象的类型
//   value_type - 要赋值的类型
//   line - 行号（用于错误报告）
// 返回：
//   1 = 类型兼容，0 = 类型不兼容（已报告错误）
int type_utils_check_array_index_assignment(TypeInfo* obj_type, TypeInfo* value_type, int line, int column) {
    if (!obj_type || obj_type->kind != TYPE_ARRAY) {
        return 1;  // 不是数组，不检查
    }
    
    TypeInfo* elem_type = obj_type->element_type;
    if (!elem_type || elem_type->kind == TYPE_ANY) {
        return 1;  // 数组元素类型未指定或为 any，不检查
    }
    
    if (!value_type) {
        return 1;  // 无法推断类型，不检查
    }
    
    // any 不能赋值给具体类型（从不确定到确定需要显式转换）
    if (value_type->kind == TYPE_ANY) {
        char msg[BUFFER_MEDIUM];
        format_detailed_type_error(msg, sizeof(msg),
            elem_type, value_type, "数组元素类型不匹配");
        error_add_at(ERR_SEMANTIC, line, column, msg);
        return 0;
    }
    
    if (value_type->kind == elem_type->kind) {
        return 1;  // 类型相同，允许
    }
    
    // int 可以隐式转为 float，但 float 不能转为 int
    if (elem_type->kind == TYPE_FLOAT && value_type->kind == TYPE_INT) {
        return 1;  // int -> float 允许
    }
    
    // 类型不兼容，报告错误
    char msg[BUFFER_MEDIUM];
    format_detailed_type_error(msg, sizeof(msg),
        elem_type, value_type, "数组元素类型不匹配");
    error_add_at(ERR_SEMANTIC, line, column, msg);
    
    return 0;
}

// ============================================================================
// 字典索引赋值类型检查工具函数
// ============================================================================

// 检查字典索引赋值的值类型兼容性
// 参数：
//   dict_sym - 字典变量符号
//   assign_type - 要赋值的类型
//   line - 行号（用于错误报告）
// 返回：
//   1 = 类型兼容或已更新类型，0 = 类型不兼容（已报告错误）
int type_utils_check_dict_index_assignment(Symbol* dict_sym, TypeInfo* assign_type, int line, int column) {
    if (!dict_sym || !dict_sym->type || dict_sym->type->kind != TYPE_DICT) {
        return 1;  // 不是字典，不检查
    }
    
    if (!assign_type) {
        return 1;  // 无法推断类型，不检查
    }
    
    // 首先尝试更新字典值类型（如果是第一次赋值）
    int type_updated = type_utils_try_update_dict_value_type(dict_sym, assign_type);
    if (type_updated) {
        return 1;  // 类型已更新，不需要进一步检查
    }
    
    // 检查字典值类型
    TypeInfo* value_type = dict_sym->type->value_type;
    if (!value_type || value_type->kind == TYPE_ANY) {
        return 1;  // 字典值类型未指定或为 any，不检查
    }
    
    // any 不能赋值给具体类型（从不确定到确定需要显式转换）
    if (assign_type->kind == TYPE_ANY) {
        char msg[BUFFER_MEDIUM];
        format_detailed_type_error(msg, sizeof(msg),
            value_type, assign_type, "字典值类型不匹配");
        error_add_at(ERR_SEMANTIC, line, column, msg);
        return 0;
    }
    
    if (assign_type->kind == value_type->kind) {
        return 1;  // 类型相同，允许
    }

    // 允许 impl face 的 struct 赋给 face 类型的 Dict 值
    if (value_type->kind == TYPE_FACE && assign_type->kind == TYPE_STRUCT && assign_type->struct_name) {
        ObjFaceDef* fdef = face_def_find(value_type->struct_name);
        ObjStructDef* sdef = struct_def_find(assign_type->struct_name);
        if (fdef && sdef && struct_implements_face(sdef, fdef)) {
            return 1;
        }
    }

    // int 可以隐式转为 float
    if (value_type->kind == TYPE_FLOAT && assign_type->kind == TYPE_INT) {
        return 1;  // int -> float 允许
    }
    
    // 类型不兼容，报告错误
    char msg[BUFFER_MEDIUM];
    format_detailed_type_error(msg, sizeof(msg),
        value_type, assign_type, "字典值类型不匹配");
    error_add_at(ERR_SEMANTIC, line, column, msg);
    
    return 0;
}
