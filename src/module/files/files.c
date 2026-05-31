#include "include/lenolang.h"
#include "include/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ==================== 文件对象操作 ====================

// 创建文件对象
static ObjFile* file_new(FILE* fp, ObjString* path, ObjString* mode) {
    ObjFile* file = (ObjFile*)gc_alloc(sizeof(ObjFile), OBJ_FILE);
    if (!file) return NULL;

    file->fp = fp;
    file->path = path;
    file->mode = mode;
    file->is_closed = 0;
    return file;
}

// ==================== 静态辅助函数 ====================

// 检查值是否是文件对象
static int is_file_value(Value value) {
    return val_is_obj(value) && val_as_obj(value)->type == OBJ_FILE;
}

static int is_string_value(Value value) {
    return val_is_obj(value) && val_as_obj(value)->type == OBJ_STRING;
}

static ObjString* value_to_objstring(Value value) {
    if (is_string_value(value)) {
        return (ObjString*)val_as_obj(value);
    }
    char* temp = value_to_string(value);
    ObjString* result = str_copy(temp, (int)strlen(temp));
    free(temp);
    return result;
}

// ==================== 文件方法 ====================

// f.read() 或 f.read(n)
static Value file_method_read(int argCount, Value* args) {
    if (argCount < 1 || !is_file_value(args[0])) {
        native_throw_error("read 方法需要文件对象作为 receiver");
        return val_null();
    }

    ObjFile* file = (ObjFile*)val_as_obj(args[0]);
    if (file->is_closed || !file->fp) {
        native_throw_error("无法读取行");
        return val_null();
    }

    // 检查模式是否允许读取
    if (strchr(file->mode->chars, 'w') && !strchr(file->mode->chars, '+')) {
        native_throw_error("文件以写入模式打开，无法读取");
        return val_null();
    }

    if (argCount == 1) {
        // 读取全部内容
        long current = ftell(file->fp);
        fseek(file->fp, 0, SEEK_END);
        long size = ftell(file->fp);
        fseek(file->fp, current, SEEK_SET);

        // 计算剩余可读字节数
        long remaining = size - current;
        if (remaining <= 0) {
            return val_obj((Object*)str_copy("", 0));
        }

        char* buffer = (char*)malloc(remaining + 1);
        if (!buffer) {
            native_throw_error("内存分配失败");
            return val_null();
        }

        size_t read_size = fread(buffer, 1, remaining, file->fp);
        buffer[read_size] = '\0';

        ObjString* result = str_copy(buffer, (int)read_size);
        free(buffer);
        return val_obj((Object*)result);
    } else {
        // 读取指定字节数
        if (!val_is_num(args[1])) {
            native_throw_error("读取字节数必须是数字");
            return val_null();
        }
        int n = (int)value_to_double(args[1]);
        if (n <= 0) {
            native_throw_error("读取字节数必须大于0");
            return val_null();
        }

        char* buffer = (char*)malloc(n + 1);
        if (!buffer) {
            native_throw_error("内存分配失败");
            return val_null();
        }

        size_t read_size = fread(buffer, 1, n, file->fp);
        buffer[read_size] = '\0';

        ObjString* result = str_copy(buffer, (int)read_size);
        free(buffer);
        return val_obj((Object*)result);
    }
}

// f.readline()
static Value file_method_readline(int argCount, Value* args) {
    if (argCount < 1 || !is_file_value(args[0])) {
        native_throw_error("readline 方法需要文件对象作为 receiver");
        return val_null();
    }

    ObjFile* file = (ObjFile*)val_as_obj(args[0]);
    if (file->is_closed || !file->fp) {
        native_throw_error("无法读取已关闭的文件");
        return val_null();
    }

    // 动态缓冲区读取一行
    int capacity = 256;
    int count = 0;
    char* buffer = (char*)malloc(capacity);
    if (!buffer) {
        native_throw_error("内存分配失败");
        return val_null();
    }

    int c;
    while ((c = fgetc(file->fp)) != EOF && c != '\n') {
        if (count + 1 >= capacity) {
            capacity *= 2;
            char* new_buffer = (char*)realloc(buffer, capacity);
            if (!new_buffer) {
                free(buffer);
                native_throw_error("内存分配失败");
                return val_null();
            }
            buffer = new_buffer;
        }
        buffer[count++] = (char)c;
    }

    buffer[count] = '\0';
    ObjString* result = str_copy(buffer, count);
    free(buffer);
    return val_obj((Object*)result);
}

// f.readlines()
static Value file_method_readlines(int argCount, Value* args) {
    if (argCount < 1 || !is_file_value(args[0])) {
        native_throw_error("readlines 方法需要文件对象作为 receiver");
        return val_null();
    }

    ObjFile* file = (ObjFile*)val_as_obj(args[0]);
    if (file->is_closed || !file->fp) {
        native_throw_error("无法读取已关闭的文件");
        return val_null();
    }

    // 先回到文件开头
    fseek(file->fp, 0, SEEK_SET);

    ObjArray* lines = arr_new(16);
    char buffer[BUFFER_XXLARGE];

    while (fgets(buffer, sizeof(buffer), file->fp)) {
        // 去掉末尾的换行符
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
            len--;
        }
        if (len > 0 && buffer[len - 1] == '\r') {
            buffer[len - 1] = '\0';
            len--;
        }

        // 扩容检查
        if (lines->count >= lines->capacity) {
            arr_grow(lines);
        }

        lines->elements[lines->count++] = val_obj((Object*)str_copy(buffer, (int)len));
        gc_write_barrier((Object*)lines, lines->elements[lines->count - 1]);
    }

    return val_obj((Object*)lines);
}

// f.write(string)
static Value file_method_write(int argCount, Value* args) {
    if (argCount < 2 || !is_file_value(args[0])) {
        native_throw_error("write 方法需要文件对象作为 receiver");
        return val_null();
    }

    ObjFile* file = (ObjFile*)val_as_obj(args[0]);
    if (file->is_closed || !file->fp) {
        native_throw_error("无法写入已关闭的文件");
        return val_null();
    }

    // 获取要写入的字符串
    ObjString* str = value_to_objstring(args[1]);

    size_t written = fwrite(str->chars, 1, str->len, file->fp);
    return val_int((int)written);
}

// f.writeln(string)
static Value file_method_writeln(int argCount, Value* args) {
    if (argCount < 2 || !is_file_value(args[0])) {
        native_throw_error("writeln 方法需要文件对象作为 receiver");
        return val_null();
    }

    ObjFile* file = (ObjFile*)val_as_obj(args[0]);
    if (file->is_closed || !file->fp) {
        native_throw_error("无法写入已关闭的文件");
        return val_null();
    }

    // 获取要写入的字符串
    ObjString* str = value_to_objstring(args[1]);

    fwrite(str->chars, 1, str->len, file->fp);
    fwrite("\n", 1, 1, file->fp);

    return val_null();
}

// f.seek(pos, whence="set")
static Value file_method_seek(int argCount, Value* args) {
    if (argCount < 2 || !is_file_value(args[0])) {
        native_throw_error("seek 方法需要文件对象作为 receiver");
        return val_null();
    }

    ObjFile* file = (ObjFile*)val_as_obj(args[0]);
    if (file->is_closed || !file->fp) {
        native_throw_error("无法定位已关闭的文件");
        return val_null();
    }

    if (!val_is_num(args[1])) {
        native_throw_error("seek 位置必须是数字");
        return val_null();
    }
    int pos = (int)value_to_double(args[1]);
    int whence = SEEK_SET;  // 默认从头开始

    if (argCount >= 3 && is_string_value(args[2])) {
        ObjString* whence_str = (ObjString*)val_as_obj(args[2]);
        if (strcmp(whence_str->chars, "set") == 0) {
            whence = SEEK_SET;
        } else if (strcmp(whence_str->chars, "cur") == 0) {
            whence = SEEK_CUR;
        } else if (strcmp(whence_str->chars, "end") == 0) {
            whence = SEEK_END;
        }
    }

    int result = fseek(file->fp, pos, whence);
    return val_int(result);
}

// f.tell()
static Value file_method_tell(int argCount, Value* args) {
    if (argCount < 1 || !is_file_value(args[0])) {
        native_throw_error("tell 方法需要文件对象作为 receiver");
        return val_null();
    }

    ObjFile* file = (ObjFile*)val_as_obj(args[0]);
    if (file->is_closed || !file->fp) {
        native_throw_error("无法获取已关闭文件的位置");
        return val_null();
    }

    long pos = ftell(file->fp);
    return val_int((int)pos);
}

// f.len()
static Value file_method_len(int argCount, Value* args) {
    if (argCount < 1 || !is_file_value(args[0])) {
        native_throw_error("len 方法需要文件对象作为 receiver");
        return val_null();
    }

    ObjFile* file = (ObjFile*)val_as_obj(args[0]);
    if (file->is_closed || !file->fp) {
        native_throw_error("无法获取已关闭文件的大小");
        return val_null();
    }

    long current = ftell(file->fp);
    fseek(file->fp, 0, SEEK_END);
    long size = ftell(file->fp);
    fseek(file->fp, current, SEEK_SET);

    return val_int((int)size);
}

// f.eof()
static Value file_method_eof(int argCount, Value* args) {
    if (argCount < 1 || !is_file_value(args[0])) {
        native_throw_error("eof 方法需要文件对象作为 receiver");
        return val_null();
    }

    ObjFile* file = (ObjFile*)val_as_obj(args[0]);
    if (file->is_closed || !file->fp) {
        return val_bool(1);
    }

    return val_bool(feof(file->fp) != 0);
}

// f.close()
static Value file_method_close(int argCount, Value* args) {
    if (argCount < 1 || !is_file_value(args[0])) {
        native_throw_error("close 方法需要文件对象作为 receiver");
        return val_null();
    }

    ObjFile* file = (ObjFile*)val_as_obj(args[0]);
    if (!file->is_closed && file->fp) {
        fclose(file->fp);
        file->fp = NULL;
        file->is_closed = 1;
    }

    return val_null();
}

// ==================== 模块静态方法 ====================

// files.open(path, mode)
static Value native_files_open(int argCount, Value* args) {
    if (argCount < 2) {
        native_throw_error("open 需要路径和模式参数");
        return val_null();
    }

    if (!is_string_value(args[0]) || !is_string_value(args[1])) {
        native_throw_error("open 参数必须是字符串");
        return val_null();
    }

    ObjString* path = (ObjString*)val_as_obj(args[0]);
    ObjString* mode = (ObjString*)val_as_obj(args[1]);

    FILE* fp = fopen(path->chars, mode->chars);
    if (!fp) {
        native_throw_error("无法打开文件");
        return val_null();
    }

    ObjFile* file = file_new(fp, path, mode);
    return val_obj((Object*)file);
}

// files.exists(path)
static Value native_files_exists(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("exists 需要路径参数");
        return val_null();
    }

    if (!is_string_value(args[0])) {
        native_throw_error("exists 参数必须是字符串");
        return val_null();
    }

    ObjString* path = (ObjString*)val_as_obj(args[0]);
    FILE* fp = fopen(path->chars, "r");
    if (fp) {
        fclose(fp);
        return val_bool(1);
    }
    return val_bool(0);
}

// files.delete(path)
static Value native_files_delete(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("delete 需要路径参数");
        return val_null();
    }

    if (!is_string_value(args[0])) {
        native_throw_error("delete 参数必须是字符串");
        return val_null();
    }

    ObjString* path = (ObjString*)val_as_obj(args[0]);
    int result = remove(path->chars);
    return val_bool(result == 0);
}

// files.read(path) - 快捷读取全部
static Value native_files_read(int argCount, Value* args) {
    if (argCount < 1) {
        native_throw_error("read 需要路径参数");
        return val_null();
    }

    if (!is_string_value(args[0])) {
        native_throw_error("read 参数必须是字符串");
        return val_null();
    }

    ObjString* path = (ObjString*)val_as_obj(args[0]);
    FILE* fp = fopen(path->chars, "r");
    if (!fp) {
        native_throw_error("无法打开文件");
        return val_null();
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        fclose(fp);
        native_throw_error("内存分配失败");
        return val_null();
    }

    size_t read_size = fread(buffer, 1, size, fp);
    buffer[read_size] = '\0';
    fclose(fp);

    ObjString* result = str_copy(buffer, (int)read_size);
    free(buffer);
    return val_obj((Object*)result);
}

// files.write(path, content)
static Value native_files_write(int argCount, Value* args) {
    if (argCount < 2) {
        native_throw_error("write 需要路径和内容参数");
        return val_null();
    }

    if (!is_string_value(args[0])) {
        native_throw_error("write 第一个参数必须是字符串路径");
        return val_null();
    }

    ObjString* path = (ObjString*)val_as_obj(args[0]);
    ObjString* content = value_to_objstring(args[1]);

    FILE* fp = fopen(path->chars, "w");
    if (!fp) {
        native_throw_error("无法创建文件");
        return val_null();
    }

    fwrite(content->chars, 1, content->len, fp);
    fclose(fp);

    return val_null();
}

// 外部声明：文件方法注册函数
extern void file_register_method_with_params(const char* name, ObjNative* method, int arity,
                                              int min_arity, int max_arity,
                                              TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
// 外部声明：创建原生函数对象的辅助函数
extern ObjNative* make_native(NativeFn fn, int arity, const char* name);

// 前向声明：文件实例方法初始化
void files_init_instance_methods(void);

// ==================== 初始化 ====================

void files_init_module(void) {
    // 注册静态方法
    // files.open 返回 TYPE_FILE 类型，让编译器知道变量类型
    TypeKind open_params[] = {TYPE_STRING, TYPE_STRING};
    TypeKind string_params[] = {TYPE_STRING};
    TypeKind string2_params[] = {TYPE_STRING, TYPE_STRING};
    
    native_register_module_method("files", "open", native_files_open, 2, -1, -1, TYPE_FILE, TYPE_UNKNOWN, open_params);
    native_register_module_method("files", "exists", native_files_exists, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, string_params);
    native_register_module_method("files", "delete", native_files_delete, 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, string_params);
    native_register_module_method("files", "read", native_files_read, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, string_params);
    native_register_module_method("files", "write", native_files_write, 2, -1, -1, TYPE_ANY, TYPE_UNKNOWN, string2_params);

    // 调用 files_init_instance_methods 注册文件实例方法
    files_init_instance_methods();
}

// 仅用于编译期注册实例方法元信息（由 native_register_all_instance_method_metas 调用）
void files_init_instance_methods(void) {
    // 初始化文件方法表
    file_init_methods();

    // 注册文件实例方法元信息（同时注册运行时方法和编译期元信息）
    // 注意：make_native 的 arity 需要包含 receiver（+1）
    // f.read() / f.read(n) - 用户可见 0 或 1 个参数，实际 1 或 2 个（含 receiver）
    TypeKind read_params[] = {TYPE_INT};
    file_register_method_with_params("read", make_native(file_method_read, -1, "read"), -1, 0, 1, TYPE_STRING, TYPE_UNKNOWN, read_params);
    // f.readline() - 用户可见 0 个参数，实际 1 个（含 receiver）
    TypeKind no_params[] = {};
    file_register_method_with_params("readline", make_native(file_method_readline, 1, "readline"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, no_params);
    // f.readlines() - 用户可见 0 个参数，实际 1 个（含 receiver）
    file_register_method_with_params("readlines", make_native(file_method_readlines, 1, "readlines"), 0, -1, -1, TYPE_ARRAY, TYPE_UNKNOWN, no_params);
    // f.write(string) - 用户可见 1 个参数，实际 2 个（含 receiver）
    TypeKind write_params[] = {TYPE_STRING};
    file_register_method_with_params("write", make_native(file_method_write, 2, "write"), 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, write_params);
    // f.writeln(string) - 用户可见 1 个参数，实际 2 个（含 receiver）
    file_register_method_with_params("writeln", make_native(file_method_writeln, 2, "writeln"), 1, -1, -1, TYPE_ANY, TYPE_UNKNOWN, write_params);
    // f.seek(pos, whence) - 用户可见 1 或 2 个参数，实际 2 或 3 个（含 receiver）
    TypeKind seek_params[] = {TYPE_INT, TYPE_STRING};
    file_register_method_with_params("seek", make_native(file_method_seek, -1, "seek"), -1, 1, 2, TYPE_INT, TYPE_UNKNOWN, seek_params);
    // f.tell() - 用户可见 0 个参数，实际 1 个（含 receiver）
    file_register_method_with_params("tell", make_native(file_method_tell, 1, "tell"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    // f.len() - 用户可见 0 个参数，实际 1 个（含 receiver）
    file_register_method_with_params("len", make_native(file_method_len, 1, "len"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, no_params);
    // f.eof() - 用户可见 0 个参数，实际 1 个（含 receiver）
    file_register_method_with_params("eof", make_native(file_method_eof, 1, "eof"), 0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, no_params);
    // f.close() - 用户可见 0 个参数，实际 1 个（含 receiver）
    file_register_method_with_params("close", make_native(file_method_close, 1, "close"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, no_params);
}
