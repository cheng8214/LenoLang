# LenoC 编译器 Bug：跨模块函数返回类型无法识别通过 `use` 导入的 struct

## 问题描述

当模块 A 通过 `use B.SomeType` 导入另一个模块 B 的 struct 类型，并将其作为 `export func` 的返回类型时：

```leno
// outer.leno
import "./inner.leno" as inner
use inner.Inner

export func makeOuter(): Inner {
    return inner.makeInner()
}
```

外部模块调用 `outer.makeOuter()` 时，编译器无法正确推断返回类型为 `Inner`，而是退化为 `any`：

```leno
// use.leno
import "./outer.leno" as outer

main() {
    var o = outer.makeOuter()   // o 被推断为 any
    print(o.hello())            // 报错: 不能在 any 类型上调用方法 'hello'
}
```

## 错误信息

```
[语义错误] use.leno 第 5 行第 1 列: 不能在 any 类型上调用方法 'hello'
提示: 使用 if x is Type { x.hello() } 类型收窄
或者将参数声明为具体类型
[语义错误] use.leno 第 5 行第 1 列: 不能对未初始化的变量 'o' 调用方法 'hello'
```

