# 注解系统 & `@optkeys` 设计草案

> **状态：草案（未实现）** · 2026-10-31
>
> **起因**：`docs/语言与框架问题_Aria实战_2026-10-31.md` 的 **L5** ——
> LenoSDL3 各控件用 `dochk.check(who, opts, _opts_keys())` 在**运行时**对账选项键，
> 传错键只 `print` 一行警告：
>
> - 没有文件:行号（`who` 只有控件名，看不出是哪个 `createHBox`）；
> - **打包后完全看不见**（单文件 exe 走 `-mwindows`，没有控制台，`print` 丢进虚空）；
> - 每个控件创建都要遍历一遍 opts，纯浪费。
>
> **本方案**：给语言加一套**注解**语法，让框架把"这个构造函数接受哪些键"声明成
> 编译器可消费的元数据，把检查**提前到编译期**。
>
> ⚠ 核心约束：**编译器不需要知道 LenoSDL3 的存在**。注解是语言特性，框架只是使用者。

---

## 1. 目标与非目标

### 目标

| # | 目标 |
|---|---|
| G1 | 传错选项键在 **`--check` / 编译期**就报出来，带文件:行号与"你是不是想要 X" |
| G2 | 打包后的应用同样受检（不依赖控制台） |
| G3 | 运行时零开销（`dochk.check` 可整套删掉） |
| G4 | 编译器与框架解耦：注解机制通用，`@optkeys` 只是第一个使用者 |

> **实现状态（2026-10-31）**：G1~G4 已落地。同模块与**跨模块**检查均已验证：
>
> ```
> _tmp.leno:7: [警告] createHBox 的 opts 不认识键 'tip'
>         提示: 你是不是想要 'clip'？
>         提示: 接受的键见 …\LenoSDL3\lib\SDL3.leno 里该函数的 @optkeys
> ```
>
> 全量套件 533 passed / 0 failed、**0 条误报**；Aria（5000+ 行、大量控件调用）同样零误报。
> 第 13 节记录了实现过程中踩到的三个坑（注解该加在哪一层、检查落点、缓存）。

### 非目标（一期不做）

- 注解参数引用 `const` 常量（只允许字面量）
- 自定义注解注册（注解表由编译器维护）
- 注解继承 / 组合 / 展开
- 把既有的 `@deprecated` 之类**一起**实现（但语法要为它们留位）

---

## 2. 语法设计

### 2.1 注解的基本形态

```
注解 := '@' 标识符 [ '(' 参数字面量 ')' ]
```

```leno
@deprecated                                  // 无参
export func old_api() { ... }

@deprecated("改用 createBox")                 // 一个字符串
export func old_api() { ... }

@optkeys(["w", "h", "pad_x", "spacing"])     // 字符串数组
export func createHBox(Dict opts): HBox { ... }

@optkeys("*")                                // 通配：接受任意键，不检查
export func createCustom(Dict opts): HBox { ... }
```

**参数只允许字面量**：`string` / `int` / `float` / `bool` / 由它们组成的数组。
不允许表达式、不允许 identifier、不允许跨行。

> 理由：注解是**编译期数据**，一旦允许表达式就要引入"注解求值"，失败语义、
> 循环依赖、求值顺序全得设计。一期把口子收死，报错文案写"注解参数必须是字面量"即可。

### 2.2 多个注解可叠加

```leno
@optkeys(["w", "h", "pad_x"])
@deprecated("改用 createBox")
export func createHBox(Dict opts): HBox { ... }
```

收集成有序列表挂在声明上。同名注解出现多次 ⇒ **参数并集**（见 2.4）。

### 2.3 `@optkeys` 挂在**函数**上，不是类型

```leno
// LenoSDL3/lib/sdl_layout.leno
@optkeys(["w", "h", "pad_x", "pad_y", "spacing", "bg_color", "bg_radius",
          "clip", "justify", "align", "visible"])
export func createHBox(Dict opts): HBox {
    // dochk.check("HBox", opts, _opts_keys())   ← 一期之后再删
    ...
}
```

**为什么不挂在 `struct HBox` 上**：

1. `HBox` 是 struct，但 opts **不是它的字段**，而是构造参数 —— 语义上属于构造函数；
2. 同一类型可能有多个构造函数（`createHBox` / `createHBoxFrom`），键表可以不同；
3. 挂在函数上，检查时不需要"从返回类型反查类型表"，直接看 callee 的注解即可（实现更简单）。

语义定义为：**"本函数的 Dict 参数接受这些键"**。函数有多个 Dict 参数时，一期只认**第一个**，并在文档里写明。

### 2.4 并集语义

```leno
@optkeys(["w", "h"])
export func createHBox(Dict opts): HBox { ... }

@optkeys(["justify"])            // 同一个函数上的第二个 optkeys ⇒ 并集
@optkeys(["w", "h"])
export func createHBox(Dict opts): HBox { ... }
```

为什么需要并集：框架里"基础布局键"和"主题相关键"可能由不同模块/不同作者追加，
合并比"后覆盖前"更符合直觉（也不会因为声明顺序不同产生不同结果）。

### 2.5 未知注解直接报错

编译器维护一张已知注解表（一期只有 `optkeys`）：

```
[错误] aria.leno:42: 不认识的注解 '@optkey'
  [提示] 你是不是想要 '@optkeys'？
```

比"静默忽略"好：注解拼错等于没写，而且**最难发现**（代码看着是对的）。

---

## 3. 词法与 AST

### 3.1 词法

新增一个 token：

```c
TOK_AT        // '@'
```

`@` 在 Leno 里当前未被占用（字符串插值 `$"..."`、可切片 `arr[a:b]`、可空 `T?`、
空合并 `??` 都不冲突），不会引入歧义。

### 3.2 AST

```c
// 注解节点（挂在任意声明上的链）
typedef struct {
    char*  name;          // "optkeys" / "deprecated"
    int    line;          // 注解所在行（诊断用）
    // 参数：一期只支持 0 或 1 个字面量
    int    arg_kind;      // 0=无参 / 1=string / 2=int / 3=float / 4=bool / 5=字符串数组
    char*  arg_str;
    double arg_num;
    int    arg_bool;
    char** arg_list;      // 字符串数组
    int    arg_list_len;
} AstAnno;

// 声明侧挂链（不新建声明节点，只在现有节点的 u.xxx 里加两个字段）
AstAnno* annos;           // 注解数组
int      anno_count;
```

挂在 `AST_FUNC_DECL` / `AST_STRUCT_DECL` / `AST_VAR_DECL` 上。

> 为什么不做一个 `AST_ANNOTATED { annos, decl }` 包装节点：那会让所有消费声明的
> 地方（semantic / codegen / 模块符号表）都要多拆一层。直接给声明加字段，改动面最小。

---

## 4. parser 落点

**主入口**：`src/parser/parser.c` 的模块级 `declaration()`（当前 switch 在 `:160` 起）。

在 **switch 之前** 插入注解收集：

```c
// 伪代码
static Ast* declaration(Parser* p) {
    AstAnno* annos = NULL;
    int anno_n = 0;

    // ① 先把所有 @xxx 收完
    while (p->lex.current.type == TOK_AT) {
        lexer_next(&p->lex);                       // 吃掉 '@'
        if (p->lex.current.type != TOK_IDENT) {
            error_add_at_fmt(ERR_SYNTAX, line, col, "'@' 后面必须跟注解名");
            return NULL;
        }
        AstAnno a = parse_anno_args(p);            // 名字 + 可选 (字面量)
        annos = realloc_add(annos, &anno_n, &a);
    }

    Ast* stmt = parse_decl_core(p);                // 原来的 switch

    // ② 挂到声明上（允许挂的声明类型有限）
    if (anno_n > 0) {
        if (!decl_can_be_annotated(stmt)) {
            error_add_at_fmt(ERR_SYNTAX, ...,
                "注解只能用在模块级声明（func / struct / var / const）之前");
        } else {
            attach_annos(stmt, annos, anno_n);
        }
    }
    return stmt;
}
```

**注意 `export` 的顺序**：现在的写法是 `export func ...`。注解要在最外层：

```leno
@optkeys([...])
export func createHBox(...) { }        // ✓ 注解在最前
export @optkeys([...]) func ...        // ✗ 不支持（不给自己挖坑）
```

因为 `parser_module.c:414` 的 `TOK_EXPORT` 分支会递归调回 `declaration()`，
所以只要注解收集在 `declaration()` 顶部，`@x export func ...` 天然可用 ✓

**参数解析**：

```c
static AstAnno parse_anno_args(Parser* p) {
    // 名字
    // 可选 '('
    //   TOK_STRING → 一个字符串
    //   TOK_LBRACKET → 字符串数组（只允许 string 元素）
    //   TOK_INT/FLOAT/TRUE/FALSE → 标量
    // ')'
    // 其它 token ⇒ "注解参数必须是字面量"
}
```

---

## 5. semantic 检查

### 5.1 收集

在遍历声明时把 `@optkeys` 记到函数符号上：

```c
// 全局（每编译单元一份）
typedef struct {
    const char* func_name;     // 全限定或模块内唯一名
    char** keys; int n;        // 已合并（并集）
    bool wildcard;             // '*' ⇒ 不检查
    int decl_line, decl_file;  // 诊断里指"去哪看"
} OptKeySet;
```

### 5.2 检查

在 `AST_CALL` 的处理里（`src/semantic/visitinc/visit_expr.inc`）：

```c
// ① callee 解析到具体函数符号，且它带 @optkeys
// ② 排除 wildcard
// ③ 找到"第一个 Dict 参数"对应的实参
// ④ 该实参必须是 AST_DICT_LITERAL（字面量）—— 否则放行
// ⑤ 逐个键：不在集合里 ⇒ 报
```

```c
for (每个键 k in dict_literal) {
    if (key_in_set(k)) continue;
    const char* hint = nearest_key(k, set.keys, set.n);   // 编辑距离 ≤2
    if (hint)
        error_add_at_fmt(ERR_*_UNKNOWN_OPT, call_line, col,
            "%s 的 opts 不认识键 '%s'\n"
            "  提示: 你是不是想要 '%s'？\n"
            "  提示: 接受的键见 %s:%d 的 @optkeys",
            func_name, k, hint, set.decl_file, set.decl_line);
    else
        error_add_at_fmt(... 同上，无"是不是想要"一行 ...);
}
```

**编辑距离工具**：`semantic_method_hint` 那套（`semantic_type_utils.c:1439` 附近）
已经有"相似名"的实现，可以直接复用/抽出通用版本。

### 5.3 为什么只查字面量

```leno
createHBox({w: 30, tip: "x"})     // ✓ 查
var o = {w: 30, tip: "x"}
createHBox(o)                      // ✗ 放行（静态不可知）
createHBox(buildOpts())            // ✗ 放行
```

这是 `threads.start` 那条教训的直接应用（见
`docs/语言与框架问题_Aria实战_2026-10-31.md` L3）：**宁漏勿误报**。
"有时查得到、有时查不到"如果还伴随误报，用户会直接关掉这个特性。

---

## 6. codegen

注解**不产生任何字节码**。`AST_*` 上的 `annos` 字段在 codegen 里被忽略即可。

> 一期不需要 `AST_ANNO` 这个独立节点（见 3.2），所以 codegen 甚至不用改。

---

## 7. 诊断文案

三种：

```
// ① 键写错（有相近的）
[错误] aria.leno:1090: createHBox 的 opts 不认识键 'tip'
  [提示] 你是不是想要 'clip'？（编辑距离 2）
  [提示] 接受的键见 LenoSDL3/lib/sdl_layout.leno:42 的 @optkeys

// ② 键写错（没有相近的）
[错误] aria.leno:1090: createHBox 的 opts 不认识键 'title_size'
  [提示] 接受的键见 LenoSDL3/lib/sdl_layout.leno:42 的 @optkeys

// ③ 注解名拼错
[错误] aria.leno:42: 不认识的注解 '@optkey'
  [提示] 你是不是想要 '@optkeys'？
```

第三行（"见 xxx:42"）是本方案相对现状最大的改进 —— 它把"去哪改"直接指出来，
而且**不需要 `_line()` 内建**，因为位置是注解的声明处，编译器本来就知道。

---

## 8. 迁移路径

| 阶段 | 行为 | 说明 |
|---|---|---|
| P1 | 编译**警告**，不中断 | 存量的错键不至于升级即编不过 |
| P2 | `--check --strict` 下是**错误** | CI 用 strict 拦住新增 |
| P3 | 默认错误 | 存量清干净后切换 |

在 P3 之后删掉 `LenoSDL3/lib/sdl_opts.leno` 的 `check()` 运行时路径，
`@optkeys` 成为唯一事实来源。

---

## 9. 对账用例（必须做）

风险最高的地方不是检查逻辑，而是**声明和实现不同步**：

```leno
export func createHBox(Dict opts): HBox {
    float w = opts.get("w", 0.0)          // ← 实现读了 "w"
    ...
}
@optkeys(["w", "h"])                      // ← 声明漏了 "h"？
```

一旦声明漏键，**正常代码编译不过** —— 比运行时静默更让人恼火。

**兜底用例**（放 `assert/`，CI 跑）：

```
扫描 LenoSDL3/lib/sdl_*.leno 里所有
    opts.get("X") / opts.has("X") / opts["X"]
与同文件的 @optkeys 声明比对：
    实现读到的键 ⊆ 声明集合      （漏声明 ⇒ 失败）
    声明集合 ⊆ 实现读到的键 + 白名单（多声明 ⇒ 警告，不算失败）
```

这条用例是整套机制能不能长期用的关键，一期就要带上。

---

## 10. 一期范围

### 做

- `TOK_AT` 词法
- `declaration()` 上的注解收集 + 3 种参数形态（无参 / 单字面量 / 字符串数组）
- 注解挂到 `AST_FUNC_DECL` 上
- 已知注解表（`optkeys`）+ 拼错提示
- `@optkeys` 检查：只查字面量 Dict、只查第一个 Dict 参数、warning 级
- 对账用例
- `LenoSDL3` 各 `createXxx` 补 `@optkeys`（约 20 个控件）

### 不做（留给后续）

- `@deprecated` / `@arity` / `@default` 等其它注解的**语义**
- 注解参数引用 const
- 对 struct 字段级注解
- 注解的反射（运行期读到注解）

---

## 11. 风险与取舍

| 风险 | 影响 | 对策 |
|---|---|---|
| 声明漏键导致误报 | 正常代码编不过 | 第 9 节对账用例 + P1 先只警告 |
| **只在实现层加注解** | 应用侧完全查不到（见 13.1） | 门面层必须同样加 |
| 检查放错语义阶段 | 一次都命中不了（见 13.2） | 挂在"参数个数检查"旁 |
| 符号表缓存丢键表 | 缓存命中时跨模块失效（见 13.3） | `.lenosymc` 升版本 + 写缓存前先填充 |
| 注解参数要表达式 | 一期不支持，用户觉得受限 | 报错文案明确写"注解参数必须是字面量"，后续再放开 |
| 挂错位置（挂到类型上） | 一个类型多构造时键表冲突 | 文档写明"挂函数"，并在 parser 里对挂在不支持的声明上报错 |
| 多个 Dict 参数 | 只查第一个可能漏 | 一期文档写明；后续可加 `@optkeys(arg = "opts", ...)` |
| 编译器与框架耦合 | —— | **不存在**：编译器只认注解名 `optkeys`，不认 `LenoSDL3` |

---

## 12. 实现踩坑记录（2026-10-31，都实测过）

### 13.1 注解必须加在**门面层**，不是实现层

LenoSDL3 是"门面 + 实现"两层：

```leno
// SDL3.leno（门面，应用 import 的是这个）
export func createHBox(Dict opts): HBox { return lay.createHBox(opts) }

// sdl_layout.leno（实现）
export func createHBox(Dict opts): HBox { HBox b = new HBox(); b.set(opts); return b }
```

把 `@optkeys` 加到**实现层**时，应用里的 `SDL3.createHBox({...})` 一个都不报——因为符号表里
它看到的是**门面那个函数**（无注解），门面转发给实现时传的是 `opts` 变量（非字面量 ⇒ 按规则放行）。

⇒ **门面与实现两处都要加**（实现层那份覆盖"模块内部互相调用"）。

### 13.2 检查落点：挂在"参数个数检查"旁边

先后试过三个位置，都收不到控件构造调用：

| 位置 | 结果 |
|---|---|
| `visit_expr.inc` 的 `case AST_CALL` 开头 | 调用是 `AST_MODULE_CALL`，不进这个分支 ✗ |
| `infer_expr_type` 的 `case AST_MODULE_CALL` | 该分支只处理 native 模块，用户模块的构造调用不来 ✗ |
| `visit_module.inc` 的 `case AST_MODULE_CALL` 开头 | 实测只收到模块**内部**的转发调用（`mod=lay fn=createHBox`）✗ |

**可行的位置**：`visit_expr.inc` 里 `func_def` 已解析出来、正要报"参数数量不足"的那一处——
那里跨模块也能拿到被调函数的 AST（否则报不出参数个数）⇒ 直接读 `func_def->annos`，
连符号表都不需要 ✓

### 13.3 符号表缓存会丢键表

`ModuleFuncSymbol.optkeys` 是 v39 新增字段。注意两点：

- `add_func` 里**必须显式置空** `func->optkeys`（`table->funcs` 是 realloc 扩容的，
  新槽位是垃圾值，不置空会读野指针 ✗ —— 与 `param_types` 那几条注释同一个坑）
- 写缓存的顺序是「读源码 → **AST 填充** → 写缓存」（`sym_table_entry.inc:351/376`）⇒
  optkeys 能进缓存 ✓；升版本号（v38 → v39）让旧缓存失效重扫 ✓

实测：连续两次 `--check`（第二次命中缓存）都能正常报出，说明缓存往返是通的。

### 13.4 ✅ 运行时白名单已整套删除（2026-10-31 收尾）

补齐注解后，`_opts_keys()` 那套已删（提交 `8f6d523`，41 个文件、-326 行）：

| 删掉 | 保留 |
|---|---|
| 41 处 `dochk.check(..., _opts_keys()/_set_keys()/label_keys()/alert_keys())` | `add(c, opts)` 的 `dochk.check(..., _add_keys())` |
| 39 个白名单函数定义 | `popmenu` / `tray` / `window` 三处（内部控件，未加注解） |
| | `sdl_opts.leno` 本体（`add` 与 `warn_once` 还在用） |

**为什么要删**——不只是"重复"，而是**已经实际漂移过**：

> `sdl_gridlayout.leno` 的 `set()` 读了 `min_col_width`，但它的 `_opts_keys()` 白名单里**没有**
> ⇒ 影视海报墙传 `min_col_width` 一直被运行时误报"不认识的选项"（全量扫 `--check` 才发现）。

这正是"两份数据"的必然后果。删完只剩注解一份，改选项时只改一个地方。

**踩坑**：`label_keys` / `alert_keys` 原本是 `pri func XXX()` 形式，批量删除时正则只匹配 `func` 那段，
留下孤立的 `pri` —— 它把后面的 `func set(Dict opts)` 变成了私有方法，`createLabel` 在 struct 外面
调 `lbl.set(opts)` 直接编译失败（`'Label.set' 是 pri 私有方法`）。修法：删掉那行孤立的 `pri`。

**验证**：533 passed / 0 failed；47 个 GUI 主程序 `--check` 零误报；Aria 实跑界面正常、运行时零 `[ui]` 告警。

### 13.5 补注解时的两种来源

- **有 `_opts_keys()` 的**（36 个）：逐字抄白名单 —— 但注意上面 gridlayout 那种漂移，
  遇到"这个键不认识"的误报时，先查实现里是不是真的读了它；
- **没有白名单的**（`Label` / `Alert` / `VirtualGrid`）：扫描实现里的 `opts.get("…")` / `opts.has("…")`
  反推键表，再补通用几何键（`w`/`h`）。这类最容易漏，**必须靠全量应用验证**兜底。

### 13.6 覆盖现状

已加注解 **43 个** `createXxx`（含 `HBox/VBox/AnchorBox` 门面 + 实现两处）。
未加的内部控件：`createPopMenu` / `createTray` / `createDragHint` / `createTitleBar` / `createWindow`
（应用不直接调用，仍走各自保留的运行时检查）。
其余控件可按同一模式增量补（键表直接抄 `_opts_keys()`）。

## 13. 验收清单

- [ ] `@optkeys(["w"])` + `createHBox({w: 1, tip: "x"})` ⇒ 编译期报 `tip`，带"见 xxx:行"
- [ ] `@optkeys("*")` ⇒ 不报
- [ ] `createHBox(o)`（非字面量）⇒ 不报
- [ ] `@optkey`（拼错注解）⇒ 报"不认识的注解 + 是不是想要 @optkeys"
- [ ] `@optkeys` 挂到 `var` 上 ⇒ 报"注解只能用在……"
- [ ] `--check` 全量套件零误报（存量 532 个用例）
- [ ] 对账用例通过（声明 ⊇ 实现读到的键）
- [ ] 删掉 `dochk.check` 后 Aria 全功能回归正常

---

## 附：与现有方案的关系

| 方案 | 现状 | 本方案 |
|---|---|---|
| 检查时机 | 运行到那一行 | **编译期** |
| 打包后可见 | ✗（无控制台） | ✓ |
| 运行时开销 | 每次建控件遍历 opts | 零（可删整套运行时对账） |
| 能否进 CI | ✗ | ✓（`--check --strict`） |
| 声明维护 | 手写 `_opts_keys()` 数组 | 手写 `@optkeys`（同一份工作量，但有对账用例兜着） |
| 编译器是否知道框架 | —— | **不知道**（只认注解名） |
