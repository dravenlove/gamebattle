# Battle engine performance: NIF, Port and plain Erlang

[中文](engine-benchmark.md) | **English**

## Summary

- **With the current interface, switching to C++ does not pay off: it is slower.** For an Erlang caller, the plain-Erlang engine is the fastest way to run a battle: 0.30 ms for the example battle, against 1.02 ms through the NIF and 1.20 ms through the Port. With four concurrent callers, plain Erlang runs about 9,400 battles a second, the NIF about 1,700, and the Port about 800 with one worker or 1,800 with four.
- **The C++ engine itself is fast.** Counting the simulation alone, C++ is about 4–8 times faster than Erlang, and the gap grows with the length of the battle.
- **The difference comes from how the result is handed over.** C++ encodes the report as ETF, where every event is a map with 12 keys, and Erlang decodes it again. For the example battle the C++ simulation takes 64 µs, but the ETF encoding takes 323 µs and the Erlang decoding another 456 µs.
- **To make C++ worth it, change the result format first.** Return only the summary the server uses (winner, unit HP) as an Erlang term, and emit the event log directly as the protobuf bytes sent to the client. Working from the measured stage times, the example battle would then take about 0.13 ms, about 2 times faster than plain Erlang, and the long battle about 1.6 ms, about 6 times faster. Once the protobuf encoding for the client is counted, the gaps are about 5 and 13 times.
- **Recommendation:**
  - For moderate battle volume and short battles, use the plain-Erlang engine. It is the simplest, cannot take the node down, and can be hot-upgraded.
  - For high-volume PvP, long battles or heavy recomputation, use C++, but change the result format first.

## What was measured

The same engine, run three ways:

| Adapter | What it is |
|---|---|
| `nif` | C++ inside the BEAM, on a dirty CPU scheduler |
| `port` | C++ in its own OS process, over a `{packet, 4}` pipe |
| `erlang` | `gamebattle_erl`: a line-by-line port of the engine to plain Erlang, running in the calling process |

All three return identical results; the benchmark checks this for every request before timing. In addition, the differential tests compare the plain-Erlang engine with the C++ Port case by case, and every one of these matched, results and error messages alike:

- 20,000 random valid requests, covering every event type and end reason, chains, negates and fizzles included;
- 5,000 random invalid requests;
- 3,000 requests that use a config pack;
- 3,000 corrupted config packs.

Each timed call includes everything the caller pays for: encoding the request, the simulation and decoding the result.

Scenarios:

| Scenario | Contents | Events on average |
|---|---|---:|
| `example` | `gamebattle:example_request()`: two heroes a side, plus support units (beauty, pet, artifact) | 226 |
| `random_mix` | 200 random requests, rich in buffs, reactions and chains | 201 |
| `long` | Six heroes a side with area skills, stacking damage over time and healing passives, running all 50 rounds | 4834 |

Environment: a 4-vCPU cloud container (Intel Xeon, 2.8 GHz), Erlang/OTP 25 with the JIT, C++ built by GCC 13 with `-O3`. The absolute numbers hold only for this machine; the ratios between the three are what carries over.

## Results

### One caller (sequential)

| Scenario | Adapter | Mean | p50 | p99 | Per event |
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

### Four concurrent callers (battles per second)

| Scenario | nif | port (1 worker) | port (4 workers) | erlang |
|---|---:|---:|---:|---:|
| example | 1685 | 802 | 1805 | **9404** |
| random_mix | 1729 | 703 | 1470 | **5434** |
| long | 80 | 31 | 88 | **447** |

A second full run stayed within 15% of these numbers.

- The plain-Erlang engine scales with the schedulers: four callers get about 2.8 times the throughput of one.
- A Port worker runs one battle at a time, which is deliberate backpressure; concurrency needs several workers.
- The NIF gains only about 1.7 times under concurrency, possibly because decoding and garbage-collecting large results compete with the dirty schedulers for the same four cores.

## Where the time goes

In µs. The C++ stages were timed inside C++; the Erlang stages are medians.

| Stage | example | random_mix | long |
|---|---:|---:|---:|
| **Plain Erlang**: parse and validate | 29 | 155 | 205 |
| Plain Erlang: simulate (including building the result maps) | 245 | 550 | 9192 |
| **C++**: decode the request's ETF | 24 | 83 | 77 |
| C++: parse the request | 16 | 54 | 36 |
| C++: simulate | **64** | **94** | **1152** |
| C++: encode the result as ETF | 323 | 297 | 8650 |
| Erlang: `term_to_binary(Request)` | 10 | 53 | 29 |
| Erlang: `binary_to_term(Result)` | 456 | 629 | 9360 |
| For comparison: C++ packing the events compactly (protobuf-like) | 16 | 15 | 339 |
| For comparison: Erlang encoding the result as protobuf with gpb | 398 | 691 | 12593 |

- **Simulation alone: C++ is 3.8 times (example), 5.8 times (random_mix) and 8 times (long) faster than Erlang.** C++ takes about 0.24–0.47 µs per event, Erlang about 1.1–2.7 µs. The profile has no single hot spot: the gap is spread over immutable data (every change to a unit copies a tuple), bignum arithmetic for the 64-bit random numbers, and function-call overhead in general.
- **On the C++ path most of the time goes to the boundary.** For the example battle, ETF encoding (323 µs) plus Erlang decoding (456 µs) cost about 12 times the simulation itself (64 µs). This is mainly because every event is a 12-key map whose keys and enum values are atoms: decoding looks each atom up in the atom table and then builds the maps one by one.
- **The plain-Erlang engine has no such boundary:** it builds the result maps in the calling process, with literal atoms as keys.

## Is switching to C++ worth it?

**With the current interface: no.** In all three scenarios an Erlang caller gets its result map 2–3.5 times sooner from the plain-Erlang engine than from the NIF, and 2.5–4 times sooner than from the Port.

**After changing the result format: yes, especially for long battles.** Server logic only reads the winner, the end reason and each unit's HP (settlement, gauntlet carry-over); the event log usually goes to the client as is. If C++ returns a small summary term plus the already encoded protobuf `BattleReport` bytes, the ETF encoding, the Erlang decoding and the gpb encoding all disappear. Working from the measured stage times above:

| | example | long |
|---|---:|---:|
| Plain Erlang: get the result (median) | 0.27 ms | 9.4 ms |
| Plain Erlang: get the result and encode it as client protobuf | 0.67 ms | 22 ms |
| C++ returning a compact result (estimate) | about 0.13 ms | about 1.6 ms |

The estimate is the sum of request encoding, C++ decoding, parsing, simulation and compact encoding. It leaves out the Port's pipe transfer (tens of µs).

**Before switching, also weigh:**

| | Plain Erlang | NIF | Port |
|---|---|---|---|
| A crash in engine code | Affects one process | Crashes the whole node | The Port process exits and the supervisor restarts it |
| An endless loop, a very long battle | Preemptible; other processes are unaffected | Holds a dirty scheduler, cannot be killed from Erlang | The Port is closed after the timeout |
| Concurrency | Scales with the schedulers | Limited by the dirty schedulers | Needs a worker pool |
| Deployment | Just beam files; hot upgrades work | Must be built for the OTP major version | Needs an executable per platform |
| Maintenance | One code base | Two engines to maintain, which must stay identical | As for the NIF |

If both engines are kept (say, Erlang during development and C++ in production), every rule change has to be made on both sides. The differential tests in `erlang/test/gamebattle_erl_tests.erl` compare the two battle by battle and catch any divergence.

**Recommendation:**

1. Up to about 3,000 example-sized battles per second per core, mostly short ones: use `gamebattle:simulate(erlang, Request)` directly.
2. For more throughput or many long battles: keep C++, but first change its result to "summary term + protobuf event bytes", then use a pool of Port workers or the NIF.
3. Either way, rerun the benchmark below on production hardware, with real battle data in place of the sample scenarios.

## Many battles at once

Large 7v7 battles with dozens of passives per unit and per-round limits, 1 to 16 at a time on 4 cores. "Delay to other processes" is the extra delay seen by a probe process that wakes every 1 ms: how much game logic would be held up.

| Scenario | Adapter | One battle | Saturated throughput on 4 cores | Delay to other processes, p99 |
|---|---|---:|---:|---:|
| 40 mixed passives each, 3 a round (about 48,000 events) | plain Erlang | 178 ms | 21/s | ≤ 5 ms |
| | current NIF | 283 ms | 8.6/s | about 210 ms |
| | current Port (4 workers) | 313 ms | 12/s | 50–120 ms |
| | C++ with a compact result (no transport) | 22 ms | 168/s | — |
| 20 chaining passives each, 3 a round (about 110,000 events) | plain Erlang | 413 ms | 9/s | ≤ 5 ms |
| | current NIF | 640 ms | 3.6/s | 2–215 ms |
| | current Port (4 workers) | 665 ms | 4.8/s | 36–88 ms |
| | C++ with a compact result (no transport) | 40 ms | 91/s | — |

- Beyond the number of cores throughput stops growing, and in plain Erlang all battles slow down together: with 16 at once each takes about 4 times as long as alone. Queueing in a fixed-size battle pool gives a lower average time than unlimited concurrency.
- With the current ETF result format, the NIF and the Port hold up other processes on the node by hundreds of milliseconds under load; plain Erlang's preemptive scheduling does not.
- A large running battle takes 10–18 MB in the plain-Erlang engine, mostly the event log.

See [client/README.en.md](../client/README.en.md) for how many cores 100 battles a second need and for measured load tests.

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
erl -noshell -pa _build/test/lib/gamebattle/ebin _build/test/lib/gamebattle/test \
    -eval 'gamebattle_bench:run(), halt().'
```

The differential tests (they need the C++ Port):

```bash
cd erlang
GAMEBATTLE_PORT=priv/gamebattle_port \
GAMEBATTLE_TEST_CONFIG=../out/build/bench/generated-config/example.gbcfg \
  rebar3 eunit --module=gamebattle_erl_tests
```

`gamebattle_erl_tests:differential(1, 20000)` runs any number of random cases (start the `gamebattle` application first). It returns the seeds whose results differ; an empty list means every case matched.
