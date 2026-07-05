# Bug: `Array[alias_func]` 编译期保留类型，运行时元素为 `any`

## 环境
- 发现: 最新
- 复现: `examples/测试/repro_array_handler_type.leno`

## 现象

```leno
export alias Handler = func(int):int
Array[Handler] arr = []
Handler h = func(int x): int { return x * 2 }
arr.add(h)

var v2 = arr[0]
print(v2 is Handler)  // true（编译期）
print(v2(5))          // ✅ 正常（编译期类型保留）

// ⚠️ 但直接 arr[0](5) 在非本地作用域（如 struct 方法内、export func 内）报错:
// "只能调用函数（对象类型不正确）"
```

## 影响

`Array[EventHandler]` 在模块级变量中声明后，struct 方法内访问 `arr[i](ev)` 运行时失败。需用 `var cb = arr[i]; cb(ev)` 或 `if cb is Handler { cb(ev) }` 规避。

## workaround

```leno
var cb = _runEvts[i]     // 提取到局部 var
if cb is EventHandler { cb(ev) }
```

## 状态
- [ ] 待修复
