# 第 5 课：选目标

**中文** | [English](en/05-target-selector.md)

> 对应文件：`src/target_selector.cpp`（83 行）

这个文件只做一件事：**给定规则，返回一组目标的下标**。它不扣血也不加 Buff，只负责"选谁"。这一课把 lambda 和 `std::sort` 讲透，它们在整个项目里到处都是。

## 1. `static` 成员函数

```cpp
class TargetSelector {
public:
    static std::vector<std::size_t> select(
        BattleState& state,
        std::size_t owner_index,
        TargetRule rule,
        std::int32_t requested_count,
        std::optional<std::size_t> trigger_unit);
};
```

`static` 表示这个函数**不属于任何对象**，直接写 `TargetSelector::select(...)`。完全等同于 Erlang 的 `target_selector:select(State, Owner, Rule, Count, Trigger)`。

返回值是**下标的列表**，原因见第 2 课第 4 节。

## 2. 两个特殊规则：直接返回

```cpp
if (rule == TargetRule::self) {
    return state.units[owner_index].alive()
               ? std::vector<std::size_t>{owner_index}   // 只有自己
               : std::vector<std::size_t>{};             // 空列表
}
if (rule == TargetRule::trigger_unit) {
    if (trigger_unit.has_value() && *trigger_unit < state.units.size() && ...) {
        return {*trigger_unit};                          // 直接写 {}
    }
    return {};
}
```

- `return {*trigger_unit};` / `return {};` 是**隐式构造**：编译器知道返回类型是 `vector<size_t>`。
- 三目运算符里**必须显式写出类型**，因为 `?:` 要求两个分支先统一类型，而光秃秃的 `{}` 没有类型。实测 `return alive ? {i} : {};` 报 `expected primary-expression before '{' token`。（第 3 课第 8 节）

## 3. 筛选候选人

```cpp
const Side target_side =
    rule == TargetRule::ally_lowest_hp || rule == TargetRule::all_allies
        ? state.units[owner_index].side
        : other(state.units[owner_index].side);

std::vector<std::size_t> candidates;
for (std::size_t index = 0; index < state.units.size(); ++index) {
    if (state.units[index].side == target_side && state.units[index].alive() &&
        state.units[index].config.targetable) {
        candidates.push_back(index);
    }
}
```

等同于 Erlang 的列表推导：

```erlang
Candidates = [I || {I, U} <- Indexed, U#unit.side =:= TargetSide, alive(U), U#unit.targetable].
```

C++ 没有列表推导，就用"空 vector + 循环 `push_back`"。

## 4. lambda：C++ 的匿名函数

```cpp
std::sort(candidates.begin(), candidates.end(),
          [&](std::size_t left, std::size_t right) {
              if (state.units[left].config.position != state.units[right].config.position) {
                  return state.units[left].config.position < state.units[right].config.position;
              }
              return state.units[left].config.id < state.units[right].config.id;
          });
```

```
[捕获列表](参数列表) -> 返回类型 { 函数体 }      返回类型通常省略
```

对应 Erlang 的 `fun(Left, Right) -> ... end`。

**关键区别在捕获列表。** Erlang 的 fun 自动把外部变量**复制**进来。C++ 要你自己决定：

| 写法 | 含义 |
|---|---|
| `[]` | 不捕获 |
| `[x]` | **复制** `x` |
| `[&x]` | **引用** `x` |
| `[=]` | 用到的都复制 |
| `[&]` | 用到的都引用 |
| `[this]` | 捕获当前对象 |

实测：

```cpp
int round = 1;
auto by_value = [round]  { return round; };
auto by_ref   = [&round] { return round; };
round = 5;
// by_value=1 by_ref=5
```

项目几乎都用 `[&]`，因为这些 lambda **当场用完就扔**。危险在于 lambda 被存起来、比它引用的变量活得更久，那时就是悬空引用。

**经验法则：lambda 当场用完，用 `[&]`；要被带走，用值捕获。** `effect_system.cpp:12` 的 `[instance_id]` 只复制了一个整数，读代码的人一眼就知道它依赖什么。

## 5. 最低血量排序：lambda 里套 lambda，全部用整数

```cpp
std::sort(candidates.begin(), candidates.end(),
          [&](std::size_t left, std::size_t right) {
              const auto& lhs = state.units[left];
              const auto& rhs = state.units[right];
              const auto ratio = [](std::int64_t hp, std::int64_t maximum) {
                  return (hp / maximum) * kBasisPoints +
                         ((hp % maximum) * kBasisPoints) / maximum;
              };
              const auto lhs_ratio = ratio(lhs.hp, lhs.config.final_stats.hp);
              const auto rhs_ratio = ratio(rhs.hp, rhs.config.final_stats.hp);
              if (lhs_ratio != rhs_ratio) return lhs_ratio < rhs_ratio;              // ① 血量百分比
              if (lhs.config.position != rhs.config.position)
                  return lhs.config.position < rhs.config.position;                  // ② 站位
              return lhs.config.id < rhs.config.id;                                  // ③ ID
          });
```

- **lambda 可以存进变量**，之后像普通函数一样调用。
- **比较的是百分比**：10000 血剩 3000（30%）比 1000 血剩 800（80%）更残。
- **不用浮点数**：浮点运算在不同 CPU、编译器、优化级别下最后几位可能不同，两个单位百分比很接近时，Windows 和 Linux 可能选出不同目标。
- **拆成 `/` 和 `%`**：避免 `hp * 10000` 这种中间结果溢出（第 7 课）。

## 6. `std::sort` 的比较函数：最危险的一块

### 规则一：必须用 `<`，绝对不能用 `<=`

比较函数必须满足**严格弱序**，最基本的一条：`compare(a, a)` 必须返回 `false`。用 `<=` 时相等元素会互相"排在对方前面"，`std::sort` 内部为了速度省掉了边界检查，直接越界。实测 100 个相同元素：

```
ERROR: AddressSanitizer: heap-buffer-overflow
```

标准库调试模式（`-D_GLIBCXX_DEBUG`）直接指出原因：

```
Error: comparison doesn't meet irreflexive requirements, assert(!(a < a)).
```

**Erlang 的 `lists:sort/2` 约定的恰好是 `=<`，C++ 必须是严格的 `<`。** 只有出现多个相等元素时才会触发，平时测试不一定测得出来。

### 规则二：最后必须能分出先后，否则结果不确定

`std::sort` **不稳定**：比较结果相等的元素，排序后谁在前没有保证。实测 40 个单位按站位（只有 0 和 1）排序：

```
sort       : 1000 1026 1024 1028 1022 1020   ← 相等元素被打乱了
stable_sort: 1000 1002 1004 1006 1008 1010   ← 保留原来的顺序
```

更麻烦的是，MSVC 和 GCC 打乱的方式不同。所以**项目里所有比较函数的最后一句都是比较 `id`**：ID 全局唯一，任何两个单位最终都能分出先后，结果只有唯一一种。`acting_order`（`battle_state.cpp:493`）也是：速度 → 站位 → ID。

技能排序用的是 `std::stable_sort`（`battle_state.cpp:395`），因为技能没有唯一的兜底字段，用稳定排序保持配置表里的原始顺序。**要么有唯一的兜底字段，要么用稳定排序。**

## 7. 截取前 N 个

```cpp
if (rule != TargetRule::all_enemies && rule != TargetRule::all_allies) {
    const auto count = static_cast<std::size_t>(std::max(0, requested_count));
    if (candidates.size() > count) {
        candidates.resize(count);          // 只保留前 count 个
    }
}
return candidates;
```

- `resize(n)` 相当于 `lists:sublist(L, N)`。
- **`std::max` 要求两个参数类型完全相同**。`0` 和 `requested_count` 都是 `int` 所以能编译；`int64_t` 配 `int32_t` 会报 `no matching function for call to 'max(int64_t&, int32_t&)'`。解决办法是 `std::max<std::int64_t>(a, b)`。
- `size_t` 是无符号，负数转过去会变成巨大的正数，所以先 `std::max(0, ...)` 再转换。
- `return candidates;` 返回局部变量，自动移动，不用写 `std::move`。

## 小结

| 概念 | 要点 |
|---|---|
| `static` 成员函数 | 不需要对象，相当于 Erlang 的模块函数 |
| lambda | `[捕获](参数) { 函数体 }`，相当于 `fun` |
| 捕获方式 | `[&]` 当场用完；`[x]` 被带走 |
| 比较函数 | **必须用 `<`**，与 Erlang 的约定相反 |
| 确定性 | 最后比较唯一 ID，或用 `stable_sort` |
| 整数万分比 | 不用浮点，避免平台差异 |
| `std::max` | 两个参数类型必须相同 |

下一课：[第 6 课：效果系统](06-effect-system.md)
