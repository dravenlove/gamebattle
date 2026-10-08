# 实战代码

这些程序**直接链接真实的战斗引擎**（`gamebattle_core`），不修改引擎本身的任何代码。它们配合第 17~22 课使用，也是简历上"项目经验"的素材（第 24 课）。

| 程序 | 课 | 做什么 |
|---|---|---|
| `battle_thread_pool` | 17 | 线程池并发跑 2000 场战斗，验证结果与单线程逐字节一致 |
| `battle_tcp_server` | 19 | epoll + 线程池的 TCP 战斗服务器，`{packet, 4}` 帧格式，Erlang 可用 `gen_tcp` 直连 |
| `battle_bench` | 21 | 测量完整链路各阶段耗时（解码 / 解析 / 模拟 / 编码） |
| `encode_bench` | 21 | 三种结果编码方式的对比实验，先验证输出逐字节相同再计时 |
| `term_fuzz` | 22 | ETF 入口的模糊测试（兼容 libFuzzer 的入口 + 自带变异驱动） |
| `known_issues` | 22 | 复现课程中发现的引擎问题，并报告每个问题是否已修复（修复后的回归检查） |
| `ds_rank_test` / `ds_timers` / `ds_aoi` / `ds_consistent_hash` / `ds_aos_soa` | 20 | 跳表排行榜、时间轮、九宫格 AOI、一致性哈希、AoS/SoA，都带正确性校验 |

辅助文件：`sample_battle.hpp`（5v5 示例战斗）、`request_codec.hpp`（`BattleRequest` → ETF）、`direct_encode.hpp`（流式结果编码器）、`thread_pool.hpp`、`result_hash.hpp`、`tcp_client.py`（TCP 服务器的测试客户端）、`ds/ranklist.hpp`（跳表）。

## 构建

```bash
cd lessons/practice
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

./build/battle_thread_pool 2000
./build/battle_bench 3000
./build/encode_bench 1000
```

带 sanitizer 的构建（第 18、22 课）：

```bash
# 内存错误 + 未定义行为
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
cmake --build build-asan --target term_fuzz && ./build-asan/term_fuzz 200000 1

# 数据竞争
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build-tsan --target battle_thread_pool && ./build-tsan/battle_thread_pool 200
```

TCP 服务器（仅 Linux）：

```bash
./build/battle_bench --dump-request sample_request.etf
./build/battle_tcp_server 9000 4 &
python3 tcp_client.py 9000 sample_request.etf
kill -TERM %1
```
