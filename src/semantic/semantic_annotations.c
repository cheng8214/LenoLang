// semantic_annotations.c — 注解 `@optkeys` 的收集与检查（2026-10-31）
// ----------------------------------------------------------------------------
// 设计文档：docs/注解系统与optkeys设计草案.md
//
// 为什么单独一个文件：它消费的是**编译期元数据**，与类型推断 / 作用域完全无关；
//   塞进 semantic_type.c（7000+ 行）只会让两边都更难找 ✓
//
// 一期范围（有意收窄）：
//   · 只处理**同模块**的直接调用（callee 是 AST_VAR）
//   · 只查**字面量 Dict** 实参（非字面量一律放行 —— 宁漏勿误报，同 L3 的教训 ✓）
//   · 只警告、不中断编译（存量代码里可能有传错键但靠静默丢弃跑着的地方）
// 跨模块（`SDL3.createHBox`）需要把注解读进模块符号表，见设计文档第 10 节 ✓

#include "../include/lenolang.h"
#include "../include/leno_ast.h"
#include "semantic_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ★ 2026-11-01：**去掉了进程级的 `AnnoOptKeys` 表**。
//   它原本给"同模块 + AST_CALL 阶段"那条检查路径用；那条路径与 `annos_check_call_def`
//   （参数检查处，同模块/跨模块通用）**完全重叠**，同一个错误会打两遍
//   （实测 `{visible:"false"}` 每键报 2 次，加上类型检查变 4 条）⇒ 只保留后者，
//   表就没人读了。留着是死代码 + 每次编译遍历所有函数建表，不如删掉 ✓
//   跨模块的 `module.member(...)` 仍由 `annos_check_call` 的 ① 分支处理
//   （那里在 AST 被改写成 AST_MODULE_CALL 之前，才拿得到原始字面量）。

// 每次编译前调用（保留 API：main 会调）
void annos_reset_all(void) {
    // 已经没有进程级状态需要清理（原来的 g_optkeys 表已删除）
}

// ==================== 键的类型后缀（2026-11-01，设计见文档第 14 节）====================
// 语法：`@optkeys(["visible:bool", "w:num", "headers:Array", "align"])`
//   冒号后是类型，**不写 = 不查**（与加类型之前完全一致）。
// 类型词：int / float / num / bool / string / Array
//   · `num` 是**注解专用记号**（语言里没有这个类型）：表示"int 或 float 都行"。
//     检查行为与 `float` 完全一致（float 本来就收 int，见 type.c:1047），差别只在**语义表达**：
//     控件的 `x/y/w/h/item_h` 这类尺寸键内部有时按 float 取、有时按 int 取，
//     标 `num` 比标 `float` 准确，也避免维护者误以为"这里必须写 0.5 这种浮点字面量" ✓
//   · float 收 int（int→float 是允许的隐式转换）；int **不**收 float（要 `_int()` 显式转换）
//   · Array[...] 只认外层：`Array[int]` / `Array[Array[int]]` ⇒ 都当 Array（元素暂不检查，见文档 14.6）
//   · 认不出的词（拼错）⇒ 退化成"不查"，不报错（宁漏勿误报）

// 大小写不敏感比较（只用于 6 个类型词，不引 <strings.h>）
static int anno_word_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

// 裸类型词 → **期望的 TypeInfo 类型**；认不出/空 ⇒ TYPE_ANY（= 不查）
//   注：`num` 与 `float` 返回同一个 TYPE_FLOAT —— 检查行为一致，报错文案才区分（见 anno_check_value）
static TypeKind anno_type_word_to_kind(const char* w, size_t len) {
    if (!w || len == 0 || len >= 16) return TYPE_ANY;
    char tmp[16];
    memcpy(tmp, w, len);
    tmp[len] = '\0';
    if (anno_word_eq(tmp, "int"))    return TYPE_INT;
    if (anno_word_eq(tmp, "float"))  return TYPE_FLOAT;
    if (anno_word_eq(tmp, "num"))    return TYPE_FLOAT;   // 数字：int 与 float 都收
    if (anno_word_eq(tmp, "bool"))   return TYPE_BOOL;
    if (anno_word_eq(tmp, "string")) return TYPE_STRING;
    if (anno_word_eq(tmp, "Array"))  return TYPE_ARRAY;
    return TYPE_ANY;
}

// 编辑距离（与 semantic_type_utils 里那份同口径：≤2 才给"你是不是想要"）
static int anno_lev(const char* a, const char* b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 48 || lb > 48) return 99;
    int d[49][49];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int m = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + cost < m) m = d[i - 1][j - 1] + cost;
            d[i][j] = m;
        }
    }
    return d[la][lb];
}

// ==================== 收集：AST_FUNC_DEF 上调用一次 ====================
// ★ 2026-11-01：改成**空实现**。原来这里把每个函数的 `@optkeys` 收集进进程级的
//   `g_optkeys` 表，供"同模块 + AST_CALL 阶段"那条检查路径查；那条路径已删（见文件头），
//   表也就不需要了 —— 注解直接从 `func_def->annos` 读（`annos_check_call_def`），
//   跨模块则从模块符号表读（`annos_check_module`），都不经过这张表 ✓
//   保留函数与调用点：`visit_func_def.inc` 每定义一次就调一次，删调用点会牵动三个文件，
//   留个空函数（一次判断就返回）更省事，将来要加"注解合法性校验"也有地方落 ✓
void annos_collect_func(Ast* ast) {
    (void)ast;
}

// ==================== 检查：AST_CALL 上调用一次 ====================
static const char* anno_dict_key(Ast* k) {
    if (!k) return NULL;
    if (k->kind == AST_STRING) return k->u.string.value;
    if (k->kind == AST_VAR)    return k->u.var.name;    // `{w: 30}` 的键在建树时是标识符 ✓
    return NULL;
}

// ==================== 值类型检查（2026-11-01，见文档第 14 节）====================
// 复用语义分析现成的两个入口，不另立规则：
//   · infer_expr_type()  推断值的类型（semantic_type.c:1071，自带 cached_type）
//   · type_is_compatible() 判断兼容（leno_types.h:513，与参数检查同一口径 ⇒ int→float 等规则一致）
// 覆盖范围因此不止字面量：`{visible: flag}` 里 flag 是 string 也能抓到 ✓
// want_word/want_len：注解里写的**原始类型词**（用于报错文案——`num` 与 `float` 检查行为相同，
//   但文案必须显示用户写的那一个，否则"我写 num 你报 float"会让人莫名其妙）
static void anno_check_value(Semantic* s, Ast* value, const char* want_word, size_t want_len,
                             const char* key, const char* fn_show, const char* where) {
    if (!s || !value || !want_word || want_len == 0) return;
    TypeKind want = anno_type_word_to_kind(want_word, want_len);
    if (want == TYPE_ANY) return;          // 认不出的类型词 ⇒ 不查
    // 只对"字面量 / 变量"推断：函数调用这类先放行，避免在检查阶段提前触发别处的推断
    switch (value->kind) {
        case AST_NUM: case AST_STRING: case AST_BOOL:
        case AST_ARRAY: case AST_DICT: case AST_VAR:
            break;
        default:
            return;
    }
    TypeInfo* got = infer_expr_type(s, value);
    if (!got) return;
    if (got->kind == TYPE_ANY) { type_free(got); return; }
    TypeInfo want_t;
    memset(&want_t, 0, sizeof(want_t));
    want_t.kind = want;
    if (!type_is_compatible(&want_t, got)) {
        char word[16];
        snprintf(word, sizeof(word), "%.*s", (int)want_len, want_word);
        const char* file = error_get_filename();
        fprintf(stderr,
                "%s:%d: [警告] %s 的 opts 键 '%s' 期望 %s，实际给了 %s\n"
                "        提示: 键与类型一起写在 %s 里（如 \"%s:%s\"）\n",
                file ? file : "?", value->line, fn_show, key,
                word, type_kind_to_string(got->kind), where, key, word);
        fflush(stderr);
    }
    type_free(got);
}

// 前向声明：跨模块检查与同模块共用同一段逻辑（定义在文件后半 ✓）
static void anno_check_dict_args(Ast** items, int count, const char* csv,
                                 const char* fn_show, const char* where, Semantic* s);

void annos_check_call(Semantic* s, Ast* ast) {
    if (!ast || ast->kind != AST_CALL) return;

    Ast* callee = ast->u.call.callee;
    if (!callee) return;

    // ① 跨模块：`SDL3.createHBox({...})` —— callee 是 `module.member`
    //   数据来自模块符号表（AST 填充器把 @optkeys 写进去，.lenosymc v39 起随缓存往返 ✓）
    //   ⚠ **必须在这个阶段查**：一旦被改写成 AST_MODULE_CALL，实参已经不是原始字面量了
    //     （实测 `u.module_call.args.items[0]->kind` 读到 7 = AST_VAR，不是 AST_DICT ✗）
    if (callee->kind == AST_MODULE_ACCESS && s != NULL) {
        const char* mname = callee->u.module_access.module_name;
        const char* fname = callee->u.module_access.member_name;
        if (mname && fname) {
            ImportedModuleInfo* mi = find_imported_module(s, mname);
            if (mi && mi->sym_table) {
                ModuleFuncSymbol* fs = module_symbol_table_find_func(mi->sym_table, fname);
                if (fs && fs->optkeys) {
                    char where[256];
                    snprintf(where, sizeof(where), "%s 里该函数的 @optkeys",
                             mi->file_path ? mi->file_path : "被导入模块");
                    anno_check_dict_args(ast->u.call.args.items,
                                         (int)ast->u.call.args.count,
                                         fs->optkeys, fname, where, s);
                }
            }
        }
        return;
    }

    // ② 同模块：callee 是普通函数名
    //   ★ 2026-11-01：**这条不再查了**——同模块的调用在 visit_expr.inc 的参数检查处
    //   还会走一次 `annos_check_call_def`（那里 func_def 已解析，同模块/跨模块通用）。
    //   两处都查 ⇒ 同一条错误打**两遍**（实测 `{visible:"false"}` 每键报 2 次，加上类型
    //   检查变 4 条）。而 call_def 覆盖面完全包含这里（普通函数调用），所以保留那边、
    //   砍掉这条；跨模块的 `module.member(...)` 仍由上面的 ① 处理（AST 改写前才拿得到字面量 ✓）
    (void)s;
    (void)ast;
    (void)callee;
    return;
}

// ==================== 跨模块（2026-10-31）====================
//   数据来源：模块符号表里函数的 `optkeys`（逗号分隔，.lenosymc v39 起随缓存往返 ✓）
//   为什么这里用"逗号串"而不是 AnnoOptKeys 表：跨模块时调用方**拿不到被调函数的 AST**，
//     只能从符号表拿到一个字符串 —— 就地解析比再建一张表省事 ✓

// 逗号串里的一项 `align:int`：键名是冒号前那段（2026-11-01 起带类型后缀）
static size_t anno_csv_item_name_len(const char* p, size_t len) {
    size_t nl = 0;
    while (nl < len && p[nl] != ':') nl++;
    return nl;
}

static int anno_csv_has(const char* csv, const char* k) {
    if (!csv || !k) return 0;
    size_t kl = strlen(k);
    const char* p = csv;
    while (*p) {
        const char* e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        // ★ 只比冒号前的键名：`visible:bool` 与调用点的 `visible` 是同一个键 ✓
        if (anno_csv_item_name_len(p, len) == kl && strncmp(p, k, kl) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

// 取某键在逗号串里声明的**类型词**（返回词长，0 = 没写；*out_word 指向 csv 内部，不拷贝）
//   为什么返回词而不是 TypeKind：报错文案要显示用户写的原词（如 `num`），见 anno_check_value ✓
static size_t anno_csv_type_word(const char* csv, const char* k, const char** out_word) {
    *out_word = NULL;
    if (!csv || !k) return 0;
    size_t kl = strlen(k);
    const char* p = csv;
    while (*p) {
        const char* e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (anno_csv_item_name_len(p, len) == kl && strncmp(p, k, kl) == 0) {
            if (len > kl + 1 && p[kl] == ':') {
                const char* t = p + kl + 1;
                size_t tl = 0;
                while (tl < (len - kl - 1) && t[tl] != '[' && t[tl] != ' ') tl++;
                *out_word = t;
                return tl;
            }
            return 0;
        }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

// 在逗号串里找与 k 最相近的键（≤2）；找到写进 out 并返回 1
//   ⚠ 只拿**键名**比：`align:int` 与 `algin` 的编辑距离是 5，不截断就一次都提示不出来 ✗
static int anno_csv_nearest(const char* csv, const char* k, char* out, size_t outsz) {
    int bestd = 3;
    const char* p = csv;
    out[0] = '\0';
    while (*p) {
        const char* e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        size_t nlen = anno_csv_item_name_len(p, len);
        if (nlen > 0 && nlen < 48) {
            char tmp[48];
            memcpy(tmp, p, nlen);
            tmp[nlen] = '\0';
            int d = anno_lev(k, tmp);
            if (d < bestd) {
                bestd = d;
                snprintf(out, outsz, "%s", tmp);
            }
        }
        if (!e) break;
        p = e + 1;
    }
    return bestd < 3;
}

// 对一组实参按逗号串键表检查（同模块 / 跨模块共用）
static void anno_check_dict_args(Ast** items, int count, const char* csv,
                                 const char* fn_show, const char* where, Semantic* s) {
    if (!csv || !csv[0] || strcmp(csv, "*") == 0) return;
    const char* file = error_get_filename();
    for (int i = 0; i < count; i++) {
        Ast* arg = items[i];
        if (!arg || arg->kind != AST_DICT) continue;      // 只查字面量 ✓
        for (int j = 0; j < arg->u.dict.count; j++) {
            const char* key = anno_dict_key(arg->u.dict.entries[j].key);
            if (!key || !key[0]) continue;
            if (anno_csv_has(csv, key)) {
                // 键名对上了 ⇒ 若声明里带类型，再看值（2026-11-01）
                const char* tw = NULL;
                size_t tl = anno_csv_type_word(csv, key, &tw);
                if (tl > 0) {
                    anno_check_value(s, arg->u.dict.entries[j].value, tw, tl, key, fn_show, where);
                }
                continue;
            }
            char hint[48];
            int has = anno_csv_nearest(csv, key, hint, sizeof(hint));
            if (has) {
                fprintf(stderr,
                        "%s:%d: [警告] %s 的 opts 不认识键 '%s'\n"
                        "        提示: 你是不是想要 '%s'？\n"
                        "        提示: 接受的键见 %s\n",
                        file ? file : "?", arg->line, fn_show, key, hint, where);
            } else {
                fprintf(stderr,
                        "%s:%d: [警告] %s 的 opts 不认识键 '%s'\n"
                        "        提示: 接受的键见 %s\n",
                        file ? file : "?", arg->line, fn_show, key, where);
            }
            fflush(stderr);
        }
        return;      // 只查第一个 Dict 字面量 ✓
    }
}

// 跨模块调用：`SDL3.createHBox({...})`
//   keys_csv / mod_path 由调用方从模块符号表取（见 visit_module.inc 的 AST_MODULE_CALL ✓）
void annos_check_module(Ast* ast, const char* keys_csv, const char* mod_path, Semantic* s) {
    if (!ast || ast->kind != AST_MODULE_CALL) return;
    char where[256];
    snprintf(where, sizeof(where), "%s 里该函数的 @optkeys", mod_path ? mod_path : "被导入模块");
    anno_check_dict_args(ast->u.module_call.args.items,
                         (int)ast->u.module_call.args.count,
                         keys_csv,
                         ast->u.module_call.method_name,
                         where, s);
}

// 从"被调函数的 AST"上直接取 @optkeys 再检查 —— **同模块 / 跨模块通用** ✓
//   为什么这条最好用：调用点（visit_expr.inc 的参数个数检查处）本来就把 `func_def` 解出来了
//   （跨模块也一样，否则报不出"参数数量不足"）⇒ 直接读 `func_def->annos`，不依赖符号表、
//   也不受"AST 在哪个阶段被改写"的影响（前面试过 AST_CALL / AST_MODULE_CALL / infer_expr_type
//   三处都不稳定：控件构造调用实测只走到参数检查这一处 ✗）
void annos_check_call_def(Ast* call_ast, Ast* func_def, Semantic* s) {
    if (!call_ast || call_ast->kind != AST_CALL) return;
    if (!func_def || func_def->anno_count <= 0 || !func_def->annos) return;

    char keys[2048];
    keys[0] = '\0';
    int wildcard = 0, has_anno = 0;
    for (int i = 0; i < func_def->anno_count && !wildcard; i++) {
        AstAnno* a = &func_def->annos[i];
        if (!a->name || strcmp(a->name, "optkeys") != 0) continue;
        has_anno = 1;
        if (a->arg_kind == 1 && a->arg_str && strcmp(a->arg_str, "*") == 0) { wildcard = 1; break; }
        if (a->arg_kind != 5) continue;
        for (int k = 0; k < a->arg_list_len; k++) {
            const char* key = a->arg_list[k];
            if (!key || !key[0]) continue;
            size_t used = strlen(keys);
            if (used + strlen(key) + 2 >= sizeof(keys)) break;   // 超长截断：宁少报不误报 ✓
            if (used) strcat(keys, ",");
            strcat(keys, key);
        }
    }
    if (!has_anno || wildcard || !keys[0]) return;

    const char* fname = func_def->u.func.name ? func_def->u.func.name : "?";
    char where[160];
    snprintf(where, sizeof(where), "%s 的 @optkeys", fname);
    anno_check_dict_args(call_ast->u.call.args.items,
                         (int)call_ast->u.call.args.count,
                         keys, fname, where, s);
}
