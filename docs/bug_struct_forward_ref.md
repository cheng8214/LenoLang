# Bug: struct 前向引用在函数作用域内失效

## 现象

函数定义在 struct 之前时，函数体内无法访问 struct 字段：

```leno
// ❌ 报错："无法确定字段索引，struct 类型可能未定义"
func makePixel(int r, int g, int b): Pixel {
    var p = new Pixel()
    p.r = r     // ← 这里报错
    p.g = g
    p.b = b
    return p
}

struct Pixel { int r; int g; int b }

main() {
    var p = makePixel(255, 128, 64)
    print(p.r)   // 这里正常
}
```

如果 struct 定义在函数之前则正常（`makePixel` 后，`struct Pixel` 前）。

