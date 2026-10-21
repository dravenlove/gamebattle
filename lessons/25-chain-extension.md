# 第 25 课：在自动战斗上加连锁

**中文** | [English](en/25-chain-extension.md)

> 第七部分「扩展实战」。对应改动：`include/gamebattle/engine.hpp`、`src/battle_runtime.hpp`、`src/effect_system.cpp`、`src/battle_state.cpp`、`src/wire.cpp`、`tools/config_compiler.cpp`、`src/config_store.cpp`、`tests/`。
>
> 前 24 课是读懂一个现成的引擎。这一课反过来：在一个已经在线上运行的引擎上加新玩法。玩法本身很简单，难点在于**怎么加才不会把旧的战斗结果改掉**。

## 1. 目标和约束

**目标**：保持"布阵 → 全自动战斗 → 下一回合"不变，加入游戏王式的连锁：

- 主动技能发动后先不结算，对方可以响应（比如无效它），队友也可以响应（比如先给施法者加攻击力）；
- 响应还可以被再响应；
- 最后加入的最先结算。

**约束**：

| 约束 | 原因 |
|---|---|
| 不用新玩法的请求，结果必须**逐字节不变** | 第 3 课：多消耗或少消耗一次随机数，后面所有判定都会错位；线上的战报回放、反作弊校验都依赖它 |
| 协议向后兼容 | README 的原则：只新增，不改变已有字段的含义 |
| 回合流程不动 | `engine.cpp` 一行都不改 |

完整规则写在根目录 README 的「连锁与响应」一节，这里只讲实现。

## 2. 数据模型：两个触发点、一个效果

```cpp
enum class Trigger : std::uint8_t {
    battle_start, ..., round_end,
    enemy_activate,     // 新增：敌方加入了一个连锁环节
    ally_activate       // 新增：（除自己外的）友方加入了一个连锁环节
};
enum class EffectKind : std::uint8_t {
    ..., direct_damage = 4,
    negate = 5          // 新增：无效这个响应所回应的环节
};

inline constexpr bool is_response_trigger(Trigger trigger) {
    return trigger == Trigger::enemy_activate || trigger == Trigger::ally_activate;
}
```

三个细节：

- **新值一律加在末尾。** 枚举的数字会写进 `.gbcfg`（第 10 课），插在中间会让旧文件里的数字全部错位。
- **`kTriggerCount` 要跟着改。** 它原来是 `static_cast<std::size_t>(Trigger::round_end) + 1`，用来决定 `passives_by_trigger` 数组的大小。不改的话，新触发点的下标会越界：`.at()` 会抛异常，`[]` 就是未定义行为。
- **头文件里的函数要写 `inline`**（第 23 课第 8 题），`constexpr` 函数默认就是 `inline`。

按第 10 课的维护清单，新增一个 `EffectKind` 要同时改 6 个地方。这次全部走了一遍：`engine.hpp`、编译器的 `kEffectKinds`、加载器 `checked_enum` 的最大值、`wire.cpp` 的 `parse_effect_kind`、`effect_system.cpp` 的 `switch`，以及第 6 条格式版本号。版本号最后**没有升**，原因见第 5 节。

## 3. 连锁本体

### 3.1 一个环节记什么

```cpp
struct ChainLink {
    std::size_t source_index{0};               // 发动者（units 的下标）
    std::uint32_t source_id{0};                // 技能或被动 ID
    const std::vector<Effect>* effects{nullptr};
    std::optional<std::size_t> answered_unit;  // 回应的是谁，结算时作为 trigger_unit
    bool negated{false};
};
```

`effects` 是**借用的指针**，指向单位配置里的技能或被动。这样做是安全的，因为第 2 课讲过：战斗中 `units` 不增不删，配置的生命周期一定比连锁长。下标 `source_index` 能长期持有，也是同一个原因。

### 3.2 发动：只有主动技能开启连锁

```cpp
trigger_owner(actor_index, Trigger::on_attack, actor_index, selected->id);
if (selected == &basic || !responses_possible_) {
    execute_effects(...);        // 和以前完全一样
    return;
}
run_chain(actor_index, *selected);
```

普攻不开启连锁。`responses_possible_` 是第 4 节的"零开销开关"。

### 3.3 构建：一次只加一个环节

```cpp
bool EffectSystem::add_response() {
    const auto answered = chain_.back().source_index;
    const auto answered_side = state_.units[answered].side;
    for (const auto side : {other(answered_side), answered_side}) {      // 先对方，后同一方
        const auto trigger = side == answered_side ? Trigger::ally_activate
                                                   : Trigger::enemy_activate;
        for (const auto unit_index : state_.response_order(side)) {       // 速度 → 站位 → ID
            if (已经在连锁里) continue;                                    // 每个单位最多一个环节
            for (被动 : 这个单位挂在 trigger 上的被动) {
                if (!roll(chance)) continue;
                if (超过每回合次数) continue;
                chain_.push_back(...);
                emit("chain", ...);
                return true;                                              // 加了一个就返回，重新询问
            }
        }
    }
    return false;
}
```

调用方是 `while (!state_.event_limit && add_response()) {}`。

- **为什么加一个就返回？** 新环节加入后，"最上面的环节"变了，回应的对象也就变了，必须从头重新询问。
- **为什么每个单位最多一个环节？** 防止两个单位来回无效对方、无限循环。它同时给连锁长度设了上限：不会超过单位总数。
- **`{other(answered_side), answered_side}`** 是第 8 课用过的花括号列表写法，临时构造一个两个元素的列表来遍历。
- **`response_order` 和 `acting_order` 共用一个排序函数 `sort_by_speed`**，区别只是不过滤 `can_act`：宠物、神兵不能行动，但它们的被动可以响应。排序最后一定比较唯一 ID，结果是确定的（第 5 课）。
- **先判定概率，再检查次数**，和普通被动（`trigger_owner_snapshot`）的顺序一致。两边顺序不同的话，同样的配置在两种触发方式下消耗的随机数也会不同，很难排查。

### 3.4 结算：倒序循环

```cpp
for (std::size_t link = chain_.size(); link-- > 0;) {
    const ChainLink current = chain_[link];          // 复制，不用引用
    if (current.negated) continue;
    if (chain_.size() > 1 && !units[current.source_index].alive()) {
        emit("fizzle", ...);                         // 发动者已经死了：失效
        continue;
    }
    resolving_link_ = link;
    execute_effects(..., *current.effects, ..., current.answered_unit);
}
```

**倒序循环的写法**：`link` 是无符号的 `size_t`，最直接的写法是错的（实测）：

```cpp
for (std::size_t link = chain.size() - 1; link >= 0; --link)
// warning: comparison of unsigned expression in '>= 0' is always true [-Wtype-limits]
// 运行结果：link 减到 0 之后再减一，变成 18446744073709551615
```

无符号数永远 `>= 0`，循环停不下来（第 3、7 课）。`link-- > 0` 先比较、后减一：比较时 `link` 是 1，进入循环体时已经是 0，正好处理完下标 0 后退出。这是 C++ 里倒序遍历的标准写法之一，另一种是用反向迭代器 `rbegin()` / `rend()`。

**为什么复制 `current`，而不是用 `auto&`？** 结算过程中，`negate` 会修改 `chain_` 里的其他元素（把下面那个环节标成 `negated`）。这里 `chain_` 不会扩容，引用其实不会失效；但一边持有容器元素的引用、一边修改同一个容器，读代码的人每次都得重新证明一遍它是安全的（第 2、6 课）。`ChainLink` 只有几十字节，复制一份就不用再操心。

### 3.5 `negate`：不选目标的效果

```cpp
for (const auto& effect : effects) {
    if (effect.kind == EffectKind::negate) {
        negate_answered_link(source_index);   // 把 chain_[resolving_link_ - 1] 标成 negated
        continue;                             // 不走选目标的流程
    }
    ...
    switch (executable->kind) {
    ...
    case EffectKind::negate:                  // 走不到这里，但必须写
        break;
    }
}
```

`switch` 里那个走不到的 `case` 必须写：第 6 课讲过，故意不写 `default`，这样漏处理新枚举值时 `-Wswitch` 会报警告。新增 `negate` 后，这个警告正是提醒我们去处理它的地方。

每个响应回应的总是"它加入时最上面的那个环节"，也就是紧挨在它下面的环节，所以"被回应的环节"就是 `resolving_link_ - 1`，不需要另外记录。

### 3.6 为什么不需要递归深度保护

连锁只在 `execute_action` 里开启，响应被动结算时不会再开启新连锁。所以连锁不会嵌套，`chain_` 可以是一个普通的成员变量。环节结算时触发的 `on_hit` 等普通被动仍然走原来的递归，深度 32 的上限照常起作用。

## 4. 零开销开关：怎么保证旧结果不变

```cpp
EffectSystem::EffectSystem(BattleState& state) : state_(state) {
    for (const auto& unit : state_.units) {
        for (const auto trigger : {Trigger::enemy_activate, Trigger::ally_activate}) {
            if (!unit.passives_by_trigger.at(static_cast<std::size_t>(trigger)).empty()) {
                responses_possible_ = true;
            }
        }
    }
}
```

没有任何单位带响应被动时，技能走的是和以前**完全相同的那一行代码**。其实即使没有这个开关，也不会多消耗随机数（没有候选被动就不会判定概率），但开关还省掉了每次发动技能时的排序，而且让"旧请求不受影响"一眼就能看出来。

`fizzle` 的检查也加了 `chain_.size() > 1` 这个条件：只有一个环节（没人响应）时，行为必须和以前一模一样，连"施法者已死就失效"这条新规则都不能生效。

**验证方法**：改代码之前，先用旧引擎跑 2000 场不同 seed 的示例战斗，把每场结果编码成 ETF、算哈希、再合并成一个总哈希；改完后用新引擎再跑一遍。实测：

```
改之前: battles=2000 events=1759731 combined=6d21b37b7457c504
改之后: battles=2000 events=1759731 combined=6d21b37b7457c504
```

175 万个事件逐字节相同。这和第 21 课验证性能优化的方法是一样的：**先证明结果没变，再谈别的。**

## 5. 校验放在三层

| 位置 | 检查什么 | 为什么需要 |
|---|---|---|
| 配置编译器 | 技能和 Buff reaction 不能引用 `negate`；`negate` 只能出现在响应被动里；`decrement_on` 和 reaction 的 `trigger` 不能是响应触发点 | 发布前报错，带文件名和行号（`skills.csv:2: ...`） |
| 加载器 `ConfigStore` | 同样的规则 | 防止有人手工改坏 `.gbcfg` |
| `validate_request` | 同样的规则 | 内联请求不经过编译器；用配置 ID 的请求也会在这里再查一次 |

这是第 8、10 课"尽早报错，运行时兜底"的同一个思路。

**`.gbcfg` 版本号为什么没升？** 文件布局没变，只是枚举多了取值。旧加载器读到新取值时，第 10 课的 `checked_enum` 会明确报错 `contains an unknown enum value`，不会读错；不用新玩法的配置文件和以前逐字节相同，新旧加载器都能读。如果升了版本号，新加载器反而会拒绝所有现存的 2.0 文件。第 10 课的维护清单已经按这个区别更新了。

## 6. 测试：怎么证明新功能是对的

### 6.1 去掉随机性，让每个数字都能算出来

测试里的单位命中率 100%、暴击率 0、所有被动概率 100%。第 3 课讲过，0% 和 100% 的判定不消耗随机数。所以整场战斗没有任何随机性，每个数字都能手算：

- 攻击力 100 的技能，倍率 300%，对防御 0 的目标造成 **300** 点伤害；
- 队友响应、先加 1000 攻击力，同一个技能造成 **3300** 点。

新增的测试覆盖了：无效、反无效（3 → 2 → 1）、队友辅助先结算、施法者被响应打死后失效、非法配置被拒绝、ETF 解析新名字，以及加载器读取编译好的连锁配置并打一场战斗。断言不只检查"有没有某个事件"，还检查事件的先后顺序（`seq`）。

### 6.2 测试本身有没有用：故意改坏

测试全部一次通过时，反而要怀疑它们是不是根本没测到东西。验证办法是**故意改坏代码**，看测试能不能发现（这叫变异测试）：

| 故意改坏 | 结果 |
|---|---|
| `negate` 什么都不做 | 测试失败：`count_events(negated, "damage", 5101) == 0` |
| 改成先进先出结算 | 测试失败：同一条断言 |

两种改法都被抓到了，说明测试确实在检查连锁的核心语义。

### 6.3 反例测试要检查"失败的原因"

CMake 原有的反例测试用的是 `WILL_FAIL`：只要命令失败就算通过。但程序崩溃、参数写错、文件找不到也都是"失败"。新加的两个反例改用 `PASS_REGULAR_EXPRESSION`，要求输出里包含预期的错误信息：

```cmake
set_tests_properties(gamebattle_config_compiler_invalid_negate_skill
    PROPERTIES PASS_REGULAR_EXPRESSION
        "skills.csv:2: negate effects are only valid in response passives")
```

### 6.4 Sanitizer 和模糊测试

- 全部 8 个测试在 ASan + UBSan 下通过。
- 第 22 课的 `term_fuzz` 加了一个带响应被动的种子。先确认这个种子经过 ETF 后真的会产生连锁（50 场里出现了 1557 个连锁环节、500 次无效、1 次失效），再跑 6 万次变异，没有崩溃。

## 7. 下一步可以加什么

| 玩法 | 改哪里 |
|---|---|
| 羁绊 / 联动条件（"X 在场时才生效"） | `Passive` 加一个条件字段，判定概率之前先检查条件；协议、编译器、加载器各加一列 |
| 响应攻击宣言（普攻也能被响应） | 在 `execute_action` 里让普攻也走 `run_chain`；注意这会改变旧结果，需要一个开关或新的引擎版本号 |
| 更多响应效果（无效并破坏、反弹） | 新的 `EffectKind`，照第 2 节的 6 处清单改 |
| 响应链上限、每方轮流 | 只改 `add_response` |
| 玩家手动决定是否响应 | 这就变成了真正的对战卡牌：引擎要能在决策点暂停、等玩家操作，接口从一次性的 `simulate` 变成会话，改动大得多 |

## 小结

| 要点 | 做法 |
|---|---|
| 新玩法不改旧结果 | 不用就零开销；改前改后 2000 场逐字节对比 |
| 新增枚举值 | 加在末尾；同步改 6 个地方；只加取值可以不升文件版本 |
| 连锁 | `vector` 当栈；一次加一个环节；每个单位最多一个；倒序结算 |
| 倒序循环 | `for (size_t i = n; i-- > 0;)`，不要写 `i >= 0` |
| 借用与复制 | 配置用指针借用；结算时复制环节，不持有容器元素的引用 |
| 校验 | 编译器、加载器、运行时三层 |
| 测试 | 去掉随机性算出精确数字；故意改坏来验证测试；反例要检查失败原因 |

回到目录：[课程总览](README.md)
