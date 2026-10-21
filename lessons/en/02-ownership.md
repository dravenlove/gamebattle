# Lesson 2: Values, references, pointers, const, std::move

[中文](../02-ownership.md) | **English**

> Files: `src/battle_runtime.hpp`, `src/battle_state.cpp`
> Exercise code: `lessons/lesson2.cpp` (optional)

## 1. The core question: is this variable mine, or borrowed?

In Erlang all data is immutable values. When you pass a map to another function you don't worry about it being changed, and you don't care when it gets collected.

C++ is different. Every variable you declare and every parameter you write answers one question: **do I own this data, or am I borrowing someone else's?** Get it wrong and you either copy piles of data for nothing or read memory that has already been freed.

### Five forms

| Form | Meaning | Example in the project |
|---|---|---|
| `T` | I own a copy (copied or moved in) | `Side other(Side side)`, `RuntimeUnit::config` |
| `const T&` | Borrowed, read-only | `validate_request(const BattleRequest&)` |
| `T&` | Borrowed, may modify | `EffectSystem(BattleState& state)` |
| `T*` / `const T*` | Borrowed, **may be null** | `const Skill* selected = nullptr` |
| `shared_ptr<T>` | Owned jointly by several holders | `Effect::buff` |

**How to choose**:
- Small types (integers, enums, `size_t`) are passed by value.
- Large objects: `const T&` to read, `T&` to modify.
- A borrow that may be "nothing": `T*`.
- To take ownership: accept by value, then use `std::move`.

A `&` reference is "another name for a variable": it must be bound at birth, can't be rebound later and can't be null. A `*` pointer is a variable holding an address: it can be null, can be re-pointed, and member access is written `->`.

## 2. Three ownership decisions in the project

```cpp
struct RuntimeUnit {
    UnitConfig config;              // ① value: a copy
};
class BattleState {
    const BattleRequest& request;   // ② const reference: read-only borrow
};
class EffectSystem {
    BattleState& state_;            // ③ reference: writable borrow
};
```

**① Why does `RuntimeUnit` copy `config`?** `battle_state.cpp:395` sorts skills by priority, and sorting modifies data, but the request is `const` and can't be touched. So each battle copies it first and sorts only its own copy. It's the same idea as an Erlang process keeping a copy of the data in its own State.

**② Why does `BattleState` borrow the request?** Requests are large and copying is wasteful. The precondition for borrowing: **the borrowed request must outlive the `BattleState`**. `return runtime::BattleRunner(request).run();` at `engine.cpp:132` guarantees that.

**③ `EffectSystem` borrows its "sibling member".** Inside `BattleRunner`, `effects_` holds a reference to `state_`. That's fine, but it carries a hidden ordering requirement; see pitfall 3.

## 3. Why `shared_ptr<const BuffSpec>` and not `BuffSpec*`

Split the question in two: `shared_ptr` decides **who frees it**, and `const` decides **who can modify it**.

### First half: `shared_ptr` answers "who calls delete"

C++ has no garbage collector. Every block of heap memory must be freed by someone, and **exactly once**: free it too early and you read freed memory; forget and it leaks; free it twice and you crash. A raw pointer is just an address and **records no owner**.

The README says that `load_config/2` switches to a new config package and **battles already in progress keep using the old config snapshot**. Simulating that with a raw pointer (checked with AddressSanitizer):

```cpp
auto* old_config = new BuffSpec{801, "poison"};   // "poison" from the old config package
Effect effect{old_config};                        // a battle is using it
delete old_config;                                // hot reload: swap in the new package, free the old one
std::cout << effect.buff->name;                   // the battle keeps reading
```
```
ERROR: AddressSanitizer: heap-use-after-free
```

Without the checker it might happen to work, print garbage, crash, or quietly compute the wrong damage and break determinism. In NIF mode it would also take down the whole BEAM.

With `shared_ptr`:

```
use_count after load   = 1     the config store holds one
use_count in battle    = 2     the battle takes another
store released, battle still reads: poison (use_count=1)
battle done, freed             the last holder lets go; only now is it freed
```

`shared_ptr` keeps a **reference count** inside: copying adds 1, destroying subtracts 1, and at 0 it deletes automatically. This is the same mechanism Erlang uses for large binaries (>64 bytes).

In the project, the holders of the same `BuffSpec` include `ConfigStore::buffs_`, the `Effect`s of several skills, the `ActiveBuff`s on units, the `Effect`s of passives… Their lifetimes all differ, and there is no single "owner who leaves last". That is exactly where `shared_ptr` belongs.

### Second half: `const` guarantees "nobody can change it"

```cpp
auto spec = std::make_shared<const BuffSpec>();
spec->duration = 99;
// error: assignment of member 'BuffSpec::duration' in read-only object
```

1. **The definition is shared, so changing it in one place changes it everywhere.** With `const`, code like "make this poison last one more round" by editing the definition simply doesn't compile.
2. **Only read-only data can be shared safely across threads.** In NIF mode several threads run battles at the same time and read the same `ConfigStore`. Reads need no lock.
3. **Mutable state lives elsewhere.** Remaining rounds, stacks and source all go into `ActiveBuff`:

```cpp
struct ActiveBuff {
    std::shared_ptr<const BuffSpec> definition;  // the shared, read-only "template"
    std::int32_t remaining{0};                    // this battle's, this unit's own mutable state
    std::int32_t stacks{1};
};
```

### The costs

- **Reference cycles leak.** A holds B, B holds A, and the count never reaches 0. That's why both the config compiler and `validate_request` reject buff reference cycles (lessons 8, 10). Erlang's GC handles cycles; `shared_ptr` doesn't.
- **There is overhead.** 16 bytes, and copying modifies the count atomically. That's why parameters are usually written `const std::shared_ptr<const BuffSpec>&`.

### When to use a raw pointer

| | Meaning | When |
|---|---|---|
| `shared_ptr<T>` | **Joint ownership**: while I exist, it exists | Uncertain lifetime, several holders |
| `T*` or `T&` | **Borrowing**: I only look at it and don't manage its life | You can be sure the object outlives you |

`const Skill* selected` in `execute_action` lives only inside the function, and the skill it points to belongs to the unit's config, which certainly lives longer, so a borrow is enough.

**The test: ask yourself, "if the other side is freed first, could I still be using it?"** If yes, `shared_ptr`; if no, a raw pointer or reference.

## 4. Indices, pointers, references: which survive vector growth and erasure

Links between units in the project are always **indices** (`std::size_t actor_index`), never stored pointers or references. The reason: when a `vector` is full, it **moves everything to a new block of memory**.

### A vector is a row of contiguous seats

```
address 0x...040       0x...050
     ┌──────────────┬──────────────┐
     │ 1001 / 1800  │ 2001 / 1500  │     capacity = 2 (full)
     └──────────────┴──────────────┘
```

`size()` is how many seats are taken; `capacity()` is how many seats there are. When seats run out, the vector allocates a larger block, moves the elements over and **frees the old memory**. Measured:

```
before growth: array start=0x503000000040  ptr=0x503000000050  capacity=2
after growth:  array start=0x506000000020  ptr=0x503000000050  capacity=4
units[index].id = 2001  (the index is still correct)
ERROR: AddressSanitizer: heap-use-after-free on address 0x503000000050
```

- **A pointer (and a reference is essentially an address too) stores an absolute address**, and nobody tells it about the move.
- **An index stores "which one"**, and every `units[index]` recomputes from the **current** start address: `start + index × element size`.

An analogy: the whole class moves from building A to building B. The pointer remembers "building A, room 205"; the index remembers "student number 2 in the class".

Erlang never lets you hold a memory address at all; you only ever have values, Pids or ETS keys. C++ hands you the address, and "is it still valid?" is your responsibility.

### Indices aren't a cure-all either: erasure shifts them

```cpp
std::vector<Buff> buffs{{1, 801}, {2, 802}, {3, 803}};
buffs.erase(buffs.begin());          // 801 expired and was erased
```
```
is index=2 still valid? no, out of range
index=1 used to mean 802, now buffs[1].buff_id=803     ← silently points at a different buff
```

### So the project uses two schemes for two kinds of container

- **`units` uses indices.** Units are added once at construction and during the battle **only die (`hp = 0`); they are never erased or added**, so indices are stable for the whole battle.
- **`buffs` uses unique IDs.** Buffs expire and get dispelled, so they get `erase`d. Each instance has a monotonically increasing `instance_id` used to look it up again; an erased one can't be found, and that is detectable (lesson 6).

| What you remember | Growth (`push_back`) | Erasing from the middle (`erase`) | Used where |
|---|---|---|---|
| Pointer, reference | ❌ reads freed memory | ❌ everything after it is invalid | Short stretches of code where neither happens |
| Index | ✅ | ❌ silently shifted | `units` |
| Unique ID + lookup | ✅ | ✅ an erased one isn't found | `buffs` |

> **Watch out when you implement summons later**: in `take_side_turn`, `auto& actor = state_.units[actor_index];` is used throughout the whole action, and its safety rests entirely on "`units` doesn't grow or shrink during a battle". The moment you `push_back` into `units`, references like that may become invalid. Either reserve slots in advance or re-fetch by index every time.

## 5. Five pitfalls (all measured)

### Pitfall 1: `auto` copies; only `auto&` is a reference

```cpp
auto  copy = units[0];  copy.hp -= 30;   // units[0].hp = 100  ← modified the copy
auto& ref  = units[0];  ref.hp  -= 30;   // units[0].hp = 70
```

The damage was "dealt", but to a copy that's thrown away immediately. It compiles without complaint. In the project: write `auto&` to modify, `const auto&` to read.

### Pitfall 2: references taken before adding to a vector may become invalid

See section 4.

### Pitfall 3: members are initialized in declaration order, regardless of how the initializer list is written

```cpp
struct Runner {
    Effects effects_;   // declared first → constructed first
    State   state_;     // declared later → constructed later
    Runner() : state_(), effects_(state_) {}
};
```
```
warning: 'Runner::state_' will be initialized after [-Wreorder]
effects saw round = 0 (expected 7)
```

In `battle_runtime.hpp:179-180`, `state_` must be written before `effects_`. **Don't ignore `-Wall` warnings.**

### Pitfall 4: a reference member bound to a temporary

```cpp
Runner runner(Request{});         // the temporary Request is destroyed at the end of this line
runner.first_hp();                // 💥 stack-use-after-scope
```

`BattleRunner(request).run()` puts construction and the call in **the same expression**, so it's safe. Split it into two lines with a temporary argument and it breaks, and the compiler **gives no warning**.

### Pitfall 5: `std::move` doesn't move anything by itself

```cpp
std::vector<int> result = std::move(events);
// result.size=10000  events.size=0  same buffer=1
std::vector<int> copy = result;
// copy same buffer=0
```

`std::move` is only a type cast meaning "I no longer need this; you may take its internals". The receiver takes over that memory directly without copying a single element. The moved-from object is still valid, but **don't read its contents again**.

Three typical places in the project:

```cpp
units.push_back(std::move(runtime));      // ① a local gives up ownership
void emit(std::string type, ...) {        // ② accept by value, then move it in
    result.events.push_back(Event{.type = std::move(type), ...});
}
BattleResult finish() {
    return std::move(result);             // ③ returning a member needs an explicit move
}
```

Point ③: with `return local_variable;` the compiler moves automatically; but `result` is a **member**, the compiler won't take it on its own, and without `std::move` the whole events array is copied.

## 6. const member functions

```cpp
bool alive() const { return hp > 0; }          // promises not to modify the object
Stats effective_stats(std::size_t index);      // no const
```

`effective_stats` sounds like a query but is **not** `const`, because it writes the cache fields `cached_stats` and `stats_dirty`. `const` describes "does it modify the object", not "does it look like a getter". Only `const` member functions can be called on a `const` object.

## Optional exercises

```bash
g++ -std=c++20 -Wall -Wextra -Wpedantic -g -fsanitize=address lessons/lesson2.cpp -o lesson2 && ./lesson2
```

Each exercise breaks the code on purpose so you can watch the consequences:
1. Change `auto& target` in `apply_damage` to `auto target`.
2. Swap the declaration order of `state_` and `effects_` in `BattleRunner`.
3. In `run()`, take `auto& actor = state_.units[0];`, then `push_back`, then use `actor`.
4. Remove the `std::move` in `finish()` and think about what extra work happens.

## Summary

| Concept | Key point |
|---|---|
| value / `const&` / `&` / `*` / `shared_ptr` | Own it / read-only borrow / writable borrow / nullable borrow / joint ownership |
| `shared_ptr<const T>` | Reference counting handles freeing, `const` handles modification; beware cycles |
| Index vs pointer | After growth, indices stay valid and pointers don't; after erasure neither is reliable, so use unique IDs |
| `auto` vs `auto&` | `auto` is a copy |
| Member initialization order | Declaration order |
| `std::move` | Only "you may take it"; write it explicitly when returning a member |

Next: [Lesson 3: Classes and deterministic random numbers](03-class-and-random.md)
