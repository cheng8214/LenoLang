// ============================================================================
// 枚举成员常量表达式求值（符号表扫描阶段用）
// ----------------------------------------------------------------------------
// 为什么需要这一份：模块符号表扫描器是**纯文本**扫描，不能建 AST，但枚举成员值
// 又必须在扫描阶段定下来 —— 跨模块 `use mod.E` 与模块限定的成员值都来自这里。
//
// 历史教训：这里原是一段「简易词法 + 从左到右求值」的临时实现，与解析器
// （parser_func.c 的 eval_const_expr，走 AST 递归下降）**语义不一致**：
//     P = 2 + 3 * 4    → 解析器 14，扫描器 (2+3)*4 = 20
//     Q = 2 * (3 + 4)  → 解析器 14，扫描器 2*3+4 = 10（括号被当未知字符直接跳过）
// 于同一份源码、同一个程序里，成员值随「怎么写」而变（同模块/use/模块限定三个
// 计算点给出不同数）。现改为与解析器**同优先级**的递归下降求值：
//     or_expr    = xor_expr ('|' xor_expr)*
//     xor_expr   = and_expr ('^' and_expr)*
//     and_expr   = shift_expr ('&' shift_expr)*
//     shift_expr = add_expr (('<<'|'>>') add_expr)*
//     add_expr   = mul_expr (('+'|'-') mul_expr)*
//     mul_expr   = unary (('*'|'/'|'%') unary)*
//     unary      = ('-'|'+'|'~') unary | primary
//     primary    = 十进制/0x/0b 字面量 | '(' or_expr ')' | 先前已解析的成员名
//
// 求值失败（含残留未识别字符、除零、引用了非成员标识符）返回 0：调用方保持
// 「视为无显式值」的宽松行为，不新增硬错误 —— 模块自身编译时解析器会对不可求值
// 的表达式报「必须是编译期整数常量表达式」，扫描器只管跨模块查询与 LSP。
// ============================================================================

typedef struct {
    const char* p;        // 当前读取位置
    char** names;         // 先前成员名（成员引用）
    int64_t* values;      // 先前成员值
    int name_count;
    int ok;               // 0 = 求值过程中遇到无法处理的东西
} SymEnumExpr;

static void sym_enum_skip_ws(SymEnumExpr* e) {
    while (*e->p == ' ' || *e->p == '\t' || *e->p == '\r' || *e->p == '\n') e->p++;
}

static int64_t sym_enum_parse_or(SymEnumExpr* e);

static int64_t sym_enum_parse_primary(SymEnumExpr* e) {
    sym_enum_skip_ws(e);
    if (*e->p == '(') {
        e->p++;
        int64_t v = sym_enum_parse_or(e);
        sym_enum_skip_ws(e);
        if (*e->p == ')') e->p++;
        else e->ok = 0;
        return v;
    }
    if (*e->p == '0' && (e->p[1] == 'x' || e->p[1] == 'X')) {
        e->p += 2;
        int64_t v = 0;
        int digits = 0;
        while (isxdigit((unsigned char)*e->p)) {
            v = v * 16 + (*e->p <= '9' ? *e->p - '0'
                                       : (*e->p <= 'F' ? *e->p - 'A' + 10 : *e->p - 'a' + 10));
            e->p++;
            digits = 1;
        }
        if (!digits) e->ok = 0;
        return v;
    }
    if (*e->p == '0' && (e->p[1] == 'b' || e->p[1] == 'B')) {
        e->p += 2;
        int64_t v = 0;
        int digits = 0;
        while (*e->p == '0' || *e->p == '1') {
            v = v * 2 + (*e->p - '0');
            e->p++;
            digits = 1;
        }
        if (!digits) e->ok = 0;
        return v;
    }
    if (isdigit((unsigned char)*e->p)) {
        int64_t v = 0;
        while (isdigit((unsigned char)*e->p)) {
            v = v * 10 + (*e->p - '0');
            e->p++;
        }
        return v;
    }
    if (isalpha((unsigned char)*e->p) || *e->p == '_') {
        char ident[64];
        int n = 0;
        while ((isalnum((unsigned char)*e->p) || *e->p == '_') && n < 63) {
            ident[n++] = *e->p++;
        }
        ident[n] = '\0';
        for (int i = 0; i < e->name_count; i++) {
            if (strcmp(e->names[i], ident) == 0) return e->values[i];
        }
        e->ok = 0;   // 不是先前成员（可能是别的常量）⇒ 扫描阶段无法求值
        return 0;
    }
    e->ok = 0;
    return 0;
}

static int64_t sym_enum_parse_unary(SymEnumExpr* e) {
    sym_enum_skip_ws(e);
    if (*e->p == '-') { e->p++; return -sym_enum_parse_unary(e); }
    if (*e->p == '+') { e->p++; return sym_enum_parse_unary(e); }
    if (*e->p == '~') { e->p++; return ~sym_enum_parse_unary(e); }
    return sym_enum_parse_primary(e);
}

static int64_t sym_enum_parse_mul(SymEnumExpr* e) {
    int64_t v = sym_enum_parse_unary(e);
    for (;;) {
        sym_enum_skip_ws(e);
        char c = *e->p;
        if (c != '*' && c != '/' && c != '%') break;
        e->p++;
        int64_t r = sym_enum_parse_unary(e);
        if (c == '*') {
            v *= r;
        } else if (r == 0) {
            e->ok = 0;   // 除零：判失败（解析器同样不接受）
            return 0;
        } else {
            v = (c == '/') ? v / r : v % r;
        }
    }
    return v;
}

static int64_t sym_enum_parse_add(SymEnumExpr* e) {
    int64_t v = sym_enum_parse_mul(e);
    for (;;) {
        sym_enum_skip_ws(e);
        char c = *e->p;
        if (c != '+' && c != '-') break;
        e->p++;
        int64_t r = sym_enum_parse_mul(e);
        v = (c == '+') ? v + r : v - r;
    }
    return v;
}

static int64_t sym_enum_parse_shift(SymEnumExpr* e) {
    int64_t v = sym_enum_parse_add(e);
    for (;;) {
        sym_enum_skip_ws(e);
        if (e->p[0] == '<' && e->p[1] == '<') {
            e->p += 2;
            v <<= sym_enum_parse_add(e);
        } else if (e->p[0] == '>' && e->p[1] == '>') {
            e->p += 2;
            v >>= sym_enum_parse_add(e);
        } else {
            break;
        }
    }
    return v;
}

static int64_t sym_enum_parse_band(SymEnumExpr* e) {
    int64_t v = sym_enum_parse_shift(e);
    for (;;) {
        sym_enum_skip_ws(e);
        if (*e->p != '&' || e->p[1] == '&') break;   // 不支持逻辑 &&
        e->p++;
        v &= sym_enum_parse_shift(e);
    }
    return v;
}

static int64_t sym_enum_parse_bxor(SymEnumExpr* e) {
    int64_t v = sym_enum_parse_band(e);
    for (;;) {
        sym_enum_skip_ws(e);
        if (*e->p != '^') break;
        e->p++;
        v ^= sym_enum_parse_band(e);
    }
    return v;
}

static int64_t sym_enum_parse_or(SymEnumExpr* e) {
    int64_t v = sym_enum_parse_bxor(e);
    for (;;) {
        sym_enum_skip_ws(e);
        if (*e->p != '|' || e->p[1] == '|') break;   // 不支持逻辑 ||
        e->p++;
        v |= sym_enum_parse_bxor(e);
    }
    return v;
}

// 入口：成功返回 1 并写 *out；失败返回 0（包括表达式后还有未识别字符）
static int sym_enum_eval_expr(const char* text, char** names, int64_t* values,
                              int name_count, int64_t* out) {
    if (!text || !out) return 0;
    SymEnumExpr e;
    e.p = text;
    e.names = names;
    e.values = values;
    e.name_count = name_count;
    e.ok = 1;
    int64_t v = sym_enum_parse_or(&e);
    sym_enum_skip_ws(&e);
    if (!e.ok || *e.p != '\0') return 0;
    *out = v;
    return 1;
}
