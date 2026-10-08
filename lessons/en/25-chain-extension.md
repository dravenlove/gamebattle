# Lesson 25: Adding chains to the auto-battle

[中文](../25-chain-extension.md) | **English**

> Part 7, "Extending the engine". Changes: `include/gamebattle/engine.hpp`, `src/battle_runtime.hpp`, `src/effect_system.cpp`, `src/battle_state.cpp`, `src/wire.cpp`, `tools/config_compiler.cpp`, `src/config_store.cpp`, `tests/`.
>
> The first 24 lessons were about understanding an existing engine. This lesson goes the other way: adding new gameplay to an engine that's already running in production. The gameplay itself is simple; the hard part is **adding it without changing any old battle results**.

## 1. Goals and constraints

**Goal**: keep "set formations → fully automatic battle → next round" unchanged, and add Yu-Gi-Oh-style chains:

- when an active skill is activated it doesn't resolve right away; the other side can respond (say, negate it), and allies can respond too (say, buff the caster's attack first);
- responses can themselves be responded to;
- the last one in resolves first.

**Constraints**:

| Constraint | Why |
|---|---|
| Requests that don't use the new gameplay must produce **byte-identical** results | Lesson 3: consuming one random number more or less shifts every later roll; production replays and anti-cheat verification depend on this |
| The protocol stays backward compatible | The README's principle: only add, never change what existing fields mean |
| The round flow doesn't change | Not a single line of `engine.cpp` changes |

The full rules are in the "Chains and responses" section of the root README; this lesson covers only the implementation.

## 2. The data model: two triggers and one effect

```cpp
enum class Trigger : std::uint8_t {
    battle_start, ..., round_end,
    enemy_activate,     // new: an enemy added a chain link
    ally_activate       // new: an ally (other than yourself) added a chain link
};
enum class EffectKind : std::uint8_t {
    ..., direct_damage = 4,
    negate = 5          // new: cancel the link this response answered
};

inline constexpr bool is_response_trigger(Trigger trigger) {
    return trigger == Trigger::enemy_activate || trigger == Trigger::ally_activate;
}
```

Three details:

- **New values always go at the end.** Enum numbers are written into `.gbcfg` (lesson 10); inserting in the middle would shift every number in old files.
- **`kTriggerCount` must change too.** It used to be `static_cast<std::size_t>(Trigger::round_end) + 1` and sizes the `passives_by_trigger` array. Without the change, the new triggers' indices would be out of range: `.at()` would throw, and `[]` would be undefined behavior.
- **Functions in headers must be `inline`** (lesson 23, question 8); `constexpr` functions are implicitly `inline`.

Following lesson 10's maintenance checklist, a new `EffectKind` means changing 6 places together. This change went through all of them: `engine.hpp`, the compiler's `kEffectKinds`, the maximum in the loader's `checked_enum`, `parse_effect_kind` in `wire.cpp`, the `switch` in `effect_system.cpp`, and item 6, the format version. In the end the version was **not** bumped; section 5 explains why.

## 3. The chain itself

### 3.1 What a link records

```cpp
struct ChainLink {
    std::size_t source_index{0};               // the owner (an index into units)
    std::uint32_t source_id{0};                // the skill or passive ID
    const std::vector<Effect>* effects{nullptr};
    std::optional<std::size_t> answered_unit;  // who it answered; becomes trigger_unit when it resolves
    bool negated{false};
};
```

`effects` is a **borrowing pointer** to a skill or passive in the unit's config. That's safe because, as lesson 2 explained, `units` never grows or shrinks during a battle, so the config always outlives the chain. It's also why the index `source_index` can be held for a long time.

### 3.2 Activation: only active skills open a chain

```cpp
trigger_owner(actor_index, Trigger::on_attack, actor_index, selected->id);
if (selected == &basic || !responses_possible_) {
    execute_effects(...);        // exactly as before
    return;
}
run_chain(actor_index, *selected);
```

Basic attacks don't open a chain. `responses_possible_` is the "zero-cost switch" from section 4.

### 3.3 Building: one link at a time

```cpp
bool EffectSystem::add_response() {
    const auto answered = chain_.back().source_index;
    const auto answered_side = state_.units[answered].side;
    for (const auto side : {other(answered_side), answered_side}) {      // the other side first, then the same side
        const auto trigger = side == answered_side ? Trigger::ally_activate
                                                   : Trigger::enemy_activate;
        for (const auto unit_index : state_.response_order(side)) {       // speed → position → ID
            if (already in the chain) continue;                           // at most one link per unit
            for (each of this unit's passives on trigger) {
                if (!roll(chance)) continue;
                if (over the per-round limit) continue;
                chain_.push_back(...);
                emit("chain", ...);
                return true;                                              // added one: return and ask again
            }
        }
    }
    return false;
}
```

The caller is `while (!state_.event_limit && add_response()) {}`.

- **Why return after adding one?** Once a new link joins, "the top link" has changed, and so has what everyone would be answering, so the asking must start over.
- **Why at most one link per unit?** It stops two units from negating each other back and forth forever. It also caps the chain length: never more than the total number of units.
- **`{other(answered_side), answered_side}`** is the braced-list technique from lesson 8, building a temporary two-element list to iterate over.
- **`response_order` and `acting_order` share one sorting function, `sort_by_speed`**; the only difference is that it doesn't filter on `can_act`: pets and artifacts can't act, but their passives can respond. The sort always ends by comparing the unique ID, so the result is deterministic (lesson 5).
- **Roll the chance first, then check the limit**, the same order as ordinary passives (`trigger_owner_snapshot`). If the two orders differed, the same config would consume different random numbers depending on how it was triggered, which is very hard to track down.

### 3.4 Resolving: a reverse loop

```cpp
for (std::size_t link = chain_.size(); link-- > 0;) {
    const ChainLink current = chain_[link];          // a copy, not a reference
    if (current.negated) continue;
    if (chain_.size() > 1 && !units[current.source_index].alive()) {
        emit("fizzle", ...);                         // the owner is dead: the link fizzles
        continue;
    }
    resolving_link_ = link;
    execute_effects(..., *current.effects, ..., current.answered_unit);
}
```

**How the reverse loop is written**: `link` is an unsigned `size_t`, and the most obvious version is wrong (measured):

```cpp
for (std::size_t link = chain.size() - 1; link >= 0; --link)
// warning: comparison of unsigned expression in '>= 0' is always true [-Wtype-limits]
// at run time: once link reaches 0, decrementing it once more gives 18446744073709551615
```

An unsigned number is always `>= 0`, so the loop never stops (lessons 3, 7). `link-- > 0` compares first and decrements afterwards: the comparison sees `link` as 1, the body sees 0, and the loop exits right after handling index 0. It's one of the standard ways to iterate backwards in C++; the other is reverse iterators, `rbegin()` / `rend()`.

**Why copy `current` instead of using `auto&`?** During resolution, `negate` modifies other elements of `chain_` (marking the link below as `negated`). `chain_` never reallocates here, so a reference wouldn't actually dangle; but holding a reference to a container element while modifying the same container forces every reader to prove all over again that it's safe (lessons 2, 6). A `ChainLink` is a few dozen bytes; copying it removes the worry.

### 3.5 `negate`: an effect with no targets

```cpp
for (const auto& effect : effects) {
    if (effect.kind == EffectKind::negate) {
        negate_answered_link(source_index);   // marks chain_[resolving_link_ - 1] as negated
        continue;                             // skips target selection
    }
    ...
    switch (executable->kind) {
    ...
    case EffectKind::negate:                  // unreachable, but required
        break;
    }
}
```

That unreachable `case` must be there: as lesson 6 explained, there's deliberately no `default`, so `-Wswitch` warns when a new enum value isn't handled. After adding `negate`, that warning was exactly what pointed to this spot.

Every response answers "the top link at the moment it joined", which is the link directly below it, so "the link being answered" is simply `resolving_link_ - 1`, with nothing extra to record.

### 3.6 Why no recursion-depth guard is needed

Chains are opened only in `execute_action`, and resolving a response passive never opens a new chain. So chains don't nest, and `chain_` can be an ordinary member variable. Ordinary passives like `on_hit` triggered while a link resolves still go through the old recursion, and its depth limit of 32 still applies.

## 4. The zero-cost switch: keeping old results unchanged

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

When no unit has a response passive, skills go through **exactly the same line of code** as before. Even without the switch no extra random numbers would be consumed (with no candidate passives there's nothing to roll), but the switch also skips a sort on every skill activation, and makes "old requests are unaffected" obvious at a glance.

The `fizzle` check also has the condition `chain_.size() > 1`: with a single link (nobody responded) the behavior must be exactly as before, and not even the new "a dead caster's link fizzles" rule may apply.

**How it was verified**: before changing any code, run 2000 sample battles with different seeds on the old engine, encode each result as ETF, hash it, and fold the hashes into one total; after the change, run the same thing on the new engine. Measured:

```
before: battles=2000 events=1759731 combined=6d21b37b7457c504
after:  battles=2000 events=1759731 combined=6d21b37b7457c504
```

1.76 million events, byte-identical. It's the same method lesson 21 used to verify a performance optimization: **prove the results haven't changed before anything else.**

## 5. Validation in three layers

| Where | What it checks | Why it's needed |
|---|---|---|
| Config compiler | Skills and buff reactions can't reference `negate`; `negate` can only appear in response passives; `decrement_on` and a reaction's `trigger` can't be response triggers | Reports errors before release, with file names and line numbers (`skills.csv:2: ...`) |
| Loader `ConfigStore` | The same rules | Guards against a hand-edited, broken `.gbcfg` |
| `validate_request` | The same rules | Inline requests skip the compiler; requests using config IDs are checked once more here too |

It's the same idea as lessons 8 and 10: fail as early as possible, with the run time as the last line of defense.

**Why wasn't the `.gbcfg` version bumped?** The file layout didn't change; the enums just gained values. An older loader that reads a new value fails clearly through lesson 10's `checked_enum` with `contains an unknown enum value`, rather than misreading it; config files that don't use the new gameplay are byte-identical to before and load in both old and new loaders. Bumping the version would instead make the new loader reject every existing 2.0 file. Lesson 10's maintenance checklist has been updated to reflect this distinction.

## 6. Tests: proving the new feature is right

### 6.1 Remove randomness so every number can be worked out

The test units have 100% hit, 0 crit, and every passive has a 100% chance. As lesson 3 explained, rolls at 0% and 100% consume no random numbers. So the whole battle has no randomness at all, and every number can be worked out by hand:

- a skill with 300% ratio from attack 100 deals **300** damage to a target with 0 defense;
- with an ally responding to add 1000 attack first, the same skill deals **3300**.

The new tests cover: negate, counter-negate (3 → 2 → 1), ally support resolving first, a caster killed by a response fizzling, invalid configs being rejected, parsing the new names from ETF, and the loader reading a compiled chain config and running a battle with a negate. The assertions check not only "does this event exist" but also the order of events (`seq`).

### 6.2 Do the tests actually test anything? Break the code on purpose

When every test passes on the first try, that's a reason to suspect they don't test anything. The way to check is to **break the code on purpose** and see whether the tests notice (this is called mutation testing):

| Deliberate breakage | Result |
|---|---|
| `negate` does nothing | Test fails: `count_events(negated, "damage", 5101) == 0` |
| Resolve first-in, first-out | Test fails: the same assertion |

Both breakages were caught, so the tests really do check the core semantics of the chain.

### 6.3 Negative tests should check why something failed

The existing negative tests in CMake use `WILL_FAIL`: the test passes as long as the command fails. But a crash, a wrong argument or a missing file are "failures" too. The two new negative tests use `PASS_REGULAR_EXPRESSION` instead, requiring the expected error message in the output:

```cmake
set_tests_properties(gamebattle_config_compiler_invalid_negate_skill
    PROPERTIES PASS_REGULAR_EXPRESSION
        "skills.csv:2: negate effects are only valid in response passives")
```

### 6.4 Sanitizers and fuzzing

- All 8 tests pass under ASan + UBSan.
- Lesson 22's `term_fuzz` gained a seed with response passives. First it was confirmed that this seed really produces chains after going through ETF (1557 chain links, 500 negates and 1 fizzle over 50 battles), then 60,000 mutations ran without a crash.

## 7. What could be added next

| Gameplay | Where to change |
|---|---|
| Bonds / synergy conditions ("only while X is on the field") | Add a condition field to `Passive` and check it before rolling the chance; add a column each to the protocol, the compiler and the loader |
| Responding to attack declarations (basic attacks can be responded to too) | Route basic attacks through `run_chain` in `execute_action`; note this changes old results, so it needs a switch or a new engine version number |
| More response effects (negate and destroy, reflect) | New `EffectKind`s, following the 6-place checklist in section 2 |
| A chain length cap, strict alternation between sides | Only `add_response` changes |
| Players deciding by hand whether to respond | This turns it into a real card battler: the engine must pause at decision points and wait for player input, and the interface changes from a one-shot `simulate` to a session; a much bigger change |

## Summary

| Point | How |
|---|---|
| New gameplay doesn't change old results | Zero cost when unused; 2000 battles compared byte for byte before and after |
| Adding enum values | Append at the end; change all 6 places together; adding values alone doesn't require a file version bump |
| The chain | A `vector` as a stack; one link added at a time; at most one per unit; resolve in reverse |
| Reverse loops | `for (size_t i = n; i-- > 0;)`, never `i >= 0` |
| Borrowing vs copying | Borrow the config through a pointer; copy each link while resolving instead of holding references to container elements |
| Validation | Three layers: compiler, loader, run time |
| Tests | Remove randomness to get exact numbers; break the code on purpose to check the tests; negative tests check the reason for failure |

Back to the index: [Course overview](README.md)
