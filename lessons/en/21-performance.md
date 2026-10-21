# Lesson 21: Performance analysis and optimization

[中文](../21-performance.md) | **English**

> The first lesson of part 5, "Performance and production". Practice code: `practice/battle_bench.cpp`, `practice/encode_bench.cpp`, `practice/direct_encode.hpp`.
>
> This lesson is a complete record of a real performance investigation I did on this project: from measuring, locating and finding the cause to running experiments that validate the fix. The process itself is project experience you can talk about in interviews (lesson 24).

## 1. Method: measure first, then act

```
1. Build a repeatable benchmark ──▶ 2. Find the most expensive part (profile) ──▶ 3. Form a hypothesis about the cause
        ▲                                                                                  │
        │                                                                                  ▼
6. Measure again, confirm the gain ◀── 5. Verify the results haven't changed (correctness) ◀── 4. Make the smallest change
```

The most common mistake is skipping the first two steps and optimizing on instinct. This project is a fine counterexample: instinct says the most expensive part should be **battle simulation** (hundreds of rounds, thousands of events, recursive triggers), but measured, it's only 15%.

**Amdahl's law**: if a part takes only 15% of the total time, then even optimizing it to zero makes the whole only 1.18× faster. So always find the real heavyweight first.

## 2. Benchmarking: time every segment of the full pipeline

`practice/battle_bench.cpp` simulates the Port's full handling of a request, timing four segments: ETF decoding → parsing into a `BattleRequest` → simulation → encoding the result. 3000 5v5 battles with different seeds, Release build (measured, two runs):

```
iterations: 3000, request bytes: 8674, avg events/battle: 879.4

stage                              us/battle     share
term::decode                        49.5~56.2   4.3~4.5%
wire::parse_request                 20.7~23.9   1.8~1.9%
Engine::simulate                   163.1~217.6  14.8~16.5%
encode_result + term::encode       868.1~1022.4 77.5~78.8%
total                             1101.5~1320.0
```

**Encoding the result to ETF takes nearly 80% of the time**, 4–5× the battle simulation itself.

The two runs' totals differ by 20%: this is a shared cloud VM, and the noise is large. So every conclusion is run at least twice, and what matters is the **proportions** and **stable ratios**, not any single run's absolute numbers.

### Pitfalls in writing benchmarks

**Pitfall 1: code deleted by the optimizer.** If a computation's result isn't used, the compiler may delete the whole computation. I fell into this twice myself in this course:

- In lesson 14, measuring "passing `shared_ptr` by `const&`", the first result was 0.2 ms for 10 million calls, 0.02 ns per call, shorter than one CPU clock cycle. GCC had worked out the function had no side effects and hoisted the call out of the loop. Only after disabling that cross-function analysis with `__attribute__((noipa))` did I get the real 10–15 ms.
- In lesson 20, measuring skiplist ranks, the first result was 3 ns, equally impossible. The sum wasn't printed, so the whole loop was deleted. Printing the result gave the real 2 µs.

**Defense**: write results into a `volatile` variable or print them; stay suspicious of numbers that don't make sense, and work out "how many nanoseconds and clock cycles is that per operation".

**Pitfall 2: measuring a Debug build.** The same benchmark, measured:

```
Debug   (-O0): simulate 1796 us, encode 27784 us, total 30693 us
Release (-O3): simulate  159 us, encode   844 us, total  1073 us
```

A **29×** difference, and the proportions change too (at `-O0` encoding is 90%). **Only measure and analyze Release builds** (use `RelWithDebInfo`, i.e. `-O2 -g`, when you need symbols).

**Pitfall 3: no warm-up.** On the first run the caches are cold and the memory allocator hasn't set up its pools yet. `battle_bench` runs 50 iterations before timing starts.

**Pitfall 4: the change is smaller than the noise.** I tried link-time optimization (LTO, `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`); two rounds of comparison came out 23% slower once and 4% faster once, entirely within the noise. **Without repeated measurement and statistics, you can't claim a change is "5% faster".** The careful way is to run several rounds, take medians, then compare.

## 3. Locating: see where the time goes with a profiler

A benchmark tells you "encoding is slow"; a profiler tells you "which line inside encoding is slow".

### valgrind callgrind

This machine has no `perf`, so I used valgrind's callgrind (it simulates and counts every instruction; it runs tens of times slower, but the results are very precise and repeatable):

```bash
cmake -S lessons/practice -B build-prof -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-prof --target battle_bench
valgrind --tool=callgrind --callgrind-out-file=cg.out ./build-prof/battle_bench 150
callgrind_annotate --inclusive=yes cg.out | head -40
```

Results (sorted by "instructions including callees", excerpt):

```
40.46%  wire::encode_result(BattleResult const&)
30.98%  term::encode(Value const&)
21.29%  std::vector<std::pair<std::string, Value>>::vector(const vector&)   ← copy constructor!
13.16%  std::vector<unsigned char>::_M_range_insert                          ← the output buffer inserted bit by bit
11.57%  Engine::simulate
11.31%  std::vector<Value>::vector(const vector&)                            ← copy constructor!
```

The clue is unmistakable: **lots of time is spent in `vector` copy constructors**. Encoding a result should just be "read the result, write bytes", so why is anything being copied?

### On a machine with perf

On real servers `perf` is more common (sampling-based, very low overhead, can run directly in production):

```bash
perf record -g ./battle_bench 3000      # sample call stacks
perf report                              # browse the hotspots interactively
# flame graph: perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

In a flame graph, horizontal width represents share of time, so the widest blocks jump out at a glance.

## 4. Finding the cause: `initializer_list` can only copy

Covered in detail in lesson 13, section 6. `encode_result` in `wire.cpp` builds the result like this:

```cpp
return Value::object({
    ...
    {"events", Value::list(std::move(events))},    // thought to be a move
    {"units", Value::list(std::move(units))}
});
```

A braced list first constructs a `std::initializer_list`, whose elements are **`const`**, so `Value::object` can only copy from it. As a result the entire event list (about 900 events, 12 fields each) gets deep-copied once. The `Value::object({...})` inside each event does the same. That's where those two copy-constructor lines in callgrind come from.

There's also a secondary cause: `term.cpp`'s `Writer` doesn't `reserve` before writing output, and `insert`s every few bytes, so the output buffer grows again and again (that 13% of `_M_range_insert`).

Lesson 16's allocation measurements confirm it too: **encoding one battle allocates 2 MB of memory, while the output is only 148 KB**.

## 5. Validating fixes

Without changing the engine code, I wrote two alternative implementations in `practice/encode_bench.cpp` and compared them side by side with the original:

- **A**: the original (build a `Value` tree with `initializer_list`, then `term::encode`).
- **B**: build the same `Value` tree, but move elements in with `reserve` + `emplace_back`, with no `initializer_list` (using lesson 15's variadic templates and fold expressions).
- **C**: build no intermediate tree at all; stream ETF bytes straight from the `BattleResult`, estimating the size up front and `reserve`-ing once (`practice/direct_encode.hpp`).

**The first step isn't timing but verifying correctness**: the program first encodes 1000 battles with all three methods and confirms the outputs are **byte-identical**, exiting with an error otherwise. Only then does it time them (measured, three runs):

```
all 1000 results: A, B and C produce byte-identical ETF

A  initializer_list tree + term::encode : 1092~1170 us/battle
B  moved tree + term::encode            :  555~731  us/battle  (x1.5~2.0)
C  direct streaming writer              :  246~307  us/battle  (x3.6~4.4)
```

Then measured back in the full pipeline to see the overall effect (the last line of `battle_bench`, again first verifying the first 100 battles' output is byte-identical):

```
with streaming encoder (byte-identical on first 100 battles): 575~613 us/battle, 1632~1739 battles/s, x1.8~1.9
```

**Single-thread throughput rises from about 900 battles/s to about 1700 battles/s, nearly double.** Encoding goes from 4–5× the simulation to about the same as the simulation.

### Choosing between B and C

| | B: fix the copies | C: stream it out |
|---|---|---|
| Gain | About 1.9× | About 4× |
| Scope of change | Only how `encode_result` is written internally | A new dedicated encoder |
| Maintenance cost | Low: still goes through the generic `Value` structure | Medium: when `BattleResult` gains a field, the encoder must be updated too; and it must stay in line with `term.cpp`'s integer encoding rules |
| Risk control | The existing tests cover it | Needs a "byte-for-byte comparison against the generic encoder" test kept permanently (`encode_bench` is one) |

In a real project I'd do B first (low risk, an instant 2×), and if encoding were still the bottleneck, move to C and add the byte-for-byte comparison test to CI. **Explaining the trade-off impresses interviewers more than just saying "I made it 4× faster".**

> This change is left for you to do: rewrite `encode_result` in `src/wire.cpp` in B's style, get `ctest` and `encode_bench`'s byte-for-byte comparison passing, and commit it to your own branch. It'll be a real, verifiable optimization on your résumé (lesson 24).

## 6. Where's the next bottleneck?

With encoding fixed, simulation becomes the biggest piece. Lesson 16's measurements point the way:

| Observation | Data | Directions to try |
|---|---|---|
| Lots of heap allocation per battle | 1093 allocations / 318 KB | `result.events.reserve(...)` after estimating the event count; give `TargetSelector` a reused buffer (`thread_local` or a member) |
| `Event` is too big | 136 bytes, 64 of them in two `std::string`s | Change `phase` and `type` to `enum class : uint8_t`, converted to atoms at encoding time |
| Allocator contention under multithreading | 1566 `mprotect` system calls in the TCP server test (lesson 19) | Allocate less; or switch to jemalloc / mimalloc (`LD_PRELOAD` is enough to try it) |
| Random branches | Lesson 12: dispatch cost mostly comes from branch mispredictions | Usually not worth changing the design over |

Every item follows section 1's process: measure, change, verify the results are unchanged, measure again.

Also, C++17's `std::pmr` (polymorphic memory resources) offers a way to "give each battle one memory pool and free it all at once when the battle ends":

```cpp
std::array<std::byte, 256 * 1024> buffer;
std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size());
std::pmr::vector<Event> events(&arena);      // allocates from the arena, never calls malloc
```

This requires switching the containers in `BattleState` to their `std::pmr` versions, a fairly large change best made after confirming allocation really is the bottleneck.

## 7. What the compiler can do for you

| Option | Effect | Measured or notes for this project |
|---|---|---|
| `-O2` / `-O3` | Standard optimization | 29× faster than `-O0` |
| LTO (`-flto`) | Inlining and optimization across `.cpp` files at link time | Two rounds within the noise; no measurable gain |
| PGO (`-fprofile-generate` / `-fprofile-use`) | Run once to collect how hot branches and calls are, then optimize accordingly | Often a 10%–30% gain for branchy code; needs representative training data |
| `-march=native` | Use every instruction set the local CPU supports | **Careful**: the program may crash outright on older CPUs; production deployments usually target something conservative (e.g. `-march=x86-64-v2`) |

## 8. Concurrency and latency data

Measurements from earlier lessons, collected here:

| Scenario | Data | Lesson |
|---|---|---|
| Thread pool running battles concurrently | 3.5× speedup with 4 threads, results byte-identical to single-threaded | 17 |
| Thread pool scheduling overhead | Empty tasks: about 2 µs with 1 worker, about 20 µs with 4 | 17 |
| TCP server latency | Serial on one connection: p50 2.1 ms, p99 4.2 ms | 19 |
| TCP server throughput | 8 connections, 200 battles in 0.15 s | 19 |
| False sharing | Two independent counters side by side are 4× slower | 18 |
| AoS vs SoA | Updating one field, SoA is 13× faster | 20 |

**Look at latency percentiles, not just the average.** p99 = 4.2 ms means the slowest 1 in 100 requests takes 4.2 ms. Game servers often care most about "how are the slowest players doing", which an average hides.

## 9. Optimization priorities

```
1. Algorithms and data structures  ── O(n) → O(log n): skiplist rank lookups 8000× faster (lesson 20)
2. Don't do unnecessary work       ── remove deep copies: encoding 2–4× faster (this lesson)
3. Fewer memory allocations        ── reserve, reused buffers, memory pools
4. Data layout                     ── SoA, smaller structs, avoiding false sharing
5. Parallelism                     ── a thread pool: 3.5× faster on 4 cores
6. Compiler options                ── LTO / PGO: gains usually between noise level and 30%
7. Micro-optimization              ── table CRC 4× faster, but meaningless for a 413-byte config (lesson 15)
```

The higher up the list, the bigger the gain and the smaller the risk.

## Interview questions

**Q1: What performance optimizations have you done? How did you go about it?**
(Answer with this lesson's experience.) I first wrote a segmented benchmark covering the full pipeline and found that result encoding took about 78% of the time; callgrind showed most of it was spent in `vector` copy constructors, because building nested maps with `initializer_list` deep-copied the entire event list; I wrote two alternative implementations, first verified that their output for 1000 battles was byte-identical, then timed them: removing the copies was about 1.9× faster, streaming the output about 4× faster, and full-pipeline throughput rose from about 900 battles/s to about 1700 battles/s.

**Q2: How do you write a trustworthy benchmark?**
Measure a Release build; warm up first; make sure results are used (`volatile` or printing) so they aren't optimized away; run several times to check stability; stay suspicious of numbers that don't make sense (such as less than one clock cycle per operation); verify that results match before comparing timings.

**Q3: What profiling tools do you commonly use?**
`perf` (sampling, low overhead, usable in production), flame graphs, valgrind's callgrind (precise instruction-level counts) and massif (heap memory), gprof, Intel VTune. Plus business-level metrics: throughput per second and latency percentiles.

**Q4: What is Amdahl's law?**
The overall speedup from optimizing one part is limited by that part's share of the total time. In this project simulation is only about 15%, so making it infinitely fast would only make the whole about 1.18× faster; making the 78% encoding part 4× faster makes the whole nearly 2× faster.

**Q5: Why can't you benchmark a Debug build?**
Unoptimized code can be tens of times slower (measured: 29×), and the relative proportions of the parts change too, so you'd chase the wrong bottleneck. For analysis use `RelWithDebInfo`, which has both optimization and symbols.

**Q6: What are the ways to reduce memory allocations?**
`reserve` ahead of time; reuse buffers (members or `thread_local`); object pools; `std::pmr` memory pools (one arena per request, freed all at once at the end); avoid unnecessary copies (`initializer_list`, pass-by-value); switch to a multithreading-friendly allocator (jemalloc, tcmalloc, mimalloc).

**Q7: Which matters more, average latency or p99 latency?**
Watch both, but p99 (and p999) better reflect the worst-case user experience; the average hides a few very slow requests. This project's TCP server measured p50 at 2.1 ms and p99 at 4.2 ms.

Next: [Lesson 22: Production troubleshooting and fuzzing](22-debugging-and-fuzzing.md)
