# Lesson 11: Building, testing and debugging

[中文](../11-build-and-test.md) | **English**

> Files: `CMakeLists.txt`, `CMakePresets.json`, `scripts/*.ps1`, `tests/*.cpp`

Every build and test result in this lesson was actually run on Linux (g++ 13.3, CMake).

## 1. C++'s compilation model: very different from Erlang

**Erlang**: each `.erl` compiles to a `.beam`, the VM loads modules on demand at run time, and modules are only "wired together" at run time.

**C++** works in two steps:

```
engine.cpp ───────┐  included headers   ┌─ compile (each .cpp separately) ─┐
battle_state.cpp ─┤  are pasted in      │                                   │
effect_system.cpp ┘  verbatim           └→ engine.o, battle_state.o, ...
                                                   │
                                        link (stitch all the .o files together)
                                                   ▼
                                       gamebattle_port (executable)
```

- **Compiling**: each `.cpp`, together with all the headers it includes, is compiled **on its own** into an object file (`.o` / `.obj`). While compiling `engine.cpp`, the compiler knows nothing about what's written in `effect_system.cpp`; it only knows the declarations in the headers.
- **Linking**: all object files are stitched into the final program, and only now is it checked whether "a declared function actually has an implementation". A function that's declared but never implemented fails at link time (`undefined reference to ...`), not at compile time.
- **Change a header**, and every `.cpp` that includes it must be recompiled; **change a `.cpp`**, and only that file is recompiled before relinking. This is another reason lesson 1 said "headers hold only declarations": the more stable the headers, the less recompilation.
- A **static library** (`.a` / `.lib`) is just a bundle of `.o` files.

## 2. Reading `CMakeLists.txt` section by section

CMake isn't a compiler; it's "a tool that generates build scripts": it reads `CMakeLists.txt` and generates Makefiles (Linux) or Visual Studio projects (Windows), which in turn invoke the compiler. Its role is a bit like rebar3, but lower level.

### Language standard

```cmake
cmake_minimum_required(VERSION 3.20)
project(gamebattle VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)      # fail outright if the compiler lacks C++20, rather than quietly downgrading
set(CMAKE_CXX_EXTENSIONS OFF)            # use -std=c++20, not GCC's extended -std=gnu++20
```

Extensions are turned off so you don't accidentally use GCC-only syntax on Linux that then fails to compile with MSVC.

### Three switches

```cmake
option(GAMEBATTLE_BUILD_TESTS "Build the C++ battle-core tests" ON)
option(GAMEBATTLE_BUILD_NIF "Build the Erlang NIF adapter" OFF)
option(GAMEBATTLE_BUILD_CONFIG_COMPILER "Build the standalone battle config compiler" OFF)
```

Turn one on from the command line with `-DGAMEBATTLE_BUILD_NIF=ON`. By default only the Port and the tests are built; the NIF and the config compiler must be enabled explicitly.

### The core static library: compiled once, reused everywhere

```cmake
add_library(gamebattle_core STATIC
    src/battle_state.cpp  src/config_store.cpp  src/effect_system.cpp
    src/engine.cpp  src/target_selector.cpp  src/term.cpp  src/wire.cpp
)
set_target_properties(gamebattle_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(gamebattle_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(gamebattle_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(gamebattle_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(gamebattle_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
```

- The Port, the NIF and the tests all link `gamebattle_core`, so the battle logic is compiled once and all three use **the same** code.
- **`POSITION_INDEPENDENT_CODE ON`** (`-fPIC`): the NIF is a **shared library** (`.so`) loaded into the BEAM process at an address nobody knows in advance. Code linked into a shared library must be "position independent", or linking fails on Linux with `relocation ... can not be used when making a shared object; recompile with -fPIC`.
- **`PUBLIC` vs `PRIVATE`**:
  - `target_include_directories(... PUBLIC include)`: used not only by core itself; **every target that links core** automatically gets this header search path too. That's why `gamebattle_port` can simply write `#include "gamebattle/wire.hpp"`.
  - `target_compile_options(... PRIVATE ...)`: the warning options apply only when compiling core itself and aren't passed on to its users.
- **The three MSVC options**: `/W4` is a high warning level; `/permissive-` demands strict standard conformance; `/utf-8` tells the compiler the source files are UTF-8. The project's source contains Chinese strings; without it, MSVC reads the source in the system code page (GBK on Chinese Windows) and the strings become garbage.

### The executable and the NIF

```cmake
add_executable(gamebattle_port src/port_main.cpp)
target_link_libraries(gamebattle_port PRIVATE gamebattle_core)

if(GAMEBATTLE_BUILD_NIF)
    if(NOT ERLANG_ERTS_INCLUDE_DIR)
        message(FATAL_ERROR "Set ERLANG_ERTS_INCLUDE_DIR to the directory containing erl_nif.h")
    endif()
    add_library(gamebattle_nif SHARED src/nif.cpp)
    target_include_directories(gamebattle_nif PRIVATE ${ERLANG_ERTS_INCLUDE_DIR})
    target_link_libraries(gamebattle_nif PRIVATE gamebattle_core)
    set_target_properties(gamebattle_nif PROPERTIES PREFIX "" OUTPUT_NAME "gamebattle_nif")
endif()
```

- `SHARED` means a shared library.
- **`PREFIX ""`**: on Linux, shared libraries are named `libgamebattle_nif.so` by default. But the Erlang side calls `erlang:load_nif(".../gamebattle_nif", 0)`, which appends only the extension, not a `lib` prefix. So the prefix is removed to produce `gamebattle_nif.so`.
- If `erl_nif.h` can't be found, `FATAL_ERROR` stops at configure time instead of waiting until compile time to spew incomprehensible errors.

### Tests: positive cases, negative cases and "data generated at build time"

```cmake
add_executable(gamebattle_tests tests/engine_test.cpp)
target_link_libraries(gamebattle_tests PRIVATE gamebattle_core)
add_test(NAME gamebattle_tests COMMAND gamebattle_tests)
```

The config tests are more interesting:

```cmake
add_custom_command(                               # ① at build time, run the config compiler first to generate a test .gbcfg
    OUTPUT ${GAMEBATTLE_TEST_CONFIG}
    COMMAND $<TARGET_FILE:gamebattle_config_compiler>
            --input-dir ${CMAKE_CURRENT_SOURCE_DIR}/config/example
            --output ${GAMEBATTLE_TEST_CONFIG}
    DEPENDS gamebattle_config_compiler tools/config_compiler.cpp config/example/buffs.csv ...)
add_custom_target(gamebattle_test_config DEPENDS ${GAMEBATTLE_TEST_CONFIG})
add_executable(gamebattle_config_tests tests/config_store_test.cpp)
add_dependencies(gamebattle_config_tests gamebattle_test_config)
target_compile_definitions(gamebattle_config_tests PRIVATE           # ② pass the file path into the code as a macro
    GAMEBATTLE_TEST_CONFIG_PATH="${GAMEBATTLE_TEST_CONFIG}")

add_test(NAME gamebattle_config_compiler_invalid_buff_cycle          # ③ a negative test
    COMMAND gamebattle_config_compiler
        --input-dir ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/config_invalid_buff_cycle
        --check-only)
set_tests_properties(gamebattle_config_compiler_invalid_reference
                     gamebattle_config_compiler_invalid_buff_cycle
                     PROPERTIES WILL_FAIL TRUE)
```

- ① `DEPENDS` lists the CSV files: **change any of the sample tables and the next build regenerates the `.gbcfg` automatically**.
- ② `target_compile_definitions` is like adding `-DGAMEBATTLE_TEST_CONFIG_PATH="..."` to the compiler, so the test code can get the path straight from the macro.
- ③ `WILL_FAIL TRUE` means "this command **should** fail". Feed the compiler a config with a cycle, and the test passes only if it returns non-zero. Testing only "valid input passes" isn't enough; you also have to test "invalid input is rejected".

### Install components

```cmake
install(TARGETS gamebattle_port RUNTIME DESTINATION priv COMPONENT BattleRuntime)
install(TARGETS gamebattle_config_compiler RUNTIME DESTINATION bin COMPONENT ConfigTools)
```

`cmake --install ... --component BattleRuntime` installs only what the runtime needs (Port, NIF) into `priv/`, which happens to be where an Erlang application keeps its native executables (`code:priv_dir(gamebattle)`). The config compiler is a separate component installed into `bin/`; production servers don't need it.

## 3. Presets: saving common configuration combinations

Typing a long string of `-D` options every time is error-prone. `CMakePresets.json` gives those combinations names:

| Preset | Platform | Purpose |
|---|---|---|
| `clion-runtime-debug` | Windows | Debug the battle core, Port and tests |
| `clion-config-compiler-debug` | Windows | Debug only the config compiler |
| `linux-runtime-debug` | Linux | Debug the battle core and tests |
| `linux-runtime-release` | Linux | Production Port, without tests, NIF or compiler |
| `linux-runtime-release-nif` | Linux | Inherits the previous one (`inherits`) and turns on the NIF |
| `linux-config-compiler-release` | Linux | Build only the config compiler |

Each preset has a `condition`, so only the presets for the host system are shown. `CMakeUserPresets.json` is machine-specific (it hard-codes, say, the local OTP 29 header path), is excluded by `.gitignore`, and isn't committed.

The full flow on Linux:

```bash
cmake --preset linux-runtime-debug                    # configure
cmake --build --preset build-linux-runtime-debug      # build (this preset builds only gamebattle_tests)
ctest --test-dir out/build/linux-runtime-debug --output-on-failure
```

On Windows use `scripts/build.ps1` (optionally with `-WithoutNif`), and `scripts/build-config-compiler.ps1` for the config compiler on its own.

## 4. Doing an actual build

A Debug build with tests and the config compiler turned on (measured, output excerpt):

```
battle_state.cpp:382:7: warning: missing initializer for member 'gamebattle::BattleResult::reason' [-Wmissing-field-initializers]
battle_state.cpp:382:7: warning: missing initializer for member 'gamebattle::BattleResult::events' [-Wmissing-field-initializers]
battle_state.cpp:382:7: warning: missing initializer for member 'gamebattle::BattleResult::units' [-Wmissing-field-initializers]
[ 57%] Built target gamebattle_core
[ 84%] Built target gamebattle_port
[100%] Built target gamebattle_tests
...
100% tests passed, 0 tests failed out of 6
```

Those three warnings are exactly pitfall 4 from lesson 1, section 11: the `BattleState` constructor (`battle_state.cpp:382`) uses designated initializers for only three fields, `battle_id`, `seed` and `source_battle_id`.

Why are only `reason`, `events` and `units` warned about, while `winner` and `rounds`, also omitted, aren't? Because GCC warns only about fields **without a default member initializer**: `winner{Winner::draw}` and `rounds{0}` have defaults in `engine.hpp`, while `std::string reason;` and `std::vector<Event> events;` don't (this rule was verified by experiment).

Those fields are still initialized normally to an empty string and empty vectors, so the program is correct. But it makes a point: **you need to be able to read a warning and judge whether it's a real problem or acceptable**. There are two ways to silence it: add `.reason = {}, .events = {}, .units = {}` in declaration order in the initializer, or give those three fields a `{}` in `engine.hpp` too.

The 6 tests are:

| Test | What it covers |
|---|---|
| `gamebattle_tests` | Battle core: determinism, buffs, battle continuation, protocol encoding/decoding and more, 9 groups |
| `gamebattle_config_tests` | Loads the sample `.gbcfg` generated at build time |
| `gamebattle_config_loader_invalid_buff_cycle` | The loader rejects a config with a cycle |
| `gamebattle_config_compiler_valid` | The sample tables pass the compiler's checks |
| `gamebattle_config_compiler_invalid_reference` | A reference to a nonexistent ID → must fail |
| `gamebattle_config_compiler_invalid_buff_cycle` | A buff reference cycle → must fail |

## 5. How the tests are written, and a big trap

`tests/engine_test.cpp` uses no framework like GoogleTest; it's just ordinary functions plus `assert`:

```cpp
void test_engine_flow_and_determinism() {
    const auto request = sample_request();
    const auto first = gamebattle::Engine{}.simulate(request);
    const auto second = gamebattle::Engine{}.simulate(request);
    assert(first.attacker_initiative == 220);
    ...
    const auto first_bytes = gamebattle::term::encode(gamebattle::wire::encode_result(first));
    const auto second_bytes = gamebattle::term::encode(gamebattle::wire::encode_result(second));
    assert(first_bytes == second_bytes);          // the same request run twice must encode to identical bytes
}

int main() {
    test_wire_generic_buff_schema_and_reject_legacy_fields();
    ...
    std::cout << "all gamebattle tests passed\n";
}
```

The benefit is zero dependencies. The determinism test compares **encoded bytes**, which is stricter than comparing field by field: any difference in any event or any field is caught.

### The big trap: `assert` disappears in Release builds

`assert` is a macro, and when `NDEBUG` is defined it's replaced with "do nothing". CMake's Release mode adds `-DNDEBUG` by default (measured, the Release compile flags):

```
CXX_FLAGS = -O3 -DNDEBUG -std=c++20
```

Verified with an assertion that must fail (measured):

```cpp
assert(1 + 1 == 3 && "this should fail");
```
```
--- without NDEBUG ---
Aborted       exit code=134                  ← the assertion fires and the program terminates
--- with -DNDEBUG ---
assert skipped, program finished normally    ← the assertion was deleted
```

In other words, **if you build the tests in Release mode, no `assert` checks anything and the tests always "pass"**. The project's presets avoid this trap: every preset with tests is Debug, and every Release preset turns the tests off. But if you run `cmake -DCMAKE_BUILD_TYPE=Release -DGAMEBATTLE_BUILD_TESTS=ON` by hand, you get a set of tests that check nothing.

Also: **never put code with side effects inside an `assert`**, such as `assert(engine.simulate(r).winner == ...)`. In Release the whole expression is deleted and `simulate` never runs. That's why the project's tests always compute the result first and then `assert`.

If you want checks that work in every mode, define your own macro:

```cpp
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond "\n"; \
            std::abort();                                                        \
        }                                                                        \
    } while (false)
```

- A macro is **pure text substitution** by the preprocessor, before real compilation happens.
- `#cond` turns the argument into a string as written, so a failure can print which condition it was.
- `__FILE__` and `__LINE__` are the current file name and line number, supplied by the compiler.
- `do { ... } while (false)` is the standard trick for multi-statement macros, so that `if (x) CHECK(y); else ...` doesn't break.

## 6. Sanitizers: turning undefined behavior into clear errors

The AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) used again and again in earlier lessons can be switched on for the whole project at once:

```bash
cmake -S . -B out/build/asan -DCMAKE_BUILD_TYPE=Debug \
      -DGAMEBATTLE_BUILD_TESTS=ON -DGAMEBATTLE_BUILD_CONFIG_COMPILER=ON \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build out/build/asan --parallel
ctest --test-dir out/build/asan --output-on-failure
```

Measured: `100% tests passed, 0 tests failed out of 6`; the current code is clean under both checkers.

| Tool | Catches | Examples from earlier lessons |
|---|---|---|
| ASan | Use of freed memory, out-of-bounds access, references to destroyed temporaries | Lesson 2's dangling reference after reallocation, lesson 5's out-of-bounds `<=` comparator, lesson 6's iterator used after erase |
| UBSan | Signed overflow, out-of-range shifts, invalid enum values and more | Lesson 7's compute-then-check overflow |

Programs run 2–3× slower, so use them only in development and CI, never in production. MSVC on Windows supports ASan: `/fsanitize=address`.

**Add a sanitizer build to CI**: when you add features later (summons, say), many memory bugs will surface during testing.

## 7. Debugging

### Debugging battle logic: start from the test program

Once started, `gamebattle_port` waits forever for ETF data on stdin, which makes debugging it directly awkward. For pure battle logic, debug `gamebattle_tests`:

- **CLion** (the steps in the README): load the preset, choose `gamebattle_tests` as the run configuration, and set breakpoints in places like `BattleRunner::run` and `EffectSystem::apply_damage`.
- **Command-line gdb**:

```bash
gdb --args out/build/linux-runtime-debug/gamebattle_tests
(gdb) break gamebattle::runtime::EffectSystem::apply_damage
(gdb) run
(gdb) bt                  # the call stack: which passive and which recursion level this damage came from
(gdb) print target.hp     # inspect a variable
(gdb) next                # step
(gdb) finish              # run until the current function returns
```

Lesson 6's "chain reaction" diagram becomes very concrete when you look at the call stack with `bt`.

### Debugging the Port protocol

- Let Erlang start the Port, then **attach** a debugger to the `gamebattle_port` process (`gdb -p <PID>`, or CLion's Attach to Process).
- Or, as in lesson 9, write a small script that sends bytes in `{packet, 4}` format straight into the Port's stdin, with no Erlang needed.
- Once more: **never print debug output to stdout in the Port**; use `std::cerr`.

### Bugs that only show up in Release

If a problem can't be reproduced in Debug and only appears in Release, suspect **undefined behavior** first (lesson 7: the optimizer exploits UB to delete your code). A run under UBSan usually pinpoints it.

## 8. Deployment notes

- **Build Linux artifacts on Linux.** A `.exe` / `.dll` built on Windows can't be deployed to Linux.
- **glibc versions**: a program built on an older system usually runs on a newer one, but not the other way round. So the build machine's distribution version should be **no newer** than production's, ideally identical (for example, the same container image).
- **The NIF** must be compiled against an `erl_nif.h` from **the same OTP major version** as production.
- The Erlang side looks for the Port executable in the order `application:get_env(gamebattle, port_executable)` → the environment variable `GAMEBATTLE_PORT` → the `priv/` directory (`gamebattle_port.erl:103`).

## 9. Course recap: which lessons' warnings apply when you extend the engine

The README's "explicit boundaries of v1" lists features not yet implemented. Before you start on one, check this table:

| Planned feature | What to watch out for | Related lessons |
|---|---|---|
| **Summons** | Adding units to `units` reallocates the vector, so every `auto&` held across effect execution may become invalid. Either reserve slots or switch everything to re-fetching by index | Lesson 2 §4, lesson 4 §8 |
| **Shields** | A new `EffectKind` means changing 6 places together; decide at which step of `apply_damage` absorption happens; compute absorbed amounts with saturating arithmetic | Lesson 10 §3.4, lesson 6, lesson 7 |
| **Energy, skill cooldowns** | New per-battle state goes in `RuntimeUnit`; add it to `UnitInitialState` if it carries over between battles; for per-round resets see `reset_round_trigger_counts` | Lessons 2, 4 |
| **Revival** | `alive()` only checks `hp > 0`; `side_defeated` and the acting-order snapshot all need rethinking | Lesson 4 |
| **Dispel tags** | A new field on `BuffSpec` → the CSV tables, compiler, loader and `.gbcfg` version all change | Lesson 10 |
| **Any new random roll** | Changes the consumption order of every later random number; old battle reports can't be reproduced on the new version | Lesson 3 §4 |

And the most important principle in the README: **extend `EffectKind` and event types for new mechanics, keep the Erlang request format backward compatible, and never write the damage formula twice, once in Erlang and once in C++.**

## Summary

| Concept | Key point |
|---|---|
| Compiling and linking | Each `.cpp` compiles separately and is linked at the end; changing a header has a wide impact |
| Static library | Port, NIF and tests share one copy of the core code |
| `-fPIC` | Required when a static library is linked into a shared library (the NIF) |
| `PUBLIC` / `PRIVATE` | Whether settings propagate to users |
| `/utf-8` | Required for MSVC to compile source containing Chinese |
| `PREFIX ""` | Makes the NIF's file name match what `erlang:load_nif` expects |
| `WILL_FAIL` | Negative tests: invalid input must be rejected |
| `assert` and `NDEBUG` | Every `assert` disappears in Release; never put side effects inside one |
| Sanitizers | ASan catches memory errors, UBSan catches undefined behavior; put them in CI |
| Debugging | Debug battle logic through the test program; for the Port, attach to the process or feed bytes with a script |
| Deployment | Build on a Linux no newer than production; the NIF must match the OTP major version |

That's the end of part 1, "Reading the project". Next: [Lesson 12: Object model and polymorphism](12-object-model.md)
