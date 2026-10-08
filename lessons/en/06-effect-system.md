# Lesson 6: The effect system

[中文](../06-effect-system.md) | **English**

> File: `src/effect_system.cpp` (455 lines, the most complex in the project)

It handles "everything that happens when an effect actually lands": dealing damage, healing, applying buffs, removing buffs, and the **chain reactions** of passives and buff reactions that follow.

## 0. The big picture: how one attack sets off a chain

```
execute_action (a unit acts)
 ├─ pick a skill by chance; basic attack if none triggers
 ├─ trigger_owner(on_attack)          → the attacker's "on attack" passives
 └─ execute_effects(the skill's effect list)
      └─ for each target, dispatch on the effect kind:
           apply_damage ─┬─ trigger_owner(on_hit)      → the attacker's "on hit" passives ────┐
                         ├─ trigger_owner(on_damaged)  → the target's "on damaged" passives ──┤
                         └─ trigger_all(unit_death)    → everyone's "someone died" passives ──┤
           apply_heal / apply_buff / remove_buff                                              │
                                                                                              ▼
      trigger_owner_snapshot: run passives and buff reactions → execute_effects(……) → back up top
```

This is a **recursive** structure and could in principle loop forever ("counterattack when hit" meets "counterattacks can be countered"). Two safeguards:

- **A depth limit**: each recursion level does `depth + 1`; past `kMaxTriggerDepth = 32` it returns immediately.
- **An event limit**: when the event count reaches `max_events`, `event_limit = true`, and almost every function checks it at the top.

The C++ call stack is only a few MB; recursion that goes too deep overflows the stack and crashes, so you must set a limit yourself.

## 1. `execute_action`: a pointer that chooses between two sources

```cpp
const Skill* selected = nullptr;
for (const auto& skill : state_.units[actor_index].config.skills) {
    if (state_.random.roll(skill.chance_bp)) {
        selected = &skill;          // points at one of the skills in the config
        break;
    }
}

Skill basic;                        // a local object: the basic attack
if (selected == nullptr) {
    basic.name = "basic_attack";
    basic.effects.push_back(Effect{});
    selected = &basic;              // now points at the local basic attack
}
```

- Skills are already sorted by priority, so the first one to pass its chance roll is the one cast this time.
- `selected` is a **borrowing pointer**; from here on everything uses `selected->effects`.
- Pointing at a local is safe: `basic` lives until the function ends, and `selected` is only used inside the function.
- `Effect{}` is an effect with all defaults: `damage`, `enemy_front`, 100% of attack. So the basic attack is "hit the front row once".

## 2. `execute_effects`: copy only when needed, then dispatch

```cpp
for (const auto& effect : effects) {
    Effect scaled_effect;
    const Effect* executable = &effect;           // by default use the original effect, no copy
    if (magnitude_stacks > 1 && (damage/heal/direct damage)) {
        scaled_effect = effect;                   // copy only when it must be scaled by stacks
        scaled_effect.flat = saturating_multiply(effect.flat, magnitude_stacks);
        executable = &scaled_effect;
    }
```

This corresponds to a buff's `per_stack`: three stacks of poison deal three times the damage of one. The config is `const`, so it has to be copied before it's changed; when no scaling is needed it is simply borrowed.

### Dispatching with `switch`

```cpp
switch (executable->kind) {
case EffectKind::damage:
case EffectKind::direct_damage:        // two cases stacked: they share the same code
    apply_damage(...);
    break;                             // always write break
case EffectKind::heal:
    apply_heal(...);
    break;
...
}
```

- **Without `break`, execution "falls through" into the next case.** Here that's used on purpose so the two kinds of damage share code; but a forgotten `break` is a sneaky bug.
- **No `default`, on purpose**: if a new `EffectKind` is added later and someone forgets to handle it, `-Wall` warns (measured):

```
warning: enumeration value 'remove_buff' not handled in switch [-Wswitch]
```

Writing a `default` makes the warning go away. Erlang only reports `case_clause` at run time; C++ can catch it at compile time, provided there is **no default**.

## 3. `apply_damage`: the damage formula, then the chain reaction

```cpp
auto damage = saturating_add(scale(actor_stats.attack, effect.attack_bp), effect.flat);  // attack × ratio + flat
if (!direct) {
    hit roll: roll(hit rate - dodge rate); on a miss → emit("miss") → return
    damage = max(1, damage - defense)
    damage = max(1, damage × (1 + damage bonus))
    damage = max(1, damage × (1 - damage reduction))
    crit roll: roll(crit rate); on a crit → damage × crit damage
} else {
    direct_damage: skip all of the above, at least 1 point
}
damage = std::min(damage, target.hp);     // can't go below zero
target.hp -= damage;
```

After the HP loss, three kinds of trigger fire:

```cpp
if (!direct) trigger_owner(actor_index,  Trigger::on_hit,      target_index, ...);
             trigger_owner(target_index, Trigger::on_damaged,  actor_index,  ...);
if (!target.alive()) trigger_all(Trigger::unit_death, target_index, ...);
```

The third argument, the "unit involved in the event", becomes `trigger_unit` in target rules: `on_hit` passes the target that was hit, `on_damaged` passes the attacker. So a "counterattack when hit" configured with `target = trigger_unit` hits back at the attacker.

## 4. Iterators: a concept Erlang doesn't have

An iterator is **a "cursor" pointing at some position in a container**:

```
buffs:     [ 801 ][ 802 ][ 803 ]
             ↑                   ↑
         begin()              end()     ← "one past the last element", not any element
```

- Ranges are half-open: `[begin, end)`.
- `*it` gets the element, `it->field` accesses a member, `++it` advances.
- **`end()` must not be dereferenced**; it only means "reached the end" or "not found".

The closest Erlang concept is the current position while walking `[H | T]`, with `end()` corresponding to reaching `[]`.

## 5. `apply_buff`: look up, add, or stack

```cpp
auto iterator = std::find_if(
    target.buffs.begin(), target.buffs.end(),
    [&](const ActiveBuff& active) {
        return active.definition != nullptr && active.definition->id == definition->id;
    });
```

When `find_if` finds nothing it returns `end()`, just as `lists:search/2` returns `false`.

```cpp
if (iterator == target.buffs.end()) {             // not found: apply a new one
    ActiveBuff active;
    active.definition = std::move(definition);
    active.instance_id = state_.next_buff_instance_id++;
    target.buffs.push_back(std::move(active));
    iterator = std::prev(target.buffs.end());      // ← note this line
} else {                                          // found: stack or refresh
    ...
}
```

**Why reassign `iterator` after `push_back`?** The old `iterator` was `end()`; more importantly, **`push_back` may reallocate, and after reallocation every old iterator is invalid**. `std::prev(end())` is the element just added.

**Rule: after modifying a container, iterators, pointers and references obtained earlier may all be invalid; obtain them again.**

### Declaring variables inside a case needs braces

```cpp
case RefreshPolicy::extend: {                      // ← required
    const auto extended = saturating_add(iterator->remaining, spec.lifetime.duration);
    iterator->remaining = ...;
    break;
}
```

Without them you get `error: jump to case label`. The whole `switch` shares one scope; jumping to the `keep` case would **skip the initialization of `extended`** while the name is still visible there. The braces confine it to its own case.

## 6. `remove_buff`: the erase-remove idiom

```cpp
target.buffs.erase(
    std::remove_if(target.buffs.begin(), target.buffs.end(),
                   [&](const ActiveBuff& active) { return active.definition->id == buff_id; }),
    target.buffs.end());
```

**`std::remove_if` doesn't actually erase anything.** Measured:

```
original:          [801, 802, 801, 803]
after remove_if:   size=4  valid part length=2       ← the container's size didn't change!
after erase:       size=2  contents=802 803
```

`remove_if` moves the elements to keep to the front and returns "the new logical end"; an algorithm only gets iterators and **can't change the container's size**, so the real removal has to be done by the container's own `erase`. In C++20 you can write `std::erase_if(target.buffs, predicate);`.

The counterpart of `lists:filter/2`.

## 7. `trigger_owner_snapshot`: the core of the core

### Phase A: passives

```cpp
for (const auto passive_index : owner.passives_by_trigger.at(trigger_index)) {
    const auto& passive = owner.config.passives[passive_index];
    if (!state_.random.roll(passive.chance_bp)) continue;
    auto& count = owner.passive_triggers[passive.id];         // ← note the []
    if (passive.max_triggers_per_round > 0 && count >= passive.max_triggers_per_round) continue;
    ++count;
    execute_effects(owner_index, owner_index, passive.effects, passive.id, depth + 1, event_unit);
}
```

- `passives_by_trigger` is an array grouped by trigger at construction time, so not every passive has to be checked every time.
- **`unordered_map`'s `[]` automatically inserts a default value when the key is missing** (measured):

```
size=0 -> after accessing [701] size=1 count=0 -> after ++ map[701]=1
```

  Here that's exactly what's wanted: the first access inserts 0, then `++`. It's like `maps:update_with(Id, fun(C) -> C + 1 end, 1, Map)`. **But don't use `[]` for read-only lookups**: it inserts records out of thin air; use `find()`. `[]` also can't be used on a `const` map.

### Phase B: buff reactions (collect first, then execute)

```cpp
struct PendingReaction {
    std::uint64_t instance_id;
    std::size_t reaction_index;
};
std::vector<PendingReaction> pending;
for (const auto& active : owner.buffs) {                        // step 1: read-only pass, collecting
    if (active.instance_id > buff_instance_cutoff) continue;    // applied after this trigger started: excluded
    ...pending.push_back({active.instance_id, index});
}

for (const auto& item : pending) {                              // step 2: execute one by one
    auto active = find_buff_instance(owner, item.instance_id);  // look up by ID again each time
    if (active == owner.buffs.end()) continue;                  // already removed: skip
    auto definition = active->definition;                       // copy the shared_ptr
    const auto& reaction = definition->reactions[item.reaction_index];
    ...
    execute_effects(...);                                       // this may modify owner.buffs!
}
```

**Why not execute while iterating `owner.buffs`?** Executing a reaction may `add_buff` (reallocation) or `remove_buff` (elements shift forward): **the container is modified mid-iteration**. So there are two steps: first a read-only pass that records unique IDs; then look each one up by ID and execute it. An Erlang list is naturally a snapshot; **C++ makes you build the snapshot yourself**.

**`buff_instance_cutoff`**: the largest instance_id that existed when the trigger started. Buffs applied during the trigger have larger IDs and are skipped. This implements the design doc's rule, "buffs added after the current event started don't take part in this reaction"; otherwise a poison applied at round end would tick immediately.

**Why must `auto definition = active->definition;` be a copy?** `execute_effects` may remove this very buff through `remove_buff`: the `active` iterator becomes invalid; if it was the last holder of the `BuffSpec`, the `BuffSpec` is freed too; and the `reaction` reference points right into that `BuffSpec`. The local `definition` guarantees the `BuffSpec` stays alive for this iteration.

The final call uses `active->stacks`: that's safe because **function arguments are evaluated before the function is called**, while `active` is still valid; after the call the function never touches `active` again.

### Phase C: buff expiry (the correct way to erase while iterating)

```cpp
auto active = owner.buffs.begin();
while (active != owner.buffs.end()) {
    if (should decrement) --active->remaining;
    if (should decrement && active->remaining <= 0) {
        emit("buff_expire");
        active = owner.buffs.erase(active);    // ← erase returns the next valid position
    } else {
        ++active;                               // ← advance only when nothing was erased
    }
}
```

**The wrong way**:

```cpp
for (auto it = buffs.begin(); it != buffs.end(); ++it) {
    if (--it->remaining <= 0) buffs.erase(it);     // it is invalid after erase, and the for still does ++it
}
```

Measured:

```
ERROR: AddressSanitizer: heap-buffer-overflow
Error: attempt to increment a singular iterator.
```

After erasing, `it = erase(it)`; otherwise `++it`. It's one or the other, so you need a `while` loop that controls advancing itself. This is a fixed pattern you simply have to memorize in C++.

## 8. A small helper: `value_or`

```cpp
effect_source_index = state_.find_unit(active->source).value_or(owner_index);
```

Take the value if there is one, otherwise use the default, like `maps:get(Key, Map, Default)`. `applier`-type reactions compute with the applier's stats, and fall back to the holder if the applier can't be found.

## Summary

| Concept | Key point | Erlang |
|---|---|---|
| Recursive triggers | Two safeguards: depth 32 + an event limit | Recursion rarely worries about the stack |
| A pointer choosing one of two | One variable points at the config or a local object | Variable binding |
| `switch` | Stacked cases share code; only without `default` do you get the missing-case warning | `case` |
| Declaring variables in a case | Must add `{}` | None |
| Iterators | A cursor into a container; `end()` means the end or not found | The current position while walking |
| erase-remove | `remove_if` only moves elements | `lists:filter/2` |
| `map[key]` | Inserts a default when missing | `maps:update_with/4` |
| Collect then execute | The container may change during iteration → snapshot IDs, then look up by ID | Lists are naturally snapshots |
| `it = erase(it)` | The only correct way to erase while iterating | The problem doesn't exist |

The hard parts of this lesson are really one problem: **Erlang data is immutable, so nobody changes it while you iterate; C++ data can be modified in place, so you must always ask "has the thing I'm using already been changed or removed?"**

Next: [Lesson 7: Integer safety](07-integer-safety.md)
