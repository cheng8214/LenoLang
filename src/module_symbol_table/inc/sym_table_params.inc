// ============================================================================
// 扫描器侧「参数表 → 类型」解析（唯一实现）
// ============================================================================
// 背景：同一件事（把 `(int a, string b)` 这样的参数表解析成类型）在扫描器里**曾有三份**：
//   ① `scan_struct.inc` 的方法参数段（内联、最完整，还带泛型参数名）；
//   ② `scan_face.inc`（⑰-2 时新写的内联版）；
//   ③ 语义侧 ⑱ 的就地文本解析（`param_text` 逐段 malloc 类型名）。
// 本文件是收敛的起点：② 已改为调用本函数（行为与原内联实现逐条对应，由
// `assert/test_face_method_args.leno` 的 5 判据 + 真实应用 47 模块冷扫守）。
// ① 仍是内联（它额外承担泛型参数名的收集，属"更全"的一份，留待后续单独收）；③ 待 ⑱ 转正时收。
//
// ⚠ 包含位置有讲究：本文件必须在 module_symbol_table.c 的**文件作用域**被包含
//   （见该文件 "类型字符串解析器" 之后的一行），**不能**放进 `sym_table_scan.inc` 里 ——
//   那批 `scan/*.inc` 是在扫描**函数体内**包含的代码片段（因此能直接用 table/source 等局部名），
//   在里面定义函数会得到 "invalid storage class for function"（实测踩过）。
//
// 规则（与 ① 同源）：
//   · 每个参数的**第一个标识符 token 是类型**；若它后面还跟着标识符，后者是参数名；
//     否则该 token 本身是"无类型参数名" ⇒ 记 TYPE_ANY（与 ① 的 TYPE_ANY 兜底一致）。
//   · 类型名依次查：基本类型（`parse_base_type`）→ clib → face → cstruct → struct
//     —— 四张表都查（这正是 ⑧ 那一族"各处认的名字集合不一致"要避免的）。
//   · 认不出一律记 TYPE_ANY：消费方遇到 ANY 会跳过 ⇒ **宁漏不误报**。
//   · 默认值里的括号/方括号/花括号/字符串不影响分段（只有顶层逗号才算参数分隔）。
//
// 单个类型 token 的解析（**唯一实现**）：基本类型 → clib → face → cstruct → struct → 泛型参数名。
// 这是"认哪些名字算类型"的**唯一政策点** —— 扫描器里两条参数走法（本文件的逐段走法、
// `scan_struct.inc` 的逐 token 走法）都调它。⑧ 那一族（"各处认的名字集合不一致"）的根源就在
// 这类地方散成多份，所以即使是"看着重复"的两条走法，名字政策也必须只有一份。
// 输出：out_agg_name = 聚合类型名（可空）、out_generic_name = 泛型参数名（可空）；
//       返回 kind（认不出一律 TYPE_ANY ⇒ 消费方跳过，宁漏不误报）。
static TypeKind mod_resolve_param_type(const char* tok,
                                       char* out_agg_name, int agg_cap,
                                       char* out_generic_name, int gen_cap,
                                       char** clib_names, int clib_count,
                                       char** face_names, int face_count,
                                       char** cstruct_names, int cstruct_count,
                                       char** struct_names, int struct_count,
                                       char** type_param_names, int type_param_count) {
    if (out_agg_name && agg_cap > 0) out_agg_name[0] = '\0';
    if (out_generic_name && gen_cap > 0) out_generic_name[0] = '\0';
    if (!tok || !*tok) return TYPE_ANY;

    TypeKind pt = parse_base_type(tok);
    if (pt == TYPE_ANY) {
        const char* agg = NULL;
        if (is_known_clib(tok, clib_names, clib_count)) { pt = TYPE_CLIB; agg = tok; }
        else if (is_known_face(tok, face_names, face_count)) { pt = TYPE_FACE; agg = tok; }
        else if (is_known_cstruct(tok, cstruct_names, cstruct_count)) { pt = TYPE_CSTRUCT; agg = tok; }
        else if (is_known_struct(tok, struct_names, struct_count)) { pt = TYPE_STRUCT; agg = tok; }
        if (agg && out_agg_name && agg_cap > 0) {
            strncpy(out_agg_name, agg, agg_cap - 1);
            out_agg_name[agg_cap - 1] = '\0';
        }
    }
    // 泛型类型参数（T/K/V…）**覆盖**上面结果：命中所在 struct/方法的泛型参数名即记下名字
    // （下游据此做泛型方法实例化，丢了会坏泛型方法）。
    if (type_param_names && type_param_count > 0) {
        for (int tpi = 0; tpi < type_param_count; tpi++) {
            if (type_param_names[tpi] && strcmp(tok, type_param_names[tpi]) == 0) {
                pt = TYPE_GENERIC_PARAM;
                if (out_generic_name && gen_cap > 0) {
                    strncpy(out_generic_name, tok, gen_cap - 1);
                    out_generic_name[gen_cap - 1] = '\0';
                }
                break;
            }
        }
    }
    return pt;
}

// 返回：解析到的参数个数（≤ max_params）。
// `out_struct_names` 为聚合类型名（按参数下标对齐；非聚合或无名时为空串）；
// `out_generic_names`（可传 NULL）为**泛型类型参数名**（`T`/`K`/`V` 等，按参数下标对齐），
// 命中 `type_param_names` 里某个名字时 kind 记 TYPE_GENERIC_PARAM（这条是 scan_struct 那份
// 内联实现原有的能力，收敛时必须保留）。
// 注：调用方传入的名字表就是扫描器自己的（clib/face/cstruct/struct + 所在 struct/方法的
//     泛型参数名），不复制、不改动。
static int mod_scan_params(const char* param_text,
                           int max_params,
                           TypeKind* out_types,
                           char (*out_struct_names)[MOD_MAX_NAMES],
                           char (*out_generic_names)[MOD_MAX_NAMES],
                           char** clib_names, int clib_count,
                           char** face_names, int face_count,
                           char** cstruct_names, int cstruct_count,
                           char** struct_names, int struct_count,
                           char** type_param_names, int type_param_count) {
    if (!param_text || !out_types || max_params <= 0) return 0;
    // 显式写 TYPE_ANY，不依赖 memset 的 0 —— TypeKind 的第一个枚举值未必是 TYPE_ANY，
    // 靠零值会把"未识别"变成某个真实类型（下游就会去判它，可能假报错）。
    for (int zi = 0; zi < max_params; zi++) out_types[zi] = TYPE_ANY;
    if (out_struct_names) memset(out_struct_names, 0, sizeof(char) * MOD_MAX_NAMES * max_params);
    if (out_generic_names) memset(out_generic_names, 0, sizeof(char) * MOD_MAX_NAMES * max_params);

    const char* pp = param_text;
    int idx = 0;
    while (*pp && idx < max_params) {
        while (*pp == ' ' || *pp == '\t' || *pp == ',' || *pp == '\n' || *pp == '\r') pp++;
        if (!*pp) break;

        // 取第一个标识符 token
        const char* ts = pp;
        while (*pp && (isalnum((unsigned char)*pp) || *pp == '_')) pp++;
        int tl = (int)(pp - ts);
        char tok[64] = {0};
        if (tl > 0 && tl < 64) { memcpy(tok, ts, tl); tok[tl] = '\0'; }

        // 该 token 后面若还跟着标识符 ⇒ 它是类型；否则它是"无类型参数名"
        const char* save = pp;
        while (*pp == ' ' || *pp == '\t') pp++;
        int token_is_type = (*pp && (isalnum((unsigned char)*pp) || *pp == '_'));
        if (!token_is_type) pp = save;

        // 跳到该参数结束（顶层逗号；跳过默认值里的括号与字符串）
        int depth2 = 0;
        while (*pp && !(*pp == ',' && depth2 == 0)) {
            if (*pp == '(' || *pp == '[' || *pp == '{') depth2++;
            else if (*pp == ')' || *pp == ']' || *pp == '}') { if (depth2 > 0) depth2--; }
            else if (*pp == '"') {
                pp++;
                while (*pp && *pp != '"') { if (*pp == '\\' && *(pp + 1)) pp++; pp++; }
            }
            if (*pp && !(*pp == ',' && depth2 == 0)) pp++;
        }
        if (*pp == ',') pp++;

        if (token_is_type && tl > 0) {
            char agg_tmp[MOD_MAX_NAMES] = {0};
            char gen_tmp[MOD_MAX_NAMES] = {0};
            out_types[idx] = mod_resolve_param_type(tok,
                agg_tmp, MOD_MAX_NAMES, gen_tmp, MOD_MAX_NAMES,
                clib_names, clib_count, face_names, face_count,
                cstruct_names, cstruct_count, struct_names, struct_count,
                type_param_names, type_param_count);
            if (out_struct_names && agg_tmp[0]) {
                memcpy(out_struct_names[idx], agg_tmp, MOD_MAX_NAMES - 1);
                out_struct_names[idx][MOD_MAX_NAMES - 1] = '\0';
            }
            if (out_generic_names && gen_tmp[0]) {
                memcpy(out_generic_names[idx], gen_tmp, MOD_MAX_NAMES - 1);
                out_generic_names[idx][MOD_MAX_NAMES - 1] = '\0';
            }
        }
        idx++;
    }
    return idx;
}
