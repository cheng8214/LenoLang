#include "../include/leno_package.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

static int dir_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

static int create_dir(const char* path) {
    if (dir_exists(path)) return 0;
    return MKDIR(path);
}

static int file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int write_text_file(const char* path, const char* content) {
    FILE* fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s", content);
    fclose(fp);
    return 0;
}

/* 从路径中提取默认包名（取最后一段目录名） */
static const char* extract_pkg_name(const char* path) {
    if (!path || path[0] == '\0' || strcmp(path, ".") == 0) return "my-package";

    /* 去掉尾部分隔符 */
    size_t len = strlen(path);
    while (len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
        len--;
    }

    /* 找最后一个分隔符（支持 / \ 和 :） */
    const char* base = path;
    for (const char* p = path; p < path + len; p++) {
        if (*p == '/' || *p == '\\' || (*p == ':' && (p + 1) < (path + len) && 
            ((*(p + 1) == '\\') || (*(p + 1) == '/')))) {
            base = p + 1;
        }
    }

    return base;
}

/* ★ 2026-11-09：包名 → **合法标识符**（模块别名）。
 *
 * 为什么需要：`leno --init` 的默认包名是 `my-package`（带连字符），而模板里写的是
 *   `import "../lib/my-package.leno" as my-package` —— `my-package` 不是合法标识符，
 *   于是**生成的项目从来编译不过**（报「未定义的变量: package」）。
 *   这个问题在旧模板里就存在，只是没人用默认名跑过。
 *
 * 规则：非 [A-Za-z0-9_] 的字符换成 '_'；首字符是数字则前置 '_'。
 *   `my-package` → `my_package`；`2fa` → `_2fa`。 */
static const char* to_ident(const char* s, char* out, size_t out_size) {
    size_t j = 0;
    if (out_size == 0) return s;
    if (s && s[0] >= '0' && s[0] <= '9') {
        if (j + 1 < out_size) out[j++] = '_';
    }
    for (size_t i = 0; s && s[i] != '\0' && j + 1 < out_size; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out[j++] = ok ? c : '_';
    }
    out[j] = '\0';
    return out;
}

int package_init(const char* dir_path, const char* package_name) {
    const char* pkg_name = package_name;
    if (!pkg_name || pkg_name[0] == '\0') {
        pkg_name = extract_pkg_name(dir_path);
    }

    /* 模块别名必须是合法标识符（默认包名 `my-package` 带连字符 ⇒ 旧模板编不过） */
    char ident_buf[256];
    const char* pkg_ident = to_ident(pkg_name, ident_buf, sizeof(ident_buf));

    /* 创建目录 */
    if (create_dir(dir_path) != 0) {
        fprintf(stderr, "[leno init] 无法创建目录: %s\n", dir_path);
        return -1;
    }

    /* 创建子目录 */
    char buf[1024];

    snprintf(buf, sizeof(buf), "%s/lib", dir_path);
    create_dir(buf);

    snprintf(buf, sizeof(buf), "%s/src", dir_path);
    create_dir(buf);

    snprintf(buf, sizeof(buf), "%s/native", dir_path);
    create_dir(buf);

    snprintf(buf, sizeof(buf), "%s/examples", dir_path);
    create_dir(buf);

    snprintf(buf, sizeof(buf), "%s/test", dir_path);
    create_dir(buf);

    /* 创建 leno.toml */
    char toml[2048];
    snprintf(toml, sizeof(toml),
        "[package]\n"
        "name = \"%s\"\n"
        "version = \"0.1.0\"\n"
        "description = \"A Leno package\"\n"
        "authors = \"\"\n"
        "license = \"Apache-2.0\"\n"
        "leno_version = \">=0.1.0\"\n"
        "\n"
        "[dependencies]\n"
        "\n"
        "[dev-dependencies]\n"
        "\n"
        "[modules]\n"
        "root = \"lib\"\n",
        pkg_name);

    snprintf(buf, sizeof(buf), "%s/leno.toml", dir_path);
    if (file_exists(buf)) {
        fprintf(stderr, "[leno init] leno.toml 已存在，跳过创建\n");
    } else {
        write_text_file(buf, toml);
    }

    /* 创建示例模块 */
    snprintf(buf, sizeof(buf), "%s/lib/%s.leno", dir_path, pkg_name);
    if (!file_exists(buf)) {
        char sample[512];
        snprintf(sample, sizeof(sample),
            "// %s - 示例模块\n"
            "\n"
            "export func hello() {\n"
            "    print(\"Hello from %s!\")\n"
            "}\n",
            pkg_name, pkg_name);
        write_text_file(buf, sample);
    }

    /* 创建示例入口 —— ★ 2026-11-09：改成"5 种惯用写法"的可运行样例。
     *
     * 为什么改：原来的模板只有 `print("Hello from ...")`，新项目看不到 struct / enum /
     *   switch / map / Result **任何**一个样例。评估文档 §3.6 实测指出这是"0 使用"的
     *   结构性原因之一（生成侧没有任何机制把作者推向惯用写法）。
     *   这里的样例都实跑验证过（`leno run` 能出正确输出）。 */
    snprintf(buf, sizeof(buf), "%s/src/main.leno", dir_path);
    if (!file_exists(buf)) {
        char main_sample[8192];
        snprintf(main_sample, sizeof(main_sample),
            "// 入口文件 —— `leno run` 从这里开始\n"
            "//\n"
            "// 这份文件是**可运行的样例**，覆盖 5 种惯用写法（配套约定见 AGENTS.md）：\n"
            "//   ① struct + Array[struct]   —— 不要用 Array[Array[string]]\n"
            "//   ② enum 表达状态            —— 不要用魔法整数 / \"0\"..\"5\" 字符串码\n"
            "//   ③ switch 分派              —— >=3 个同值比较时用它，不要 if-else 链\n"
            "//   ④ map / filter             —— 不要手写\"空数组 + for + add\"\n"
            "//   ⑤ Result 形态              —— 标准库暂无，项目内先约定一种\n"
            "import \"../lib/%s.leno\" as %s\n"
            "\n"
            "// ==================== ① 数据形态 ====================\n"
            "struct Task {\n"
            "    string title = \"\"\n"
            "    int status = 0\n"
            "}\n"
            "\n"
            "// ==================== ② 状态：enum int 常量 ====================\n"
            "enum TaskStatus {\n"
            "    todo = 0\n"
            "    doing = 1\n"
            "    done = 2\n"
            "}\n"
            "\n"
            "// ==================== ③ 分支：switch ====================\n"
            "func status_name(int st): string {\n"
            "    switch st {\n"
            "        case TaskStatus.todo { return \"待办\" }\n"
            "        case TaskStatus.doing { return \"进行中\" }\n"
            "        case TaskStatus.done { return \"已完成\" }\n"
            "        default { return \"未知\" }\n"
            "    }\n"
            "}\n"
            "\n"
            "// ==================== ⑤ 错误：Result 形态 ====================\n"
            "struct Parsed {\n"
            "    bool ok = false\n"
            "    int value = 0\n"
            "    string error = \"\"\n"
            "}\n"
            "\n"
            "func parse_count(string s): Parsed {\n"
            "    Parsed r = new Parsed()\n"
            "    string t = s.trim()\n"
            "    if t == \"\" { r.error = \"空字符串\"; return r }\n"
            "    for t to c {\n"
            "        if c < \"0\" or c > \"9\" { r.error = \"不是纯数字：\" + t; return r }\n"
            "    }\n"
            "    r.ok = true\n"
            "    r.value = _int(t)\n"
            "    return r\n"
            "}\n"
            "\n"
            "main() {\n"
            "    %s.hello()\n"
            "\n"
            "    // ① / ② 用 struct 数组 + enum；改字段是**原地生效**的\n"
            "    Array[Task] tasks = []\n"
            "    Task a = new Task()\n"
            "    a.title = \"写个样例\"\n"
            "    a.status = TaskStatus.done\n"
            "    tasks.add(a)\n"
            "\n"
            "    Task b = new Task()\n"
            "    b.title = \"读 AGENTS.md\"\n"
            "    b.status = TaskStatus.todo\n"
            "    tasks.add(b)\n"
            "\n"
            "    tasks[0].title = \"写个可运行的样例\"   // 原地改，不需要\"取出-改-写回\"\n"
            "\n"
            "    // ④ filter + map 替代手写循环\n"
            "    Array[string] doneTitles = tasks.filter(func(Task t) { return t.status == TaskStatus.done })\n"
            "                                    .map(func(Task t) { return t.title })\n"
            "\n"
            "    print(\"\\n已完成 \" + _str(doneTitles.len()) + \" 项：\")\n"
            "    for doneTitles to t { print(\"  · \" + t) }\n"
            "\n"
            "    print(\"\\n全部任务：\")\n"
            "    for tasks to t { print(\"  [\" + status_name(t.status) + \"] \" + t.title) }\n"
            "\n"
            "    // ⑤ 用 Result 而不是让 _int 抛异常\n"
            "    print(\"\")\n"
            "    var good = parse_count(\"42\")\n"
            "    if good.ok { print(\"parse_count(\\\"42\\\") = \" + _str(good.value)) }\n"
            "    else { print(\"parse_count(\\\"42\\\") 失败：\" + good.error) }\n"
            "\n"
            "    var bad = parse_count(\"4x2\")\n"
            "    if bad.ok { print(\"parse_count(\\\"4x2\\\") = \" + _str(bad.value)) }\n"
            "    else { print(\"parse_count(\\\"4x2\\\") 失败：\" + bad.error) }\n"
            "}\n",
            pkg_name, pkg_ident, pkg_ident);
        write_text_file(buf, main_sample);
    }

    /* 创建项目级 AGENTS.md —— 给 AI / 新人的"默认写法"。
     *   评估文档 §3.6 实测：四个真实项目 + Leno 根目录此前 AGENTS.md 为 **0 个**，
     *   没有任何机制把作者推向惯用形态。新项目从第一步就该带上。 */
    snprintf(buf, sizeof(buf), "%s/AGENTS.md", dir_path);
    if (!file_exists(buf)) {
        char agents_sample[4096];
        snprintf(agents_sample, sizeof(agents_sample),
            "# AGENTS.md —— 本项目默认写法\n"
            "\n"
            "> 给 AI / 新加入的人：**照这 5 条写**。`src/main.leno` 里有可直接运行的样例。\n"
            "\n"
            "| 场景 | 用这个 | 不要 |\n"
            "|---|---|---|\n"
            "| 数据 | `struct` + `Array[struct]` | `Array[Array[string]]`、下标当字段名 |\n"
            "| 状态 | `enum { todo = 0 ... }` | 魔法整数、`\"0\"..\"5\"` 字符串码 |\n"
            "| 分支 | >=3 个同值比较用 `switch` | 长 `if - else if` 链 |\n"
            "| 集合变换 | `.map` / `.filter` / `.reduce` | `Array[x] out = []` + `for` + `add` |\n"
            "| 错误 | 项目内 `Result` struct（`ok` + `error`） | 到处 `throw` 字符串、裸用 `_int` |\n"
            "\n"
            "已经存在、不要手写的（标准库）：\n"
            "\n"
            "```leno\n"
            "strings.pad_start(_str(v), 2, \"0\")   // 补零（= 以前各项目手写的 pad2）\n"
            "strings.split(s, \",\")                // 切分\n"
            "strings.lines(s)                     // 按行切（认 \\r\\n）\n"
            "strings.fmt_size(1536)               // \"1 KB\"\n"
            "arr.sort(func(int a, int b) { return b - a })   // 自定义比较器（稳定）\n"
            "maths.pow_mod(base, exp, m)          // 模幂\n"
            "```\n"
            "\n"
            "自检：`leno run` · `leno run test/smoke.leno`\n");
        write_text_file(buf, agents_sample);
    }

    /* 创建冒烟测试 —— 让 test/ 不留空，且演示"没有测试框架时怎么写断言"。 */
    snprintf(buf, sizeof(buf), "%s/test/smoke.leno", dir_path);
    if (!file_exists(buf)) {
        char smoke_sample[2048];
        snprintf(smoke_sample, sizeof(smoke_sample),
            "// 冒烟测试：`leno run test/smoke.leno`\n"
            "//\n"
            "// 标准库暂无测试框架 ⇒ 直接用内建 `assert_eq` / `assert_true`（失败会报错并中断）。\n"
            "// 新加功能时照这个形状加断言即可。\n"
            "import strings\n"
            "import \"../lib/%s.leno\" as %s\n"
            "\n"
            "main() {\n"
            "    // ① 模块能 import（包路径解析正常）\n"
            "    %s.hello()\n"
            "\n"
            "    // ② 标准库可用\n"
            "    assert_eq(strings.pad_start(\"7\", 2, \"0\"), \"07\", \"pad_start\")\n"
            "    assert_eq(strings.split(\"a,b\", \",\").len(), 2, \"split\")\n"
            "\n"
            "    // ③ 数组方法可用\n"
            "    assert_eq([3, 1, 2].sort().join(\",\"), \"1,2,3\", \"sort + join\")\n"
            "    assert_eq([1, 2, 3].filter(func(int x) { return x > 1 }).len(), 2, \"filter\")\n"
            "\n"
            "    print(\"[PASS] smoke\\n\")\n"
            "}\n",
            pkg_name, pkg_ident, pkg_ident);
        write_text_file(buf, smoke_sample);
    }

    /* 创建 .gitignore */
    snprintf(buf, sizeof(buf), "%s/.gitignore", dir_path);
    if (!file_exists(buf)) {
        const char* gitignore =
            "# Leno\n"
            "*.lenb\n"
            "leno.lock\n"
            "\n"
            "# 系统\n"
            ".DS_Store\n"
            "Thumbs.db\n";
        write_text_file(buf, gitignore);
    }

    printf("[leno init] 包 '%s' 创建成功！\n", pkg_name);
    printf("  目录: %s\n", dir_path);
    printf("  文件: leno.toml, lib/%s.leno, src/main.leno\n", pkg_name);
    printf("        AGENTS.md（默认写法约定）, test/smoke.leno（冒烟测试）\n");
    printf("  模块别名: %s（包名 %s 含非法字符时用下划线替换）\n", pkg_ident, pkg_name);
    printf("\n");
    printf("  运行: leno src/main.leno\n");

    return 0;
}
