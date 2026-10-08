# 战斗引擎性能对比：NIF、Port 与纯 Erlang

**中文** | [English](engine-benchmark.en.md)

## 结论

- **按现在的接口，换成 C++ 不值得，反而更慢。** 对 Erlang 调用方来说，同一场战斗纯 Erlang 引擎最快：示例战斗 0.30 ms，NIF 1.02 ms，Port 1.20 ms；四个调用方并发时，纯 Erlang 每秒约 9400 场，NIF 约 1700 场，Port 一个 worker 约 800 场、四个 worker 约 1800 场。
- **C++ 引擎本身确实快。** 只算战斗本身，C++ 比 Erlang 快约 4–8 倍，战斗越长差距越大。
- **差距来自结果的传递方式。** C++ 把战报编码成 ETF，每个事件都是一个 12 个键的 map；Erlang 再把它解码回来。示例战斗里，C++ 模拟只用 64 µs，ETF 编码却用 323 µs，Erlang 解码又用 456 µs。
- **要让 C++ 值得，需要先改结果格式：** 只把服务端要用的摘要（胜负、单位血量）作为 Erlang term 返回，事件日志直接输出成发给客户端的 protobuf 字节。按实测的各阶段耗时推算，这样示例战斗约 0.13 ms，比纯 Erlang 快约 2 倍；长战斗约 1.6 ms，快约 6 倍。算上给客户端编码 protobuf 的开销，差距约为 5 倍和 13 倍。
- **建议：**
  - 战斗量不大、战斗较短时，用纯 Erlang 引擎。它最简单，不会拖垮节点，也能热更新。
  - 高并发 PvP、长战斗或大量重算的场景，用 C++，但要先改结果格式。

## 测了什么

三种方式运行同一个引擎：

| 方式 | 说明 |
|---|---|
| `nif` | C++ 在 BEAM 进程内的 dirty CPU 调度器上运行 |
| `port` | C++ 在独立的操作系统进程里运行，经 `{packet, 4}` 管道通信 |
| `erlang` | `gamebattle_erl`：逐行移植的纯 Erlang 引擎，在调用进程里运行 |

三者结果完全相同。基准开始前会先核对每个请求的结果。此外，差异测试把纯 Erlang 引擎和 C++ Port 逐个比对，下面这些用例全部一致，结果和错误信息都相同：

- 20,000 个随机合法请求，覆盖所有事件类型和结束原因，包括连锁、无效和失效；
- 5,000 个随机非法请求；
- 3,000 个使用配置包的请求；
- 3,000 个损坏的配置包。

每次调用的计时包含调用方要付出的全部开销：编码请求、模拟、解码结果。

场景：

| 场景 | 内容 | 平均事件数 |
|---|---|---:|
| `example` | `gamebattle:example_request()`：双方各 2 名英雄，另有美人、宠物、神兵等辅助单位 | 226 |
| `random_mix` | 200 个随机请求，Buff、反应和连锁很多 | 201 |
| `long` | 每方 6 名英雄，群攻、可叠层的持续伤害、治疗被动，打满 50 回合 | 4834 |

环境：4 vCPU（Intel Xeon 2.8 GHz）云容器，Erlang/OTP 25（JIT），C++ 用 GCC 13 `-O3` 编译。绝对数值只适用于这台机器；更有参考价值的是三者之间的比例。

## 结果

### 单个调用方（顺序执行）

| 场景 | 方式 | 平均 | p50 | p99 | 每事件 |
|---|---|---:|---:|---:|---:|
| example | nif | 1.02 ms | 0.98 ms | 1.62 ms | 4.5 µs |
| | port | 1.20 ms | 1.10 ms | 2.43 ms | 5.3 µs |
| | **erlang** | **0.30 ms** | **0.27 ms** | **0.53 ms** | **1.3 µs** |
| random_mix | nif | 1.25 ms | 0.87 ms | 9.04 ms | 6.2 µs |
| | port | 1.55 ms | 1.12 ms | 10.9 ms | 7.7 µs |
| | **erlang** | **0.61 ms** | **0.41 ms** | **3.89 ms** | **3.0 µs** |
| long | nif | 24.5 ms | 23.6 ms | 33.3 ms | 5.1 µs |
| | port | 33.7 ms | 32.6 ms | 46.6 ms | 7.0 µs |
| | **erlang** | **8.9 ms** | **8.3 ms** | **14.9 ms** | **1.8 µs** |

### 四个调用方并发（每秒场数）

| 场景 | nif | port（1 个 worker） | port（4 个 worker） | erlang |
|---|---:|---:|---:|---:|
| example | 1685 | 802 | 1805 | **9404** |
| random_mix | 1729 | 703 | 1470 | **5434** |
| long | 80 | 31 | 88 | **447** |

第二次完整运行的结果与上表相差不超过 15%。

- 纯 Erlang 引擎随调度器数扩展：4 个调用方时吞吐约为单个调用方的 2.8 倍。
- 一个 Port worker 一次只算一场，这是刻意的背压设计；要并发就得开多个 worker。
- NIF 并发时只提升约 1.7 倍。可能的原因是大块结果的解码和垃圾回收与 dirty 调度器争用同样的 4 个核。

## 时间花在哪

单位 µs。C++ 各阶段在 C++ 内直接测量；Erlang 各阶段取中位数。

| 阶段 | example | random_mix | long |
|---|---:|---:|---:|
| **纯 Erlang 引擎**：解析与校验 | 29 | 155 | 205 |
| 纯 Erlang 引擎：模拟（含构建结果 map） | 245 | 550 | 9192 |
| **C++**：ETF 解码请求 | 24 | 83 | 77 |
| C++：解析请求 | 16 | 54 | 36 |
| C++：模拟 | **64** | **94** | **1152** |
| C++：把结果编码成 ETF | 323 | 297 | 8650 |
| Erlang：`term_to_binary(请求)` | 10 | 53 | 29 |
| Erlang：`binary_to_term(结果)` | 456 | 629 | 9360 |
| 对照：C++ 把事件紧凑编码（类似 protobuf） | 16 | 15 | 339 |
| 对照：Erlang 用 gpb 把结果编码成 protobuf | 398 | 691 | 12593 |

- **纯计算：C++ 比 Erlang 快 3.8 倍（example）、5.8 倍（random_mix）、8 倍（long）。** C++ 每个事件约 0.24–0.47 µs，Erlang 约 1.1–2.7 µs。profile 里没有单一热点，差距分散在不可变数据（每次修改单位都要复制元组）、64 位随机数的大整数运算和普遍的函数调用开销上。
- **C++ 路径的大头在边界上。** 示例战斗：ETF 编码 323 µs 加 Erlang 解码 456 µs，约是模拟本身（64 µs）的 12 倍。主要是因为每个事件都是 12 个键的 map，键和枚举值都是原子：Erlang 解码时要为每个原子查一次原子表，再逐个构建 map。
- **纯 Erlang 引擎没有这道边界：** 结果 map 直接在调用进程里构建，键都是字面量原子。

## 值不值得换成 C++

**按现在的接口：不值得。** 在三个场景里，Erlang 调用方拿到结果 map 的耗时，纯 Erlang 引擎都比 NIF 快 2–3.5 倍，比 Port 快 2.5–4 倍。

**改结果格式之后：值得，尤其是长战斗。** 服务端逻辑真正要读的只有胜负、结束原因和各单位血量（结算、车轮战续战）。事件日志通常原样发给客户端。如果 C++ 返回小的摘要 term，再加上已经编码好的 protobuf `BattleReport` 字节，就能省掉 ETF 编码、Erlang 解码和 gpb 编码三笔开销。按上表的实测阶段耗时推算：

| | example | long |
|---|---:|---:|
| 纯 Erlang：得到结果（中位数） | 0.27 ms | 9.4 ms |
| 纯 Erlang：得到结果并编码成客户端 protobuf | 0.67 ms | 22 ms |
| C++ 返回紧凑结果（推算） | 约 0.13 ms | 约 1.6 ms |

推算值等于请求编码、C++ 解码、解析、模拟、紧凑编码五项之和，没有计入 Port 的管道传输（几十 µs）。

**换成 C++ 之前还要考虑：**

| | 纯 Erlang | NIF | Port |
|---|---|---|---|
| 引擎代码崩溃 | 只影响一个进程 | 整个节点崩溃 | Port 进程退出，监督树重启 |
| 死循环、超长战斗 | 可抢占，不影响其他进程 | 占住一个 dirty 调度器，无法从 Erlang 侧杀掉 | 超时后关闭 Port |
| 并发 | 随调度器数扩展 | 受 dirty 调度器数限制 | 需要 worker 池 |
| 部署 | 只有 beam 文件，可热更新 | 需按 OTP 主版本编译 | 需要每个平台的可执行文件 |
| 维护 | 一份代码 | 两份引擎都要维护，并且必须保持一致 | 同 NIF |

如果两套引擎都保留（例如开发期用 Erlang，线上用 C++），修改规则时两边要同步改。`erlang/test/gamebattle_erl_tests.erl` 的差异测试会逐场比对两边的结果，任何不一致都会被它发现。

**建议：**

1. 战斗量在单核每秒约 3000 场示例战斗以内，且以短战斗为主：直接用 `gamebattle:simulate(erlang, Request)`。
2. 需要更高吞吐，或者有很多长战斗：保留 C++，但先把结果改成“摘要 term + protobuf 事件字节”，再用 Port worker 池或 NIF。
3. 无论选哪种，先在生产机型上用下面的命令重跑一遍，用真实战斗数据替换示例场景。

## 多场并发

7v7、每人几十个被动、按回合限次的大战斗，4 核机器上同时打 1–16 场。“其他进程卡顿”是一个每 1 ms 醒一次的探针进程测到的额外延迟，代表游戏逻辑会被拖慢多少。

| 场景 | 方式 | 单场 | 4 核饱和吞吐 | 其他进程卡顿 p99 |
|---|---|---:|---:|---:|
| 每人 40 个混合被动、每回合 3 次（约 48,000 个事件） | 纯 Erlang | 178 ms | 21 场/秒 | ≤ 5 ms |
| | 现有 NIF | 283 ms | 8.6 场/秒 | 约 210 ms |
| | 现有 Port（4 个 worker） | 313 ms | 12 场/秒 | 50–120 ms |
| | C++ 加紧凑结果（不含传输） | 22 ms | 168 场/秒 | — |
| 每人 20 个连锁被动、每回合 3 次（约 110,000 个事件） | 纯 Erlang | 413 ms | 9 场/秒 | ≤ 5 ms |
| | 现有 NIF | 640 ms | 3.6 场/秒 | 2–215 ms |
| | 现有 Port（4 个 worker） | 665 ms | 4.8 场/秒 | 36–88 ms |
| | C++ 加紧凑结果（不含传输） | 40 ms | 91 场/秒 | — |

- 超过核数后，吞吐不再增长，纯 Erlang 下所有战斗一起变慢；同时打 16 场时，每场比单打慢约 4 倍。用固定大小的战斗池排队，比不限并发的平均耗时更低。
- 按现在的 ETF 结果格式，NIF 和 Port 在高并发时会让节点上的其他进程卡上百毫秒；纯 Erlang 的抢占式调度不会。
- 纯 Erlang 引擎每场运行中的大战斗占 10–18 MB 内存，主要是事件日志。

每秒 100 场需要多少核、实测压测结果，见 [client/README.md](../client/README.md)。

## 如何复现

先用 Release 模式构建 Port 和 NIF，再运行基准：

```bash
cmake -S . -B out/build/bench -DCMAKE_BUILD_TYPE=Release \
  -DGAMEBATTLE_BUILD_NIF=ON -DGAMEBATTLE_BUILD_CONFIG_COMPILER=ON \
  -DERLANG_ERTS_INCLUDE_DIR="$(erl -noshell -eval 'io:format("~s/erts-~s/include", [code:root_dir(), erlang:system_info(version)]), halt().')"
cmake --build out/build/bench --parallel
cmake --install out/build/bench --prefix erlang --component BattleRuntime

cd erlang
rebar3 as test compile
erl -noshell -pa _build/test/lib/gamebattle/ebin _build/test/lib/gamebattle/test \
    -eval 'gamebattle_bench:run(), halt().'
```

差异测试（需要 C++ Port）：

```bash
cd erlang
GAMEBATTLE_PORT=priv/gamebattle_port \
GAMEBATTLE_TEST_CONFIG=../out/build/bench/generated-config/example.gbcfg \
  rebar3 eunit --module=gamebattle_erl_tests
```

`gamebattle_erl_tests:differential(1, 20000)` 可以跑任意数量的随机用例（先启动 `gamebattle` 应用）。它返回结果不一致的种子列表，为空表示全部一致。
