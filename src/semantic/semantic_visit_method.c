#include "semantic_internal.h"

// ============================================================================
// struct 方法字段访问转换
// ============================================================================

// 检查名字是否在遮蔽集合中（局部 var 声明遮蔽了同名字段）
static int is_shadowed(const char* name, char** shadowed, int shadowed_count) {
    if (!name || !shadowed || shadowed_count <= 0) return 0;
    for (int i = 0; i < shadowed_count; i++) {
        if (shadowed[i] && strcmp(name, shadowed[i]) == 0) return 1;
    }
    return 0;
}

// 名字是否是本 struct 的字段（返回下标；-1 = 不是字段）
static int field_index_of(const char* name, char** field_names, int field_count) {
    if (!name || !field_names || field_count <= 0) return -1;
    for (int i = 0; i < field_count; i++) {
        if (field_names[i] && strcmp(name, field_names[i]) == 0) return i;
    }
    return -1;
}

// 表达式是否是 `self.字段` 形态（手写的 self.x 与转换出来的都算 ✓）
// ⚠ 三种 AST 都要认（实测逐个踩出来的）：
//     · AST_FIELD_ACCESS —— 本函数转换出来的形态（`self.x = v` 的**左值**也是它）；
//     · AST_MODULE_ACCESS —— **源码里手写的** `self.x` 在表达式位置被解析成"模块成员访问"
//       （module_name="self"）⇒ 只认 FIELD_ACCESS 的话 `x = self.x` 会误报成"遮蔽字段的笔误" ✗
//     · AST_INDEX + 字符串下标 —— 语义层 `s.field` 的等价形态（`.name`→`["name"]` 约定）✓
// 用途：`float h = 0.0; h = self.h` 这种"把字段拷进同名局部"是作者本意 ⇒ 豁免 ✓
static int is_self_field_access(const Ast* e) {
    if (!e) return 0;
    if (e->kind == AST_MODULE_ACCESS) {
        return e->u.module_access.module_name && strcmp(e->u.module_access.module_name, "self") == 0;
    }
    const Ast* obj = NULL;
    if (e->kind == AST_FIELD_ACCESS) {
        obj = e->u.field_access.obj;
    } else if (e->kind == AST_INDEX && e->u.index.index && e->u.index.index->kind == AST_STRING) {
        obj = e->u.index.obj;
    } else {
        return 0;
    }
    return obj && obj->kind == AST_VAR && obj->u.var.name && strcmp(obj->u.var.name, "self") == 0;
}

// 名字是否是本方法的形参
// 为什么要区分形参与局部变量：`int y = year` + `y = y - 1`、`bool ok = true` + `ok = false`
// 这类"局部变量与字段同名"在本仓库是**既有且有意**的写法（实测 sdl_calendar / sdl_renderer
// 各有若干，改名前就是这样 ✗ 不是重构引入的）⇒ 对局部变量报等于刷屏噪音 ✗；
// 而**形参**遮蔽字段后被赋值几乎必是"以为在写字段"的笔误（本次踩的 16 处里 15 处如此 ✓）
static int is_param_name(const char* name, char** param_names, int param_count) {
    if (!name || !param_names || param_count <= 0) return 0;
    for (int i = 0; i < param_count; i++) {
        if (param_names[i] && strcmp(name, param_names[i]) == 0) return 1;
    }
    return 0;
}

// 当前正在转换的方法名（只在一次 transform_method_body 调用期间有效 ✓）
// 为什么要它：判"方法体内的裸调用与**所在方法**同名"= 自己调自己（见 AST_CALL 的诊断 ✓）。
// 单线程即可：编译器与 LSP 都是顺序做语义分析（src 里唯一的线程封装是**脚本侧**的 threads
// 模块，与编译期无关 ✓）⇒ 用 static 传状态足够，且不用给 56 处递归调用各加一个参数 ✓
static const char* cur_method_name = NULL;

// 同名歧义表（由调用方按 struct 备好，见 visit_type_def.inc 的说明 ✓）：
//   amb_names[i] 既是本 struct 的方法名、又能按裸名解析到全局/模块函数（amb_pcnts[i] = 其形参个数）
// 为什么要它：那种裸调用**两种解释都成立**，而实际会解析成方法（方法优先）⇒ 可能静默走错目标 ✗
static char** cur_amb_names = NULL;
static int* cur_amb_pcnts = NULL;
static int cur_amb_count = 0;

// ★★ 2026-10-10 新增：**形参个数闸门**用的两张表（都由调用方按 struct 备好 ✓）
//   为什么要它：裸名调用原先**只看名字**就优先当方法 ⇒ 与同名自由/全局函数撞车时，
//     要么报“参数过多”（挡住正确写法 ✗），要么在个数恰好吻合时**静默调错目标** ✗
//     （上游 LenoSDL3 打包后窗口句柄下发静默失效、本仓 10 组同名撞车都是这一条 ✓）
//   cur_meth_pcnts[i]     与 method_names[i] 平行：那个方法**自己**声明的形参个数 ✓
//   cur_amb_meth_pcnts[i] 与 amb_names/amb_pcnts 平行：撞名**方法**自己的形参个数 ✓
//                         （amb_pcnts[i] 记的是同名**函数**的个数 ✓ 两个都要 ✓）
static int* cur_meth_pcnts = NULL;
static int  cur_meth_pcnt_count = 0;
static int* cur_amb_meth_pcnts = NULL;

// 前向声明
static void transform_method_body_ex(Ast* ast, char** field_names, int field_count,
    char** method_names, int method_count, const char* struct_name,
    char** shadowed_names, int shadowed_count,
    char** param_names, int param_count, char** const_names, int const_count);

// 将方法体中对字段名和方法名的访问转换为 self.字段名 / StructName::method(self, args)
// 这是实现 struct 方法的核心：方法体内可以直接写字段名访问字段，直接写方法名调用同 struct 方法
// param_names/param_count: 方法参数名列表，参数与字段同名时参数优先（遮蔽字段）
// const_names/const_count: 关联常量名列表，方法体内可以直接用常量名访问 StructName.CONST
// method_name: 本方法自己的名字（只给诊断用：判"裸调用与所在方法同名"⇒ 自递归 ✓）
// amb_*: 同名歧义表（本 struct 的方法名 ∩ 能按裸名解析到的全局/模块函数，且形参个数相同 ✓）
void transform_method_body(Ast* ast, char** field_names, int field_count, char** method_names, int method_count, const char* struct_name,
    char** param_names, int param_count, char** const_names, int const_count, const char* method_name,
    char** amb_names, int* amb_pcnts, int amb_count,
    int* method_pcounts, int* amb_meth_pcnts) {
    const char* saved_method = cur_method_name;
    char** saved_amb_names = cur_amb_names;
    int* saved_amb_pcnts = cur_amb_pcnts;
    int saved_amb_count = cur_amb_count;
    int* saved_meth_pcnts = cur_meth_pcnts;
    int saved_meth_pcnt_count = cur_meth_pcnt_count;
    int* saved_amb_meth_pcnts = cur_amb_meth_pcnts;
    cur_method_name = method_name;
    cur_amb_names = amb_names;
    cur_amb_pcnts = amb_pcnts;
    cur_amb_count = amb_count;
    cur_meth_pcnts = method_pcounts;
    cur_meth_pcnt_count = method_pcounts ? method_count : 0;
    cur_amb_meth_pcnts = amb_meth_pcnts;
    transform_method_body_ex(ast, field_names, field_count, method_names, method_count, struct_name,
        param_names, param_count, param_names, param_count, const_names, const_count);
    cur_method_name = saved_method;
    cur_amb_names = saved_amb_names;
    cur_amb_pcnts = saved_amb_pcnts;
    cur_amb_count = saved_amb_count;
    cur_meth_pcnts = saved_meth_pcnts;
    cur_meth_pcnt_count = saved_meth_pcnt_count;
    cur_amb_meth_pcnts = saved_amb_meth_pcnts;
}

// shadowed_names/shadowed_count: 当前**生效的**遮蔽集合（= 形参 + 所在块内声明过的局部变量 ✓）
// param_names/param_count: 形参那一份（全程不变）—— 只给诊断用（见 is_param_name 的理由 ✓）
static void transform_method_body_ex(Ast* ast, char** field_names, int field_count,
    char** method_names, int method_count, const char* struct_name,
    char** shadowed_names, int shadowed_count,
    char** param_names, int param_count, char** const_names, int const_count) {
    if (!ast) return;
    
    switch (ast->kind) {
        case AST_VAR: {
            // 如果被局部变量遮蔽，跳过转换
            if (is_shadowed(ast->u.var.name, shadowed_names, shadowed_count)) break;
            // 检查是否是字段名
            for (int i = 0; i < field_count; i++) {
                if (strcmp(ast->u.var.name, field_names[i]) == 0) {
                    // 保存行号和字段名
                    int line = ast->line;
                    char* saved_field_name = strdup(field_names[i]);

                    // 释放原节点的变量数据
                    free(ast->u.var.name);
                    if (ast->u.var.ref.name) free(ast->u.var.ref.name);

                    // 将当前节点转换为 FIELD_ACCESS 节点：self.field_name
                    // 使用 AST_FIELD_ACCESS 而非 AST_INDEX，使语义分析能填充 field_index，
                    // 代码生成能使用 OP_GET_FIELD 做编译期索引直接访问（O(1) 数组下标）
                    ast->kind = AST_FIELD_ACCESS;

                    // 创建 self 变量节点
                    Ast* self_var = ast_new(AST_VAR, line);
                    self_var->u.var.name = strdup("self");
                    self_var->u.var.ref.name = strdup("self");
                    self_var->u.var.ref.kind = SYM_PARAM;  // self 是参数
                    self_var->u.var.ref.index = 0;         // self 是第一个参数
                    ast->u.field_access.obj = self_var;

                    // 设置字段名（语义分析会填充 field_index）
                    ast->u.field_access.field_name = saved_field_name;
                    ast->u.field_access.field_index = -1;   // 待语义分析填充

                    break;
                }
            }
            // 如果已经转换为字段访问，跳过常量检查
            if (ast->kind == AST_FIELD_ACCESS) break;
            // 检查是否是关联常量名
            if (const_names && const_count > 0 && struct_name) {
                for (int i = 0; i < const_count; i++) {
                    if (const_names[i] && strcmp(ast->u.var.name, const_names[i]) == 0) {
                        // 将裸常量名转换为 StructName.CONST（AST_MODULE_ACCESS）
                        char* saved_const_name = strdup(const_names[i]);
                        char* saved_struct_name = strdup(struct_name);

                        // 释放原节点的变量数据
                        free(ast->u.var.name);
                        if (ast->u.var.ref.name) free(ast->u.var.ref.name);

                        // 转换为 MODULE_ACCESS 节点：StructName.CONST
                        ast->kind = AST_MODULE_ACCESS;
                        ast->u.module_access.module_name = saved_struct_name;
                        ast->u.module_access.member_name = saved_const_name;
                        memset(&ast->u.module_access.ref, 0, sizeof(SymRef));

                        break;
                    }
                }
            }
            break;
        }
        case AST_ASSIGN: {
            // 先处理 value 中的字段访问（在转换前保存 value 引用）
            Ast* value = ast->u.assign.value;
            // ★ 诊断用：变换**前**记下右侧那个裸标识符的名字 —— 变换会把 `X` 就地改写成 `self.X`，
            //   之后就分不清"是不是自赋值"了；而且它会 free 掉原 name ⇒ 必须先拷出来 ✓
            char rhs_var_before[128];
            rhs_var_before[0] = '\0';
            if (value && value->kind == AST_VAR && value->u.var.name) {
                snprintf(rhs_var_before, sizeof(rhs_var_before), "%s", value->u.var.name);
            }
            transform_method_body_ex(value, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);

            // 处理赋值左侧的复杂目标表达式（如 v[0].x = 1.0）
            if (ast->u.assign.targets) {
                for (int i = 0; i < ast->u.assign.name_count; i++) {
                    if (ast->u.assign.targets[i]) {
                        transform_method_body_ex(ast->u.assign.targets[i], field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
                    }
                }
            }

            // ★★ 编译期诊断（2026-09-30）：字段被形参/局部变量遮蔽时，赋值会**静默失效** ★★
            //   实测踩到：pri 去下划线重构把 16 处 `_字段 = 形参` 批量改成了 `字段 = 形参`
            //   （如 Window.setRootAt 里的 `root = root`）⇒ 字段永远是 null ⇒ 整个界面空白，
            //   而编译器一声不吭（966/966 全都"编译通过" —— 运行时语义它测不出来 ✗）。
            //   判据两条（**少报不误报**：正常写法一句不报 ✓）：
            //     ① 自赋值 `X = X`：不管 X 是字段还是变量都**没有任何效果** ⇒ 必是笔误；
            //     ② **形参**遮蔽字段后被赋了别的值：字段不参与赋值 ⇒ 告诉他 `self.X` 写法 ✓。
            //        ⚠ 只认形参，不认局部变量：`int y = year` + `y = y - 1`、`bool ok = true` +
            //          `ok = false` 这类"局部与字段同名"是仓库既有且**有意**的写法（改名前就在 ✓）
            //          ⇒ 对局部变量报就是刷屏噪音 ✗（实测：全 SDK 只有这两处会误报 ✓）
            //   唯一豁免：右侧**就是** `self.X`（`float h = 0.0; h = self.h` 是作者本意 ✓）。
            //   回归用例：assert/test_field_shadow_assign.leno ✓
            for (int i = 0; i < ast->u.assign.name_count; i++) {
                const char* tname = ast->u.assign.names[i];
                if (!tname) continue;
                int is_field = (field_index_of(tname, field_names, field_count) >= 0);
                int shadowed = is_shadowed(tname, shadowed_names, shadowed_count);
                char msg[BUFFER_MEDIUM];
                if (rhs_var_before[0] && strcmp(rhs_var_before, tname) == 0) {
                    // ① 自赋值 `X = X`
                    if (is_field && shadowed) {
                        snprintf(msg, sizeof(msg),
                            "自赋值 `%s = %s` 不产生任何效果：此处的 `%s` 是形参/局部变量（它遮蔽了同名字段 "
                            "`%s`）⇒ 字段不会被修改。要改字段请写 `self.%s = %s`；要读字段请写 `self.%s`",
                            tname, tname, tname, tname, tname, tname, tname);
                    } else if (is_field) {
                        snprintf(msg, sizeof(msg),
                            "自赋值 `%s = %s` 不产生任何效果（字段 `%s` 的值不变）—— 请检查是否漏写 `self.`",
                            tname, tname, tname);
                    } else {
                        snprintf(msg, sizeof(msg),
                            "自赋值 `%s = %s` 不产生任何效果（形参/局部变量赋给自己）—— 请检查是否漏写",
                            tname, tname);
                    }
                    warning_add_at(WARN_FIELD_SHADOW_ASSIGN, ast->line, ast->column, msg);
                } else if (is_field && shadowed && !is_self_field_access(value) &&
                           is_param_name(tname, param_names, param_count)) {
                    // ② 形参遮蔽字段后被赋了"别的值"
                    snprintf(msg, sizeof(msg),
                        "赋值只作用于形参 `%s`（它遮蔽了同名字段 `%s`）⇒ 字段不会被修改；"
                        "要改字段请写 `self.%s = ...`；若确实只想改这个形参，请给它改名避免遮蔽",
                        tname, tname, tname);
                    warning_add_at(WARN_FIELD_SHADOW_ASSIGN, ast->line, ast->column, msg);
                }
            }

            // 处理赋值左侧的字段名
            for (int i = 0; i < ast->u.assign.name_count; i++) {
                // 跳过 NULL 名称（复杂赋值目标如 v[0].x，names[i] 为 NULL）
                if (!ast->u.assign.names[i]) continue;
                // 如果被局部变量遮蔽，跳过转换
                if (is_shadowed(ast->u.assign.names[i], shadowed_names, shadowed_count)) continue;
                for (int j = 0; j < field_count; j++) {
                    if (strcmp(ast->u.assign.names[i], field_names[j]) == 0) {
                        // 将字段名赋值转换为 self.字段名 = value
                        // 保存原始 value 引用
                        Ast* original_value = ast->u.assign.value;

                        // 替换当前 AST 节点为 INDEX_ASSIGN
                        ast->kind = AST_INDEX_ASSIGN;

                        // 创建 self 变量节点
                        Ast* self_var = ast_new(AST_VAR, ast->line);
                        self_var->u.var.name = strdup("self");
                        self_var->u.var.ref.name = strdup("self");
                        self_var->u.var.ref.kind = SYM_PARAM;  // self 是参数
                        self_var->u.var.ref.index = 0;         // self 是第一个参数
                        ast->u.index_assign.obj = self_var;

                        // 创建字段名字符串节点
                        Ast* field_str = ast_new(AST_STRING, ast->line);
                        field_str->u.string.value = strdup(field_names[j]);
                        field_str->u.string.len = (int)strlen(field_names[j]);
                        ast->u.index_assign.index = field_str;

                        // 设置 value（已经处理过其中的字段访问）
                        ast->u.index_assign.value = original_value;

                        // 不需要 break，因为已经转换了整个赋值语句
                        return;
                    }
                }
            }
            break;
        }
        case AST_BINOP:
            transform_method_body_ex(ast->u.binop.l, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            transform_method_body_ex(ast->u.binop.r, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        case AST_UNARY:
            transform_method_body_ex(ast->u.unary.operand, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        case AST_CALL: {
            // 检查 callee 是否是同 struct 的方法名调用
            int is_struct_method_call = 0;
            int warned_self_forward = 0;
            if (ast->u.call.callee->kind == AST_VAR && struct_name &&
                !is_shadowed(ast->u.call.callee->u.var.name, shadowed_names, shadowed_count)) {
                const char* callee_name = ast->u.call.callee->u.var.name;
                for (int i = 0; i < method_count; i++) {
                    if (strcmp(callee_name, method_names[i]) == 0) {
                        // ★★ 2026-10-10 治本（上游 LenoSDL3「打包后窗口句柄下发静默失效」根因 ①）：
                        //   **形参个数对不上就不许劫持** ✓
                        //   症状：模块 A 的顶层自由函数 `f(a, b)` 与模块 B 的方法 `T.f(a)` **同名** ✗
                        //     裸调用 `f(x, y)` 原先被**无条件**改写成 `self["f"](self, x, y)` ⇒
                        //     ① 个数差得多时当场报“参数过多/不足” ⇒ 把**正确写法**挡在编译期 ✗
                        //     ② 个数恰好吻合时两种解释都成立 ⇒ **静默调错目标** ✗
                        //     （本仓实测 10 组同名撞车 + 那次输入法候选框跑偏都是这一条 ✓）
                        //   现在：只有当这个裸调用**在个数上也像那个方法**时才劫持 ✓；
                        //     个数不符、且**裸名确实还能解析到同名函数、其个数正好等于实参个数**时
                        //     （= 这行调用其实是在调那个函数 ✓）⇒ 不劫持，原样交给正常解析 ✓
                        //   ⚠ 为什么不能简单写成“个数不等就不许劫持”：方法可以带**默认参数**
                        //     （`func f(int a, int b = 1)` 的 pcnt = 2 ✓）⇒ 裸写 `f(1)` 是**合法的方法调用**
                        //     ✓，一律拒绝就会把它推给自由/全局函数 ⇒ 引入新的静默走错 ✗
                        //     只有“另一个同名函数正好吃 1 个实参”时，这行调用才真的另有解释 ✓
                        {
                            int argc = ast->u.call.args.count;
                            int mpc = (cur_meth_pcnts && i < cur_meth_pcnt_count) ? cur_meth_pcnts[i] : -1;
                            int refuse = 0;
                            if (mpc >= 0) {
                                if (argc > mpc) {
                                    // ★ 硬判据：实参**比方法形参还多** ⇒ 铁定不是这个方法 ✓
                                    //   为什么零风险：方法可以有默认参数（实参只会**更少** ✓），
                                    //   绝不可能"实参多于形参"还合法 ✓ ⇒ 拒绝劫持必然正确 ✓
                                    refuse = 1;
                                } else if (argc < mpc) {
                                    // 实参更少：可能是默认参数（**合法的方法调用** ✓）⇒ 只有
                                    // "同名裸函数正好吃这么多实参"（表里有 ✓）时才让位给它 ✓
                                    for (int ai = 0; ai < cur_amb_count; ai++) {
                                        if (cur_amb_names[ai] && strcmp(cur_amb_names[ai], callee_name) == 0 &&
                                            cur_amb_pcnts[ai] == argc) {
                                            refuse = 1;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (refuse) continue;
                        }
                        int line = ast->u.call.callee->line;
                        // 先保存方法名，再释放 callee
                        char* saved_method_name = strdup(callee_name);
                        // 将 callee 从 AST_VAR 改为 AST_INDEX(self, "method_name")
                        // 这样 codegen 会用 OP_GET_METHOD 处理
                        free(ast->u.call.callee->u.var.name);
                        if (ast->u.call.callee->u.var.ref.name) free(ast->u.call.callee->u.var.ref.name);
                        ast->u.call.callee->kind = AST_INDEX;
                        Ast* self_var = ast_new(AST_VAR, line);
                        self_var->u.var.name = strdup("self");
                        self_var->u.var.ref.name = strdup("self");
                        self_var->u.var.ref.kind = SYM_PARAM;
                        self_var->u.var.ref.index = 0;
                        ast->u.call.callee->u.index.obj = self_var;
                        Ast* method_str = ast_new(AST_STRING, line);
                        method_str->u.string.value = saved_method_name;
                        method_str->u.string.len = (int)strlen(saved_method_name);
                        ast->u.call.callee->u.index.index = method_str;
                        is_struct_method_call = 1;
                        // ★★ 诊断（2026-09-30）：裸调用与**所在方法自己**同名，且实参就是它自己的
                        //   形参 ⇒ 这条"同 struct 方法优先"的改写让它变成**自己调自己** ⇒ 必然
                        //   无限递归（栈溢出）。实测踩到：sdl_calendar 的
                        //   `func _dow(int y, int m, int d) { return dow(y, m, d) }` 去下划线改名成
                        //   `dow` 后既与**模块级** `dow()` 撞名、又被本规则劫持 ⇒ 数据看板
                        //   /dashboard 直接「调用栈溢出」，而编译期一声不吭 ✗。
                        //   只认"实参全是本方法形参"这一种（= 纯转发，改名前它是转发给模块函数的 ✓）；
                        //   带修改的递归是正常写法 ⇒ 不报 ✓（如 `walk(node.child)` ✓）
                        if (cur_method_name && strcmp(saved_method_name, cur_method_name) == 0 &&
                            param_count > 0 && ast->u.call.args.count == param_count) {
                            int all_params = 1;
                            for (int ai = 0; ai < ast->u.call.args.count; ai++) {
                                Ast* a = ast->u.call.args.items[ai];
                                if (!a || a->kind != AST_VAR || !a->u.var.name ||
                                    !is_param_name(a->u.var.name, param_names, param_count)) {
                                    all_params = 0;
                                    break;
                                }
                            }
                            if (all_params) {
                                char cmsg[BUFFER_LARGE];
                                snprintf(cmsg, sizeof(cmsg),
                                    "裸调用 `%s(...)` 与所在方法同名，实参就是它自己的形参 ⇒ 会被解析成"
                                    "**调用自身** ⇒ 无限递归（栈溢出）。若本意是调同文件的模块级函数 `%s`"
                                    "（去下划线改名后两者撞名了），请给这个方法改名区分开；确实要递归"
                                    "请改实参或加终止条件",
                                    saved_method_name, saved_method_name);
                                warning_add_at(WARN_SELF_FORWARD, ast->line, ast->column, cmsg);
                                warned_self_forward = 1;   // 已报更具体的那条 ⇒ 不必再报"同名歧义" ✓
                            }
                        }
                        // ★★ 诊断（2026-09-30）：**同名歧义** —— 裸调用与**所在方法同名**，而这个名字
                        //   还能按裸名解析到形参个数相同的全局/模块函数 ⇒ 两种解释都成立，实际走方法。
                        //   为什么只收"与所在方法同名"（= 自己调自己）：调**兄弟**方法时（如
                        //   GameBot.attach 里 `attachPid(pid)`）作者本意几乎必是那个兄弟方法，且类型/
                        //   返回值通常也对得上 ⇒ 报了就是噪音 ✗（实测 LenoHack 就有一处，行为是对的 ✓）
                        //   为什么要"形参个数也相同"：个数只吻合函数时编译器当场报参数不足/过多（响的 ✓）
                        //   为什么 ② 报过就不报这条：② 更具体（说清了"纯转发 ⇒ 必炸"）✓
                        for (int ai = 0; !warned_self_forward && ai < cur_amb_count; ai++) {
                            // ★ 2026-10-10：歧义表改成**全收**（原先只收“函数与方法个数相同”的那批 ✗，
                            //   那正好把“个数不符 ⇒ 被方法劫持”这条 bug 掩盖掉了 ⇒ 见 visit_type_def.inc ✓）
                            //   ⇒ 这里必须自己把“两边个数都吻合”这个条件补齐 ✓ 告警语义与从前一致 ✓
                            if (cur_amb_names[ai] && strcmp(cur_amb_names[ai], saved_method_name) == 0 &&
                                cur_method_name && strcmp(saved_method_name, cur_method_name) == 0 &&
                                ast->u.call.args.count == cur_amb_pcnts[ai] &&
                                (!cur_amb_meth_pcnts || ast->u.call.args.count == cur_amb_meth_pcnts[ai])) {
                                char amsg[BUFFER_LARGE];
                                snprintf(amsg, sizeof(amsg),
                                    "裸调用 `%s(...)` 有歧义：它与所在方法同名，而这个名字还能按裸名解析到"
                                    "形参个数相同的全局/模块函数 `%s`，**实际会调用本 struct 的方法**"
                                    "（方法优先）。要调方法请显式写 `self.%s(...)`；要调那个函数请改名区分",
                                    saved_method_name, saved_method_name, saved_method_name);
                                warning_add_at(WARN_METHOD_NAME_AMBIGUOUS, ast->line, ast->column, amsg);
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            transform_method_body_ex(ast->u.call.callee, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            
            // 如果是同 struct 方法调用，需要将 self 作为第一个参数插入
            if (is_struct_method_call) {
                // 创建新的参数列表，self 作为第一个参数
                int new_count = ast->u.call.args.count + 1;
                Ast** new_items = (Ast**)malloc(sizeof(Ast*) * new_count);
                
                // 第一个参数是 self
                Ast* self_arg = ast_new(AST_VAR, ast->line);
                self_arg->u.var.name = strdup("self");
                self_arg->u.var.ref.name = strdup("self");
                self_arg->u.var.ref.kind = SYM_PARAM;
                self_arg->u.var.ref.index = 0;
                new_items[0] = self_arg;
                
                // 复制原有参数
                for (int i = 0; i < ast->u.call.args.count; i++) {
                    new_items[i + 1] = ast->u.call.args.items[i];
                    transform_method_body_ex(new_items[i + 1], field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
                }
                
                // 释放旧列表，使用新列表
                free(ast->u.call.args.items);
                ast->u.call.args.items = new_items;
                ast->u.call.args.count = new_count;
            } else {
                // 普通调用，正常处理参数
                for (int i = 0; i < ast->u.call.args.count; i++) {
                    transform_method_body_ex(ast->u.call.args.items[i], field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
                }
            }
            break;
        }
        case AST_INDEX:
            transform_method_body_ex(ast->u.index.obj, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            transform_method_body_ex(ast->u.index.index, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        case AST_FIELD_ACCESS:
            transform_method_body_ex(ast->u.field_access.obj, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        case AST_INDEX_ASSIGN:
            transform_method_body_ex(ast->u.index_assign.obj, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            transform_method_body_ex(ast->u.index_assign.index, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            transform_method_body_ex(ast->u.index_assign.value, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        case AST_IF: {
            transform_method_body_ex(ast->u.if_.cond, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            transform_method_body_ex(ast->u.if_.then, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            transform_method_body_ex(ast->u.if_.else_, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        }
        case AST_WHILE: {
            transform_method_body_ex(ast->u.while_.cond, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            transform_method_body_ex(ast->u.while_.body, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        }
        case AST_FOR: {
            transform_method_body_ex(ast->u.for_.start, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            transform_method_body_ex(ast->u.for_.end, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            transform_method_body_ex(ast->u.for_.step, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            transform_method_body_ex(ast->u.for_.body, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        }
        case AST_BLOCK: {
            // 收集块内所有 var_decl 的变量名，与传入的遮蔽集合合并
            char* merged_shadows[128];
            int merged_shadow_count = 0;
            // 先加入传入的遮蔽名（参数名等）
            for (int i = 0; i < shadowed_count && merged_shadow_count < 128; i++) {
                if (shadowed_names[i]) {
                    merged_shadows[merged_shadow_count++] = shadowed_names[i];
                }
            }
            // 再加入块内 var_decl 的变量名
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* item = ast->u.block.items[i];
                if (item && item->kind == AST_VAR_DECL && item->u.var_decl.name) {
                    if (merged_shadow_count < 128) {
                        merged_shadows[merged_shadow_count++] = item->u.var_decl.name;
                    }
                }
            }
            // 用合并后的遮蔽集合处理块内语句
            for (int i = 0; i < ast->u.block.count; i++) {
                // 对 var_decl 的 init 部分不传入自身名（防止 init 引用自身时被遮蔽）
                if (ast->u.block.items[i]->kind == AST_VAR_DECL) {
                    transform_method_body_ex(ast->u.block.items[i]->u.var_decl.init,
                        field_names, field_count, method_names, method_count, struct_name,
                        shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);  // init 中仍可访问字段
                } else {
                    transform_method_body_ex(ast->u.block.items[i],
                        field_names, field_count, method_names, method_count, struct_name,
                        merged_shadows, merged_shadow_count, param_names, param_count, const_names, const_count);
                }
            }
            break;
        }
        case AST_RETURN:
            transform_method_body_ex(ast->u.ret, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        case AST_RETURN_MULTI:
            for (int i = 0; i < ast->u.ret_multi.count; i++) {
                transform_method_body_ex(ast->u.ret_multi.exprs[i], field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            }
            break;
    case AST_VAR_DECL:
        transform_method_body_ex(ast->u.var_decl.init, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
        break;
    case AST_DESTRUCT_DECL:
        transform_method_body_ex(ast->u.destruct_decl.init, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
        break;
    case AST_EXPR_STMT:
            transform_method_body_ex(ast->u.expr_stmt.expr, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        case AST_MODULE_CALL: {
            // 检查 module_name 是否是字段名（如 scores.len() 中的 scores）
            int is_field = 0;
            if (!is_shadowed(ast->u.module_call.module_name, shadowed_names, shadowed_count)) {
                for (int i = 0; i < field_count; i++) {
                    if (strcmp(ast->u.module_call.module_name, field_names[i]) == 0) {
                        is_field = 1;
                        break;
                    }
                }
            }

            // 检查是否是同 struct 的方法调用
            int is_method = 0;
            if (!is_shadowed(ast->u.module_call.method_name, shadowed_names, shadowed_count)) {
                for (int i = 0; i < method_count; i++) {
                    if (strcmp(ast->u.module_call.method_name, method_names[i]) == 0) {
                        is_method = 1;
                        break;
                    }
                }
            }
            
            if (is_field) {
                // 将 field.method() 转换为 self.field["method"]()
                // 即：把 MODULE_CALL 转换为 CALL，callee 是 INDEX(self.field, "method")
                // 使用 AST_FIELD_ACCESS 优化字段读取，语义分析会填充 field_index
                int line = ast->line;
                
                // 保存原始信息
                char* field_name = strdup(ast->u.module_call.module_name);
                char* method_name = strdup(ast->u.module_call.method_name);
                int arg_count = ast->u.module_call.args.count;
                Ast** arg_items = ast->u.module_call.args.items;
                
                // 释放 module_name 和 method_name
                free(ast->u.module_call.module_name);
                free(ast->u.module_call.method_name);
                
                // 转换节点类型
                ast->kind = AST_CALL;
                
                // 创建 callee: self.field["method"]
                Ast* callee = ast_new(AST_INDEX, line);
                
                // 创建 self.field（使用 AST_FIELD_ACCESS 优化）
                Ast* self_var = ast_new(AST_VAR, line);
                self_var->u.var.name = strdup("self");
                self_var->u.var.ref.name = strdup("self");
                self_var->u.var.ref.kind = SYM_PARAM;
                self_var->u.var.ref.index = 0;

                Ast* self_field = ast_new(AST_FIELD_ACCESS, line);
                self_field->u.field_access.obj = self_var;
                self_field->u.field_access.field_name = field_name;
                self_field->u.field_access.field_index = -1;  // 待语义分析填充

                // 创建 ["method"]
                callee->u.index.obj = self_field;
                Ast* method_str = ast_new(AST_STRING, line);
                method_str->u.string.value = method_name;
                method_str->u.string.len = (int)strlen(method_name);
                callee->u.index.index = method_str;
                
                ast->u.call.callee = callee;
                ast->u.call.args.items = arg_items;
                ast->u.call.args.count = arg_count;
                ast->u.call.args.capacity = arg_count;
                
                // 处理参数
                for (int i = 0; i < arg_count; i++) {
                    transform_method_body_ex(ast->u.call.args.items[i], field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
                }
            // 仅当 module_name 为 "self" 时才转换为同 struct 方法调用
            // 否则可能是外部模块调用（如 sm.getValue()），不应被转换为 self 调用
            } else if (is_method && strcmp(ast->u.module_call.module_name, "self") == 0) {
                // 同 struct 方法调用：method(args) -> self["method"](self, args)
                int line = ast->line;
                char* method_name = strdup(ast->u.module_call.method_name);
                int arg_count = ast->u.module_call.args.count;
                Ast** arg_items = ast->u.module_call.args.items;
                
                free(ast->u.module_call.module_name);
                free(ast->u.module_call.method_name);
                
                ast->kind = AST_CALL;
                
                // 创建 callee: self["method"]
                Ast* callee = ast_new(AST_INDEX, line);
                Ast* self_var = ast_new(AST_VAR, line);
                self_var->u.var.name = strdup("self");
                self_var->u.var.ref.name = strdup("self");
                self_var->u.var.ref.kind = SYM_PARAM;
                self_var->u.var.ref.index = 0;
                callee->u.index.obj = self_var;
                Ast* method_str = ast_new(AST_STRING, line);
                method_str->u.string.value = method_name;
                method_str->u.string.len = (int)strlen(method_name);
                callee->u.index.index = method_str;

                ast->u.call.callee = callee;
                
                // 创建新的参数列表，self 作为第一个参数
                int new_count = arg_count + 1;
                Ast** new_items = (Ast**)malloc(sizeof(Ast*) * new_count);
                
                Ast* self_arg = ast_new(AST_VAR, line);
                self_arg->u.var.name = strdup("self");
                self_arg->u.var.ref.name = strdup("self");
                self_arg->u.var.ref.kind = SYM_PARAM;
                self_arg->u.var.ref.index = 0;
                new_items[0] = self_arg;

                for (int i = 0; i < arg_count; i++) {
                    new_items[i + 1] = arg_items[i];
                    transform_method_body_ex(new_items[i + 1], field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
                }
                
                free(arg_items);
                ast->u.call.args.items = new_items;
                ast->u.call.args.count = new_count;
                ast->u.call.args.capacity = new_count;
            } else {
                // 普通模块调用，只处理参数
                for (int i = 0; i < ast->u.module_call.args.count; i++) {
                    transform_method_body_ex(ast->u.module_call.args.items[i], field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
                }
            }
            break;
        }
        case AST_COMPOUND_ASSIGN: {
            // 如果被参数/局部变量遮蔽，跳过字段转换
            //   ⚠ 同一个坑的复合形态（`x += 1`）：被遮蔽时字段也不参与运算 ⇒ 一并告警 ✓
            //   （口径与上面 ② 完全一致：只认**形参**遮蔽 + 字段同名 ✓；
            //     正常写法 `self.x += 1` 走 AST_FIELD_ACCESS，压根到不了这里 ✓）
            if (is_shadowed(ast->u.compound_assign.name, shadowed_names, shadowed_count) &&
                field_index_of(ast->u.compound_assign.name, field_names, field_count) >= 0 &&
                is_param_name(ast->u.compound_assign.name, param_names, param_count)) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg),
                    "复合赋值只作用于形参 `%s`（它遮蔽了同名字段 `%s`）⇒ 字段不会被修改；"
                    "要改字段请写成 `self.%s` 的复合赋值；若确实只想改这个形参，请给它改名避免遮蔽",
                    ast->u.compound_assign.name, ast->u.compound_assign.name,
                    ast->u.compound_assign.name);
                warning_add_at(WARN_FIELD_SHADOW_ASSIGN, ast->line, ast->column, msg);
            }
            if (!is_shadowed(ast->u.compound_assign.name, shadowed_names, shadowed_count)) {
            // 检查是否是字段名的复合赋值
            for (int j = 0; j < field_count; j++) {
                if (strcmp(ast->u.compound_assign.name, field_names[j]) == 0) {
                    // 将字段复合赋值转换为 self.字段名 的复合赋值
                    free(ast->u.compound_assign.name);
                    ast->u.compound_assign.name = strdup(field_names[j]);
                    // 标记为需要通过 self 访问（使用 ref.name 存储标记）
                    free(ast->u.compound_assign.ref.name);
                    ast->u.compound_assign.ref.name = strdup("__self_field__");
                    // 存储字段索引，供代码生成器使用（优化：避免运行时线性搜索）
                    ast->u.compound_assign.ref.index = j;
                    break;
                }
            }
            } // end shadowed check
            transform_method_body_ex(ast->u.compound_assign.value, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        }
        case AST_SWITCH: {
            transform_method_body_ex(ast->u.switch_.expr, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                for (int j = 0; j < ast->u.switch_.cases[i].values.count; j++) {
                    transform_method_body_ex(ast->u.switch_.cases[i].values.items[j], field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
                }
                transform_method_body_ex(ast->u.switch_.cases[i].body, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            }
            transform_method_body_ex(ast->u.switch_.default_body, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        }
        case AST_TRY: {
            transform_method_body_ex(ast->u.try_.try_body, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            transform_method_body_ex(ast->u.try_.catch_body, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            transform_method_body_ex(ast->u.try_.finally_body, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        }
        case AST_THROW:
            transform_method_body_ex(ast->u.throw_.expr, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        case AST_ARRAY: {
            for (int i = 0; i < ast->u.array.count; i++) {
                transform_method_body_ex(ast->u.array.items[i], field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            }
            break;
        }
        case AST_DICT: {
            for (int i = 0; i < ast->u.dict.count; i++) {
                transform_method_body_ex(ast->u.dict.entries[i].value, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            }
            break;
        }
        case AST_INTERP_STRING: {
            for (int i = 0; i < ast->u.interp_string.count - 1; i++) {
                transform_method_body_ex(ast->u.interp_string.exprs[i], field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            }
            break;
        }
        case AST_TYPE_CHECK:
        case AST_AS_CAST:
            transform_method_body_ex(ast->u.type_check.expr, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        case AST_MODULE_ACCESS: {
            // 检查 module_name 是否是字段名（如 head.next 中的 head）
            if (!is_shadowed(ast->u.module_access.module_name, shadowed_names, shadowed_count)) {
            for (int i = 0; i < field_count; i++) {
                if (strcmp(ast->u.module_access.module_name, field_names[i]) == 0) {
                    // 将 module_access 转换为 field_access: self.field.member
                    int line = ast->line;
                    char* member_name = ast->u.module_access.member_name;

                    // 释放原 module_name
                    free(ast->u.module_access.module_name);

                    // 转换为 INDEX 节点：obj 是 self.field，index 是 member
                    ast->kind = AST_INDEX;

                    // 创建 self.field 作为 obj（使用 AST_FIELD_ACCESS 优化字段读取）
                    Ast* self_var = ast_new(AST_VAR, line);
                    self_var->u.var.name = strdup("self");
                    self_var->u.var.ref.name = strdup("self");
                    self_var->u.var.ref.kind = SYM_PARAM;
                    self_var->u.var.ref.index = 0;

                    Ast* field_access = ast_new(AST_FIELD_ACCESS, line);
                    field_access->u.field_access.obj = self_var;
                    field_access->u.field_access.field_name = strdup(field_names[i]);
                    field_access->u.field_access.field_index = -1;  // 待语义分析填充

                    ast->u.index.obj = field_access;

                    // 创建 member_name 作为 index
                    Ast* member_str = ast_new(AST_STRING, line);
                    member_str->u.string.value = member_name;
                    member_str->u.string.len = (int)strlen(member_name);
                    ast->u.index.index = member_str;

                    break;
                }
            }
            } // end shadowed check
            break;
        }
        case AST_FUNC_DEF:
            // 处理嵌套函数：转换函数体内的字段访问和方法调用
            transform_method_body_ex(ast->u.func.body, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        case AST_AWAIT:
            // 处理 await 表达式中的字段访问
            transform_method_body_ex(ast->u.await.expr, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            break;
        case AST_ADDRESS_OF:
            // 处理取地址表达式中的字段访问
            if (ast->u.address_of.operand) {
                transform_method_body_ex(ast->u.address_of.operand, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            }
            break;
        case AST_SLICE:
            // 切片表达式：arr[start:end]
            // 递归处理 obj（可能是 struct 字段名）、start、end 中的裸字段名
            if (ast->u.slice.obj) {
                transform_method_body_ex(ast->u.slice.obj, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            }
            if (ast->u.slice.start) {
                transform_method_body_ex(ast->u.slice.start, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            }
            if (ast->u.slice.end) {
                transform_method_body_ex(ast->u.slice.end, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            }
            break;
        case AST_SAFE_ACCESS:
            // 安全访问：expr?.field / expr?.method(args)
            // 转换 obj 和 args 中的裸字段名为 self["field"]
            if (ast->u.safe_access.obj) {
                transform_method_body_ex(ast->u.safe_access.obj, field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            }
            for (int i = 0; i < ast->u.safe_access.args.count; i++) {
                transform_method_body_ex(ast->u.safe_access.args.items[i], field_names, field_count, method_names, method_count, struct_name, shadowed_names, shadowed_count, param_names, param_count, const_names, const_count);
            }
            break;
        case AST_CLIB_DEF:
        default:
            break;
    }
}
