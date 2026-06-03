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

int package_init(const char* dir_path, const char* package_name) {
    const char* pkg_name = package_name;
    if (!pkg_name || pkg_name[0] == '\0') {
        pkg_name = extract_pkg_name(dir_path);
    }

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
        "license = \"MIT\"\n"
        "leno_version = \">=1.0.0\"\n"
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

    /* 创建示例入口 */
    snprintf(buf, sizeof(buf), "%s/src/main.leno", dir_path);
    if (!file_exists(buf)) {
        char main_sample[512];
        snprintf(main_sample, sizeof(main_sample),
            "// 入口文件\n"
            "import \"../lib/%s.leno\" as %s\n"
            "\n"
            "main() {\n"
            "    %s.hello()\n"
            "}\n",
            pkg_name, pkg_name, pkg_name);
        write_text_file(buf, main_sample);
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
    printf("\n");
    printf("  运行: leno src/main.leno\n");

    return 0;
}
