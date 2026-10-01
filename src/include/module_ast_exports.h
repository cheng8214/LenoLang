/* 从 parser AST 提取模块顶层导出名（compiler-only）
 *
 * 定位：这是「符号表文本扫描链 → parser AST」迁移的**第一步 —— 对拍基准**。
 *   终局是 `module_symbol_table` 那套 ~6800 行文本扫描器改从 AST 提取
 *   （docs/待办_单一事实来源与重复实现收敛.md:2719-2723 的 S10"下一步"）。
 *   在迁移过程中，这个函数用来证明"两条路径给出的导出名集合是否一致"。
 *
 * ⚠ 为什么必须放在 compiler 专属文件（不能进 module_symbol_table.c / module_loader.c）：
 *   那两个文件在 `sources_core.txt`（**VM-only 也要编**），而 lexer/parser 只在
 *   `sources_compiler.txt` ⇒ 一旦它们在编译期依赖 AST，`build_vm.bat` 链接必炸
 *   （论证见 docs/待办_单一事实来源与重复实现收敛.md:2713-2717）。
 *
 * 语义口径（与扫描链**逐字对齐**，否则对拍会给出假阳性）：
 *   · 只认**顶层** `export` 包裹的声明 —— AST 上就是父节点 kind == AST_EXPORT
 *     （扫描链侧对应 sym_table_scan.inc:53 的 `strncmp(p,"export",6)` 顶层闸门）；
 *   · 解构 `export var[T](a,b)` 贡献**每个**名字（scan_var.inc:75）；
 *   · `clib xxx { }` **不进**导出表（扫描链 12 处 add 里没有 clib）⇒ 这里也不收；
 *   · `export use` 不存在（parser_module.c:466-469 直接拒）。
 */
#ifndef LENO_MODULE_AST_EXPORTS_H
#define LENO_MODULE_AST_EXPORTS_H

// 提取结果：与 module_symbol_table_export_names 同形态（名字数组 + 个数）
typedef struct {
    char** names;     // 内部 strdup，需 module_ast_exports_free 释放
    int count;
    int capacity;
} AstExportList;

// 从**源码文本**提取顶层导出名。
//   返回 0 = 提取成功（out 已填充，可能 0 个）；-1 = 解析失败或入参非法。
//   顺序 = 源码里顶层声明的出现顺序（对拍时两边都排序后再比，避免顺序误判 ✓）。
int module_ast_collect_exports(const char* src, AstExportList* out);

void module_ast_exports_free(AstExportList* list);

// 对拍：给定一个 .leno 文件路径，分别用"符号表扫描链"与"parser AST"取导出名并比较。
//   退出码：0 = 一致；1 = **不一致**（已打印差异）；2 = 无法比较（读不了源码 / 语法解析失败）。
//   ⚠ 2 必须与 1 分开：`assert/error_col` 下是**故意的语法错误**用例，它们永远比不了，
//     混进"不一致"会让批量统计永远有噪音 ✗
//   verbose=0 时只在"不一致"时打印详情 ✓
int module_ast_export_diff_file(const char* path, int verbose);

#endif  // LENO_MODULE_AST_EXPORTS_H
