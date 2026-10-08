# 第 7 课：整数安全

> 对应文件：`src/battle_state.cpp:21-43`（`saturating_multiply`）、`:321-336`（`saturating_add`、`scale`）

这三个小函数只有几十行，却是整个伤害计算的地基。

## 1. 为什么 Erlang 程序员要专门学这一课

Erlang 的整数是**大数**：再大都不会溢出，只是占的内存变多。C++ 的 `std::int64_t` 上限是 `9223372036854775807`（约 9.2×10¹⁸），**有符号整数溢出是未定义行为（UB）**。下面的实验说明"未定义"有多可怕。

## 2. 最反直觉的实验：先算后查，被编译器删掉了

```cpp
bool overflowed_after(std::int64_t hp, std::int64_t heal) {
    std::int64_t result = hp + heal;
    return heal > 0 && result < hp;       // 加了正数结果反而变小 → 溢出了
}
```

传入 `hp = int64 最大值`、`heal = 10`（实测）：

```
-O0: overflowed_after = 1      ← 不开优化：检测到了
-O2: overflowed_after = 0      ← 开启优化：没检测到！
```

`-O2` 生成的汇编：

```
_Z16overflowed_afterll:
	xorl	%eax, %eax        ← 返回值设为 0（false）
	ret
```

**整个检查被删掉了，函数永远返回 false。** 标准规定有符号溢出是 UB，编译器因此**有权假设它不会发生**；在这个假设下，"加正数后变小"不可能成立，条件恒为 false。

这就是为什么 Debug（`-O0`）测试一切正常，Release（`-O2`）上线才出问题。

**结论：溢出检查必须在计算之前做，确保这次运算根本不会溢出。算完再查已经来不及了。**

UBSan 可以在运行时抓到：

```bash
g++ -std=c++20 -fsanitize=undefined ...
# runtime error: signed integer overflow: 9223372036854775807 + 10 cannot be represented in type 'long int'
```

开发时建议 `-fsanitize=address,undefined` 一起开（第 11 课）。

## 3. `saturating_add`：加之前先判断

```cpp
std::int64_t saturating_add(std::int64_t left, std::int64_t right) {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();   // 会超上限 → 返回上限
    }
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) {
        return std::numeric_limits<std::int64_t>::min();   // 会低于下限 → 返回下限
    }
    return left + right;                                   // 确定安全，才真正相加
}
```

把"`left + right > max`"改写成"`left > max - right`"：`right > 0` 时 `max - right` 不会溢出；`right < 0` 时 `min - right` 也不会。**判断条件本身也不能溢出**，这是写这类函数的核心技巧。

"saturating"是**饱和**：超出范围时停在边界，不绕回。实测 `saturating_add(max, 1) = 9223372036854775807`。

## 4. `saturating_multiply`：用除法预判乘法

```cpp
if (left == 0 || right == 0) return 0;          // 先排除 0，下面才能放心做除法
if (left > 0) {
    if (right > 0 && left > maximum / right) return maximum;   // 正 × 正 > max ?
    if (right < 0 && right < minimum / left) return minimum;   // 正 × 负 < min ?
} else {
    if (right > 0 && left < minimum / right) return minimum;   // 负 × 正 < min ?
    if (right < 0 && left < maximum / right) return maximum;   // 负 × 负 > max ?
}
return left * right;
```

把"`a × b > max`"改写成"`a > max / b`"。乘积正负取决于两个数的符号，所以分四种情况。实测 `saturating_multiply(4e18, 3) = 9223372036854775807`。

GCC/Clang 有 `__builtin_mul_overflow`，C++26 会有 `std::add_sat` / `std::mul_sat`。项目手写，是为了在 MSVC 和 GCC 上表现完全一致。

## 5. `scale`：万分比乘法，拆开算避免溢出

```
结果 = value × bp / 10000
```

直接写 `value * bp / 10000`，中间结果 `value * bp` 可能溢出，即使最终结果不大。项目写法：

```cpp
std::int64_t scale(std::int64_t value, std::int64_t basis_points) {
    return saturating_add(
        saturating_multiply(value / kBasisPoints, basis_points),                   // 整万部分
        saturating_multiply(value % kBasisPoints, basis_points) / kBasisPoints);   // 零头部分
}
```

数学原理：

```
value = q × 10000 + r         （q = value / 10000，r = value % 10000）
value × bp / 10000 = q × bp  +  r × bp / 10000
```

零头 `r` 永远小于 10000；整万部分**先除后乘**。实测攻击力 1e15、倍率 200%：

```
朴素 attack*bp/10000 (按补码绕回) = 155325592629044      ← 完全错误
scale(attack, bp)                 = 2000000000000000     ← 正确：2e15
```

### 整数除法的取整规则

```
scale(12345, 15000)  = 18517    精确值 18517.5
scale(-15001, 5000)  = -7500    精确值 -7500.5
-7 / 2 = -3     -7 % 2 = -1
```

C++ 整数除法**向零取整**，`%` 与被除数同号。这和 Erlang 的 `div` / `rem` 一致（`-7 div 2` 是 `-3`，`-7 rem 2` 是 `-1`）。Erlang 那边要复现同样计算，用 `div` 和 `rem`，**不要用 `/`**（返回浮点）。

截断规则固定、所有平台一致，不影响确定性。这正是不用浮点数的原因。

## 6. 大转小：先 clamp，再 cast

```cpp
std::int64_t v = 3'000'000'000LL;
static_cast<std::int32_t>(v)                            // -1294967296   ← 30 亿变成了负数
static_cast<std::int32_t>(std::clamp<std::int64_t>(     //  2147483647   ← 停在 int32 上限
    v, INT32_MIN, INT32_MAX))
```

直接 `static_cast` 只保留低 32 位。项目里都是**先 `std::clamp` 限制到目标范围，再转换**，比如 `set_attribute_value`（`battle_state.cpp:61`）和放大 `attack_bp`（`effect_system.cpp:84`）。`std::clamp<std::int64_t>` 显式指定类型，原因和 `std::max` 一样：参数类型必须相同。

## 7. 有符号和无符号混用

```cpp
std::vector<int> events(5);
std::int32_t max_events = -1;
events.size() >= max_events      // 结果是 false！
```

比较时 `-1` 被转换成无符号数 `18446744073709551615`。`-Wextra` 会警告 `comparison of integer expressions of different signedness`。

`emit` 里的写法（`battle_state.cpp:522`）：

```cpp
if (result.events.size() >= static_cast<std::size_t>(request.max_events)) {
```

安全的前提是 `validate_request` 已保证 `max_events` 在 100 到 1000000 之间。显式 `static_cast` 既消除警告，也表明"确认过它是安全的"。

## 8. 三道防线

| 防线 | 位置 | 作用 |
|---|---|---|
| ① 入口校验 | `validate_request` | HP、攻击力 ≤ 1e12，倍率 ≤ 1e6，挡住离谱输入 |
| ② 饱和运算 | `saturating_add`、`saturating_multiply`、`scale` | 多层 Buff 叠加超出预期时停在边界，不变成 UB |
| ③ 窄化保护 | 先 `clamp` 再 `static_cast` | 64 位转 32 位不得到垃圾值 |

另外：`1'000'000'000'000LL` 里的单引号是 C++14 的**数字分隔符**，编译器忽略；`LL` 后缀保证字面量是 64 位，否则某些平台会按 32 位 `int` 处理而溢出。

传输层还有一道：Erlang 可以传来超过 int64 的大整数，`term.cpp` 的 `read_big` 会直接拒绝（第 9 课）。

## 小结

| 概念 | 要点 |
|---|---|
| 有符号溢出 | UB，编译器假设它不会发生，先算后查的代码会被**直接删掉** |
| 检查时机 | **计算之前**判断，判断条件本身也不能溢出 |
| 饱和运算 | 超出范围停在边界 |
| `scale` | 拆成"整万 + 零头"，避免中间结果溢出 |
| 整数除法 | 向零取整，与 Erlang `div` / `rem` 一致 |
| 大转小 | 先 `clamp` 再 `static_cast` |
| 符号混用 | 负数会变成巨大的正数 |
| 工具 | `-fsanitize=address,undefined` |

下一课：[第 8 课：校验与异常](08-validation-exceptions.md)
