# Lesson 1: Reading the domain model through Erlang eyes

[中文](../01-domain-model.md) | **English**

> File: `include/gamebattle/engine.hpp`
> Exercise code: `lessons/lesson1.cpp` (optional)

`engine.hpp` holds only data and knows nothing about Erlang. Think of it as a set of records in `gamebattle.hrl`.

## 1. The three layers of the system

```
Erlang map ─term_to_binary─▶ ETF bytes ─{packet,4}─▶ port_main.cpp
                                                      │ wire.cpp: ETF → BattleRequest
                                                      ▼
  Erlang ◀─ ETF bytes ◀─ wire.cpp: BattleResult → ETF ◀─ Engine::simulate()
```

| Layer | Files | Responsibility |
|---|---|---|
| Domain model | `engine.hpp` | Pure data: requests, units, skills, buffs, results |
| Runtime | `battle_runtime.hpp` and `engine/battle_state/effect_system/target_selector.cpp` | Battle rules |
| Transport | `term/wire/port_main/nif.cpp` | ETF encoding/decoding, talking to Erlang |

The course goes "inside out": domain model first, then the runtime, and the transport layer last.

## 2. Headers ≈ `.hrl`

```cpp
#pragma once                       // include this header only once
#include <cstdint>                 // angle brackets: the standard library
#include "gamebattle/engine.hpp"   // double quotes: the project's own files
```

`#include` literally pastes the file's contents in. That's why a `.hpp` holds only declarations (struct definitions, function signatures) and the implementations go in the `.cpp`.

## 3. namespace ≈ module-name prefix

- `gamebattle::Engine` is like the `gamebattle:` prefix in `gamebattle:simulate`.
- `namespace gamebattle::runtime { }` is the C++17 nested form.
- An anonymous `namespace { ... }` in a `.cpp` is the equivalent of **unexported private functions** in an Erlang module: visible only in that file. `validate_request` at `battle_state.cpp:11` lives in one.

## 4. Fixed-width integers: Erlang never worries about them, C++ must

```cpp
using UnitId = std::uint64_t;       // ≈ -type unit_id() :: non_neg_integer().
using BasisPoints = std::int32_t;   // 10000 = 100%
```

- Erlang integers are bignums and never overflow. C++'s `std::int64_t` tops out at about 9.2×10¹⁸.
- **Signed integer overflow is undefined behavior**, and the result is unpredictable. That's why the project uses `saturating_add` everywhere (lesson 7).
- `using` only creates an alias, not a new type. `UnitId` and `uint64_t` mix freely and the compiler won't stop you.

## 5. `enum class` ≈ atoms

```cpp
enum class Side : std::uint8_t { attacker = 0, defender = 1 };
```

- You must write `Side::attacker`; the names don't leak into the enclosing scope.
- **No implicit conversion to an integer**: to print one you first write `static_cast<int>(side)`.
- `: std::uint8_t` means it occupies 1 byte.
- Why does `EffectKind` spell out `= 0, = 1 …`? These numbers are written into the `.gbcfg` binary config file (lesson 10). Once they are fixed, reordering the enumerators later won't change what old files mean. Erlang atoms compare by name, so Erlang doesn't have this problem.
- An enum is a closed set. If a `switch` misses a case, `-Wall` warns at **compile time**; Erlang only reports `case_clause` at run time (lesson 6).

## 6. struct with defaults ≈ record with defaults

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

- **A built-in type without an initial value is a chunk of random garbage memory.** Erlang has no such concept, and this is why `engine.hpp` gives every integer a `{0}`.
- Class types such as `std::string` and `std::vector` are automatically constructed empty, so `std::string name;` doesn't need `{}`.
- Braces are used instead of `=` because braces forbid conversions that lose precision (see pitfall 1 in section 11).

## 7. Container comparison

| Erlang | C++ | Notes |
|---|---|---|
| list `[A, B]` | `std::vector<T>` | Contiguous memory, O(1) access by index; holds only one type |
| binary `<<"poison">>` | `std::string` | **Its contents are mutable**; it's really a byte array, so UTF-8 is stored just fine |
| map `#{K => V}` | `std::unordered_map<K, V>` | **Iteration order is unspecified** |
| ordered map | `std::map<K, V>` | Iterates in key order |
| `undefined \| V` | `std::optional<T>` | Common calls: `has_value()`, `*opt`, `value_or(x)` |
| Reference-counted sharing of a large binary | `std::shared_ptr<const T>` | Lesson 2 |

The point that matters most for determinism: `unit_index` in `BattleState` is an `unordered_map`, but it is **only used for lookup by ID**. Every iteration over units goes through the ordered `std::vector<RuntimeUnit> units`. If you iterated the `unordered_map`, the same seed could produce different battle reports.

## 8. The forward declaration `struct BuffSpec;`

`engine.hpp:104` has a lonely line, `struct BuffSpec;`. It solves two problems: **a name must be seen before it is used**, and **a struct's size must be known at compile time**.

### First, how the three structs refer to each other

```
Effect                     one effect, e.g. "apply the poison buff"
 └─ buff ──────────────┐   points to a buff definition
                       ▼
BuffSpec                   a buff definition, e.g. "poison"
 └─ reactions: vector<BuffReaction>
      └─ effects: vector<Effect>   ← back to Effect again
```

In Erlang this is no problem at all:

```erlang
-record(effect,    {kind, buff}).        %% the buff field can hold any term
-record(buff_spec, {id, reactions}).
```

Record fields are untyped; every term is essentially a uniformly sized "slot", and for large data the slot holds a reference to the heap. Neither of those is true in C++.

### Rule 1: the compiler reads top to bottom and doesn't know names it hasn't seen

Remove the forward declaration and compile (measured):

```
error: ISO C++ forbids declaration of 'type name' with no type
error: template argument 1 is invalid
```

Could `BuffSpec` move before `Effect`? No. `BuffSpec` indirectly needs `Effect`, so moving it up just means `Effect` is the one not seen yet. With a cycle, whatever order you choose, one side is used first. `struct BuffSpec;` tells the compiler up front: "`BuffSpec` is a struct; its contents come later."

### Rule 2: structs are stored by value, so their size must be known at compile time

By default, C++ struct members are **embedded by value**. If you wrote `BuffSpec buff;`, the size of `Effect` would include `BuffSpec`, which (indirectly) contains `Effect`… an infinitely recursive size. The compiler refuses:

```
error: field 'buff' has incomplete type 'BuffSpec'
```

**A pointer breaks the cycle because a pointer has a fixed size.** Measured:

```
sizeof(BuffSpec*)   = 8    raw pointer: one memory address
sizeof(shared_ptr)  = 16   object address + control-block address
sizeof(Effect)      = 24   int(4) + alignment padding(4) + shared_ptr(16)
```

This is in fact what Erlang does by default: large data lives on the heap and the slot holds only a reference. C++ makes **you decide** whether to embed by value or store a pointer.

### Incomplete types: what you can do with only a declaration

| Operation | Allowed? | Why |
|---|---|---|
| `BuffSpec*`, `BuffSpec&`, `shared_ptr<const BuffSpec>` | ✅ | Fixed size, the contents don't matter |
| `BuffSpec buff;` stored by value | ❌ | Size unknown |
| `buff->id` member access | ❌ | Members unknown (measured: `invalid use of incomplete type`) |
| `sizeof(BuffSpec)` | ❌ | Size unknown |

So every piece of the project that actually reads a `BuffSpec`'s contents lives in a `.cpp`, by which point `BuffSpec` is long since complete.

The order in `engine.hpp:104-132` is the only arrangement that works: one edge in the cycle must be a pointer, and the forward declaration goes before that edge.

```cpp
struct BuffSpec;                  // ① declare the name first
struct Effect {                   // ② only uses a pointer to BuffSpec → OK
    std::shared_ptr<const BuffSpec> buff;
};
struct BuffReaction {             // ③ uses Effect through a vector → OK, Effect is complete
    std::vector<Effect> effects;
};
struct BuffSpec {                 // ④ the full definition; the cycle closes here
    std::vector<BuffReaction> reactions;
};
```

The pointer brings another benefit: **each buff definition is stored only once.** If 100 skills all apply "poison", they all point to the same `BuffSpec`. For why it's `shared_ptr<const BuffSpec>` rather than a raw pointer, see lesson 2.

## 9. C++20 designated initializers ≈ building a map

```erlang
#{id => 1001, position => 1, final_stats => #{hp => 1800}}
```
```cpp
UnitConfig{.id = 1001, .position = 1, .final_stats = {.hp = 1800}}
```

**Rule: fields must be written in declaration order.** You can skip fields in the middle; skipped fields take their defaults.

## 10. Reading the `Engine` signature

```cpp
BattleResult simulate(const BattleRequest& request) const;
```

- `const BattleRequest&`: passed by reference, not copied, and guaranteed not to be modified. Because Erlang data is immutable, Erlang arguments have these semantics naturally.
- The trailing `const`: this method doesn't modify the `Engine` itself. So `Engine` is stateless, and many threads can share one.
- Returning `BattleResult` by value: it reads like a copy, but the compiler uses a move or return value optimization, so the whole events array is not copied.

## 11. Pitfalls the compiler catches for you (all measured)

```bash
g++ -std=c++20 -Wall -Wextra -Wpedantic lessons/lesson1.cpp -o lesson1 && ./lesson1
```

1. `std::int32_t max_rounds{50.5};` errors with `narrowing conversion`. This is the benefit of `{}`; with `= 50.5` it would be silently truncated to 50.
2. `std::cout << Side::attacker;` errors with `no match for 'operator<<'`, because an enum class doesn't convert to an integer automatically.
3. `P{.y = 1, .x = 2}` errors with `designator order ... does not match declaration order`.
4. With `-Wextra`, omitting a field **that has no default member initializer** (such as a `std::string`, `std::vector` or `std::optional` without `{}`) in a designated initializer triggers a `missing initializer` warning; omitting a field with a default (such as `int rounds{0};`) doesn't. That is why the exercise code spells out `.forced_first_side = std::nullopt`. The project has one too: when building you'll see `battle_state.cpp:382: warning: missing initializer for member 'BattleResult::reason'`. The omitted field is still initialized normally and the warning is harmless, but you should be able to read it (lesson 11, section 4).

## Optional exercises

1. Add `enum class Trigger` (copied from `engine.hpp:26`, 11 hook points in total) and `struct Passive`, and give `UnitConfig` a `std::vector<Passive> passives;`.
2. Write `const char* to_string(Side side)` with a `switch`; delete one case on purpose and see what `-Wall` says.
3. Think about it: why is `hp` an `int64` while `crit_rate_bp` is an `int32`? (Hint: look at the value limits in `battle_state.cpp:253-268`.)

## Summary

| C++ | Erlang |
|---|---|
| `.hpp` / `#include` | `.hrl` / `-include` |
| `namespace` | Module-name prefix |
| Anonymous `namespace {}` | Unexported functions |
| `enum class` | Atoms (but a closed set) |
| `struct` + default member values | record + defaults |
| `std::vector` / `std::unordered_map` / `std::optional` | list / map / `undefined \| V` |
| Designated initializers `{.id = 1}` | `#{id => 1}` |
| Forward declarations | Not needed |

Next: [Lesson 2: Values, references, pointers, const, move](02-ownership.md)
