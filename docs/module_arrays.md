# LenoC Arrays 模块

本文档详细说明 `arrays` 模块提供的数组操作功能。

## 目录

- [概述](#概述)
- [使用方式](#使用方式)
- [数组创建](#数组创建)
- [核心方法](#核心方法)
- [查找方法](#查找方法)
- [排序和反转](#排序和反转)
- [字符串操作](#字符串操作)
- [函数式方法](#函数式方法)
- [批量操作](#批量操作)
- [实例方法 vs 模块方法](#实例方法-vs-模块方法)
- [示例代码](#示例代码)
- [性能说明](#性能说明)
- [注意事项](#注意事项)

---

## 概述

Arrays 模块是 Leno 语言中用于数组操作的核心模块，提供了丰富的数组操作方法。所有方法都支持**双模式调用**：既可以通过实例直接调用，也可以通过模块方式调用。

### 特性

- **双模式调用**：实例方法 `arr.method()` 或模块方法 `arrays.method(arr)`
- **原地操作**：`sort()` 和 `reverse()` 直接修改原数组
- **类型安全**：支持任意类型的数组操作
- **高性能**：核心操作使用 C 语言实现

---

## 使用方式

```leno
// 方式1：实例方法（推荐）
var arr = [1, 2, 3]
arr.add(4)
arr.sort()

// 方式2：模块方法
import arrays
arrays.add(arr, 4)
arrays.sort(arr)
```

---

## 数组创建

### 字面量创建

```leno
// 空数组
var empty = []

// 数字数组
var numbers = [1, 2, 3, 4, 5]

// 字符串数组
var fruits = ["apple", "banana", "cherry"]

// 混合类型数组
var mixed = [1, "hello", true, null]

// 嵌套数组
var matrix = [[1, 2], [3, 4], [5, 6]]
```

### 类型注解

```leno
// 指定数组元素类型
Array[int] numbers = [1, 2, 3]
Array[string] names = ["Alice", "Bob"]
```

---

## 核心方法

### `len()`

返回数组元素个数。

**参数**: 无  
**返回**: `int` - 数组长度

```leno
var arr = [1, 2, 3, 4, 5]
print(arr.len())  // 5

// 空数组
var empty = []
print(empty.len())  // 0
```

---

### `add(value)`

在数组末尾添加元素。

**参数**:
- `value` (any): 要添加的元素

**返回**: `int` - 添加后的数组长度

```leno
var arr = [1, 2, 3]
var new_len = arr.add(4)
print(arr)      // [1, 2, 3, 4]
print(new_len)  // 4

// 添加不同类型
arr.add("hello")
print(arr)  // [1, 2, 3, 4, hello]
```

---

### `pop()`

移除并返回最后一个元素。

**参数**: 无  
**返回**: `any` - 被移除的元素，数组为空返回 `null`

```leno
var arr = [1, 2, 3]
var last = arr.pop()
print(last)   // 3
print(arr)    // [1, 2]

// 空数组
var empty = []
var result = empty.pop()
print(result)  // null
```

---

### `insert(index, value)`

在指定位置插入元素。

**参数**:
- `index` (int): 插入位置（支持负索引）
- `value` (any): 要插入的元素

**返回**: `int` - 插入后的数组长度

```leno
var arr = [1, 2, 3]
arr.insert(1, 99)
print(arr)  // [1, 99, 2, 3]

// 负索引（从末尾计算）
arr.insert(-1, 88)
print(arr)  // [1, 99, 2, 88, 3]

// 插入到末尾
arr.insert(5, 77)
print(arr)  // [1, 99, 2, 88, 3, 77]
```

---

### `remove(index)`

移除并返回指定位置的元素。

**参数**:
- `index` (int): 要移除的位置（支持负索引）

**返回**: `any` - 被移除的元素

**错误**: 索引越界时抛出错误

```leno
var arr = ["a", "b", "c", "d"]
var removed = arr.remove(1)
print(removed)  // b
print(arr)      // [a, c, d]

// 负索引
arr.remove(-1)  // 移除最后一个
print(arr)      // [a, c]

// 越界错误
// arr.remove(10)  // 错误：remove() 索引越界
```

---

### `has(value)`

检查数组是否包含指定元素。

**参数**:
- `value` (any): 要查找的元素

**返回**: `bool` - 是否包含

```leno
var arr = [1, 2, 3, 4, 5]
print(arr.has(3))   // true
print(arr.has(10))  // false

// 字符串数组
var fruits = ["apple", "banana"]
print(fruits.has("apple"))   // true
print(fruits.has("grape"))   // false
```

---

### `copy()`

创建数组的深拷贝。

**参数**: 无  
**返回**: `Array` - 新数组

```leno
var arr1 = [1, 2, [3, 4]]
var arr2 = arr1.copy()

// 修改原数组不影响拷贝
arr1[0] = 99
arr1[2][0] = 99
print(arr1)  // [99, 2, [99, 4]]
print(arr2)  // [1, 2, [3, 4]]  <- 深拷贝，嵌套数组也被复制
```

---

### `clear()`

清空数组所有元素。

**参数**: 无  
**返回**: `Array` - 原数组（支持链式调用）

```leno
var arr = [1, 2, 3]
arr.clear()
print(arr)      // []
print(arr.len()) // 0

// 链式调用
var result = [1, 2, 3].clear().add(4)
print(result)  // [4]
```

---

## 查找方法

### `index_of(value)`

查找元素首次出现的索引。

**参数**:
- `value` (any): 要查找的元素

**返回**: `int` - 索引位置，未找到返回 `-1`

```leno
var arr = ["apple", "banana", "cherry", "banana"]
print(arr.index_of("banana"))   // 1
print(arr.index_of("cherry"))   // 2
print(arr.index_of("grape"))    // -1

// 数字数组
var nums = [10, 20, 30, 20]
print(nums.index_of(20))  // 1
```

---

### `last_index_of(value)`

查找元素最后出现的索引。

**参数**:
- `value` (any): 要查找的元素

**返回**: `int` - 索引位置，未找到返回 `-1`

```leno
var arr = ["apple", "banana", "cherry", "banana"]
print(arr.last_index_of("banana"))   // 3
print(arr.last_index_of("apple"))    // 0
print(arr.last_index_of("grape"))    // -1
```

---

## 排序和反转

### `sort()`

原地排序数组（使用 C 层 qsort）。

**参数**: 无  
**返回**: `Array` - 原数组（支持链式调用）

**排序规则**:
- 数字：按数值大小
- 字符串：按字典序
- 不同类型：按类型排序

```leno
// 数字排序
var nums = [64, 34, 25, 12, 22, 11, 90]
nums.sort()
print(nums)  // [11, 12, 22, 25, 34, 64, 90]

// 字符串排序
var fruits = ["cherry", "apple", "banana"]
fruits.sort()
print(fruits)  // [apple, banana, cherry]

// 链式调用
var result = [3, 1, 2].sort().reverse()
print(result)  // [3, 2, 1]
```

---

### `reverse()`

原地反转数组。

**参数**: 无  
**返回**: `Array` - 原数组（支持链式调用）

```leno
var arr = [1, 2, 3, 4, 5]
arr.reverse()
print(arr)  // [5, 4, 3, 2, 1]

// 链式调用
var result = [1, 2, 3].reverse().add(0)
print(result)  // [3, 2, 1, 0]
```

---

## 字符串操作

### `join(separator)`

将数组元素拼接为字符串。

**参数**:
- `separator` (string): 分隔符

**返回**: `string` - 拼接后的字符串

```leno
// 字符串数组
var words = ["Hello", "World", "Leno"]
print(words.join(" "))    // Hello World Leno
print(words.join(","))    // Hello,World,Leno
print(words.join(""))     // HelloWorldLeno

// 数字数组（自动转换）
var nums = [1, 2, 3, 4]
print(nums.join("-"))     // 1-2-3-4

// 混合数组
var mixed = [1, "a", true]
print(mixed.join(" | "))  // 1 | a | true
```

---

## 函数式方法

### `map(callback)`

遍历数组，对每个元素执行回调函数，返回新数组。

**参数**:
- `callback` (function): 回调函数，接收 `(item, index)` 两个参数，返回转换后的值

**返回**: `Array` - 新数组

```leno
var nums = [1, 2, 3, 4, 5]

// 基本用法
var doubled = nums.map(func(var x, var i) { return x * 2 })
print(doubled)  // [2, 4, 6, 8, 10]

// 使用索引
var indexed = nums.map(func(var x, var i) { return x + i })
print(indexed)  // [1, 3, 5, 7, 9]

// 模块方法
import arrays
var tripled = arrays.map(nums, func(var x, var i) { return x * 3 })
```

---

### `filter(callback)`

遍历数组，只保留回调函数返回 `true` 的元素，返回新数组。

**参数**:
- `callback` (function): 回调函数，接收 `(item, index)` 两个参数，返回 `bool`

**返回**: `Array` - 新数组

```leno
var nums = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]

// 过滤偶数
var evens = nums.filter(func(var x, var i) { return x % 2 == 0 })
print(evens)  // [2, 4, 6, 8, 10]

// 过滤大于5的数
var big = nums.filter(func(var x, var i) { return x > 5 })
print(big)  // [6, 7, 8, 9, 10]
```

---

### `reduce(callback, initial)`

对数组进行累积计算，返回单个值。

**参数**:
- `callback` (function): 回调函数，接收 `(accumulator, item, index)` 三个参数，返回新的累积值
- `initial` (any, 可选): 初始值。如果不提供，使用数组第一个元素作为初始值，从第二个元素开始遍历

**返回**: `any` - 累积结果

```leno
var nums = [1, 2, 3, 4, 5]

// 求和（带初始值）
var sum = nums.reduce(func(var acc, var x, var i) { return acc + x }, 0)
print(sum)  // 15

// 求积（带初始值）
var product = nums.reduce(func(var acc, var x, var i) { return acc * x }, 1)
print(product)  // 120

// 不带初始值（使用第一个元素）
var sum2 = nums.reduce(func(var acc, var x, var i) { return acc + x })
print(sum2)  // 15
```

---

### 链式调用

`map` 和 `filter` 返回新数组，支持链式调用：

```leno
var nums = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]

// 先过滤大于3的数，再乘以2
var result = nums.filter(func(var x, var i) { return x > 3 })
                 .map(func(var x, var i) { return x * 2 })
print(result)  // [8, 10, 12, 14, 16, 18, 20]

// 复杂管道：过滤偶数 -> 平方 -> 求和
var sum_of_squares = nums.filter(func(var x, var i) { return x % 2 == 0 })
                         .map(func(var x, var i) { return x * x })
                         .reduce(func(var acc, var x, var i) { return acc + x }, 0)
print(sum_of_squares)  // 220 (4+16+36+64+100)
```

---

## 批量操作

### 数组切片（语法糖）

使用 `[:]` 语法获取数组子集。

```leno
var arr = [1, 2, 3, 4, 5, 6, 7, 8]

// 基本切片
print(arr[2:5])   // [3, 4, 5, 6]  索引 2 到 5（包含）

// 从开头
print(arr[:3])    // [1, 2, 3, 4]  开始到索引 3

// 到结尾
print(arr[5:])    // [6, 7, 8]     索引 5 到结尾
```

---

## 实例方法 vs 模块方法

所有数组方法都支持两种调用方式：

| 实例方法 | 模块方法 | 说明 |
|---------|---------|------|
| `arr.len()` | `arrays.len(arr)` | 获取长度 |
| `arr.add(x)` | `arrays.add(arr, x)` | 添加元素 |
| `arr.pop()` | `arrays.pop(arr)` | 移除末尾 |
| `arr.insert(i, x)` | `arrays.insert(arr, i, x)` | 插入元素 |
| `arr.remove(i)` | `arrays.remove(arr, i)` | 移除指定位置 |
| `arr.has(x)` | `arrays.has(arr, x)` | 检查包含 |
| `arr.copy()` | `arrays.copy(arr)` | 深拷贝 |
| `arr.clear()` | `arrays.clear(arr)` | 清空 |
| `arr.index_of(x)` | `arrays.index_of(arr, x)` | 查找索引 |
| `arr.last_index_of(x)` | `arrays.last_index_of(arr, x)` | 反向查找 |
| `arr.reverse()` | `arrays.reverse(arr)` | 反转 |
| `arr.sort()` | `arrays.sort(arr)` | 排序 |
| `arr.join(s)` | `arrays.join(arr, s)` | 拼接字符串 |
| `arr.map(fn)` | `arrays.map(arr, fn)` | 映射转换 |
| `arr.filter(fn)` | `arrays.filter(arr, fn)` | 过滤元素 |
| `arr.reduce(fn, init)` | `arrays.reduce(arr, fn, init)` | 累积计算 |

### 选择建议

- **实例方法**：代码更简洁，推荐日常使用
- **模块方法**：需要动态方法名或函数式编程时使用

```leno
// 实例方法（推荐）
var arr = [3, 1, 2]
arr.sort().reverse()

// 模块方法（特殊场景）
import arrays
var method = "sort"
arrays[method](arr)  // 动态调用
```

---

## 示例代码

### 示例1：数据处理管道

```leno
main() {
    var data = [64, 34, 25, 12, 22, 11, 90]
    
    // 排序并过滤（模拟）
    data.sort()
    print("排序后: " + data)
    
    // 转换为字符串
    var str = data.join(", ")
    print("字符串: " + str)
    
    // 查找位置
    var idx = data.index_of(25)
    print("25 的位置: " + idx)
}
```

### 示例2：去重（使用辅助数组）

```leno
func unique(arr) {
    var result = []
    for arr to item {
        if not result.has(item) {
            result.add(item)
        }
    }
    return result
}

main() {
    var arr = [1, 2, 2, 3, 3, 3, 4]
    var unique_arr = unique(arr)
    print(unique_arr)  // [1, 2, 3, 4]
}
```

### 示例3：栈的实现

```leno
// 使用数组实现栈
func stack_new() {
    return []
}

func stack_push(stack, value) {
    stack.add(value)
}

func stack_pop(stack) {
    return stack.pop()
}

func stack_peek(stack) {
    return stack[stack.len() - 1]
}

func stack_is_empty(stack) {
    return stack.len() == 0
}

main() {
    var stack = stack_new()
    
    stack_push(stack, 1)
    stack_push(stack, 2)
    stack_push(stack, 3)
    
    print("栈顶: " + stack_peek(stack))  // 3
    
    while not stack_is_empty(stack) {
        print("弹出: " + stack_pop(stack))
    }
}
```

### 示例4：队列的实现

```leno
// 使用数组实现队列
func queue_new() {
    return []
}

func queue_enqueue(queue, value) {
    queue.add(value)
}

func queue_dequeue(queue) {
    return queue.remove(0)
}

func queue_is_empty(queue) {
    return queue.len() == 0
}

main() {
    var queue = queue_new()
    
    queue_enqueue(queue, "A")
    queue_enqueue(queue, "B")
    queue_enqueue(queue, "C")
    
    while not queue_is_empty(queue) {
        print("出队: " + queue_dequeue(queue))
    }
}
```

### 示例5：与 cstruct 结合使用

```leno
import ffi

cstruct Point {
    i32 x
    i32 y
}

main() {
    // 创建点数组
    var points = Point.malloc_array(3)
    points[0].x = 0;  points[0].y = 0
    points[1].x = 10; points[1].y = 20
    points[2].x = 5;  points[2].y = 15
    
    // 提取坐标到普通数组
    var x_coords = []
    for 0 : points.len() - 1 to i {
        x_coords.add(points[i].x)
    }
    
    print("X 坐标: " + x_coords)  // [0, 10, 5]
    x_coords.sort()
    print("排序后: " + x_coords)  // [0, 5, 10]
    
    points.free_all()
}
```

---

## 性能说明

### 时间复杂度

| 方法 | 时间复杂度 | 说明 |
|-----|-----------|------|
| `len()` | O(1) | 直接读取计数器 |
| `add()` | O(1) 均摊 | 动态扩容 |
| `pop()` | O(1) | 移除末尾 |
| `insert()` | O(n) | 需要移动元素 |
| `remove()` | O(n) | 需要移动元素 |
| `has()` | O(n) | 线性查找 |
| `index_of()` | O(n) | 线性查找 |
| `last_index_of()` | O(n) | 线性查找 |
| `sort()` | O(n log n) | C 层 qsort |
| `reverse()` | O(n) | 双指针交换 |
| `join()` | O(n) | 创建新字符串 |
| `copy()` | O(n) | 深拷贝 |
| `map()` | O(n) | 创建新数组 |
| `filter()` | O(n) | 创建新数组 |
| `reduce()` | O(1) | 原地累积 |

### 空间复杂度

| 方法 | 空间复杂度 | 说明 |
|-----|-----------|------|
| `sort()` | O(1) | 原地排序 |
| `reverse()` | O(1) | 原地反转 |
| `copy()` | O(n) | 创建新数组 |
| `join()` | O(n) | 创建新字符串 |

---

## 注意事项

### 1. 索引越界

访问数组时索引必须在有效范围内：

```leno
var arr = [1, 2, 3]
print(arr[0])   // OK: 1
print(arr[2])   // OK: 3
// print(arr[3]) // 错误：索引越界

// 负索引（从末尾计算）
print(arr[-1])  // OK: 3
print(arr[-3])  // OK: 1
// print(arr[-4]) // 错误：索引越界
```

### 2. 原地操作

`sort()` 和 `reverse()` 会修改原数组：

```leno
var arr = [3, 1, 2]
var result = arr.sort()

// arr 和 result 是同一个数组
print(arr)     // [1, 2, 3]
print(result)  // [1, 2, 3]
print(arr == result)  // true

// 如果需要保留原数组，先拷贝
var original = [3, 1, 2]
var sorted = original.copy().sort()
print(original)  // [3, 1, 2]
print(sorted)    // [1, 2, 3]
```

### 3. 深拷贝 vs 浅拷贝

`copy()` 是深拷贝，嵌套数组也会被复制：

```leno
var arr1 = [[1, 2], [3, 4]]
var arr2 = arr1.copy()

arr1[0][0] = 99
print(arr1)  // [[99, 2], [3, 4]]
print(arr2)  // [[1, 2], [3, 4]]  <- 不受影响
```

### 4. 类型混合

数组可以包含任意类型，但某些操作可能需要相同类型：

```leno
var mixed = [1, "a", 2, "b"]

// sort() 可以处理混合类型（按类型分组排序）
mixed.sort()
// 结果可能是 [1, 2, a, b] 或类似

// join() 可以处理混合类型（自动转字符串）
print(mixed.join("-"))  // 1-2-a-b
```

---

## 最佳实践

1. **使用实例方法**：代码更简洁易读
   ```leno
   // 推荐
   arr.sort().reverse()
   
   // 不推荐
   arrays.reverse(arrays.sort(arr))
   ```

2. **链式操作**：利用返回自身的特性
   ```leno
   var result = arr.copy().sort().reverse()
   ```

3. **预分配容量**：如果知道大致大小，可以先添加再修改
   ```leno
   var arr = []
   for 0 : 1000 to i {
       arr.add(0)  // 预分配
   }
   // 然后修改
   for 0 : 999 to i {
       arr[i] = i * 2
   }
   ```

4. **使用 `has()` 替代 `index_of()`**：如果只需要检查存在性
   ```leno
   // 推荐
   if arr.has(value) { ... }
   
   // 不推荐（如果不需要索引）
   if arr.index_of(value) != -1 { ... }
   ```

---

*文档版本: 1.0*  
*最后更新: 2026-05-17*
