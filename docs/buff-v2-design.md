# Buff v2 设计

## 目标

Buff 不再为每一种可修改属性增加一个成员，也不再为周期伤害维护一套独立执行路径。
运行时模型由四个正交组件组成：

```text
BuffSpec
├── LifetimePolicy       何时减少持续计数
├── StackingPolicy       如何叠层和刷新
├── AttributeModifier[]  如何改变属性
└── BuffReaction[]       监听事件后执行哪些 Effect
```

新增一种修改已有属性的 Buff 只增加配置行；新增一种周期、受击或行动后效果只组合
Reaction 与已有 Effect。只有复活、召唤、换位等新的战斗机制才需要新增 C++ opcode。

## 类型化中间表示

```cpp
enum class Attribute : std::uint8_t {
    attack,
    defense,
    speed,
    crit_rate_bp,
    crit_damage_bp,
    hit_rate_bp,
    dodge_rate_bp,
    damage_bonus_bp,
    damage_reduction_bp
};

enum class ModifierOperation : std::uint8_t { add, scale_bp };

struct AttributeModifier {
    Attribute attribute;
    ModifierOperation operation;
    std::int64_t value;
};
```

Modifier 的结算顺序固定为：

```text
基础值
→ 累加所有 add × stacks
→ 累加所有 scale_bp × stacks
→ 一次整数万分比缩放
→ 应用该属性自身的合法范围
```

同一阶段采用求和而不是依赖容器遍历顺序，因此配置行顺序不会改变结果。

## 生命周期

```cpp
struct LifetimePolicy {
    bool permanent;
    std::int32_t duration;
    Trigger decrement_on;
};
```

有限状态在指定事件完成反应后减少持续计数。当前事件开始后新添加的 Buff 不参加本次
递减，避免“回合结束挂上的两回合 Buff 立刻只剩一回合”的歧义。永久状态不递减。

## 叠层

```cpp
enum class StackPolicy : std::uint8_t { stack, refresh };
enum class RefreshPolicy : std::uint8_t { reset, extend, keep };
enum class StackKeyPolicy : std::uint8_t { by_buff, by_buff_and_source };

struct StackingPolicy {
    std::int32_t max_stacks;
    StackPolicy mode;
    RefreshPolicy refresh;
    StackKeyPolicy key;
};
```

- `stack`：命中已有实例时增加层数，上限为 `max_stacks`。
- `refresh`：命中已有实例时保持一层，只处理持续时间。
- `reset`：剩余持续时间重置为配置值。
- `extend`：在当前剩余值上追加配置值。
- `keep`：不改变剩余持续时间。
- `by_buff`：同一持有者的相同 Buff ID 共用实例，重施时来源更新为最后施加者。
- `by_buff_and_source`：Buff ID 与施加者共同组成键，各施加者独立叠层和过期。

每层独立过期和满层溢出行为仍是后续可以增加的 StackingPolicy 组件，不需要改动
Modifier 或 Reaction。

## 事件反应

```cpp
enum class EffectSource : std::uint8_t { owner, applier };
enum class StackScaling : std::uint8_t { once, per_stack };

struct BuffReaction {
    Trigger trigger;
    std::int32_t priority;
    EffectSource source;
    StackScaling stack_scaling;
    BasisPoints chance_bp;
    std::int32_t max_triggers_per_round;
    std::vector<Effect> effects;
};
```

`owner` 是 Buff 持有者，`applier` 是施加者。目标规则中的 `self` 始终以持有者为锚点；
`source` 只决定效果的属性来源和事件归属。因此中毒可以读取施加者攻击，同时对持有者
执行 `self` 伤害。

`once` 表示效果数值与层数无关；`per_stack` 会让伤害、治疗和直接伤害的倍率与固定值
按当前层数放大。它必须显式配置，避免所有 Reaction 被隐式套用叠层规则。

同一 Trigger 的 Passive 与 Buff Reaction 进入统一候选集合，按 `priority` 降序执行；
同优先级再按单位顺序、类型、定义 ID、Buff 实例 ID 和配置顺序稳定排序。次数上限先于
概率检查，因此已经耗尽次数的 Reaction 不会消费随机数。

周期效果不再使用独立的 Tick 固定字段。例如中毒编译为：

```text
BuffReaction(round_end, applier, per_stack)
└── direct_damage(self, flat=35)
```

`direct_damage` 是明确的效果机制：绕过命中、闪避、防御、暴击、增伤和减伤，但仍触发
受击与死亡事件。普通 `damage` 保持完整攻击结算。

## 显式执行队列与事件上下文

技能、Reaction、效果解析、生命周期递减和死亡检查都转换为 `WorkItem`，由 LIFO
`WorkQueue` 深度优先执行，不再依赖 C++ 递归调用栈表达规则顺序。每个任务携带
`event_id`、`parent_event_id` 和 `depth`；伤害结算后的固定顺序是：

```text
伤害状态修改
→ 攻击者 on_hit
→ 受击者 on_damaged（致死也执行）
→ DeathCheck
→ unit_death（死亡者本人可以监听）
```

`max_execution_steps` 在每个任务执行前消费，超限以 `execution_limit` 结束；
`max_logged_events` 与 `log_level` 只裁剪战报，绝不停止计算或改变最终状态。

## 定义与实例

`Effect::buff` 持有不可变 `shared_ptr<const BuffSpec>`。运行时实例只保存定义引用和本场
状态：

```cpp
struct ActiveBuff {
    std::shared_ptr<const BuffSpec> spec;
    std::uint64_t instance_id;
    std::int32_t remaining;
    std::int32_t stacks;
    UnitId source;
    std::vector<std::int32_t> reaction_triggers;
};
```

一场战斗持有其启动时的配置快照。配置热加载只影响新请求，不改变已经开始的战斗。
配置编译器拒绝 Buff Reaction 通过 `add_buff` 构成的引用环，避免共享所有权环。

## 配置关系

```text
buffs.csv
├── buff_modifiers.csv
└── buff_reactions.csv ──→ effects.csv ──→ buffs.csv

skills.csv  ─────────────→ effects.csv
passives.csv ────────────→ effects.csv
```

当前 GBCF 主版本为 3。策划 CSV 是规范化关系表；编译器负责字符串枚举解析、范围检查、
引用检查和环检测；C++ 加载后只保留类型化、不可变对象。

## 兼容边界

以下接口保持不变：

- `gamebattle:simulate/1,2`
- `gamebattle:load_config/1,2`
- `gamebattle:carryover/2`
- `gamebattle:run_gauntlet/3,4`
- Port `{packet, 4}` ETF 帧
- `BattleResult` 的胜负与单位剩余生命字段

内嵌 Buff ETF 只接受 `lifetime`、`stacking`、`modifiers`、`reactions` 组成的通用模型，
四个组件必须显式出现。协议会拒绝旧固定字段和未知字段，不在边界维护兼容转换。
GBCF 是构建产物，当前 3.0 与 2.0 不兼容，必须由新配置编译器重新生成。

默认车轮战仍只继承 HP。若以后需要跨场继承 Buff，应单独增加包含 `buff_id`、层数、
剩余计数、来源策略和配置版本的显式持久状态，不能复制运行时指针或属性缓存。
