        case AST_TYPE_CHECK:
            if (ast->u.type_check.expr) {
                visit(s, ast->u.type_check.expr);
            }
            // 递归纠正类型名（face/enum 被 parser 记成 struct）+ 修 Array/Dict/Ptr 的实参：
            //   v3.2.2 起嵌套实参的名字会被带进运行期做名字校验 ⇒ 漏修就会误判不匹配
            resolve_type_names(s, ast->u.type_check.type);
            break;

        case AST_AS_CAST:
            if (ast->u.type_check.expr) {
                visit(s, ast->u.type_check.expr);
            }
            resolve_type_names(s, ast->u.type_check.type);
            break;