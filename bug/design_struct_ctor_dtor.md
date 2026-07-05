# 设计：struct 构造函数 / 析构函数

## 动机

`Dialog` 需要存窗口状态（handle、wid）并在离开作用域时自动销毁。
当前只能存 `Dict`/`int` 等简单类型，无法存 `Window` 对象的引用。

## 语法

```leno
export struct Dialog {
    Dict    style = {}
    Ptr[u8] _hdl  = null
    int     _wid  = -1

    // 构造函数：字段赋值完成后自动调用，无参
    func Dialog() {
        var win = wnd.createWindow(style)
        _hdl = win.getHandle()
        _wid = win.getID()
    }

    // 析构函数：变量离开作用域时自动调用，无参无返回
    func ~Dialog() {
        if _hdl != null { wnd.destroyByHandle(_hdl); _hdl = null }
    }
}

// 使用
main() {
    Dialog d = new Dialog(style={title: "确认", w: 350, h: 200})
    d.run(onEvent, onRender)
    // d 离开作用域 → ~Dialog() 自动销毁窗口
}
```

## 规则

| 规则 | 说明 |
|------|------|
| `func StructName()` | 构造，字段赋值完成后自动调用，无参 |
| `func ~StructName()` | 析构，变量离开作用域时自动调用，无参无返回 |
| 字段赋值顺序 | `new Dialog(style={...})` 先赋值字段，再调构造 |
| 不破坏现有语法 | 无构造/析构的结构体行为不变 |
| 调用顺序 | 构造 → 使用 → 离开作用域 → 析构 |
| 多次构造/析构 | 不允许，编译报错 |

## 析构触发时机

| 场景 | 行为 |
|------|------|
| 作用域结束 | 自动调用析构 |
| return 语句 | 自动调用析构后返回 |
| 异常抛出 | 暂未实现，未来支持 |

## 实现要点

### 编译器行为

```
生成代码时：
  1. 进入作用域 → 维护 localStructs 栈
  2. 声明 struct → 检查是否有 ~StructName()，标记 needDtor
  3. 遇到 return → 逆序遍历 localStructs，插入析构调用
  4. 作用域结束 → 同上
```

### 性能

- 无析构函数的 struct：零开销，不生成额外指令
- 有析构函数的 struct：作用域末尾/return 前插入 `OP_LOAD_LOCAL + OP_CALL_METHOD`
- 开销：一次方法调用，negligible

## 为什么简单

- 不学 C++：无参构造、无参析构，没有 RAII 那一套
- 不学 Go：不用 `defer`，随作用域自动管理
- 核心场景：创建窗口/文件/连接 → 用完自动关闭

## 对 Dialog 的影响

`Dialog` 现在可以存 `_hdl`/`_wid`，`run()` 直接用已创建的窗口，`~Dialog()` 自动销毁。
不需要在 `run()` 里临时创建窗口。

## 教程章节（补充到入门教程）

在 **"结构体（Struct）"** 章节后新增 **"构造与析构"** 小节：

```leno
// 构造函数
struct Counter {
    int count = 0
    func Counter() { print("创建") }
}

// 析构函数
struct Resource {
    Ptr handle = null
    func Resource() { handle = ffi.malloc(1024) }
    func ~Resource() { if handle != null { ffi.free(handle) } }
}

func test() {
    var r = new Resource()   // 构造：分配内存
}   // 析构：自动释放内存
```

**注意事项：**
- 构造/析构无参数，简化设计
- 析构在作用域结束或 return 时自动调用
- 异常抛出时的析构暂未实现

## 状态

- [x] 设计完成
- [ ] 编译器实现（作用域末尾析构）
- [ ] 编译器实现（return 前析构）
- [ ] 编译器实现（异常时析构，未来）
