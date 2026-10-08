# Lesson 3: Classes and deterministic random numbers

[中文](../03-class-and-random.md) | **English**

> Files: `src/battle_runtime.hpp:24-32` (`Random`), `src/battle_state.cpp:359-389`

## 1. class and struct: one default apart

```cpp
struct A { int x; };   // members are public by default
class  B { int x; };   // members are private by default
```

That is the only syntactic difference. The project's convention:

- **`struct`**: plain data; every field can be read and written freely, like an Erlang record. `engine.hpp` is all structs.
- **`class`**: has **invariants** to protect inside. For example `Random`:

```cpp
class Random {
public:
    explicit Random(std::uint64_t seed);
    std::uint64_t next();
    bool roll(BasisPoints chance_bp);
private:
    std::uint64_t state_;     // can't be changed from outside
};
```

Outside code can only advance the state through `next()`, like an Erlang module that exports only its API. A trailing `_` on a member name is the project's habit for marking private members.

## 2. Why write our own RNG instead of using the standard library

The same seed must produce exactly the same battle report on a **Windows dev machine (MSVC)** and a **Linux production machine (GCC)**.

The standard-library trap: the algorithms of random engines (such as `std::mt19937`) are fixed by the standard, but "distributions" (such as `std::uniform_int_distribution`) **specify only the effect, not the algorithm**. MSVC and GCC implement them differently, so the same seed can give different numbers.

So the project writes its own **SplitMix64** (the generator behind Java's `SplittableRandom`):

```cpp
std::uint64_t Random::next() {
    state_ += 0x9e3779b97f4a7c15ULL;                          // ① add a fixed constant to the state
    auto value = state_;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL; // ② shift-xor then multiply to scramble the bits
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}
```

You don't need to understand where the constants come from. Just know: the state is a single 64-bit integer, and everything is add, multiply, shift and xor, so every platform gets the same result.

- The `ULL` suffix means `unsigned long long`, guaranteeing 64-bit unsigned arithmetic.
- `30U` is an unsigned 30.
- `^` is xor (Erlang's `bxor`), `>>` is right shift (`bsr`).

## 3. Unsigned overflow is safe; signed overflow isn't

`state_ += constant` runs over and over and will exceed the maximum sooner or later. That's **intentional** (measured: `uint64 max + 1 = 0`):

| | On overflow | Use in the project |
|---|---|---|
| Unsigned (`uint64_t`) | **Well-defined**: modulo 2⁶⁴, wraps around | The RNG wraps on purpose |
| Signed (`int64_t`) | **Undefined behavior (UB)** | Damage and HP all use saturating arithmetic (lesson 7) |

## 4. `roll()` and what determinism really means

```cpp
bool Random::roll(BasisPoints chance_bp) {
    if (chance_bp <= 0)            return false;   // doesn't call next()
    if (chance_bp >= kBasisPoints) return true;    // doesn't call next()
    return static_cast<std::int64_t>(next() % kBasisPoints) < chance_bp;
}
```

Modulo 10000 gives 0–9999; with `chance_bp = 1500`, landing in 0–1499 counts as a hit, exactly 15%.

**The key: determinism depends on the order and number of `next()` calls.**

```
seed=42 → 1st number: break the first-move tie → 2nd number: does skill A trigger? → 3rd number: hit? → 4th number: crit? ...
```

- Rolls at 0% and 100% **don't consume a random number**. So configuring a skill to trigger 100% of the time doesn't disturb any of the later rolls.
- **If a code change adds or removes one `roll()`, every later roll shifts.** The same seed gives different battle reports on the old and new engines. "Same seed, same result" holds only **within one engine version**. Replaying old battle reports in production requires keeping that engine version around, or recording the engine version in the report.

The Erlang counterpart: put the `rand` state explicitly into the State and pass it all the way down, rather than relying on the process dictionary. `BattleState` holds the one and only `random`, and every module draws numbers from it.

## 5. Constructor initializer lists

```cpp
Random::Random(std::uint64_t seed) : state_(seed) {}
//                                 ^^^^^^^^^^^^^^ initializer list
```

The part after the colon runs before the function body is entered and constructs members directly from the given values. Two kinds of member **can only** be initialized there: **reference members** (which must be bound at birth) and **const members**.

The `BattleState` constructor (`battle_state.cpp:379`):

```cpp
BattleState::BattleState(const BattleRequest& request_value)
    : request(request_value),                          // reference: must be bound here
      random(request_value.seed),                      // calls Random's constructor
      result{.battle_id = request_value.battle_id,     // C++20 designated initializers
             .seed = request_value.seed,
             .source_battle_id = request_value.initial_conditions.source_battle_id} {
    validate_request(request);                         // the body: every member is ready
    add_formation(request.attacker, Side::attacker);
    add_formation(request.defender, Side::defender);
    apply_initial_conditions();
}
```

Reminder: **members are initialized in declaration order** (lesson 2, pitfall 3).

## 6. A constructor that throws: the object is either complete or doesn't exist

`validate_request` throws when it finds something invalid. When a constructor throws, C++ guarantees the object **is treated as never having existed**: the members already constructed are destroyed automatically, and the caller can't get hold of a "half-built" object. So if a `BattleState` exists, its data has definitely passed validation.

The closest thing in Erlang is `init/1` returning `{stop, Reason}`.

## 7. `explicit`: stop the compiler from converting behind your back

C++'s default rule: **a constructor that takes one argument is also an "automatic type conversion rule".**

```cpp
class Random { public: Random(std::uint64_t seed); };   // no explicit
void simulate(std::uint64_t battle_id, const Random& rng);
```

Accidentally swapping the two arguments at a call site:

```cpp
std::uint64_t battle_id = 3001, seed = 42;
simulate(battle_id, Random(seed));   // correct
simulate(seed, battle_id);           // slip of the hand: swapped
```

**Without `explicit`**: it compiles, and `-Wall -Wextra` gives no warning. The result (measured):

```
battle_id=3001 seed=42
battle_id=42 seed=3001     ← the battle ID and the seed were silently swapped
```

The compiler saw that the second parameter needs a `Random`, had a `uint64_t` in hand, and called `Random(battle_id)` automatically.

**With `explicit`**: compilation fails and points straight at the offending line:

```
error: invalid initialization of reference of type 'const Random&' from expression of type 'uint64_t'
```

Every single-argument constructor in the project is `explicit`: `Random`, `BattleState`, `EffectSystem`, `BattleRunner`. Imagine `BattleState` without it: pass a `BattleRequest` where a `BattleState&` is expected, and the compiler quietly builds an entire new battle.

**Rule of thumb: mark every single-argument constructor `explicit`.** Erlang never converts types implicitly; `explicit` makes C++ as strict as Erlang here.

## 8. Explicit vs implicit construction

There is exactly one difference: **did you write the class name yourself, or did the compiler fill it in?**

**Explicit construction**: the class name is visible in the code.

```cpp
Random a(42);                   // class name + parentheses
Random b{42};                   // class name + braces
auto c = Random(42);
simulate(Random(42));
static_cast<Random>(42);
```

**Implicit construction**: there's only a value; the compiler notices the types don't match and calls the constructor for you. It happens in only four places:

```cpp
Random d = 42;                  // ① = initialization
simulate(42);                   // ② passing an argument
Random make() { return 42; }    // ③ return
Random make() { return {42}; }  // ④ a braced list
```

`explicit` forbids exactly these four. Not even braces are allowed (measured): `converting to 'Random' from initializer list would use explicit constructor`.

### Implicit construction isn't always bad

The project relies on it heavily (measured, these compile):

```cpp
emit("damage");                                   // const char*   → std::string
trigger_owner(actor_index, ...);                  // size_t        → std::optional<size_t>
trigger_all(Trigger::battle_start, std::nullopt); // nullopt       → optional
apply_buff(..., std::make_shared<BuffSpec>());    // shared_ptr<T> → shared_ptr<const T>
```

**The test: does the value mean the same thing before and after the conversion?** A literal becoming a string, "a value" going into "maybe a value", writable becoming read-only: the meaning is unchanged, so implicit is fine. "An integer becomes a random number generator" is too big a jump in meaning, so it must be explicit.

### One place where you must write explicit construction by hand

`battle_state.cpp:506-510`:

```cpp
return found == unit_index.end() ? std::nullopt
                                 : std::optional<std::size_t>(found->second);
```

Written as `: found->second`, it fails to compile:

```
error: operands to '?:' have different types 'const std::nullopt_t' and 'std::size_t'
```

The conditional operator **first** requires both branches to agree on one type, and **only then** considers the return type. An if statement doesn't need this, because each `return` performs its own implicit conversion.

### The other direction: can an object convert implicitly to another type?

```cpp
std::optional<int> found = 5;
if (found) { ... }      // ✅ conversion to bool is allowed in if/while/!
bool b = found;         // ❌ error: cannot convert 'std::optional<int>' to 'bool'
```

The standard library allows it only in the clear-cut "does it have a value" context, preventing code like `int x = found + 1;` from quietly computing the wrong thing.

| | Explicit construction | Implicit construction |
|---|---|---|
| Form | The class name is visible: `Random(42)` | Only a value: `= 42`, an argument, `return 42` |
| Who decides to call the constructor | You | The compiler |
| `explicit` constructor | ✅ | ❌ |
| Suited to | The meaning changes | The meaning stays the same |

## Summary

| Concept | In one sentence | Erlang counterpart |
|---|---|---|
| class and struct | Differ only in default access | Exported API vs internal state |
| SplitMix64 | A hand-written RNG that's identical across platforms | Passing the `rand` state explicitly |
| Unsigned overflow | Defined, wraps around; signed overflow is UB | Integers are unbounded |
| Determinism | Depends on the order and count of `next()` calls | Same |
| Initializer lists | Reference and const members must be initialized here | None |
| A throwing constructor | The object is either complete or doesn't exist | `init/1` returning `{stop, Reason}` |
| `explicit` | Forbids implicit conversion through single-argument constructors | Erlang never converts implicitly anyway |

Next: [Lesson 4: The round loop](04-battle-loop.md)
