# Buff v2 design

[中文](buff-v2-design.md) | **English**

## Goals

Buffs no longer add a member for every attribute they can modify, and no longer maintain a separate execution path for periodic damage.
The runtime model is made of four orthogonal components:

```text
BuffSpec
├── LifetimePolicy       when the duration counter decreases
├── StackingPolicy       how stacking and refreshing work
├── AttributeModifier[]  how attributes are changed
└── BuffReaction[]       which effects run in response to events
```

A new buff that modifies an existing attribute adds only config rows; a new periodic, on-hit or after-action effect only combines
a reaction with existing effects. Only genuinely new battle mechanics such as revival, summoning or swapping positions need a new C++ opcode.

## Typed intermediate representation

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

Modifiers are always resolved in this order:

```text
base value
→ sum of all add × stacks
→ sum of all scale_bp × stacks
→ one integer basis-point scaling
→ apply the attribute's own valid range
```

Within a stage values are summed rather than depending on container iteration order, so the order of config rows doesn't change the result.

## Lifetime

```cpp
struct LifetimePolicy {
    bool permanent;
    std::int32_t duration;
    Trigger decrement_on;
};
```

A finite state decreases its duration counter after the specified event's reactions finish. Buffs added after the current event started don't take part
in this decrement, avoiding the ambiguity of "a two-round buff applied at round end immediately has only one round left". Permanent states don't decrement.

## Stacking

```cpp
enum class StackPolicy : std::uint8_t { stack, refresh };
enum class RefreshPolicy : std::uint8_t { reset, extend, keep };

struct StackingPolicy {
    std::int32_t max_stacks;
    StackPolicy mode;
    RefreshPolicy refresh;
};
```

- `stack`: when it hits an existing instance, add a stack, up to `max_stacks`.
- `refresh`: when it hits an existing instance, keep one stack and only handle the duration.
- `reset`: reset the remaining duration to the configured value.
- `extend`: add the configured value on top of the current remaining value.
- `keep`: don't change the remaining duration.

The first version still merges by `buff_id`. Per-source stacks, independently expiring stacks and overflow behavior at max stacks are StackingPolicy components
that can be added later, without touching modifiers or reactions again.

## Event reactions

```cpp
enum class EffectSource : std::uint8_t { owner, applier };
enum class StackScaling : std::uint8_t { once, per_stack };

struct BuffReaction {
    Trigger trigger;
    EffectSource source;
    StackScaling stack_scaling;
    BasisPoints chance_bp;
    std::int32_t max_triggers_per_round;
    std::vector<Effect> effects;
};
```

`owner` is the buff holder and `applier` is whoever applied it. `self` in target rules is always anchored on the holder;
`source` only decides where the effect's stats come from and who the event is attributed to. So poison can read the applier's attack while dealing
`self` damage to the holder.

`once` means the effect's values don't depend on the stack count; `per_stack` scales the ratios and flat values of damage, healing and direct damage
by the current stack count. It must be configured explicitly, so stacking rules aren't implicitly applied to every reaction.

Periodic effects no longer use separate fixed tick fields. Poison, for example, compiles to:

```text
BuffReaction(round_end, applier, per_stack)
└── direct_damage(self, flat=35)
```

`direct_damage` is an explicit effect mechanism: it bypasses hit, dodge, defense, crit, damage bonus and damage reduction, but still triggers
damaged and death events. Ordinary `damage` keeps the full attack resolution.

## Definitions and instances

`Effect::buff` holds an immutable `shared_ptr<const BuffSpec>`. A runtime instance stores only a reference to the definition and this battle's
state:

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

A battle holds the config snapshot it started with. A config hot reload only affects new requests and doesn't change battles already in progress.
The config compiler rejects reference cycles formed by buff reactions through `add_buff`, avoiding shared-ownership cycles.

## Config relationships

```text
buffs.csv
├── buff_modifiers.csv
└── buff_reactions.csv ──→ effects.csv ──→ buffs.csv

skills.csv  ─────────────→ effects.csv
passives.csv ────────────→ effects.csv
```

The GBCF major version is bumped to 2. The designers' CSV files are normalized relational tables; the compiler is responsible for parsing string enums, range checks,
reference checks and cycle detection; after loading, C++ keeps only typed, immutable objects.

## Compatibility boundary

The following interfaces stay unchanged:

- `gamebattle:simulate/1,2`
- `gamebattle:load_config/1,2`
- `gamebattle:carryover/2`
- `gamebattle:run_gauntlet/3,4`
- Port `{packet, 4}` ETF frames
- `BattleResult`'s winner, events and remaining-unit-HP fields

Inline buff ETF only accepts the generic model made of `lifetime`, `stacking`, `modifiers` and `reactions`,
and all four components must appear explicitly. The protocol rejects old fixed fields and unknown fields, and maintains no compatibility conversion at the boundary.
GBCF is a build artifact; this change upgrades it directly to 2.0, regenerated by the new config compiler.

By default the gauntlet still carries over only HP. If carrying buffs between battles is needed later, add explicit persistent state containing `buff_id`, stack count,
remaining count, source policy and config version; never copy runtime pointers or attribute caches.
