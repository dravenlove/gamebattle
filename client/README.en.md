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
| `GAMEBATTLE_ENGINE` | `erlang` | Engine to use: `erlang`, `nif` or `port` (`port` runs one battle at a time) |
| `GAMEBATTLE_BATTLE_WORKERS` | number of schedulers (usually the cores) | Battles at a time |
| `GAMEBATTLE_BATTLE_QUEUE` | 8 × battles at a time | Battles that may wait; more are rejected as busy |

The server logs its throughput every 10 seconds, for example `battles: 16.5/s done, 83.5/s rejected as busy, 4 running, 32 queued`.

## Stages

Stages and lineups are demo data, defined in [`erlang/src/gamebattle_demo.erl`](../erlang/src/gamebattle_demo.erl). Add your own units and passive setups there in the same way, then test them with the client.

| Stage | Contents | One battle (measured on 4 cores) |
|---:|---|---|
| 1 | A normal battle: your lineup against five heroes; one area skill and three passives each | about 1,400 events, 6 ms |
| 2 | Mixed-passive stress: 7v7, 40 passives per hero over 7 triggers, each at most 3 times a round, all 30 rounds, nobody dies | about 48,000 events, about 180 ms |
| 3 | Chain stress: 7v7, 20 damage passives per hero set off by being hit or hitting, each at most 3 times a round, all 30 rounds, nobody dies | about 110,000 events, about 450 ms |

"Nobody dies" stands in for the worst case where revives keep everyone in the fight. Lineups pick heroes 1–7 as `hero:position`, for example `--lineup 1:1,2:2,3:3`.

## One battle: `play`

```bash
python battle_client.py play --stage 1
```

It prints the result, the round trip time, the report size, every unit's HP, the first events and a count of events by type.

| Option | Meaning |
|---|---|
| `--stage N` | Stage, default 1 |
| `--lineup 1:1,2:2` | Lineup; by default heroes 1–5 for stage 1 and 1–7 for stages 2 and 3 |
| `--events N` | Print the first N events, `0` for all, default 40 |
| `--summary-only` | No event log, just the result |
| `--json FILE` | Write the full report as JSON |
| `--host`, `--port` | Server address, default `127.0.0.1:7000`; put them before the subcommand |

## Load test: `load`

```bash
python battle_client.py load --stage 2 --rate 100 --duration 30 --summary-only
```

The client starts battles at a fixed rate without waiting for earlier ones, like many players starting battles at once: a slower server doesn't mean fewer requests. It prints a line of progress every second, then a summary and a verdict.

| Option | Meaning |
|---|---|
| `--rate N` | Battles started per second, default 100 |
| `--duration N` | Seconds to run, default 30 |
| `--connections N` | TCP connections to use, default 8 |
| `--summary-only` | No event log. Use it to measure battle throughput alone; leave it out to include report bandwidth |
| `--drain N` | Seconds to keep waiting for replies after sending stops, default 60 |

How to read the results:

- "Busy" (繁忙) rejections mean the server's queue was full, so the rate is above what the server can handle.
- It held up only if there were no busy rejections, every battle finished, and p99 is acceptable.
- Adjust `--rate` until busy rejections just reach 0: that rate is this machine's capacity for that stage.
- A few busy rejections far below capacity are usually bursts: requests bunched up after a short pause and overflowed the queue. Size the queue as "acceptable waiting time × battles per second" (`GAMEBATTLE_BATTLE_QUEUE`).

## Measured: 4 cores, plain-Erlang engine

| Stage | Report | Target rate | Finished | p99 of finished battles | Busy rejections |
|---:|---|---:|---:|---:|---:|
| 1 | full | 100/s | 99.6/s | 12 ms | 0 |
| 2 | summary only | 100/s | 17.1/s | 2.4 s | 81% |
| 2 | summary only | 15/s | 14.9/s | 331 ms | 0 |
| 3 | summary only | 100/s | 6.4/s | 6.0 s | 92% |
| 3 | summary only | 5/s | 4.9/s | 665 ms | 0 |
| 2 | full | 10/s | 7.2/s | 4.9 s | 5% |

- A full stage-2 report is about 1.4 MB. Building such a report and encoding it as protobuf cuts throughput from about 17 to about 7 battles a second.
- Above capacity, queued battles wait for the ones ahead, so the p99 of those that finish reaches seconds. Requests beyond the queue are rejected at once instead of waiting forever.

## How many cores 100 battles a second need

Estimated from the per-battle CPU cost measured above, keeping CPU use at 70% for headroom:

| Stage | Plain Erlang (summary only) | Plain Erlang (full report) | C++ with a compact result (estimate) | Report traffic per second (full report) |
|---:|---:|---:|---:|---:|
| 1 | about 1 core (0.4 measured) | about 1 core (0.7 measured) | under 1 core | about 3 MB/s |
| 2 | about 34 cores | about 80 cores | about 3.5 cores | about 140 MB/s |
| 3 | about 90 cores | — | about 6.5 cores | about 300 MB/s (estimate) |

- The C++ column comes from measuring C++ on several threads: 168 battles a second at stage-2 size and 91 at stage-3 size, results already packed compactly. It needs the C++ result format changed first; see [docs/engine-benchmark.en.md](../docs/engine-benchmark.en.md).
- Normal battles like stage 1 are no problem: at 100 a second the server used 0.7 cores on average (full reports), or 0.4 (summary only).
- With passive chains as large as stages 2 and 3, 100 battles a second need tens to a hundred cores in plain Erlang, or C++.
- Whichever engine you choose, full reports cannot go to clients as they are: at 100 battles a second they come to more than 1 Gbps.

## Notes

- The test gateway is for verification and load tests only: its stages and lineups are demo data and it has no authentication. Don't expose it to the internet or enable it on production servers.
- The load-test client needs CPU too. Run client and server on different machines when you can; if the client machine's CPU is saturated, the results come out too low.
- The numbers above come from a 4-vCPU cloud container. Rerun the same commands on your own machines.
