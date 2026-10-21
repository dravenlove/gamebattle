# Practice code

[中文](README.md) | **English**

These programs **link directly against the real battle engine** (`gamebattle_core`) without modifying any of the engine's own code. They accompany lessons 17–22 and double as "project experience" material for your résumé (lesson 24).

| Program | Lesson | What it does |
|---|---|---|
| `battle_thread_pool` | 17 | Runs 2000 battles concurrently on a thread pool and verifies the results are byte-identical to single-threaded runs |
| `battle_tcp_server` | 19 | An epoll + thread pool TCP battle server with `{packet, 4}` framing; Erlang can connect directly with `gen_tcp` |
| `battle_bench` | 21 | Measures the time of each stage of the full pipeline (decode / parse / simulate / encode) |
| `encode_bench` | 21 | Compares three ways of encoding results, verifying the outputs are byte-identical before timing |
| `term_fuzz` | 22 | Fuzz testing for the ETF entry point (a libFuzzer-compatible entry function + a built-in mutation driver) |
| `known_issues` | 22 | Reproduces the engine problems found during the course and reports whether each one has been fixed (a regression check once fixed) |
| `ds_rank_test` / `ds_timers` / `ds_aoi` / `ds_consistent_hash` / `ds_aos_soa` | 20 | Skiplist leaderboard, timing wheel, grid AOI, consistent hashing, AoS/SoA, all with correctness checks |

Helper files: `sample_battle.hpp` (a sample 5v5 battle), `request_codec.hpp` (`BattleRequest` → ETF), `direct_encode.hpp` (a streaming result encoder), `thread_pool.hpp`, `result_hash.hpp`, `tcp_client.py` (a test client for the TCP server), `ds/ranklist.hpp` (the skiplist).

## Building

```bash
cd lessons/practice
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

./build/battle_thread_pool 2000
./build/battle_bench 3000
./build/encode_bench 1000
```

Builds with sanitizers (lessons 18, 22):

```bash
# memory errors + undefined behavior
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
cmake --build build-asan --target term_fuzz && ./build-asan/term_fuzz 200000 1

# data races
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build-tsan --target battle_thread_pool && ./build-tsan/battle_thread_pool 200
```

The TCP server (Linux only):

```bash
./build/battle_bench --dump-request sample_request.etf
./build/battle_tcp_server 9000 4 &
python3 tcp_client.py 9000 sample_request.etf
kill -TERM %1
```
