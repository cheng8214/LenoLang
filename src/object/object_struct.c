#include "../include/lenolang.h"
#include "../include/native.h"
#include "../include/platform_thread.h"
#include "../include/method_table.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// 结构体定义表（线程局部）
// ============================================================================

#define MAX_STRUCT_DEFS 256

static THREAD_LOCAL ObjStructDef* struct_def_table[MAX_STRUCT_DEFS];
static THREAD_LOCAL int struct_def_count = 0;

// ----------------------------------------------------------------------------
// 名字 → 定义 的桶索引（S2/性能，2026-09-16）：把 *_def_find* 从 O(定义数) 降到 O(1)
// ----------------------------------------------------------------------------
// 为什么需要（实测，不是推断）：JIT 把 OP_STRUCT_INIT 实现为 **callout**
// （jit/jit_callout.c 的 jit_callout_struct_init），那条路径**没有内联缓存** ⇒ 每次 `new`
// 都真查一次定义表。探针（200 个 struct 声明、循环 300 万次 `new T199()`）：
//   1 个定义 215ms → 200 个定义 6250ms（**29×**，扫描成本吃掉全部时间）。
// 链按**注册顺序**串（新条目接到桶尾）⇒ "同名取先注册者"这条既有语义天然保持：
// 桶内第一个就是过去线性扫描时先遇到的那个。
// 覆盖替换（struct_def_replace）**不需要**动索引 —— 它只在同名分支发生（调用方保证），
// 槽位号与名字都不变，桶里那条记录指的仍是同一个名字。
// 桶数取 2 × MAX_STRUCT_DEFS ⇒ 负载因子 ≤ 0.5，桶内链平均 1~2 条。
#define STRUCT_DEF_NAME_BUCKETS 512

// 表很小时**线性扫描更快**：走桶要先算哈希、再两级依赖加载（桶 → 槽位 → 名字）。
// 实测（-O2，3M 次 new 的 JIT callout 路径，四路交错 A/B）：
//   基线（纯线性）                     208.7ms
//   保留索引维护 + 查找还原成基线        211~216ms   ← 索引维护本身不要钱
//   查找经辅助函数（小表特判在函数里）   235.4ms     ← 多一次调用 + 重复取下标，+8ns/次分配
// 所以小表分支必须**就地**写在各查找函数里（内容 = 改动前那几行原样），
// 而不是塞进辅助函数里特判。阈值两侧语义**完全相同**（都返回注册最早的同名者）。
#define SD_LINEAR_SCAN_MAX 4

// 编码：0 = 空；i + 1 = 表下标 i（静态存储零初始化即"空"，无需显式初始化）
static THREAD_LOCAL int sd_bucket_head[STRUCT_DEF_NAME_BUCKETS];
static THREAD_LOCAL int sd_bucket_tail[STRUCT_DEF_NAME_BUCKETS];
static THREAD_LOCAL int sd_chain_next[MAX_STRUCT_DEFS];

static uint32_t sd_name_hash(const char* s) {
    uint32_t h = 2166136261u;   // FNV-1a
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h;
}

// 把已就位的槽位登记进桶（只在 append 路径调用；末尾追加 ⇒ 桶内自然按注册顺序）
static void sd_index_append(int idx) {
    const char* name = struct_def_table[idx]->name;
    if (!name) return;
    uint32_t b = sd_name_hash(name) & (STRUCT_DEF_NAME_BUCKETS - 1);
    int code = idx + 1;
    if (sd_bucket_head[b] == 0) {
        sd_bucket_head[b] = code;
    } else {
        sd_chain_next[sd_bucket_tail[b] - 1] = code;
    }
    sd_bucket_tail[b] = code;
}

// 桶内**同名**的第一个槽位（= 注册最早的那个）；没有返回 -1
// （桶按哈希取，可能混入哈希碰撞的别的名字，故这里仍要比名字）
//
// ⚠ 这两个是**纯哈希桶**走法：只给"大表"与注册路径用。小表不要在**这里**加特判 ——
// 实测那样做等于每次查找多一次（不可内联的）调用 + 一次重复下标，1 个定义时 +8ns/次分配
// （3M 次 new：基线 208.7ms → 经辅助函数 235.4ms），而"就地线性扫描"与基线无差
// （保留索引维护、只把查找还原成基线 ⇒ 211~216ms）。所以小表分支写在各个查找函数里，
// 内容是改动前那几行**原样**。
static int sd_hash_first(const char* name) {
    if (!name) return -1;
    uint32_t b = sd_name_hash(name) & (STRUCT_DEF_NAME_BUCKETS - 1);
    for (int i = sd_bucket_head[b] - 1; i >= 0; i = sd_chain_next[i] - 1) {
        const char* n = struct_def_table[i]->name;
        if (n && strcmp(n, name) == 0) return i;
    }
    return -1;
}

// 从 i 往后找**同名**的下一个槽位（同样按注册顺序）；没有返回 -1。
// 调用前 i 必须是 sd_hash_first/sd_hash_next_same 返回过的合法下标。
static int sd_hash_next_same(int i, const char* name) {
    for (i = sd_chain_next[i] - 1; i >= 0; i = sd_chain_next[i] - 1) {
        const char* n = struct_def_table[i]->name;
        if (n && strcmp(n, name) == 0) return i;
    }
    return -1;
}

// ============================================================================
// 结构体定义操作
// ============================================================================

ObjStructDef* struct_def_new(const char* name, int field_count, int method_count) {
    ObjStructDef* def = (ObjStructDef*)gc_alloc(sizeof(ObjStructDef), OBJ_STRUCT_DEF);
    def->name = strdup(name);
    def->owner = NULL;   // 声明来源：由 OP_DEFINE_STRUCT 从 module frame 补上（见 S2）
    def->field_count = field_count;
    def->fields = (StructFieldInfo*)calloc(field_count, sizeof(StructFieldInfo));
    def->method_count = method_count;
    def->methods = (StructMethodInfo*)calloc(method_count, sizeof(StructMethodInfo));
    def->impl_names = NULL;
    def->impl_count = 0;
    def->type_param_count = 0;
    def->type_param_names = NULL;
    def->type_param_constraints = NULL;
    def->has_ctor = 0;
    def->ctor_index = -1;
    def->has_dtor = 0;
    def->dtor_index = -1;
    def->const_names = NULL;
    def->const_values = NULL;
    def->const_count = 0;

    // 追踪 fields 和 methods 数组的内存
    gc_track_memory((Object*)def, 0,
        (size_t)field_count * sizeof(StructFieldInfo) +
        (size_t)method_count * sizeof(StructMethodInfo));

    return def;
}

void struct_def_set_field(ObjStructDef* def, int index, const char* name, TypeKind type,
                          const char* struct_type_name, Value default_value, int has_default, TypeKind element_type, int nullable) {
    if (index < 0 || index >= def->field_count) return;

    def->fields[index].name = strdup(name);
    def->fields[index].type = type;
    def->fields[index].struct_type_name = struct_type_name ? strdup(struct_type_name) : NULL;
    def->fields[index].default_value = default_value;
    def->fields[index].has_default = has_default;
    def->fields[index].element_type = element_type;
    def->fields[index].nullable = nullable;
}

// ============================================================================
// 跨模块同名类型的冲突判定（四张定义表共用）
// ----------------------------------------------------------------------------
// 背景（docs/待办_单一事实来源与重复实现收敛.md 的 S2，已实测静默错值）：
// struct/cstruct/face/enum 四张表都是**全局、只按名字**索引，同名就是"后注册者覆盖先注册者"。
// 而字段索引 / enum 成员值是在**编译期**按各自模块的定义定死的（`OP_GET_FIELD` 的直接操作数
// 就是一个字段序号字节）。于是 A、B 两模块各定义同名类型时：编译期两边各有自己的符号表、
// 互不冲突（所以编译器一声不吭），运行期表里只剩后注册的那份 ⇒ A 模块生成的"第 0 个字段"
// 读到 B 定义的第 0 个字段 —— 名字是 A 的、槽位是 B 的，静默错值。
//
// 判定基于**声明来源**（def->owner，由 OP_DEFINE_* 从 module frame 取）：
//   - 同一来源 ⇒ 放行（同模块重注册、子线程按指针重注册都是正常路径）；
//   - 来源未知（NULL：编译期早期注册、反序列化产生的定义、入口程序自身的类型）⇒ 放行，避免误报；
//   - 两个不同来源且**形状不一致** ⇒ 冲突：报错并让调用方**保留先注册的那份**（不再覆盖）；
//   - 两个不同来源但**形状一致** ⇒ 放行：字段序号/方法名/成员值都对得上，覆盖是空操作，
//     报错会误伤（语料里确有同名同形的写法，见 use_mod_a/use_mod_c 的 Point）。
// ============================================================================

static const char* def_owner_label(ObjModule* m) {
    if (!m) return "?";
    if (m->source_path) return m->source_path;
    return m->name ? m->name : "?";
}

// 字符串允许 NULL 的相等比较（四张表的形状比对共用，见 leno_value.h 的声明）
int def_str_eq(const char* a, const char* b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

// 两个 struct 定义的"形状"是否一致（跨模块同名时判断覆盖是否无害）
//
// ⚠ "形状一致"**不等于**"行为一致"：方法体 / 构造函数体 / impl 实现都不在形状里
// （它们的 `func` 各自指向各自模块的字节码）。实测（2026-09-16）：
//   lib/a.leno 与 lib/b.leno 各定义 `Point { int x  func tag(): int { return 1 | 2 } }`
//   —— 字段、方法**名**、参数个数全同，只有方法体不同 ⇒ 被判"覆盖无害" ⇒ 表里只剩后注册的
//   那份 ⇒ `new a.Point().tag()` 跑的是 b 的方法体，**静默返回 2（期望 1）**，一条错误都没有。
// 因此这里收紧为：
//   - 任一方带方法 / ctor / dtor / impl ⇒ 无法证明行为等价 ⇒ 判为**不一致**（响亮报错）；
//   - 关联常量除名字外还要比**值**（值会随定义序列化、运行期可见，只比名字不够）。
// 只有"纯数据、且常量值也相同"的同名定义才算一致（此时覆盖确为空操作）。
static int struct_def_same_shape(ObjStructDef* a, ObjStructDef* b) {
    if (a->field_count != b->field_count) return 0;
    if (a->method_count != b->method_count) return 0;
    if (a->impl_count != b->impl_count) return 0;
    if (a->const_count != b->const_count) return 0;
    if (a->type_param_count != b->type_param_count) return 0;
    if (a->has_ctor != b->has_ctor || a->ctor_index != b->ctor_index) return 0;
    if (a->has_dtor != b->has_dtor || a->dtor_index != b->dtor_index) return 0;

    // 行为承载成员：形状比不出行为是否等价 ⇒ 一律按不一致处理（见函数头注释）
    if (a->method_count > 0 || b->method_count > 0) return 0;
    if (a->has_ctor || b->has_ctor || a->has_dtor || b->has_dtor) return 0;
    if (a->impl_count > 0 || b->impl_count > 0) return 0;

    for (int i = 0; i < a->field_count; i++) {
        StructFieldInfo* fa = &a->fields[i];
        StructFieldInfo* fb = &b->fields[i];
        if (fa->type != fb->type) return 0;
        if (fa->element_type != fb->element_type) return 0;
        if (fa->nullable != fb->nullable) return 0;
        if (fa->has_default != fb->has_default) return 0;
        // 默认值**本身**也要比（S2/2a++，2026-09-16 实测）：`{int x = 1}` 与 `{int x = 3}`
        // 只差默认值时，旧口径判"同形" ⇒ 覆盖 ⇒ 两份同名定义的实例都拿到同一个默认值
        // （实测 A=3 / B=3，期望 1 / 3，**零诊断**）。比原始位是保守做法：值不同即判不一致 ⇒
        // 走冲突报错（响亮），而不是静默换值；对象类默认值（字符串/数组）指针不同也判不一致，
        // 宁可报错也不赌"它们相等"。
        if (fa->has_default && fa->default_value != fb->default_value) return 0;
        if (!def_str_eq(fa->name, fb->name)) return 0;
        if (!def_str_eq(fa->struct_type_name, fb->struct_type_name)) return 0;
    }
    for (int i = 0; i < a->const_count; i++) {
        if (!def_str_eq(a->const_names[i], b->const_names[i])) return 0;
        if (a->const_values[i] != b->const_values[i]) return 0;   // 值也参与比对
    }
    return 1;
}

// 两个 enum 定义的形状是否一致（成员名 + 成员值）
static int enum_def_same_shape(ObjEnumDef* a, ObjEnumDef* b) {
    if (a->member_count != b->member_count) return 0;
    for (int i = 0; i < a->member_count; i++) {
        if (a->members[i].value != b->members[i].value) return 0;
        if (!def_str_eq(a->members[i].name, b->members[i].name)) return 0;
    }
    return 1;
}

int def_owner_conflict(const char* kind, const char* name,
                       ObjModule* old_owner, ObjModule* new_owner, int same_shape) {
    if (!old_owner || !new_owner || old_owner == new_owner) return 0;
    if (same_shape) return 0;   // 形状一致 ⇒ 覆盖是空操作，放行

    char msg[BUFFER_XLARGE];
    snprintf(msg, sizeof(msg),
             "跨模块同名%s '%s'：%s 与 %s 各定义了一份且无法证明等价（字段/成员不完全相同，"
             "或带方法体/构造析构/impl 等本判定比不出来的成员），而四张类型表全局只按名字索引、"
             "运行期只能保留一份（保留先注册的定义）。字段索引、方法体、成员值在编译期都按各自"
             "模块的定义确定，混用会静默读到同序号的错误字段、或跑到另一份的方法体 —— 请给其中一个改名。",
             kind, name, def_owner_label(old_owner), def_owner_label(new_owner));
    error_add_at(ERR_RUNTIME, 0, 0, msg);
    return 1;
}

// 注册结构体定义
// 注册（含同名覆盖）时递增代数计数器，供 VM 内联缓存校验 def 是否被重定义
static uint32_t struct_def_gen = 0;

uint32_t struct_def_generation(void) {
    return struct_def_gen;
}

// 覆盖注册表中第 i 个条目：把旧定义的资源指针置 NULL，防止 gc_free_all 时 double-free
// （旧定义对象仍由 GC 管理，gc_free_all 会调用 free_object_resources）
static void struct_def_replace(int i, ObjStructDef* def) {
    ObjStructDef* old_def = struct_def_table[i];
    old_def->name = NULL;
    old_def->fields = NULL;
    old_def->field_count = 0;
    old_def->methods = NULL;
    old_def->method_count = 0;
    old_def->impl_names = NULL;
    old_def->impl_count = 0;
    old_def->const_names = NULL;
    old_def->const_values = NULL;
    old_def->const_count = 0;

    // 索引无需维护：调用方保证 def 与旧条目**同名**（只有同名分支才会走到覆盖），
    // 槽位号与名字都不变 ⇒ 桶里那条记录仍然指向这个同名的槽位。
    struct_def_table[i] = def;
    struct_def_gen++;
}

void struct_def_register(ObjStructDef* def) {
    if (struct_def_count >= MAX_STRUCT_DEFS) {
        error_add_at(ERR_RUNTIME, 0, 0, "结构体定义数量超过上限");
        return;
    }

    // 1) 同一来源的同名条目：覆盖（同模块重注册、子线程按指针重注册、模块重新加载）
    //    只走"同名"的桶内链（S2/性能）⇒ 不再遍历整张表
    for (int i = sd_hash_first(def->name); i >= 0; i = sd_hash_next_same(i, def->name)) {
        ObjStructDef* old = struct_def_table[i];
        if (old->owner != def->owner) continue;
        if (old == def) return;   // 同一个对象再注册一次：空操作
        struct_def_replace(i, def);
        return;
    }

    // 2) 跨模块同名：判定冲突（冲突**仍然报错**），但两份都留在表里（S2/2b）——
    //    带模块前缀的查找（struct_def_find_qualified）要能各自取回自己那份；
    //    裸名查找按"先注册者"返回，给尚未迁移的调用点（cast / face / JIT / 原生桥接）用，
    //    而那些点一旦拿错，当前的响亮报错就是兜底。故这里**不能**直接丢弃新定义。
    for (int i = sd_hash_first(def->name); i >= 0; i = sd_hash_next_same(i, def->name)) {
        ObjStructDef* old = struct_def_table[i];
        if (def_owner_conflict("struct", def->name, old->owner, def->owner,
                               struct_def_same_shape(old, def))) {
            break;   // 已报错 ⇒ 让两份共存：跳出循环走末尾 append
        }
        struct_def_replace(i, def);
        return;
    }

    struct_def_table[struct_def_count] = def;
    sd_index_append(struct_def_count);   // 只有在 append 时才需要登记索引
    struct_def_count++;
    struct_def_gen++;
}

// 更新所有结构体方法函数的 module 指针
void struct_def_update_method_modules(ObjModule* old_module, ObjModule* new_module) {
    if (!old_module || !new_module) return;
    for (int i = 0; i < struct_def_count; i++) {
        ObjStructDef* def = struct_def_table[i];
        if (def->methods) {
            for (int j = 0; j < def->method_count; j++) {
                if (def->methods[j].func && def->methods[j].func->module == old_module) {
                    def->methods[j].func->module = new_module;
                }
                if (def->methods[j].closure && def->methods[j].closure->function &&
                    def->methods[j].closure->function->module == old_module) {
                    def->methods[j].closure->function->module = new_module;
                }
            }
        }
    }
}

// 查找结构体定义（小表就地线性扫描；大表走名字桶索引 —— 见 SD_LINEAR_SCAN_MAX）
ObjStructDef* struct_def_find(const char* name) {
    if (struct_def_count <= SD_LINEAR_SCAN_MAX) {
        for (int i = 0; i < struct_def_count; i++) {
            if (strcmp(struct_def_table[i]->name, name) == 0) {
                return struct_def_table[i];
            }
        }
        return NULL;
    }
    int i = sd_hash_first(name);   // 桶内第一个同名 = 注册最早的那个（与线性扫描同解）
    return i >= 0 ? struct_def_table[i] : NULL;
}

// 在指定模块**声明**的定义里按裸名查找（S2/2b-2）：这是最精确的一级 —— 编译期把
// "别名 → 该导入模块的槽位"写进字节码，运行期取出模块对象后就能唯一确定该取哪一份，
// 完全不依赖"别名是否等于模块名"。
ObjStructDef* struct_def_find_in_module(ObjModule* owner, const char* name) {
    if (!owner || !name) return NULL;
    if (struct_def_count <= SD_LINEAR_SCAN_MAX) {   // 小表：就地线性扫描（与改动前同）
        for (int i = 0; i < struct_def_count; i++) {
            ObjStructDef* d = struct_def_table[i];
            if (d->owner == owner && d->name && strcmp(d->name, name) == 0) {
                return d;
            }
        }
        return NULL;
    }
    // 大表：只走"同名"的桶内链 ⇒ "取哪个模块声明的那份"由 O(定义数) 降为 O(1)
    for (int i = sd_hash_first(name); i >= 0; i = sd_hash_next_same(i, name)) {
        ObjStructDef* d = struct_def_table[i];
        if (d->owner == owner) {
            return d;
        }
    }
    return NULL;
}

// 按"可能带模块前缀的名字"查找定义（S2/2b）。
//
// 背景：同名类型可以来自不同模块（A、B 各声明 Point），裸名查找只能返回"先注册者"，
// 于是跨模块引用会静默拿到**别人**的定义 —— 字段索引/方法体都是按各自模块编译期定死的。
// 编译期其实完整知道声明来源（`new a.Point()` 里的前缀 `a` 就是 `import ... as a` 的别名，
// 与模块的 `name` 一致），只是过去在 codegen 里被剥掉了；这里让运行期按前缀精查。
//
// 名字不带前缀时（模块内 `new Point()`、旧字节码、原生桥接、按名反射）行为与
// struct_def_find 完全一致 ⇒ 向后兼容。
ObjStructDef* struct_def_find_qualified(const char* qualified_name) {
    if (!qualified_name) return NULL;

    const char* dot = strchr(qualified_name, '.');
    if (dot && dot != qualified_name && dot[1]) {
        size_t mod_len = (size_t)(dot - qualified_name);
        const char* bare = dot + 1;
        if (struct_def_count <= SD_LINEAR_SCAN_MAX) {   // 小表：就地线性扫描（与改动前同）
            for (int i = 0; i < struct_def_count; i++) {
                ObjStructDef* d = struct_def_table[i];
                if (!d->name || strcmp(d->name, bare) != 0) continue;
                if (!d->owner || !d->owner->name) continue;
                if (strlen(d->owner->name) == mod_len &&
                    strncmp(d->owner->name, qualified_name, mod_len) == 0) {
                    return d;
                }
            }
        } else {                                        // 大表：只扫同名的桶内链
            for (int i = sd_hash_first(bare); i >= 0; i = sd_hash_next_same(i, bare)) {
                ObjStructDef* d = struct_def_table[i];
                if (!d->owner || !d->owner->name) continue;
                if (strlen(d->owner->name) == mod_len &&
                    strncmp(d->owner->name, qualified_name, mod_len) == 0) {
                    return d;
                }
            }
        }
    }
    // 回退：按**裸名部分**查找（不是整串！）——精查没命中时（前缀对不上模块名、
    // 定义无 owner 等）必须退回旧行为：拿 "Point" 去裸名查找，而不是拿 "a.Point"。
    // 这一步是"精查是增量、绝不比旧行为更差"的保证。
    return struct_def_find(dot ? dot + 1 : qualified_name);
}

// 返回当前线程结构体定义表的数量（供主线程抓取快照传给子线程）
int struct_def_get_count(void) {
    return struct_def_count;
}

// 返回当前线程结构体定义表中第 i 个定义（供主线程抓取快照）
ObjStructDef* struct_def_get(int i) {
    if (i < 0 || i >= struct_def_count) return NULL;
    return struct_def_table[i];
}

// 将主线程定义的结构体导入当前（子）线程的定义表。
// struct 定义是只读类型元数据，与 cstruct 定义一样可跨线程共享。
// 注意：def 可能分配在主线程 GC 堆上，子线程只读不写；
// 主线程通过模块对象持续持有 def，保证其存活，因此子线程引用安全。
void struct_def_import_from_thread(ObjStructDef** defs, int count) {
    for (int i = 0; i < count; i++) {
        if (defs[i]) {
            struct_def_register(defs[i]);
        }
    }
}

// ============================================================================
// 结构体实例操作
// ============================================================================

ObjStruct* struct_instance_new(ObjStructDef* def);

// 最大嵌套实例化深度，防止循环引用导致无限递归
#define STRUCT_INSTANCE_MAX_DEPTH 16

ObjStruct* struct_instance_new_depth(ObjStructDef* def, int depth) {
    if (depth > STRUCT_INSTANCE_MAX_DEPTH) {
        return NULL;
    }

    // 头 + 字段数组一次分配：小对象（如 2 字段 Hit）从 2 次系统分配降为 1 次，
    // 尾部内联数组被 obj->size 覆盖（GC 记账/清零自动正确），无需 gc_track_memory
    // （对象版光线追踪实测：struct 分配慢于 Python 的主因之一）
    size_t total_size = sizeof(ObjStruct) + (size_t)def->field_count * sizeof(Value);
    ObjStruct* obj = (ObjStruct*)gc_alloc(total_size, OBJ_STRUCT);
    /* 分配失败时 gc_alloc 已经 error_add_at；这里必须返回 NULL 而不是继续解引用
     * （JIT 的 OP_STRUCT_INIT callout 与解释器构造都会检查 NULL 走各自的失败路径）。 */
    if (!obj) return NULL;
    obj->def = def;
    obj->field_values = (Value*)((char*)obj + sizeof(ObjStruct));
    obj->declared_face = NULL;
    obj->generic_type_args = NULL;
    obj->generic_type_arg_count = 0;
    obj->fields_inline = 1;

    // 使用默认值初始化字段
    for (int i = 0; i < def->field_count; i++) {
        if (def->fields[i].has_default) {
            // 深拷贝引用类型的默认值（Array、Dict、Struct），避免所有实例共享同一对象
            Value dv = def->fields[i].default_value;
            if (val_is_obj(dv)) {
                Object* dobj = val_as_obj(dv);
                if (dobj->type == OBJ_ARRAY || dobj->type == OBJ_DICT || dobj->type == OBJ_STRUCT) {
                    obj->field_values[i] = value_copy(dv);
                    continue;
                }
            }
            obj->field_values[i] = dv;
        } else if (def->fields[i].nullable) {
            // nullable 字段默认为 null（不递归分配/不自动创建空容器）
            obj->field_values[i] = val_null();
        } else if (def->fields[i].type == TYPE_STRUCT && def->fields[i].struct_type_name) {
            // 嵌套 struct 类型：递归创建实例
            ObjStructDef* nested_def = struct_def_find(def->fields[i].struct_type_name);
            if (nested_def) {
                ObjStruct* nested_obj = struct_instance_new_depth(nested_def, depth + 1);
                if (nested_obj) {
                    obj->field_values[i] = val_obj((Object*)nested_obj);
                } else {
                    obj->field_values[i] = val_null();
                }
            } else {
                obj->field_values[i] = val_null();
            }
        } else if (def->fields[i].type == TYPE_ARRAY) {
            // Array 字段：自动初始化为空数组
            ObjArray* arr = arr_new(0);
            obj->field_values[i] = arr ? val_obj((Object*)arr) : val_null();
        } else if (def->fields[i].type == TYPE_DICT) {
            // Dict 字段：自动初始化为空字典
            ObjDict* dict = dict_new(0);
            obj->field_values[i] = dict ? val_obj((Object*)dict) : val_null();
        } else {
            obj->field_values[i] = val_null();
        }
    }

    return obj;
}

ObjStruct* struct_instance_new(ObjStructDef* def) {
    return struct_instance_new_depth(def, 0);
}

int struct_get_field_index(ObjStructDef* def, const char* name) {
    for (int i = 0; i < def->field_count; i++) {
        if (strcmp(def->fields[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

Value struct_get_field(ObjStruct* obj, int index) {
    if (index < 0 || index >= obj->def->field_count) {
        return val_null();
    }
    return obj->field_values[index];
}

// struct_set_field 已移到 leno_value.h 内联（字段写入热路径，见该处说明）。

// ============================================================================
// 结构体方法表（运行时 - 使用通用 MethodTable）
// ============================================================================

#define STRUCT_METHOD_TABLE_INITIAL_CAPACITY 16

static THREAD_LOCAL MethodTable structMethodTable = {NULL, 0, 0};

// 注册结构体方法（带参数类型）
void struct_register_method_with_params(const char* name, ObjNative* method, int arity, int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    method_table_register_with_params(&structMethodTable, "struct", name, method, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

// 查找结构体方法的元信息（用于编译期类型检查）
StructMethodEntry struct_find_method_meta(const char* name) {
    return method_table_find_meta(&structMethodTable, name);
}

// 查找结构体方法
ObjNative* struct_find_method(const char* name) {
    return method_table_find(&structMethodTable, name);
}

// 标记 struct 方法表中的所有方法（供 GC 使用）
void struct_mark_methods(void) {
    method_table_mark(&structMethodTable);
}

void struct_init_methods(void) {
    method_table_init_methods(&structMethodTable, STRUCT_METHOD_TABLE_INITIAL_CAPACITY);
}

// 标记所有结构体定义（供 GC 使用）
void struct_def_mark_all(void) {
    extern void gc_mark_object(Object* obj);
    for (int i = 0; i < struct_def_count; i++) {
        gc_mark_object((Object*)struct_def_table[i]);
    }
}

// ============================================================================
// enum 定义表（运行时全局）
// ============================================================================

#define MAX_ENUM_DEFS 256

static THREAD_LOCAL ObjEnumDef* enum_def_table[MAX_ENUM_DEFS];
static THREAD_LOCAL int enum_def_count = 0;

// ============================================================================
// enum 定义操作
// ============================================================================

ObjEnumDef* enum_def_new(const char* name, int member_count) {
    ObjEnumDef* def = (ObjEnumDef*)gc_alloc(sizeof(ObjEnumDef), OBJ_ENUM_DEF);
    def->name = strdup(name);
    def->owner = NULL;   // 声明来源：由 OP_DEFINE_ENUM 从 module frame 补上（见 S2）
    def->member_count = member_count;
    def->members = (EnumMemberInfo*)calloc(member_count, sizeof(EnumMemberInfo));

    // 追踪 members 数组的内存
    gc_track_memory((Object*)def, 0, (size_t)member_count * sizeof(EnumMemberInfo));

    return def;
}

void enum_def_set_member(ObjEnumDef* def, int index, const char* name, int64_t value) {
    if (index < 0 || index >= def->member_count) return;

    def->members[index].name = strdup(name);
    def->members[index].value = value;
}

int enum_def_lookup_member_value(ObjEnumDef* def, const char* name, int64_t* out_value) {
    if (!def || !name) return 0;
    for (int i = 0; i < def->member_count; i++) {
        if (strcmp(def->members[i].name, name) == 0) {
            if (out_value) *out_value = def->members[i].value;
            return 1;
        }
    }
    return 0;
}

// 注册 enum 定义
void enum_def_register(ObjEnumDef* def) {
    if (enum_def_count >= MAX_ENUM_DEFS) {
        error_add_at(ERR_RUNTIME, 0, 0, "enum 定义数量超过上限");
        return;
    }

    // 检查是否已存在同名 enum
    for (int i = 0; i < enum_def_count; i++) {
        if (strcmp(enum_def_table[i]->name, def->name) == 0) {
            // 跨模块同名：拦住，别覆盖（否则就是 S2 那个静默错值）
            if (def_owner_conflict("enum", def->name,
                                   enum_def_table[i]->owner, def->owner,
                                   enum_def_same_shape(enum_def_table[i], def))) {
                return;
            }
            // 覆盖旧定义：将旧定义的资源指针置 NULL，防止 gc_free_all 时 double-free
            ObjEnumDef* old_def = enum_def_table[i];
            old_def->name = NULL;
            old_def->members = NULL;
            old_def->member_count = 0;

            enum_def_table[i] = def;
            return;
        }
    }

    enum_def_table[enum_def_count++] = def;
}

// 查找 enum 定义
ObjEnumDef* enum_def_find(const char* name) {
    for (int i = 0; i < enum_def_count; i++) {
        if (strcmp(enum_def_table[i]->name, name) == 0) {
            return enum_def_table[i];
        }
    }
    return NULL;
}
