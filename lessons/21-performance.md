# 第 21 课：性能分析与优化

**中文** | [English](en/21-performance.md)

> 第五部分「性能与线上」的第一课。实战代码：`practice/battle_bench.cpp`、`practice/encode_bench.cpp`、`practice/direct_encode.hpp`。
>
> 这一课完整记录了我在本项目上做的一次真实的性能分析：从测量、定位、找到原因，到做实验验证修复方案。整个过程本身就是面试时可以讲的项目经历（第 24 课）。

## 1. 方法：先测量，再动手

```
1. 建立可重复的基准测试 ──▶ 2. 找到最耗时的部分（profile）──▶ 3. 提出原因假设
        ▲                                                            │
        │                                                            ▼
6. 再测一次，确认收益 ◀── 5. 验证结果没有变（正确性）◀── 4. 做最小的修改
```

最常见的错误是跳过前两步，凭直觉优化。本项目就是一个很好的反例：直觉上最耗时的应该是**战斗模拟**（几百个回合、上千个事件、递归触发），测下来它只占 15%。

**阿姆达尔定律**：一个部分只占总时间的 15%，就算把它优化到 0，总体也只能快 1.18 倍。所以一定要先找到真正的大头。

## 2. 基准测试：测完整链路的每一段

`practice/battle_bench.cpp` 模拟 Port 收到一个请求后的完整处理过程，分四段计时：ETF 解码 → 解析成 `BattleRequest` → 模拟 → 编码结果。3000 场不同 seed 的 5v5 战斗，Release 构建（实测，两次运行）：

```
iterations: 3000, request bytes: 8674, avg events/battle: 879.4

stage                              us/battle     share
term::decode                        49.5~56.2   4.3~4.5%
wire::parse_request                 20.7~23.9   1.8~1.9%
Engine::simulate                   163.1~217.6  14.8~16.5%
encode_result + term::encode       868.1~1022.4 77.5~78.8%
total                             1101.5~1320.0
```

**把结果编码成 ETF 占了将近 80% 的时间**，是战斗模拟本身的 4~5 倍。

两次运行的总时间相差 20%：这是一台共享的云虚拟机，噪声很大。所以每个结论都至少跑两次，看的是**比例**和**稳定的倍数差**，不是某一次的绝对值。

### 写基准测试的几个坑

**坑 1：被优化器删掉的代码。** 计算结果没有被使用，编译器就可以把整个计算删掉。这门课里我自己就踩了两次：

- 第 14 课测"按 `const&` 传 `shared_ptr`"，第一次测出 1000 万次调用只要 0.2 ms，相当于每次 0.02 ns，比 CPU 一个时钟周期还短。原因是 GCC 分析出那个函数没有副作用，把调用提到了循环外面。改用 `__attribute__((noipa))` 禁止这种跨函数分析后，才得到真实的 10~15 ms。
- 第 20 课测跳表排名，第一次测出 3 ns，同样不可能。原因是求和结果没有输出，循环被整个删掉了。把结果打印出来之后，才得到真实的 2 µs。

**防御办法**：把结果写进 `volatile` 变量或者打印出来；对明显不合理的数字保持怀疑，算一下"每次操作折合多少纳秒、多少个时钟周期"。

**坑 2：测 Debug 构建。** 实测同一个基准程序：

```
Debug   (-O0): simulate 1796 us，encode 27784 us，总计 30693 us
Release (-O3): simulate  159 us，encode   844 us，总计  1073 us
```

差了 **29 倍**，而且各部分的比例也变了（`-O0` 下编码占 90%）。**只测、只分析 Release 构建**（需要符号信息时用 `RelWithDebInfo`，也就是 `-O2 -g`）。

**坑 3：没有预热。** 第一次运行时缓存是冷的，内存分配器也还没有准备好内存池。`battle_bench` 正式计时前先跑 50 次。

**坑 4：变化比噪声还小。** 我试了链接时优化（LTO，`-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`），两轮对比的结果是一次慢了 23%、一次快了 4%，完全在噪声范围内。**没有重复测量和统计，就不能说某个改动"快了 5%"**。认真的做法是多跑几轮，取中位数，再比较。

## 3. 定位：用 profiler 看时间花在哪里

基准测试告诉你"编码慢"，profiler 告诉你"编码里面哪一行慢"。

### valgrind callgrind

这台机器上没有 `perf`，所以用 valgrind 的 callgrind（它模拟执行每一条指令并计数，会慢几十倍，但结果非常精确、可重复）：

```bash
cmake -S lessons/practice -B build-prof -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-prof --target battle_bench
valgrind --tool=callgrind --callgrind-out-file=cg.out ./build-prof/battle_bench 150
callgrind_annotate --inclusive=yes cg.out | head -40
```

结果（按"包含子函数在内的指令数"排序，节选）：

```
40.46%  wire::encode_result(BattleResult const&)
30.98%  term::encode(Value const&)
21.29%  std::vector<std::pair<std::string, Value>>::vector(const vector&)   ← 复制构造！
13.16%  std::vector<unsigned char>::_M_range_insert                          ← 输出缓冲区一点点地插入
11.57%  Engine::simulate
11.31%  std::vector<Value>::vector(const vector&)                            ← 复制构造！
```

线索非常明确：**大量时间花在 `vector` 的复制构造函数上**。编码结果本该只是"读取结果、写出字节"，为什么要复制？

### 在有 perf 的机器上

真实服务器上更常用 `perf`（采样式，开销很小，可以直接在线上跑）：

```bash
perf record -g ./battle_bench 3000      # 采样调用栈
perf report                              # 交互式查看热点
# 火焰图：perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

火焰图里横向宽度代表耗时占比，一眼就能看出最宽的那几块是谁。

## 4. 找到原因：`initializer_list` 只能复制

第 13 课第 6 节详细讲过。`wire.cpp` 的 `encode_result` 这样构造结果：

```cpp
return Value::object({
    ...
    {"events", Value::list(std::move(events))},    // 以为是移动
    {"units", Value::list(std::move(units))}
});
```

花括号列表会先构造一个 `std::initializer_list`，它的元素是 **`const`** 的，`Value::object` 只能从里面复制。于是整个事件列表（约 900 个事件，每个 12 个字段）被深复制了一遍。每个事件内部的 `Value::object({...})` 也一样。callgrind 里那两行复制构造就是这么来的。

再加上一个次要原因：`term.cpp` 的 `Writer` 写输出时没有预先 `reserve`，每写几个字节就 `insert` 一次，输出缓冲区反复扩容（那 13% 的 `_M_range_insert`）。

第 16 课实测的分配数据也印证了这一点：**编码一场战斗分配了 2 MB 内存，而输出只有 148 KB**。

## 5. 验证修复方案

我没有改引擎代码，而是在 `practice/encode_bench.cpp` 里写了两种替代实现，和原实现放在一起比较：

- **A**：原实现（`initializer_list` 构建 `Value` 树，再 `term::encode`）。
- **B**：同样构建 `Value` 树，但用 `reserve` + `emplace_back` 移动元素，不再有 `initializer_list`（用到了第 15 课的可变参数模板和折叠表达式）。
- **C**：完全不构建中间的树，直接从 `BattleResult` 流式写出 ETF 字节，预先估算大小 `reserve` 一次（`practice/direct_encode.hpp`）。

**第一步不是计时，而是验证正确性**：程序先对 1000 场战斗分别用三种方法编码，确认输出**逐字节相同**，否则直接报错退出。然后才计时（实测三次运行）：

```
all 1000 results: A, B and C produce byte-identical ETF

A  initializer_list tree + term::encode : 1092~1170 us/battle
B  moved tree + term::encode            :  555~731  us/battle  (x1.5~2.0)
C  direct streaming writer              :  246~307  us/battle  (x3.6~4.4)
```

再放回完整链路里测，看对总体有多大影响（`battle_bench` 的最后一行，同样先验证前 100 场输出逐字节相同）：

```
with streaming encoder (byte-identical on first 100 battles): 575~613 us/battle, 1632~1739 battles/s, x1.8~1.9
```

**单线程吞吐量从约 900 场/秒提升到约 1700 场/秒，接近翻倍**。编码只是从 4~5 倍于模拟，变成了和模拟差不多。

### B 和 C 怎么选

| | B：修正复制 | C：流式写出 |
|---|---|---|
| 收益 | 约 1.9 倍 | 约 4 倍 |
| 改动范围 | 只改 `encode_result` 内部的写法 | 新增一个专用的编码器 |
| 维护成本 | 低：仍然通过通用的 `Value` 结构 | 中：`BattleResult` 加字段时，要同步修改编码器；而且必须和 `term.cpp` 的整数编码规则保持一致 |
| 风险控制 | 现有测试即可覆盖 | 需要一个"与通用编码器逐字节对比"的测试长期保留（`encode_bench` 就是） |

实际项目中我会先做 B（低风险、立刻翻倍），如果编码仍是瓶颈再上 C，并把逐字节对比的测试加进 CI。**把取舍讲清楚，比只讲"我优化了 4 倍"更能打动面试官。**

> 这个修改留给你自己完成：在 `src/wire.cpp` 里把 `encode_result` 改成 B 的写法，跑通 `ctest` 和 `encode_bench` 的逐字节对比，提交到你自己的分支。这会是你简历上一个真实、可验证的优化（第 24 课）。

## 6. 下一个瓶颈在哪里

编码问题解决后，模拟就成了最大的一块。第 16 课的实测数据给出了方向：

| 观察 | 数据 | 可以尝试的方向 |
|---|---|---|
| 每场战斗大量堆分配 | 1093 次分配 / 318 KB | 估算事件数后 `result.events.reserve(...)`；`TargetSelector` 改用复用的缓冲区（`thread_local` 或成员变量） |
| `Event` 太大 | 136 字节，其中两个 `std::string` 占 64 字节 | `phase` 和 `type` 改为 `enum class : uint8_t`，编码时再转原子 |
| 多线程下分配器有竞争 | TCP 服务器测试中 1566 次 `mprotect` 系统调用（第 19 课） | 减少分配；或换用 jemalloc / mimalloc（`LD_PRELOAD` 即可试验） |
| 随机分支 | 第 12 课：分派开销主要来自分支预测失败 | 一般不值得为此改设计 |

每一项都要按第 1 节的流程：先测、再改、验证结果不变、再测。

另外，C++17 的 `std::pmr`（多态内存资源）提供了一种"一场战斗用一块内存池，战斗结束整块释放"的方式：

```cpp
std::array<std::byte, 256 * 1024> buffer;
std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size());
std::pmr::vector<Event> events(&arena);      // 从 arena 里分配，不调用 malloc
```

这需要把 `BattleState` 里的容器换成 `std::pmr` 版本，是一个比较大的改动，适合在确认分配确实是瓶颈后再做。

## 7. 编译器能帮你做什么

| 选项 | 作用 | 本项目实测或说明 |
|---|---|---|
| `-O2` / `-O3` | 常规优化 | 比 `-O0` 快 29 倍 |
| LTO（`-flto`） | 链接时跨 `.cpp` 文件内联和优化 | 两轮测试结果在噪声范围内，没有可测量的收益 |
| PGO（`-fprofile-generate` / `-fprofile-use`） | 先跑一遍收集分支和调用的热度，再按热度优化 | 对分支多的代码常有 10%~30% 收益，需要有代表性的训练数据 |
| `-march=native` | 使用本机 CPU 支持的全部指令集 | **小心**：编出来的程序可能在旧 CPU 上直接崩溃，生产部署一般指定一个保守的目标（比如 `-march=x86-64-v2`） |

## 8. 并发和延迟的数据

前面几课的实测数据，汇总在这里：

| 场景 | 数据 | 课 |
|---|---|---|
| 线程池并发跑战斗 | 4 线程加速 3.5 倍，结果与单线程逐字节相同 | 17 |
| 线程池调度开销 | 空任务：1 个工作线程约 2 µs，4 个约 20 µs | 17 |
| TCP 服务器延迟 | 单连接串行：p50 2.1 ms，p99 4.2 ms | 19 |
| TCP 服务器吞吐 | 8 个连接、200 场战斗用时 0.15 s | 19 |
| 伪共享 | 两个独立计数器挨在一起慢 4 倍 | 18 |
| AoS vs SoA | 只更新一个字段，SoA 快 13 倍 | 20 |

**延迟要看分位数，不能只看平均值**。p99 = 4.2 ms 表示 100 个请求里最慢的 1 个要 4.2 ms。游戏服务器关心的往往是"最慢的那些玩家体验如何"，平均值会把这些掩盖掉。

## 9. 优化的优先级

```
1. 算法和数据结构  ── O(n) → O(log n)，跳表排名快 8000 倍（第 20 课）
2. 不做多余的工作  ── 去掉深复制，编码快 2~4 倍（本课）
3. 减少内存分配    ── reserve、复用缓冲区、内存池
4. 数据布局        ── SoA、缩小结构体、避免伪共享
5. 并行            ── 线程池，4 核加速 3.5 倍
6. 编译器选项      ── LTO / PGO，收益通常在噪声级别到 30% 之间
7. 微观优化        ── 查表 CRC 快 4 倍，但对 413 字节的配置毫无意义（第 15 课）
```

越往上，收益越大、风险越小。

## 面试题

**Q1：你做过哪些性能优化？怎么做的？**
（用本课的经历回答）先写了覆盖完整链路的分段基准测试，发现结果编码占约 78% 的时间；用 callgrind 定位到大量时间花在 `vector` 的复制构造上，原因是用 `initializer_list` 构造嵌套的 map 导致整个事件列表被深复制；写了两种替代实现，先验证 1000 场战斗的输出逐字节相同，再计时：去掉复制快约 1.9 倍，流式写出快约 4 倍，完整链路吞吐从约 900 场/秒提升到约 1700 场/秒。

**Q2：怎么写一个可信的基准测试？**
测 Release 构建；先预热；计算结果要被使用（`volatile` 或打印），防止被优化掉；多次运行看稳定性；对明显不合理的数字（比如每次操作不到一个时钟周期）保持怀疑；比较前后先验证结果一致。

**Q3：常用的性能分析工具有哪些？**
`perf`（采样，开销低，可用于线上）、火焰图、valgrind 的 callgrind（指令级精确计数）和 massif（堆内存）、gprof、Intel VTune。还要配合业务层面的指标：每秒处理量、延迟分位数。

**Q4：什么是阿姆达尔定律？**
优化某一部分带来的总体加速，受限于这部分在总时间中的占比。本项目中模拟只占约 15%，把它优化到无限快也只能总体快约 1.18 倍；而占 78% 的编码部分优化 4 倍，就能让总体快近 2 倍。

**Q5：为什么不能在 Debug 构建上做性能测试？**
未优化的代码可能慢几十倍（实测 29 倍），而且各部分的相对比例也会改变，导致找错瓶颈。分析时用 `RelWithDebInfo`，既有优化又有符号信息。

**Q6：减少内存分配有哪些手段？**
`reserve` 预分配；复用缓冲区（成员变量或 `thread_local`）；对象池；`std::pmr` 内存池（一次请求一个 arena，结束时整体释放）；避免不必要的复制（`initializer_list`、按值传参）；换用多线程友好的分配器（jemalloc、tcmalloc、mimalloc）。

**Q7：平均延迟和 p99 延迟哪个更重要？**
都要看，但 p99（以及 p999）更能反映最差情况下的用户体验，平均值会掩盖少数非常慢的请求。本项目 TCP 服务器实测 p50 为 2.1 ms、p99 为 4.2 ms。

下一课：[第 22 课：线上问题排查与模糊测试](22-debugging-and-fuzzing.md)
