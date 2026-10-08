# Lesson 4: The round loop

[中文](../04-battle-loop.md) | **English**

> File: `src/engine.cpp` (135 lines)

`engine.cpp` decides only **the order of events** across a battle. How a single attack is resolved is handed to `EffectSystem` in lesson 6.

## 1. Mapping the README's flow to the code

```
README battle flow                     engine.cpp
─────────────────────────────────────────────────────────
Decide who moves first                 :12-39  compute initiative, decide first side
(one side already wiped out at start)  :43     finish_if_decided("initial_state")
battle_start passives                  :47     trigger_all(battle_start)
┌ each round ─────────────────────     :52     for (round = 1; ...)
│ round_start hook                     :57     trigger_all(round_start)
│ first side acts → second side acts   :62-74  for (side : order) take_side_turn
│ round_end hook, buffs expire         :80     trigger_all(round_end)
└ check for a winner                   finish_if_decided after every step
max rounds reached → draw              :86-92
```

## 2. Two ways of thinking about loops

In Erlang a "loop" is recursion with the state passed down as an argument; to end the loop you simply **stop recursing**:

```erlang
round_loop(Round, S) when Round > S#state.max_rounds ->
    finish(draw(max_rounds, S));
round_loop(Round, S0) ->
    S1 = trigger_all(round_start, S0#state{round = Round}),
    case finish_if_decided(S1, round_start) of
        {true, S}   -> finish(S);                      %% no more recursion = leave the loop
        {false, S2} -> round_loop(Round + 1, run_sides(S2))
    end.
```

C++ thinks differently: **there is exactly one state (`state_`), modified in place inside the loop**, and `break` / `continue` / `return` control when you leave:

```cpp
for (state_.round = 1;
     state_.round <= state_.request.max_rounds && !state_.event_limit;
     ++state_.round) {
    state_.phase = "round_start";
    effects_.trigger_all(Trigger::round_start, std::nullopt);
    if (state_.finish_if_decided("round_start")) {
        break;
    }
    ...
}
```

In Erlang each step `S0 → S1 → S2` is a new value; in C++ there is only one `state_` from start to finish, and any function call may change it. That leads to a rule that runs through the whole project: **after every step, check the state again.**

## 3. The three parts of a for loop, and the value after the loop

```cpp
for (init; condition; run after each iteration) { body }
```

Execution order: init once → check the condition → body → `++round` → check the condition again → …

The loop variable is **the member `state_.round`**, so `emit` can read the current round directly. The side effect: when the loop runs to completion, the final `++` overshoots the limit (measured: with `max_rounds = 50`, `round = 51` after the loop). That's why `engine.cpp:91` writes:

```cpp
state_.result.rounds = std::min(state_.round, state_.request.max_rounds);
```

## 4. `break` only leaves the innermost loop

```cpp
for (round ...) {                                  // outer: rounds
    for (const Side side : order) {                // inner: first side, second side
        take_side_turn(side);
        if (state_.finish_if_decided("all_units_defeated")) {
            break;                                 // ← only leaves the inner loop!
        }
    }
    if (state_.decided || state_.event_limit) {    // ← so check again here
        break;
    }
    ...round_end...
}
```

Measured:

```
round 1 side 0
round 1 side 1
round 2 side 0
  -> break           ← only skipped side 1 of round 2
round 3 side 0       ← the outer loop goes on into round 3 as usual!
round 3 side 1
```

Without the check on line 75, the code would carry on to `round_end` after a winner is decided, and even into the next round. C++ has no syntax for "break out of several levels at once"; the two common approaches both appear in this file:
- **Flags**: `decided`, `event_limit`.
- **Move the inner logic into a function and leave with `return`**: `take_side_turn`.

## 5. Early `return`: deal with the early-exit cases first

```cpp
if (state_.finish_if_decided("initial_state")) {
    return state_.finish();
}
effects_.trigger_all(Trigger::battle_start, std::nullopt);
if (state_.finish_if_decided("battle_start")) {
    return state_.finish();
}
```

These are **guard clauses**: the main flow doesn't have to indent level after level. In Erlang you'd usually write nested `case`s.

`finish_if_decided` means "check, and record while you're at it": with no winner it returns `false` and changes nothing; with a winner it writes `decided`, `winner`, `reason` and `rounds` and returns `true`. The string passed in appears unchanged in the result's `reason` field.

## 6. Deciding who moves first: optional, the conditional operator and a precedence trap

```cpp
if (state_.request.initial_conditions.forced_first_side.has_value()) {
    state_.first_side = *state_.request.initial_conditions.forced_first_side;
} else if (attacker_initiative == defender_initiative) {
    state_.first_side = (state_.random.next() & 1U) == 0 ? Side::attacker : Side::defender;
} else {
    state_.first_side = attacker_initiative > defender_initiative ? Side::attacker : Side::defender;
}
```

- `has_value()` checks whether there's a value; `*` takes it out.
- `condition ? A : B` is the conditional operator. It's an **expression**, so it can sit on the right of an assignment.
- `next() & 1U` takes the lowest bit, like flipping a coin. A random number is consumed only when initiative is tied.

**The precedence trap**: `&` has **lower** precedence than `==`.

```cpp
bool bad = r & 1U == 0;       // actually parsed as r & (1U == 0), always 0
```

Measured with `r = 6`: `good=1 bad=0`. `-Wall` warns `suggest parentheses around comparison in operand of '&'`. **Whenever bitwise operators and comparisons appear together, add parentheses.**

## 7. `std::array` and range-for

```cpp
const std::array<Side, 2> order{state_.first_side, other(state_.first_side)};
for (const Side side : order) { ... }
```

- `std::array<T, N>`: the length is fixed at compile time; it lives on the stack and needs no heap memory.
- `for (element : container)` is like `lists:foreach`.
- `const Side side` takes each element by value: `Side` is a single byte; for large objects write `const auto&`.

## 8. `take_side_turn`: snapshot first, re-check after every step

```cpp
void BattleRunner::take_side_turn(Side side) {
    const auto order = state_.acting_order(side);        // ① a snapshot of the acting order
    for (const auto actor_index : order) {
        if (state_.event_limit || state_.side_defeated(other(side))) {
            return;                                      // ② the other side is wiped out: the whole phase ends
        }
        auto& actor = state_.units[actor_index];
        if (!actor.alive()) {
            continue;                                    // ③ this unit is already dead: skip it
        }
        effects_.trigger_owner(actor_index, Trigger::before_action, actor_index, 0);
        if (!actor.alive()) {                            // ④ a before-action passive may have killed it
            continue;
        }
        effects_.execute_action(actor_index);
        if (actor.alive()) {                             // ⑤ reflected damage may already have killed the actor
            effects_.trigger_owner(actor_index, Trigger::after_action, actor_index, 0);
        }
    }
}
```

- **① Snapshot**: buffs may change speed mid-phase; re-sorting after every step could make the same unit act twice or be skipped. Erlang lists are naturally immutable; in C++ you have to **copy one deliberately**.
- **② `return` vs ③ `continue`**: `continue` skips the current unit; `return` ends the whole function.
- **④⑤ Checking `alive()` again and again**: after every call that might change the state, re-confirm your preconditions.

`auto& actor` can be used across all these calls only because "`units` doesn't grow or shrink during a battle" (lesson 2, section 4).

## 9. Exposing only one entry point

```cpp
namespace gamebattle {
BattleResult Engine::simulate(const BattleRequest& request) const {
    return runtime::BattleRunner(request).run();
}
}
```

- The internals live in `gamebattle::runtime`; at the end `gamebattle` is reopened to implement the public `Engine`. Port and NIF only know about `Engine`.
- **One battle gets one `BattleRunner`, thrown away after use**; no state is kept across battles. It's like spawning a temporary process per battle that exits when the battle is over.

## Summary

| Concept | Key point |
|---|---|
| How state changes | Erlang passes new values; C++ modifies the single `state_` in place, so re-check after every step |
| The for-loop variable | Overshoots by one on normal exit; correct it with `std::min` |
| `break` | Leaves only the innermost loop; outer loops rely on flags |
| `continue` / `return` | Skip the current element / end the whole function |
| Early `return` | Keeps the main flow flat |
| `&` and `==` | `&` binds more loosely, so parentheses are required |
| Snapshot | Copy the acting order first so iteration isn't affected by state changes |

Next: [Lesson 5: Target selection](05-target-selector.md)
