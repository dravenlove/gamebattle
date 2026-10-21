# Lesson 8: Validation and exceptions

[中文](../08-validation-exceptions.md) | **English**

> Files: `src/battle_state.cpp:103-334` (`validate_request`, **throws**), `src/wire.cpp:611-661` (`handle_etf`, **catches**)

## 1. The big picture: how an error travels all the way back to Erlang

```
Erlang: gamebattle:simulate(port, Request)
   │
   ▼  ETF bytes
handle_etf  ── try { ───────────────────────────────────────────────────────────────────┐
   ├─ term::decode           → malformed bytes:                throw DecodeError        │
   ├─ parse_request          → missing field or bad enum:      throw DecodeError        │
   └─ Engine::simulate                                                                  │
        └─ BattleState constructor                                                      │
             └─ validate_request → value out of range:         throw invalid_argument   │
   } catch ── translate by exception type ◀─────────────────────────────────────────────┘
   ▼
{error, #{type => invalid_request, message => <<"max_rounds must be between 1 and 10000">>}}
```

No matter how deep the error happens, it "bounces" all the way back to `handle_etf` and is translated into a tuple Erlang understands.

## 2. What happens after a throw

```cpp
void validate() { Guard g{"local object in validate"}; throw std::invalid_argument("max_rounds ..."); }
void build()    { Guard g{"local object in build"};    validate(); std::cout << "this line never runs\n"; }
int main() {
    try { build(); }
    catch (const std::invalid_argument& e) { std::cout << "caught: " << e.what() << '\n'; }
}
```

Measured:

```
  destroying local object in validate
  destroying local object in build
caught: max_rounds must be between 1 and 10000
```

1. The current function **stops immediately**;
2. It unwinds back up the call chain level by level, **automatically destroying each level's local objects** (stack unwinding);
3. Until it meets a `catch` whose type matches.

Step 2 is **RAII**: put resource release in a destructor, and the destructor runs whether the function returns normally or is interrupted by an exception. A vector's memory, a `shared_ptr`'s reference count, a lock (`std::unique_lock`) are all released automatically.

## 3. Compared with Erlang: the biggest difference is "no process isolation"

| | Erlang | C++ |
|---|---|---|
| Throwing | `throw(R)` / `error(R)` / `exit(R)` | `throw exception_object;` |
| Catching | `try ... catch Class:Reason -> ... end` | `try { ... } catch (const Type& e) { ... }` |
| Matched by | Pattern matching | **Type** (including base classes) |
| Nobody catches it | **Only the current process** exits and the supervisor restarts it | **The whole OS process** terminates |

Measured, when nobody catches it:

```
terminate called after throwing an instance of 'std::runtime_error'
  what():  nobody catches me
Aborted          process exit code=134
```

- **Port mode**: what dies is the separate `gamebattle_port` process, and the supervisor restarts it.
- **NIF mode**: the C++ runs **inside the BEAM process**, so what dies is **the entire Erlang node**.

That's why the outermost layer of `handle_etf` has a `catch (...)`: it's the **firewall** between C++ and Erlang.

## 4. The exception type hierarchy and catch order

```
std::exception                      base class of all standard exceptions, provides what()
 ├─ std::logic_error
 │   ├─ std::invalid_argument       ← what validate_request throws
 │   └─ std::out_of_range           ← ConfigStore::require_* when an ID isn't found
 └─ std::runtime_error
     └─ term::DecodeError           ← project-defined: ETF parsing failed
```

The catch chain in `handle_etf`:

```cpp
} catch (const term::DecodeError& error) {         // ① most specific first
    return term::encode(error_value("invalid_request", error.what()));
} catch (const std::invalid_argument& error) {
    return term::encode(error_value("invalid_request", error.what()));
} catch (const std::exception& error) {            // ② other standard exceptions
    return term::encode(error_value("internal_error", error.what()));
} catch (...) {                                    // ③ anything that isn't even a standard exception
    return term::encode(error_value("internal_error", "unknown C++ exception"));
}
```

catch clauses **match top to bottom, and a base class catches its subclasses**, so specific ones go first. Get it backwards and GCC warns:

```
warning: exception of type 'DecodeError' will be caught by earlier handler [-Wexceptions]
```

The two error categories let Erlang take different strategies:
- `invalid_request`: **the caller's fault**; don't retry, fix the request.
- `internal_error`: **something unexpected on the C++ side**; raise an alert and investigate.

## 5. Always catch by reference

```cpp
catch (const std::exception& e)   // ✅
catch (std::exception e)          // ❌
```

Measured, throwing `DecodeError("unit.kind must be hero")`:

```
by value:     what()=std::exception                ← the specific message is lost
by reference: what()=unit.kind must be hero
```

Catching by value copies only the base-class part, and the subclass data is "sliced off" (**object slicing**). GCC warns `catching polymorphic type by value`.

## 6. A custom exception takes three lines

```cpp
class DecodeError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;   // inherit the constructors
};
```

Why not just use `runtime_error`? So it **can be told apart in a catch**: `DecodeError` is classified as `invalid_request`, while other `runtime_error`s are `internal_error`. **An exception's type is the classification it carries**, like tagging error reasons with different atoms in Erlang.

## 7. Exception translation: catch, add context, rethrow

`wire.cpp:435-447`:

```cpp
try {
    ...unit.skills.push_back(configs->require_skill(id));
} catch (const std::out_of_range& error) {
    throw term::DecodeError(std::string("unit loadout: ") + error.what());
}
```

When `ConfigStore` can't find a skill ID it throws `out_of_range`, which would be classified as `internal_error`. But "the request referenced a skill ID that doesn't exist" is clearly the request's fault, so this adds context and rethrows it as a `DecodeError`. **The lower layer doesn't know who called it, so it can't tell whose fault it is; the upper layer knows, so the upper layer translates.**

## 8. How `validate_request` is structured

### Small lambdas as local predicates

```cpp
const auto valid_probability = [](BasisPoints value) {
    return value >= 0 && value <= kBasisPoints;
};
```

### Iterating over two objects at once

```cpp
for (const auto* formation : {&request.attacker, &request.defender}) {
    for (const auto& unit : formation->units) { ... }
}
```

`{&a, &b}` builds a temporary list of two pointers, like `lists:foreach(F, [Attacker, Defender])`. Storing pointers avoids copying the formations.

### The return value of `emplace` / `insert`: checking for duplicates on the way

```cpp
if (unit.id == 0 || !configs.emplace(unit.id, &unit).second) {
    throw std::invalid_argument("unit ids must be non-zero and unique across both sides");
}
```

It returns a pair `(iterator, whether it was actually inserted)`. **If the key already exists it doesn't overwrite and returns `false`**, so inserting and checking for duplicates happen in one step.

C++17 **structured bindings** unpack the pair:

```cpp
const auto [known, inserted] = buff_definitions.emplace(buff->id, buff);
```

Measured: `first inserted=1  second inserted=0  the map keeps poison-A`. In form it's Erlang's `{Known, Inserted} = ...`, but it only unpacks by position and can't match specific values at the same time.

## 9. Mutually recursive lambdas: you need `std::function`

```
validate_effect(effect) ──effect is add_buff──▶ validate_buff(buff)
       ▲                                           │
       └───────── each effect in each of the buff's reactions ┘
```

An ordinary lambda can't call itself (measured):

```cpp
auto depth_of = [&](std::size_t n) { return n == 0 ? 0 : 1 + depth_of(n - 1); };
// error: use of 'depth_of' before deduction of 'auto'
```

The compiler has to **see the whole lambda** to deduce the type of `depth_of`, but the body already needs it. The project's solution (`battle_state.cpp:129-133`):

```cpp
std::function<void(const Effect&, std::size_t)> validate_effect;                       // ① declare first, type spelled out
std::function<void(const std::shared_ptr<const BuffSpec>&, std::size_t)> validate_buff;

validate_buff = [&](const std::shared_ptr<const BuffSpec>& definition, std::size_t depth) {
    ...validate_effect(effect, depth + 1);    // ② captures the box itself by reference
};
validate_effect = [&](const Effect& effect, std::size_t depth) {
    ...validate_buff(effect.buff, depth + 1);
};
```

`std::function<ReturnType(Args...)>` is "a box that can hold any callable", with its type written out in advance. Declare two empty boxes first, then put the lambdas in; by the time either is actually called, both boxes are filled. Measured: the mutual recursion works.

Functions within an Erlang module can naturally call each other. `std::function` is slightly slower than a direct call, but validation runs once per request, so it doesn't affect performance.

## 10. Detecting buff reference cycles with three-color marking

```cpp
std::unordered_set<const BuffSpec*> validating_buffs;   // gray: being checked
std::unordered_set<const BuffSpec*> validated_buffs;    // black: done
                                                        // white: in neither, not visited yet
validate_buff = [&](...) {
    const auto* buff = definition.get();
    if (validated_buffs.contains(buff))  return;                         // black: skip
    if (validating_buffs.contains(buff)) throw ...("ownership cycle");   // gray: we came back around → a cycle!
    validating_buffs.insert(buff);                                       // mark gray
    for (each effect in its reactions) validate_effect(effect, depth + 1);
    validating_buffs.erase(buff);
    validated_buffs.insert(buff);                                        // mark black
};
```

Example: poison A's reaction applies burn B, and burn B's reaction applies poison A:

```
check A → mark A gray → found add_buff B
  → check B → mark B gray → found add_buff A
    → A is gray! → throw "ownership cycle"
```

- The sets store **pointers**, because the question is "is this the same object?". `definition.get()` extracts a raw pointer, a borrow for the duration of validation.
- `contains()` is new in C++20; previously you wrote `find(x) != end()`.
- The config compiler and `ConfigStore` also detect cycles, with a different algorithm (topological sort, lesson 10). Inline buffs in a request don't go through the compiler, so the runtime has to check again.

## 11. When to use exceptions and when not to

- **Use exceptions**: invalid input, corrupted config. Once it happens, the whole request is abandoned.
- **Don't use exceptions**: normal branches during a battle (a miss, a dead target, a buff not found); use return values, `optional`, `end()`, early returns.

C++ exceptions **cost almost nothing when not thrown and a lot once thrown**, so they suit "rare, and when it happens, give up on the whole operation".

`noexcept` promises the caller that no exception will be thrown:

```cpp
const BuffSpec* find_buff(std::uint32_t id) const noexcept;   // returns nullptr when not found
```

> **An advanced observation**: the catch blocks in `handle_etf` call `term::encode(...)`, which allocates memory. In extreme cases (memory exhausted) that step itself could throw `std::bad_alloc`, with no protection left outside. In Port mode that only makes the Port process exit and the supervisor restarts it; in NIF mode it would take the whole BEAM with it. The odds are tiny, but it shows that **the NIF boundary must never let an exception escape**, a much higher bar than for a Port, and one of the reasons the README says "only enable the NIF after thorough load testing and fuzzing".

## Summary

| Concept | Key point | Erlang |
|---|---|---|
| `throw` / `catch` | Matched by type; a base class catches subclasses | `try ... catch Class:Reason` |
| Stack unwinding / RAII | Local objects are destroyed automatically while unwinding | Resources freed when a process exits |
| Nobody catches it | `std::terminate`; **the whole process** terminates | Only the current process exits |
| catch order | Specific ones first | Clause order |
| Catch by reference | By value slices | None |
| Custom exceptions | The type is the category | Different atoms |
| Exception translation | The upper layer adds context and rethrows | `catch ... -> {error, ...}` |
| `emplace().second` | Insert and duplicate check in one step | `maps:is_key` + `maps:put` |
| Structured bindings | `auto [a, b] = ...` | `{A, B} = ...` |
| Mutually recursive lambdas | Declare `std::function`s first, then assign | Works naturally |
| Three-color marking | A gray node visited again → a cycle | `digraph:get_cycle/2` |

Next: [Lesson 9: Talking to Erlang](09-erlang-bridge.md)
