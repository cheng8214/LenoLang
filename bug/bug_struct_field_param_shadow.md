# Bug: struct 方法参数与字段同名时类型推断混乱

## 概述

当 struct 字段与方法参数同名时，编译器不报错也不正确覆盖，而是将字段类型错误地套用到参数上，产生误导性报错。

## 复现（`sdl_button.leno` 真实场景）

```leno
struct Button {
    int cb          // 背景蓝色值

    func on_click(func():void cb) {  // cb 与字段同名
        // 报错：期望 func():void，实际 int
        // 编译器把 int cb(字段) 的类型套到了 func():void cb(参数) 上
    }
}
```

**错误**：
```
[类型不匹配] 字段 '_onClick' 类型不匹配: 期望 'func():void'，实际 'int'
```

当时我们 `_onClick = cb` 这行报错，根源就是参数 `cb` 被字段 `int cb` 的类型覆盖了。

文件：`examples/测试/test_bug_struct_field_shadow.leno`

## 预期行为

1. **方案 A（报错）**：检测到参数与字段同名 → 明确报错 "参数名 cb 与字段冲突"
2. **方案 B（遮蔽）**：参数优先，方法内 `cb` 指向参数；字段通过 `_cb` 或 `this.cb` 访问

当前两者都没做到——不做任何处理，随机选用错误的类型。

## 另外测试：同类型参数遮蔽字段

```leno
struct S {
    int x
    func test(int x) { print(x) }  // x 是参数还是字段？
}

main() {
    S s = new S()
    s.x = 10
    s.test(20)  // 输出 20（参数遮蔽字段正常）
}
```

同类型参数遮蔽字段功能正常，问题只出在**不同类型**时。

## 影响

- 按钮控件开发时被 `int cb` / `func cb` 命名冲突坑了近 20 分钟
- 错误消息完全不指向真正的命名冲突问题

## 修复方向

`semantic_type.c` 或 `semantic_visit_method.c` 中，方法参数解析时应先检查是否与 struct 字段同名，若有冲突且类型不同则报错；同类型则可遮蔽。

## 环境

- 提交: c17a0097
- 场景: `sdl_button.leno` `on_click` 方法中 `cb` 参数 vs `cb` 字段

## 状态

- [ ] 待修复
