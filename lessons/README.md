# GameBattle C++ 课程

**中文** | [English](en/README.md)

写给以 Erlang 为主语言、很久没写 C++、目标是 **C++ 游戏服务端岗位**的工程师。课程以本仓库的战斗引擎为教材，分七个部分：

| 部分 | 课 | 目标 |
|---|---|---|
| 一、项目精读 | 1~11 | 读懂引擎的每一行，把 C++ 语法细节放回真实代码里讲，用 Erlang 概念作类比 |
| 二、C++ 语言深入 | 12~16 | 对象模型、移动语义、智能指针、模板、STL 原理：面试必问、项目里用得少的部分 |
| 三、并发 | 17~18 | 线程池并发跑真实战斗；原子操作与内存模型 |
| 四、网络与游戏服务器 | 19~20 | epoll TCP 战斗服务器；服务器架构与常用数据结构 |
| 五、性能与线上 | 21~22 | 对引擎做真实的性能分析和模糊测试，记录发现的问题 |
| 六、求职 | 23~24 | 面试题库；简历、项目讲述、动手清单和学习计划 |
| 七、扩展实战 | 25 | 在不改变旧结果的前提下给引擎加新玩法（连锁） |

文中标注"实测"的输出，都是在一台 4 核 Linux 云虚拟机上（g++ 13.3 / clang 18，`-std=c++20`）实际编译运行得到的。第二部分起的实战代码在 [`practice/`](practice/README.md) 目录，它们直接链接真实的引擎，但不修改引擎代码。

## 课程地图

### 第一部分：项目精读

| 课 | 主题 | 对应代码 | C++ 重点 |
|---|---|---|---|
| [1](01-domain-model.md) | 用 Erlang 的眼光读领域模型 | `include/gamebattle/engine.hpp` | 头文件、namespace、定宽整数、`enum class`、struct 默认值、容器、指定初始化、前向声明 |
| [2](02-ownership.md) | 值、引用、指针、const、move | `src/battle_runtime.hpp` | 五种所有权写法、`shared_ptr<const T>`、下标与指针在扩容/删除后的区别、`auto` 与 `auto&`、成员初始化顺序、`std::move` |
| [3](03-class-and-random.md) | 类与确定性随机数 | `Random`、`BattleState` 构造函数 | class/struct、SplitMix64、无符号溢出、初始化列表、`explicit`、显式与隐式构造 |
| [4](04-battle-loop.md) | 回合主循环 | `src/engine.cpp` | 递归 vs 循环、`break` 只跳一层、提前 return、`&` 的优先级、`std::array`、快照 |
| [5](05-target-selector.md) | 选目标 | `src/target_selector.cpp` | `static` 成员函数、lambda 与捕获、`std::sort` 严格弱序、不稳定排序与确定性 |
| [6](06-effect-system.md) | 效果系统 | `src/effect_system.cpp` | 递归触发、`switch`、迭代器、erase-remove、`map[]` 自动插入、先收集再执行、`it = erase(it)` |
| [7](07-integer-safety.md) | 整数安全 | `saturating_add` / `scale` | 有符号溢出 UB、先判断再计算、饱和运算、万分比、窄化转换、符号混用 |
| [8](08-validation-exceptions.md) | 校验与异常 | `validate_request`、`handle_etf` | 栈展开与 RAII、catch 顺序、按引用捕获、自定义异常、`std::function` 递归、三色标记 |
| [9](09-erlang-bridge.md) | 对接 Erlang | `port_main.cpp`、`term.cpp`、`wire.cpp`、`nif.cpp` | `{packet,4}`、字节序、`std::span`、ETF、`std::variant`、函数模板、读写锁、dirty scheduler |
| [10](10-config-pipeline.md) | 配置管线 | `tools/config_compiler.cpp`、`src/config_store.cpp` | `std::from_chars`、确定性输出、CRC32、原子写文件、空壳→冻结、Kahn 拓扑排序、强异常保证 |
| [11](11-build-and-test.md) | 构建、测试与调试 | `CMakeLists.txt`、`tests/` | 编译与链接、CMake 目标、`-fPIC`、presets、`assert` 与 `NDEBUG`、sanitizer、gdb、部署 |

### 第二部分：C++ 语言深入

| 课 | 主题 | 实测亮点 |
|---|---|---|
| [12](12-object-model.md) | 对象模型与多态：虚函数表、虚析构、override、菱形继承、RTTI | 虚函数分派比 `switch` 慢约 45%；非虚析构导致子类成员泄漏 |
| [13](13-move-semantics.md) | 特殊成员函数、值类别、移动语义、完美转发、复制省略 | 只写析构函数让"移动"变成 100 次复制；`initializer_list` 导致本项目编码慢 4 倍 |
| [14](14-smart-pointers.md) | 智能指针深入：控制块、`make_shared`、`weak_ptr`、线程安全 | 4 线程复制同一个 `shared_ptr` 每次约 300 ns |
| [15](15-templates.md) | 模板、concepts、特化、`if constexpr`、折叠表达式、`constexpr`、CRTP | concepts 让报错从 109 行降到 16 行；编译期 CRC 表快 4.3 倍 |
| [16](16-stl-internals.md) | STL 容器内部与迭代器失效 | 一场战斗 1093 次堆分配；编码分配 2 MB 只为输出 148 KB |

### 第三部分：并发

| 课 | 主题 | 实测亮点 |
|---|---|---|
| [17](17-threads-and-pools.md) | 线程、锁、条件变量、线程池 | 4 线程并发跑战斗加速 3.5 倍，结果逐字节一致，TSan 无警告 |
| [18](18-atomics-memory-model.md) | 原子操作、内存序、伪共享、CAS、无锁队列 | relaxed 发布在 x86 上"碰巧正确"但 TSan 报竞争；伪共享慢 4 倍 |

### 第四部分：网络与游戏服务器

| 课 | 主题 | 实测亮点 |
|---|---|---|
| [19](19-epoll-battle-server.md) | epoll 与 TCP 战斗服务器（Erlang 可用 `gen_tcp` 直连） | 粘包/半包/超长帧全部正确处理；p50 2.1 ms、p99 4.2 ms |
| [20](20-game-server-architecture.md) | 服务器架构、同步模式、时间轮、AOI、跳表排行榜、一致性哈希、ECS | 时间轮快 5 倍、九宫格快 140 倍、跳表排名快 8000 倍 |

### 第五部分：性能与线上

| 课 | 主题 | 实测亮点 |
|---|---|---|
| [21](21-performance.md) | 基准测试、profiling、优化验证 | 编码占 78%；修复后单线程吞吐约 900 → 1700 场/秒 |
| [22](22-debugging-and-fuzzing.md) | core dump、gdb、内存问题、模糊测试 | 20 万次模糊测试无崩溃；发现两个真实问题 |

### 第六部分：求职

| 课 | 主题 |
|---|---|
| [23](23-interview-questions.md) | 面试题库：129 道题，按主题分类，链接到对应课程 |
| [24](24-resume-and-pitch.md) | 简历怎么写、三个 STAR 故事、动手清单、6 周计划 |

### 第七部分：扩展实战

| 课 | 主题 | 实测亮点 |
|---|---|---|
| [25](25-chain-extension.md) | 在自动战斗上加游戏王式的连锁：响应触发点、`negate`、后进先出结算、三层校验 | 改前改后 2000 场战斗逐字节相同；故意改坏的代码都被测试发现 |

**建议的读法**：第 1~2 课是后面所有内容的基础，先读透；第 3~11 课按顺序读，每课对照源码。第二部分起每课都可以独立阅读，但第 21~22 课会引用前面的很多数据。第 24 课的"动手清单"建议尽早开始，边学边做。

练习代码：

- `lesson1.cpp`、`lesson2.cpp`：第 1、2 课的可选练习
- [`practice/`](practice/README.md)：第 17~22 课的实战程序（线程池、TCP 服务器、基准测试、模糊测试、数据结构），以及复现已知问题的 `known_issues`

## 课程过程中发现的引擎问题

| 问题 | 影响 | 课 |
|---|---|---|
| 结果编码中 `initializer_list` 导致深复制 | 编码占完整链路约 78% 的时间 | 13、21 |
| 嵌套容器按声明个数预分配 | 501 字节输入让虚拟内存峰值增长 3.8 GB | 22 |
| 两个单位带同一个内联 Buff 时请求被拒绝 | 只影响内联格式的请求 | 22 |
| NIF 二进制没有 RAII 保护 | 异常路径上可能泄漏 | 9、14 |

这些问题都留给你自己修复（第 24 课的动手清单），`practice/known_issues` 可以检查修复是否生效。

## 常用命令

```bash
# 单个练习文件（开启全部常用警告和内存检测）
g++ -std=c++20 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined lessons/lesson2.cpp -o lesson2 && ./lesson2

# 整个项目：Debug + 测试
cmake --preset linux-runtime-debug
cmake --build --preset build-linux-runtime-debug
ctest --test-dir out/build/linux-runtime-debug --output-on-failure

# 整个项目：带 sanitizer 跑全部测试（第 11 课）
cmake -S . -B out/build/asan -DCMAKE_BUILD_TYPE=Debug \
      -DGAMEBATTLE_BUILD_TESTS=ON -DGAMEBATTLE_BUILD_CONFIG_COMPILER=ON \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build out/build/asan --parallel && ctest --test-dir out/build/asan --output-on-failure
```

Windows（VS 2022 Developer PowerShell）编译单个文件：`cl /std:c++20 /W4 /EHsc /Zi /fsanitize=address lessons\lesson2.cpp`。项目整体构建见仓库根目录的 `README.md`。

## Erlang ↔ C++ 速查表

| Erlang | C++ | 课 |
|---|---|---|
| `.hrl` / `-include` | `.hpp` / `#include` | 1 |
| 模块名前缀 `gamebattle:` | `namespace gamebattle::` | 1 |
| 不导出的函数 | 匿名 `namespace { }` | 1 |
| 原子 `attacker` | `enum class Side { attacker }` | 1 |
| `-record(stats, {hp = 1})` | `struct Stats { std::int64_t hp{1}; };` | 1 |
| `#{id => 1}` | `UnitConfig{.id = 1}`（按声明顺序） | 1 |
| list | `std::vector<T>` | 1 |
| map | `std::unordered_map<K, V>`（无序）/ `std::map<K, V>`（有序） | 1、10 |
| binary | `std::string` / `std::vector<std::uint8_t>` | 1、9 |
| `undefined \| V` | `std::optional<T>` | 1 |
| 大 binary 的引用计数共享 | `std::shared_ptr<const T>` | 2 |
| 子 binary 引用 | `std::span<T>` / `std::string_view`（只借用，不拥有） | 9 |
| 任意 term | `std::variant<...>` | 9 |
| 带 guard 的函数子句 | `std::get_if<T>` / `std::visit` + 重载 | 9 |
| `fun(X) -> ... end` | `[捕获](auto x) { ... }` | 5 |
| `lists:sort(fun(A, B) -> A =< B end, L)` | `std::sort(..., [](a, b) { return a < b; })`（**严格小于**） | 5 |
| `lists:filter/2` | erase-remove / `std::erase_if` | 6 |
| `lists:search/2` 返回 `false` | `std::find_if` 返回 `end()` | 6 |
| `maps:get(K, M, Default)` | `opt.value_or(Default)` | 6 |
| `maps:update_with(K, F, Init, M)` | `++map[key]`（不存在时自动插入） | 6 |
| `{A, B} = Tuple` | `auto [a, b] = pair;` | 8 |
| `try ... catch Class:Reason` | `try { } catch (const T& e) { }` | 8 |
| `erlang:raise/3` 原样重抛 | `throw;` | 10 |
| `init/1` 返回 `{stop, Reason}` | 构造函数抛异常 | 3 |
| `div` / `rem` | `/` / `%`（都向零取整） | 7 |
| `band` / `bxor` / `bsr` | `&` / `^` / `>>` | 3、4 |
| `<<Len:32/big>>` | 手动移位拼装 | 9 |
| `erlang:crc32/1` | 项目里的 `crc32()`（同一个算法） | 10 |
| `digraph_utils:topsort/1` | Kahn 算法 | 10 |
| 注册进程 / `persistent_term` | 函数内 `static` 对象 | 9 |
| 监督树重启崩溃进程 | 无；未捕获的异常会终止整个 OS 进程 | 8 |

## 十二条最重要的规则

这些是课程里反复出现、最容易出 Bug 的地方。全部都有实测例子。

1. **内置类型不初始化就是垃圾值**，结构体里的整数一律写 `{0}`。（第 1 课）
2. **`auto` 是副本，`auto&` 才是引用**。要修改原对象就写 `auto&`。（第 2 课）
3. **修改容器之后，之前拿到的指针、引用、迭代器都可能失效**。长期关联用下标（不删除的容器）或唯一 ID（会删除的容器）。（第 2、6 课）
4. **成员按声明顺序初始化**，和初始化列表的书写顺序无关。（第 2 课）
5. **返回成员变量要写 `std::move`**，返回局部变量不用写。（第 2 课）
6. **单参数构造函数一律加 `explicit`**。（第 3 课）
7. **新增或删除一次随机判定，会让同一个 seed 的后续结果全部错位**。（第 3 课）
8. **`break` 只跳出最内层循环**；位运算和比较写在一起要加括号。（第 4 课）
9. **`std::sort` 的比较函数必须用 `<`**，用 `<=` 会越界；最后一句比较唯一 ID，保证结果确定。（第 5 课）
10. **一边遍历一边删除，只能写 `it = erase(it)`**；遍历过程中可能修改容器时，先收集 ID 快照再执行。（第 6 课）
11. **有符号溢出是未定义行为，先算后查会被优化器删掉**。溢出检查必须在计算之前做。（第 7 课）
12. **异常要按 `const&` 捕获、具体类型写在前面**；C++ 的异常没人捕获就会终止整个进程，所以 Port/NIF 边界必须用 `catch (...)` 兜底。（第 8 课）

外加两条工程规则：Port 进程里**不要往 stdout 打印任何东西**（第 9 课）；Release 模式下 **`assert` 会被删除**，测试用 Debug 构建（第 11 课）。

第二部分以后的几条：

13. **只写了析构函数，类就失去了移动能力**；能用零法则就用零法则。（第 13 课）
14. **自己写的移动操作要标 `noexcept`**，否则 vector 扩容时会退回复制。（第 13 课）
15. **装大对象时不要用花括号列表初始化**，`initializer_list` 只能复制。（第 13、21 课）
16. **基类析构函数要么是 public virtual，要么类标 `final`**。（第 12 课）
17. **只是使用对象时，按 `const&` 传 `shared_ptr`**。（第 14 课）
18. **先测量再优化；基准测试先验证结果一致，再计时；不合理的数字要怀疑**。（第 21 课）
19. **外部输入里的长度字段，分配内存前要和剩余数据量比较**。（第 22 课）
