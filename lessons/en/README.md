# GameBattle C++ Course

[中文](../README.md) | **English**

For engineers whose main language is Erlang, who haven't written C++ in a long time, and who are aiming for a **C++ game-server role**. The course uses this repository's battle engine as its textbook and is split into six parts:

| Part | Lessons | Goal |
|---|---|---|
| 1. Reading the project | 1–11 | Understand every line of the engine, explain C++ syntax details in real code, and use Erlang concepts as analogies |
| 2. C++ in depth | 12–16 | Object model, move semantics, smart pointers, templates, STL internals: what interviews always ask and the project rarely shows |
| 3. Concurrency | 17–18 | Run real battles concurrently on a thread pool; atomics and the memory model |
| 4. Networking and game servers | 19–20 | An epoll TCP battle server; server architecture and common data structures |
| 5. Performance and production | 21–22 | Real profiling and fuzzing of the engine, with the problems found recorded |
| 6. Job hunting | 23–24 | Interview question bank; résumé, project pitch, hands-on list and study plan |

Output marked "measured" was produced by actually compiling and running the code on a 4-core Linux cloud VM (g++ 13.3 / clang 18, `-std=c++20`). The hands-on code from part 2 onward lives in [`practice/`](../practice/README.en.md); it links against the real engine without modifying the engine's code.

## Course map

### Part 1: Reading the project

| Lesson | Topic | Code | C++ focus |
|---|---|---|---|
| [1](01-domain-model.md) | Reading the domain model through Erlang eyes | `include/gamebattle/engine.hpp` | Headers, namespaces, fixed-width integers, `enum class`, struct defaults, containers, designated initializers, forward declarations |
| [2](02-ownership.md) | Values, references, pointers, const, move | `src/battle_runtime.hpp` | Five ways to express ownership, `shared_ptr<const T>`, indices vs pointers after growth/erasure, `auto` vs `auto&`, member initialization order, `std::move` |
| [3](03-class-and-random.md) | Classes and deterministic random numbers | `Random`, the `BattleState` constructor | class/struct, SplitMix64, unsigned overflow, initializer lists, `explicit`, explicit vs implicit construction |
| [4](04-battle-loop.md) | The round loop | `src/engine.cpp` | Recursion vs loops, `break` exits one level only, early return, precedence of `&`, `std::array`, snapshots |
| [5](05-target-selector.md) | Target selection | `src/target_selector.cpp` | `static` member functions, lambdas and captures, `std::sort` strict weak ordering, unstable sorting and determinism |
| [6](06-effect-system.md) | The effect system | `src/effect_system.cpp` | Recursive triggers, `switch`, iterators, erase-remove, auto-insertion by `map[]`, collect-then-execute, `it = erase(it)` |
| [7](07-integer-safety.md) | Integer safety | `saturating_add` / `scale` | Signed-overflow UB, check before computing, saturating arithmetic, basis points, narrowing conversions, mixed signedness |
| [8](08-validation-exceptions.md) | Validation and exceptions | `validate_request`, `handle_etf` | Stack unwinding and RAII, catch order, catching by reference, custom exceptions, recursive `std::function`, three-color marking |
| [9](09-erlang-bridge.md) | Talking to Erlang | `port_main.cpp`, `term.cpp`, `wire.cpp`, `nif.cpp` | `{packet,4}`, byte order, `std::span`, ETF, `std::variant`, function templates, reader-writer locks, dirty schedulers |
| [10](10-config-pipeline.md) | The config pipeline | `tools/config_compiler.cpp`, `src/config_store.cpp` | `std::from_chars`, deterministic output, CRC32, atomic file writes, shell → freeze, Kahn topological sort, strong exception guarantee |
| [11](11-build-and-test.md) | Building, testing and debugging | `CMakeLists.txt`, `tests/` | Compiling and linking, CMake targets, `-fPIC`, presets, `assert` and `NDEBUG`, sanitizers, gdb, deployment |

### Part 2: C++ in depth

| Lesson | Topic | Measured highlights |
|---|---|---|
| [12](12-object-model.md) | Object model and polymorphism: vtables, virtual destructors, override, diamond inheritance, RTTI | Virtual dispatch is about 45% slower than a `switch`; a non-virtual destructor leaks derived-class members |
| [13](13-move-semantics.md) | Special member functions, value categories, move semantics, perfect forwarding, copy elision | Writing only a destructor turns a "move" into 100 copies; `initializer_list` makes this project's encoder 4× slower |
| [14](14-smart-pointers.md) | Smart pointers in depth: control blocks, `make_shared`, `weak_ptr`, thread safety | Four threads copying the same `shared_ptr` cost about 300 ns per copy |
| [15](15-templates.md) | Templates, concepts, specialization, `if constexpr`, fold expressions, `constexpr`, CRTP | Concepts cut an error message from 109 lines to 16; a compile-time CRC table is 4.3× faster |
| [16](16-stl-internals.md) | STL container internals and iterator invalidation | One battle makes 1093 heap allocations; encoding allocates 2 MB to produce 148 KB of output |

### Part 3: Concurrency

| Lesson | Topic | Measured highlights |
|---|---|---|
| [17](17-threads-and-pools.md) | Threads, locks, condition variables, thread pools | Four threads run battles 3.5× faster with byte-identical results and no TSan warnings |
| [18](18-atomics-memory-model.md) | Atomics, memory orders, false sharing, CAS, lock-free queues | A relaxed publish is "accidentally correct" on x86 but TSan reports a race; false sharing is 4× slower |

### Part 4: Networking and game servers

| Lesson | Topic | Measured highlights |
|---|---|---|
| [19](19-epoll-battle-server.md) | epoll and a TCP battle server (Erlang can connect directly with `gen_tcp`) | Coalesced packets, partial packets and oversized frames are all handled correctly; p50 2.1 ms, p99 4.2 ms |
| [20](20-game-server-architecture.md) | Server architecture, sync models, timing wheels, AOI, skiplist leaderboards, consistent hashing, ECS | Timing wheel 5× faster, grid AOI 140× faster, skiplist rank lookup 8000× faster |

### Part 5: Performance and production

| Lesson | Topic | Measured highlights |
|---|---|---|
| [21](21-performance.md) | Benchmarking, profiling, verifying optimizations | Encoding takes 78%; after the fix, single-thread throughput goes from about 900 to 1700 battles/s |
| [22](22-debugging-and-fuzzing.md) | Core dumps, gdb, memory bugs, fuzzing | 200,000 fuzz iterations without a crash; two real problems found |

### Part 6: Job hunting

| Lesson | Topic |
|---|---|
| [23](23-interview-questions.md) | Interview question bank: 129 questions grouped by topic, each linked to its lesson |
| [24](24-resume-and-pitch.md) | How to write the résumé, three STAR stories, a hands-on list, a 6-week plan |

**Suggested reading order**: lessons 1–2 are the foundation for everything after them, so read them thoroughly first; read lessons 3–11 in order with the source code open. From part 2 onward each lesson can be read on its own, but lessons 21–22 cite a lot of earlier data. Start lesson 24's "hands-on list" early and work on it as you learn.

Exercise code:

- `lesson1.cpp`, `lesson2.cpp`: optional exercises for lessons 1 and 2
- [`practice/`](../practice/README.en.md): hands-on programs for lessons 17–22 (thread pool, TCP server, benchmarks, fuzzing, data structures), plus `known_issues`, which reproduces the known problems

## Engine problems found during the course

| Problem | Impact | Lessons |
|---|---|---|
| `initializer_list` causes deep copies in result encoding | Encoding takes about 78% of the full pipeline | 13, 21 |
| Nested containers preallocate by their declared element count | A 501-byte input grows peak virtual memory by 3.8 GB | 22 |
| A request is rejected when two units carry the same inline buff | Only affects requests in the inline format | 22 |
| NIF binaries have no RAII protection | Possible leak on exception paths | 9, 14 |

These are all left for you to fix (the hands-on list in lesson 24); `practice/known_issues` checks whether each fix has taken effect.

## Common commands

```bash
# A single exercise file (all common warnings and memory checking on)
g++ -std=c++20 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined lessons/lesson2.cpp -o lesson2 && ./lesson2

# The whole project: Debug + tests
cmake --preset linux-runtime-debug
cmake --build --preset build-linux-runtime-debug
ctest --test-dir out/build/linux-runtime-debug --output-on-failure

# The whole project: all tests under sanitizers (lesson 11)
cmake -S . -B out/build/asan -DCMAKE_BUILD_TYPE=Debug \
      -DGAMEBATTLE_BUILD_TESTS=ON -DGAMEBATTLE_BUILD_CONFIG_COMPILER=ON \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build out/build/asan --parallel && ctest --test-dir out/build/asan --output-on-failure
```

Compiling a single file on Windows (VS 2022 Developer PowerShell): `cl /std:c++20 /W4 /EHsc /Zi /fsanitize=address lessons\lesson2.cpp`. For building the whole project, see `README.md` at the repository root.

## Erlang ↔ C++ cheat sheet

| Erlang | C++ | Lessons |
|---|---|---|
| `.hrl` / `-include` | `.hpp` / `#include` | 1 |
| Module-name prefix `gamebattle:` | `namespace gamebattle::` | 1 |
| Functions that aren't exported | Anonymous `namespace { }` | 1 |
| Atom `attacker` | `enum class Side { attacker }` | 1 |
| `-record(stats, {hp = 1})` | `struct Stats { std::int64_t hp{1}; };` | 1 |
| `#{id => 1}` | `UnitConfig{.id = 1}` (in declaration order) | 1 |
| list | `std::vector<T>` | 1 |
| map | `std::unordered_map<K, V>` (unordered) / `std::map<K, V>` (ordered) | 1, 10 |
| binary | `std::string` / `std::vector<std::uint8_t>` | 1, 9 |
| `undefined \| V` | `std::optional<T>` | 1 |
| Reference-counted sharing of large binaries | `std::shared_ptr<const T>` | 2 |
| Sub-binary references | `std::span<T>` / `std::string_view` (borrow only, no ownership) | 9 |
| Any term | `std::variant<...>` | 9 |
| Function clauses with guards | `std::get_if<T>` / `std::visit` + overloads | 9 |
| `fun(X) -> ... end` | `[captures](auto x) { ... }` | 5 |
| `lists:sort(fun(A, B) -> A =< B end, L)` | `std::sort(..., [](a, b) { return a < b; })` (**strictly less than**) | 5 |
| `lists:filter/2` | erase-remove / `std::erase_if` | 6 |
| `lists:search/2` returning `false` | `std::find_if` returning `end()` | 6 |
| `maps:get(K, M, Default)` | `opt.value_or(Default)` | 6 |
| `maps:update_with(K, F, Init, M)` | `++map[key]` (inserts automatically when missing) | 6 |
| `{A, B} = Tuple` | `auto [a, b] = pair;` | 8 |
| `try ... catch Class:Reason` | `try { } catch (const T& e) { }` | 8 |
| `erlang:raise/3` to rethrow unchanged | `throw;` | 10 |
| `init/1` returning `{stop, Reason}` | Throwing from a constructor | 3 |
| `div` / `rem` | `/` / `%` (both truncate toward zero) | 7 |
| `band` / `bxor` / `bsr` | `&` / `^` / `>>` | 3, 4 |
| `<<Len:32/big>>` | Assembling bytes with manual shifts | 9 |
| `erlang:crc32/1` | The project's `crc32()` (the same algorithm) | 10 |
| `digraph_utils:topsort/1` | Kahn's algorithm | 10 |
| Registered process / `persistent_term` | A function-local `static` object | 9 |
| A supervision tree restarts crashed processes | No equivalent; an uncaught exception terminates the whole OS process | 8 |

## The twelve most important rules

These are the places that come up again and again in the course and are the easiest to get wrong. Every one has a measured example.

1. **Built-in types hold garbage unless initialized**; write `{0}` for every integer in a struct. (Lesson 1)
2. **`auto` is a copy; only `auto&` is a reference**. Write `auto&` when you want to modify the original. (Lesson 2)
3. **After a container is modified, pointers, references and iterators taken earlier may all be invalid**. For long-lived links use an index (containers that never erase) or a unique ID (containers that erase). (Lessons 2, 6)
4. **Members are initialized in declaration order**, regardless of the order written in the initializer list. (Lesson 2)
5. **Returning a member variable needs `std::move`**; returning a local variable doesn't. (Lesson 2)
6. **Mark every single-argument constructor `explicit`**. (Lesson 3)
7. **Adding or removing one random roll shifts every later result for the same seed**. (Lesson 3)
8. **`break` only leaves the innermost loop**; parenthesize bitwise operators mixed with comparisons. (Lesson 4)
9. **`std::sort` comparators must use `<`**; `<=` can run out of bounds. Make the final comparison a unique ID so the result is deterministic. (Lesson 5)
10. **To erase while iterating, the only correct form is `it = erase(it)`**; when the container may change during iteration, snapshot the IDs first and then execute. (Lesson 6)
11. **Signed overflow is undefined behavior, and compute-then-check gets deleted by the optimizer**. Overflow checks must happen before the computation. (Lesson 7)
12. **Catch exceptions by `const&`, most specific type first**; an uncaught C++ exception terminates the whole process, so the Port/NIF boundary needs a `catch (...)` safety net. (Lesson 8)

Plus two engineering rules: **never print anything to stdout** inside a Port process (lesson 9); **`assert` is removed** in Release builds, so test with a Debug build (lesson 11).

From part 2 onward:

13. **A class with only a user-written destructor loses its move operations**; use the rule of zero whenever you can. (Lesson 13)
14. **Mark your own move operations `noexcept`**, or vector growth falls back to copying. (Lesson 13)
15. **Don't use braced list initialization to hold large objects**; an `initializer_list` can only be copied from. (Lessons 13, 21)
16. **A base-class destructor should be either public and virtual, or the class should be `final`**. (Lesson 12)
17. **When you only use the object, pass the `shared_ptr` by `const&`**. (Lesson 14)
18. **Measure before optimizing; verify that benchmark results match before timing; distrust numbers that don't make sense**. (Lesson 21)
19. **Compare length fields from external input against the remaining data before allocating memory**. (Lesson 22)
