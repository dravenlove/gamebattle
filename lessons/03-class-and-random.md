# 第 3 课：类与确定性随机数

> 对应文件：`src/battle_runtime.hpp:24-32`（`Random`）、`src/battle_state.cpp:338-368`

## 1. class 和 struct：只差一个默认值

```cpp
struct A { int x; };   // 成员默认 public
class  B { int x; };   // 成员默认 private
```

语法上只有这一点区别。项目的约定是：

- **`struct`**：纯数据，所有字段都能随便读写，相当于 Erlang 的 record。`engine.hpp` 里全是 struct。
- **`class`**：内部有**不变量**需要保护。比如 `Random`：

```cpp
class Random {
public:
    explicit Random(std::uint64_t seed);
    std::uint64_t next();
    bool roll(BasisPoints chance_bp);
private:
    std::uint64_t state_;     // 外部不能直接改
};
```

外部只能通过 `next()` 推进状态，相当于一个 Erlang 模块只导出 API。成员名末尾加 `_` 是项目标记私有成员的习惯。

## 2. 为什么自己写随机数，不用标准库

同一个 seed 必须在 **Windows 开发机（MSVC）** 和 **Linux 生产机（GCC）** 上打出完全相同的战报。

C++ 标准库的坑：随机数引擎（如 `std::mt19937`）的算法是标准规定死的；但"分布"（如 `std::uniform_int_distribution`）**只规定了效果，没规定具体算法**。MSVC 和 GCC 的实现不同，同一个 seed 可能得到不同的数。

所以项目自己写了 **SplitMix64**（Java 的 `SplittableRandom` 用的就是它）：

```cpp
std::uint64_t Random::next() {
    state_ += 0x9e3779b97f4a7c15ULL;                          // ① 状态加一个固定常数
    auto value = state_;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL; // ② 移位异或再乘法，打散比特
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}
```

不用理解常数的来历，只要知道：状态只有一个 64 位整数；全部是加、乘、移位、异或，任何平台结果都一样。

- `ULL` 后缀表示 `unsigned long long`，保证按 64 位无符号处理。
- `30U` 是无符号的 30。
- `^` 是异或（Erlang 的 `bxor`），`>>` 是右移（`bsr`）。

## 3. 无符号溢出是安全的，有符号溢出不是

`state_ += 常数` 反复执行，迟早超过上限。这是**故意的**（实测 `uint64 max + 1 = 0`）：

| | 溢出时 | 项目里的用法 |
|---|---|---|
| 无符号（`uint64_t`） | **有明确定义**：对 2⁶⁴ 取模，自动绕回 | 随机数故意让它绕回 |
| 有符号（`int64_t`） | **未定义行为（UB）** | 伤害、HP 全部用饱和运算（第 7 课） |

## 4. `roll()` 与确定性的真正含义

```cpp
bool Random::roll(BasisPoints chance_bp) {
    if (chance_bp <= 0)            return false;   // 不调用 next()
    if (chance_bp >= kBasisPoints) return true;    // 不调用 next()
    return static_cast<std::int64_t>(next() % kBasisPoints) < chance_bp;
}
```

对 10000 取余得到 0~9999，`chance_bp = 1500` 时落在 0~1499 算命中，正好 15%。

**关键：确定性依赖于 `next()` 被调用的顺序和次数。**

```
seed=42 → 第1个数：判先手平局 → 第2个数：技能A触发？ → 第3个数：命中？ → 第4个数：暴击？ ...
```

- 0% 和 100% 的判定**不消耗随机数**。所以把一个技能配成 100% 触发，不会影响后面所有判定。
- **改代码时加了或删了一次 `roll()`，后面所有判定都会整体错位。** 同一个 seed 在新旧引擎上会打出不同战报。"同 seed 同结果"只在**同一个引擎版本内**成立。线上回放旧战报，需要保留当时的引擎版本，或在战报里记录引擎版本号。

Erlang 的对应做法：把 `rand` 状态显式放进 State 一路传下去，而不是依赖进程字典。`BattleState` 持有唯一一个 `random`，所有模块都从它取数。

## 5. 构造函数初始化列表

```cpp
Random::Random(std::uint64_t seed) : state_(seed) {}
//                                 ^^^^^^^^^^^^^^ 初始化列表
```

冒号后面的部分在进入函数体之前执行，直接用给定的值构造成员。有两类成员**只能**用初始化列表：**引用成员**（必须一出生就绑定）和 **const 成员**。

`BattleState` 的构造函数（`battle_state.cpp:358`）：

```cpp
BattleState::BattleState(const BattleRequest& request_value)
    : request(request_value),                          // 引用：必须在这里绑定
      random(request_value.seed),                      // 调用 Random 的构造函数
      result{.battle_id = request_value.battle_id,     // C++20 指定初始化
             .seed = request_value.seed,
             .source_battle_id = request_value.initial_conditions.source_battle_id} {
    validate_request(request);                         // 函数体：所有成员都已就绪
    add_formation(request.attacker, Side::attacker);
    add_formation(request.defender, Side::defender);
    apply_initial_conditions();
}
```

提醒：**成员按声明顺序初始化**（第 2 课坑 3）。

## 6. 构造函数抛异常：对象要么完整，要么不存在

`validate_request` 发现非法时会 `throw`。在构造函数里抛异常，C++ 保证这个对象**被当作从未存在过**，已构造好的成员自动销毁，调用方拿不到"构造了一半"的对象。所以只要一个 `BattleState` 存在，它的数据就一定通过了校验。

Erlang 里最接近的是 `init/1` 返回 `{stop, Reason}`。

## 7. `explicit`：禁止编译器悄悄替你转换

C++ 的默认规则：**只接收一个参数的构造函数，同时也是一条"自动类型转换规则"。**

```cpp
class Random { public: Random(std::uint64_t seed); };   // 没加 explicit
void simulate(std::uint64_t battle_id, const Random& rng);
```

调用时不小心把两个参数写反了：

```cpp
std::uint64_t battle_id = 3001, seed = 42;
simulate(battle_id, Random(seed));   // 正确写法
simulate(seed, battle_id);           // 手滑写反
```

**不加 `explicit`**：编译通过，`-Wall -Wextra` 也没有警告。运行结果（实测）：

```
battle_id=3001 seed=42
battle_id=42 seed=3001     ← 战斗 ID 和随机种子被悄悄对调了
```

编译器看到第二个参数需要 `Random`，手里有个 `uint64_t`，就自动调用了 `Random(battle_id)`。

**加上 `explicit`**：编译失败，直接指到出错的那一行：

```
error: invalid initialization of reference of type 'const Random&' from expression of type 'uint64_t'
```

项目里所有单参数构造函数都加了 `explicit`：`Random`、`BattleState`、`EffectSystem`、`BattleRunner`。想象 `BattleState` 没加：传一个 `BattleRequest` 给期望 `BattleState&` 的地方，编译器会悄悄构造出一整场新战斗。

**经验法则：单参数构造函数一律加 `explicit`。** Erlang 不会自动转换类型；`explicit` 让 C++ 在这件事上像 Erlang 一样严格。

## 8. 显式构造与隐式构造

区别只有一点：**是你亲手写出了类名，还是编译器替你补上的。**

**显式构造**：代码里能看到类名。

```cpp
Random a(42);                   // 类名 + 圆括号
Random b{42};                   // 类名 + 花括号
auto c = Random(42);
simulate(Random(42));
static_cast<Random>(42);
```

**隐式构造**：只有一个值，编译器发现类型对不上，自动调用构造函数补上。只发生在四个地方：

```cpp
Random d = 42;                  // ① = 初始化
simulate(42);                   // ② 传参
Random make() { return 42; }    // ③ return
Random make() { return {42}; }  // ④ 花括号列表
```

`explicit` 禁止的就是这四种。连花括号也不行（实测）：`converting to 'Random' from initializer list would use explicit constructor`。

### 隐式构造并不都是坏事

项目里大量用到它（实测编译通过）：

```cpp
emit("damage");                                   // const char*   → std::string
trigger_owner(actor_index, ...);                  // size_t        → std::optional<size_t>
trigger_all(Trigger::battle_start, std::nullopt); // nullopt       → optional
apply_buff(..., std::make_shared<BuffSpec>());    // shared_ptr<T> → shared_ptr<const T>
```

**判断标准：转换前后，这个值表达的是不是同一个意思？** 字面量变成字符串、"有一个值"装进"可能有值"、可写变只读，含义都没变，允许隐式。"一个整数变成随机数生成器"含义跳跃太大，要求显式。

### 一处必须手写显式构造的地方

`battle_state.cpp:470-474`：

```cpp
return found == unit_index.end() ? std::nullopt
                                 : std::optional<std::size_t>(found->second);
```

如果写成 `: found->second`，编译失败：

```
error: operands to '?:' have different types 'const std::nullopt_t' and 'std::size_t'
```

三目运算符**先**要求两个分支统一成同一种类型，**然后**才考虑返回值类型。改成 if 语句就不需要，因为每个 `return` 会单独做隐式转换。

### 反方向：对象能不能隐式转成别的类型

```cpp
std::optional<int> found = 5;
if (found) { ... }      // ✅ 在 if/while/! 里允许转成 bool
bool b = found;         // ❌ error: cannot convert 'std::optional<int>' to 'bool'
```

标准库只在"判断有没有值"这种明确场景里放行，防止 `int x = found + 1;` 这种代码悄悄算出错误结果。

| | 显式构造 | 隐式构造 |
|---|---|---|
| 写法 | 能看到类名：`Random(42)` | 只有值：`= 42`、传参、`return 42` |
| 谁决定调用构造函数 | 你 | 编译器 |
| `explicit` 构造函数 | ✅ | ❌ |
| 适合 | 含义变了 | 含义不变 |

## 小结

| 概念 | 一句话 | Erlang 里对应的东西 |
|---|---|---|
| class 和 struct | 只差默认访问权限 | 导出的 API 与内部状态 |
| SplitMix64 | 自己写的随机算法，跨平台一致 | 显式传递 `rand` 状态 |
| 无符号溢出 | 有定义，会绕回；有符号溢出是 UB | 整数无上限 |
| 确定性 | 依赖 `next()` 的调用顺序和次数 | 同上 |
| 初始化列表 | 引用和 const 成员必须在这里初始化 | 无 |
| 构造函数抛异常 | 对象要么完整，要么不存在 | `init/1` 返回 `{stop, Reason}` |
| `explicit` | 禁止单参数构造函数的隐式转换 | Erlang 本来就不隐式转换 |

下一课：[第 4 课：回合主循环](04-battle-loop.md)
