# LenoC 时间模块 (times)

本文档详细说明 `times` 模块提供的所有时间操作方法。

## 目录

- [使用方式](#使用方式)
- [全局函数](#全局函数)
- [时间戳获取](#时间戳获取)
- [时间格式化](#时间格式化)
- [日期时间操作](#日期时间操作)
- [时间单位对比](#时间单位对比)
- [格式化说明符](#格式化说明符)
- [示例代码](#示例代码)
- [注意事项](#注意事项)

---

## 使用方式

```leno
import times

main() {
    // 获取当前时间戳
    var now = times.now()
    print(now)
    
    // 休眠 1 秒
    sleep(1000)
}
```

---

## 全局函数

### `sleep(ms)`

休眠指定的毫秒数。

**参数**:
- `ms` (int): 休眠时间，单位毫秒

**返回**: `null`

```leno
print("开始")
sleep(1000)     // 休眠 1 秒
print("1 秒后")

sleep(500)      // 休眠 0.5 秒
sleep(2000)     // 休眠 2 秒
```

**注意**: 
- 如果传入负数，会自动转为 0
- 在 Windows 和 Linux/macOS 上都有良好支持

---

## 时间戳获取

### `ms()`

获取当前时间（毫秒级精度）。

**参数**: 无  
**返回**: `int` - 毫秒时间戳

```leno
var t1 = times.ms()
// ... 执行一些操作
var t2 = times.ms()
print("耗时: " + (t2 - t1) + " ms")
```

### `us()`

获取当前时间（微秒级精度）。

**参数**: 无  
**返回**: `float` - 微秒时间戳

```leno
var t1 = times.us()
// ... 执行一些操作  
var t2 = times.us()
print("耗时: " + (t2 - t1) + " us")
```

### `ns()`

获取当前时间（纳秒级精度）。

**参数**: 无  
**返回**: `float` - 纳秒时间戳

```leno
var t1 = times.ns()
// ... 执行一些操作
var t2 = times.ns()
print("耗时: " + (t2 - t1) + " ns")
```

### `now()`

获取当前 Unix 时间戳（秒级精度）。

**参数**: 无  
**返回**: `int` - Unix 时间戳（从 1970-01-01 00:00:00 UTC 开始的秒数）

```leno
var timestamp = times.now()
print(timestamp)  // 例如: 1777650164

// 配合 format 使用
print(times.format(timestamp, "%Y-%m-%d %H:%M:%S"))
```

---

## 时间格式化

### `format(timestamp, fmt)`

将 Unix 时间戳格式化为可读的字符串。

**参数**:
- `timestamp` (int): Unix 时间戳（秒）
- `fmt` (string): 格式字符串

**返回**: `string` - 格式化后的时间字符串

```leno
var now = times.now()

// 标准格式
print(times.format(now, "%Y-%m-%d %H:%M:%S"))  // 2026-05-01 14:30:45

// 日期格式
print(times.format(now, "%Y年%m月%d日"))       // 2026年05月01日

// 时间格式
print(times.format(now, "%H:%M:%S"))           // 14:30:45

// 自定义格式
print(times.format(now, "%Y/%m/%d %I:%M %p"))  // 2026/05/01 02:30 PM
```

---

## 日期时间操作

### `datetime()`

获取当前日期时间的各个组成部分。

**参数**: 无  
**返回**: `array` - 包含 [年, 月, 日, 时, 分, 秒] 的数组

```leno
var dt = times.datetime()
// dt = [2026, 5, 1, 14, 30, 45]

print("年: " + dt[0])   // 2026
print("月: " + dt[1])   // 5
print("日: " + dt[2])   // 1
print("时: " + dt[3])   // 14
print("分: " + dt[4])   // 30
print("秒: " + dt[5])   // 45

// 组合输出
print("现在是 " + dt[0] + "年" + dt[1] + "月" + dt[2] + "日 " + 
      dt[3] + ":" + dt[4] + ":" + dt[5])
```

---

## 时间单位对比

| 方法 | 精度 | 用途 | 示例值 |
|------|------|------|--------|
| `ms()` | 毫秒 | 一般计时 | 1777650164000 |
| `us()` | 微秒 | 高精度计时 | 1777650164123456.0 |
| `ns()` | 纳秒 | 超高精度计时 | 1777650164123456789.0 |
| `now()` | 秒 | Unix 时间戳 | 1777650164 |

**使用建议**:
- 一般性能测试 → 使用 `ms()`
- 高精度性能分析 → 使用 `us()` 或 `ns()`
- 需要存储或传输时间 → 使用 `now()`
- 需要格式化显示 → 使用 `now()` + `format()`

---

## 格式化说明符

| 说明符 | 含义 | 示例 |
|--------|------|------|
| `%Y` | 四位年份 | 2026 |
| `%m` | 月份 (01-12) | 05 |
| `%d` | 日期 (01-31) | 01 |
| `%H` | 小时 (00-23) | 14 |
| `%I` | 小时 (01-12) | 02 |
| `%M` | 分钟 (00-59) | 30 |
| `%S` | 秒 (00-59) | 45 |
| `%p` | AM/PM | PM |
| `%A` | 星期几全称 | Friday |
| `%a` | 星期几简称 | Fri |
| `%B` | 月份全称 | May |
| `%b` | 月份简称 | May |
| `%j` | 一年中的第几天 | 121 |
| `%W` | 一年中的第几周 | 17 |

---

## 示例代码

### 1. 性能测试

```leno
import times

main() {
    var start = times.ms()
    
    // 测试代码
    var sum = 0
    for 1:1000000 to i {
        sum += i
    }
    
    var end = times.ms()
    print("计算耗时: " + (end - start) + " ms")
}
```

### 2. 倒计时程序

```leno
import times

main() {
    print("倒计时开始!")
    
    for 10:1 to i {
        print("还剩 " + i + " 秒...")
        sleep(1000)
    }
    
    print("时间到!")
}
```

### 3. 定时任务

```leno
import times

main() {
    print("任务调度器启动")
    
    var last_run = times.ms()
    var interval = 2000  // 每 2 秒执行一次
    
    for 1:5 to count {
        var now = times.ms()
        var elapsed = now - last_run
        
        if (elapsed < interval) {
            sleep(interval - elapsed)
        }
        
        print("执行任务 #" + count + " (" + times.format(times.now(), "%H:%M:%S") + ")")
        last_run = times.ms()
    }
}
```

### 4. 获取当前日期信息

```leno
import times

main() {
    var dt = times.datetime()
    
    // 判断是否是周末
    var weekday = times.format(times.now(), "%w")  // 0=周日, 1=周一...
    if (weekday == "0" || weekday == "6") {
        print("今天是周末，好好休息!")
    } else {
        print("今天是工作日，加油!")
    }
    
    // 判断上午/下午
    var hour = dt[3]
    if (hour < 12) {
        print("上午好!")
    } else {
        print("下午好!")
    }
}
```

### 5. 格式化时间显示

```leno
import times

main() {
    var now = times.now()
    
    // 不同格式的时间显示
    print("标准格式: " + times.format(now, "%Y-%m-%d %H:%M:%S"))
    print("中文格式: " + times.format(now, "%Y年%m月%d日 %H时%M分%S秒"))
    print("美式格式: " + times.format(now, "%m/%d/%Y %I:%M %p"))
    print("简洁格式: " + times.format(now, "%H:%M"))
    
    // 显示星期
    print("今天是: " + times.format(now, "%A"))
    print("本月是: " + times.format(now, "%B"))
}
```

---

## 注意事项

1. **精度差异**
   - `ms()` 精度为毫秒，适合一般用途
   - `us()` 和 `ns()` 精度更高，但可能受系统时钟影响

2. **时区问题**
   - `now()` 和 `format()` 使用本地时区
   - 如果需要 UTC 时间，目前需要自行转换

3. **sleep 精度**
   - `sleep()` 的实际休眠时间可能略长于指定时间
   - 系统调度和其他因素会导致微小延迟

4. **性能考虑**
   - 频繁调用高精度计时函数 (`us()`, `ns()`) 可能有一定开销
   - 在性能关键代码中，建议减少调用次数

---

*文档版本: 1.0*  
*最后更新: 2026-05-01*
