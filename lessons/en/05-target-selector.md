# Lesson 5: Target selection

[中文](../05-target-selector.md) | **English**

> File: `src/target_selector.cpp` (83 lines)

This file does one thing: **given a rule, return a list of target indices**. It doesn't deal damage or apply buffs; it only decides "who". This lesson covers lambdas and `std::sort` thoroughly, because they appear all over the project.

## 1. `static` member functions

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

`static` means the function **doesn't belong to any object**; you call it directly as `TargetSelector::select(...)`. It's exactly like Erlang's `target_selector:select(State, Owner, Rule, Count, Trigger)`.

It returns **a list of indices**; see lesson 2, section 4 for why.

## 2. Two special rules: return straight away

```cpp
if (rule == TargetRule::self) {
    return state.units[owner_index].alive()
               ? std::vector<std::size_t>{owner_index}   // just yourself
               : std::vector<std::size_t>{};             // an empty list
}
if (rule == TargetRule::trigger_unit) {
    if (trigger_unit.has_value() && *trigger_unit < state.units.size() && ...) {
        return {*trigger_unit};                          // just write {}
    }
    return {};
}
```

- `return {*trigger_unit};` / `return {};` are **implicit construction**: the compiler knows the return type is `vector<size_t>`.
- Inside the conditional operator **the type must be written out**, because `?:` needs both branches to agree on a type first, and a bare `{}` has no type. Measured: `return alive ? {i} : {};` gives `expected primary-expression before '{' token`. (Lesson 3, section 8)

## 3. Filtering candidates

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

The same as an Erlang list comprehension:

```erlang
Candidates = [I || {I, U} <- Indexed, U#unit.side =:= TargetSide, alive(U), U#unit.targetable].
```

C++ has no list comprehensions, so you use "an empty vector + a `push_back` loop".

## 4. Lambdas: C++'s anonymous functions

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
[captures](parameters) -> return type { body }      the return type is usually omitted
```

The counterpart of Erlang's `fun(Left, Right) -> ... end`.

**The key difference is the capture list.** An Erlang fun automatically **copies** outside variables in. C++ makes you decide:

| Form | Meaning |
|---|---|
| `[]` | Capture nothing |
| `[x]` | **Copy** `x` |
| `[&x]` | **Reference** `x` |
| `[=]` | Copy everything used |
| `[&]` | Reference everything used |
| `[this]` | Capture the current object |

Measured:

```cpp
int round = 1;
auto by_value = [round]  { return round; };
auto by_ref   = [&round] { return round; };
round = 5;
// by_value=1 by_ref=5
```

The project almost always uses `[&]`, because these lambdas **are used on the spot and thrown away**. The danger is a lambda that is stored and outlives the variables it refers to: that's a dangling reference.

**Rule of thumb: a lambda used on the spot can use `[&]`; one that gets carried away should capture by value.** `[instance_id]` at `effect_system.cpp:12` copies just one integer, and a reader sees at a glance what it depends on.

## 5. Lowest-HP sorting: a lambda inside a lambda, all in integers

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
              if (lhs_ratio != rhs_ratio) return lhs_ratio < rhs_ratio;              // ① HP percentage
              if (lhs.config.position != rhs.config.position)
                  return lhs.config.position < rhs.config.position;                  // ② position
              return lhs.config.id < rhs.config.id;                                  // ③ ID
          });
```

- **A lambda can be stored in a variable** and then called like an ordinary function.
- **It compares percentages**: 3000 left out of 10000 (30%) is more wounded than 800 left out of 1000 (80%).
- **No floating point**: the last few bits of floating-point results can differ across CPUs, compilers and optimization levels; when two units' percentages are very close, Windows and Linux could pick different targets.
- **Split into `/` and `%`**: avoids overflow in an intermediate like `hp * 10000` (lesson 7).

## 6. `std::sort` comparators: the most dangerous part

### Rule 1: you must use `<`, never `<=`

The comparator must be a **strict weak ordering**, and the most basic requirement is that `compare(a, a)` returns `false`. With `<=`, equal elements each claim to "come before the other", and since `std::sort` skips bounds checks internally for speed, it runs straight out of bounds. Measured with 100 identical elements:

```
ERROR: AddressSanitizer: heap-buffer-overflow
```

The standard library's debug mode (`-D_GLIBCXX_DEBUG`) names the cause directly:

```
Error: comparison doesn't meet irreflexive requirements, assert(!(a < a)).
```

**Erlang's `lists:sort/2` convention is exactly `=<`; C++ must be a strict `<`.** It only triggers when several elements are equal, so ordinary tests may not catch it.

### Rule 2: the last comparison must break every tie, or the result isn't deterministic

`std::sort` is **not stable**: elements that compare equal have no guaranteed order afterwards. Measured: 40 units sorted by position (only 0 and 1):

```
sort       : 1000 1026 1024 1028 1022 1020   ← equal elements got shuffled
stable_sort: 1000 1002 1004 1006 1008 1010   ← the original order is kept
```

Worse, MSVC and GCC shuffle them differently. That's why **the last line of every comparator in the project compares `id`**: IDs are globally unique, so any two units can always be ordered and there is exactly one possible result. `acting_order` (`battle_state.cpp:456`) does the same: speed → position → ID.

Skill sorting uses `std::stable_sort` (`battle_state.cpp:374`) because skills have no unique tie-break field, and a stable sort keeps the original order from the config table. **Either have a unique tie-break field, or use a stable sort.**

## 7. Taking the first N

```cpp
if (rule != TargetRule::all_enemies && rule != TargetRule::all_allies) {
    const auto count = static_cast<std::size_t>(std::max(0, requested_count));
    if (candidates.size() > count) {
        candidates.resize(count);          // keep only the first count
    }
}
return candidates;
```

- `resize(n)` is like `lists:sublist(L, N)`.
- **`std::max` requires both arguments to have exactly the same type**. `0` and `requested_count` are both `int`, so it compiles; `int64_t` with `int32_t` gives `no matching function for call to 'max(int64_t&, int32_t&)'`. The fix is `std::max<std::int64_t>(a, b)`.
- `size_t` is unsigned, and a negative number converted to it becomes a huge positive one, so clamp with `std::max(0, ...)` before converting.
- `return candidates;` returns a local, which is moved automatically; no `std::move` needed.

## Summary

| Concept | Key point |
|---|---|
| `static` member functions | Need no object; like Erlang module functions |
| Lambdas | `[captures](parameters) { body }`, like a `fun` |
| Capture modes | `[&]` when used on the spot; `[x]` when carried away |
| Comparators | **Must use `<`**, the opposite of Erlang's convention |
| Determinism | Compare a unique ID last, or use `stable_sort` |
| Integer basis points | No floating point, avoiding platform differences |
| `std::max` | Both arguments must have the same type |

Next: [Lesson 6: The effect system](06-effect-system.md)
