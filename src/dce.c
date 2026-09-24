// ============================================================================
// 入口产物（.lenb）的方法级死代码消除 —— 编译期引用图 + 可达性
//   规则、fail-safe、开关见 include/leno_dce.h 顶部的总说明。
//   本文件只依赖 leno_value.h（core 构建里与 serialize.c 同源，VM 侧也链接），
//   不许调用 codegen —— 那边只是**喂**引用给这里。
// ============================================================================

#include "include/leno_dce.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 引用种类
// ---------------------------------------------------------------------------
typedef enum {
    DREF_SLOT = 0,    // 读本编译单元的槽位（slot 字段；函数值 / 直呼都算）
    DREF_NAME = 1,    // 按名字引用（a = 名字；只命中**顶层函数**）
    DREF_METHOD = 2,  // (类型, 方法)：a = 类型名（NULL ⇒ 名字通配），b = 方法名
    DREF_TYPE = 3,    // 类型被引用（a = 类型名）—— 名字通配时用它过滤
    DREF_FUNC = 4,    // 直接引用一个函数对象（fn 字段）：匿名闭包 / 局部函数 ——
                      //   它们没有槽位、也没有名字，只有"创建点"这一次引用
} DceRefKind;

typedef struct {
    DceRefKind kind;
    int slot;
    int unit;
    char* a;
    char* b;
    ObjFunction* fn;   // DREF_FUNC
} DceRef;

typedef struct {
    ObjFunction* func;
    int unit;
    char* type_name;      // 归一化后的方法所属类型名；非方法 = NULL
    int is_ctor, is_dtor;   // 构造/析构：隐式调用，无条件保活（见 run_finalize 的说明）
    int live;
    int parent;           // 外层函数（局部函数跟随父函数）；-1 = 顶层
    DceRef* refs;         // 本函数体内发出的引用
    int ref_count, ref_cap;
} DceFunc;

typedef struct {
    ObjModule* mod;
    int* slots;           // 槽位
    ObjFunction** fns;    // 槽位对应的函数（def 点）
    int def_count, def_cap;
    DceRef* roots;        // 模块级/顶层代码里的引用（根 ⇒ 一定可达）
    int root_count, root_cap;
    int saved_func;       // 进入本单元前"当前函数"（模块可以嵌套在某个函数体内被编译）
    int parent_unit;      // 进入本单元前"当前单元"（退出时恢复；单元索引**只增不复用**）
} DceUnit;

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
static int s_enabled = -1;        // -1 = 还没读环境变量
static int s_collect = 0;         // 是否收集引用图（只在 -c / -p 这类"产出分发产物"的编译里开）
                                  //   关掉时所有 hook 立刻返回：.leno 直跑（断言套件 373 条）
                                  //   不该为一份用不上的图付代价
static int s_active = 0;          // 是否正在写入口 .lenb
static int s_verbose = 0;
static int s_wild_safe = 0;       // 名字通配不再按"类型被引用"过滤（最保守档，对比用）
static int s_cached_module = 0;   // 有模块来自 .lenomc ⇒ 图不完整
static int s_finalized = 0;

static DceUnit* s_units = NULL;
static int s_unit_count = 0, s_unit_cap = 0;
static int s_cur_unit = 0;

static DceFunc* s_funcs = NULL;
static int s_func_count = 0, s_func_cap = 0;
static int s_cur_func = -1;       // 当前正在生成的函数索引；-1 = 顶层/模块级

static char* s_method_owner = NULL;  // gen_struct_def 设置的方法所属类型名

// 指针 → 函数索引 的哈希表（开放寻址；序列化时按函数对象查"活不活"，必须是 O(1)）
static ObjFunction** s_map_keys = NULL;
static int* s_map_vals = NULL;
static int s_map_cap = 0, s_map_count = 0;

// --- 可达性阶段的状态（dce_reset 里要清理 ⇒ 声明必须在这之前）---
// 名字/类型集合用「有序数组 + 二分」（去重 + 边标边查）；规模是百量级，比建哈希表简单
typedef struct {
    char** items;
    int count, cap;
} StrSet;

static StrSet s_used_names;         // DREF_NAME 的名字（只命中顶层函数）
static StrSet s_used_method_names;  // 名字通配的方法名
static StrSet s_used_types;         // 被引用的类型名
static int* s_wl = NULL;            // 待处理队列（工作列表）
static int s_wl_count = 0, s_wl_cap = 0;
static int s_live_count = 0;
static int* s_by_name = NULL;   // 顶层函数：按名字
static int* s_by_tn = NULL;     // 方法：按 (类型, 名)
static int* s_by_mname = NULL;  // 方法：按 名
static int s_top_count = 0, s_meth_count = 0;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static void* xrealloc(void* p, size_t n) {
    void* q = realloc(p, n);
    if (!q) {
        fprintf(stderr, "[DCE] 内存不足\n");
        exit(1);
    }
    return q;
}

// 类型名归一化：去掉模块限定前缀（取最后一个 '.' 之后）与泛型实参后缀（'[' 起）。
// 为什么要归一化：同一个类型在不同位置的名字口径不一样 ——
//   `new mod.Renderer()` 的 struct_name 可能是 "mod.Renderer"、泛型实例是 "Box[T]"，
//   而**定义**侧的方法表只有声明名（"Renderer" / "Box"）。不归一化就会出现
//   "(类型,名) 精确引用"匹配不上任何方法 ⇒ 活方法被当死代码剪掉（静默返回 null）。
// 归一化的代价是不同模块的同名类型会并成一组 ⇒ 只会**多留**（安全方向）。
static void norm_type(const char* in, char* out, size_t n) {
    out[0] = '\0';
    if (!in || !in[0]) return;
    const char* p = in;
    const char* dot = NULL;
    for (const char* q = p; *q; q++) {
        if (*q == '.') dot = q;
    }
    if (dot) p = dot + 1;
    size_t i = 0;
    while (p[i] && p[i] != '[' && i + 1 < n) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

static int strset_has(StrSet* s, const char* v) {
    int lo = 0, hi = s->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(s->items[mid], v);
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int strset_add(StrSet* s, const char* v) {
    if (!v || !v[0]) return 0;
    int lo = 0, hi = s->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(s->items[mid], v);
        if (c == 0) return 0;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    if (s->count >= s->cap) {
        s->cap = s->cap == 0 ? 16 : s->cap * 2;
        s->items = (char**)xrealloc(s->items, sizeof(char*) * (size_t)s->cap);
    }
    memmove(&s->items[lo + 1], &s->items[lo], sizeof(char*) * (size_t)(s->count - lo));
    s->items[lo] = strdup(v);
    s->count++;
    return 1;
}

// 清空（保留已分配的 items 缓冲，下一次编译直接复用）
static void strset_clear(StrSet* s) {
    for (int i = 0; i < s->count; i++) free(s->items[i]);
    s->count = 0;
}

// ---------------------------------------------------------------------------
// 指针哈希表
// ---------------------------------------------------------------------------
static size_t ptr_hash(ObjFunction* f) {
    size_t x = (size_t)f;
    x ^= x >> 17;
    x *= (size_t)0x9E3779B97F4A7C15ULL;
    x ^= x >> 13;
    return x;
}

static void map_rehash(int new_cap) {
    ObjFunction** ok = s_map_keys;
    int* ov = s_map_vals;
    int oc = s_map_cap;
    s_map_keys = (ObjFunction**)calloc((size_t)new_cap, sizeof(ObjFunction*));
    s_map_vals = (int*)calloc((size_t)new_cap, sizeof(int));
    if (!s_map_keys || !s_map_vals) {
        fprintf(stderr, "[DCE] 内存不足\n");
        exit(1);
    }
    s_map_cap = new_cap;
    s_map_count = 0;
    for (int i = 0; i < oc; i++) {
        if (ok[i]) {
            size_t m = ptr_hash(ok[i]) & (size_t)(s_map_cap - 1);
            while (s_map_keys[m]) m = (m + 1) & (size_t)(s_map_cap - 1);
            s_map_keys[m] = ok[i];
            s_map_vals[m] = ov[i];
            s_map_count++;
        }
    }
    free(ok);
    free(ov);
}

static int map_find(ObjFunction* f) {
    if (!f || s_map_cap == 0) return -1;
    size_t m = ptr_hash(f) & (size_t)(s_map_cap - 1);
    while (s_map_keys[m]) {
        if (s_map_keys[m] == f) return s_map_vals[m];
        m = (m + 1) & (size_t)(s_map_cap - 1);
    }
    return -1;
}

static void map_put(ObjFunction* f, int idx) {
    if (s_map_cap == 0 || (s_map_count + 1) * 4 >= s_map_cap * 3) {
        map_rehash(s_map_cap == 0 ? 256 : s_map_cap * 2);
    }
    size_t m = ptr_hash(f) & (size_t)(s_map_cap - 1);
    while (s_map_keys[m]) {
        if (s_map_keys[m] == f) {
            s_map_vals[m] = idx;
            return;
        }
        m = (m + 1) & (size_t)(s_map_cap - 1);
    }
    s_map_keys[m] = f;
    s_map_vals[m] = idx;
    s_map_count++;
}

// ---------------------------------------------------------------------------
// 注册 / 引用记录
// ---------------------------------------------------------------------------

static DceUnit* cur_unit(void) {
    if (s_unit_count == 0) return NULL;
    return &s_units[s_cur_unit];
}

static void unit_add_def(DceUnit* u, int slot, ObjFunction* func) {
    for (int i = 0; i < u->def_count; i++) {
        if (u->slots[i] == slot) {
            u->fns[i] = func;   // 同槽位重复定义：以后者为准
            return;
        }
    }
    if (u->def_count >= u->def_cap) {
        u->def_cap = u->def_cap == 0 ? 16 : u->def_cap * 2;
        u->slots = (int*)xrealloc(u->slots, sizeof(int) * (size_t)u->def_cap);
        u->fns = (ObjFunction**)xrealloc(u->fns, sizeof(ObjFunction*) * (size_t)u->def_cap);
    }
    u->slots[u->def_count] = slot;
    u->fns[u->def_count] = func;
    u->def_count++;
}

static ObjFunction* unit_find_def(DceUnit* u, int slot) {
    if (!u) return NULL;
    for (int i = 0; i < u->def_count; i++) {
        if (u->slots[i] == slot) return u->fns[i];
    }
    return NULL;
}

static void ref_push(DceRef** arr, int* count, int* cap, DceRef ref) {
    if (*count >= *cap) {
        *cap = *cap == 0 ? 8 : *cap * 2;
        *arr = (DceRef*)xrealloc(*arr, sizeof(DceRef) * (size_t)*cap);
    }
    (*arr)[*count] = ref;
    (*count)++;
}

static void note_ref(DceRef ref) {
    if (!s_collect) return;
    ref.unit = s_cur_unit;
    if (s_cur_func >= 0) {
        DceFunc* f = &s_funcs[s_cur_func];
        ref_push(&f->refs, &f->ref_count, &f->ref_cap, ref);
    } else {
        DceUnit* u = cur_unit();
        if (u) ref_push(&u->roots, &u->root_count, &u->root_cap, ref);
    }
}

int dce_enabled(void) {
    if (s_enabled < 0) {
        s_enabled = getenv("LENO_NO_DCE") ? 0 : 1;
        const char* vb = getenv("LENO_DCE_VERBOSE");
        s_verbose = vb ? atoi(vb) : 0;
        if (vb && s_verbose == 0) s_verbose = 1;   // 只写 "1"/其它非数字也算开启
        s_wild_safe = getenv("LENO_DCE_WILD_SAFE") ? 1 : 0;
    }
    return s_enabled;
}

void dce_set_active(int on) {
    s_active = on;
}

int dce_active(void) {
    return s_active;
}

void dce_note_module_cached(void) {
    if (dce_enabled()) s_cached_module = 1;
}

void dce_reset(void) {
    for (int i = 0; i < s_func_count; i++) {
        free(s_funcs[i].type_name);
        for (int j = 0; j < s_funcs[i].ref_count; j++) {
            free(s_funcs[i].refs[j].a);
            free(s_funcs[i].refs[j].b);
        }
        free(s_funcs[i].refs);
    }
    free(s_funcs);
    s_funcs = NULL;
    s_func_count = s_func_cap = 0;
    s_cur_func = -1;

    for (int i = 0; i < s_unit_count; i++) {
        free(s_units[i].slots);
        free(s_units[i].fns);
        for (int j = 0; j < s_units[i].root_count; j++) {
            free(s_units[i].roots[j].a);
            free(s_units[i].roots[j].b);
        }
        free(s_units[i].roots);
    }
    free(s_units);
    s_units = NULL;
    s_unit_count = s_unit_cap = 0;

    free(s_map_keys);
    free(s_map_vals);
    s_map_keys = NULL;
    s_map_vals = NULL;
    s_map_cap = s_map_count = 0;

    free(s_method_owner);
    s_method_owner = NULL;

    strset_clear(&s_used_names);
    strset_clear(&s_used_method_names);
    strset_clear(&s_used_types);

    free(s_by_name);
    free(s_by_tn);
    free(s_by_mname);
    s_by_name = s_by_tn = s_by_mname = NULL;
    s_top_count = s_meth_count = 0;
    free(s_wl);
    s_wl = NULL;
    s_wl_count = s_wl_cap = 0;
    s_live_count = 0;

    s_cached_module = 0;
    s_finalized = 0;
    s_active = 0;
    s_collect = dce_enabled();   // 只有显式 reset（= 即将产出分发产物）才开收集

    // 单元 0 = 入口程序（没有对应 ObjModule）
    s_unit_cap = 8;
    s_units = (DceUnit*)calloc((size_t)s_unit_cap, sizeof(DceUnit));
    s_unit_count = 1;
    s_cur_unit = 0;
}

void dce_enter_unit(ObjModule* module) {
    if (!s_collect) return;
    if (s_unit_count == 0) dce_reset();
    if (s_unit_count >= s_unit_cap) {
        s_unit_cap *= 2;
        s_units = (DceUnit*)xrealloc(s_units, sizeof(DceUnit) * (size_t)s_unit_cap);
    }
    // ★ 单元索引**只增不复用**（数组槽位一旦用过就留着）：引用里存的是单元索引，
    //   复用会让前一个模块的"槽位 ← 函数"表残留下来 —— 实测后果是后一个模块的同号槽位
    //   被前一个模块的函数占住（`single` 被 `hammer` 顶掉 ⇒ single 解析不到 ⇒ 被误剪）。
    memset(&s_units[s_unit_count], 0, sizeof(DceUnit));
    s_units[s_unit_count].mod = module;
    s_units[s_unit_count].saved_func = s_cur_func;
    s_units[s_unit_count].parent_unit = s_cur_unit;
    s_cur_unit = s_unit_count;
    s_unit_count++;
    s_cur_func = -1;    // 模块级代码没有"当前函数"（否则模块函数会被挂成某个函数的局部函数）
}

void dce_exit_unit(void) {
    if (!s_collect) return;
    // 单元 0 是入口程序，永远在栈底（弹不掉）
    if (s_cur_unit <= 0 || s_unit_count == 0) {
        s_cur_unit = 0;
        s_cur_func = -1;
        return;
    }
    s_cur_func = s_units[s_cur_unit].saved_func;      // 恢复被模块打断的那个函数
    s_cur_unit = s_units[s_cur_unit].parent_unit;     // 回到进入本单元前的单元
}

void dce_set_method_owner(const char* struct_name) {
    if (!s_collect) return;
    free(s_method_owner);
    if (struct_name && struct_name[0]) {
        char buf[256];
        norm_type(struct_name, buf, sizeof(buf));
        s_method_owner = strdup(buf);
    } else {
        s_method_owner = NULL;
    }
}

void dce_enter_func(ObjFunction* func, int is_ctor, int is_dtor) {
    if (!s_collect || !func) return;
    if (s_verbose >= 3) fprintf(stderr, "[DCE] enter_func name=%s unit=%d\n", func->name ? func->name : "?", s_cur_unit);
    int idx = map_find(func);
    if (idx < 0) {
        if (s_func_count >= s_func_cap) {
            s_func_cap = s_func_cap == 0 ? 256 : s_func_cap * 2;
            s_funcs = (DceFunc*)xrealloc(s_funcs, sizeof(DceFunc) * (size_t)s_func_cap);
            memset(&s_funcs[s_func_count], 0, sizeof(DceFunc) * (size_t)(s_func_cap - s_func_count));
        }
        idx = s_func_count++;
        s_funcs[idx].func = func;
        s_funcs[idx].parent = s_cur_func;
        s_funcs[idx].unit = s_cur_unit;
        map_put(func, idx);
    }
    s_funcs[idx].is_ctor = is_ctor;
    s_funcs[idx].is_dtor = is_dtor;
    if (s_method_owner && !s_funcs[idx].type_name) {
        s_funcs[idx].type_name = strdup(s_method_owner);
    }
    s_cur_func = idx;
}

void dce_exit_func(void) {
    if (!s_collect) return;
    s_cur_func = (s_cur_func >= 0) ? s_funcs[s_cur_func].parent : -1;
}

void dce_note_func_def(int slot, ObjFunction* func) {
    if (!s_collect || !func || slot < 0) return;
    if (s_verbose >= 3) fprintf(stderr, "[DCE] def slot=%d unit=%d name=%s\n", slot, s_cur_unit, func->name ? func->name : "?");
    DceUnit* u = cur_unit();
    if (!u) return;
    unit_add_def(u, slot, func);
}

void dce_note_slot_ref(int slot) {
    if (!s_collect || slot < 0) return;
    if (s_verbose >= 3) fprintf(stderr, "[DCE] slot_ref slot=%d unit=%d func=%d\n", slot, s_cur_unit, s_cur_func);
    DceRef r;
    memset(&r, 0, sizeof(r));
    r.kind = DREF_SLOT;
    r.slot = slot;
    note_ref(r);
}

void dce_note_name_ref(const char* name) {
    if (!s_collect || !name || !name[0]) return;
    DceRef r;
    memset(&r, 0, sizeof(r));
    r.kind = DREF_NAME;
    r.a = strdup(name);
    note_ref(r);
}

void dce_note_method_ref(const char* type_name, const char* method_name) {
    if (!s_collect || !method_name || !method_name[0]) return;
    DceRef r;
    memset(&r, 0, sizeof(r));
    r.kind = DREF_METHOD;
    if (type_name && type_name[0]) {
        char buf[256];
        norm_type(type_name, buf, sizeof(buf));
        r.a = strdup(buf);
    }
    r.b = strdup(method_name);
    note_ref(r);
}

void dce_note_func_value(ObjFunction* func) {
    if (!s_collect || !func) return;
    DceRef r;
    memset(&r, 0, sizeof(r));
    r.kind = DREF_FUNC;
    r.fn = func;
    note_ref(r);
}

void dce_note_type_ref(const char* type_name) {
    if (!s_collect || !type_name || !type_name[0]) return;
    char buf[256];
    norm_type(type_name, buf, sizeof(buf));
    if (!buf[0]) return;
    DceRef r;
    memset(&r, 0, sizeof(r));
    r.kind = DREF_TYPE;
    r.a = strdup(buf);
    note_ref(r);
}

// ---------------------------------------------------------------------------
// 可达性
// ---------------------------------------------------------------------------

// 集合 / 工作列表 / 排序索引 的声明见文件顶部的"全局状态"（dce_reset 要清理它们）

static void wl_push(int idx) {
    if (s_wl_count >= s_wl_cap) {
        s_wl_cap = s_wl_cap == 0 ? 256 : s_wl_cap * 2;
        s_wl = (int*)xrealloc(s_wl, sizeof(int) * (size_t)s_wl_cap);
    }
    s_wl[s_wl_count++] = idx;
}

static void mark_live(int idx) {
    if (idx < 0 || idx >= s_func_count) return;
    if (s_funcs[idx].live) return;
    s_funcs[idx].live = 1;
    s_live_count++;
    wl_push(idx);
}

static int cmp_by_name(const void* pa, const void* pb) {
    int a = *(const int*)pa, b = *(const int*)pb;
    int c = strcmp(s_funcs[a].func->name ? s_funcs[a].func->name : "",
                   s_funcs[b].func->name ? s_funcs[b].func->name : "");
    if (c) return c;
    return a - b;
}

static int cmp_by_tn(const void* pa, const void* pb) {
    int a = *(const int*)pa, b = *(const int*)pb;
    int c = strcmp(s_funcs[a].type_name, s_funcs[b].type_name);
    if (c) return c;
    c = strcmp(s_funcs[a].func->name, s_funcs[b].func->name);
    if (c) return c;
    return a - b;
}

static int cmp_by_mname(const void* pa, const void* pb) {
    int a = *(const int*)pa, b = *(const int*)pb;
    int c = strcmp(s_funcs[a].func->name, s_funcs[b].func->name);
    if (c) return c;
    return a - b;
}

// (类型, 名) 精确命中：返回命中个数（0 ⇒ 调用方退回"名字通配"）
static int resolve_exact(const char* type_name, const char* method_name) {
    int hits = 0;
    for (int i = 0; i < s_meth_count; i++) {
        int idx = s_by_tn[i];
        int c = strcmp(s_funcs[idx].type_name, type_name);
        if (c < 0) continue;
        if (c > 0) break;
        if (strcmp(s_funcs[idx].func->name ? s_funcs[idx].func->name : "", method_name) != 0) continue;
        mark_live(idx);
        hits++;
    }
    return hits;
}

static void resolve_used_names(void) {
    for (int i = 0; i < s_used_names.count; i++) {
        for (int k = 0; k < s_top_count; k++) {
            int idx = s_by_name[k];
            const char* n = s_funcs[idx].func->name;
            if (!n || strcmp(n, s_used_names.items[i]) != 0) continue;
            mark_live(idx);
        }
    }
}

// 名字通配：方法名被通配引用，且（默认）其所属类型被引用 ⇒ 活
static void resolve_wildcard_methods(void) {
    for (int i = 0; i < s_used_method_names.count; i++) {
        const char* mn = s_used_method_names.items[i];
        for (int k = 0; k < s_meth_count; k++) {
            int idx = s_by_mname[k];
            const char* n = s_funcs[idx].func->name;
            if (!n || strcmp(n, mn) != 0) continue;
            if (s_wild_safe || strset_has(&s_used_types, s_funcs[idx].type_name)) {
                mark_live(idx);
            }
        }
    }
}

static void apply_ref(const DceRef* r) {
    switch (r->kind) {
    case DREF_SLOT: {
        ObjFunction* f = unit_find_def(&s_units[r->unit], r->slot);
        if (f) {
            int idx = map_find(f);
            if (idx >= 0) mark_live(idx);
        }
        break;
    }
    case DREF_NAME:
        strset_add(&s_used_names, r->a);
        break;
    case DREF_METHOD:
        if (r->a) {
            strset_add(&s_used_types, r->a);
            if (resolve_exact(r->a, r->b) == 0) {
                // 精确口径没命中（类型名口径不一致 / 该名其实不是脚本方法）⇒ 退回名字通配
                strset_add(&s_used_method_names, r->b);
            }
        } else {
            strset_add(&s_used_method_names, r->b);
        }
        break;
    case DREF_TYPE:
        strset_add(&s_used_types, r->a);
        break;
    case DREF_FUNC: {
        int idx = map_find(r->fn);
        if (idx >= 0) mark_live(idx);
        // 局部函数的父链关系保证：它活则其内部引用的东西也活（drain 会处理它自己的 refs）
        break;
    }
    }
}

static void drain_worklist(void) {
    while (s_wl_count > 0) {
        int idx = s_wl[--s_wl_count];
        DceFunc* f = &s_funcs[idx];
        for (int i = 0; i < f->ref_count; i++) {
            apply_ref(&f->refs[i]);
        }
    }
}

static void build_indexes(void) {
    // 局部函数跟随外层父函数 ⇒ 索引里只放**顶层**（parent == -1）的实体
    s_by_name = (int*)xrealloc(NULL, sizeof(int) * (size_t)(s_func_count > 0 ? s_func_count : 1));
    s_by_tn = (int*)xrealloc(NULL, sizeof(int) * (size_t)(s_func_count > 0 ? s_func_count : 1));
    s_by_mname = (int*)xrealloc(NULL, sizeof(int) * (size_t)(s_func_count > 0 ? s_func_count : 1));
    s_top_count = 0;
    s_meth_count = 0;
    for (int i = 0; i < s_func_count; i++) {
        if (s_funcs[i].parent != -1) continue;      // 局部函数：跟父函数一起活
        if (s_funcs[i].type_name) {
            s_by_tn[s_meth_count] = i;
            s_by_mname[s_meth_count] = i;
            s_meth_count++;
        } else if (s_funcs[i].func->name) {
            s_by_name[s_top_count++] = i;
        }
    }
    if (s_top_count > 1) qsort(s_by_name, (size_t)s_top_count, sizeof(int), cmp_by_name);
    if (s_meth_count > 1) {
        qsort(s_by_tn, (size_t)s_meth_count, sizeof(int), cmp_by_tn);
        qsort(s_by_mname, (size_t)s_meth_count, sizeof(int), cmp_by_mname);
    }
}

static void run_finalize(void) {
    if (s_finalized) return;
    s_finalized = 1;
    build_indexes();

    if (s_cached_module) return;   // 图不完整 ⇒ 谁都不标"活"，但 dce_func_is_live 会一律放行

    // ③ ctor/dtor：VM 按 ctor_index/dtor_index **隐式**调用（`new T()` 走 OP_STRUCT_INIT，
    //   `OP_CLOSE`/return 走析构），静态**没有任何**方法名引用点 ⇒ 必须无条件全留。
    //   实测这一条不花体积：那两个产物里 ctor/dtor 本来就被别的规则覆盖（开/关同尺寸），
    //   删掉它却会在"类型只经隐式路径使用"的写法上静默剪掉构造器 ⇒ 保留。
    for (int i = 0; i < s_func_count; i++) {
        if (s_funcs[i].is_ctor || s_funcs[i].is_dtor) mark_live(i);
    }

    // 根：每个单元的模块级/顶层代码里的引用（这些代码一定会执行）
    for (int u = 0; u < s_unit_count; u++) {
        DceUnit* unit = &s_units[u];
        for (int i = 0; i < unit->root_count; i++) {
            apply_ref(&unit->roots[i]);
        }
    }

    // 不动点：函数引用是直接的（drain 即可），名字/类型是间接的，要等集合稳定
    for (;;) {
        drain_worklist();
        int before = s_live_count;
        resolve_used_names();
        resolve_wildcard_methods();
        drain_worklist();
        if (s_live_count == before) break;
    }

    // 局部函数跟随外层父函数（父活 ⇒ 子活，因为静态看不到"局部函数被调用"的引用点）
    for (;;) {
        int before = s_live_count;
        for (int i = 0; i < s_func_count; i++) {
            int p = s_funcs[i].parent;
            if (p >= 0 && s_funcs[p].live) mark_live(i);
        }
        drain_worklist();
        if (s_live_count == before) break;
    }

    if (s_verbose) {
        dce_report();
    }
}

int dce_func_is_live(ObjFunction* func) {
    if (!func) return 1;
    if (!dce_enabled() || !s_active) return 1;
    run_finalize();
    if (s_cached_module) return 1;    // ② 图不完整 ⇒ 一律保活
    int idx = map_find(func);
    if (idx < 0) return 1;            // ① 未登记 ⇒ 保活
    return s_funcs[idx].live;
}

void dce_report(void) {
    int live = 0;
    int meth = 0, top = 0, local = 0;
    for (int i = 0; i < s_func_count; i++) {
        if (s_funcs[i].live) live++;
        if (s_funcs[i].parent != -1) local++;
        else if (s_funcs[i].type_name) meth++;
        else top++;
    }
    fprintf(stderr, "[DCE] 函数 %d（顶层 %d / 方法 %d / 局部 %d）；可达 %d，可剪 %d%s\n",
            s_func_count, top, meth, local, live, s_func_count - live,
            s_cached_module ? "（有模块命中缓存 ⇒ 本次不剪）" : "");
    // LENO_DCE_VERBOSE=2：把"判定为可剪"的函数逐个列出来（排查"误剪"用；正常不输出）
    if (s_verbose >= 2) {
        for (int i = 0; i < s_func_count; i++) {
            if (s_funcs[i].live) continue;
            fprintf(stderr, "[DCE] cut: %s [%s] unit=%d parent=%d\n",
                    s_funcs[i].func->name ? s_funcs[i].func->name : "?",
                    s_funcs[i].type_name ? s_funcs[i].type_name : "-",
                    s_funcs[i].unit, s_funcs[i].parent);
        }
    }
}