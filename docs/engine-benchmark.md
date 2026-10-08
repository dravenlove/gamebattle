# 战斗引擎性能对比：NIF、Port 与纯 Erlang

**中文** | [English](engine-benchmark.en.md)

## 结论

- **用紧凑结果后，C++ 比纯 Erlang 快：短战斗约 3 倍，大战斗 7–9 倍。** 4 核机器上四个调用方并发时，7v7 大规模被动战斗（每场 48,000 个事件）NIF 每秒 96 场，四个 Port worker 89 场，纯 Erlang 13 场，约 7 倍；每场 110,000 个事件时是 55 场对 6 场，约 9 倍。
- **紧凑结果就是请求带上 `report` 选项**（`summary`、`actions` 或 `events`）。这时 C++ 只返回服务端要用的摘要（胜负、结束原因、各单位 HP），再加上已经编码成 protobuf 的客户端 `BattleReport`；以前则是把每个事件都变成 Erlang map 返回。见 README 的[紧凑结果](../README.md#紧凑结果)。
- **为什么差这么多：** C++ 模拟一直很快，慢的是把结果交给 Erlang。一场 48,000 个事件的战斗，C++ 模拟用 18 ms，之后把事件编码成 ETF 却要 110 ms，Erlang 解码还要更久。紧凑结果把这些全省掉了：C++ 写出 protobuf 战报只用 3.5 ms。
- **发给客户端的应该是按行动聚合的战报（`actions`）。** 它把每次行动的事件汇总起来：48,000 个事件的战斗从 1.5 MB 降到 240 KB，110,000 个事件的从 3.8 MB 降到 125 KB。
- **建议：**
  - 战斗较短、量不大时，纯 Erlang 引擎就够了。它最简单，不会拖垮节点，也能热更新。
  - 长战斗、大量被动连锁或高并发时，用 C++ 并带上 `report` 选项：NIF，或者 Port worker 池。
  - 如果同一个节点还跑玩法逻辑，要给它留一个核：NIF 占满所有核时会让其他进程延迟 30–50 ms。可以让 dirty CPU 调度器比核数少一个（4 核用 `+SDcpu 3:3`，吞吐少约四分之一），或者改用 Port worker。

## 测了什么

三种方式运行同一个引擎：

| 方式 | 说明 |
|---|---|
| `nif` | C++ 在 BEAM 进程内的 dirty CPU 调度器上运行 |
| `port` | C++ 在独立的操作系统进程里运行，经 `{packet, 4}` 管道通信 |
| `erlang` | `gamebattle_erl`：逐行移植的纯 Erlang 引擎，在调用进程里运行 |

三者结果完全相同，紧凑结果也一样，基准开始前会先核对每个请求的结果。此外，差异测试把纯 Erlang 引擎和 C++ Port 逐个比对，下面这些用例全部一致，结果和错误信息都相同：

- 20,000 个随机合法请求，覆盖所有事件类型和结束原因，包括连锁、无效和失效；
- 15,000 个带 `report` 选项的随机请求，每档 5,000 个，战报字节也完全相同；
- 7,000 个随机非法请求，其中包括非法的 `report` 取值；
- 5,000 个使用配置包的请求，其中 2,000 个带战报；
- 3,000 个损坏的配置包。

不带 `report` 的请求，得到的字节和引入紧凑结果之前完全一样：用上一版 Port 比对了 4,001 个请求。

每次调用的计时包含调用方要付出的全部开销：编码请求、模拟、解码结果。带 `report` 时，结果里包含编码好的客户端战报。

场景：

| 场景 | 内容 | 平均事件数 |
|---|---|---:|
| `example` | `gamebattle:example_request()`：双方各 2 名英雄，另有美人、宠物、神兵等辅助单位 | 226 |
| `random_mix` | 200 个随机请求，Buff、反应和连锁很多 | 201 |
| `long` | 每方 6 名英雄，群攻、可叠层的持续伤害、治疗被动，打满 50 回合 | 4,834 |
| `stage2` | 网关的关卡 2：7v7，每人 40 个混合触发的被动，每个每回合最多 3 次，打满 30 回合，没有人会死 | 48,288 |
| `stage3` | 网关的关卡 3：7v7，每人 20 个命中或受击触发的伤害被动，每个每回合最多 3 次，打满 30 回合 | 110,896 |

环境：4 vCPU（Intel Xeon 2.8 GHz）云容器，Erlang/OTP 25（JIT），C++ 用 GCC 13 `-O3` 编译。绝对数值只适用于这台机器；更有参考价值的是三者之间的比例。

## 结果

### 紧凑结果（`report => actions`）

单个调用方（顺序执行）：

| 场景 | NIF | Port | 纯 Erlang | Erlang ÷ 最快的 C++ |
|---|---:|---:|---:|---:|
| example | **0.17 ms** | 0.23 ms | 0.54 ms | 3.1 |
| random_mix | **0.48 ms** | 0.63 ms | 0.86 ms | 1.8 |
| long | 2.58 ms | **2.57 ms** | 14.0 ms | 5.4 |
| stage2 | **35 ms** | 38 ms | 255 ms | 7.3 |
| stage3 | 63 ms | **41 ms** | 595 ms | 14.5 |

四个调用方并发，每秒场数：

| 场景 | NIF | Port（1 个 worker） | Port（4 个 worker） | 纯 Erlang | 最快的 C++ ÷ Erlang |
|---|---:|---:|---:|---:|---:|
| example | **16,604** | 4,302 | 11,942 | 5,225 | 3.2 |
| random_mix | **5,648** | 1,678 | 4,547 | 3,858 | 1.5 |
| long | 751 | 302 | **1,101** | 198 | 5.6 |
| stage2 | **96** | 30 | 89 | 13 | 7.4 |
| stage3 | 51 | 23 | **55** | 6 | 9.2 |

- 战斗越大，差距越大：每次调用都有固定开销（编解码请求、切换到 dirty 调度器或 Port），短战斗摊不掉。
- `random_mix` 提升最少。它的请求里内联定义了 Buff 和效果，解码和解析请求（160 µs）比模拟战斗本身（110 µs）还贵。用配置包（`skill_ids`、`passive_ids`）的请求要小得多。
- 只要摘要（`report => summary`）时，C++ 快 3–10 倍：关卡 2 四个 Port worker 每秒 115 场，纯 Erlang 18 场；关卡 3 是 70 场对 7 场。

### 完整结果（不带 `report`），作为对照

也就是以前的结果格式，每个事件都是一个 Erlang map：

| 场景 | NIF | Port | 纯 Erlang | 4 个调用方：NIF / Port（4 个 worker）/ Erlang |
|---|---:|---:|---:|---|
| example | 1.13 ms | 1.25 ms | **0.45 ms** | 每秒 1,730 / 1,752 / **6,882** |
| random_mix | 1.32 ms | 1.63 ms | **0.79 ms** | 1,795 / 1,319 / **4,718** |
| long | 25.0 ms | 28.3 ms | **9.4 ms** | 88 / 84 / **349** |
| stage2 | 272 ms | 322 ms | **260 ms** | 8 / 7 / **17** |
| stage3 | 567 ms | 667 ms | **459 ms** | 4 / 4 / **8** |

在这种格式下纯 Erlang 处处领先；紧凑结果让 C++ 比原来快了 5–16 倍。这里纯 Erlang 的耗时还不含客户端战报，用 gpb 编码战报还要再花差不多一场战斗的时间；上面紧凑结果的表已经包含了写战报的开销，用的是更快的 `gamebattle_report`。

## 时间花在哪

单位 µs，一场战斗。C++ 各阶段在 C++ 内直接测量；Erlang 各阶段单独调用每一步测量。

| 阶段 | example | long | stage2 | stage3 |
|---|---:|---:|---:|---:|
| **C++**：ETF 解码请求 | 23 | 92 | 767 | 290 |
| C++：解析请求 | 16 | 44 | 420 | 163 |
| C++：模拟 | **63** | **1,331** | **18,173** | **35,312** |
| C++：写 `actions` 战报 | 18 | 404 | 3,513 | 5,066 |
| C++：写 `events` 战报 | 20 | 475 | 5,857 | 16,608 |
| C++：把紧凑结果（摘要 + `actions` 战报）编码成 ETF | 27 | 390 | 3,877 | 4,744 |
| C++，以前：把完整结果编码成 ETF | 256 | 9,325 | 109,733 | 249,552 |
| **纯 Erlang**：模拟 | 322 | 9,017 | 198,211 | — |
| 纯 Erlang：写 `actions` 战报 | 110 | 5,383 | 53,536 | — |
| 纯 Erlang：写 `events` 战报 | 203 | 4,488 | 130,154 | — |
| 对照：Erlang 用 gpb 编码 `events` 战报 | 481 | 10,893 | 225,337 | — |

- **只看模拟：C++ 比 Erlang 快 5–11 倍。** C++ 每个事件约 0.3–0.4 µs，Erlang 约 1.4–4 µs。profile 里没有单一热点，差距分散在不可变数据（每次修改单位都要复制元组）、64 位随机数的大整数运算和普遍的函数调用开销上。
- **以前，边界上的开销远超战斗本身。** 关卡 2 的 ETF 编码要 110 ms，是模拟的 6 倍，Erlang 解码这些 map 还要更久。每个事件都是 12 个键的 map，键和枚举值都是原子：解码时要为每个原子查一次原子表，再逐个构建 map。
- **C++ 写战报的开销是模拟的 15–30%**，每个事件 50–85 ns，和只把每个事件的字段打包成 varint 差不多，所以汇总本身几乎不花时间。
- **纯 Erlang 引擎用 `gamebattle_report` 写战报**，它是 C++ 战报写入逻辑的移植，写出的字节完全相同，比 gpb 快 2–2.5 倍。

## 怎么选引擎

| | 纯 Erlang | NIF | Port |
|---|---|---|---|
| 引擎代码崩溃 | 只影响一个进程 | 整个节点崩溃 | Port 进程退出，监督树重启 |
| 死循环、超长战斗 | 可抢占，不影响其他进程 | 占住一个 dirty 调度器，无法从 Erlang 侧杀掉 | 超时后关闭 Port |
| 并发 | 随调度器数扩展 | 受 dirty 调度器数限制；占满所有核时会拖慢其他进程 | 需要 worker 池 |
| 部署 | 只有 beam 文件，可热更新 | 需按 OTP 主版本编译 | 需要每个平台的可执行文件 |
| 维护 | 一份代码 | 两份引擎都要维护，并且必须保持一致 | 同 NIF |

如果两套引擎都保留（例如开发期用 Erlang，线上用 C++），修改规则时两边要同步改。`erlang/test/gamebattle_erl_tests.erl` 的差异测试会逐场比对两边的结果（包括战报字节），任何不一致都会被它发现。

**建议：**

1. 短战斗、每秒几千场以内：直接用 `gamebattle:simulate(erlang, Request)`。
2. 长战斗、大量被动连锁或高并发：用 C++，并且始终带上 `report` 选项。单次调用 NIF 最快。Port worker 不会因引擎崩溃拖垮节点，并发能力随 worker 数增长，池的大小按分给战斗的核数来定。
3. 发给客户端的战报用 `actions`。
4. 无论选哪种，先在生产机型上用下面的命令重跑一遍，用真实战斗数据替换示例场景。

## 多场并发

关卡 2、3 的大战斗，4 核机器上同时打 1–16 场，使用紧凑结果（`report => actions`）。“其他进程延迟”是一个每 1 ms 醒一次的探针进程测到的额外延迟，代表同一节点上的玩法逻辑会被拖慢多少。

| 场景 | 方式 | 单场 | 4 核饱和吞吐 | 其他进程延迟 p99 |
|---|---|---:|---:|---:|
| stage2（48,000 个事件） | 纯 Erlang | 264 ms | 15 场/秒 | ≤ 6 ms |
| | NIF | 34 ms | 99 场/秒 | 同时 4 场及以上时 29–31 ms |
| | NIF，`+SDcpu 3:3` | 37 ms | 77 场/秒 | ≤ 2.3 ms |
| | Port（每场一个 worker） | 38 ms | 102 场/秒 | 8 个 worker 以内 ≤ 9 ms |
| stage3（110,000 个事件） | 纯 Erlang | 561 ms | 7 场/秒 | ≤ 6 ms |
| | NIF | 65 ms | 58 场/秒 | 同时 4 场及以上时 44–51 ms |
| | NIF，`+SDcpu 3:3` | 62 ms | 46 场/秒 | ≤ 1.6 ms |
| | Port（每场一个 worker） | 62 ms | 62 场/秒 | 8 个 worker 以内 ≤ 8 ms |

- 用完整结果时，NIF 会让其他进程卡上 210 ms，Port 50–120 ms；改用紧凑结果后降到上表的水平。
- NIF 剩下的延迟来自 CPU 争用：4 个 dirty 调度器占满 4 个核，普通调度器分不到足够的 CPU。少开一个 dirty 调度器（`+SDcpu 3:3`），延迟就能保持在 2 ms 左右。
- 超过核数后，吞吐不再增长，所有战斗一起变慢。用固定大小的战斗池排队，比不限并发的平均耗时更低。
- 纯 Erlang 引擎每场运行中的大战斗占 10–18 MB 内存，主要是事件日志。

经网关的压测结果、每秒 100 场需要多少核，见 [client/README.md](../client/README.md)。

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
# 完整结果
erl -noshell -pa _build/test/lib/gamebattle/ebin _build/test/lib/gamebattle/test \
    -eval 'gamebattle_bench:run(), halt().'
# 紧凑结果，加上两个压力关卡
erl -noshell -pa _build/test/lib/gamebattle/ebin _build/test/lib/gamebattle/test \
    -eval 'gamebattle_bench:run(#{report => actions, scenarios => [example, random_mix, long, stage2, stage3]}), halt().'
```

差异测试（需要 C++ Port）：

```bash
cd erlang
GAMEBATTLE_PORT=priv/gamebattle_port \
GAMEBATTLE_TEST_CONFIG=../out/build/bench/generated-config/example.gbcfg \
  rebar3 eunit --module=gamebattle_erl_tests
```

`gamebattle_erl_tests:differential(1, 20000)` 可以跑任意数量的随机用例（先启动 `gamebattle` 应用）。它返回结果不一致的种子列表，为空表示全部一致。
