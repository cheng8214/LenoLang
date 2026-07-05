# Bug: `export struct` 含 `Dict` 字段时，`use` 导入后方法全部丢失

## 环境
- 发现: 最新
- 复现: `examples/测试/repro_dict_field_use_*.leno`

## 现象

```leno
// A.leno
export struct A_Int  { int  val = 0; func ok(): int { return val } }
export struct B_Dict { Dict val = {}; func ok(): int { return 1 } }

// B.leno
import "A.leno" as m; use m.A_Int; use m.B_Dict
A_Int a = new A_Int(val=1); a.ok()  // ✅ 正常
B_Dict b = new B_Dict(val={}); b.ok()  // ❌ "没有方法 'ok'"
```

`Dict` 作为 `export struct` 字段时，通过 `use` 导入后该 struct 的**所有方法不可见**。`int`/`bool`/`string` 等基本类型正常。

## 影响

- `Dialog` 结构体（含 `Dict style` 字段）无法跨模块使用
- 所有含 `Dict` 字段的导出结构体受影响

## 状态

- [ ] 待修复
