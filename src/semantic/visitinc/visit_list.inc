void visit_list(Semantic* s, AstList* list) {
    // ★★ 2026-10-10 新增：**不可达代码检查**（WARN_UNREACHABLE）就放在这里 ✓
    //   为什么是这里（踩过一次才定下来的落点 ✗）：这个警告种类原先**只有枚举与名字表**、
    //     全仓零调用点 ✗（= 检查从未实现 ✓）。我第一版把它加在 `visit_block.inc` 的
    //     `case AST_BLOCK:` 里 ⇒ **循环体 / if 体 / case 体全都不报** ✗ —— 因为那些块体
    //     是走 `visit_list(s, &body->u.block)` 直接进来的 ✓ **绕过了 AST_BLOCK 分支** ✗。
    //     `visit_list` 是**所有块体**的唯一入口 ✓（见 visit_control.inc 的 if/while/for/switch ✓）
    //     ⇒ 放这里一处即全覆盖 ✓，也不会有"两条路径各报一次"的重复 ✓。
    //   口径：同一条列表里，终止语句（return / return a,b / throw / break / continue）
    //     后面**紧邻**的那一条永远不会执行 ⇒ 报 ✓；**一案只报第一条** ✓（免得一段里刷屏 ✓）。
    //   ⚠ 本函数也用于**实参列表**（u.call.args ✓）⇒ 会被顺带扫一遍 ✓ 但那里面不可能出现
    //     终止语句 ✓ ⇒ 不会误报 ✓（0 成本 ✓ 不必为它加分支 ✓）。
    for (int ui = 0; ui + 1 < list->count; ui++) {
        Ast* ucur = list->items[ui];
        Ast* unxt = list->items[ui + 1];
        if (!ucur || !unxt) continue;
        const char* uwhat = NULL;
        if (ucur->kind == AST_RETURN || ucur->kind == AST_RETURN_MULTI) uwhat = "return";
        else if (ucur->kind == AST_THROW) uwhat = "throw";
        else if (ucur->kind == AST_BREAK) uwhat = "break";
        else if (ucur->kind == AST_CONTINUE) uwhat = "continue";
        if (!uwhat) continue;
        char urmsg[BUFFER_MEDIUM];
        snprintf(urmsg, sizeof(urmsg),
                 "这条语句在 '%s' 之后，永远不会被执行（不可达代码）", uwhat);
        warning_add_at(WARN_UNREACHABLE, unxt->line, unxt->column, urmsg);
        break;
    }

    for (int i = 0; i < list->count; i++) {
        visit(s, list->items[i]);
    }
}
