// ============================================================================
// ★ 2026-11-10（P4 enum 载荷 步 3/6 + 步 4/6）：带载荷 enum 的两处**改写**
// ----------------------------------------------------------------------------
// 降级形态（见 semantic.c 的 enum_payload_synthesize）：
//   `enum Msg { progress(int done, int total)  finished(string path) }`
//     ⇒ `struct Msg { int tag; int p0; int p1; string p2 }`（同名，用户看不见这层）
//       载荷槽**按成员分段**：progress 占 p0/p1、finished 占 p2（见 semantic.c 的布局说明）
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
// 判据"这是不是带载荷 enum 的隐藏 struct"**只有一个**：
//   `AST_STRUCT_DEF` 的 `enum_src != NULL`（见 leno_ast.h 的说明）。
// ============================================================================

// 名字是带载荷 enum 的隐藏 struct 名 ⇒ 返回那条 struct 定义 AST（否则 NULL）
static Ast* sem_payload_enum_struct_of_type(Semantic* s, const char* type_name) {
    if (!s || !type_name || !type_name[0]) return NULL;
    Symbol* ts = scope_resolve(s->current, type_name);
    if (!ts && s->root_scope) ts = scope_resolve(s->root_scope, type_name);
    if (!ts) return NULL;
    Ast* sd = (Ast*)ts->type_decl_ast;
    if (sd && sd->kind == AST_STRUCT_DEF && sd->u.struct_def.enum_src) return sd;
    return NULL;
}

// 成员名 → 序号（= tag 值）；没有 ⇒ -1
static int sem_payload_member_index(const Ast* esrc, const char* name) {
    if (!esrc || esrc->kind != AST_ENUM_DEF || !name) return -1;
    for (int i = 0; i < esrc->u.enum_def.member_count; i++) {
        if (esrc->u.enum_def.member_names[i] &&
            strcmp(esrc->u.enum_def.member_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

// 第 mi 个成员的载荷基址（= 前面所有成员的参数个数之和）—— **唯一实现**
//   `enum Msg { progress(int,int)  finished(string) }`
//     progress ⇒ 0（p0/p1）、finished ⇒ 2（p2）
//   ⚠ 构造（步 3）与 case 绑定（步 4）必须用同一个公式，否则读错槽 = 静默错值
static int sem_payload_member_base(const Ast* esrc, int mi) {
    if (!esrc || esrc->kind != AST_ENUM_DEF || mi <= 0) return 0;
    int base = 0;
    for (int i = 0; i < mi && i < esrc->u.enum_def.member_count; i++) {
        base += esrc->u.enum_def.member_param_counts[i];
    }
    return base;
}

// `Enum.成员(…)` 的**统一识别**（AST_CALL 与 AST_MODULE_CALL 两种形状都认）
//   命中 ⇒ 返回那条隐藏 struct 定义，并把成员名 / 实参列表写到出参（都可为 NULL）
//   没命中（不是带载荷 enum 的成员调用 / 带泛型实参 / 名字不存在）⇒ NULL
static Ast* sem_payload_enum_of_pattern(Semantic* s, Ast* v,
                                        const char** out_mname, AstList** out_args) {
    if (!s || !v) return NULL;
    const char* ename = NULL;
    const char* mname = NULL;
    AstList* args = NULL;
    if (v->kind == AST_CALL) {
        Ast* callee = v->u.call.callee;
        if (!callee || callee->kind != AST_MODULE_ACCESS) return NULL;
        if (v->u.call.generic_type_args || v->u.call.generic_type_names) return NULL;
        ename = callee->u.module_access.module_name;
        mname = callee->u.module_access.member_name;
        args = &v->u.call.args;
    } else if (v->kind == AST_MODULE_CALL) {
        if (v->u.module_call.generic_type_args || v->u.module_call.generic_type_names) return NULL;
        ename = v->u.module_call.module_name;
        mname = v->u.module_call.method_name;
        args = &v->u.module_call.args;
    } else {
        return NULL;
    }
    if (!ename || !mname) return NULL;
    Symbol* esym = scope_resolve(s->current, ename);
    if (!esym) return NULL;
    Ast* sd = (Ast*)esym->type_decl_ast;
    if (!sd || sd->kind != AST_STRUCT_DEF || !sd->u.struct_def.enum_src) return NULL;
    if (sem_payload_member_index(sd->u.struct_def.enum_src, mname) < 0) return NULL;
    if (out_mname) *out_mname = mname;
    if (out_args) *out_args = args;
    return sd;
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

// `Enum.成员(实参…)` 已识别（sd/esrc/mi 确定）⇒ 把 ast（AST_CALL，union 已被腾空）
//   改写成 `new Enum(tag = mi, p<base+k> = 实参 k)`，然后走正常的 struct 初始化检查。
static void sem_payload_emit_struct_init(Semantic* s, Ast* ast, Ast* sd, Ast* esrc,
                                         int mi, AstList args, const char* mname) {
    int want = esrc->u.enum_def.member_param_counts[mi];
    int got = args.count;
    for (int i = 0; i < got; i++) {
        Ast* a = args.items[i];
        if (a && a->kind == AST_ASSIGN) {
            error_add_at_fmt(ERR_SEMANTIC, a->line, a->column,
                "带载荷 enum 的成员构造不支持命名实参（`%s.%s(名字 = 值)`）—— "
                "请按声明顺序传：`%s.%s(实参…)`",
                esrc->u.enum_def.name, mname, esrc->u.enum_def.name, mname);
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
            esrc->u.enum_def.name, mname, want, got);
    }

    int use_n = got < want ? got : want;
    int base = sem_payload_member_base(esrc, mi);

    memset(&ast->u, 0, sizeof(ast->u));
    ast->kind = AST_STRUCT_INIT;
    ast->u.struct_init.struct_name = strdup(sd->u.struct_def.name);
    ast->u.struct_init.from_enum_payload = 1;
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
    Ast* sd = (Ast*)esym->type_decl_ast;
    if (!sd || sd->kind != AST_STRUCT_DEF || !sd->u.struct_def.enum_src) return 0;

    // 泛型实参的写法不在本期范围：交给原路径（会报"没有这个方法"之类），不静默改写
    if (ast->u.call.generic_type_args || ast->u.call.generic_type_names) return 0;

    Ast* esrc = sd->u.struct_def.enum_src;
    int mi = sem_payload_member_index(esrc, mname);
    if (mi < 0) {
        error_add_at_fmt(ERR_UNDEFINED_VAR, ast->line, ast->column,
            "带载荷 enum '%s' 里没有成员 '%s'", esrc->u.enum_def.name, mname);
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
    sem_payload_emit_struct_init(s, ast, sd, esrc, mi, args, mname);
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
    Ast* sd = (Ast*)esym->type_decl_ast;
    if (!sd || sd->kind != AST_STRUCT_DEF || !sd->u.struct_def.enum_src) return 0;
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
    Ast* sd = NULL;
    if (subject->kind == AST_VAR && subject->u.var.name) {
        SymRef ref;
        memset(&ref, 0, sizeof(ref));
        Symbol* vs = resolve_variable_with_upvalue(s, subject->u.var.name, &ref);
        if (vs && vs->type && vs->type->kind == TYPE_STRUCT && vs->type->struct_name) {
            sd = sem_payload_enum_struct_of_type(s, vs->type->struct_name);
        }
        symref_dispose(&ref);
    }

    int pattern_n = 0;
    for (int i = 0; i < sw->u.switch_.case_count; i++) {
        AstList* vl = &sw->u.switch_.cases[i].values;
        for (int j = 0; j < vl->count; j++) {
            if (sem_payload_enum_of_pattern(s, vl->items[j], NULL, NULL)) pattern_n++;
        }
    }
    if (!sd && pattern_n == 0) return;   // 与带载荷 enum 无关 ⇒ 原路径（零变化 ✓）

    if (!sd) {
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
    Ast* esrc = sd->u.struct_def.enum_src;
    int member_count = esrc->u.enum_def.member_count;

    for (int i = 0; i < sw->u.switch_.case_count; i++) {
        AstList* vl = &sw->u.switch_.cases[i].values;
        const char* mname = NULL;
        AstList* args = NULL;
        Ast* pat = NULL;
        for (int j = 0; j < vl->count; j++) {
            if (sem_payload_enum_of_pattern(s, vl->items[j], &mname, &args)) {
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

        Ast* psd = sem_payload_enum_of_pattern(s, pat, NULL, NULL);
        if (psd != sd) {
            error_add_at_fmt(ERR_SEMANTIC, pat->line, pat->column,
                "case 的模式来自 enum '%s'，而 switch 的主题是 '%s' —— 两者必须是同一个 enum",
                psd && psd->u.struct_def.enum_src ? psd->u.struct_def.enum_src->u.enum_def.name : "?",
                esrc->u.enum_def.name);
            continue;
        }

        int mi = sem_payload_member_index(esrc, mname);
        int want = (mi >= 0) ? esrc->u.enum_def.member_param_counts[mi] : 0;
        int got = args ? args->count : 0;
        if (got != want) {
            error_add_at_fmt(ERR_SEMANTIC, pat->line, pat->column,
                "case 模式 '%s.%s' 需要 %d 个绑定变量，实际写了 %d 个",
                esrc->u.enum_def.name, mname, want, got);
        }

        int use_n = got < want ? got : want;
        int base = sem_payload_member_base(esrc, mi >= 0 ? mi : 0);
        Ast** binds = (Ast**)calloc((size_t)(use_n > 0 ? use_n : 1), sizeof(Ast*));
        int bn = 0;
        for (int k = 0; k < use_n; k++) {
            Ast* a = args->items[k];
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
                Ast* v = vl->items[j];
                if (!v || v->kind != AST_NUM || v->u.num.is_float || v->u.num.is_bigint) {
                    unknown = 1;      // 认不出的形状 ⇒ 宁漏勿误报
                    break;
                }
                int idx = (int)v->u.num.value;
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
                if (missing_n <= 6) {
                    if (missing[0]) {
                        strncat(missing, "、", sizeof(missing) - strlen(missing) - 1);
                    }
                    strncat(missing, esrc->u.enum_def.member_names[mi],
                            sizeof(missing) - strlen(missing) - 1);
                }
            }
            if (missing_n > 0) {
                warning_add_at_fmt(WARN_SWITCH_NOT_EXHAUSTIVE, sw->line, sw->column,
                    "switch 的主题是带载荷 enum '%s'，但少了 %d 个成员（%s%s）且没有 default："
                    "这些成员会直接穿过去、什么都不做；请补 case，或加 default"
                    "（哪怕只写 throw / 赋一个兜底值）",
                    esrc->u.enum_def.name, missing_n, missing,
                    missing_n > 6 ? " 等" : "");
            }
        }
        if (covered) free(covered);
    }
}
