#include "include/native.h"
#include <stdio.h>      // snprintf（"不支持的转义"错误消息要用，见 re_report_invalid）
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

// 前向声明：字符串对象创建函数
extern ObjString* str_new(const char* chars, int len);
extern ObjString* str_copy(const char* chars, int len);
extern ObjString* str_alloc(int len);

// 前向声明：数组操作
extern ObjArray* arr_new(int capacity);
extern int arr_grow(ObjArray* arr);

// 前向声明：字典操作
extern ObjDict* dict_new(int capacity);
extern void dict_set(ObjDict* dict, Value key, Value value);

// 辅助函数：向数组添加元素
static void arr_push_custom(ObjArray* arr, Value value) {
    if (arr->count >= arr->capacity) {
        arr_grow(arr);
    }
    arr->elements[arr->count++] = value;
    gc_write_barrier((Object*)arr, value);
}

// ==================== 简单正则表达式引擎 ====================
// 支持：. ^ $ * + ? [] () |

typedef enum {
    RE_CHAR,         // 普通字符
    RE_ANY,          // .
    RE_START,        // ^
    RE_END,          // $
    RE_CLASS,        // [...]
    RE_CLASS_NEG,    // [^...]
    RE_GROUP_START,  // ( 的起点标记（携带组号，零宽）
    RE_GROUP_END,    // ) 的终点标记（携带组号，零宽）
    RE_OR,           // |
    RE_STAR,         // *
    RE_PLUS,         // +
    RE_QUESTION,     // ?
    RE_EMPTY,        // 空表达式（`()` / `a|` 这类）—— **零宽成功**
    RE_END_PATTERN   // 结束标记
} ReOp;

// 捕获组上限（内存池按节点算：一组占 2 个标记节点 ⇒ 16 组足够，超出在解析期报错 ✓）
#define RE_MAX_GROUPS 16

typedef struct ReNode {
    ReOp op;
    char ch;            // RE_CHAR 用
    char* class_chars;  // RE_CLASS/RE_CLASS_NEG 用
    int class_len;
    int group;             // RE_GROUP_START / RE_GROUP_END 用（组号从 1 起）
    struct ReNode* left;   // 左子节点（OR / 量词指向被重复的子链）
    struct ReNode* right;  // 右子节点（OR）
    struct ReNode* next;   // 下一个节点
} ReNode;

// 捕获日志单条：记下"某组被改写前的旧值"，回溯时按水位撤销 ✓
typedef struct { int gi; int is_end; int old; } ReCapLog;

// 捕获日志上限（满了就**不再记**：匹配照常进行，只是极端回溯下捕获可能不准 —— 记注释不静默 ✓）
#define RE_CAP_LOG_MAX 256

// 递归深度保护：防病态 pattern（如 `(a*)*`）把栈吃光 ⇒ 超限直接判失败 ✓
#define RE_DEPTH_MAX 4096

// 一次匹配的上下文（**放栈上、随参数传递** ⇒ 不用全局状态，多路匹配/递归组互不干扰 ✓）
typedef struct {
    const char* str;                        // 主机串（算偏移用）
    int gstart[RE_MAX_GROUPS + 1];          // 各组起点偏移（-1 = 未参与本次匹配）
    int gend[RE_MAX_GROUPS + 1];            // 各组终点偏移（不含）
    int ngroup;                             // 本次 pattern 的组数
    int mstart;                             // 本次**整体匹配**的区间（`$0` 展开要用 ✓）
    int mend;
    ReCapLog log[RE_CAP_LOG_MAX];           // 捕获改写日志（回溯回滚用 ✓）
    int nlog;
    int depth;                              // 当前递归深度
} ReCtx;

static void re_ctx_reset(ReCtx* ctx, const char* str, int ngroup) {
    ctx->str = str;
    ctx->ngroup = ngroup;
    ctx->mstart = -1;
    ctx->mend = -1;
    ctx->nlog = 0;
    ctx->depth = 0;
    for (int i = 0; i <= RE_MAX_GROUPS; i++) {
        ctx->gstart[i] = -1;
        ctx->gend[i] = -1;
    }
}

// 记一条捕获改写（日志满 ⇒ 丢弃，不报错 ✓）
static void re_cap_push(ReCtx* ctx, int gi, int is_end, int old) {
    if (ctx->nlog < RE_CAP_LOG_MAX) {
        ctx->log[ctx->nlog].gi = gi;
        ctx->log[ctx->nlog].is_end = is_end;
        ctx->log[ctx->nlog].old = old;
        ctx->nlog++;
    }
}

// 回滚到某个水位（回溯时把捕获恢复到该点 ✓）
static void re_cap_rollback(ReCtx* ctx, int mark) {
    while (ctx->nlog > mark) {
        ctx->nlog--;
        ReCapLog* L = &ctx->log[ctx->nlog];
        if (L->is_end) ctx->gend[L->gi] = L->old;
        else           ctx->gstart[L->gi] = L->old;
    }
}

// 简单的内存池
#define RE_POOL_SIZE 256
static ReNode re_pool[RE_POOL_SIZE];
static int re_pool_idx = 0;

static ReNode* re_alloc_node(void) {
    if (re_pool_idx >= RE_POOL_SIZE) return NULL;
    ReNode* node = &re_pool[re_pool_idx++];
    memset(node, 0, sizeof(ReNode));
    return node;
}

static void re_free_all(void) {
    for (int i = 0; i < re_pool_idx; i++) {
        if (re_pool[i].class_chars) {
            free(re_pool[i].class_chars);
        }
    }
    re_pool_idx = 0;
}

// 解析期的组号计数器（**只在一次 parse 会话内有效**：入口置 0、解析完立刻读走 ✓）。
//   ⚠ 顺带记录一笔既有事实：本模块整体**不是线程安全的** —— 内存池 `re_pool` 与它都是静态的，
//     多线程同时调 regexs 会互相踩（2026-10-01 记录在案；本轮未处理，见 docs 的说明）。
static int re_group_counter = 0;

// 可转义的字符 —— **元字符 + `-` + `/`**。其余（`\d` `\w` `\s` `\b` `\t` … 字母数字类）一律算
// "不支持的转义"，编译期就失败 ⇒ 上层报错。
//   为什么必须报错、不能当字面量：`\d` 现在被当**字面字母 d** ⇒ `regexs.match("123", "\\d+")`
//   返回 false、`match("d", "\\d")` 返回 true（2026-10-01 实测）—— 而 Python 的 `r"\d+"`
//   照抄过来正是这个下场，**且编译器不给任何警告**（只有写错成单反斜杠 `"\d"` 才有
//   `[无效转义]` 提示）⇒ 这是最难查的一类"静默失配"。宁可响亮失败 ✓
//   写法：数字类用 `[0-9]`、字母类用 `[A-Za-z]`、单词类用 `[A-Za-z0-9_]`、空白用 `[ ]` ✓
static bool re_is_escapable(char c) {
    return strchr(".^$*+?()[]{}|\\/-", c) != NULL;
}

// 编译失败时给出**具体**消息（指出第一个不支持的转义）。
//   为什么不设全局错误缓冲：那是静态可写状态，多线程下（threads 模块）会互相踩 ✗；
//   这里改为把 pattern 再扫一遍 —— 无状态、可重入 ✓
static void re_report_invalid(const char* pattern) {
    for (const char* q = pattern; *q; q++) {
        if (*q == '\\' && *(q + 1)) {
            if (!re_is_escapable(*(q + 1))) {
                char msg[192];
                snprintf(msg, sizeof(msg),
                         "无效的正则表达式：不支持的转义 \"\\%c\"（本引擎只支持元字符转义；"
                         "数字类请写 [0-9]，字母类 [A-Za-z]，空白 [ ]）",
                         *(q + 1));
                native_throw_error(msg);
                return;
            }
            q++;
            continue;
        }
        // 量词写法（`{` / `}`）—— 与 shorthand 同理：不支持就**说清楚**，不要静默当字面量 ✓
        if (*q == '{' || *q == '}') {
            native_throw_error("无效的正则表达式：不支持量词 {}（本引擎只有 * + ?；"
                               "要匹配字面花括号请写 \\{ 或 \\}）");
            return;
        }
        // POSIX 字符类写法（`[:digit:]` 等）
        if (*q == '[' && *(q + 1) == ':') {
            native_throw_error("无效的正则表达式：不支持 POSIX 字符类 [:...:]"
                               "（数字写 [0-9]、字母写 [A-Za-z]、空白写 [ ]）");
            return;
        }
    }
    native_throw_error("无效的正则表达式");
}

// 解析字符类 [abc] 或 [^abc]
static const char* parse_class(const char* p, ReNode* node) {
    bool negated = false;
    if (*p == '^') {
        negated = true;
        p++;
    }
    
    node->op = negated ? RE_CLASS_NEG : RE_CLASS;
    
    // 收集字符
    char chars[256];
    int len = 0;
    
    while (*p && *p != ']' && len < 256) {
        // POSIX 字符类 `[:digit:]` 这类 **不支持**（文档曾承诺、实现从来没有）⇒ 编译失败、响亮报错 ✓
        if (*p == '[' && *(p+1) == ':') return NULL;
        if (*p == '\\' && *(p+1)) {
            // 类内转义同理（`\-` 也在允许集合里 —— 类内它是"阻止范围解释"的正当写法 ✓）
            if (!re_is_escapable(*(p+1))) return NULL;
            p++;
            chars[len++] = *p++;
        } else if (*p == '-' && len > 0 && *(p+1) && *(p+1) != ']') {
            // 范围 a-z
            char start = chars[len-1];
            char end = *(++p);
            for (char c = start + 1; c <= end && len < 256; c++) {
                chars[len++] = c;
            }
            p++;
        } else {
            chars[len++] = *p++;
        }
    }
    
    // 未闭合的字符类（如 `[invalid` / 单个 `[`）⇒ **编译失败、响亮报错** ✓
    //   （以前会一路吃到串尾当"字符类到此为止"⇒ 静默按错的内容匹配 ✗，与"不静默"的方针相冲）
    if (*p != ']') return NULL;
    p++;
    
    node->class_chars = (char*)malloc(len + 1);
    if (node->class_chars) {
        memcpy(node->class_chars, chars, len);
        node->class_chars[len] = '\0';
        node->class_len = len;
    }
    
    return p;
}

// 解析正则表达式
static ReNode* parse_regex(const char** pp);
static ReNode* parse_term(const char** pp);
static ReNode* parse_factor(const char** pp);

static ReNode* parse_regex(const char** pp) {
    ReNode* left = parse_term(pp);
    if (!left) return NULL;
    
    while (**pp == '|') {
        (*pp)++;
        ReNode* right = parse_term(pp);
        if (!right) return right;
        
        ReNode* or_node = re_alloc_node();
        if (!or_node) return NULL;
        or_node->op = RE_OR;
        or_node->left = left;
        or_node->right = right;
        left = or_node;
    }
    
    return left;
}

static ReNode* parse_term(const char** pp) {
    ReNode* first = NULL;
    ReNode* last = NULL;
    
    while (**pp && **pp != ')' && **pp != '|') {
        ReNode* node = parse_factor(pp);
        if (!node) return NULL;

        // ⚠ 串联必须串到**链的真尾**，而不是"刚解析出来的那个节点"：
        //   组的产物是 `START → 子链 → END` **一条链**（parse_factor 返回链头 START）⇒
        //   若把 START 直接当尾去接下一个元素，就会把 `START->next`（子链）**覆盖掉** ✗
        //   （2026-10-01 实测：`([0-9]+)-([0-9]+)` 这类带组的 pattern 因此永远匹配不上 ✓）
        ReNode* tail = node;
        while (tail->next) tail = tail->next;
        if (!first) {
            first = last = node;
        } else {
            last->next = node;
        }
        last = tail;
    }
    
    if (first) return first;
    // 空表达式（`()` / `a|`）：给一个**零宽成功**节点。
    //   ⚠ 以前这里直接返回 `re_alloc_node()`（op 字段是 memset 出来的 0 ⇒ 恰好等于 RE_CHAR、
    //     ch=0）⇒ 匹配器把它当"匹配 NUL 字符"，成功时还把位置前推 1 字节 —— 在串尾就是**越界读** ✗
    //     （2026-10-01 随匹配器重写一并修正 ✓）
    ReNode* empty = re_alloc_node();
    if (!empty) return NULL;
    empty->op = RE_EMPTY;
    return empty;
}

static ReNode* parse_factor(const char** pp) {
    const char* p = *pp;
    ReNode* node = NULL;
    
    if (*p == '(') {
        p++;
        // 组 = `START 标记 → 子链 → END 标记`（三段都挂在序列上）。
        //   为什么展平、而不是"一个 GROUP 节点包住子链"：展平后匹配器只要按普通序列走，
        //   起点/终点各记一次 ⇒ **不必给组写专门的匹配与续延代码**，回溯回滚也统一 ✓
        int gi = ++re_group_counter;
        if (gi > RE_MAX_GROUPS) return NULL;      // 超上限 ⇒ 编译失败（上层报到错误 ✓）
        ReNode* start = re_alloc_node();
        if (!start) return NULL;
        start->op = RE_GROUP_START;
        start->group = gi;
        ReNode* sub = parse_regex(&p);
        // ⚠ 子表达式失败（如组内写了 `(\d)`）必须**向上传播 NULL**：否则会带着 NULL 继续解析
        //   ⇒ 既不报错、又静默按"空表达式"匹配 ✗（2026-10-01 补）
        if (!sub) return NULL;
        ReNode* tail = sub;
        while (tail->next) tail = tail->next;
        ReNode* endn = re_alloc_node();
        if (!endn) return NULL;
        endn->op = RE_GROUP_END;
        endn->group = gi;
        tail->next = endn;                        // 子链尾接 END
        start->next = sub;                        // START 接子链
        node = start;
        if (*p == ')') p++;
    } else if (*p == '.') {
        p++;
        node = re_alloc_node();
        if (!node) return NULL;
        node->op = RE_ANY;
    } else if (*p == '^') {
        p++;
        node = re_alloc_node();
        if (!node) return NULL;
        node->op = RE_START;
    } else if (*p == '$') {
        p++;
        node = re_alloc_node();
        if (!node) return NULL;
        node->op = RE_END;
    } else if (*p == '[') {
        p++;
        node = re_alloc_node();
        if (!node) return NULL;
        p = parse_class(p, node);
        // ⚠ 字符类解析失败返回 NULL ⇒ **必须在这里拦住**：原来直接 `p = parse_class(...)`，
        //   接着就用 NULL 指针做 `*p` ⇒ 段错误（2026-10-01 实测 `[\d]` 崩溃 0xC0000005）✗
        if (!p) return NULL;
    } else if (*p == '\\' && *(p+1)) {
        // ★ 只认元字符转义：`\d` / `\w` / `\s` 这类 shorthand **不支持** ⇒ 编译失败 ⇒ 上层响亮报错
        //   （以前是把 `\d` 当字面字母 d —— 静默失配，理由见 re_is_escapable 的函数头 ✓）
        if (!re_is_escapable(*(p+1))) return NULL;
        p++;
        node = re_alloc_node();
        if (!node) return NULL;
        node->op = RE_CHAR;
        node->ch = *p++;
    } else if (*p == '{' || *p == '}') {
        // 量词 `{n}` / `{n,}` / `{n,m}` **不支持**（文档曾承诺，实现从来没有 —— 2026-10-01 实测：
        //   `a{2}` 匹配 "aa" 是 false、匹配字面 "a{2}" 是 true ⇒ 静默错 ✗）⇒ 编译失败、响亮报错。
        //   要匹配字面花括号请转义 `\{` / `\}`（在允许集合里 ✓）
        return NULL;
    } else if (*p && strchr("*+?|)", *p) == NULL) {
        node = re_alloc_node();
        if (!node) return NULL;
        node->op = RE_CHAR;
        node->ch = *p++;
    } else {
        return NULL;
    }
    
    // 处理量词
    if (*p == '*' || *p == '+' || *p == '?') {
        ReNode* quant = re_alloc_node();
        if (!quant) return NULL;
        quant->left = node;
        if (*p == '*') quant->op = RE_STAR;
        else if (*p == '+') quant->op = RE_PLUS;
        else quant->op = RE_QUESTION;
        p++;
        node = quant;
    }
    
    *pp = p;
    return node;
}

// 解析入口：把 pattern 编成节点链，并**带出组数**（组号 1..ngroup ✓）
//   ⚠ 必须整体成功才可用：中途失败（不支持的语法 / 节点池耗尽）⇒ 返回 NULL ⇒ 上层报错 ✓
static ReNode* re_parse(const char* pattern, int* out_ngroup) {
    re_group_counter = 0;
    const char* p = pattern;
    ReNode* n = parse_regex(&p);
    *out_ngroup = re_group_counter;
    return n;
}

// 匹配字符类
static bool match_class(ReNode* node, char c) {
    bool found = false;
    for (int i = 0; i < node->class_len; i++) {
        if (node->class_chars[i] == c) {
            found = true;
            break;
        }
    }
    return (node->op == RE_CLASS) ? found : !found;
}

// （旧的 `执行匹配` 入口 match_node / match_regex 已于 2026-10-01 重写为 m_chain / m_repeat ✓）

// ==================== 匹配：递归回溯 + 捕获（2026-10-01 重写）====================
// 旧实现是"顺序走链、节点匹配完把位置往下传" ⇒ 量词只**贪婪吃满、从不回退**
//   ⇒ `[a-z.]+\.[a-z]+` 这类"贪婪段后接字面量"的写法永远匹配不上（静默 false ✗）。
// 新结构：`m_chain(node, pos)` = "把 node 这条链从 pos 匹配完，成功给出结束位置"；
//   每个节点**递归匹配自己的 next** ⇒ 量词可以把"剩余链"当续延、逐位回退再试 ✓
//   · 量词：先贪婪吃满，再从**最长往短**回退尝试（经典回溯 ✓，贪吃但会吐）
//   · OR：先左后右，任一支成功即成功 ✓
//   · 捕获：`(` `)` 在解析期已展平成 GROUP_START / GROUP_END 零宽标记 ⇒ 匹配到就记位置；
//     回溯必须**回滚** ⇒ 用"捕获日志"按水位撤销 ✓
//   ⚠ 已知限制（有意保留，避免复杂度失控）：量词**单元内部**只取"首个可行解"
//     （如 `(a|ab)+` 不会为单元尝试第二种切分）；深度超 RE_DEPTH_MAX 判失败 ✓

static bool m_chain(ReNode* node, const char* pos, ReCtx* ctx, const char** out);

// 把 unit 从 pos 起重复匹配 k 次（迭代、不回滚捕获 ⇒ 用来"重放"修正捕获 ✓）
static bool m_replay(ReNode* unit, const char* pos, int k, ReCtx* ctx, const char** out) {
    const char* p = pos;
    for (int i = 0; i < k; i++) {
        const char* nx;
        if (!m_chain(unit, p, ctx, &nx) || nx == p) return false;
        p = nx;
    }
    *out = p;
    return true;
}

// 量词（* / +）：贪婪吃满 ⇒ 从最长往短回退，每次都用"剩余链"当续延再试 ✓
static bool m_repeat(ReNode* node, const char* pos, ReCtx* ctx, const char** out) {
    int min = (node->op == RE_PLUS) ? 1 : 0;
    int mark = ctx->nlog;

    // ① 先数出"最多能吃几次"（只计数：回退时用重放定位，省一个历史数组 ✓）
    int cnt = 0;
    {
        const char* p = pos;
        while (true) {
            const char* nx;
            if (!m_chain(node->left, p, ctx, &nx)) break;
            if (nx == p) break;                 // 零宽单元 ⇒ 立刻停（防死循环 ✓）
            p = nx;
            cnt++;
        }
    }

    // ② 贪婪回退：k 从 cnt 递减到 min
    //   ⚠ 最坏 O(cnt²)（每次回退都要重放定位）—— 这是正则引擎常见的"灾难回溯"成本，
    //     病态 pattern（如 `(a+)+b` 匹配一长串 a 无 b）会慢但不会错/不会崩 ✓
    for (int k = cnt; k >= min; k--) {
        re_cap_rollback(ctx, mark);             // 每次回到"进入量词时"的捕获状态 ✓
        const char* at = pos;
        if (k > 0 && !m_replay(node->left, pos, k, ctx, &at)) continue;
        if (m_chain(node->next, at, ctx, out)) {
            if (k > 0) {
                // 成功路径确定后**重放一次**，把重复单元里的捕获修正成最终路径的值 ✓
                //   （重放只写捕获、不改位置：位置已由 out 决定 ✓）
                const char* dummy;
                m_replay(node->left, pos, k, ctx, &dummy);
            }
            return true;
        }
    }
    re_cap_rollback(ctx, mark);
    return false;
}

// 链匹配：node 为 NULL ⇒ 整条链走完 ⇒ 成功（*out = pos）✓
static bool m_chain(ReNode* node, const char* pos, ReCtx* ctx, const char** out) {
    if (!node) { *out = pos; return true; }
    if (ctx->depth >= RE_DEPTH_MAX) return false;   // 病态 pattern 的深度保护 ✓

    ctx->depth++;
    bool ok = false;

    switch (node->op) {
        case RE_CHAR:
            if (*pos && *pos == node->ch) ok = m_chain(node->next, pos + 1, ctx, out);
            break;
        case RE_ANY:
            if (*pos) ok = m_chain(node->next, pos + 1, ctx, out);
            break;
        case RE_CLASS:
        case RE_CLASS_NEG:
            if (*pos && match_class(node, *pos)) ok = m_chain(node->next, pos + 1, ctx, out);
            break;
        case RE_EMPTY:
            ok = m_chain(node->next, pos, ctx, out);
            break;
        case RE_START:
            // 本引擎只按**整串**开头算行首（没有多行模式 ⇒ `^` 恒等于"位置 0" ✓）
            if (pos == ctx->str) ok = m_chain(node->next, pos, ctx, out);
            break;
        case RE_END:
            if (*pos == '\0') ok = m_chain(node->next, pos, ctx, out);
            break;
        case RE_GROUP_START: {
            int gi = node->group;
            re_cap_push(ctx, gi, 0, ctx->gstart[gi]);
            ctx->gstart[gi] = (int)(pos - ctx->str);
            ok = m_chain(node->next, pos, ctx, out);
            break;
        }
        case RE_GROUP_END: {
            int gi = node->group;
            re_cap_push(ctx, gi, 1, ctx->gend[gi]);
            ctx->gend[gi] = (int)(pos - ctx->str);
            ok = m_chain(node->next, pos, ctx, out);
            break;
        }
        case RE_STAR:
        case RE_PLUS:
            ok = m_repeat(node, pos, ctx, out);
            break;
        case RE_QUESTION: {
            int mark = ctx->nlog;
            if (m_chain(node->left, pos, ctx, out)) { ok = true; break; }   // 先试"吃一次"
            re_cap_rollback(ctx, mark);                                     // 失败 ⇒ 撤销
            ok = m_chain(node->next, pos, ctx, out);                        // 再试"跳过"
            break;
        }
        case RE_OR: {
            int mark = ctx->nlog;
            const char* at = NULL;
            // 左分支：先匹配分支子链，成功后再接着匹配"OR 之后的链" ✓
            if (m_chain(node->left, pos, ctx, &at) && m_chain(node->next, at, ctx, out)) {
                ok = true;
                break;
            }
            re_cap_rollback(ctx, mark);                                     // 回滚左分支的副作用 ✓
            if (m_chain(node->right, pos, ctx, &at) && m_chain(node->next, at, ctx, out)) {
                ok = true;
                break;
            }
            re_cap_rollback(ctx, mark);
            break;
        }
        default:
            break;
    }

    ctx->depth--;
    return ok;
}

// （旧的 match_node 已随 2026-10-01 的匹配器重写删除：它顺序走链、量词只贪吃不会吐 ⇒
//   静默失配；新逻辑在 m_chain / m_repeat。删掉而不是留着，免得两套匹配语义并存 ✓）

// 查找第一个匹配：从每个起点逐一试（**含串尾** —— 空 pattern / `$` 也要能在末尾命中 ✓）
//   ⚠ `base` 与 `from` **必须分开**：`base` 是整个原串起点（**捕获偏移的基准**，也是 `^` 的判定基准），
//     `from` 只是"从哪儿开始找"。replace_all 会在**同一原串上分段落**继续搜索 ——
//     若拿分段起点当偏移基准，捕获偏移就变成"相对那一段"的 ⇒ 展开替换串时取到错的内容 ✗
//     （2026-10-01 实测：`replace_all("a1b2", "([a-z])([0-9])", "$1")` 曾得到 "aa" ✗）
static const char* find_match(ReNode* pattern, const char* base, const char* from, int ngroup, ReCtx* ctx,
                              const char** start, const char** end) {
    for (const char* pos = from; ; pos++) {
        re_ctx_reset(ctx, base, ngroup);         // 每个起点都从干净状态试 ✓（基准恒为 base ✓）
        const char* e = NULL;
        if (m_chain(pattern, pos, ctx, &e)) {
            *start = pos;
            *end = e;
            ctx->mstart = (int)(pos - base);     // 整体匹配区间（`$0` 展开用 ✓）
            ctx->mend = (int)(e - base);
            return e;
        }
        if (*pos == '\0') break;                 // 串尾也试过了 ⇒ 结束
    }
    return NULL;
}

// ==================== 核心方法实现 ====================

// 展开替换串里的反向引用（2026-10-01 真支持，此前是"报错挡住" ✓）：
//   `$0` = 整体匹配，`$1`..`$9` = 第 N 个捕获组，`$$` = 字面 `$`；
//   其余 `$x`（后面不是数字/$）按字面 `$x` 原样保留 ✓；**超出组号**（如只有 2 组却写 `$5`）⇒ 展开成空串 ✓
//   dst 传 NULL 时只算长度（`replace_all` 需要先知道总长 ✓）—— 一个函数两用，避免长度/填充两套逻辑漂移 ✓
static int re_expand_replacement(const char* repl, ObjString* str, ReCtx* ctx, char* dst) {
    int n = 0;
    for (int i = 0; repl[i]; i++) {
        if (repl[i] == '$' && repl[i + 1] == '$') {
            if (dst) dst[n] = '$';
            n++;
            i++;
            continue;
        }
        if (repl[i] == '$' && repl[i + 1] >= '0' && repl[i + 1] <= '9') {
            int gi = repl[i + 1] - '0';
            int s = -1;
            int e = -1;
            if (gi == 0) {
                s = ctx->mstart;           // $0 = 整体匹配 ✓
                e = ctx->mend;
            } else if (gi <= ctx->ngroup) {
                s = ctx->gstart[gi];       // 未参与的组 ⇒ -1 ⇒ 展开成空串 ✓
                e = ctx->gend[gi];
            }
            if (s >= 0 && e >= s) {
                if (dst) memcpy(dst + n, str->chars + s, (size_t)(e - s));
                n += (e - s);
            }
            i++;
            continue;
        }
        if (dst) dst[n] = repl[i];
        n++;
    }
    return n;
}

// 1. 检查字符串是否匹配正则表达式
static Value regex_match(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));

    ReCtx ctx;                                  // 捕获状态（栈上，几 KB ✓）
    int ngroup = 0;
    ReNode* pattern = re_parse(pattern_str->chars, &ngroup);
    
    if (!pattern) {
        re_free_all();
        re_report_invalid(pattern_str->chars);   // 具体到"哪个转义不支持"（原来只有一句笼统话 ✓）
        return val_null();
    }
    
    re_ctx_reset(&ctx, str->chars, ngroup);
    const char* e = NULL;
    bool matched = m_chain(pattern, str->chars, &ctx, &e);   // match = 从头部**锚定**（等价 `^pattern` ✓）
    
    re_free_all();
    return val_bool(matched);
}

// 2. 查找第一个匹配位置
static Value regex_find(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));

    ReCtx ctx;                                  // 捕获状态（栈上，几 KB ✓）
    int ngroup = 0;
    ReNode* pattern = re_parse(pattern_str->chars, &ngroup);
    
    if (!pattern) {
        re_free_all();
        re_report_invalid(pattern_str->chars);   // 具体到"哪个转义不支持"（原来只有一句笼统话 ✓）
        return val_null();
    }
    
    const char* start = NULL;
    const char* end = NULL;
    find_match(pattern, str->chars, str->chars, ngroup, &ctx, &start, &end);
    
    re_free_all();
    
    if (start) {
        // 返回 0-based 位置
        return val_int((int)(start - str->chars));
    }
    return val_int(-1);
}

// ==================== RegexMatch 的类型规格（find_all 的返回元素，v3.2.7） ====================
// 为什么不用 Dict（这是全仓**最后一个**返回裸 Dict 的地方）：
//   三个键**类型不齐** —— `start` / `end` 是 int、`text` 是 string ⇒ 同质的 `Dict[K,V]`
//   表达不了"这个键 int、那个键 string"；退一步用裸 `Dict` 则让调用点拿到 any
//   （`m["text"]` 是 any，还得手动收窄）。
// 改结构体后字段类型**编译期已知**、零收窄 —— 与 `DirEntry`（dirs.walk）/ `DirInfo`（dirs.stat）
//   同一套做法：**编译期字段表 + 运行期 ObjStructDef 是同一份声明** ⇒ 字段顺序不可能漂。
// 字段名与旧的字典键**逐字相同**（start / end / text）⇒ 只是取值方式从 `m["x"]` 变成 `m.x`。
// 顺带：不再每次匹配都 `str_copy` 三个键名（那是 3 次分配 → 0 次）。
static const NativeTypeSpec S_RX_STRING          = { NTYPE_STRING, NULL, NULL, NULL, 0, -1 };
static const NativeTypeSpec S_RX_INT             = { NTYPE_INT,    NULL, NULL, NULL, 0, -1 };
static const NativeTypeSpec S_REGEXMATCH_SPEC    = { NTYPE_STRUCT, "RegexMatch", NULL, NULL, 0, -1 };
static const NativeTypeSpec S_REGEXMATCH_ARR_SPEC = { NTYPE_ARRAY, NULL, &S_REGEXMATCH_SPEC, NULL, 0, -1 };

static const char* REGEXMATCH_FIELD_NAMES[] = { "start", "end", "text" };
static const NativeTypeSpec* REGEXMATCH_FIELD_TYPES[] = { &S_RX_INT, &S_RX_INT, &S_RX_STRING };
static const NativeStructSpec REGEXMATCH_STRUCT_SPEC = {
    "regexs", "RegexMatch", 3, REGEXMATCH_FIELD_NAMES, REGEXMATCH_FIELD_TYPES
};

// 3. 查找所有匹配位置（返回 `Array[RegexMatch]`，v3.2.7 起）
static Value regex_find_all(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));

    ReCtx ctx;                                  // 捕获状态（栈上，几 KB ✓）
    int ngroup = 0;
    ReNode* pattern = re_parse(pattern_str->chars, &ngroup);
    
    if (!pattern) {
        re_free_all();
        re_report_invalid(pattern_str->chars);   // 具体到"哪个转义不支持"（原来只有一句笼统话 ✓）
        return val_null();
    }
    
    ObjArray* result = arr_new(8);
    if (!result) {
        re_free_all();
        return val_null();
    }

    // ★ result 自己也要 root：循环里每次 `native_struct_new` / `str_copy` 都会 gc_alloc
    //   ⇒ 它是还没交出去的中间对象（同 dirs.walk 的说明）。
    Value result_val = val_obj((Object*)result);
    gc_push_root(&result_val);
    
    const char* pos = str->chars;
    while (*pos) {
        const char* start = NULL;
        const char* end = NULL;
        find_match(pattern, str->chars, pos, ngroup, &ctx, &start, &end);
        
        if (!start) break;
        
        // 创建 `RegexMatch{ start, end, text }`（字段顺序 = REGEXMATCH_STRUCT_SPEC 的声明顺序）
        ObjStruct* m = native_struct_new("RegexMatch");
        if (!m) break;
        Value m_val = val_obj((Object*)m);
        gc_push_root(&m_val);   // 填字段期间它还没进 result ⇒ 必须自己护住

        native_struct_set(m, "start", val_int((int)(start - str->chars)));   // 0-based
        native_struct_set(m, "end",   val_int((int)(end - str->chars)));     // 0-based，不含
        native_struct_set(m, "text",  val_obj((Object*)str_copy(start, (int)(end - start))));

        arr_push_custom(result, m_val);
        gc_pop_root();   // m_val
        
        if (end == start) {
            pos++; // 避免空匹配无限循环
        } else {
            pos = end;
        }
    }
    
    gc_pop_root();   // result_val
    re_free_all();
    return val_obj((Object*)result);
}

// 4. 提取匹配的子串
static Value regex_extract(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));

    ReCtx ctx;                                  // 捕获状态（栈上，几 KB ✓）
    int ngroup = 0;
    ReNode* pattern = re_parse(pattern_str->chars, &ngroup);
    
    if (!pattern) {
        re_free_all();
        re_report_invalid(pattern_str->chars);   // 具体到"哪个转义不支持"（原来只有一句笼统话 ✓）
        return val_null();
    }
    
    const char* start = NULL;
    const char* end = NULL;
    find_match(pattern, str->chars, str->chars, ngroup, &ctx, &start, &end);
    
    re_free_all();
    
    if (start) {
        int match_len = (int)(end - start);
        ObjString* result = str_copy(start, match_len);
        return val_obj((Object*)result);
    }
    return val_null();
}

// 5. 提取所有匹配的子串
static Value regex_extract_all(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));

    ReCtx ctx;                                  // 捕获状态（栈上，几 KB ✓）
    int ngroup = 0;
    ReNode* pattern = re_parse(pattern_str->chars, &ngroup);
    
    if (!pattern) {
        re_free_all();
        re_report_invalid(pattern_str->chars);   // 具体到"哪个转义不支持"（原来只有一句笼统话 ✓）
        return val_null();
    }
    
    ObjArray* result = arr_new(8);
    if (!result) {
        re_free_all();
        return val_null();
    }
    
    const char* pos = str->chars;
    while (*pos) {
        const char* start = NULL;
        const char* end = NULL;
        find_match(pattern, str->chars, pos, ngroup, &ctx, &start, &end);
        
        if (!start) break;
        
        int match_len = (int)(end - start);
        ObjString* matched_str = str_copy(start, match_len);
        arr_push_custom(result, val_obj((Object*)matched_str));
        
        if (end == start) {
            pos++;
        } else {
            pos = end;
        }
    }
    
    re_free_all();
    return val_obj((Object*)result);
}

// 6. 替换第一个匹配
static Value regex_replace(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    ObjString* replacement = (ObjString*)val_as_obj(args[2]);
    // ★ 反向引用（`$1`）自 2026-10-01 起**真支持**（按本次匹配的捕获展开，见 re_expand_replacement ✓）
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));

    ReCtx ctx;                                  // 捕获状态（栈上，几 KB ✓）
    int ngroup = 0;
    ReNode* pattern = re_parse(pattern_str->chars, &ngroup);
    
    if (!pattern) {
        re_free_all();
        re_report_invalid(pattern_str->chars);   // 具体到"哪个转义不支持"（原来只有一句笼统话 ✓）
        return val_null();
    }
    
    const char* start = NULL;
    const char* end = NULL;
    find_match(pattern, str->chars, str->chars, ngroup, &ctx, &start, &end);
    
    re_free_all();
    
    if (!start) {
        return val_obj((Object*)str);  // 未找到，返回原字符串
    }
    
    // 计算新字符串长度（替换串里的 `$1` 要**按本次匹配的捕获展开**后再算 ✓）
    int before_len = (int)(start - str->chars);
    int match_len = (int)(end - start);
    int after_len = str->len - before_len - match_len;
    int repl_len = re_expand_replacement(replacement->chars, str, &ctx, NULL);
    int new_len = before_len + repl_len + after_len;

    char* result = (char*)malloc(new_len + 1);
    if (!result) {
        native_throw_error("内存分配失败");
        return val_null();
    }

    memcpy(result, str->chars, before_len);
    re_expand_replacement(replacement->chars, str, &ctx, result + before_len);   // 展开后填进去 ✓
    memcpy(result + before_len + repl_len, end, after_len);
    result[new_len] = '\0';
    
    ObjString* result_str = str_new(result, new_len);
    free(result);
    
    if (!result_str) return val_null();
    return val_obj((Object*)result_str);
}

// 7. 替换所有匹配
static Value regex_replace_all(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    ObjString* replacement = (ObjString*)val_as_obj(args[2]);
    // ★ 反向引用（`$1`）自 2026-10-01 起**真支持**（按本次匹配的捕获展开，见 re_expand_replacement ✓）
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));

    ReCtx ctx;                                  // 捕获状态（栈上，几 KB ✓）
    int ngroup = 0;
    ReNode* pattern = re_parse(pattern_str->chars, &ngroup);
    
    if (!pattern) {
        re_free_all();
        re_report_invalid(pattern_str->chars);   // 具体到"哪个转义不支持"（原来只有一句笼统话 ✓）
        return val_null();
    }
    
    // 计算结果长度
    //   ⚠ 替换串里的 `$1` 每次都要**按那一次匹配的捕获**展开 ⇒ 长度不能按 `replacement->len` 估 ✗
    const char* pos = str->chars;
    int match_count = 0;
    int total_match_len = 0;
    int total_repl_len = 0;

    while (*pos) {
        const char* start = NULL;
        const char* end = NULL;
        find_match(pattern, str->chars, pos, ngroup, &ctx, &start, &end);
        if (!start) break;
        match_count++;
        total_match_len += (int)(end - start);
        total_repl_len += re_expand_replacement(replacement->chars, str, &ctx, NULL);
        if (end == start) pos++;
        else pos = end;
    }

    if (match_count == 0) {
        re_free_all();
        return val_obj((Object*)str);
    }

    int new_len = str->len - total_match_len + total_repl_len;
    char* result = (char*)malloc(new_len + 1);
    if (!result) {
        re_free_all();
        native_throw_error("内存分配失败");
        return val_null();
    }
    
    // 构建结果字符串
    pos = str->chars;
    char* dst = result;
    
    while (*pos) {
        const char* start = NULL;
        const char* end = NULL;
        find_match(pattern, str->chars, pos, ngroup, &ctx, &start, &end);
        if (!start) break;

        // 复制匹配前的内容
        int before = (int)(start - pos);
        memcpy(dst, pos, before);
        dst += before;
        
        // 复制替换内容（同样按**本次**捕获展开 `$1` ✓）
        dst += re_expand_replacement(replacement->chars, str, &ctx, dst);
        
        pos = end;
        if (end == start) {
            *dst++ = *pos++;
        }
    }
    
    // 复制剩余内容
    strcpy(dst, pos);
    
    re_free_all();
    
    ObjString* result_str = str_new(result, new_len);
    free(result);
    
    if (!result_str) return val_null();
    return val_obj((Object*)result_str);
}

// 8. 分割字符串
static Value regex_split(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    // 限制分割次数（可选）
    int limit = -1;
    if (argc >= 3) {
        limit = val_as_int(args[2]);
    }
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));

    ReCtx ctx;                                  // 捕获状态（栈上，几 KB ✓）
    int ngroup = 0;
    ReNode* pattern = re_parse(pattern_str->chars, &ngroup);
    
    if (!pattern) {
        re_free_all();
        re_report_invalid(pattern_str->chars);   // 具体到"哪个转义不支持"（原来只有一句笼统话 ✓）
        return val_null();
    }
    
    ObjArray* result = arr_new(8);
    if (!result) {
        re_free_all();
        return val_null();
    }
    
    const char* pos = str->chars;
    const char* last_pos = pos;
    int count = 0;
    
    while (*pos && (limit < 0 || count < limit - 1)) {
        const char* start = NULL;
        const char* end = NULL;
        find_match(pattern, str->chars, pos, ngroup, &ctx, &start, &end);
        if (!start) break;
        
        int part_len = (int)(start - last_pos);
        ObjString* part = str_copy(last_pos, part_len);
        arr_push_custom(result, val_obj((Object*)part));
        
        pos = end;
        last_pos = pos;
        count++;
    }
    
    // 添加最后一部分
    if (*last_pos || result->count == 0) {
        ObjString* part = str_copy(last_pos, strlen(last_pos));
        arr_push_custom(result, val_obj((Object*)part));
    }
    
    re_free_all();
    return val_obj((Object*)result);
}

// 9. groups(str, pattern) —— 返回 `[整体匹配, 组1, 组2, ...]`
//   ★ 2026-10-01 起**真的含捕获组**（此前只回 1 个元素，名不副实 —— 匹配器那时不记录组 ✓）
//   未参与本次匹配的组（如 `(a)|(b)` 命中 "b" 时的组 1）⇒ 给**空串**（数组元素类型是 string ✓）
static Value regex_groups(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);

    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));

    ReCtx ctx;                                  // 捕获状态（栈上，几 KB ✓）
    int ngroup = 0;
    ReNode* pattern = re_parse(pattern_str->chars, &ngroup);

    if (!pattern) {
        re_free_all();
        re_report_invalid(pattern_str->chars);
        return val_null();
    }

    const char* start = NULL;
    const char* end = NULL;
    find_match(pattern, str->chars, str->chars, ngroup, &ctx, &start, &end);

    re_free_all();     // 节点池可以放；捕获是"相对 str 的整数偏移"⇒ 不受影响 ✓

    if (!start) {
        return val_null();
    }

    ObjArray* result = arr_new(ngroup + 2);
    if (!result) return val_null();
    Value result_val = val_obj((Object*)result);
    gc_push_root(&result_val);                  // arr 还没交出去 ⇒ 得 root（同 find_all ✓）

    // 第 0 个元素：整体匹配 ✓
    arr_push_custom(result, val_obj((Object*)str_copy(start, (int)(end - start))));
    // 第 1..ngroup 个：各捕获组（未参与 ⇒ 空串）
    for (int gi = 1; gi <= ngroup; gi++) {
        if (ctx.gstart[gi] >= 0 && ctx.gend[gi] >= ctx.gstart[gi]) {
            arr_push_custom(result, val_obj((Object*)str_copy(str->chars + ctx.gstart[gi],
                                                              ctx.gend[gi] - ctx.gstart[gi])));
        } else {
            arr_push_custom(result, val_obj((Object*)str_copy("", 0)));
        }
    }

    gc_pop_root();
    return result_val;
}

// 10. 转义正则特殊字符
static Value regex_escape(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    
    // 正则特殊字符：. ^ $ * + ? { } [ ] \ | ( )
    const char* special = ".^$*+?{}[]\\|()";
    
    // 计算转义后的长度
    int new_len = 0;
    for (int i = 0; i < str->len; i++) {
        if (strchr(special, str->chars[i])) {
            new_len += 2;
        } else {
            new_len += 1;
        }
    }
    
    char* result = (char*)malloc(new_len + 1);
    if (!result) {
        native_throw_error("内存分配失败");
        return val_null();
    }
    
    int j = 0;
    for (int i = 0; i < str->len; i++) {
        if (strchr(special, str->chars[i])) {
            result[j++] = '\\';
        }
        result[j++] = str->chars[i];
    }
    result[j] = '\0';
    
    ObjString* result_str = str_new(result, j);
    free(result);
    
    if (!result_str) return val_null();
    return val_obj((Object*)result_str);
}

// ==================== 初始化 ====================

void regexs_init_module(void) {
    // 注册正则模块方法
    TypeKind str_params[] = {TYPE_STRING, TYPE_STRING};
    TypeKind str3_params[] = {TYPE_STRING, TYPE_STRING, TYPE_STRING};

    native_register_module_method_spec("regexs", "match", regex_match, 2, -1, -1, &NATIVE_T_BOOL, str_params);
    native_register_module_method_spec("regexs", "find", regex_find, 2, -1, -1, &NATIVE_T_INT, str_params);
    native_register_module_method_spec("regexs", "extract", regex_extract, 2, -1, -1, &NATIVE_T_STRING, str_params);
    native_register_module_method_spec("regexs", "replace", regex_replace, 3, -1, -1, &NATIVE_T_STRING, str3_params);
    native_register_module_method_spec("regexs", "split", regex_split, -1, 2, 3, &NATIVE_T_ARR_STRING, str_params);
    // split(str, pattern[, limit])：前两位必须是 string（v3.2.7 显式声明可变参数的前缀类型 ——
    //   此前 `arity == -1` ⇒ param_types 被整份忽略，`regexs.split(1, 2)` 编译期不报错 ✗）
    native_set_method_vararg_params("regexs", "split", 2, str_params, TYPE_ANY);
    native_register_module_method_spec("regexs", "groups", regex_groups, 2, -1, -1, &NATIVE_T_ARR_STRING, str_params);

    // 返回数组的方法
    // find_all：返回 `Array[RegexMatch]`（**字段类型编译期已知**，v3.2.7）——
    //   原来是 `Array[Dict]`（裸 Dict）⇒ `m["text"]` 是 any、取值要手动收窄；
    //   改结构体后 `m.text` 直接是 string、`m.start` 直接是 int ✓
    native_register_struct_spec(&REGEXMATCH_STRUCT_SPEC);
    native_register_module_method_spec("regexs", "find_all", regex_find_all, 2, -1, -1, &S_REGEXMATCH_ARR_SPEC, str_params);
    native_register_module_method_spec("regexs", "extract_all", regex_extract_all, 2, -1, -1, &NATIVE_T_ARR_STRING, str_params);
    native_register_module_method_spec("regexs", "replace_all", regex_replace_all, 3, -1, -1, &NATIVE_T_STRING, str3_params);

    // 单参数方法
    TypeKind single_str_params[] = {TYPE_STRING};
    native_register_module_method_spec("regexs", "escape", regex_escape, 1, -1, -1, &NATIVE_T_STRING, single_str_params);
}
