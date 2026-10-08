# Battle engine performance: NIF, Port and plain Erlang

[中文](engine-benchmark.md) | **English**

## Summary

- **With compact results, C++ is about 3 times faster than plain Erlang for short battles and 7–9 times for large ones.** With four concurrent callers on 4 cores, the large 7v7 passive battles run 96 a second through the NIF and 89 through four Port workers (48,000 events each), against 13 in plain Erlang: about 7 times as many. With 110,000 events it is 55 against 6, about 9 times. Short battles gain about 3 times.
- **A compact result is a request with the `report` option** (`summary`, `actions` or `events`). C++ then returns only the summary the server needs (winner, end reason, every unit's HP) plus the client's `BattleReport`, already encoded as protobuf bytes. Before, it returned every event as an Erlang map. See [Compact results](../README.en.md#compact-results).
- **Why it matters:** the C++ simulation was always fast, but handing its result to Erlang was not. For a battle with 48,000 events, C++ simulates in 18 ms, then spent 110 ms encoding the events as ETF; Erlang decoded them for longer still. The compact result skips all of that: writing the protobuf report takes C++ 3.5 ms.
- **Reports per step (`actions`) are what clients should get.** They add up the events of each action: 240 KB instead of 1.5 MB for the 48,000-event battle, and 125 KB instead of 3.8 MB for the 110,000-event one.
- **Recommendation:**
  - For short battles at moderate volume, plain Erlang is enough. It is the simplest, cannot take the node down, and can be hot-upgraded.
  - For long battles, heavy passive chains or high volume, use C++ with the `report` option: NIF or a pool of Port workers.
  - On a node that also runs game logic, keep a core free for it, since the NIF on every core delays other processes by 30–50 ms. Either start it with one dirty CPU scheduler fewer than cores (`+SDcpu 3:3` on 4 cores, which costs about a quarter of the throughput), or use Port workers.

## What was measured

The same engine, run three ways:

| Adapter | What it is |
|---|---|
| `nif` | C++ inside the BEAM, on a dirty CPU scheduler |
| `port` | C++ in its own OS process, over a `{packet, 4}` pipe |
| `erlang` | `gamebattle_erl`: a line-by-line port of the engine to plain Erlang, running in the calling process |

All three return identical results, compact ones included: the benchmark checks this for every request before timing. In addition, the differential tests compare the plain-Erlang engine with the C++ Port case by case. Every one of these matched, results and error messages alike:

- 20,000 random valid requests, covering every event type and end reason, chains, negates and fizzles included;
- 15,000 random requests with the `report` option, 5,000 for each level, whose report bytes are identical too;
- 7,000 random invalid requests, among them invalid `report` values;
- 5,000 requests that use a config pack, 2,000 of them with a report;
- 3,000 corrupted config packs.

Requests without `report` still get exactly the bytes they got before compact results existed: 4,001 requests were compared with the previous build of the Port.

Each timed call includes everything the caller pays: encoding the request, the simulation and decoding the result. With `report`, the result includes the encoded client report.

Scenarios:

| Scenario | Contents | Events on average |
|---|---|---:|
| `example` | `gamebattle:example_request()`: two heroes a side, plus support units (beauty, pet, artifact) | 226 |
| `random_mix` | 200 random requests, rich in buffs, reactions and chains | 201 |
| `long` | Six heroes a side with area skills, stacking damage over time and healing passives, running all 50 rounds | 4,834 |
| `stage2` | The gateway's stage 2: 7v7, 40 passives per hero on mixed triggers, at most 3 times a round each, all 30 rounds, nobody dies | 48,288 |
| `stage3` | The gateway's stage 3: 7v7, 20 damage passives per hero set off by hitting and being hit, at most 3 times a round, all 30 rounds | 110,896 |

Environment: a 4-vCPU cloud container (Intel Xeon, 2.8 GHz), Erlang/OTP 25 with the JIT, C++ built by GCC 13 with `-O3`. The absolute numbers hold only for this machine; the ratios between the three are what carries over.

## Results

### Compact results (`report => actions`)

One caller (sequential):

| Scenario | NIF | Port | Plain Erlang | Erlang ÷ fastest C++ |
|---|---:|---:|---:|---:|
| example | **0.17 ms** | 0.23 ms | 0.54 ms | 3.1 |
| random_mix | **0.48 ms** | 0.63 ms | 0.86 ms | 1.8 |
| long | 2.58 ms | **2.57 ms** | 14.0 ms | 5.4 |
| stage2 | **35 ms** | 38 ms | 255 ms | 7.3 |
| stage3 | 63 ms | **41 ms** | 595 ms | 14.5 |

Four concurrent callers, battles per second:

| Scenario | NIF | Port (1 worker) | Port (4 workers) | Plain Erlang | Fastest C++ ÷ Erlang |
|---|---:|---:|---:|---:|---:|
| example | **16,604** | 4,302 | 11,942 | 5,225 | 3.2 |
| random_mix | **5,648** | 1,678 | 4,547 | 3,858 | 1.5 |
| long | 751 | 302 | **1,101** | 198 | 5.6 |
| stage2 | **96** | 30 | 89 | 13 | 7.4 |
| stage3 | 51 | 23 | **55** | 6 | 9.2 |

- The bigger the battle, the bigger the gap: a call has a fixed cost (encoding and decoding the request, switching to a dirty scheduler or the Port), which short battles cannot hide.
- `random_mix` gains least. Its requests define buffs and effects inline, so decoding and parsing the request (160 µs) costs more than simulating the battle (110 µs). Requests that use a config pack (`skill_ids`, `passive_ids`) are much smaller.
- With the summary only (`report => summary`), C++ is 3–10 times faster: stage 2 runs 115 a second through four Port workers against 18 in Erlang, stage 3 70 against 7.

### Full results (no `report` option), for comparison

The result format from before, with every event as an Erlang map:

| Scenario | NIF | Port | Plain Erlang | 4 callers: NIF / Port (4 workers) / Erlang |
|---|---:|---:|---:|---|
| example | 1.13 ms | 1.25 ms | **0.45 ms** | 1,730 / 1,752 / **6,882** a second |
| random_mix | 1.32 ms | 1.63 ms | **0.79 ms** | 1,795 / 1,319 / **4,718** |
| long | 25.0 ms | 28.3 ms | **9.4 ms** | 88 / 84 / **349** |
| stage2 | 272 ms | 322 ms | **260 ms** | 8 / 7 / **17** |
| stage3 | 567 ms | 667 ms | **459 ms** | 4 / 4 / **8** |

With this format plain Erlang wins everywhere; the compact result makes C++ 5–16 times faster than it was. These Erlang times have no client report yet. Encoding one with gpb takes about as long again as the battle; the compact tables above include writing it, with the faster `gamebattle_report`.

## Where the time goes

In µs, for one battle. The C++ stages were timed inside C++; the Erlang ones by calling each step on its own.

| Stage | example | long | stage2 | stage3 |
|---|---:|---:|---:|---:|
| **C++**: decode the request's ETF | 23 | 92 | 767 | 290 |
| C++: parse the request | 16 | 44 | 420 | 163 |
| C++: simulate | **63** | **1,331** | **18,173** | **35,312** |
| C++: write the `actions` report | 18 | 404 | 3,513 | 5,066 |
| C++: write the `events` report | 20 | 475 | 5,857 | 16,608 |
| C++: encode the compact result (summary + `actions` report) as ETF | 27 | 390 | 3,877 | 4,744 |
| C++, before: encode the full result as ETF | 256 | 9,325 | 109,733 | 249,552 |
| **Plain Erlang**: simulate | 322 | 9,017 | 198,211 | — |
| Plain Erlang: write the `actions` report | 110 | 5,383 | 53,536 | — |
| Plain Erlang: write the `events` report | 203 | 4,488 | 130,154 | — |
| For comparison: Erlang encoding the `events` report with gpb | 481 | 10,893 | 225,337 | — |

- **Simulation alone: C++ is 5–11 times faster than Erlang.** C++ takes about 0.3–0.4 µs per event, Erlang 1.4–4 µs. The profile has no single hot spot: the gap is spread over immutable data (every change to a unit copies a tuple), bignum arithmetic for the 64-bit random numbers, and function-call overhead in general.
- **Before, the boundary cost far more than the battle.** For stage 2, ETF encoding took 110 ms, six times the simulation, and Erlang then spent longer still decoding the maps. Every event was a 12-key map whose keys and enum values are atoms: decoding looks each atom up in the atom table and then builds the maps one by one.
- **Writing the report costs C++ 15–30% of the simulation**, 50–85 ns per event. Packing each event's fields into varints and nothing else takes about as long, so adding the events up costs little.
- **The plain-Erlang engine writes its reports with `gamebattle_report`**, a port of the C++ report writer that produces the same bytes. It is 2–2.5 times faster than gpb.

## Choosing an engine

| | Plain Erlang | NIF | Port |
|---|---|---|---|
| A crash in engine code | Affects one process | Crashes the whole node | The Port process exits and the supervisor restarts it |
| An endless loop, a very long battle | Preemptible; other processes are unaffected | Holds a dirty scheduler, cannot be killed from Erlang | The Port is closed after the timeout |
| Concurrency | Scales with the schedulers | Limited by the dirty schedulers; delays other processes when they take every core | Needs a worker pool |
| Deployment | Just beam files; hot upgrades work | Must be built for the OTP major version | Needs an executable per platform |
| Maintenance | One code base | Two engines to maintain, which must stay identical | As for the NIF |

If both engines are kept (say, Erlang during development and C++ in production), every rule change has to be made on both sides. The differential tests in `erlang/test/gamebattle_erl_tests.erl` compare the two battle by battle, report bytes included, and catch any divergence.

**Recommendation:**

1. Short battles, up to a few thousand a second: `gamebattle:simulate(erlang, Request)` is enough.
2. Long battles, heavy passive chains or high volume: C++, always with the `report` option. The NIF is the fastest for one call. Port workers keep engine crashes out of the node and scale with the number of workers. Size the pool to the cores you give to battles.
3. Send clients `actions` reports.
4. Either way, rerun the benchmark below on production hardware, with real battle data in place of the sample scenarios.

## Many battles at once

The stage 2 and 3 battles, 1 to 16 at a time on 4 cores, with compact results (`report => actions`). "Delay to other processes" is the extra delay seen by a probe process that wakes every 1 ms: how much game logic on the same node would be held up.

| Scenario | Adapter | One battle | Saturated throughput on 4 cores | Delay to other processes, p99 |
|---|---|---:|---:|---:|
| stage2 (48,000 events) | plain Erlang | 264 ms | 15/s | ≤ 6 ms |
| | NIF | 34 ms | 99/s | 29–31 ms at 4 or more battles |
| | NIF, `+SDcpu 3:3` | 37 ms | 77/s | ≤ 2.3 ms |
| | Port (one worker per battle) | 38 ms | 102/s | ≤ 9 ms up to 8 workers |
| stage3 (110,000 events) | plain Erlang | 561 ms | 7/s | ≤ 6 ms |
| | NIF | 65 ms | 58/s | 44–51 ms at 4 or more battles |
| | NIF, `+SDcpu 3:3` | 62 ms | 46/s | ≤ 1.6 ms |
| | Port (one worker per battle) | 62 ms | 62/s | ≤ 8 ms up to 8 workers |

- With full results the NIF held up other processes for up to 210 ms and the Port for 50–120 ms; with compact results these delays come down to the figures above.
- What remains for the NIF is CPU contention: four dirty schedulers busy on four cores leave the normal schedulers too little CPU. One dirty scheduler fewer (`+SDcpu 3:3`) keeps the delay at about 2 ms.
- Beyond the number of cores throughput stops growing, and all battles slow down together. Queueing in a fixed-size battle pool gives a lower average time than unlimited concurrency.
- A large running battle takes 10–18 MB in the plain-Erlang engine, mostly the event log.

See [client/README.en.md](../client/README.en.md) for load tests through the gateway and how many cores 100 battles a second need.

## Reproducing

Build the Port and the NIF in Release mode, then run the benchmark:

```bash
cmake -S . -B out/build/bench -DCMAKE_BUILD_TYPE=Release \
  -DGAMEBATTLE_BUILD_NIF=ON -DGAMEBATTLE_BUILD_CONFIG_COMPILER=ON \
  -DERLANG_ERTS_INCLUDE_DIR="$(erl -noshell -eval 'io:format("~s/erts-~s/include", [code:root_dir(), erlang:system_info(version)]), halt().')"
cmake --build out/build/bench --parallel
cmake --install out/build/bench --prefix erlang --component BattleRuntime

cd erlang
rebar3 as test compile
# full results
erl -noshell -pa _build/test/lib/gamebattle/ebin _build/test/lib/gamebattle/test \
    -eval 'gamebattle_bench:run(), halt().'
# compact results, with the stress stages
erl -noshell -pa _build/test/lib/gamebattle/ebin _build/test/lib/gamebattle/test \
    -eval 'gamebattle_bench:run(#{report => actions, scenarios => [example, random_mix, long, stage2, stage3]}), halt().'
```

The differential tests (they need the C++ Port):

```bash
cd erlang
GAMEBATTLE_PORT=priv/gamebattle_port \
GAMEBATTLE_TEST_CONFIG=../out/build/bench/generated-config/example.gbcfg \
  rebar3 eunit --module=gamebattle_erl_tests
```

`gamebattle_erl_tests:differential(1, 20000)` runs any number of random cases (start the `gamebattle` application first). It returns the seeds whose results differ; an empty list means every case matched.
