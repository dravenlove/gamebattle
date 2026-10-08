# 第 6 课：效果系统

> 对应文件：`src/effect_system.cpp`（455 行，全项目最复杂）

它负责"一个效果真正落地时发生的所有事"：扣血、加血、挂 Buff、移除 Buff，以及由此引发的被动和 Buff 反应的**连锁触发**。

## 0. 整体：一次攻击怎样引发连锁

```
execute_action（单位行动）
 ├─ 按概率选技能，都没触发就普攻
 ├─ trigger_owner(on_attack)          → 攻击者的"攻击时"被动
 └─ execute_effects(技能的效果列表)
      └─ 对每个目标，按效果类型分派：
           apply_damage ─┬─ trigger_owner(on_hit)      → 攻击者"命中时"被动 ──┐
                         ├─ trigger_owner(on_damaged)  → 受击者"受击时"被动 ──┤
                         └─ trigger_all(unit_death)    → 所有人"有人死亡"被动 ┤
           apply_heal / apply_buff / remove_buff                             │
                                                                             ▼
      trigger_owner_snapshot：执行被动和 Buff 反应 → execute_effects(……) → 又回到上面
```

这是**递归**结构，理论上可以无限循环（"受击反击"遇上"反击也会被反击"）。两道保险：

- **深度上限**：每递归一层 `depth + 1`，超过 `kMaxTriggerDepth = 32` 直接 return。
- **事件上限**：事件数达到 `max_events` 时 `event_limit = true`，几乎每个函数开头都会检查。

C++ 的调用栈只有几 MB，递归太深会栈溢出崩溃，必须主动设上限。

## 1. `execute_action`：用指针在两个来源中选一个

```cpp
const Skill* selected = nullptr;
for (const auto& skill : state_.units[actor_index].config.skills) {
    if (state_.random.roll(skill.chance_bp)) {
        selected = &skill;          // 指向配置里的某个技能
        break;
    }
}

Skill basic;                        // 局部对象：普攻
if (selected == nullptr) {
    basic.name = "basic_attack";
    basic.effects.push_back(Effect{});
    selected = &basic;              // 改为指向局部的普攻
}
```

- 技能已按优先级排好序，第一个通过概率判定的就是本次释放的技能。
- `selected` 是**借用指针**，之后统一用 `selected->effects`。
- 指向局部变量安全：`basic` 活到函数结束，`selected` 只在函数里用。
- `Effect{}` 是全默认值的效果：`damage`、`enemy_front`、100% 攻击力。所以普攻就是"打前排一下"。

## 2. `execute_effects`：按需复制，然后分派

```cpp
for (const auto& effect : effects) {
    Effect scaled_effect;
    const Effect* executable = &effect;           // 默认直接用原效果，不复制
    if (magnitude_stacks > 1 && (伤害/治疗/直接伤害)) {
        scaled_effect = effect;                   // 需要按层数放大时，才复制一份
        scaled_effect.flat = saturating_multiply(effect.flat, magnitude_stacks);
        executable = &scaled_effect;
    }
```

对应 Buff 的 `per_stack`：3 层中毒伤害是 1 层的 3 倍。配置是 `const` 的，要先复制再改；不需要放大就直接借用。

### `switch` 分派

```cpp
switch (executable->kind) {
case EffectKind::damage:
case EffectKind::direct_damage:        // 两个 case 叠在一起：共用同一段代码
    apply_damage(...);
    break;                             // 一定要写 break
case EffectKind::heal:
    apply_heal(...);
    break;
...
}
```

- **不写 `break` 会"掉"进下一个 case**（fall-through）。这里正是利用它让两种伤害共用代码；但忘写 `break` 就是隐蔽 bug。
- **故意不写 `default`**：以后新增一种 `EffectKind` 却忘了处理时，`-Wall` 会警告（实测）：

```
warning: enumeration value 'remove_buff' not handled in switch [-Wswitch]
```

写了 `default` 反而会让这个警告消失。Erlang 要等运行时报 `case_clause`；C++ 可以在编译期查出来，前提是**不写 default**。

## 3. `apply_damage`：伤害公式，然后引发连锁

```cpp
auto damage = saturating_add(scale(actor_stats.attack, effect.attack_bp), effect.flat);  // 攻击力×倍率+固定值
if (!direct) {
    命中判定：roll(命中率 - 闪避率)，没命中 → emit("miss") → return
    damage = max(1, damage - 防御)
    damage = max(1, damage × (1 + 增伤))
    damage = max(1, damage × (1 - 减伤))
    暴击判定：roll(暴击率)，暴击 → damage × 暴击伤害
} else {
    direct_damage：跳过以上所有步骤，至少 1 点
}
damage = std::min(damage, target.hp);     // 不能扣成负数
target.hp -= damage;
```

扣血后引发三种触发：

```cpp
if (!direct) trigger_owner(actor_index,  Trigger::on_hit,      target_index, ...);
             trigger_owner(target_index, Trigger::on_damaged,  actor_index,  ...);
if (!target.alive()) trigger_all(Trigger::unit_death, target_index, ...);
```

第三个参数"事件相关单位"会成为目标规则里的 `trigger_unit`：`on_hit` 传受击者，`on_damaged` 传攻击者。所以"受击时反击"配置成 `target = trigger_unit` 就能打回攻击者。

## 4. 迭代器：Erlang 里没有的概念

迭代器是**指向容器中某个位置的"光标"**：

```
buffs:     [ 801 ][ 802 ][ 803 ]
             ↑                   ↑
         begin()              end()     ← "最后一个元素的下一个位置"，不是任何元素
```

- 区间是"左闭右开" `[begin, end)`。
- `*it` 取元素，`it->字段` 访问成员，`++it` 前进。
- **`end()` 不能解引用**，只表示"到头了"或"没找到"。

最接近的 Erlang 概念是遍历 `[H | T]` 时的当前位置，`end()` 相当于走到 `[]`。

## 5. `apply_buff`：查找、新增或叠层

```cpp
auto iterator = std::find_if(
    target.buffs.begin(), target.buffs.end(),
    [&](const ActiveBuff& active) {
        return active.definition != nullptr && active.definition->id == definition->id;
    });
```

`find_if` 找不到时返回 `end()`，相当于 `lists:search/2` 返回 `false`。

```cpp
if (iterator == target.buffs.end()) {             // 没找到：新挂一个
    ActiveBuff active;
    active.definition = std::move(definition);
    active.instance_id = state_.next_buff_instance_id++;
    target.buffs.push_back(std::move(active));
    iterator = std::prev(target.buffs.end());      // ← 注意这一行
} else {                                          // 找到了：叠层或刷新
    ...
}
```

**为什么 `push_back` 之后要重新给 `iterator` 赋值？** 原来的 `iterator` 等于 `end()`；更重要的是 **`push_back` 可能扩容，扩容后所有旧迭代器都失效**。`std::prev(end())` 就是刚加进去的那个。

**规则：修改容器之后，之前拿到的迭代器、指针、引用都可能失效，要重新获取。**

### case 里声明变量要加花括号

```cpp
case RefreshPolicy::extend: {                      // ← 必须
    const auto extended = saturating_add(iterator->remaining, spec.lifetime.duration);
    iterator->remaining = ...;
    break;
}
```

不加会报 `error: jump to case label`。整个 `switch` 共用一个作用域，跳到 `keep` 分支会**跳过 `extended` 的初始化**，而这个名字在那里依然可见。花括号把它限制在自己的分支里。

## 6. `remove_buff`：erase-remove 惯用法

```cpp
target.buffs.erase(
    std::remove_if(target.buffs.begin(), target.buffs.end(),
                   [&](const ActiveBuff& active) { return active.definition->id == buff_id; }),
    target.buffs.end());
```

**`std::remove_if` 并不会真正删除任何元素。** 实测：

```
原始：   [801, 802, 801, 803]
remove_if 后 size=4  有效部分长度=2       ← 容器大小没变！
erase 后 size=2  内容=802 803
```

`remove_if` 把要保留的元素挪到前面，返回"新的逻辑终点"；算法只拿到迭代器，**无法改变容器大小**，真正的删除只能靠容器自己的 `erase`。C++20 可以写 `std::erase_if(target.buffs, 条件);`。

对应 `lists:filter/2`。

## 7. `trigger_owner_snapshot`：核心中的核心

### 阶段 A：被动

```cpp
for (const auto passive_index : owner.passives_by_trigger.at(trigger_index)) {
    const auto& passive = owner.config.passives[passive_index];
    if (!state_.random.roll(passive.chance_bp)) continue;
    auto& count = owner.passive_triggers[passive.id];         // ← 注意 []
    if (passive.max_triggers_per_round > 0 && count >= passive.max_triggers_per_round) continue;
    ++count;
    execute_effects(owner_index, owner_index, passive.effects, passive.id, depth + 1, event_unit);
}
```

- `passives_by_trigger` 是构造时按触发时机预先分组的数组，不用每次检查所有被动。
- **`unordered_map` 的 `[]` 在 key 不存在时会自动插入默认值**（实测）：

```
size=0 -> 访问 [701] 后 size=1 count=0 -> ++ 后 map[701]=1
```

  这里正好利用它：第一次自动插入 0，再 `++`。相当于 `maps:update_with(Id, fun(C) -> C + 1 end, 1, Map)`。**但只读查询别用 `[]`**，会凭空插入记录，要用 `find()`；`[]` 也不能用在 `const` map 上。

### 阶段 B：Buff 反应（先收集，再执行）

```cpp
struct PendingReaction {
    std::uint64_t instance_id;
    std::size_t reaction_index;
};
std::vector<PendingReaction> pending;
for (const auto& active : owner.buffs) {                        // 第一步：只读遍历，收集
    if (active.instance_id > buff_instance_cutoff) continue;    // 本次触发开始后才挂上的，不参与
    ...pending.push_back({active.instance_id, index});
}

for (const auto& item : pending) {                              // 第二步：逐个执行
    auto active = find_buff_instance(owner, item.instance_id);  // 每次按 ID 重新查找
    if (active == owner.buffs.end()) continue;                  // 已经被删了，跳过
    auto definition = active->definition;                       // 复制一份 shared_ptr
    const auto& reaction = definition->reactions[item.reaction_index];
    ...
    execute_effects(...);                                       // 这一步可能修改 owner.buffs！
}
```

**为什么不一边遍历 `owner.buffs` 一边执行？** 执行反应时可能 `add_buff`（扩容）或 `remove_buff`（元素前移），**容器在遍历过程中被修改**。所以分两步：先只读遍历，记下唯一 ID；再逐个按 ID 查找执行。Erlang 的列表天然是快照，**C++ 要你主动造一份快照**。

**`buff_instance_cutoff`**：触发开始时已存在的最大 instance_id。触发过程中新挂上的 Buff ID 更大，被跳过。这实现了设计文档的规则："当前事件开始后新添加的 Buff 不参加本次反应"，否则回合结束时挂上的中毒会立刻结算一跳。

**`auto definition = active->definition;` 为什么必须复制？** `execute_effects` 可能通过 `remove_buff` 把这个 Buff 本身删掉：`active` 迭代器失效；如果它是 `BuffSpec` 的最后一个持有者，`BuffSpec` 也被释放；而 `reaction` 引用指向的正是 `BuffSpec` 内部。局部的 `definition` 保证这次循环中 `BuffSpec` 一定活着。

最后调用时用到了 `active->stacks`：它安全，因为**函数参数在函数被调用之前就已经算好了**，此时 `active` 仍然有效；执行完之后函数再没碰过 `active`。

### 阶段 C：Buff 过期（一边遍历一边删除的正确写法）

```cpp
auto active = owner.buffs.begin();
while (active != owner.buffs.end()) {
    if (需要递减) --active->remaining;
    if (需要递减 && active->remaining <= 0) {
        emit("buff_expire");
        active = owner.buffs.erase(active);    // ← erase 返回下一个有效位置
    } else {
        ++active;                               // ← 没删除时才前进
    }
}
```

**错误写法**：

```cpp
for (auto it = buffs.begin(); it != buffs.end(); ++it) {
    if (--it->remaining <= 0) buffs.erase(it);     // erase 之后 it 失效，for 还会 ++it
}
```

实测：

```
ERROR: AddressSanitizer: heap-buffer-overflow
Error: attempt to increment a singular iterator.
```

删除了用 `it = erase(it)`，没删除 `++it`，二选一，所以要用 `while` 自己控制前进。这是 C++ 必须背下来的固定写法。

## 8. 小工具：`value_or`

```cpp
effect_source_index = state_.find_unit(active->source).value_or(owner_index);
```

有值取值，没有值用默认值，对应 `maps:get(Key, Map, Default)`。`applier` 类型的反应用施加者的属性计算，施加者找不到就退回持有者。

## 小结

| 概念 | 要点 | Erlang |
|---|---|---|
| 递归触发 | 深度 32 + 事件上限两道保险 | 递归不太担心栈 |
| 指针二选一 | 同一个变量指向配置或局部对象 | 变量绑定 |
| `switch` | 叠写 case 共用代码；不写 `default` 才有漏分支警告 | `case` |
| case 里声明变量 | 必须加 `{}` | 无 |
| 迭代器 | 容器里的光标，`end()` 表示到头或没找到 | 遍历时的当前位置 |
| erase-remove | `remove_if` 只挪位置 | `lists:filter/2` |
| `map[key]` | 不存在时插入默认值 | `maps:update_with/4` |
| 先收集再执行 | 遍历中可能修改容器 → 存 ID 快照，再按 ID 查找 | 列表天然是快照 |
| `it = erase(it)` | 一边遍历一边删除的唯一正确写法 | 不存在这个问题 |

这一课的难点本质上是同一个问题：**Erlang 的数据不可变，遍历时不会被别人修改；C++ 的数据可以原地修改，要时刻留意"我正在用的东西是不是已经被改掉或删掉了"。**

下一课：[第 7 课：整数安全](07-integer-safety.md)
