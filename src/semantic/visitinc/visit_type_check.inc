        case AST_TYPE_CHECK:
            if (ast->u.type_check.expr) {
                visit(s, ast->u.type_check.expr);
            }
            // ★ 2026-10-10（A 组 ①(a)）：**`not x is T` 的优先级陷阱** ✓
            //   左边正是 `not X` ⇒ 它其实是 `(not X) is T` ✗（实测 x=42 时结果**相反** ✗）
            //   见 semantic_type_utils.c 的 semantic_check_not_precedence ✓
            semantic_check_not_precedence(ast->u.type_check.expr, ast->line, ast->column, 0);
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
            // ★ 「as 恒失败」静态诊断（2026-10-09，用户实测"误判两次"后要的 ✓）——
            //   见 vm.c 的 vm_as_cast（「as」语义唯一来源）：as 只做**表示/数值转换**、
            //   **不做字符串解析**；且 `as bool` 只认 bool ⇒ 下面这些"源是具体类型、目标
            //   铁定不收"的组合，运行期必然回 null ⇒ 编译期直接拒绝（比 null 静默冒出友好 ✓）。
            //   ⚠ 与 vm_as_cast 的 switch 是**同一套口径**：改动要两边一起改（vm.c:636）。
            //   ⚠ 零误报优先：
            //     · 源是 any/null/unknown/infer ⇒ 放行（运行时才知道 ✓ —— 下面只认具体标量）
            //     · TYPE_STRING 是**可靠信号**（字面量 / 已解析的 string 符号 ✓），不像 face 参数
            //       会被 infer_expr_type 兜底成数值（见上面"不可能收窄"的注释 ✓）
            {
                TypeInfo* as_src = NULL;
                if (ast->u.type_check.expr) {
                    as_src = infer_expr_type(s, ast->u.type_check.expr);
                }
                TypeInfo* as_dst = ast->u.type_check.type;
                if (as_src && as_dst) {
                    TypeKind sk = as_src->kind;
                    TypeKind dk = as_dst->kind;
                    int dst_num = (dk == TYPE_INT || dk == TYPE_FLOAT ||
                                   (dk >= TYPE_I8 && dk <= TYPE_U64) ||
                                   dk == TYPE_F32 || dk == TYPE_F64);
                    int src_num = (sk == TYPE_INT || sk == TYPE_FLOAT || sk == TYPE_BIGINT ||
                                   (sk >= TYPE_I8 && sk <= TYPE_U64) ||
                                   sk == TYPE_F32 || sk == TYPE_F64);
                    // ① string → 数值类 / bool：as 不做字符串解析
                    if (sk == TYPE_STRING && (dst_num || dk == TYPE_BOOL)) {
                        char msg[384];
                        snprintf(msg, sizeof(msg),
                            "as 不做字符串解析：string 无法 as %s（运行期必回 null）。"
                            "as 只做表示/数值转换（数值→string、float→int 截断、bool→1/0）；"
                            "要解析字符串请用 _int()（严格版，坏串会抛异常）或 _int_or(v, 默认)（宽容版）",
                            sem_as_kind_name(dk));
                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                    }
                    // ② 数值类 → bool：as bool 只认 bool（int/float/bigint as bool 恒失败）
                    else if (dk == TYPE_BOOL && sk != TYPE_BOOL && src_num) {
                        char msg[384];
                        snprintf(msg, sizeof(msg),
                            "as bool 只认 bool：%s 无法 as bool（运行期必回 null）。"
                            "要取真值请显式比较（如 x != 0）",
                            sem_as_kind_name(sk));
                        error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                    }
                }
                if (as_src) type_free(as_src);
            }
            break;