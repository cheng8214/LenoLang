#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform.h"
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
    gc_write_barrier((Object*)file, val_obj((Object*)path));
    file->mode = mode;
    gc_write_barrier((Object*)file, val_obj((Object*)mode));
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
    (void)argCount;
    if (!is_file_value(args[0])) {
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
        // ★ 同 native_files_read：不靠 fseek/ftell 的「文件大小」——/proc、/sys 这类伪文件
        //   上报 size=0，原实现直接返回空串 ✗ 改为从当前位置边读边扩容到 EOF ✓
        size_t cap = 4096, read_size = 0;
        char* buffer = (char*)malloc(cap + 1);
        if (!buffer) {
            native_throw_error("内存分配失败");
            return val_null();
        }
        for (;;) {
            if (read_size == cap) {
                cap *= 2;
                char* nb = (char*)realloc(buffer, cap + 1);
                if (!nb) { free(buffer); native_throw_error("内存分配失败"); return val_null(); }
                buffer = nb;
            }
            size_t n = fread(buffer + read_size, 1, cap - read_size, file->fp);
            read_size += n;
            if (n == 0) break;      // EOF（或读错误）
        }
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

        /* 确保 UTF-8 字符边界完整：若尾部截断了多字节序列，回退到上一个完整字符 */
        /* 注意：二进制模式（含'b'）时不做 UTF-8 处理，直接按原始字节读写 */
        if (read_size > 0 && !strchr(file->mode->chars, 'b')) {
            size_t safe_len = read_size;
            unsigned char last = (unsigned char)buffer[safe_len - 1];
            /* 尾字节(0x80-0xBF)说明多字节序列被截断，向前找到首字节 */
            if (last >= 0x80 && last <= 0xBF) {
                while (safe_len > 0 && (unsigned char)buffer[safe_len - 1] >= 0x80
                       && (unsigned char)buffer[safe_len - 1] <= 0xBF) {
                    safe_len--;
                }
                /* 首字节也去掉，这样剩余部分全是完整字符 */
                if (safe_len > 0 && (unsigned char)buffer[safe_len - 1] >= 0xC0) {
                    safe_len--;
                }
            }
            /* 回退多读的字节：让文件指针退到完整字符边界 */
            long rollback = (long)(read_size - safe_len);
            if (rollback > 0) fseek(file->fp, -rollback, SEEK_CUR);
            buffer[safe_len] = '\0';
            read_size = safe_len;
        }

        ObjString* result = str_copy(buffer, (int)read_size);
        free(buffer);
        return val_obj((Object*)result);
    }
}

// f.readline()
static Value file_method_readline(int argCount, Value* args) {
    (void)argCount;
    if (!is_file_value(args[0])) {
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
    (void)argCount;
    if (!is_file_value(args[0])) {
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
    (void)argCount;
    if (!is_file_value(args[0])) {
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
    (void)argCount;
    if (!is_file_value(args[0])) {
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
    (void)argCount;
    if (!is_file_value(args[0])) {
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
    (void)argCount;
    if (!is_file_value(args[0])) {
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
    (void)argCount;
    if (!is_file_value(args[0])) {
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
    (void)argCount;
    if (!is_file_value(args[0])) {
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
    (void)argCount;
    if (!is_file_value(args[0])) {
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
    (void)argCount;   // 个数由编译期把关（2026-10-01 实测：「参数数量不匹配」）⇒ 运行期不重复检查

    if (!is_string_value(args[0]) || !is_string_value(args[1])) {
        native_throw_error("open 参数必须是字符串");
        return val_null();
    }

    ObjString* path = (ObjString*)val_as_obj(args[0]);
    ObjString* mode = (ObjString*)val_as_obj(args[1]);

#ifdef _WIN32
    /* Windows: 使用 _wfopen 支持中文路径 */
    wchar_t* wpath = utf8_to_utf16(path->chars);
    wchar_t* wmode = utf8_to_utf16(mode->chars);
    FILE* fp = NULL;
    if (wpath && wmode) {
        fp = _wfopen(wpath, wmode);
    }
    free(wpath);
    free(wmode);
#else
    FILE* fp = fopen(path->chars, mode->chars);
#endif
    if (!fp) {
        char msg[128];
        snprintf(msg, sizeof(msg), "无法打开文件 '%s'", path->chars);
        native_throw_error(msg);
        return val_null();
    }

    ObjFile* file = file_new(fp, path, mode);
    return val_obj((Object*)file);
}

// files.exists(path)
static Value native_files_exists(int argCount, Value* args) {
    (void)argCount;   // 个数由编译期把关（2026-10-01 实测：「参数数量不匹配」）⇒ 运行期不重复检查

    if (!is_string_value(args[0])) {
        native_throw_error("exists 参数必须是字符串");
        return val_null();
    }

    ObjString* path = (ObjString*)val_as_obj(args[0]);
#ifdef _WIN32
    wchar_t* wpath = utf8_to_utf16(path->chars);
    FILE* fp = NULL;
    if (wpath) {
        fp = _wfopen(wpath, L"r");
    }
    free(wpath);
#else
    FILE* fp = fopen(path->chars, "r");
#endif
    if (fp) {
        fclose(fp);
        return val_bool(1);
    }
    return val_bool(0);
}

// files.delete(path)
static Value native_files_delete(int argCount, Value* args) {
    (void)argCount;   // 个数由编译期把关（2026-10-01 实测：「参数数量不匹配」）⇒ 运行期不重复检查

    if (!is_string_value(args[0])) {
        native_throw_error("delete 参数必须是字符串");
        return val_null();
    }

    ObjString* path = (ObjString*)val_as_obj(args[0]);
#ifdef _WIN32
    wchar_t* wpath = utf8_to_utf16(path->chars);
    int result = -1;
    if (wpath) {
        result = _wremove(wpath);
    }
    free(wpath);
#else
    int result = remove(path->chars);
#endif
    return val_bool(result == 0);
}

// files.read(path) - 快捷读取全部
static Value native_files_read(int argCount, Value* args) {
    (void)argCount;   // 个数由编译期把关（2026-10-01 实测：「参数数量不匹配」）⇒ 运行期不重复检查

    if (!is_string_value(args[0])) {
        native_throw_error("read 参数必须是字符串");
        return val_null();
    }

    ObjString* path = (ObjString*)val_as_obj(args[0]);
#ifdef _WIN32
    wchar_t* wpath = utf8_to_utf16(path->chars);
    FILE* fp = NULL;
    if (wpath) {
        fp = _wfopen(wpath, L"r");
    }
    free(wpath);
#else
    FILE* fp = fopen(path->chars, "r");
#endif
    if (!fp) {
        // 消息口径与 native_files_open 逐字一致（带路径）：此前这里只有"无法打开文件"四个字，
        //   调用点 catch 到的 e.msg 看不出是哪个文件 ⇒ 与 open 不一致（2026-09-30 对齐）。
        char msg[128];
        snprintf(msg, sizeof(msg), "无法打开文件 '%s'", path->chars);
        native_throw_error(msg);
        return val_null();
    }

    // ★ 不依赖 fseek/ftell 的「文件大小」：/proc、/sys 这类**伪文件**（以及管道）上报
    //   size=0，按 size 分配会把内容整个漏掉 —— 实测 files.read("/proc/self/stat") 恒为
    //   空串，连带 LenoSys 的 currentPid / listProcesses / processMemoryKb 在 Linux 全废 ✗
    //   改为边读边扩容直到 EOF（对普通文件行为不变 ✓）
    size_t cap = 4096, read_size = 0;
    char* buffer = (char*)malloc(cap + 1);
    if (!buffer) {
        fclose(fp);
        native_throw_error("内存分配失败");
        return val_null();
    }
    for (;;) {
        if (read_size == cap) {
            cap *= 2;
            char* nb = (char*)realloc(buffer, cap + 1);
            if (!nb) { free(buffer); fclose(fp); native_throw_error("内存分配失败"); return val_null(); }
            buffer = nb;
        }
        size_t n = fread(buffer + read_size, 1, cap - read_size, fp);
        read_size += n;
        if (n == 0) break;      // EOF（或读错误）
    }
    buffer[read_size] = '\0';
    fclose(fp);

    // ★ 剥掉 UTF-8 BOM（EF BB BF）——本函数是**文本语义**的快捷读取：
    //   带 BOM 的文件（Windows 记事本、PowerShell 的 `Set-Content -Encoding utf8` 产出）
    //   若原样带出 0xEF 0xBB 0xBF，正则 / 字符串比较 / 数字解析都会**静默失配** ✗
    //   （2026-10-01：examples\工具\lines_check.py 用 `encoding="utf-8-sig"` 正是为躲这个坑）
    //   要原始字节请走 `files.open(p, "rb").read()`（二进制模式**不**做任何处理 ✓）
    size_t skip = 0;
    if (read_size >= 3 && (unsigned char)buffer[0] == 0xEF
        && (unsigned char)buffer[1] == 0xBB && (unsigned char)buffer[2] == 0xBF) {
        skip = 3;
    }

    ObjString* result = str_copy(buffer + skip, (int)(read_size - skip));
    free(buffer);
    return val_obj((Object*)result);
}

// files.write(path, content)
static Value native_files_write(int argCount, Value* args) {
    (void)argCount;   // 个数由编译期把关（2026-10-01 实测：「参数数量不匹配」）⇒ 运行期不重复检查

    if (!is_string_value(args[0])) {
        native_throw_error("write 第一个参数必须是字符串路径");
        return val_null();
    }

    ObjString* path = (ObjString*)val_as_obj(args[0]);
    ObjString* content = value_to_objstring(args[1]);

#ifdef _WIN32
    wchar_t* wpath = utf8_to_utf16(path->chars);
    FILE* fp = NULL;
    if (wpath) {
        fp = _wfopen(wpath, L"w");
    }
    free(wpath);
#else
    FILE* fp = fopen(path->chars, "w");
#endif
    if (!fp) {
        // 同上：消息带路径，与 open / read 一致（此前只有"无法创建文件"四个字）。
        char msg[128];
        snprintf(msg, sizeof(msg), "无法创建文件 '%s'", path->chars);
        native_throw_error(msg);
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
    
    native_register_module_method_spec("files", "open", native_files_open, 2, -1, -1, &NATIVE_T_FILE, open_params);
    native_register_module_method_spec("files", "exists", native_files_exists, 1, -1, -1, &NATIVE_T_BOOL, string_params);
    native_register_module_method_spec("files", "delete", native_files_delete, 1, -1, -1, &NATIVE_T_BOOL, string_params);
    native_register_module_method_spec("files", "read", native_files_read, 1, -1, -1, &NATIVE_T_STRING, string_params);
    // 返回 `null`（v3.2.6）：实现是 `return val_null()`（只写盘、不产生值）—— 原来标 any，
    //   连"失败时会 throw"这一条都看不出来；标 null 后 `var x = files.write(...)` 才是诚实推断。
    native_register_module_method_spec("files", "write", native_files_write, 2, -1, -1, &NATIVE_T_NULL, string2_params);

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
    // 返回 `Array[string]`（v3.2.6）：实现逐行 `str_copy` 装进数组 ⇒ 元素是 string，不是 any
    //   （原来注册成 `TYPE_ARRAY + TYPE_UNKNOWN`（裸 Array）⇒ `f.readlines()[0]` 是 any）。
    native_register_instance_method_return_spec("File", "readlines", &NATIVE_T_ARR_STRING);
    // f.write(string) - 用户可见 1 个参数，实际 2 个（含 receiver）
    TypeKind write_params[] = {TYPE_STRING};
    file_register_method_with_params("write", make_native(file_method_write, 2, "write"), 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, write_params);
    // f.writeln(string) - 用户可见 1 个参数，实际 2 个（含 receiver）
    file_register_method_with_params("writeln", make_native(file_method_writeln, 2, "writeln"), 1, -1, -1, TYPE_ANY, TYPE_UNKNOWN, write_params);
    // 返回 `null`（v3.2.6）：实现是 `return val_null()`（注意与 f.write() 不同 —— 那个返回写入字节数 int）
    native_register_instance_method_return_spec("File", "writeln", &NATIVE_T_NULL);
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
    // 返回 `null`（v3.2.6）：实现是 `return val_null()`
    native_register_instance_method_return_spec("File", "close", &NATIVE_T_NULL);
}
