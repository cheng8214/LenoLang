// ============================================================================
// ★ 2026-11-10（P4 enum 载荷）：带载荷 enum 的**语义改写**（构造 / switch 匹配 / 跨模块导入）
// ----------------------------------------------------------------------------
// 降级形态（见 semantic.c 的 enum_payload_synthesize）：
//   `enum Msg { progress(int done, int total)  finished(string path) }`
//     ⇒ `struct Msg { int tag; int p0; int p1; string p2 }`（同名，用户看不见这层）
//       载荷槽**按成员分段**：progress 占 p0/p1、finished 占 p2
//   于是：
//     · 构造  `Msg.progress(3, 10)`      ⇒ `new Msg(tag = 0, p0 = 3, p1 = 10)`
//     · 匹配  `switch m { case Msg.progress(d, t) { … } }`
//             ⇒ `switch m.tag { case 0 { var d = m.p0; var t = m.p1; … } }`
//       基址公式只有一处：`sem_payload_member_base()`（构造与匹配必须同口径）
//
// 两种语法节点都要认：
//   `Msg.progress(3, 10)`  **不是** AST_CALL —— parser 把"变量.名字(...)"统一建成了
//   `AST_MODULE_CALL`（见 parser_expr.c 的 parse_dot），`Msg.progress` 不带括号才是
//   AST_MODULE_ACCESS。漏掉 MODULE_CALL 这条路的症状是：
//   「'Msg' 是 struct 类型名，不能直接当接收者用」。
//
// ★ 跨模块（LENOSYMC v44）：消费端 `use m.Msg` 时**没有 AST**（enum_src 恒为 NULL）⇒
//   载荷信息只能来自模块符号表（ModuleEnumSymbol 的 member_param_counts/…，经
//   `semantic_import_payload_enum` 拷进 Symbol）。因此下面一切判据都走 **SemPayloadEnum**
//   这个"视图"：本地声明取 AST，跨模块取符号表 —— 两者字段名/公式完全一致 ✓
// ============================================================================

// ---------------------------------------------------------------------------
// 「带载荷 enum 视图」：本地声明与跨模块导入的唯一统一表示
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;        // enum 名（诊断文案用；构造时也用它当 struct 名）
    int member_count;        // 成员数（tag 取值 0..member_count-1）
    char** member_names;     // 成员名数组（借用，不释放）
    int* counts;             // [成员] = 该成员的参数个数（借用，不释放）
    Ast* esrc;               // 本地声明的 enum AST；跨模块为 NULL（只用于诊断/穷尽提示）
} SemPayloadEnum;

// 第 mi 个成员的载荷基址（= 前面所有成员的参数个数之和）—— **唯一实现**
//   `enum Msg { progress(int,int)  finished(string) }`
//     progress ⇒ 0（p0/p1）、finished ⇒ 2（p2）
//   ⚠ 构造（步 3）与 case 绑定（步 4）必须用同一个公式，否则读错槽 = 静默错值
static int sem_payload_member_base(const SemPayloadEnum* v, int mi) {
    if (!v || !v->counts || mi <= 0) return 0;
    int base = 0;
    for (int i = 0; i < mi && i < v->member_count; i++) base += v->counts[i];
    return base;
}

// 成员名 → 序号（= tag 值）；没有 ⇒ -1
static int sem_payload_member_index(const SemPayloadEnum* v, const char* name) {
    if (!v || !v->member_names || !name) return -1;
    for (int i = 0; i < v->member_count; i++) {
        if (v->member_names[i] && strcmp(v->member_names[i], name) == 0) return i;
    }
    return -1;
}

// 从符号取视图：本地声明（type_decl_ast → struct.enum_src）或跨模块（Symbol 上的载荷表）
static int sem_payload_view_from_symbol(Symbol* sym, SemPayloadEnum* out) {
    if (!sym) return 0;
    Ast* sd = (Ast*)sym->type_decl_ast;
    if (sd && sd->kind == AST_STRUCT_DEF && sd->u.struct_def.enum_src) {
        Ast* ed = sd->u.struct_def.enum_src;
        if (ed->kind != AST_ENUM_DEF) return 0;
        out->name = ed->u.enum_def.name;
        out->member_count = ed->u.enum_def.member_count;
        out->member_names = ed->u.enum_def.member_names;
        out->counts = ed->u.enum_def.member_param_counts;
        out->esrc = ed;
        return out->name && out->member_names && out->counts && out->member_count > 0;
    }
    if (sym->enum_payload_counts && sym->enum_payload_member_count > 0) {
        out->name = sym->name;
        out->member_count = sym->enum_payload_member_count;
        out->member_names = sym->enum_value_names;   // 导入时一起填的成员名
        out->counts = sym->enum_payload_counts;
        out->esrc = NULL;
        return out->name && out->member_names;
    }
    return 0;
}

// 按**类型名**取视图（变量声明类型、enum 名）
static int sem_payload_view_by_name(Semantic* s, const char* type_name, SemPayloadEnum* out) {
    if (!s || !type_name || !type_name[0]) return 0;
    Symbol* ts = scope_resolve(s->current, type_name);
    if (!ts && s->root_scope) ts = scope_resolve(s->root_scope, type_name);
    return sem_payload_view_from_symbol(ts, out);
}

// `Enum.成员(…)` 的**统一识别**（AST_CALL 与 AST_MODULE_CALL 两种形状都认）
//   命中 ⇒ 1，view 填好、实参列表写到 out_args
static int sem_payload_view_of_pattern(Semantic* s, Ast* v, SemPayloadEnum* out,
                                       AstList** out_args) {
    if (!s || !v) return 0;
    const char* ename = NULL;
    const char* mname = NULL;
    AstList* args = NULL;
    if (v->kind == AST_CALL) {
        Ast* callee = v->u.call.callee;
        if (!callee || callee->kind != AST_MODULE_ACCESS) return 0;
        if (v->u.call.generic_type_args || v->u.call.generic_type_names) return 0;
        ename = callee->u.module_access.module_name;
        mname = callee->u.module_access.member_name;
        args = &v->u.call.args;
    } else if (v->kind == AST_MODULE_CALL) {
        if (v->u.module_call.generic_type_args || v->u.module_call.generic_type_names) return 0;
        ename = v->u.module_call.module_name;
        mname = v->u.module_call.method_name;
        args = &v->u.module_call.args;
    } else {
        return 0;
    }
    if (!ename || !mname) return 0;
    Symbol* esym = scope_resolve(s->current, ename);
    if (!esym) return 0;
    if (!sem_payload_view_from_symbol(esym, out)) return 0;
    if (sem_payload_member_index(out, mname) < 0) return 0;   // 名字不对 ⇒ 交给原路径报错
    if (out_args) *out_args = args;
    return 1;
}

static Ast* sem_payload_new_int(int value, int line) {
    Ast* n = ast_new(AST_NUM, line);
    if (!n) return NULL;
    n->u.num.value = (double)value;
    n->u.num.is_float = 0;
    n->u.num.is_bigint = 0;
    n->u.num.bigint_str = NULL;
    n->cached_type = type_new(TYPE_INT);
    return n;
}

// `变量名.字段名`（字段索引留给 visit_field_access 解析）
static Ast* sem_payload_make_field_access(const char* obj_name, const char* field, int line) {
    Ast* obj = ast_new(AST_VAR, line);
    if (!obj) return NULL;
    obj->u.var.name = strdup(obj_name);
    Ast* fa = ast_new(AST_FIELD_ACCESS, line);
    if (!fa) {
        ast_free(obj);
        return NULL;
    }
    fa->u.field_access.obj = obj;
    fa->u.field_access.field_name = strdup(field);
    fa->u.field_access.field_index = -1;
    return fa;
}

// 把 n 条语句按顺序插到语句块**最前面**（槽位/作用域都还没有分配 ⇒ visit 时会正常处理）
static void sem_payload_block_prepend(Ast* body, Ast** nodes, int n) {
    if (!body || body->kind != AST_BLOCK || n <= 0) return;
    AstList* bl = &body->u.block;
    int need = bl->count + n;
    if (need > bl->capacity) {
        int cap = bl->capacity > 0 ? bl->capacity : 8;
        while (cap < need) cap *= 2;
        Ast** grown = (Ast**)realloc(bl->items, sizeof(Ast*) * (size_t)cap);
        if (!grown) {
            error_add_at_fmt(ERR_RUNTIME, 0, 0, "内存分配失败");
            return;
        }
        bl->items = grown;
        bl->capacity = cap;
    }
    memmove(bl->items + n, bl->items, sizeof(Ast*) * (size_t)bl->count);
    for (int i = 0; i < n; i++) bl->items[i] = nodes[i];
    bl->count = need;
}

// `Enum.成员(实参…)` 已识别 ⇒ 把 ast（AST_CALL，union 已被腾空）改写成
//   `new Enum(tag = mi, p<base+k> = 实参 k)`，然后走正常的 struct 初始化检查。
static void sem_payload_emit_struct_init(Semantic* s, Ast* ast, const SemPayloadEnum* v,
                                         int mi, AstList args, const char* mname) {
    // ★ 2026-11-10（泛型载荷 enum，步 2/3）：**构造暂不支持** —— 别让它落到
    //   `new Res(tag=…)` 上再报"泛型 struct 'Res' 需要类型参数（如 Res[类型]）"
    //   （那句会把人引到一个**解析不进去**的语法上：`Res[int].ok(…)` 目前是语法错误）。
    //   类型实参从**期望类型**推断（var 声明 / return / 赋值右侧都会挂一个，见 Semantic.expected_type）。
    int gen_n = 0;
    TypeInfo** gen_args = NULL;
    if (v->esrc && v->esrc->u.enum_def.type_param_count > 0) {
        TypeInfo* exp = s ? s->expected_type : NULL;
        int matched = (exp && exp->kind == TYPE_STRUCT && exp->struct_name &&
                       strcmp(exp->struct_name, v->name) == 0 && exp->generic_count > 0);
        if (!matched) {
            error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                "泛型载荷 enum '%s' 的构造**推不出类型实参**：`%s.%s(…)` 只能从上下文推断，"
                "认这四处 —— 带类型的声明（`%s[类型] x = %s.%s(…)`）、函数 return（签名写了返回类型）、"
                "给已声明变量赋值、以及 `… as %s[类型]`。"
                "直接塞进比较（`%s.ok(1) == y`）或函数实参里推不出来 ⇒ 先赋给一个带类型的局部量再传；"
                "`%s[int].%s(…)` 这种显式写法还没进解析器",
                v->name, v->name, mname, v->name, v->name, mname, v->name,
                v->name, v->name, mname);
            semantic_poison_expr(ast);
            for (int k = 0; k < (int)args.count; k++) ast_free(args.items[k]);
            free(args.items);
            memset(&ast->u, 0, sizeof(ast->u));
            ast->kind = AST_CALL;
            return;
        }
        if (exp->generic_count != v->esrc->u.enum_def.type_param_count) {
            error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                "泛型载荷 enum '%s' 需要 %d 个类型实参，上下文给的是 %d 个",
                v->name, v->esrc->u.enum_def.type_param_count, exp->generic_count);
        }
        gen_n = exp->generic_count;
        gen_args = (TypeInfo**)calloc((size_t)gen_n, sizeof(TypeInfo*));
        for (int i = 0; i < gen_n; i++) {
            gen_args[i] = exp->generic_args ? type_copy(exp->generic_args[i]) : NULL;
        }
    }

    int want = v->counts[mi];
    int got = args.count;
    for (int i = 0; i < got; i++) {
        Ast* a = args.items[i];
        if (a && a->kind == AST_ASSIGN) {
            error_add_at_fmt(ERR_SEMANTIC, a->line, a->column,
                "带载荷 enum 的成员构造不支持命名实参（`%s.%s(名字 = 值)`）—— "
                "请按声明顺序传：`%s.%s(实参…)`",
                v->name, mname, v->name, mname);
            semantic_poison_expr(ast);
            for (int k = 0; k < got; k++) ast_free(args.items[k]);
            free(args.items);
            // ⚠ callee 已被调用方释放（见 sem_enum_payload_rewrite_call 的顺序）：
            //   这里必须把它清空，否则后续 ast_free 会二次释放、infer_expr_type 会读悬垂指针。
            memset(&ast->u, 0, sizeof(ast->u));
            ast->kind = AST_CALL;
            return;
        }
    }
    if (got != want) {
        error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
            "带载荷 enum 成员 '%s.%s' 需要 %d 个参数，实际给了 %d 个",
            v->name, mname, want, got);
    }

    int use_n = got < want ? got : want;
    int base = sem_payload_member_base(v, mi);

    memset(&ast->u, 0, sizeof(ast->u));
    ast->kind = AST_STRUCT_INIT;
    ast->u.struct_init.struct_name = strdup(v->name);
    ast->u.struct_init.from_enum_payload = 1;
    // 泛型载荷 enum：把推断出来的类型实参挂到改写后的 `new R[...]()` 上
    if (gen_n > 0 && gen_args) {
        ast->u.struct_init.generic_type_count = gen_n;
        ast->u.struct_init.generic_type_args = gen_args;
        gen_args = NULL;
    }
    ast->u.struct_init.field_count = 1 + use_n;
    ast->u.struct_init.field_names = (char**)calloc((size_t)(1 + use_n), sizeof(char*));
    ast->u.struct_init.field_values = (Ast**)calloc((size_t)(1 + use_n), sizeof(Ast*));
    ast->u.struct_init.field_names[0] = strdup("tag");
    ast->u.struct_init.field_values[0] = sem_payload_new_int(mi, ast->line);
    for (int i = 0; i < use_n; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "p%d", base + i);
        ast->u.struct_init.field_names[i + 1] = strdup(buf);
        ast->u.struct_init.field_values[i + 1] = args.items[i];
    }
    // 多给的实参：已经报过错了，直接丢弃（否则会变成"字段不存在"的级联错）
    for (int i = use_n; i < got; i++) ast_free(args.items[i]);
    free(args.items);

    // 改写后的节点交给正常路径做类型检查/字段索引（AST_STRUCT_INIT）
    visit(s, ast);
}

// ============================================================================
// 步 3/6：构造改写（两种节点形状各一个入口）
//   返回 1 = 已识别并改写（内部已经 visit 过改写后的节点，调用方直接 return）；
//        0 = 与带载荷 enum 无关，调用方走原路径。
// ============================================================================
static int sem_enum_payload_rewrite_call(Semantic* s, Ast* ast) {
    if (!s || !ast || ast->kind != AST_CALL) return 0;
    Ast* callee = ast->u.call.callee;
    if (!callee || callee->kind != AST_MODULE_ACCESS) return 0;
    const char* ename = callee->u.module_access.module_name;
    const char* mname = callee->u.module_access.member_name;
    if (!ename || !mname) return 0;

    Symbol* esym = scope_resolve(s->current, ename);
    if (!esym) return 0;
    SemPayloadEnum v;
    memset(&v, 0, sizeof(v));
    if (!sem_payload_view_from_symbol(esym, &v)) return 0;

    // 泛型实参的写法不在本期范围：交给原路径（会报"没有这个方法"之类），不静默改写
    if (ast->u.call.generic_type_args || ast->u.call.generic_type_names) return 0;

    int mi = sem_payload_member_index(&v, mname);
    if (mi < 0) {
        error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
            "带载荷 enum '%s' 里没有成员 '%s'", v.name, mname);
        semantic_poison_expr(ast);
        return 1;
    }

    AstList args = ast->u.call.args;
    // 丢掉旧的调用信息（callee 与 args 外壳；arg_eval 只在命名实参下才有）
    ast_free(callee);
    free(ast->u.call.arg_eval);
    free(ast->u.call.arg_eval_slots);
    if (ast->cached_type) {
        type_free(ast->cached_type);
        ast->cached_type = NULL;
    }
    sem_payload_emit_struct_init(s, ast, &v, mi, args, mname);
    return 1;
}

// `Msg.progress(3, 10)` 的实际节点是 AST_MODULE_CALL ⇒ 先转成 AST_CALL + MODULE_ACCESS
//   callee，再交给上面那个统一实现（两边的改写逻辑只写一份）。
static int sem_enum_payload_rewrite_module_call(Semantic* s, Ast* ast) {
    if (!s || !ast || ast->kind != AST_MODULE_CALL) return 0;
    const char* ename = ast->u.module_call.module_name;
    const char* mname = ast->u.module_call.method_name;
    if (!ename || !mname) return 0;

    Symbol* esym = scope_resolve(s->current, ename);
    if (!esym) return 0;
    SemPayloadEnum v;
    memset(&v, 0, sizeof(v));
    if (!sem_payload_view_from_symbol(esym, &v)) return 0;
    if (ast->u.module_call.generic_type_args || ast->u.module_call.generic_type_names) return 0;

    char* en = ast->u.module_call.module_name;
    char* mn = ast->u.module_call.method_name;
    AstList args = ast->u.module_call.args;
    if (ast->cached_type) {
        type_free(ast->cached_type);
        ast->cached_type = NULL;
    }

    Ast* callee = ast_new(AST_MODULE_ACCESS, ast->line);
    if (!callee) return 0;
    callee->column = ast->column;
    callee->u.module_access.module_name = en;
    callee->u.module_access.member_name = mn;
    callee->u.module_access.ref.kind = SYM_GLOBAL;
    callee->u.module_access.ref.index = -1;

    memset(&ast->u, 0, sizeof(ast->u));
    ast->kind = AST_CALL;
    ast->u.call.callee = callee;
    ast->u.call.args = args;
    return sem_enum_payload_rewrite_call(s, ast);
}

// ============================================================================
// 步 4/6：switch 模式改写
//   `switch m { case Msg.progress(d, t) { … } }`
//     ⇒ `switch m.tag { case 0 { var d = m.p0; var t = m.p1; … } }`
//   同时给"带载荷 enum 的 switch"补穷尽性警告（老 enum 那条只认 TYPE_ENUM，
//   降级后主题是 int ⇒ 不会触发，见 visit_control.inc 末尾那段）。
// ============================================================================
static void sem_enum_payload_desugar_switch(Semantic* s, Ast* sw) {
    if (!s || !sw || sw->kind != AST_SWITCH) return;
    Ast* subject = sw->u.switch_.expr;
    if (!subject) return;

    // 主题是不是"带载荷 enum 类型的变量"？（不先 visit：这里只想拿静态类型）
    SemPayloadEnum v;
    memset(&v, 0, sizeof(v));
    int have_view = 0;
    if (subject->kind == AST_VAR && subject->u.var.name) {
        SymRef ref;
        memset(&ref, 0, sizeof(ref));
        Symbol* vs = resolve_variable_with_upvalue(s, subject->u.var.name, &ref);
        if (vs && vs->type && vs->type->kind == TYPE_STRUCT && vs->type->struct_name) {
            have_view = sem_payload_view_by_name(s, vs->type->struct_name, &v);
        }
        symref_dispose(&ref);
    }

    int pattern_n = 0;
    for (int i = 0; i < sw->u.switch_.case_count; i++) {
        AstList* vl = &sw->u.switch_.cases[i].values;
        for (int j = 0; j < vl->count; j++) {
            // ⚠ 用临时视图：不能覆盖上面主题的 v（主题视图后面还要用）
            SemPayloadEnum pvscan;
            memset(&pvscan, 0, sizeof(pvscan));
            if (sem_payload_view_of_pattern(s, vl->items[j], &pvscan, NULL)) pattern_n++;
        }
    }
    if (!have_view && pattern_n == 0) {
        return;   // 与带载荷 enum 无关 ⇒ 原路径（零变化 ✓）
    }

    if (!have_view) {
        if (subject->kind == AST_VAR && subject->u.var.name) {
            error_add_at_fmt(ERR_SEMANTIC, sw->line, sw->column,
                "case 里写的是带载荷 enum 的成员模式（`Enum.成员(绑定…)`），但 switch 的主题"
                "（'%s'）不是**带载荷 enum**、或不是同一个 enum —— 模式必须配 "
                "`switch <该 enum 的变量>`", subject->u.var.name);
        } else {
            error_add_at_fmt(ERR_SEMANTIC, sw->line, sw->column,
                "case 里写的是带载荷 enum 的成员模式（`Enum.成员(绑定…)`），因此 switch 主题必须是"
                "**变量**（先 `var m = …` 再 `switch m`）—— 降级后要读 `m.tag` 与 `m.p0/p1`，"
                "表达式会被重复求值、可能拿到不同的值");
        }
        return;
    }
    if (subject->kind != AST_VAR || !subject->u.var.name) {
        error_add_at_fmt(ERR_SEMANTIC, sw->line, sw->column,
            "带载荷 enum 的 switch 主题必须是**变量**（先 `var m = …` 再 `switch m`）—— "
            "降级后要读 `m.tag` 与 `m.p0/p1`，表达式会被重复求值、可能拿到不同的值");
        return;
    }

    const char* subj = subject->u.var.name;
    int member_count = v.member_count;

    for (int i = 0; i < sw->u.switch_.case_count; i++) {
        AstList* vl = &sw->u.switch_.cases[i].values;
        Ast* pat = NULL;
        SemPayloadEnum pv;
        memset(&pv, 0, sizeof(pv));
        AstList* pargs = NULL;
        for (int j = 0; j < vl->count; j++) {
            if (sem_payload_view_of_pattern(s, vl->items[j], &pv, &pargs)) {
                pat = vl->items[j];
                break;
            }
        }
        if (!pat) continue;

        if (vl->count > 1) {
            error_add_at_fmt(ERR_SEMANTIC, pat->line, pat->column,
                "带载荷 enum 的 case 一次只能写**一个**模式（`case Msg.成员(a, b)`）—— "
                "多个模式会各自绑定不同的变量，合并在一个分支里没有确定语义");
            continue;
        }
        if (!pv.name || !v.name || strcmp(pv.name, v.name) != 0) {
            error_add_at_fmt(ERR_SEMANTIC, pat->line, pat->column,
                "case 的模式来自 enum '%s'，而 switch 的主题是 '%s' —— 两者必须是同一个 enum",
                pv.name ? pv.name : "?", v.name ? v.name : "?");
            continue;
        }

        const char* mname = (pat->kind == AST_CALL)
                                ? pat->u.call.callee->u.module_access.member_name
                                : pat->u.module_call.method_name;
        int mi = sem_payload_member_index(&v, mname);
        int want = (mi >= 0) ? v.counts[mi] : 0;
        int got = pargs ? pargs->count : 0;
        if (got != want) {
            error_add_at_fmt(ERR_SEMANTIC, pat->line, pat->column,
                "case 模式 '%s.%s' 需要 %d 个绑定变量，实际写了 %d 个",
                v.name, mname, want, got);
        }

        int use_n = got < want ? got : want;
        int base = sem_payload_member_base(&v, mi >= 0 ? mi : 0);
        Ast** binds = (Ast**)calloc((size_t)(use_n > 0 ? use_n : 1), sizeof(Ast*));
        int bn = 0;
        for (int k = 0; k < use_n; k++) {
            Ast* a = pargs->items[k];
            if (!a) continue;
            if (a->kind != AST_VAR || !a->u.var.name) {
                error_add_at_fmt(ERR_SEMANTIC, a->line, a->column,
                    "case 模式里的绑定必须是**变量名**（如 `case Msg.%s(d, t)`）—— "
                    "这里写的是表达式", mname);
                continue;
            }
            if (strcmp(a->u.var.name, "_") == 0) continue;   // `_` = 只匹配、不绑定
            char fbuf[32];
            snprintf(fbuf, sizeof(fbuf), "p%d", base + k);
            Ast* d = ast_new(AST_VAR_DECL, a->line);
            if (!d) continue;
            d->column = a->column;
            d->u.var_decl.name = strdup(a->u.var.name);
            d->u.var_decl.is_synthetic = 1;   // 不参与"未使用变量"警告（见 visit_var.inc）
            d->u.var_decl.type = type_new(TYPE_INFER);
            d->u.var_decl.init = sem_payload_make_field_access(subj, fbuf, a->line);
            binds[bn++] = d;
        }
        sem_payload_block_prepend(sw->u.switch_.cases[i].body, binds, bn);
        free(binds);

        // case 值换成 tag 常量（长度已校验：这里 vl->count == 1）
        ast_free(pat);
        vl->items[0] = sem_payload_new_int(mi >= 0 ? mi : 0, sw->u.switch_.cases[i].line);
    }

    // 主题改成 `m.tag`：原来的 AST_VAR 节点直接当 obj 复用（不新建、不泄漏）
    {
        Ast* fa = ast_new(AST_FIELD_ACCESS, subject->line);
        if (fa) {
            fa->column = subject->column;
            fa->u.field_access.obj = subject;
            fa->u.field_access.field_name = strdup("tag");
            fa->u.field_access.field_index = -1;
            sw->u.switch_.expr = fa;
        }
    }

    // 穷尽性（tag 取值集合 = 成员序号 0..n-1）
    if (!sw->u.switch_.default_body) {
        int* covered = (int*)calloc((size_t)(member_count > 0 ? member_count : 1), sizeof(int));
        int unknown = 0;
        int any_case = 0;
        for (int i = 0; covered && i < sw->u.switch_.case_count && !unknown; i++) {
            if (sw->u.switch_.cases[i].is_type_match) { unknown = 1; break; }
            AstList* vl = &sw->u.switch_.cases[i].values;
            for (int j = 0; j < vl->count && !unknown; j++) {
                Ast* cv = vl->items[j];
                if (!cv || cv->kind != AST_NUM || cv->u.num.is_float || cv->u.num.is_bigint) {
                    unknown = 1;      // 认不出的形状 ⇒ 宁漏勿误报
                    break;
                }
                int idx = (int)cv->u.num.value;
                if (idx >= 0 && idx < member_count) covered[idx] = 1;
                any_case = 1;
            }
        }
        if (covered && !unknown && any_case) {
            char missing[192];
            missing[0] = '\0';
            int missing_n = 0;
            for (int mi = 0; mi < member_count; mi++) {
                if (covered[mi]) continue;
                missing_n++;
                if (missing_n <= 6 && v.member_names && v.member_names[mi]) {
                    if (missing[0]) {
                        strncat(missing, "、", sizeof(missing) - strlen(missing) - 1);
                    }
                    strncat(missing, v.member_names[mi],
                            sizeof(missing) - strlen(missing) - 1);
                }
            }
            if (missing_n > 0) {
                warning_add_at_fmt(WARN_SWITCH_NOT_EXHAUSTIVE, sw->line, sw->column,
                    "switch 的主题是带载荷 enum '%s'，但少了 %d 个成员（%s%s）且没有 default："
                    "这些成员会直接穿过去、什么都不做；请补 case，或加 default"
                    "（哪怕只写 throw / 赋一个兜底值）",
                    v.name, missing_n, missing, missing_n > 6 ? " 等" : "");
            }
        }
        if (covered) free(covered);
    }
}

// ============================================================================
// 跨模块：`use m.Msg`（带载荷 enum）⇒ 在**当前作用域**造出同一套结构
// ----------------------------------------------------------------------------
// 为什么不是"把合成 struct 也放进模块符号表"：
//   .lenosymc 是在**语义之前**（get_shared → scan）按 parser AST 写盘的，而合成 struct
//   是语义阶段才造的 ⇒ 落不进缓存；改成让语义阶段回写符号表又会引入"缓存与内存两份"的
//   时序坑。这里改成"消费端按载荷表**自己重建**"：字段名/槽位公式与生产端完全一致，
//   运行期的结构定义仍由生产模块 import 时 `OP_STRUCT_DEF` 注册（名字相同 ⇒ 对得上 ✓）。
// ============================================================================
static int semantic_import_payload_enum(Semantic* s, const char* name,
                                        const ModuleEnumSymbol* es, int line, int column) {
    if (!s || !name || !es) return 0;

    // ① 幂等 + **同名不同形**检查
    //   幂等：类型归一化会对同一类型跑不止一遍（见 semantic_import_use_symbol 的说明）
    //   ★ 2026-11-10（压测发现）：两个模块各导出一个同名**带载荷** enum 时，原来直接
    //     当"幂等"放行 ⇒ 编译期默默挑一个、**运行期**才由 def_owner_conflict 报错 ——
    //     而且报错发生在 main 跑完之后（实测：先打印了 "ok" 才报"跨模块同名 struct"，
    //     那段代码已经拿着其中一份定义跑过了）。这里按成员名 + 参数个数比形状：
    //     一致才放行（与 def_owner_conflict 同口径），不一致就在**编译期**报。
    Symbol* existing = scope_resolve_local(s->current, name);
    if (existing && existing->enum_payload_counts) {
        const char* ex_name = existing->struct_type_name;
        if (!ex_name || strcmp(ex_name, name) == 0) {
            int same = (existing->enum_value_count == es->member_count);
            for (int m = 0; same && m < es->member_count; m++) {
                const char* en = existing->enum_value_names ? existing->enum_value_names[m] : NULL;
                const char* nn = es->member_names ? es->member_names[m] : NULL;
                int ep = existing->enum_payload_counts ? existing->enum_payload_counts[m] : 0;
                int np = es->member_param_counts ? es->member_param_counts[m] : 0;
                if (ep != np) same = 0;
                else if ((en == NULL) != (nn == NULL)) same = 0;
                else if (en && strcmp(en, nn) != 0) same = 0;
            }
            if (same) return 1;
            error_add_at_fmt(ERR_DUPLICATE_VAR, line, column,
                "两个模块都导出了**带载荷 enum '%s'**，而且成员对不上 —— 全局类型表按名字索引，"
                "运行期只能保留一份（保留先注册的），混用会静默读到对方的 tag / 载荷槽。"
                "请给其中一个改名，或别 `use`、改用限定名（`a.%s` / `b.%s`）",
                name, name, name);
            return 0;
        }
    }

    int mc = es->member_count;
    int total = 0;
    for (int m = 0; m < mc; m++) {
        int pc = es->member_param_counts ? es->member_param_counts[m] : 0;
        if (pc < 0) pc = 0;
        total += pc;
    }

    Symbol* sym = scope_define(s->current, name, SYM_STRUCT);
    if (!sym) {
        Symbol* dup = scope_resolve_local(s->current, name);
        error_add_at_fmt(ERR_DUPLICATE_VAR, line, column,
            "类型导入失败：'%s' 已在当前作用域中定义（带载荷 enum 会占用**同名 struct**）"
            "—— 请先 `import` 再 `use`，或给两边的类型改名", name);
        (void)dup;
        return 0;
    }

    sym->type = type_new(TYPE_STRUCT);
    sym->type->struct_name = strdup(name);
    sym->struct_type_name = strdup(name);

    // ② 字段表：tag + 按成员分段的载荷槽（与生产端 semantic.c 的降级**同一公式**）
    int fc = 1 + total;
    sym->struct_field_count = fc;
    sym->struct_field_names = (char**)calloc((size_t)fc, sizeof(char*));
    sym->struct_field_types = (TypeInfo**)calloc((size_t)fc, sizeof(TypeInfo*));
    sym->struct_field_null_default = (int*)calloc((size_t)fc, sizeof(int));
    sym->struct_field_names[0] = strdup("tag");
    sym->struct_field_types[0] = type_new(TYPE_INT);
    int pidx = 1;
    for (int m = 0; m < mc; m++) {
        int pc = es->member_param_counts ? es->member_param_counts[m] : 0;
        for (int k = 0; k < pc; k++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "p%d", pidx - 1);
            sym->struct_field_names[pidx] = strdup(buf);
            TypeInfo* pt = (es->member_param_types && es->member_param_types[m])
                               ? es->member_param_types[m][k] : NULL;
            sym->struct_field_types[pidx] = pt ? type_copy(pt) : type_new(TYPE_ANY);
            if (pt && pt->nullable) sym->struct_field_null_default[pidx] = 1;
            pidx++;
        }
    }

    // ③ 成员名 + 载荷个数：之后 `Msg.成员(...)` / `case Msg.成员(...)` / 穷尽性都靠它们
    sym->enum_payload_member_count = mc;
    sym->enum_payload_counts = (int*)calloc((size_t)(mc > 0 ? mc : 1), sizeof(int));
    for (int m = 0; m < mc; m++) {
        sym->enum_payload_counts[m] = (es->member_param_counts ? es->member_param_counts[m] : 0);
    }
    if (es->member_names) {
        sym->enum_value_count = mc;
        sym->enum_value_names = (char**)calloc((size_t)(mc > 0 ? mc : 1), sizeof(char*));
        sym->enum_values = (int64_t*)calloc((size_t)(mc > 0 ? mc : 1), sizeof(int64_t));
        for (int m = 0; m < mc; m++) {
            sym->enum_value_names[m] = es->member_names[m] ? strdup(es->member_names[m]) : NULL;
            sym->enum_values[m] = es->member_values ? es->member_values[m] : m;
        }
    }
    return 1;
}
