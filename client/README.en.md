# Battle test client

[中文](README.md) | **English**

`battle_client.py` connects to the battle service over the real client protocol ([`proto/battle_client.proto`](../proto/battle_client.proto)). It does two things: run one battle and show its report, or send battles at a fixed rate to check whether the battle system holds up under your load.

The server side is the test gateway in the Erlang application ([`erlang/src/gamebattle_gateway.erl`](../erlang/src/gamebattle_gateway.erl)). It hands requests to a fixed-size battle pool: at most N battles run at a time, more wait in a queue, and once the queue is full the gateway answers `ERROR_CODE_RETRY_LATER`.

The client's output is in Chinese.

## Setup

1. Python 3.9 or newer:

   ```bash
   pip install -r requirements.txt
   ```

   On its first run the client generates code from the `.proto` into `generated/`, and regenerates it whenever the `.proto` changes.

2. Start the battle service with the test gateway, either way:

   ```bash
   # Docker (the image has both the Port and the NIF)
   docker build -t gamebattle .
   docker run --rm -p 7000:7000 -e ERLANG_COOKIE=test -e GAMEBATTLE_GATEWAY_PORT=7000 gamebattle

   # Locally (build the Port first, as in the README)
   cd erlang && rebar3 compile
   GAMEBATTLE_GATEWAY_PORT=7000 erl -noshell -pa _build/default/lib/gamebattle/ebin \
     -eval 'application:ensure_all_started(gamebattle), timer:sleep(infinity).'
   ```

Gateway settings (the application env wins over the OS environment):

| Environment variable | Default | Meaning |
|---|---|---|
| `GAMEBATTLE_GATEWAY_PORT` | off | TCP port the gateway listens on |
| `GAMEBATTLE_ENGINE` | `erlang` | Engine to use: `erlang`, `nif` or `port` (`port` runs one battle at a time). For stages 2 and 3 use `nif`: it is 7–10 times faster |
| `GAMEBATTLE_BATTLE_WORKERS` | number of schedulers (usually the cores) | Battles at a time |
| `GAMEBATTLE_BATTLE_QUEUE` | 8 × battles at a time | Battles that may wait; more are rejected as busy |

The server logs its throughput every 10 seconds, for example `battles: 16.5/s done, 83.5/s rejected as busy, 4 running, 32 queued`.

## Stages

Stages and lineups are demo data, defined in [`erlang/src/gamebattle_demo.erl`](../erlang/src/gamebattle_demo.erl). Add your own units and passive setups there in the same way, then test them with the client.

| Stage | Contents | One battle (measured on 4 cores): plain Erlang / C++ |
|---:|---|---|
| 1 | A normal battle: your lineup against five heroes; one area skill and three passives each | about 1,400 events, 6 ms / 2 ms |
| 2 | Mixed-passive stress: 7v7, 40 passives per hero over 7 triggers, each at most 3 times a round, all 30 rounds, nobody dies | about 48,000 events, about 190 ms / 35 ms |
| 3 | Chain stress: 7v7, 20 damage passives per hero set off by being hit or hitting, each at most 3 times a round, all 30 rounds, nobody dies | about 110,000 events, about 450 ms / 40–60 ms |

"Nobody dies" stands in for the worst case where revives keep everyone in the fight. Lineups pick heroes 1–7 as `hero:position`, for example `--lineup 1:1,2:2,3:3`.

## One battle: `play`

```bash
python battle_client.py play --stage 1
```

It prints the result, the round trip time, the report size, every unit's HP and the first steps of the battle, each with its skill, what every affected unit went through and the passives and buffs it set off:

```text
     3  第1回合   先手方    2004（防守方）技能 504
          → 1001 受到 110（1 击），HP 3,013
          → 1002 受到 80（1 击），HP 3,720
          ...
          效果：触发被动 701@2004 ×2  [12 个事件]
```

| Option | Meaning |
|---|---|
| `--stage N` | Stage, default 1 |
| `--lineup 1:1,2:2` | Lineup; by default heroes 1–5 for stage 1 and 1–7 for stages 2 and 3 |
| `--detail LEVEL` | How much of the battle the report carries: `summary` (the result only), `actions` (the battle step by step, the default) or `events` (every event, megabytes for stages 2 and 3) |
| `--show N` | Print the first N steps or events, `0` for all, default 40 (`--events` also works) |
| `--json FILE` | Write the whole report as JSON |
| `--host`, `--port` | Server address, default `127.0.0.1:7000`; put them before the subcommand |

## Load test: `load`

```bash
python battle_client.py load --stage 2 --rate 100 --duration 30
```

The client starts battles at a fixed rate without waiting for earlier ones, like many players starting battles at once: a slower server doesn't mean fewer requests. It prints a line of progress every second, then a summary and a verdict.

| Option | Meaning |
|---|---|
| `--rate N` | Battles started per second, default 100 |
| `--duration N` | Seconds to run, default 30 |
| `--connections N` | TCP connections to use, default 8 |
| `--detail LEVEL` | As for `play`, default `actions`. `summary` measures the battles alone, without the report |
| `--drain N` | Seconds to keep waiting for replies after sending stops, default 60 |

How to read the results:

- "Busy" (繁忙) rejections mean the server's queue was full, so the rate is above what the server can handle.
- It held up only if there were no busy rejections, every battle finished, and p99 is acceptable.
- Adjust `--rate` until busy rejections just reach 0: that rate is this machine's capacity for that stage.
- A few busy rejections far below capacity are usually bursts: requests bunched up after a short pause and overflowed the queue. Size the queue as "acceptable waiting time × battles per second" (`GAMEBATTLE_BATTLE_QUEUE`).

## Measured: 4 cores

Reports with `--detail actions`, the client on the same machine (it takes some of the CPU too):

| Stage | Engine | Target rate | Finished | p99 of finished battles | Busy rejections | Report traffic |
|---:|---|---:|---:|---:|---:|---:|
| 1 | `nif` | 100/s | 99.8/s | 6 ms | 0 | 1.0 MB/s |
| 1 | `erlang` | 100/s | 99.8/s | 9 ms | 0 | 1.0 MB/s |
| 2 | `nif` | 80/s | 79.6/s | 87 ms | 0 | 18 MB/s |
| 2 | `nif` | 100/s | 97.2/s | 418 ms | 1% | 22 MB/s |
| 2 | `erlang` | 13/s | 12.8/s | 406 ms | 0 | 3 MB/s |
| 3 | `nif` | 55/s | 54.2/s | 374 ms | 0 | 6.5 MB/s |
| 3 | `erlang` | 5/s | 4.9/s | 707 ms | 0 | 0.6 MB/s |

- Stage 2: the NIF handles about 97 battles a second on 4 cores, the Erlang engine about 13: 7.5 times as many. Stage 3: at least 55 against 5, about 10 times.
- A report per step is about 240 KB for stage 2 and 125 KB for stage 3, against 1.5 MB and 3.8 MB with every event: 6 and 30 times less traffic. Stage 3 shrinks more because its long chains of hits on the same units add up to a few entries.
- Above capacity, queued battles wait for the ones ahead, so the p99 of those that finish grows. Requests beyond the queue are rejected at once instead of waiting forever.

## How many cores 100 battles a second need

From the throughput above, keeping CPU use at 70% for headroom. Reports per step (`actions`):

| Stage | C++ (`nif` or `port`) | Plain Erlang | Report traffic, per step | Report traffic, every event |
|---:|---:|---:|---:|---:|
| 1 | under 1 core | under 1 core | about 1 MB/s | about 2.3 MB/s |
| 2 | about 6 cores | about 44 cores | about 24 MB/s | about 150 MB/s |
| 3 | about 10 cores | about 80 cores | about 13 MB/s | about 390 MB/s |

- Normal battles like stage 1 are no problem with either engine.
- With passive chains as large as stages 2 and 3, use C++: it needs 7–8 times fewer cores. This needs reports from the engine (the `report` option, which the gateway always uses); see [docs/engine-benchmark.en.md](../docs/engine-benchmark.en.md).
- Send clients reports per step. With every event, stage 2 and 3 battles at 100 a second come to 1.2–3 Gbps.
- On a node that also runs game logic, keep a core free for it: start the node with `+SDcpu 3:3` on 4 cores (one dirty scheduler fewer), or use Port workers. A NIF on every core delays other processes by 30–50 ms; with a core kept free, by about 2 ms, at the cost of about a quarter of the throughput.

## Notes

- The test gateway is for verification and load tests only: its stages and lineups are demo data and it has no authentication. Don't expose it to the internet or enable it on production servers.
- The load-test client needs CPU too. Run client and server on different machines when you can; if the client machine's CPU is saturated, the results come out too low.
- The numbers above come from a 4-vCPU cloud container. Rerun the same commands on your own machines.
