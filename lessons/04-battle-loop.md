# 第 4 课：回合主循环

> 对应文件：`src/engine.cpp`（135 行）

`engine.cpp` 只决定整场战斗的**先后顺序**。具体一次攻击怎么结算，交给第 6 课的 `EffectSystem`。

## 1. README 流程与代码的对应关系

```
README 战斗流程                       engine.cpp
─────────────────────────────────────────────────────────
确定先手方                           :12-39  算先手值、判先手
（一方开局就全灭）                    :43     finish_if_decided("initial_state")
battle_start 被动                     :47     trigger_all(battle_start)
┌ 每回合 ─────────────────────        :52     for (round = 1; ...)
│ round_start 挂点                    :57     trigger_all(round_start)
│ 先手方全部行动 → 后手方全部行动      :62-74  for (side : order) take_side_turn
│ round_end 挂点、Buff 过期            :80     trigger_all(round_end)
└ 判断胜负                            每一步之后都会 finish_if_decided
打满回合数 → 平局                     :86-92
```

## 2. 两种写循环的思路

Erlang 里，"循环"就是递归，状态作为参数传下去；要结束循环，**不再递归**就行：

```erlang
round_loop(Round, S) when Round > S#state.max_rounds ->
    finish(draw(max_rounds, S));
round_loop(Round, S0) ->
    S1 = trigger_all(round_start, S0#state{round = Round}),
    case finish_if_decided(S1, round_start) of
        {true, S}   -> finish(S);                      %% 不再递归 = 跳出循环
        {false, S2} -> round_loop(Round + 1, run_sides(S2))
    end.
```

C++ 是另一种思路：**状态只有一份（`state_`），在循环里原地修改**，用 `break` / `continue` / `return` 控制何时离开：

```cpp
for (state_.round = 1;
     state_.round <= state_.request.max_rounds && !state_.event_limit;
     ++state_.round) {
    state_.phase = "round_start";
    effects_.trigger_all(Trigger::round_start, std::nullopt);
    if (state_.finish_if_decided("round_start")) {
        break;
    }
    ...
}
```

Erlang 的 `S0 → S1 → S2` 每一步都是新值；C++ 的 `state_` 从头到尾只有一个，任何一次函数调用都可能改掉它。这决定了一条贯穿全项目的规则：**每做完一步，都要重新检查状态。**

## 3. for 循环的三部分，以及循环结束后的值

```cpp
for (初始化; 条件; 每轮结束后执行) { 循环体 }
```

执行顺序：初始化一次 → 检查条件 → 循环体 → `++round` → 再检查条件 → ……

循环变量是**成员 `state_.round`**，这样 `emit` 能直接读到当前回合。副作用是：循环正常跑完时，最后一次 `++` 会超过上限（实测 `max_rounds = 50` 时循环后 `round = 51`）。所以 `engine.cpp:91` 写的是：

```cpp
state_.result.rounds = std::min(state_.round, state_.request.max_rounds);
```

## 4. `break` 只跳出最内层的循环

```cpp
for (round ...) {                                  // 外层：回合
    for (const Side side : order) {                // 内层：先手方、后手方
        take_side_turn(side);
        if (state_.finish_if_decided("all_units_defeated")) {
            break;                                 // ← 只跳出内层！
        }
    }
    if (state_.decided || state_.event_limit) {    // ← 所以这里要再检查一次
        break;
    }
    ...round_end...
}
```

实测：

```
round 1 side 0
round 1 side 1
round 2 side 0
  -> break           ← 只跳过了第 2 回合的 side 1
round 3 side 0       ← 外层循环照常进入第 3 回合！
round 3 side 1
```

如果漏写第 75 行的检查，分出胜负后还会继续执行 `round_end`，甚至进入下一回合。C++ 没有"一次跳出多层"的语法，常见做法有两种，这个文件都用到了：
- **标志位**：`decided`、`event_limit`。
- **把内层逻辑抽成函数，用 `return` 退出**：`take_side_turn`。

## 5. 提前 `return`：先处理掉提前结束的情况

```cpp
if (state_.finish_if_decided("initial_state")) {
    return state_.finish();
}
effects_.trigger_all(Trigger::battle_start, std::nullopt);
if (state_.finish_if_decided("battle_start")) {
    return state_.finish();
}
```

这叫**守卫语句**：主流程不用一层层缩进。Erlang 里通常要写成嵌套的 `case`。

`finish_if_decided` 是"检查并顺手记录"：没分出胜负返回 `false`、不改任何东西；分出了就写入 `decided`、`winner`、`reason`、`rounds` 并返回 `true`。传入的字符串会原样出现在结果的 `reason` 字段里。

## 6. 先手判定：optional、三目运算符和优先级陷阱

```cpp
if (state_.request.initial_conditions.forced_first_side.has_value()) {
    state_.first_side = *state_.request.initial_conditions.forced_first_side;
} else if (attacker_initiative == defender_initiative) {
    state_.first_side = (state_.random.next() & 1U) == 0 ? Side::attacker : Side::defender;
} else {
    state_.first_side = attacker_initiative > defender_initiative ? Side::attacker : Side::defender;
}
```

- `has_value()` 判断有没有值，`*` 取出值。
- `条件 ? A : B` 是三目运算符，是**表达式**，可以放在赋值号右边。
- `next() & 1U` 取最低位，相当于抛硬币。只有先手值相同时才消耗随机数。

**优先级陷阱**：`&` 的优先级**低于** `==`。

```cpp
bool bad = r & 1U == 0;       // 实际被解析成 r & (1U == 0)，永远是 0
```

实测 `r = 6`：`good=1 bad=0`。`-Wall` 会警告 `suggest parentheses around comparison in operand of '&'`。**位运算和比较写在一起，一律加括号。**

## 7. `std::array` 和范围 for

```cpp
const std::array<Side, 2> order{state_.first_side, other(state_.first_side)};
for (const Side side : order) { ... }
```

- `std::array<T, N>`：长度编译期固定，放在栈上，不需要堆内存。
- `for (元素 : 容器)` 相当于 `lists:foreach`。
- `const Side side` 按值取：`Side` 只有 1 字节；大对象写 `const auto&`。

## 8. `take_side_turn`：先取快照，每一步后重新检查

```cpp
void BattleRunner::take_side_turn(Side side) {
    const auto order = state_.acting_order(side);        // ① 行动顺序的快照
    for (const auto actor_index : order) {
        if (state_.event_limit || state_.side_defeated(other(side))) {
            return;                                      // ② 对面全灭：整个阶段结束
        }
        auto& actor = state_.units[actor_index];
        if (!actor.alive()) {
            continue;                                    // ③ 这个人已经死了：跳过
        }
        effects_.trigger_owner(actor_index, Trigger::before_action, actor_index, 0);
        if (!actor.alive()) {                            // ④ 行动前的被动可能把自己弄死
            continue;
        }
        effects_.execute_action(actor_index);
        if (actor.alive()) {                             // ⑤ 反伤可能已经杀死了行动者
            effects_.trigger_owner(actor_index, Trigger::after_action, actor_index, 0);
        }
    }
}
```

- **① 快照**：Buff 可能在行动途中改变速度，如果每步重新排序，同一个人可能行动两次或被跳过。Erlang 的列表天然不可变；C++ 要**主动复制一份**。
- **② `return` 与 ③ `continue`**：`continue` 跳过当前单位；`return` 结束整个函数。
- **④⑤ 反复检查 `alive()`**：每经过一次可能改变状态的调用，都要重新确认前提。

`auto& actor` 能跨越这些调用一直使用，前提是"战斗中 `units` 不增不删"（第 2 课第 4 节）。

## 9. 对外只暴露一个入口

```cpp
namespace gamebattle {
BattleResult Engine::simulate(const BattleRequest& request) const {
    return runtime::BattleRunner(request).run();
}
}
```

- 内部实现在 `gamebattle::runtime`，最后重新打开 `gamebattle` 实现对外的 `Engine`。Port 和 NIF 只认识 `Engine`。
- **一场战斗对应一个 `BattleRunner`，用完即扔**，不跨战斗保留状态。相当于每场战斗起一个临时进程，打完就退出。

## 小结

| 概念 | 要点 |
|---|---|
| 状态的变化方式 | Erlang 传新值，C++ 原地修改唯一的 `state_`，每一步后都要重新检查 |
| for 循环变量 | 正常结束时多加一次，用 `std::min` 修正 |
| `break` | 只跳出最内层，外层靠标志位 |
| `continue` / `return` | 跳过当前元素 / 结束整个函数 |
| 提前 `return` | 主流程保持平铺 |
| `&` 和 `==` | `&` 优先级更低，必须加括号 |
| 快照 | 先复制行动顺序，遍历中不受状态变化影响 |

下一课：[第 5 课：选目标](05-target-selector.md)
