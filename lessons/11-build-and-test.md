# 第 11 课：构建、测试与调试

**中文** | [English](en/11-build-and-test.md)

> 对应文件：`CMakeLists.txt`、`CMakePresets.json`、`scripts/*.ps1`、`tests/*.cpp`

本课的构建和测试结果，都是在 Linux（g++ 13.3、CMake）上实际跑出来的。

## 1. C++ 的编译模型：和 Erlang 差别很大

**Erlang**：每个 `.erl` 编译成一个 `.beam`，运行时由虚拟机按需加载，模块之间在运行时才"连起来"。

**C++** 分两步：

```
engine.cpp ──┐                     ┌─ 编译（每个 .cpp 单独进行）──┐
battle_state.cpp ─┤  #include 的头文件 │                           │
effect_system.cpp ┘  原样粘贴进来      └→ engine.o, battle_state.o, ...
                                                   │
                                        链接（把所有 .o 拼起来）
                                                   ▼
                                       gamebattle_port（可执行文件）
```

- **编译**：每个 `.cpp` 连同它 include 的所有头文件，**单独**编译成一个目标文件（`.o` / `.obj`）。编译 `engine.cpp` 时，编译器完全不知道 `effect_system.cpp` 里写了什么，只知道头文件里的声明。
- **链接**：把所有目标文件拼成最终程序，这时才检查"声明过的函数到底有没有实现"。函数声明了却没实现，报错发生在链接阶段（`undefined reference to ...`），而不是编译阶段。
- **改了头文件**，所有 include 它的 `.cpp` 都要重新编译；**改了某个 `.cpp`**，只重新编译它自己再重新链接。这就是第 1 课说"头文件只放声明"的另一个原因：头文件越稳定，重新编译越少。
- **静态库**（`.a` / `.lib`）就是一堆 `.o` 打成的包。

## 2. `CMakeLists.txt` 逐段读

CMake 不是编译器，它是"生成构建脚本的工具"：读 `CMakeLists.txt`，生成 Makefile（Linux）或 Visual Studio 工程（Windows），再由它们去调用编译器。作用上有点像 rebar3，但更底层。

### 语言标准

```cmake
cmake_minimum_required(VERSION 3.20)
project(gamebattle VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)      # 编译器不支持 C++20 就直接报错，而不是悄悄降级
set(CMAKE_CXX_EXTENSIONS OFF)            # 用 -std=c++20，而不是 GCC 扩展的 -std=gnu++20
```

关掉扩展，是为了避免在 Linux 上不小心用了 GCC 独有的语法，拿到 MSVC 上编译不过。

### 三个开关

```cmake
option(GAMEBATTLE_BUILD_TESTS "Build the C++ battle-core tests" ON)
option(GAMEBATTLE_BUILD_NIF "Build the Erlang NIF adapter" OFF)
option(GAMEBATTLE_BUILD_CONFIG_COMPILER "Build the standalone battle config compiler" OFF)
```

命令行用 `-DGAMEBATTLE_BUILD_NIF=ON` 打开。默认只构建 Port 和测试，NIF 和配置编译器都要显式打开。

### 核心静态库：只编译一次，到处复用

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

- Port、NIF、测试都链接 `gamebattle_core`，所以战斗逻辑只编译一次，三者用的是**同一份**代码。
- **`POSITION_INDEPENDENT_CODE ON`**（`-fPIC`）：NIF 是一个**动态库**（`.so`），会被加载到 BEAM 进程里一个事先不知道的地址。被链接进动态库的代码必须是"位置无关"的，否则 Linux 上链接会失败，报 `relocation ... can not be used when making a shared object; recompile with -fPIC`。
- **`PUBLIC` 和 `PRIVATE`**：
  - `target_include_directories(... PUBLIC include)`：不仅 core 自己用，**所有链接了 core 的目标**也自动获得这个头文件搜索路径。所以 `gamebattle_port` 里可以直接写 `#include "gamebattle/wire.hpp"`。
  - `target_compile_options(... PRIVATE ...)`：警告选项只用于编译 core 自己，不传给使用者。
- **MSVC 的三个选项**：`/W4` 是较高的警告级别；`/permissive-` 要求严格遵守标准；`/utf-8` 告诉编译器源文件是 UTF-8。项目源码里有中文字符串，不加它，MSVC 会按系统代码页（中文 Windows 上是 GBK）读源码，字符串就变成乱码。

### 可执行文件和 NIF

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

- `SHARED` 表示动态库。
- **`PREFIX ""`**：Linux 上动态库默认会被命名为 `libgamebattle_nif.so`。但 Erlang 侧调用的是 `erlang:load_nif(".../gamebattle_nif", 0)`，它只会补上扩展名，不会补 `lib` 前缀。所以要去掉前缀，产出 `gamebattle_nif.so`。
- 找不到 `erl_nif.h` 时用 `FATAL_ERROR` 在配置阶段就停下，而不是等到编译时报一堆看不懂的错误。

### 测试：正例、反例和"构建时生成的数据"

```cmake
add_executable(gamebattle_tests tests/engine_test.cpp)
target_link_libraries(gamebattle_tests PRIVATE gamebattle_core)
add_test(NAME gamebattle_tests COMMAND gamebattle_tests)
```

配置相关的测试更有意思：

```cmake
add_custom_command(                               # ① 构建时先运行配置编译器，生成测试用的 .gbcfg
    OUTPUT ${GAMEBATTLE_TEST_CONFIG}
    COMMAND $<TARGET_FILE:gamebattle_config_compiler>
            --input-dir ${CMAKE_CURRENT_SOURCE_DIR}/config/example
            --output ${GAMEBATTLE_TEST_CONFIG}
    DEPENDS gamebattle_config_compiler tools/config_compiler.cpp config/example/buffs.csv ...)
add_custom_target(gamebattle_test_config DEPENDS ${GAMEBATTLE_TEST_CONFIG})
add_executable(gamebattle_config_tests tests/config_store_test.cpp)
add_dependencies(gamebattle_config_tests gamebattle_test_config)
target_compile_definitions(gamebattle_config_tests PRIVATE           # ② 把文件路径作为宏传进代码
    GAMEBATTLE_TEST_CONFIG_PATH="${GAMEBATTLE_TEST_CONFIG}")

add_test(NAME gamebattle_config_compiler_invalid_buff_cycle          # ③ 反例测试
    COMMAND gamebattle_config_compiler
        --input-dir ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/config_invalid_buff_cycle
        --check-only)
set_tests_properties(gamebattle_config_compiler_invalid_reference
                     gamebattle_config_compiler_invalid_buff_cycle
                     PROPERTIES WILL_FAIL TRUE)
```

- ① `DEPENDS` 列出了 CSV 文件：**改了任何一张示例表，下次构建会自动重新生成 `.gbcfg`**。
- ② `target_compile_definitions` 相当于给编译器加 `-DGAMEBATTLE_TEST_CONFIG_PATH="..."`，测试代码里就能直接用这个宏拿到路径。
- ③ `WILL_FAIL TRUE` 表示"这个命令**应该**失败"。给编译器喂一个有环的配置，它返回非 0 才算测试通过。只测"正确的输入能通过"是不够的，还要测"错误的输入会被拒绝"。

### 安装组件

```cmake
install(TARGETS gamebattle_port RUNTIME DESTINATION priv COMPONENT BattleRuntime)
install(TARGETS gamebattle_config_compiler RUNTIME DESTINATION bin COMPONENT ConfigTools)
```

`cmake --install ... --component BattleRuntime` 只装运行时需要的东西（Port、NIF）到 `priv/`，这正好是 Erlang 应用存放本地可执行文件的目录（`code:priv_dir(gamebattle)`）。配置编译器是另一个组件，装到 `bin/`，生产服务器不需要它。

## 3. Presets：把常用的配置组合存下来

每次都敲一长串 `-D` 参数很容易出错。`CMakePresets.json` 把这些组合起了名字：

| 预设 | 平台 | 用途 |
|---|---|---|
| `clion-runtime-debug` | Windows | 调试战斗核心、Port 和测试 |
| `clion-config-compiler-debug` | Windows | 只调试配置编译器 |
| `linux-runtime-debug` | Linux | 调试战斗核心和测试 |
| `linux-runtime-release` | Linux | 生产 Port，不带测试、NIF、编译器 |
| `linux-runtime-release-nif` | Linux | 继承上一个（`inherits`），再打开 NIF |
| `linux-config-compiler-release` | Linux | 只构建配置编译器 |

每个预设有 `condition`，按宿主系统只显示对应平台的选项。`CMakeUserPresets.json` 是本机专用的（比如写死了本机 OTP 29 的头文件路径），被 `.gitignore` 排除，不提交。

Linux 上的完整流程：

```bash
cmake --preset linux-runtime-debug                    # 配置
cmake --build --preset build-linux-runtime-debug      # 构建（这个预设只构建 gamebattle_tests）
ctest --test-dir out/build/linux-runtime-debug --output-on-failure
```

Windows 用 `scripts/build.ps1`（可加 `-WithoutNif`），配置编译器单独用 `scripts/build-config-compiler.ps1`。

## 4. 实际构建一次

打开测试和配置编译器，Debug 构建（实测输出节选）：

```
battle_state.cpp:361:7: warning: missing initializer for member 'gamebattle::BattleResult::reason' [-Wmissing-field-initializers]
battle_state.cpp:361:7: warning: missing initializer for member 'gamebattle::BattleResult::events' [-Wmissing-field-initializers]
battle_state.cpp:361:7: warning: missing initializer for member 'gamebattle::BattleResult::units' [-Wmissing-field-initializers]
[ 57%] Built target gamebattle_core
[ 84%] Built target gamebattle_port
[100%] Built target gamebattle_tests
...
100% tests passed, 0 tests failed out of 6
```

那三条警告正是第 1 课第 11 节的坑 4：`BattleState` 构造函数（`battle_state.cpp:361`）用指定初始化只写了 `battle_id`、`seed`、`source_battle_id` 三个字段。

为什么只警告 `reason`、`events`、`units`，而同样被省略的 `winner`、`rounds` 没有警告？因为 GCC 只对**没有默认成员初始值**的字段发警告：`winner{Winner::draw}`、`rounds{0}` 在 `engine.hpp` 里写了默认值，而 `std::string reason;`、`std::vector<Event> events;` 没写（实测验证过这条规则）。

这些字段会被正常初始化成空字符串、空数组，所以程序是对的。但它说明了一件事：**要能读懂警告，判断它是真问题还是可以接受的**。想消除它有两种办法：在初始化里按声明顺序补上 `.reason = {}, .events = {}, .units = {}`；或者在 `engine.hpp` 里给这三个字段也加上 `{}`。

6 个测试分别是：

| 测试 | 内容 |
|---|---|
| `gamebattle_tests` | 战斗核心：确定性、Buff、续战、协议编解码等 9 组 |
| `gamebattle_config_tests` | 加载构建时生成的示例 `.gbcfg` |
| `gamebattle_config_loader_invalid_buff_cycle` | 加载器能拒绝带环的配置 |
| `gamebattle_config_compiler_valid` | 示例表能通过编译器检查 |
| `gamebattle_config_compiler_invalid_reference` | 引用不存在的 ID → 必须失败 |
| `gamebattle_config_compiler_invalid_buff_cycle` | Buff 引用环 → 必须失败 |

## 5. 测试代码的写法，以及一个大坑

`tests/engine_test.cpp` 没有用 GoogleTest 之类的框架，就是普通函数加 `assert`：

```cpp
void test_engine_flow_and_determinism() {
    const auto request = sample_request();
    const auto first = gamebattle::Engine{}.simulate(request);
    const auto second = gamebattle::Engine{}.simulate(request);
    assert(first.attacker_initiative == 220);
    ...
    const auto first_bytes = gamebattle::term::encode(gamebattle::wire::encode_result(first));
    const auto second_bytes = gamebattle::term::encode(gamebattle::wire::encode_result(second));
    assert(first_bytes == second_bytes);          // 同一个请求跑两次，编码后的字节必须完全相同
}

int main() {
    test_wire_generic_buff_schema_and_reject_legacy_fields();
    ...
    std::cout << "all gamebattle tests passed\n";
}
```

好处是零依赖。确定性测试比较的是**编码后的字节**，比逐个字段比较更严格：任何一个事件、任何一个字段不一样都会被发现。

### 大坑：Release 模式下 `assert` 会消失

`assert` 是一个宏，定义了 `NDEBUG` 时它会被替换成"什么都不做"。CMake 的 Release 模式默认就会加上 `-DNDEBUG`（实测 Release 的编译参数）：

```
CXX_FLAGS = -O3 -DNDEBUG -std=c++20
```

用一个必定失败的断言验证（实测）：

```cpp
assert(1 + 1 == 3 && "this should fail");
```
```
--- 不加 NDEBUG ---
Aborted       退出码=134         ← 断言生效，程序终止
--- 加 -DNDEBUG ---
assert 被跳过了，程序照常结束    ← 断言被删掉了
```

也就是说，**用 Release 模式构建测试，所有 `assert` 都不检查，测试永远"通过"**。项目的预设避开了这个坑：带测试的预设都是 Debug，Release 预设都把测试关掉了。但如果你手动执行 `cmake -DCMAKE_BUILD_TYPE=Release -DGAMEBATTLE_BUILD_TESTS=ON`，就会得到一组什么都不检查的测试。

还要注意：**不要在 `assert` 里写有副作用的代码**，比如 `assert(engine.simulate(r).winner == ...)`。Release 下整个表达式被删掉，`simulate` 根本不会执行。项目里的测试都是先算出结果、再 `assert`，就是这个原因。

如果希望检查在任何模式下都生效，可以自己定义一个宏：

```cpp
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond "\n"; \
            std::abort();                                                        \
        }                                                                        \
    } while (false)
```

- 宏是预处理器做的**纯文本替换**，发生在真正编译之前。
- `#cond` 把参数原样变成字符串，失败时能打印出是哪个条件。
- `__FILE__`、`__LINE__` 是编译器提供的当前文件名和行号。
- `do { ... } while (false)` 是写多语句宏的固定技巧，保证 `if (x) CHECK(y); else ...` 这种写法不会出错。

## 6. Sanitizer：把未定义行为变成明确的报错

前面几课反复用到的 AddressSanitizer（ASan）和 UndefinedBehaviorSanitizer（UBSan），对整个项目也可以一键打开：

```bash
cmake -S . -B out/build/asan -DCMAKE_BUILD_TYPE=Debug \
      -DGAMEBATTLE_BUILD_TESTS=ON -DGAMEBATTLE_BUILD_CONFIG_COMPILER=ON \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build out/build/asan --parallel
ctest --test-dir out/build/asan --output-on-failure
```

实测：`100% tests passed, 0 tests failed out of 6`，当前代码在这两种检测下都是干净的。

| 工具 | 能抓到 | 前几课的例子 |
|---|---|---|
| ASan | 读写已释放的内存、越界、引用了已销毁的临时对象 | 第 2 课扩容后的悬空引用、第 5 课 `<=` 比较函数越界、第 6 课 erase 后继续用迭代器 |
| UBSan | 有符号溢出、移位越界、非法的枚举值等 | 第 7 课先算后查的溢出 |

程序会变慢 2~3 倍，所以只在开发和 CI 里用，不用于生产。Windows 上 MSVC 支持 ASan：`/fsanitize=address`。

**建议把 sanitizer 构建加进 CI**：以后新增功能（比如召唤物）时，很多内存问题在测试阶段就能暴露出来。

## 7. 调试

### 调试战斗逻辑：从测试程序入手

`gamebattle_port` 启动后会一直等待 stdin 上的 ETF 数据，直接调试它很不方便。调试纯战斗逻辑，用 `gamebattle_tests`：

- **CLion**（README 里的步骤）：加载预设，运行配置选 `gamebattle_tests`，在 `BattleRunner::run`、`EffectSystem::apply_damage` 等位置下断点。
- **命令行 gdb**：

```bash
gdb --args out/build/linux-runtime-debug/gamebattle_tests
(gdb) break gamebattle::runtime::EffectSystem::apply_damage
(gdb) run
(gdb) bt                  # 看调用栈：这次伤害是从哪个被动、哪层递归来的
(gdb) print target.hp     # 看变量
(gdb) next                # 单步
(gdb) finish              # 执行到当前函数返回
```

第 6 课那张"连锁触发"的图，用 `bt` 看调用栈时会非常直观。

### 调试 Port 协议

- 让 Erlang 启动 Port，再用调试器**附加**到 `gamebattle_port` 进程上（`gdb -p <PID>`，或 CLion 的 Attach to Process）。
- 或者像第 9 课那样写一个小脚本，按 `{packet, 4}` 格式直接往 Port 的 stdin 里发字节，不需要启动 Erlang。
- 再强调一次：**Port 里不能往 stdout 打印调试信息**，用 `std::cerr`。

### 只在 Release 下出现的 Bug

如果一个问题 Debug 下复现不了、Release 下才出现，首先怀疑**未定义行为**（第 7 课：优化器会利用 UB 删掉你的代码）。用 UBSan 跑一遍，通常能直接定位。

## 8. 部署注意事项

- **在 Linux 上构建 Linux 的产物**。Windows 编出来的 `.exe` / `.dll` 不能部署到 Linux。
- **glibc 版本**：在旧版本系统上编译的程序，通常能在新版本上运行，反过来不行。所以构建机的发行版版本应该**不高于**生产机，最好完全一致（比如用相同的容器镜像）。
- **NIF** 必须用与生产环境**同一个 OTP 主版本**的 `erl_nif.h` 编译。
- Erlang 侧按 `application:get_env(gamebattle, port_executable)` → 环境变量 `GAMEBATTLE_PORT` → `priv/` 目录的顺序查找 Port 可执行文件（`gamebattle_port.erl:103`）。

## 9. 课程回顾：以后扩展时，哪些课的警告会用上

README 的"v1 的明确边界"列出了尚未实现的功能。动手之前，可以先对照这张表：

| 计划中的功能 | 需要注意 | 相关课程 |
|---|---|---|
| **召唤物** | 往 `units` 里新增单位会让 vector 扩容，所有跨越效果执行期间持有的 `auto&` 都可能失效。要么预留空位，要么全部改用下标重新取 | 第 2 课第 4 节、第 4 课第 8 节 |
| **护盾** | 新增 `EffectKind` 要同时改 6 个地方；护盾吸收要插在 `apply_damage` 的哪一步；吸收量的计算要用饱和运算 | 第 10 课第 3.4 节、第 6 课、第 7 课 |
| **能量、技能冷却** | 新的单场状态放进 `RuntimeUnit`；要跨场继承就加进 `UnitInitialState`；每回合重置的计数参考 `reset_round_trigger_counts` | 第 2 课、第 4 课 |
| **复活** | `alive()` 只看 `hp > 0`；`side_defeated`、行动顺序快照都要重新考虑 | 第 4 课 |
| **驱散标签** | `BuffSpec` 加字段 → CSV 表、编译器、加载器、`.gbcfg` 版本号都要动 | 第 10 课 |
| **任何新增的概率判定** | 会改变后续所有随机数的消耗顺序，旧战报在新版本上无法复现 | 第 3 课第 4 节 |

以及 README 里最重要的那条原则：**新机制优先扩展 `EffectKind` 和事件类型，保持 Erlang 请求格式向后兼容；不要在 Erlang 和 C++ 两边各写一份伤害公式。**

## 小结

| 概念 | 要点 |
|---|---|
| 编译与链接 | 每个 `.cpp` 单独编译，最后链接；改头文件影响面大 |
| 静态库 | Port、NIF、测试共用同一份核心代码 |
| `-fPIC` | 静态库要被链接进动态库（NIF）时必须打开 |
| `PUBLIC` / `PRIVATE` | 是否把设置传给使用者 |
| `/utf-8` | MSVC 编译含中文的源码必须加 |
| `PREFIX ""` | 让 NIF 文件名符合 `erlang:load_nif` 的期望 |
| `WILL_FAIL` | 反例测试：错误输入必须被拒绝 |
| `assert` 与 `NDEBUG` | Release 下 `assert` 全部消失；不要在里面写有副作用的代码 |
| Sanitizer | ASan 抓内存错误，UBSan 抓未定义行为，建议放进 CI |
| 调试 | 战斗逻辑用测试程序调；Port 用附加进程或脚本喂字节 |
| 部署 | 在不高于生产版本的 Linux 上构建；NIF 要匹配 OTP 主版本 |

第一部分「项目精读」到这里结束。下一课：[第 12 课：对象模型与多态](12-object-model.md)
