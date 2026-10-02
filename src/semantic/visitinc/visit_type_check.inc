        case AST_TYPE_CHECK:
            if (ast->u.type_check.expr) {
                visit(s, ast->u.type_check.expr);
            }
            // 递归纠正类型名（face/enum 被 parser 记成 struct）+ 修 Array/Dict/Ptr 的实参：
            //   v3.2.2 起嵌套实参的名字会被带进运行期做名字校验 ⇒ 漏修就会误判不匹配
            resolve_type_names(s, &ast->u.type_check.type);
            break;

        case AST_AS_CAST:
            if (ast->u.type_check.expr) {
                visit(s, ast->u.type_check.expr);
            }
            resolve_type_names(s, &ast->u.type_check.type);
            // ★ 「不可能成功的 as 收窄」提示（WARN_IMPOSSIBLE_CAST）——
            //   数值标量（int/float）**永远不可能**是一个容器/struct/face ⇒ 运行期必定抛异常；
            //   若外面套了 `catch { }` 就成了"功能静默失效" ✗。真实案例：
            //     `ti.now()` 返回的是 **int 时间戳**（不是数组），`ti.now() as Array[int]`
            //     只在**运行期**炸 ⇒ 应用里「上次清理记录」「导出报告」静默失效很久 ✗
            //     而写成 `int d = ti.now() as Array[int]` 反而会报错（声明类型不匹配 ✓）
            //     ⇒ 只有 `var` 接的写法漏网，正是本诊断要补的洞 ✓
            //   ⚠ 口径（**零误报**优先）：仅当源类型是**完全确定**的 int/float、且目标类型是
            //     Array/Dict/struct/cstruct/face 时才提示。
            //     故意放过：`any`（可能真的是容器 ✓）、泛型参数、TYPE_INFER；也放过 string
            //     （string 与字符容器之间可能有转换通道 ⇒ 不敢断言 ✓）。
            //   ⚠ 标量→标量（如 `int as float`）是**转换**（OP_CAST_*）不是类型检查 ⇒ 不能拦 ✓
            {
                // ⚠⚠ **必须排除"裸标识符"源**（实测踩到假阳性 ✗）：face 类型的**参数**标识符
                //   被 `infer_expr_type` 兜底推成了数值标量 ✗ ⇒ 于是 `s as Circle`（face→struct 的
                //   合法向下收窄 ✓）会被误判成"不可能收窄" ✗（探针实测：_tmp_face_cast_probe ✓）。
                //   宁漏不误报：只信**非标识符**的源表达式（字面量 / 调用 / 算术 ✓）——
                //   而本诊断的动机用例 `ti.now() as Array[int]` 正是**调用** ⇒ 仍然抓得住 ✓。
                //   （代价：`int n = 5; n as Dict[...]` 这种"标识符源 + 显式标量声明"会漏掉 ✗，
                //     但要抓它得先让符号类型可靠 ⇒ 等那步修好再放开 ✓）
                TypeInfo* src_t = NULL;
                if (ast->u.type_check.expr && ast->u.type_check.expr->kind != AST_VAR) {
                    src_t = infer_expr_type(s, ast->u.type_check.expr);
                }
                TypeInfo* dst_t = ast->u.type_check.type;
                if (src_t && dst_t) {
                    int src_num = (src_t->kind == TYPE_INT || src_t->kind == TYPE_FLOAT);
                    int dst_obj = (dst_t->kind == TYPE_ARRAY || dst_t->kind == TYPE_DICT ||
                                   dst_t->kind == TYPE_STRUCT || dst_t->kind == TYPE_CSTRUCT ||
                                   dst_t->kind == TYPE_FACE);
                    if (src_num && dst_obj) {
                        // ⚠ 2026-09-28 起由**警告升为错误**（用户要求 ✓）。
                        //   措辞经**实测**校正（`as` 到容器不会当场炸，而是原样带过 ✗）✓
                        error_add_at(ERR_SEMANTIC, ast->line, ast->column,
                            "[不可能收窄] 左边是确定的数值标量（int/float），右边是容器/struct ⇒ "
                            "这个 as 收窄不可能成功：as 会把原值原样带过 ✗，后面当容器用时"
                            "（索引/取字段/调方法）才失败。若确实要动态收窄，请先经 any 中转"
                            "（例如让值来自返回 any 的函数/参数 ✓）");
                    }
                }
                if (src_t) type_free(src_t);
            }
            break;