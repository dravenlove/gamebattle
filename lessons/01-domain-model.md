# 第 1 课：用 Erlang 的眼光读领域模型

> 对应文件：`include/gamebattle/engine.hpp`
> 练习代码：`lessons/lesson1.cpp`（可选）

`engine.hpp` 只放数据，完全不知道 Erlang 的存在。可以把它当成 `gamebattle.hrl` 里的一堆 record。

## 1. 整个系统的三层

```
Erlang map ─term_to_binary─▶ ETF 字节 ─{packet,4}─▶ port_main.cpp
                                                      │ wire.cpp：ETF → BattleRequest
                                                      ▼
  Erlang ◀─ ETF 字节 ◀─ wire.cpp：BattleResult → ETF ◀─ Engine::simulate()
```

| 层 | 文件 | 职责 |
|---|---|---|
| 领域模型 | `engine.hpp` | 纯数据：请求、单位、技能、Buff、结果 |
| 运行时 | `battle_runtime.hpp` 与 `engine/battle_state/effect_system/target_selector.cpp` | 战斗规则 |
| 传输层 | `term/wire/port_main/nif.cpp` | ETF 编解码，与 Erlang 通信 |

课程按"从内到外"的顺序：先领域模型，再运行时，最后传输层。

## 2. 头文件 ≈ `.hrl`

```cpp
#pragma once                       // 同一个头文件只会被包含一次
#include <cstdint>                 // 尖括号：标准库
#include "gamebattle/engine.hpp"   // 双引号：项目自己的文件
```

`#include` 就是把文件内容原样粘贴进来。所以 `.hpp` 只放声明（结构体定义、函数签名），实现放在 `.cpp` 里。

## 3. namespace ≈ 模块名前缀

- `gamebattle::Engine` 相当于 `gamebattle:simulate` 里的 `gamebattle:` 前缀。
- `namespace gamebattle::runtime { }` 是 C++17 的嵌套写法。
- `.cpp` 里的匿名 `namespace { ... }` 等于 Erlang 模块里**没导出的私有函数**，只在本文件可见。`battle_state.cpp:11` 的 `validate_request` 就放在这里面。

## 4. 定宽整数：Erlang 从不用操心，C++ 必须操心

```cpp
using UnitId = std::uint64_t;       // ≈ -type unit_id() :: non_neg_integer().
using BasisPoints = std::int32_t;   // 10000 = 100%
```

- Erlang 的整数是大数，永远不会溢出。C++ 的 `std::int64_t` 最大只有约 9.2×10¹⁸。
- **有符号整数溢出是未定义行为**，结果不可预测。项目里到处用 `saturating_add` 就是为了防这个（第 7 课）。
- `using` 只是起了个别名，不是新类型。`UnitId` 和 `uint64_t` 可以混着用，编译器不会拦你。

## 5. `enum class` ≈ 原子

```cpp
enum class Side : std::uint8_t { attacker = 0, defender = 1 };
```

- 必须写成 `Side::attacker`，名字不会泄漏到外层。
- **不会自动转成整数**，要打印得先写 `static_cast<int>(side)`。
- `: std::uint8_t` 表示只占 1 字节。
- `EffectKind` 为什么要显式写 `= 0, = 1 …`？这些数字会写进 `.gbcfg` 二进制配置文件（第 10 课）。只要写死了，以后调整枚举的书写顺序也不会改变旧文件的含义。Erlang 的原子按名字比较，没有这个问题。
- 枚举是封闭集合。`switch` 漏写一个分支，`-Wall` 在**编译期**就会警告；Erlang 要等到运行时才报 `case_clause`（第 6 课）。

## 6. struct 加默认值 ≈ record 加默认值

```erlang
-record(stats, {hp = 1, attack = 0, crit_damage_bp = 15000}).
```
```cpp
struct Stats {
    std::int64_t hp{1};
    std::int64_t attack{0};
    BasisPoints crit_damage_bp{15000};
};
```

- **内置类型不写初始值，就是一块随机的垃圾内存。** Erlang 里没有这个概念，这也是 `engine.hpp` 给每个整数都写上 `{0}` 的原因。
- `std::string`、`std::vector` 这类类类型会自动构造成空值，所以 `std::string name;` 不需要写 `{}`。
- 用 `{}` 初始化而不用 `=`，是因为花括号会禁止精度丢失的转换（见第 11 节的坑 1）。

## 7. 容器对照表

| Erlang | C++ | 注意 |
|---|---|---|
| list `[A, B]` | `std::vector<T>` | 连续内存，按下标取值是 O(1)；只能装同一种类型 |
| binary `<<"中毒">>` | `std::string` | **内容可以修改**；本质是字节数组，UTF-8 照样能存 |
| map `#{K => V}` | `std::unordered_map<K, V>` | **遍历顺序不确定** |
| 有序的 map | `std::map<K, V>` | 按 key 排序遍历 |
| `undefined \| V` | `std::optional<T>` | 常用 `has_value()`、`*opt`、`value_or(x)` |
| 引用计数共享的大 binary | `std::shared_ptr<const T>` | 第 2 课 |

对确定性影响最大的一点：`BattleState` 里的 `unit_index` 是 `unordered_map`，但它**只用来按 ID 查找**。凡是遍历单位，走的都是有序的 `std::vector<RuntimeUnit> units`。如果遍历 `unordered_map`，同一个 seed 可能打出不同的战报。

## 8. 前向声明 `struct BuffSpec;`

`engine.hpp:92` 有一行孤零零的 `struct BuffSpec;`。它解决的是两件事：**名字要先见过**，**结构体大小编译期要确定**。

### 先看这三个结构互相引用成什么样

```
Effect                     一个效果，比如"加中毒Buff"
 └─ buff ──────────────┐   指向 Buff 定义
                       ▼
BuffSpec                   Buff 定义，比如"中毒"
 └─ reactions: vector<BuffReaction>
      └─ effects: vector<Effect>   ← 又回到 Effect
```

在 Erlang 里这完全不是问题：

```erlang
-record(effect,    {kind, buff}).        %% buff 字段可以放任何 term
-record(buff_spec, {id, reactions}).
```

记录字段不带类型；所有 term 本质上都是一个统一大小的"槽"，大数据在槽里存的是指向堆的引用。C++ 这两点都不成立。

### 规则一：编译器从上往下读，没见过的名字不认识

把前向声明删掉后编译（实测）：

```
error: ISO C++ forbids declaration of 'type name' with no type
error: template argument 1 is invalid
```

能不能把 `BuffSpec` 挪到 `Effect` 前面？不行。`BuffSpec` 里间接需要 `Effect`，挪到前面就轮到 `Effect` 没见过了。有环的时候，不管怎么调整顺序，总有一方会先被用到。`struct BuffSpec;` 先告诉编译器："`BuffSpec` 是一个结构体，具体内容后面再给。"

### 规则二：结构体按值存放，编译期就要知道它多大

C++ 的结构体成员默认**按值嵌在里面**。假如写成 `BuffSpec buff;`，`Effect` 的大小要包含 `BuffSpec`，而 `BuffSpec` 里又（间接）装着 `Effect`……大小无限递归。编译器拒绝：

```
error: field 'buff' has incomplete type 'BuffSpec'
```

**指针能打破这个循环，因为指针大小是固定的。** 实测：

```
sizeof(BuffSpec*)   = 8    普通指针：一个内存地址
sizeof(shared_ptr)  = 16   对象地址 + 引用计数块地址
sizeof(Effect)      = 24   int(4) + 对齐填充(4) + shared_ptr(16)
```

这其实就是 Erlang 默认的做法：大数据放在堆上，槽里只存引用。C++ 要你**自己决定**是按值嵌入还是存指针。

### 不完整类型：只有声明时能做什么

| 操作 | 能不能做 | 原因 |
|---|---|---|
| `BuffSpec*`、`BuffSpec&`、`shared_ptr<const BuffSpec>` | ✅ | 大小固定，不用知道内容 |
| `BuffSpec buff;` 按值存放 | ❌ | 不知道大小 |
| `buff->id` 访问成员 | ❌ | 不知道有哪些成员（实测报 `invalid use of incomplete type`） |
| `sizeof(BuffSpec)` | ❌ | 不知道大小 |

所以项目里凡是真正读 `BuffSpec` 内容的代码，都写在 `.cpp` 里。到那时 `BuffSpec` 早就是完整类型了。

`engine.hpp:92-120` 的顺序是唯一可行的排法：环上必须有一条边是指针，前向声明放在这条边之前。

```cpp
struct BuffSpec;                  // ① 先声明名字
struct Effect {                   // ② 只用了 BuffSpec 的指针 → 可以
    std::shared_ptr<const BuffSpec> buff;
};
struct BuffReaction {             // ③ 按 vector 使用 Effect → 可以，Effect 已经完整
    std::vector<Effect> effects;
};
struct BuffSpec {                 // ④ 补上完整定义，环在这里闭合
    std::vector<BuffReaction> reactions;
};
```

指针还带来一个好处：**同一个 Buff 定义只存一份。** 100 个技能都挂"中毒"，它们指向同一个 `BuffSpec`。为什么用 `shared_ptr<const BuffSpec>` 而不用普通指针，见第 2 课。

## 9. C++20 指定初始化 ≈ 构造 map

```erlang
#{id => 1001, position => 1, final_stats => #{hp => 1800}}
```
```cpp
UnitConfig{.id = 1001, .position = 1, .final_stats = {.hp = 1800}}
```

**规则：字段必须按声明顺序写。** 中间的字段可以跳过，跳过的就用默认值。

## 10. 读懂 `Engine` 的签名

```cpp
BattleResult simulate(const BattleRequest& request) const;
```

- `const BattleRequest&`：传引用，不复制，而且保证不修改。Erlang 因为数据不可变，传参天然就是这个语义。
- 末尾的 `const`：这个方法不修改 `Engine` 自身。所以 `Engine` 没有状态，多个线程可以共用一个。
- 按值返回 `BattleResult`：写法像是复制，实际上编译器会用移动或返回值优化，不会把整个 events 数组复制一遍。

## 11. 编译器会拦住你的坑（全部实测）

```bash
g++ -std=c++20 -Wall -Wextra -Wpedantic lessons/lesson1.cpp -o lesson1 && ./lesson1
```

1. `std::int32_t max_rounds{50.5};` 报错 `narrowing conversion`。这就是用 `{}` 的好处；换成 `= 50.5` 会被悄悄截断成 50。
2. `std::cout << Side::attacker;` 报错 `no match for 'operator<<'`，因为 enum class 不会自动转成整数。
3. `P{.y = 1, .x = 2}` 报错 `designator order ... does not match declaration order`。
4. 用 `-Wextra` 编译时，指定初始化省略了**没有默认成员初始值**的字段（比如没写 `{}` 的 `std::string`、`std::vector`、`std::optional`），会触发 `missing initializer` 警告；写了默认值的字段（如 `int rounds{0};`）省略了不会警告。练习代码里显式写 `.forced_first_side = std::nullopt` 就是为此。项目自己也有一处：构建时会看到 `battle_state.cpp:361: warning: missing initializer for member 'BattleResult::reason'`。省略的字段照样会被正常初始化，这个警告无害，但你要能读懂它（第 11 课第 4 节）。

## 可选练习

1. 加上 `enum class Trigger`（9 个挂点，照抄 `engine.hpp:26`）和 `struct Passive`，给 `UnitConfig` 加 `std::vector<Passive> passives;`。
2. 写一个 `const char* to_string(Side side)`，用 `switch` 实现；故意删掉一个 case，看 `-Wall` 说什么。
3. 思考：为什么 `hp` 用 `int64`，而 `crit_rate_bp` 用 `int32`？（提示：看 `battle_state.cpp:241-256` 的数值上限。）

## 小结

| C++ | Erlang |
|---|---|
| `.hpp` / `#include` | `.hrl` / `-include` |
| `namespace` | 模块名前缀 |
| 匿名 `namespace {}` | 不导出的函数 |
| `enum class` | 原子（但是封闭集合） |
| `struct` + 默认成员值 | record + 默认值 |
| `std::vector` / `std::unordered_map` / `std::optional` | list / map / `undefined \| V` |
| 指定初始化 `{.id = 1}` | `#{id => 1}` |
| 前向声明 | 不需要 |

下一课：[第 2 课：值、引用、指针、const、move](02-ownership.md)
