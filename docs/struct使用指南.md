# LenoC Struct 使用指南

## 快速入门

```leno
// 定义一个 struct
struct Point {
    int x = 0
    int y = 0
}

// 创建实例
main() {
    var p = Point()     // 创建 Point 实例
    p.x = 10            // 访问字段
    p.y = 20
    print(p)            // 输出: Point{x=10, y=20}
}
```

***

## 1. 基本语法

### 1.1 定义 struct

```leno
struct 名称 {
    类型 字段名 [= 默认值]
    ...
}
```

**示例：**

```leno
struct Person {
    string name = ""
    int age = 0
    bool active = true
}

struct Size {
    int width = 100
    int height = 100
}
```

### 1.2 创建实例

```leno
// 方式1：使用默认构造函数（所有字段使用默认值）
var p = Point()

// 方式2：显式设置字段值
var p2 = Point()
p2.x = 100
p2.y = 200
```

### 1.3 访问字段

```leno
// 使用点号访问
var x = p.x
var y = p.y

// 使用索引访问（等价于点号）
var x2 = p["x"]
var y2 = p["y"]
```

***

## 2. struct 方法

### 2.1 在 struct 中定义方法

```leno
struct Rectangle {
    int width = 0
    int height = 0
    
    // 计算面积
    func area():int {
        return width * height    // 直接访问字段
    }
    
    // 计算周长
    func perimeter():int {
        return 2 * (width + height)
    }
}

main() {
    var rect = Rectangle()
    rect.width = 10
    rect.height = 5
    
    print(rect.area())       // 输出: 50
    print(rect.perimeter())  // 输出: 30
}
```

### 2.2 方法内访问字段

在 struct 方法内部，**直接写字段名**即可访问：

```leno
struct Counter {
    int count = 0
    
    func increment() {
        count = count + 1    // 直接访问 count 字段
    }
    
    func getCount():int {
        return count         // 直接返回 count
    }
}
```

### 2.3 方法调用其他方法

```leno
struct Calculator {
    int value = 0
    
    func add(int n) {
        value = value + n
    }
    
    func double() {
        add(value)    // 调用同 struct 的其他方法
    }
}
```

***

## 3. struct 嵌套

### 3.1 基本嵌套

struct 字段可以是另一个 struct 类型：

```leno
struct Point {
    int x = 0
    int y = 0
}

struct Size {
    int width = 100
    int height = 100
}

struct Rect {
    Point position      // 嵌套 Point
    Size size           // 嵌套 Size
}

main() {
    var r = Rect()
    // 嵌套对象自动初始化
    print(r)  // Rect{position=Point{x=0, y=0}, size=Size{width=100, height=100}}
    
    // 访问嵌套字段
    r.position.x = 10
    r.position.y = 20
    r.size.width = 200
    r.size.height = 150
    
    print(r.position.x)  // 10
    print(r.size.width)  // 200
}
```

### 3.2 自引用 struct（链表节点）

**⚠️ 重要：自引用必须设置 null 默认值**

```leno
// ❌ 错误：会导致栈溢出（无限递归）
struct Node {
    int value
    Node next    // 没有默认值，会无限创建 Node
}

// ✅ 正确：设置 null 默认值
struct Node {
    int value
    Node next = null    // 默认为 null，避免递归
}

struct LinkedList {
    Node head = null    // 链表头默认为 null
    int count = 0
}
```

### 3.3 链表实现示例

```leno
struct Node {
    int value
    Node next = null
}

struct LinkedList {
    Node head = null
    int count = 0
}

// 在链表尾部添加节点
func list_add(LinkedList list, int value) {
    var new_node = Node()
    new_node.value = value
    
    if (list.head == null) {
        list.head = new_node
    } else {
        var current = list.head
        while (current.next != null) {
            current = current.next
        }
        current.next = new_node
    }
    list.count = list.count + 1
}

// 打印链表
func list_print(LinkedList list) {
    print("链表: ")
    var current = list.head
    while (current != null) {
        print(current.value)
        print(" -> ")
        current = current.next
    }
    print("null")
}

main() {
    var list = LinkedList()
    
    list_add(list, 10)
    list_add(list, 20)
    list_add(list, 30)
    
    list_print(list)        // 输出: 10 -> 20 -> 30 -> null
    print("数量: ")
    print(list.count)       // 输出: 3
}
```

***

## 4. 字段默认值

### 4.1 基本默认值

```leno
struct Config {
    string name = "default"
    int port = 8080
    bool debug = false
}

main() {
    var cfg = Config()
    print(cfg.name)   // "default"
    print(cfg.port)   // 8080
    print(cfg.debug)  // false
}
```

### 4.2 null 默认值与编译时检查

**⚠️ 重要：默认值为 null 的字段，访问其成员会有编译时警告**

```leno
struct Container {
    dict data = null      // 默认 null
    array items = null    // 默认 null
}

struct Outer {
    Container inner = null
}

main() {
    var c = Container()
    
    // ✅ 获取 null 本身 - 不警告
    var d = c.data
    
    // ❌ 访问 null 字段的成员 - 编译时警告
    var v = c.data["key"]     // [语义错误] 字段 'c.data' 默认值为 null...
    var item = c.items[0]     // [语义错误] 字段 'c.items' 默认值为 null...
    
    // ❌ 链式访问 null 字段
    var o = Outer()
    var x = o.inner.data      // [语义错误] 字段 'o.inner' 默认值为 null...
}
```

### 4.3 在方法中访问 null 字段

```leno
struct LinkedList {
    Node head = null
    
    func getFirst():int {
        // ❌ 编译时警告：访问可能为 null 的字段
        return head.value
    }
    
    func safeGetFirst():int {
        // ✅ 先检查再访问
        if (head != null) {
            return head.value
        }
        return 0
    }
}
```

***

## 5. 类型检查与提示

### 5.1 未定义 struct 类型报错

```leno
main() {
    // ❌ 错误：未定义的 struct 类型
    UndefinedType x    // [未定义变量] 未定义的 struct 类型: UndefinedType
}
```

### 5.2 常见类型名拼写检查

```leno
main() {
    // ❌ 拼写错误会提示正确的类型名
    dic13412t a    // [未定义变量] 未定义的 struct 类型: dic13412t
    arry b         // [未定义变量] 未定义的 struct 类型: arry
    
    // ✅ 正确的类型名
    Dict d
    Array arr
}
```

***

## 6. 最佳实践

### 6.1 自引用字段必须设 null 默认值

```leno
// ✅ 正确
struct TreeNode {
    int value
    TreeNode left = null
    TreeNode right = null
}

// ❌ 错误：会导致栈溢出
struct TreeNode {
    int value
    TreeNode left    // 没有默认值！
    TreeNode right   // 没有默认值！
}
```

### 6.2 方法参数使用具体类型

```leno
// ✅ 正确：使用具体类型，可以访问字段
func process(LinkedList list) {
    list.head = null    // 可以访问 .head
}

// ❌ 不推荐：var 参数无法访问 struct 字段
func process(var list) {
    list.head = null    // 编译错误：无法识别 .head
}
```

### 6.3 检查 null 后再访问

```leno
struct Container {
    dict data = null
    
    func getValue(string key):any {
        // ✅ 先检查再访问，避免运行时错误
        if (data != null) {
            return data[key]
        }
        return null
    }
}
```

### 6.4 使用有意义的默认值

```leno
// ✅ 好的设计：合理的默认值
struct ServerConfig {
    string host = "localhost"
    int port = 8080
    int timeout = 30
    bool enableLog = true
}

// ❌ 不推荐：所有字段都是 0/空值
struct ServerConfig {
    string host = ""
    int port = 0
    int timeout = 0
    bool enableLog = false
}
```

### 6.5 struct 与类型守卫结合使用

当使用 `var` 接收 struct 实例时，类型守卫可以收窄类型，安全访问字段：

```leno
struct Student {
    string name = ""
    int age = 0
}

func process(var obj) {
    // ❌ 直接访问会报错：var 无法识别字段
    // print(obj.name)
    
    // ✅ 使用类型守卫收窄类型
    if obj is Student {
        // 守卫块内 obj 被视为 Student 类型
        print(obj.name)   // 可以访问字段
        print(obj.age)
    }
}

main() {
    var s = Student()
    s.name = "张三"
    s.age = 18
    process(s)   // 输出: 张三  18
}
```

### 6.6 使用插值字符串输出 struct

推荐使用插值字符串 `$"..."` 来格式化输出，比字符串拼接更简洁：

```leno
struct Point {
    int x = 0
    int y = 0
}

main() {
    var p = Point()
    p.x = 10
    p.y = 20
    
    // ❌ 字符串拼接，需要显式转换
    print("坐标: (" + _str(p.x) + ", " + _str(p.y) + ")")
    
    // ✅ 插值字符串，自动转换
    print($"坐标: ({p.x}, {p.y})")
    
    // 也可以使用 format 方法
    import strings
    print(strings.format("坐标: ({0}, {1})", p.x, p.y))
}
```

***

## 7. 完整示例

### 7.1 学生管理系统

```leno
struct Student {
    string name = ""
    int age = 0
    Array[int] scores = []
    
    func averageScore():float {
        if (scores.len() == 0) {
            return 0.0
        }
        int total = 0
        for scores to score {
            total = total + score
        }
        return total / scores.len()
    }
}

struct Class {
    string name = ""
    Array[Student] students = []
    
    func addStudent(Student s) {
        students.add(s)
    }
    
    func getAverageAge():float {
        if (students.len() == 0) {
            return 0.0
        }
        int total = 0
        for students to s {
            total = total + s.age
        }
        return total / students.len()
    }
}

main() {
    var mathClass = Class()
    mathClass.name = "数学一班"
    
    var s1 = Student()
    s1.name = "张三"
    s1.age = 18
    s1.scores = [85, 90, 78]
    
    var s2 = Student()
    s2.name = "李四"
    s2.age = 19
    s2.scores = [92, 88, 95]
    
    mathClass.addStudent(s1)
    mathClass.addStudent(s2)
    
    print("班级: " + mathClass.name)
    print("平均年龄: " + mathClass.getAverageAge())
    print("张三平均分: " + s1.averageScore())
}
```

### 7.2 二叉搜索树

```leno
struct TreeNode {
    int value
    TreeNode left = null
    TreeNode right = null
}

struct BST {
    TreeNode root = null
    
    func insert(int val) {
        root = insertNode(root, val)
    }
    
    func insertNode(TreeNode node, int val):TreeNode {
        if (node == null) {
            var newNode = TreeNode()
            newNode.value = val
            return newNode
        }
        if (val < node.value) {
            node.left = insertNode(node.left, val)
        } else if (val > node.value) {
            node.right = insertNode(node.right, val)
        }
        return node
    }
    
    func inorder() {
        inorderTraversal(root)
    }
    
    func inorderTraversal(TreeNode node) {
        if (node == null) {
            return
        }
        inorderTraversal(node.left)
        print(node.value)
        print(" ")
        inorderTraversal(node.right)
    }
}

main() {
    var tree = BST()
    tree.insert(50)
    tree.insert(30)
    tree.insert(70)
    tree.insert(20)
    tree.insert(40)
    
    print("中序遍历: ")
    tree.inorder()    // 输出: 20 30 40 50 70
}
```

***

## 8. 常见问题

### Q: 为什么自引用 struct 必须设 null 默认值？

A: 否则会无限递归创建实例，导致栈溢出：

```leno
// ❌ 错误
struct Node {
    Node next    // 创建 Node 时要创建 next，创建 next 时要创建 next.next...
}

// ✅ 正确
struct Node {
    Node next = null    // 默认为 null，停止递归
}
```

### Q: 为什么 var 参数不能访问 struct 字段？

A: `var` 参数类型为 `any`，编译器不知道具体类型，无法生成字段访问指令：

```leno
func test(var list) {
    list.head = null    // ❌ 编译器不知道 list 是什么类型
}

// 应该使用具体类型
func test(LinkedList list) {
    list.head = null    // ✅ 编译器知道是 LinkedList
}
```

### Q: 如何检查 null 字段？

A: 使用 `!= null` 或 `== null` 检查：

```leno
struct Container {
    dict data = null
}

func safeAccess(Container c) {
    if (c.data != null) {
        // 安全访问
        var v = c.data["key"]
    }
}
```

***

## 9. 性能优化

### 9.1 字段索引优化（编译期确定）

LenoC 对 struct 字段访问进行了优化，编译期确定字段索引，运行时直接访问，避免线性搜索。

**优化对比：**

```leno
struct Point {
    int x
    int y
}

main() {
    var p = Point(x=10, y=20)
    
    // ✅ 推荐：字段访问（O(1) 直接索引）
    // 编译期确定 y 的索引为 1，运行时直接访问
    var y1 = p.y
    
    // ⚠️ 较慢：索引访问（O(n) 字符串查找）
    // 运行时需要通过字符串 "y" 查找字段位置
    var y2 = p["y"]
}
```

**性能数据（100万次访问）：**

| 访问方式 | 耗时 | 说明 |
|---------|------|------|
| `p.y` | ~47ms | O(1) 直接索引访问 |
| `p["y"]` | ~62ms | O(n) 字符串查找 |

对于字段较多的 struct，性能提升更明显：

```leno
struct BigStruct {
    int field0
    int field1
    // ... 更多字段
    int field19
}

main() {
    var obj = BigStruct(field19 = 100)
    
    // 访问第 20 个字段：
    // - 字段访问：O(1)，直接定位
    // - 索引访问：O(n)，需要遍历比较字段名
    var val = obj.field19
}
```

### 9.2 优化建议

1. **优先使用字段访问** `obj.field` 而不是 `obj["field"]`
2. **在方法内直接访问字段**，编译器会自动优化为直接索引
3. **字段数量无性能担忧**，即使 struct 有几十个字段，访问速度仍然是 O(1)

***

## 10. face（接口）

face 用于定义方法签名契约，让不同 struct 实现相同的行为，实现多态。

### 10.1 定义 face

```leno
face 名称 {
    func 方法名(参数列表):返回类型
    ...
}
```

face 体只含方法签名，不含字段、不含方法体。

```leno
face Speaker {
    func speak():string
}

face Writer {
    func write(string content)
    func flush()
}
```

### 10.2 struct 实现 face

**隐式满足**（不声明，只要方法签名匹配就自动满足）：

```leno
struct Dog {
    string name = ""
    func speak():string { return "woof" }
}
// Dog 自动满足 Speaker（有 speak():string 方法）
```

**显式声明**（编译器检查是否真的满足，不满足则报错）：

```leno
struct Cat impl Speaker {
    func speak():string { return "meow" }
}

// 多个 face 用逗号分隔
struct FileLogger impl Writer, Speaker {
    func write(string content) { }
    func flush() { }
    func speak():string { return "FileLogger" }
}

// ❌ 编译错误：声明了但缺少方法
struct Fish impl Speaker {
    func swim() { }    // 缺少 speak() 方法
}

// ❌ 编译错误：返回类型不匹配
struct BadSpeaker impl Speaker {
    func speak():int { return 42 }    // face 声明返回 string，实际返回 int
}

// ❌ 编译错误：参数数量不匹配
face Adder {
    func add(int a, int b):int
}

struct BadAdder impl Adder {
    func add(int a):int { return a }    // face 声明 2 个参数，实际只有 1 个
}
```

**编译期 impl 检查规则：**

| 检查项 | 说明 | 示例 |
|--------|------|------|
| 方法缺失 | struct 必须实现 face 声明的所有方法 | `impl Speaker` 但没有 `speak()` → 报错 |
| 返回类型匹配 | 方法返回类型必须与 face 声明一致 | face 声明 `:int`，struct 返回 `:string` → 报错 |
| 参数数量匹配 | 方法参数数量（不含 self）必须与 face 一致 | face 声明 2 个参数，struct 只有 1 个 → 报错 |
| 跨模块 face | 导入模块的 face 同样适用以上检查 | `use pm.ProcessManager` 后 `impl ProcessManager` → 检查 |

### 10.3 face 作为参数类型

```leno
func make_sound(Speaker s) {
    print(s.speak())
}

var d = Dog()
var c = Cat()
make_sound(d)    // 输出: woof
make_sound(c)    // 输出: meow
```

### 10.4 face 作为变量类型

```leno
Speaker s = Dog()
print(s.speak())     // woof

s = Cat()
print(s.speak())     // meow
```

### 10.5 face 类型守卫

使用 `is` 运算符在**运行时**检查 struct 实例是否实现了某个 face（包括显式 `impl` 声明和隐式鸭子类型）：

```leno
var obj = Dog()
if obj is Speaker {
    print(obj.speak())     // 守卫内 obj 视为 Speaker 类型
}
```

**`not is` 运算符**（否定检查）：

```leno
var rock = Rock()
if rock not is Speaker {
    print("Rock 不是 Speaker")
}
```

**类型守卫的两种检查方式：**

| 检查方式 | 说明 | 示例 |
|---------|------|------|
| 显式 impl | struct 用 `impl Face` 声明 | `struct Dog impl Speaker` |
| 隐式满足 | struct 有 face 要求的所有方法签名（鸭子类型） | `struct Duck { func speak():string }` |

```leno
face Speaker {
    func speak():string
}

// 显式声明
struct Dog impl Speaker {
    func speak():string { return "woof" }
}

// 隐式满足（鸭子类型）
struct Duck {
    func speak():string { return "quack" }
}

main() {
    var d = Dog()
    var duck = Duck()

    if d is Speaker {
        print("Dog is Speaker")        // ✅ 输出
    }
    if duck is Speaker {
        print("Duck is Speaker")       // ✅ 输出（鸭子类型）
    }
}
```

### 10.5.1 as 安全类型转换

`as` 运算符用于**安全向下转型**：运行时检查类型，匹配则返回原值，不匹配则返回 `null`。这是 `as` 在 face/struct 场景下最核心的用途。

**基本语法：**

```leno
var result = 表达式 as 类型名
```

**face 向下转型（最常用）：**

```leno
face Shape {
    func area():float
}

struct Circle impl Shape {
    float _radius = 1.0
    func area():float { return 3.14159 * _radius * _radius }
}

struct Rect impl Shape {
    float _width = 1.0
    float _height = 1.0
    func area():float { return _width * _height }
}

func describe(Shape s) {
    var c = s as Circle
    if c != null {
        return "Circle radius=" + _str(c._radius)    // ✅ 安全访问 Circle 字段
    }
    var r = s as Rect
    if r != null {
        return "Rect " + _str(r._width) + "x" + _str(r._height)
    }
    return "Unknown shape"
}

assert_eq(describe(Circle()), "Circle radius=1")
assert_eq(describe(Rect()), "Rect 1x1")
```

**struct 精确匹配：**

```leno
var dog = Dog()
var d = dog as Dog       // Dog 实例（匹配，返回原值）
var c = dog as Cat       // null（不匹配，Dog 不是 Cat）
```

**`as` 与 `is` 的区别：**

| 特性 | `is` | `as` |
|------|------|------|
| 返回值 | `bool`（true/false） | 原值或 `null` |
| 用途 | 条件判断 | 安全转型 + null 检查 |
| 典型场景 | `if x is Dog { ... }` | `var d = s as Dog; if d != null { ... }` |

**`as` 与类型转换函数的区别：**

| 写法 | 语义 | 结果 |
|------|------|------|
| `42 as float` | 类型检查 | `null`（42 不是 float） |
| `_float(42)` | 类型转换 | `42.0`（int→float 转换） |
| `"abc" as int` | 类型检查 | `null`（不匹配） |
| `_int("abc")` | 类型转换 | 运行时错误（解析失败） |

> **核心区别**：`as` 是安全类型检查，不做转换，不匹配返回 `null`；`_int()`/`_float()` 等是类型转换，转换失败抛出运行时错误。

### 10.6 face 不能实例化

```leno
Speaker s = Speaker()    // ❌ 错误：face 不能实例化
```

### 10.7 方法签名必须完全匹配

编译器会检查 struct 方法的签名是否与 face 声明完全一致，包括**返回类型**和**参数数量**：

```leno
face Adder {
    func add(int a, int b):int
}

// ✅ 签名完全匹配
struct Calculator impl Adder {
    func add(int a, int b):int { return a + b }
}

// ❌ 返回类型不匹配：face 声明 int，实际 float
struct FloatCalc impl Adder {
    func add(int a, int b):float { return a + b }
}

// ❌ 参数数量不匹配：face 声明 2 个参数，实际 3 个
struct ExtraCalc impl Adder {
    func add(int a, int b, int c):int { return a + b + c }
}
```

**注意：** struct 方法的 `self` 参数不计入 face 参数数量比较。例如 face 声明 `func add(int a, int b):int`（2 个参数），struct 实现 `func add(int a, int b):int`（编译器内部 pcnt=3，含 self，但比较时自动减 1）。

**跨模块 face 检查：** 当 face 定义在导入模块中时，编译器同样会进行签名匹配检查：

```leno
import "process.leno" as pm
use pm.ProcessManager

// 编译器会检查 WinManager 是否实现了 ProcessManager 的所有方法
// 包括返回类型和参数数量
struct WinManager impl ProcessManager {
    func getCurrentPid():int { return 1234 }
    func listProcesses():Dict { return {} }
    // ... 必须实现所有 face 方法
}
```

### 10.8 空 face（标记接口）

face 可以不声明任何方法，作为**标记接口**使用：

```leno
face Empty {
}

struct Thing impl Empty {
    string name = ""
}

func check(Empty e) {
    print("got empty face")
}

main() {
    var t = Thing()
    check(t)    // 输出: got empty face
}
```

### 10.9 完整示例

```leno
face Shape {
    func area():float
    func perimeter():float
}

struct Rectangle impl Shape {
    int width = 0
    int height = 0

    func area():float { return width * height }
    func perimeter():float { return 2 * (width + height) }
}

struct Circle impl Shape {
    int radius = 0

    func area():float { return 3.14159 * radius * radius }
    func perimeter():float { return 2 * 3.14159 * radius }
}

func print_shape_info(Shape s) {
    print($"面积: {s.area()}")
    print($"周长: {s.perimeter()}")
}

main() {
    var r = Rectangle()
    r.width = 10
    r.height = 5

    var c = Circle()
    c.radius = 3

    print_shape_info(r)
    print_shape_info(c)
}
```

***

## 11. 总结

| 特性 | 说明 |
|------|------|
| 定义 | `struct 名称 { 类型 字段 = 默认值 }` |
| 创建 | `var obj = StructName()` |
| 访问 | `obj.field`（推荐）或 `obj["field"]` |
| 方法 | 在 struct 内定义，直接访问字段 |
| 嵌套 | 支持，自引用必须设 null 默认值 |
| null 检查 | 编译时警告访问可能为 null 的字段 |
| face | `face 名称 { func 签名 }` 定义接口 |
| impl | `struct 名称 impl Face1, Face2` 显式声明 |
| impl 检查 | 编译期检查方法缺失、返回类型、参数数量 |
| 多态 | face 参数/变量接受任何满足的 struct |
| 类型守卫 | `if obj is FaceName` 运行时检查是否满足 face |
| 安全转型 | `var c = obj as StructName` 匹配返回原值，不匹配返回 null |
| 鸭子类型 | 未声明 impl 但有匹配方法，is 检查也返回 true |
| 跨模块 face | import 模块中的 face 可用于 impl、参数、变量 |
| 性能 | 字段索引编译期确定，O(1) 访问 |
| use 导入 | 支持 `use module.Struct` 和 `use module.Face` 导入 |

> **核心原则**：自引用设 null，访问先检查，参数用具体类型或 face，优先用字段访问！

### face 类型守卫速查表

| 表达式 | 条件 | 结果 |
|--------|------|------|
| `obj is Face` | struct 显式 `impl Face` | `true` |
| `obj is Face` | struct 有 Face 所有方法（鸭子类型） | `true` |
| `obj is Face` | struct 不满足 Face | `false` |
| `obj not is Face` | 上述条件的否定 | 相反结果 |
| `obj as StructName` | obj 是指定 struct | 原值（struct 实例） |
| `obj as StructName` | obj 不是指定 struct | `null` |
| `obj as Face` | obj 实现了指定 face | 原值（struct 实例） |
| `obj as Face` | obj 未实现指定 face | `null` |

***

## 12. 使用 use 导入 struct 和 face

当从模块导入 struct 或 face 类型时，可以使用 `use` 语句将其导入到当前作用域，简化类型声明。

### 12.1 导入 struct 类型

**语法：**
```leno
import "模块路径" as 别名
use 别名.StructName
```

**示例：**
```leno
// math.leno 模块定义
export struct Point {
    int x
    int y
    func test():int { return x + y }
}

export func distance(Point p1, Point p2):int { ... }
```

```leno
// main.leno 使用 use 导入
import "math.leno" as math
use math.Point

main() {
    // 直接使用 Point 类型，无需 math.Point
    Point p1 = math.Point(x = 10, y = 20)
    print(p1.test())  // 调用 struct 方法
    
    // 函数参数使用导入的类型
    Point p2 = math.Point(x = 5, y = 8)
    var dist = math.distance(p1, p2)
}
```

### 12.2 导入 face 类型

**语法：**
```leno
import "模块路径" as 别名
use 别名.FaceName
```

**示例：**
```leno
// shapes.leno 模块定义
export face Shape {
    func area():float
}

export struct Rectangle impl Shape {
    float width
    float height
    func area():float { return width * height }
}

export struct Circle impl Shape {
    float radius
    func area():float { return 3.14159 * radius * radius }
}
```

```leno
// main.leno 使用 use 导入 face
import "shapes.leno" as shapes
use shapes.Shape

// 使用导入的 face 作为参数类型
func printArea(Shape s) {
    print("面积: " + s.area())
}

main() {
    var rect = shapes.Rectangle(width = 5.0, height = 3.0)
    var circle = shapes.Circle(radius = 2.0)
    
    printArea(rect)    // ✅ 输出: 面积: 15.0
    printArea(circle)  // ✅ 输出: 面积: 12.56636
}
```

### 12.3 use 与 import 的区别

| 特性 | `import` | `use` |
|------|----------|-------|
| 作用 | 导入模块，通过模块名访问内容 | 将模块中的 struct/face 导入当前作用域 |
| 语法 | `import "路径" as 别名` | `use 别名.名称` |
| 适用范围 | 模块中的所有导出内容 | 仅 struct 和 face 类型 |
| 访问方式 | `别名.func()`、`别名.Struct()` | 直接使用类型名 `StructName` |

**重要说明：**
- `use` 只能导入 **struct** 和 **face** 类型
- **func**、**var**、**enum** 必须通过模块名访问（如 `math.distance()`）
- struct 实例仍需通过模块构造函数创建（如 `math.Point(x=1, y=2)`）

### 12.4 完整示例

```leno
// geometry.leno - 几何模块
export face Shape {
    func area():float
    func perimeter():float
}

export struct Rectangle impl Shape {
    float width = 0
    float height = 0
    
    func area():float { return width * height }
    func perimeter():float { return 2 * (width + height) }
}

export struct Circle impl Shape {
    float radius = 0
    
    func area():float { return 3.14159 * radius * radius }
    func perimeter():float { return 2 * 3.14159 * radius }
}

export func createRectangle(float w, float h):Rectangle {
    return Rectangle(width = w, height = h)
}
```

```leno
// main.leno - 主程序
import "geometry.leno" as geo
use geo.Shape
use geo.Rectangle

// 使用导入的 face 作为参数
func printShapeInfo(Shape s) {
    print($"面积: {s.area()}")
    print($"周长: {s.perimeter()}")
}

// 使用导入的 struct 作为参数
func scaleRectangle(Rectangle r, float factor):Rectangle {
    return Rectangle(
        width = r.width * factor,
        height = r.height * factor
    )
}

main() {
    // 创建实例（仍需通过模块名）
    var rect = geo.createRectangle(5.0, 3.0)
    var circle = geo.Circle(radius = 2.0)
    
    print("=== Rectangle ===")
    printShapeInfo(rect)
    
    print("=== Circle ===")
    printShapeInfo(circle)
    
    print("=== Scaled Rectangle ===")
    var scaled = scaleRectangle(rect, 2.0)
    printShapeInfo(scaled)
}
```

**输出：**
```
=== Rectangle ===
面积: 15.0
周长: 16.0
=== Circle ===
面积: 12.56636
周长: 12.56636
=== Scaled Rectangle ===
面积: 60.0
周长: 32.0
```

---

## 13. cstruct 线程支持

cstruct 定义可以在多线程环境中安全使用。由于 cstruct 定义是**编译时确定的只读类型元数据**，多个线程可以共享这些定义而不会产生竞争条件。

### 13.1 在线程中使用 cstruct

```leno
import ffi
import threads

cstruct TEST_STRUCT {
    u32 field1
    u32 field2
}

// 在线程中使用 cstruct
func thread_worker(var ch) {
    try {
        var s = TEST_STRUCT.malloc()
        s.field1 = 100
        s.field2 = 200
        ch.send({type: "ok", f1: s.field1, f2: s.field2})
        s.free()
    } catch e {
        ch.send({type: "error", msg: e})
    }
}

main() {
    var ch = threads.channel(1)
    var t = threads.start(thread_worker, ch)
    var result = ch.receive()
    print("field1=" + result.f1 + ", field2=" + result.f2)
    t.join()
}
```

### 13.2 线程安全说明

| 特性 | 说明 |
|------|------|
| cstruct 定义 | 线程安全，只读共享 |
| cstruct 实例 | 每个线程独立，不共享 |
| `malloc()`/`free()` | 每个线程独立管理自己的实例 |
| `from_ptr()` | 可以安全使用，但只在当前线程有效 |

### 13.3 注意事项

1. **不要在线程间传递 cstruct 实例**：实例不能跨线程共享，每个线程应独立 `malloc()`
2. **FFI 资源独立管理**：每个线程加载的 DLL、分配的内存都是独立的
3. **通过 Channel 传递数据**：使用基本类型（int、float、string 等）或 Dict/Array 在线程间传递数据

```leno
// ✅ 正确：每个线程独立使用 cstruct
func worker(var ch) {
    var s = TEST_STRUCT.malloc()  // 子线程独立分配
    s.field1 = 123
    ch.send(s.field1)  // 发送数据，不是实例
    s.free()
}

// ❌ 错误：不要尝试共享实例
func bad_worker(var s, var ch) {
    s.field1 = 123  // 不要传递实例到线程
}
```
