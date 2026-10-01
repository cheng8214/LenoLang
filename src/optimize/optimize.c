#include "include/lenolang.h"
#include "include/leno_optimize.h"
#include <math.h>
#include <limits.h>
#include <string.h>

/* ============================================================================
 * 编译期条件剪枝：`if target.os == "windows" { ... } eif { ... }`（2026-10-01 新增）
 *
 * 为什么要有它（以及为什么**不是**预处理器）：
 *   Leno 已有运行期判断（`_os()` / `_arch()`），足够覆盖"同一个文件里按平台选 DLL 名"
 *   这类需求 ✓。覆盖不了的是四类**声明/常量级**差异：
 *     ① FFI 签名差异（`_mkdir(Ptr)` vs `mkdir(Ptr, u32)`）：
 *        `clib` 声明是静态的，运行期判断没法表达"这个平台有这个名字、那个平台是另一个"
 *     ② 编译期常量（数组长度、case 标签…）
 *     ③ 避免依赖本平台不存在的模块（Linux 上 `import win_registry` 连语义分析都不该过）
 *     ④ 打包产物里不该进别的平台的代码
 *   ⇒ 做的是"**编译期常量 + 死分支跳过**"，而不是 C 式文本预处理：**两端都解析
 *     （语法错在所有平台都能查出来 ✓），只对活分支做语义分析/代码生成**。
 *
 * ★ 纯增量设计（这条是零风险的关键）：
 *   只有**条件里出现了 `target.*`**时才做编译期判定；任何其它条件（含 `if false`、
 *   `if MY_FLAG`）一律**原样保留**给后面那道 DCE 与语义分析 ⇒
 *   因为 `target` 是本次新增的标识符，**现存代码一个字都不会改变含义** ✓✓
 *
 * ★ 三态：真 / 假 / **未知**。只有"确定"才剪枝；未知 ⇒ 一律不动（=> 向后兼容）✓
 *   条件只允许：`target.{os,arch,family,pointer_bits}`、字面量（字符串/整数/布尔）、
 *   `==` / `!=`、`and` / `or` / `not`。用到别的形态**且提到了 target** ⇒ 报错（不静默）。
 *
 * ★ 实现上**复用现有的 DCE 遍历**（`dce_expr`），不另抄一份 AST 遍历：
 *   本 pass 只把 target 条件**就地折成 true/false 字面量**，再蹭 `dce_expr` 里原有的
 *   "常量条件消除"分支做原地替换 ⇒ 遍历逻辑只有一份 ✓
 *   （`g_dce_target_only` 是它的模式开关：打开时**只**碰 target 条件，
 *     连"终止语句后的死代码裁剪"都不做 ⇒ 语义分析之前绝不动别的 ✓）
 * ========================================================================== */

/* 目标三元组的两半；NULL ⇒ 用**宿主编译目标**（与 _os()/_arch() 同口径 ✓） */
static const char* g_target_os = NULL;
static const char* g_target_arch = NULL;
static char g_target_os_buf[16];
static char g_target_arch_buf[16];

/* 1 = 当前这趟 dce_expr 只做 target 剪枝（语义分析之前那道） */
static int g_dce_target_only = 0;

/* 本条 target 条件是否**已经报过具体错**（如字段名写错）—— 报过就不再补一句笼统的
 *   "必须是编译期可判定的常量表达式"，免得同一处错误说两遍 ✗ */
static int g_tc_diag_reported = 0;

/* 解析目标规格："windows" / "linux-arm64" / "macos" / "linux" / "" / NULL
 *   · 目标**语法**只在这一处实现（CLI 那边只转发字符串 ✓ 单一事实来源）
 *   · 取值与 `_os()` / `_arch()` 同口径：os ∈ {windows, linux, macos}，arch ∈ {x64, arm64} */
int optimize_set_target(const char* spec) {
    g_target_os = NULL;
    g_target_arch = NULL;
    if (!spec || !spec[0]) return 0;                 /* 宿主 ✓ */

    char tmp[32];
    if (strlen(spec) >= sizeof(tmp)) return -1;
    strcpy(tmp, spec);

    char* os_part = tmp;
    char* arch_part = NULL;
    char* dash = strchr(tmp, '-');
    if (dash) { *dash = '\0'; arch_part = dash + 1; }

    if (os_part[0]) {                                /* "-arm64" 这种只给架构也允许 ✓ */
        if (strcmp(os_part, "windows") != 0 &&
            strcmp(os_part, "linux") != 0 &&
            strcmp(os_part, "macos") != 0) {
            return -1;
        }
        strcpy(g_target_os_buf, os_part);            /* 已校验 ⇒ 最长 7 字节 ✓ */
        g_target_os = g_target_os_buf;
    }
    if (arch_part && arch_part[0]) {
        if (strcmp(arch_part, "x64") != 0 && strcmp(arch_part, "arm64") != 0) return -1;
        strcpy(g_target_arch_buf, arch_part);        /* 已校验 ⇒ 最长 5 字节 ✓ */
        g_target_arch = g_target_arch_buf;
    }
    return 0;
}

/* 编译目标 OS：默认宿主（⚠ 编译目标 ≠ 运行期 `_os()`：打包/模拟环境下两者可能不同，
 *   所以这里读的是**编译期宏**，而不是去调运行期的 native_os ✓） */
static const char* target_os(void) {
    if (g_target_os) return g_target_os;
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return NULL;
#endif
}

static const char* target_arch(void) {
    if (g_target_arch) return g_target_arch;
#if defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__)
    return "arm";
#elif defined(__riscv) && (__riscv_xlen == 64)
    return "riscv64";
#else
    return NULL;
#endif
}

/* family 由 os **派生**（不另立一套取值 ⇒ 不会两处不一致 ✓） */
static const char* target_family(void) {
    const char* os = target_os();
    if (!os) return NULL;
    return strcmp(os, "windows") == 0 ? "windows" : "unix";
}

/* 指针位宽由 arch **派生**（同上） */
static long long target_pointer_bits(void) {
    const char* a = target_arch();
    if (!a) return -1;
    if (strcmp(a, "x64") == 0 || strcmp(a, "arm64") == 0 || strcmp(a, "riscv64") == 0) return 64;
    return 32;
}

/* ---- 三态值：求值器只在这一小组形态上有定义 ---- */
enum { TCV_UNKNOWN = 0, TCV_STR = 1, TCV_NUM = 2, TCV_BOOL = 3 };
typedef struct {
    int      kind;   /* TCV_* */
    const char* s;   /* kind==TCV_STR */
    long long   i;   /* kind==TCV_NUM / TCV_BOOL */
} TCVal;

static TCVal tcv_unknown(void) { TCVal v; v.kind = TCV_UNKNOWN; v.s = NULL; v.i = 0; return v; }
static TCVal tcv_str(const char* s) { TCVal v; v.kind = s ? TCV_STR : TCV_UNKNOWN; v.s = s; v.i = 0; return v; }
static TCVal tcv_num(long long i) { TCVal v; v.kind = TCV_NUM; v.s = NULL; v.i = i; return v; }
static TCVal tcv_bool(int b) { TCVal v; v.kind = TCV_BOOL; v.s = NULL; v.i = b ? 1 : 0; return v; }

/* 三态真值：-1 未知 / 0 假 / 1 真 */
static int tcv_truth(TCVal v) {
    if (v.kind == TCV_BOOL || v.kind == TCV_NUM) return v.i != 0;
    if (v.kind == TCV_STR) return (v.s && v.s[0]) ? 1 : 0;
    return -1;
}

static int is_target_var(Ast* e) {
    return e && e->kind == AST_VAR && e->u.var.name && strcmp(e->u.var.name, "target") == 0;
}

/* 解析 `target.<字段>`：base 不是 "target" ⇒ 未知且**不算触碰过 target**（⇒ 不剪枝、不报错 ✓）
 *   取值口径与 `_os()` / `_arch()` 一致：windows / linux / macos、x64 / arm64 ✓ */
static TCVal tc_target_field(const char* base, const char* field, Ast* node, int* saw_target) {
    if (!base || strcmp(base, "target") != 0) return tcv_unknown();
    *saw_target = 1;
    if (!field) return tcv_unknown();

    if (strcmp(field, "os") == 0) return tcv_str(target_os());
    if (strcmp(field, "arch") == 0) return tcv_str(target_arch());
    if (strcmp(field, "family") == 0) return tcv_str(target_family());
    if (strcmp(field, "pointer_bits") == 0) {
        long long bits = target_pointer_bits();
        return bits < 0 ? tcv_unknown() : tcv_num(bits);
    }
    /* 未知字段名 ⇒ **响亮报错**，而不是静默当未知
     *   （静默的话会一路飘到语义层变成"未定义的变量: target"，指向性很差 ✗） */
    {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "target 没有字段 '%s'（可用：os / arch / family / pointer_bits）", field);
        error_add_at(ERR_SEMANTIC, node->line, node->column, msg);
        g_tc_diag_reported = 1;   /* 已报具体错 ⇒ 别再补一句笼统的（见 target_cond_eval ✓）*/
    }
    return tcv_unknown();
}

/* 递归求值：只在"编译期能定"的形态上有值，其余一律 UNKNOWN（⇒ 不剪枝 ✓）
 *   *saw_target：本次求值是否**触碰过 `target.*`**（用于"提到了却判不出来 ⇒ 报错"） */
static TCVal tc_eval(Ast* e, int* saw_target) {
    if (!e) return tcv_unknown();

    switch (e->kind) {
        case AST_BOOL:  return tcv_bool(e->u.boolean);
        case AST_STRING: return tcv_str(e->u.string.value);
        case AST_NUM:
            if (e->u.num.is_bigint) return tcv_unknown();
            return tcv_num((long long)e->u.num.value);

        /* `target.os` —— ⚠ 解析器把 `ident.ident` **一律**建成 AST_MODULE_ACCESS
         *   （注释原话："暂时无法确定是模块成员访问还是 struct 字段访问，交给语义分析判断"，
         *     见 parser_expr.c:1093-1102 ✓）⇒ 两种节点都得认：
         *     · AST_MODULE_ACCESS：`target.os` 走的就是这条 ✓（剪枝发生在语义分析之前）
         *     · AST_FIELD_ACCESS：类型守卫等少数路径会直接建这个 ✓ */
        case AST_MODULE_ACCESS:
            return tc_target_field(e->u.module_access.module_name,
                                   e->u.module_access.member_name, e, saw_target);

        case AST_FIELD_ACCESS:
            return tc_target_field(is_target_var(e->u.field_access.obj) ? "target" : NULL,
                                   e->u.field_access.field_name, e, saw_target);

        case AST_UNARY: {
            if (e->u.unary.op != TOK_NOT) return tcv_unknown();
            int t = tcv_truth(tc_eval(e->u.unary.operand, saw_target));
            return t < 0 ? tcv_unknown() : tcv_bool(!t);
        }

        case AST_BINOP: {
            LenoTokenType op = e->u.binop.op;

            /* and / or：三值逻辑（一侧已定就能定结果，符合直觉且不误剪 ✓） */
            if (op == TOK_AND || op == TOK_OR) {
                int a = tcv_truth(tc_eval(e->u.binop.l, saw_target));
                int b = tcv_truth(tc_eval(e->u.binop.r, saw_target));
                if (op == TOK_AND) {
                    if (a == 0 || b == 0) return tcv_bool(0);
                    if (a == 1 && b == 1) return tcv_bool(1);
                } else {
                    if (a == 1 || b == 1) return tcv_bool(1);
                    if (a == 0 && b == 0) return tcv_bool(0);
                }
                return tcv_unknown();
            }

            if (op == TOK_EQEQ || op == TOK_NEQ) {
                TCVal a = tc_eval(e->u.binop.l, saw_target);
                TCVal b = tc_eval(e->u.binop.r, saw_target);
                if (a.kind == TCV_UNKNOWN || b.kind == TCV_UNKNOWN) return tcv_unknown();
                int eq;
                if (a.kind == TCV_STR && b.kind == TCV_STR) eq = (strcmp(a.s, b.s) == 0);
                else if (a.kind == TCV_STR || b.kind == TCV_STR) return tcv_unknown();  /* 串 vs 数 ⇒ 未知 */
                else eq = (a.i == b.i);
                return tcv_bool(op == TOK_EQEQ ? eq : !eq);
            }
            return tcv_unknown();
        }

        default:
            return tcv_unknown();
    }
}

/* 条件的三态判定：-1 未知（可能已报错）/ 0 假 / 1 真
 *   ⚠ "提到了 target 却判不出来" ⇒ **报错**：否则 `target` 会一路飘到语义层变成
 *     "未定义的变量"，那个报错对用户毫无指向性 ✗ */
static int target_cond_eval(Ast* cond) {
    int saw = 0;
    g_tc_diag_reported = 0;
    int t = tcv_truth(tc_eval(cond, &saw));

    /* ★★ 关键：**没提到 target.* 就一律不动** ★★
     *   `tc_eval` 能求值的形态里包含"普通常量"（如 `false`、`1 == 1`），若直接拿它的结果剪枝，
     *   就等于把 `if false { … }` 的死分支也不再做语义分析 —— 那会**悄悄改掉现有语义** ✗
     *   （实测踩到：`if false { 未定义变量 }` 原本必须报错，加了这个 pass 后不报了 ✗）
     *   ⇒ 判据必须是"条件里出现 target.*"，这正是"纯增量"的实现保证 ✓ */
    if (!saw) return -1;

    if (t >= 0) return t;
    if (!g_tc_diag_reported && cond) {
        error_add_at(ERR_SEMANTIC, cond->line, cond->column,
            "target 条件必须是编译期可判定的常量表达式"
            "（只允许 target.os / arch / family / pointer_bits 与字面量，配合 == != and or not）");
    }
    return -1;
}

/* 在不支持 target 条件的位置（while / for / switch 的头部）发现 `target.*` ⇒ 明确报错
 *   ⚠ 不报的话它会一路飘到语义层变成"未定义的变量: target"，指向性很差 ✗
 *   （v1 只支持 `if` 的条件 —— 那是最有表达力、且语义最干净的落点 ✓） */
static void target_cond_reject(Ast* e, const char* where) {
    if (!e) return;
    int saw = 0;
    (void)tc_eval(e, &saw);
    if (saw) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "target 条件只支持用在 if（语句或 if 表达式）里，不能用在 %s", where);
        error_add_at(ERR_SEMANTIC, e->line, e->column, msg);
    }
}

static int is_const_num(Ast* ast) {
    return ast && ast->kind == AST_NUM && !ast->u.num.is_bigint;
}

static int is_const_int(Ast* ast) {
    return is_const_num(ast) && !ast->u.num.is_float;
}

static int is_const_float(Ast* ast) {
    return is_const_num(ast) && ast->u.num.is_float;
}

static int is_const_bool(Ast* ast) {
    return ast && ast->kind == AST_BOOL;
}

static int is_const_string(Ast* ast) {
    return ast && ast->kind == AST_STRING;
}

static void replace_with_num(Ast* ast, double value, int is_float) {
    if (ast->cached_type) {
        type_free(ast->cached_type);
    }
    ast->cached_type = is_float ? type_new(TYPE_FLOAT) : type_new(TYPE_INT);
    ast->kind = AST_NUM;
    ast->u.num.value = value;
    ast->u.num.is_bigint = 0;
    ast->u.num.bigint_str = NULL;
    ast->u.num.is_float = is_float;
}

static void replace_with_bool(Ast* ast, int value) {
    if (ast->cached_type) {
        type_free(ast->cached_type);
    }
    ast->cached_type = type_new(TYPE_BOOL);
    ast->kind = AST_BOOL;
    ast->u.boolean = value;
}

static void replace_with_string(Ast* ast, char* value, int len) {
    if (ast->cached_type) {
        type_free(ast->cached_type);
    }
    ast->cached_type = type_new(TYPE_STRING);
    ast->kind = AST_STRING;
    ast->u.string.value = value;
    ast->u.string.len = len;
}

/* ★ 整数常量折叠的**精确出口**（超 int48 时落成 bigint 字面量）
 * ----------------------------------------------------------------------------
 * 为什么必须有它：运行期整数是 **48 位截断**语义（`val_int()` 掩码，见
 * vm/vminc/run/04_compare_bit_cast.inc 的位移/位运算与 03_arith.inc 的算术），
 * 而下面的 double 折叠路径只能精确到 2^53 ⇒ 原先"超 2^53 就不折"的做法**不再等价于
 * 语义不变**：不折就要走运行期指令，而运行期是截断 ⇒ 同一个表达式在"字面量位置"与
 * "运行期位置"结果分裂 ✗（实测 test_bigint_arith 的 `hex64(0x123456789ab << 16)`
 * 期望精确的 0123456789ab0000，不折就变成截断值 0000456789ab0000 ✗）。
 * 所以：能精确表示的整数一律**折成精确常量**；超出 int48 的落成 bigint 字面量
 * （`u.num.is_bigint = 1` + `bigint_str`，codegen 见 codegen_expr.c 的 AST_NUM 分支会
 * `bigint_from_string` 建真 bigint 常量 ✓）。 */
static void replace_with_i64(Ast* ast, int64_t v) {
    if (v >= INT48_MIN && v <= INT48_MAX) {
        replace_with_num(ast, (double)v, 0);
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)v);
    if (ast->cached_type) type_free(ast->cached_type);
    ast->cached_type = type_new(TYPE_INT);
    ast->kind = AST_NUM;
    ast->u.num.value = (double)v;
    ast->u.num.is_float = 0;
    ast->u.num.is_bigint = 1;
    ast->u.num.bigint_str = strdup(buf);   // 与 parser 的 copy_string 同为 malloc 系（ast_free 用 free 释放）
}

/* 整数二元折叠（精确）：能精确算出来就折、写回 ast 并返回 1；算不了/会溢出 int64 返回 0
 * （返回 0 时交给后面的 double 路径或干脆不折 —— 不折是安全的：算术越界运行期会提升
 * bigint ✓、移位 ≥ 32 运行期走 bigint 移位 ✓，两条路径本来就精确 ✓，例如 `1 << 63`）。 */
static int fold_binary_i64(Ast* ast, Ast* l, Ast* r, LenoTokenType op) {
    if (!is_const_int(l) || !is_const_int(r)) return 0;
    double dl = l->u.num.value, dr = r->u.num.value;
    if (dl < -9.007199254740992e15 || dl > 9.007199254740992e15 ||
        dr < -9.007199254740992e15 || dr > 9.007199254740992e15) {
        return 0;   // 字面量本身已超 double 精确范围：不折（交给运行期）
    }
    int64_t lv = (int64_t)dl, rv = (int64_t)dr;
    int64_t out = 0;

    switch (op) {
        case TOK_PLUS:  if (__builtin_add_overflow(lv, rv, &out)) return 0; break;
        case TOK_MINUS: if (__builtin_sub_overflow(lv, rv, &out)) return 0; break;
        case TOK_STAR:  if (__builtin_mul_overflow(lv, rv, &out)) return 0; break;
        case TOK_BITAND: out = lv & rv; break;
        case TOK_BITOR:  out = lv | rv; break;
        case TOK_BITXOR: out = lv ^ rv; break;
        case TOK_SLASH:
            if (rv == 0) return 0;
            if (lv == INT64_MIN && rv == -1) return 0;
            out = lv / rv;
            break;
        case TOK_MOD:
            if (rv == 0) return 0;
            if (lv == INT64_MIN && rv == -1) return 0;
            out = lv % rv;
            break;
        case TOK_SHL: {
            if (rv < 0 || rv > 63) return 0;
            // 用无符号算：`1 << 63` 这样"精确但超出 int64 有符号范围"的值不折
            // （运行期走 bigint 移位，结果同样精确 ✓）
            uint64_t u = ((uint64_t)lv << rv);
            if (rv > 0 && (u >> rv) != (uint64_t)lv) return 0;
            if (u > (uint64_t)INT64_MAX) return 0;
            out = (int64_t)u;
            break;
        }
        case TOK_SHR:
            if (rv < 0 || rv > 63) return 0;
            out = lv >> rv;
            break;
        case TOK_USHR: {
            if (rv < 0 || rv > 63) return 0;
            uint64_t u = ((uint64_t)lv & PAYLOAD_MASK) >> rv;   // 48 位逻辑右移（与 VM 同口径）
            out = (int64_t)u;
            break;
        }
        case TOK_EQEQ: ast_free(l); ast_free(r); replace_with_bool(ast, lv == rv); return 1;
        case TOK_NEQ:  ast_free(l); ast_free(r); replace_with_bool(ast, lv != rv); return 1;
        case TOK_LT:   ast_free(l); ast_free(r); replace_with_bool(ast, lv <  rv); return 1;
        case TOK_GT:   ast_free(l); ast_free(r); replace_with_bool(ast, lv >  rv); return 1;
        case TOK_LE:   ast_free(l); ast_free(r); replace_with_bool(ast, lv <= rv); return 1;
        case TOK_GE:   ast_free(l); ast_free(r); replace_with_bool(ast, lv >= rv); return 1;
        default: return 0;
    }

    ast_free(l);
    ast_free(r);
    replace_with_i64(ast, out);
    return 1;
}

static void fold_binary(Ast* ast) {
    Ast* l = ast->u.binop.l;
    Ast* r = ast->u.binop.r;
    LenoTokenType op = ast->u.binop.op;

    if (op == TOK_AND || op == TOK_OR || op == TOK_NULL_COALESCE) return;

    // ★ 整数精确折叠优先（见 fold_binary_i64 的说明）：`0x123456789ab << 16` 这类
    //   超 2^53 但仍在 int64 内的整数表达式必须折成精确常量，否则会走运行期的 48 位截断 ✗
    if (fold_binary_i64(ast, l, r, op)) return;

    if (is_const_num(l) && is_const_num(r)) {
        int l_float = l->u.num.is_float;
        int r_float = r->u.num.is_float;
        int result_is_float = l_float || r_float;
        double lv = l->u.num.value;
        double rv = r->u.num.value;
        double result = 0;

        switch (op) {
            case TOK_PLUS:  result = lv + rv; break;
            case TOK_MINUS: result = lv - rv; break;
            case TOK_STAR:  result = lv * rv; break;
            case TOK_SLASH:
                if (rv == 0.0) return;
                if (!result_is_float) {
                    result = (double)((long long)lv / (long long)rv);
                } else {
                    result = lv / rv;
                }
                break;
            case TOK_MOD:
                if (rv == 0.0) return;
                if (!result_is_float) {
                    long long li = (long long)lv;
                    long long ri = (long long)rv;
                    result = (double)(li % ri);
                } else {
                    result = fmod(lv, rv);
                }
                break;
            case TOK_BITAND:
                if (result_is_float) return;
                result = (double)((long long)lv & (long long)rv);
                break;
            case TOK_BITOR:
                if (result_is_float) return;
                result = (double)((long long)lv | (long long)rv);
                break;
            case TOK_BITXOR:
                if (result_is_float) return;
                result = (double)((long long)lv ^ (long long)rv);
                break;
            case TOK_SHL:
                if (result_is_float) return;
                if ((int)rv >= 32) return; // 移位量>=32时不折叠，交给VM的BigInt路径处理
                result = (double)((long long)lv << (int)rv);
                break;
            case TOK_SHR:
                if (result_is_float) return;
                if ((int)rv >= 32) return; // 移位量>=32时不折叠，交给VM的BigInt路径处理
                result = (double)((long long)lv >> (int)rv);
                break;
            case TOK_EQEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv == rv);
                return;
            case TOK_NEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv != rv);
                return;
            case TOK_LT:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv < rv);
                return;
            case TOK_GT:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv > rv);
                return;
            case TOK_LE:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv <= rv);
                return;
            case TOK_GE:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv >= rv);
                return;
            default:
                return;
        }

        /* §8.123：**整数**折叠结果的精度守卫 ✗
         * 本函数的中间计算全用 double ✓，而 double 只能**精确**表示 ≤ 2^53 的整数 ✗
         * ⇒ 超出后折出来的常量就已经失真 ✗（实测 `0x80000000 * 4294967296`：
         * 折出 2^63 ⇒ 回落 int64 时溢出 ⇒ 变成 **-2^63** ✗ —— 这正是 bug3 的"字面量表达式
         * 与变量运算语义分裂"里**字面量那一半** ✓；变量那一半已在 OP_MUL_INT 修好 ✓）。
         * 与既有"移位量 ≥ 32 就不折叠、交给 VM 的 BigInt 路径"同一策略 ✓：
         * **不精确就不折** ✓ —— 折叠只是优化，跳过它语义不变 ✓（VM 侧现已按 bigint 正确 ✓）。 */
        if (!result_is_float && fabs(result) > 9007199254740992.0) return;

        ast_free(l);
        ast_free(r);
        replace_with_num(ast, result, result_is_float);
        return;
    }

    if (is_const_bool(l) && is_const_bool(r)) {
        int lv = l->u.boolean;
        int rv = r->u.boolean;

        switch (op) {
            case TOK_EQEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv == rv);
                return;
            case TOK_NEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv != rv);
                return;
            default:
                break;
        }
        return;
    }

    if (op == TOK_PLUS && is_const_string(l) && is_const_string(r)) {
        int new_len = l->u.string.len + r->u.string.len;
        char* new_str = (char*)malloc(new_len + 1);
        memcpy(new_str, l->u.string.value, l->u.string.len);
        memcpy(new_str + l->u.string.len, r->u.string.value, r->u.string.len);
        new_str[new_len] = '\0';
        ast_free(l);
        ast_free(r);
        replace_with_string(ast, new_str, new_len);
        return;
    }

    if (is_const_num(l) && is_const_bool(r)) {
        double lv = l->u.num.value;
        int rv = r->u.boolean;
        switch (op) {
            case TOK_EQEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv == (double)rv);
                return;
            case TOK_NEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, lv != (double)rv);
                return;
            default:
                break;
        }
    }

    if (is_const_bool(l) && is_const_num(r)) {
        int lv = l->u.boolean;
        double rv = r->u.num.value;
        switch (op) {
            case TOK_EQEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, (double)lv == rv);
                return;
            case TOK_NEQ:
                ast_free(l);
                ast_free(r);
                replace_with_bool(ast, (double)lv != rv);
                return;
            default:
                break;
        }
    }
}

static void fold_unary(Ast* ast) {
    Ast* operand = ast->u.unary.operand;
    LenoTokenType op = ast->u.unary.op;

    if (is_const_num(operand)) {
        double val = operand->u.num.value;
        // ★ 取反同样按整数精确路径（原先 `~(int)val` 是 32 位、会在 32 位处截断 ✗）
        if (op == TOK_BITNOT && !operand->u.num.is_float &&
            val >= -9.007199254740992e15 && val <= 9.007199254740992e15) {
            int64_t iv = (int64_t)val;
            ast_free(operand);
            replace_with_i64(ast, ~iv);
            return;
        }
        int is_float = operand->u.num.is_float;
        double result = 0;

        switch (op) {
            case TOK_MINUS:
                result = -val;
                break;
            case TOK_BITNOT:
                if (is_float) return;
                result = (double)(~(int)val);
                break;
            default:
                return;
        }

        ast_free(operand);
        replace_with_num(ast, result, is_float);
        return;
    }

    if (is_const_bool(operand) && op == TOK_NOT) {
        int val = operand->u.boolean;
        ast_free(operand);
        replace_with_bool(ast, !val);
        return;
    }
}

static void fold_expr(Ast* ast);

static void fold_block(AstList* list) {
    for (int i = 0; i < list->count; i++) {
        fold_expr(list->items[i]);
    }
}

static void fold_expr(Ast* ast) {
    if (!ast) return;

    switch (ast->kind) {
        case AST_NUM:
        case AST_STRING:
        case AST_BOOL:
        case AST_NULL:
            break;

        case AST_BINOP:
            fold_expr(ast->u.binop.l);
            fold_expr(ast->u.binop.r);
            fold_binary(ast);
            break;

        case AST_UNARY:
            fold_expr(ast->u.unary.operand);
            fold_unary(ast);
            break;

        case AST_ARRAY:
            fold_block(&ast->u.array);
            break;

        case AST_DICT:
            for (int i = 0; i < ast->u.dict.count; i++) {
                fold_expr(ast->u.dict.entries[i].key);
                fold_expr(ast->u.dict.entries[i].value);
            }
            break;

        case AST_RANGE:
            fold_expr(ast->u.range.start);
            fold_expr(ast->u.range.end);
            break;

        case AST_VAR:
            break;

        case AST_CALL:
            fold_expr(ast->u.call.callee);
            fold_block(&ast->u.call.args);
            break;

        case AST_INDEX:
            fold_expr(ast->u.index.obj);
            fold_expr(ast->u.index.index);
            break;

        case AST_SLICE:
            fold_expr(ast->u.slice.obj);
            fold_expr(ast->u.slice.start);
            fold_expr(ast->u.slice.end);
            break;

        case AST_INDEX_ASSIGN:
            fold_expr(ast->u.index_assign.obj);
            fold_expr(ast->u.index_assign.index);
            fold_expr(ast->u.index_assign.value);
            break;

        case AST_BLOCK:
            fold_block(&ast->u.block);
            break;

        case AST_IF:
            fold_expr(ast->u.if_.cond);
            fold_expr(ast->u.if_.then);
            fold_expr(ast->u.if_.else_);
            break;

        case AST_WHILE:
            fold_expr(ast->u.while_.cond);
            fold_expr(ast->u.while_.body);
            break;

        case AST_FOR:
            fold_expr(ast->u.for_.start);
            fold_expr(ast->u.for_.end);
            fold_expr(ast->u.for_.step);
            fold_expr(ast->u.for_.body);
            break;

        case AST_SWITCH:
            fold_expr(ast->u.switch_.expr);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                fold_block(&ast->u.switch_.cases[i].values);
                fold_expr(ast->u.switch_.cases[i].body);
            }
            fold_expr(ast->u.switch_.default_body);
            break;

        case AST_FUNC_DEF:
            fold_expr(ast->u.func.body);
            break;

case AST_RETURN:
fold_expr(ast->u.ret);
break;
case AST_RETURN_MULTI:
for (int i = 0; i < ast->u.ret_multi.count; i++) {
fold_expr(ast->u.ret_multi.exprs[i]);
}
break;

        case AST_ASSIGN:
            fold_expr(ast->u.assign.value);
            break;

        case AST_COMPOUND_ASSIGN:
            fold_expr(ast->u.compound_assign.value);
            break;

case AST_VAR_DECL:
fold_expr(ast->u.var_decl.init);
break;

case AST_DESTRUCT_DECL:
fold_expr(ast->u.destruct_decl.init);
break;

case AST_EXPR_STMT:
            fold_expr(ast->u.expr_stmt.expr);
            break;

        case AST_EXPORT:
            fold_expr(ast->u.export.decl);
            break;

        case AST_MODULE_CALL:
            fold_block(&ast->u.module_call.args);
            break;

        case AST_INTERP_STRING:
            for (int i = 0; i < ast->u.interp_string.count - 1; i++) {
                fold_expr(ast->u.interp_string.exprs[i]);
            }
            break;

        case AST_TRY:
            fold_expr(ast->u.try_.try_body);
            fold_expr(ast->u.try_.catch_body);
            fold_expr(ast->u.try_.finally_body);
            break;

        case AST_THROW:
            fold_expr(ast->u.throw_.expr);
            break;

        case AST_TYPE_CHECK:
        case AST_AS_CAST:
            fold_expr(ast->u.type_check.expr);
            break;

        case AST_STRUCT_DEF:
            for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                fold_expr(ast->u.struct_def.field_defaults[i]);
            }
            for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                fold_expr(ast->u.struct_def.methods[i]);
            }
            for (int i = 0; i < ast->u.struct_def.const_count; i++) {
                fold_expr(ast->u.struct_def.const_values[i]);
            }
            break;
        case AST_FACE_DEF:
        case AST_ALIAS:
            break;

        case AST_STRUCT_INIT:
            for (int i = 0; i < ast->u.struct_init.field_count; i++) {
                fold_expr(ast->u.struct_init.field_values[i]);
            }
            break;

        case AST_FIELD_ACCESS:
            fold_expr(ast->u.field_access.obj);
            break;

        case AST_ADDRESS_OF:
            fold_expr(ast->u.address_of.operand);
            break;

        case AST_AWAIT:
            fold_expr(ast->u.await.expr);
            break;

        case AST_SAFE_ACCESS:
            fold_expr(ast->u.safe_access.obj);
            for (int i = 0; i < ast->u.safe_access.args.count; i++) {
                fold_expr(ast->u.safe_access.args.items[i]);
            }
            break;

        case AST_IMPORT:
        case AST_USE:
        case AST_MODULE_ACCESS:
        case AST_BREAK:
        case AST_CONTINUE:
        case AST_ENUM_DEF:
        case AST_CLIB_DEF:
        case AST_CFUNC_DECL:
        case AST_CSTRUCT_DEF:
            break;
    }
}

// ============================================================================
// 死代码消除（Dead Code Elimination, DCE）
//
// 在常量折叠之后执行，利用已折叠的常量条件进行更激进的消除。
//
// 优化规则：
//   1. 常量条件消除：
//      - if true  { A } else { B } → A（消除条件和 else 分支）
//      - if false { A } else { B } → B（消除条件和 then 分支）
//      - while false { A }        → null（消除整个循环）
//
//   2. 终止语句后死代码消除：
//      - return / break / continue / throw 之后的语句全部消除
//      - 例如：return 42; print("dead") → return 42;
//
//   3. 节点原地替换技术：
//      - 将 if 节点替换为其存活分支时，使用 memcpy 原地替换
//      - 避免修改父节点的指针，保持 AST 树结构完整
// ============================================================================

// 判断语句是否是终止语句
// 终止语句执行后，控制流不会继续到下一条语句，因此后续代码都是死代码
static int is_terminator(Ast* ast) {
    if (!ast) return 0;
    switch (ast->kind) {
case AST_RETURN:     // return：函数返回，后续代码不可达
case AST_RETURN_MULTI: // 多值返回：函数返回，后续代码不可达
case AST_BREAK:      // break：跳出循环，后续代码不可达
        case AST_CONTINUE:   // continue：跳到循环下一轮，后续代码不可达
        case AST_THROW:      // throw：抛出异常，后续代码不可达
            return 1;
        default:
            return 0;
    }
}

// 判断块是否以终止语句结尾（用于向上传播终止信息）
static int block_ends_with_terminator(AstList* list) {
    if (list->count == 0) return 0;
    return is_terminator(list->items[list->count - 1]);
}

// 消除块中终止语句之后的死代码
// 例如：{ return 1; x = 2; print(x); } → { return 1; }
static void dce_block(AstList* list) {
    for (int i = 0; i < list->count; i++) {
        if (is_terminator(list->items[i]) && i + 1 < list->count) {
            // 找到终止语句，释放之后的所有死代码
            for (int j = i + 1; j < list->count; j++) {
                ast_free(list->items[j]);
            }
            list->count = i + 1;
            break;
        }
    }
}

// 判断 AST 是否为编译期常量 true
// 包括：true 字面量、非零整数（常量折叠后可能产生）
static int is_always_true(Ast* ast) {
    if (!ast) return 0;
    if (ast->kind == AST_BOOL && ast->u.boolean) return 1;
    if (is_const_int(ast) && (int)ast->u.num.value != 0) return 1;
    return 0;
}

// 判断 AST 是否为编译期常量 false
// 包括：false 字面量、null、0、0.0（均为假值）
static int is_always_false(Ast* ast) {
    if (!ast) return 0;
    if (ast->kind == AST_BOOL && !ast->u.boolean) return 1;
    if (ast->kind == AST_NULL) return 1;
    if (is_const_int(ast) && (int)ast->u.num.value == 0) return 1;
    if (is_const_float(ast) && ast->u.num.value == 0.0) return 1;
    return 0;
}

// 递归消除死代码
// 返回值：当前节点是否是终止语句（用于向上传播终止信息）
static int dce_expr(Ast* ast) {
    if (!ast) return 0;

    switch (ast->kind) {
        // 叶子节点：无子引用，不是终止语句
        case AST_NUM:
        case AST_STRING:
        case AST_BOOL:
        case AST_NULL:
        case AST_VAR:
        case AST_BREAK:
        case AST_CONTINUE:
            return 0;

        // return：递归处理返回值表达式，本身是终止语句
case AST_RETURN:
if (ast->u.ret) dce_expr(ast->u.ret);
return 1;
case AST_RETURN_MULTI:
for (int i = 0; i < ast->u.ret_multi.count; i++) {
dce_expr(ast->u.ret_multi.exprs[i]);
}
return 1;

        // throw：递归处理异常表达式，本身是终止语句
        case AST_THROW:
            if (ast->u.throw_.expr) dce_expr(ast->u.throw_.expr);
            return 1;

        // 块：递归处理所有子语句，然后消除终止语句后的死代码
        case AST_BLOCK: {
            AstList* list = &ast->u.block;
            for (int i = 0; i < list->count; i++) {
                dce_expr(list->items[i]);
            }
            /* ⚠ target-only 模式：不裁剪"终止语句之后的死代码"（那是完整 DCE 的活，
             *   挪到语义分析之前会把 return 后面的语句提前丢掉 ⇒ 语义分析结果就变了 ✗） */
            if (g_dce_target_only) return 0;
            dce_block(list);
            return block_ends_with_terminator(list);
        }

        // if 语句：核心优化目标
        case AST_IF: {
            /* ★ 编译期 target 条件（新增）：条件只含 target.* 且静态可判 ⇒ 就地剪枝。
             *   替换套路与下面的"常量条件消除"一致（memcpy + free），差别有二：
             *     ① 被丢掉的那一支**也 ast_free 掉**（下面老路径是把三指针置 NULL、没释放）
             *     ② 被保留的那一支仍要递归剪枝（里面可能还有 target 条件 ✓）
             *   ⚠ 带类型守卫的 if（`if x is Foo`）不可能是 target 条件 ⇒ 直接跳过 ✓ */
            if (!ast->u.if_.guard_var && !ast->u.if_.guard_type) {
                int tv = target_cond_eval(ast->u.if_.cond);
                if (tv >= 0) {
                    Ast* keep = tv ? ast->u.if_.then : ast->u.if_.else_;
                    Ast* drop = tv ? ast->u.if_.else_ : ast->u.if_.then;
                    Ast* cond = ast->u.if_.cond;
                    ast->u.if_.then = NULL;
                    ast->u.if_.cond = NULL;
                    ast->u.if_.else_ = NULL;
                    if (keep) dce_expr(keep);      /* 活分支继续剪 ✓ */
                    if (drop) ast_free(drop);      /* 死分支：连语法树一起丢掉 ✓ */
                    if (cond) ast_free(cond);
                    if (ast->cached_type) { type_free(ast->cached_type); ast->cached_type = NULL; }
                    if (keep) {
                        memcpy(ast, keep, sizeof(Ast));   /* 原地替换：不动父节点指针 ✓ */
                        free(keep);
                    } else {
                        ast->kind = AST_NULL;             /* 无 else 分支 ⇒ 空操作 ✓ */
                    }
                    return 0;
                }
            }

            // 先递归处理条件和分支（可能产生更多常量条件）
            dce_expr(ast->u.if_.cond);
            dce_expr(ast->u.if_.then);
            dce_expr(ast->u.if_.else_);

            /* ⚠ target-only 模式（语义分析之前那道）：**只**处理 target 条件。
             *   非 target 的常量条件（`if false` / `if MY_FLAG`）一律留给 semantic 之后
             *   那道 ⇒ 现有语义零变化 ✓（这正是"纯增量"的实现保证） */
            if (g_dce_target_only) return 0;

            // 优化1：if true { then } else { else_ } → then
            // 消除条件判断和 else 分支，保留 then 分支
            if (is_always_true(ast->u.if_.cond)) {
                Ast* then_branch = ast->u.if_.then;
                // 先将指针置空，防止 ast_free 释放子节点
                ast->u.if_.then = NULL;
                ast->u.if_.cond = NULL;
                ast->u.if_.else_ = NULL;
                int is_term = is_terminator(then_branch);
                // 释放缓存的类型信息
                if (ast->cached_type) { type_free(ast->cached_type); ast->cached_type = NULL; }
                // 原地替换：将 then 分支的内容复制到当前 if 节点
                // 这样不需要修改父节点的指针
                memcpy(ast, then_branch, sizeof(Ast));
                free(then_branch);
                return is_term;
            }

            // 优化2：if false { then } else { else_ } → else_ 或 null
            // 消除条件判断和 then 分支，保留 else 分支
            if (is_always_false(ast->u.if_.cond)) {
                Ast* else_branch = ast->u.if_.else_;
                ast->u.if_.then = NULL;
                ast->u.if_.cond = NULL;
                ast->u.if_.else_ = NULL;
                if (else_branch) {
                    // 有 else 分支：用 else 替换整个 if
                    int is_term = is_terminator(else_branch);
                    if (ast->cached_type) { type_free(ast->cached_type); ast->cached_type = NULL; }
                    memcpy(ast, else_branch, sizeof(Ast));
                    free(else_branch);
                    return is_term;
                } else {
                    // 无 else 分支：替换为 null 节点（表达式语句中的空操作）
                    ast->kind = AST_NULL;
                    if (ast->cached_type) { type_free(ast->cached_type); ast->cached_type = NULL; }
                    return 0;
                }
            }

            // 条件非常量：保留 if 语句，不做消除
            return 0;
        }

        // while 循环：消除不可达循环
        case AST_WHILE: {
            target_cond_reject(ast->u.while_.cond, "while 的条件");   /* v1 只支持 if ✓ */
            dce_expr(ast->u.while_.cond);
            dce_expr(ast->u.while_.body);

            // while false { body } → null（循环体永远不会执行）
            if (!g_dce_target_only && is_always_false(ast->u.while_.cond)) {
                ast_free(ast->u.while_.cond);
                ast_free(ast->u.while_.body);
                if (ast->cached_type) { type_free(ast->cached_type); ast->cached_type = NULL; }
                ast->kind = AST_NULL;
                return 0;
            }

            // while true { body }：保留（可能包含 break/return）
            return 0;
        }

        // for 循环：递归处理子节点
        case AST_FOR: {
            target_cond_reject(ast->u.for_.start, "for 头的起点");   /* v1 只支持 if ✓ */
            target_cond_reject(ast->u.for_.end,   "for 头的终点");
            target_cond_reject(ast->u.for_.step,  "for 头的步长");
            if (ast->u.for_.start) dce_expr(ast->u.for_.start);
            dce_expr(ast->u.for_.end);
            if (ast->u.for_.step) dce_expr(ast->u.for_.step);
            dce_expr(ast->u.for_.body);
            return 0;
        }

        // switch：递归处理表达式和各 case 分支
        case AST_SWITCH: {
            target_cond_reject(ast->u.switch_.expr, "switch 的表达式");   /* v1 只支持 if ✓ */
            dce_expr(ast->u.switch_.expr);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                for (int j = 0; j < ast->u.switch_.cases[i].values.count; j++) {
                    dce_expr(ast->u.switch_.cases[i].values.items[j]);
                }
                dce_expr(ast->u.switch_.cases[i].body);
            }
            dce_expr(ast->u.switch_.default_body);
            return 0;
        }

        // 函数定义：递归处理函数体
        case AST_FUNC_DEF: {
            dce_expr(ast->u.func.body);
            return 0;
        }

        // 二元运算：递归处理左右操作数
        case AST_BINOP:
            dce_expr(ast->u.binop.l);
            dce_expr(ast->u.binop.r);
            return 0;

        // 一元运算：递归处理操作数
        case AST_UNARY:
            dce_expr(ast->u.unary.operand);
            return 0;

        // 数组字面量：递归处理所有元素
        case AST_ARRAY:
            for (int i = 0; i < ast->u.array.count; i++) {
                dce_expr(ast->u.array.items[i]);
            }
            return 0;

        // 字典字面量：递归处理所有键和值
        case AST_DICT:
            for (int i = 0; i < ast->u.dict.count; i++) {
                dce_expr(ast->u.dict.entries[i].key);
                dce_expr(ast->u.dict.entries[i].value);
            }
            return 0;

        // 范围表达式：递归处理起止值
        case AST_RANGE:
            dce_expr(ast->u.range.start);
            dce_expr(ast->u.range.end);
            return 0;

        // 函数调用：递归处理被调用者和参数
        case AST_CALL:
            dce_expr(ast->u.call.callee);
            for (int i = 0; i < ast->u.call.args.count; i++) {
                dce_expr(ast->u.call.args.items[i]);
            }
            return 0;

        // 索引访问：递归处理对象和索引
        case AST_INDEX:
            dce_expr(ast->u.index.obj);
            dce_expr(ast->u.index.index);
            return 0;

        // 切片：递归处理对象和起止索引
        case AST_SLICE:
            dce_expr(ast->u.slice.obj);
            if (ast->u.slice.start) dce_expr(ast->u.slice.start);
            if (ast->u.slice.end) dce_expr(ast->u.slice.end);
            return 0;

        // 索引赋值：递归处理对象、索引和值
        case AST_INDEX_ASSIGN:
            dce_expr(ast->u.index_assign.obj);
            dce_expr(ast->u.index_assign.index);
            dce_expr(ast->u.index_assign.value);
            return 0;

        // 变量赋值：递归处理赋值表达式
        case AST_ASSIGN:
            dce_expr(ast->u.assign.value);
            return 0;

        // 复合赋值：递归处理赋值表达式
        case AST_COMPOUND_ASSIGN:
            dce_expr(ast->u.compound_assign.value);
            return 0;

        // 变量声明：递归处理初始化表达式
case AST_VAR_DECL:
if (ast->u.var_decl.init) dce_expr(ast->u.var_decl.init);
return 0;

case AST_DESTRUCT_DECL:
if (ast->u.destruct_decl.init) dce_expr(ast->u.destruct_decl.init);
return 0;

// 表达式语句：递归处理表达式
case AST_EXPR_STMT:
            dce_expr(ast->u.expr_stmt.expr);
            return 0;

        // 导出声明：递归处理被导出的声明
        case AST_EXPORT:
            dce_expr(ast->u.export.decl);
            return 0;

        // 模块调用：递归处理参数
        case AST_MODULE_CALL:
            for (int i = 0; i < ast->u.module_call.args.count; i++) {
                dce_expr(ast->u.module_call.args.items[i]);
            }
            return 0;

        // 字符串插值：递归处理所有插值表达式
        case AST_INTERP_STRING:
            for (int i = 0; i < ast->u.interp_string.count - 1; i++) {
                dce_expr(ast->u.interp_string.exprs[i]);
            }
            return 0;

        // try-catch-finally：递归处理各部分
        case AST_TRY:
            dce_expr(ast->u.try_.try_body);
            if (ast->u.try_.catch_body) dce_expr(ast->u.try_.catch_body);
            if (ast->u.try_.finally_body) dce_expr(ast->u.try_.finally_body);
            return 0;

        // 类型检查：递归处理表达式
        case AST_TYPE_CHECK:
        case AST_AS_CAST:
            dce_expr(ast->u.type_check.expr);
            return 0;

        // 结构体定义：递归处理字段默认值和方法
        case AST_STRUCT_DEF:
            for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                if (ast->u.struct_def.field_defaults[i])
                    dce_expr(ast->u.struct_def.field_defaults[i]);
            }
            for (int i = 0; i < ast->u.struct_def.const_count; i++) {
                if (ast->u.struct_def.const_values[i])
                    dce_expr(ast->u.struct_def.const_values[i]);
            }
            for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                dce_expr(ast->u.struct_def.methods[i]);
            }
            return 0;

        case AST_FACE_DEF:
        case AST_ALIAS:
            return 0;

        // 结构体实例化：递归处理字段值
        case AST_STRUCT_INIT:
            for (int i = 0; i < ast->u.struct_init.field_count; i++) {
                dce_expr(ast->u.struct_init.field_values[i]);
            }
            return 0;

        // 字段访问：递归处理对象
        case AST_FIELD_ACCESS:
            dce_expr(ast->u.field_access.obj);
            return 0;

        case AST_ADDRESS_OF:
            dce_expr(ast->u.address_of.operand);
            return 0;

        // await：递归处理表达式
        case AST_AWAIT:
            dce_expr(ast->u.await.expr);
            return 0;

        case AST_SAFE_ACCESS:
            dce_expr(ast->u.safe_access.obj);
            for (int i = 0; i < ast->u.safe_access.args.count; i++) {
                dce_expr(ast->u.safe_access.args.items[i]);
            }
            return 0;

        // 以下节点无子表达式需要 DCE 处理
        case AST_IMPORT:
        case AST_USE:
        case AST_MODULE_ACCESS:
        case AST_ENUM_DEF:
        case AST_CLIB_DEF:
        case AST_CFUNC_DECL:
        case AST_CSTRUCT_DEF:
            return 0;
    }

    return 0;
}

// 死代码消除入口函数
// 在常量折叠之后调用，利用已折叠的常量条件进行更激进的消除
void optimize_dead_code_elimination(Ast* ast) {
    g_dce_target_only = 0;   /* 显式复位：保证这趟是"完整"模式（防上方那道 pass 的开关残留 ✓）*/
    dce_expr(ast);
}

/* ============================================================================
 * 编译期 target 条件剪枝入口 —— **语义分析之前**调用（详见本文件顶部的大段注释）
 *   与 optimize_dead_code_elimination 共用同一份遍历 `dce_expr`，
 *   靠 g_dce_target_only 区分模式：本入口**只**剪"条件含 target.* 的 if"，别的一概不碰 ✓
 * ========================================================================== */
void optimize_target_prune(Ast* ast) {
    if (!ast) return;

    /* ① 模块级（文件顶层）的 target 条件：**暂不支持**，报清楚原因（别让它飘到语义层变成
     *    "未定义的变量: target"）。
     *    为什么 v1 不做：`export` 级条件编译还牵涉**模块符号表的文本扫描器**
     *    （`module_symbol_table/inc/scan/scan_pass1.inc` 那个独立于 parser 的"第二前端"，
     *      它按文本匹配 `export`）—— 不一起处理的话，"被剪掉的 export"仍会进跨模块符号表 ✗
     *    ⇒ v1 只支持函数体内的语句级（以及 if 表达式）✓ */
    if (ast->kind == AST_BLOCK) {
        for (int i = 0; i < ast->u.block.count; i++) {
            Ast* it = ast->u.block.items[i];
            if (!it || it->kind != AST_IF) continue;
            if (it->u.if_.guard_var || it->u.if_.guard_type) continue;
            int saw = 0;
            (void)tc_eval(it->u.if_.cond, &saw);
            if (saw) {
                error_add_at(ERR_SEMANTIC, it->line, it->column,
                    "模块级的编译期 target 条件暂不支持（export 级还牵涉模块符号表的文本扫描器）；"
                    "请把它放进函数体内，或用运行期的 _os() 判断 ✓");
                return;   /* 已报错 ⇒ 别再往下剪（调用方会在本 pass 之后停住 ✓）*/
            }
        }
    }

    g_dce_target_only = 1;
    dce_expr(ast);
    g_dce_target_only = 0;
}

void optimize_constant_fold(Ast* ast) {
    fold_expr(ast);
}
