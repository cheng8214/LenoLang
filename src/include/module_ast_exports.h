/* parser AST → 模块符号表（compiler-only）
 *
 * 本文件现在承载两件**生产**功能（外加一批"把符号从 AST 填进符号表"的填充器）：
 *   ① 导出名提供者：`module_ast_exports_register()` 把"走 AST 的导出名解析"注册给
 *      `module_loader`，其 copy_module_export_names / module_has_method 都优先用它
 *      ⇒ **导出名的唯一来源是语法**（VM-only 不注册 ⇒ 回退扫描链）；
 *   ② AST 符号填充器：`module_ast_symbols_register()` —— 扫描链跑完后按声明种类把符号
 *      从 AST 建/覆盖进符号表，是 S10 逐类迁移的载体。
 *
 * ⚠ 必须放在 compiler 专属文件（不能进 module_symbol_table.c / module_loader.c）：
 *   那两个在 `sources_core.txt`（VM-only 也编），而 lexer/parser 只在 `sources_compiler.txt`
 *   ⇒ 一旦编译期依赖 AST，`build_vm.bat` 链接必炸
 *   （论证见 docs/待办_单一事实来源与重复实现收敛.md:2713-2717）。
 *
 * 历史：`--export-diff` 对拍工具已于 2026-10-01 **删除** —— 它只服务于"切到 AST 之前先证明
 *   两条路径给出的导出名一致"这一件事；导出名早就切完（9a7ba8c），且 7 类文本扫描退役后
 *   扫描链不再登记导出名 ⇒ 它只会持续报"仅 AST 有"的噪音，没有验收价值。
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
//   顺序 = 源码里顶层声明的出现顺序（消费方只关心集合 ✓）。
int module_ast_collect_exports(const char* src, AstExportList* out);

void module_ast_exports_free(AstExportList* list);

// 注册"基于 parser AST 的导出名提供者"（编译期在 main.c 调一次即可）
//   ⇒ 之后 `module_loader` 的 copy_module_export_names / module_has_method 都走语法实现 ✓
void module_ast_exports_register(void);

// S10：注册"AST 符号填充器"（编译期在 main.c 调一次）
//   ⇒ 扫描链跑完后，按声明种类由 AST 建表/覆盖；7 类文本扫描已于 2026-10-01 全部退役 ✓
void module_ast_symbols_register(void);

// 第二次填充（语义分析之后由 module_compiler 调用）：传**已语义化的 AST**。
//   参数用 void* ⇒ 不在这个头里引入 core 的 ModuleSymbolTable 类型 ✓
void module_ast_symbols_fill_from_ast(void* table, void* ast_root);

// 把 TypeInfo 渲染成**悬停口径**的类型文本（裸名字：`Widget` / `Array[Button]` / `Ptr[u8]`）
//   —— 与 type.c 的诊断口径（`struct Widget`）刻意不同，见 module_ast_exports.c 的说明。
//   用途：LSP 的悬停/参数提示显示返回类型；ti 为 NULL ⇒ 写 "any"；缓冲不足时自动截断 ✓
//   （TypeInfo 是 core 类型，这里用 void* 以便 LSP 侧直接传 TypeInfo* ✓）
void module_ast_type_text_into(void* ti, char* buf, int cap);

#endif  // LENO_MODULE_AST_EXPORTS_H
