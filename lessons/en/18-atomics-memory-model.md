# Lesson 18: Atomics and the memory model

[中文](../18-atomics-memory-model.md) | **English**

> This is widely regarded as the hardest part of C++ interviews, and mid-to-senior roles always ask about it. Every conclusion in this lesson is measured, with special attention to which phenomena are **invisible on x86** and can only be found with tools.

## 1. Why a "memory model" is needed

Lesson 17 said that `++counter` is three steps, "read, add, write", and loses updates under multithreading. A mutex fixes that, but there's a subtler problem: **both the compiler and the CPU reorder your reads and writes**.

```cpp
// thread A                        // thread B
report.rounds = 19;                while (!ready) {}
ready = true;                      use(report.rounds);   // guaranteed to read 19?
```

From thread A's point of view alone, writing `rounds` first and then `ready` could be swapped with no effect on A itself, so the compiler may swap them, and the CPU may let other cores see `ready` first. Thread B could then see `ready == true` and still read the old `rounds`.

The **memory model** is the C++ standard's specification of "when, and in what order, one thread's writes become visible to another thread". `std::atomic` plus a `memory_order` argument is your contract with the compiler and the CPU.

Erlang programmers never have to think about this: processes share no memory, and by the time a message is sent, its data has been fully copied.

## 2. Basic `std::atomic` operations

```cpp
std::atomic<long> counter{0};
counter.fetch_add(1);                 // atomically add 1, return the old value
counter.load();                       // atomic read
counter.store(5);                     // atomic write
counter.exchange(7);                  // atomically write a new value, return the old one
counter.compare_exchange_weak(expected, desired);   // CAS, see section 5
```

- Atomic operations are **indivisible**: other threads see either the value before or the value after, never "half".
- `is_lock_free()` tells you whether it's implemented with hardware atomic instructions or quietly uses a lock inside. Measured: `std::atomic<long>` is lock-free, while `std::atomic<std::shared_ptr<T>>` **isn't in libstdc++** (see section 7).
- **`volatile` has nothing to do with multithreading.** `volatile` only guarantees the compiler won't optimize away reads and writes of the variable (for memory-mapped hardware registers); it guarantees neither atomicity nor ordering. Java's `volatile` and C++'s are completely different things.

## 3. Six memory orders, of which you really need three

| `memory_order` | Meaning | When to use |
|---|---|---|
| `relaxed` | Only guarantees the operation itself is atomic; **no ordering relative to other reads and writes** | Pure counters, statistics |
| `release` (for writes) | No read or write before this write can be moved after it | Publishing data |
| `acquire` (for reads) | No read or write after this read can be moved before it | Acquiring data |
| `acq_rel` | Both at once, for read-modify-write operations | CAS, `fetch_add` used for synchronization |
| `seq_cst` (default) | On top of acquire/release, every thread sees all `seq_cst` operations in one consistent order | When in doubt |
| `consume` | No compiler really implements it; all treat it as `acquire` | Don't use it |

The core is **release-acquire pairing**:

```
thread A                                    thread B
report.rounds = 19;          ─┐
report.reason = "...";        │ these writes
ready.store(true, release); ──┘──synchronizes──▶ ready.load(acquire) reads true
                                                 ──┐
                                                   │ reads after this are guaranteed to see everything A wrote before the release
                                                 use(report.rounds);  ──┘
```

When B's acquire reads the value written by A's release, all of A's writes before the release are visible to all of B's reads after the acquire. This is the **happens-before** relationship. A mutex's `unlock` is a release and its `lock` is an acquire, which is how locks protect data.

### Measured: on x86, "wrong code happens to be right"

```cpp
BattleReport g_report;                // ordinary data
std::atomic<bool> g_ready{false};

// producer
g_report.rounds = 19; g_report.reason = "all_units_defeated";
g_ready.store(true, order);

// consumer
while (!g_ready.load(order)) {}
std::cout << g_report.rounds << g_report.reason;
```

Compiled with ThreadSanitizer, using each of the two memory orders (measured):

```
-- relaxed publish:
  read rounds=19 reason=all_units_defeated
WARNING: ThreadSanitizer: data race
-- release / acquire publish:
  read rounds=19 reason=all_units_defeated
```

**Both outputs are correct**, but TSan judges the relaxed version to be a data race. The reason is that x86's hardware memory model is fairly strong (it's called TSO): an ordinary write is never reordered after a later write anyway, so the wrong code **happens to** work on x86. But:
- the compiler is still entitled to reorder them, so a different optimization level or compiler version could break it;
- on an ARM server (many cloud servers and every phone today are ARM), the hardware itself reorders, and it will very likely break.

That's why **memory-order bugs can't be found by testing; they require correct reasoning plus TSan**.

## 4. False sharing: unrelated variables slowing each other down

The smallest unit of a CPU cache is the **cache line**, 64 bytes on x86. When two cores modify different variables within **the same cache line**, the line bounces back and forth between them. This is called **false sharing**.

```cpp
struct Packed { std::atomic<long> a{0}; std::atomic<long> b{0}; };                    // 16 bytes, one cache line
struct Padded { alignas(64) std::atomic<long> a{0}; alignas(64) std::atomic<long> b{0}; };   // one cache line each
```

Two threads each increment only their own counter 50 million times, with no logical relationship between them. Measured:

```
sizeof(Packed)=16 sizeof(Padded)=128
counters side by side: 1317 ms   one cache line each: 329 ms
counters side by side: 1498 ms   one cache line each: 351 ms
```

**About 4× slower**, purely because the two variables sit too close together.

- `alignas(64)` aligns a variable to 64 bytes so it owns a cache line. C++17 provides `std::hardware_destructive_interference_size` for this value (some compilers issue a portability warning for it, and many projects just write 64).
- Lesson 14's "4 threads copying the same `shared_ptr` was 15× slower" is at heart **true sharing**: everyone really is modifying the same count.
- In the lock-free queue below, `head_` (written by the producer) and `tail_` (written by the consumer) are placed apart for exactly this reason.

## 5. CAS: the basic building block of lock-free algorithms

**Compare-And-Swap**: if the current value equals the expected value, replace it with the new value; otherwise update the expected value to the current value and return failure. The whole thing is atomic.

Using it to "record the highest single hit concurrently":

```cpp
std::atomic<long> g_max_damage{0};

void record(long damage) {
    long current = g_max_damage.load(std::memory_order_relaxed);
    while (damage > current &&
           !g_max_damage.compare_exchange_weak(current, damage, std::memory_order_relaxed)) {
        // failed: another thread just changed it; current now holds the latest value, so compare again
    }
}
```

Measured, 4 threads each recording 100,000 random values: `max recorded concurrently by 4 threads: 999986  expected: 999986`.

- `compare_exchange_weak` allows **spurious failure** (it may fail even when the values are equal), but is faster on some platforms, so it suits loops. Outside a loop, choose `compare_exchange_strong`.
- **The ABA problem**: thread 1 reads A and is interrupted; thread 2 changes A to B and back to A; thread 1's CAS succeeds, but the data was touched in between. In structures like lock-free linked lists this causes serious bugs; a common fix is to attach a version number to the pointer.

## 6. In practice: a lock-free SPSC queue

A **single-producer single-consumer** (SPSC) queue is the simplest and most widely used lock-free structure: only one thread pushes and only one thread pops. In game servers, the network thread handing messages to the logic thread and the logic thread handing logs to the logging thread are both this scenario.

```cpp
template <typename T, std::size_t Capacity>      // Capacity must be a power of 2
class SpscQueue {
public:
    bool try_push(T value) {
        const auto head = head_.load(std::memory_order_relaxed);                   // only the producer writes head_, so reading its own value can be relaxed
        if (head - tail_.load(std::memory_order_acquire) == Capacity) return false;   // full
        slots_[head & (Capacity - 1)] = std::move(value);                         // ① write the data first
        head_.store(head + 1, std::memory_order_release);                         // ② then publish: when the consumer sees the new head, the data is guaranteed written
        return true;
    }
    std::optional<T> try_pop() {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return std::nullopt;   // empty; acquire pairs with the producer's release
        T value = std::move(slots_[tail & (Capacity - 1)]);
        tail_.store(tail + 1, std::memory_order_release);                         // tell the producer this slot can be reused
        return value;
    }
private:
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::array<T, Capacity> slots_{};
};
```

Point by point:
- `head_` is written only by the producer and `tail_` only by the consumer; **no variable is ever written by two threads**, so no CAS is needed.
- Reading the variable **you write yourself** only needs `relaxed`; reading the variable **the other side writes** uses `acquire`, pairing with the other side's `release`.
- The indices only ever increase, and `& (Capacity - 1)` replaces `% Capacity`, which is why the capacity must be a power of 2 (checked at compile time with `static_assert`). It doesn't matter if a `size_t` index overflows: unsigned numbers wrap around (lesson 3), and the difference `head - tail` is still correct.
- `head_` and `tail_` are each aligned to 64 bytes to avoid false sharing.

Measured passing 10 million integers, compared with "`std::mutex` + `std::queue`":

```
SPSC lock-free queue: 129 ms, 12.9 ns/item      (another run: 37.6 ns/item)
mutex queue         : 1265 ms, 126.5 ns/item    (another run: 124.4 ns/item)
```

3–10× faster (the lock-free version varies with which two cores the threads get scheduled on). The checksum is correct and TSan reports 0 warnings.

**A reminder**: lock-free code is extremely hard to get right. SPSC is one of the few you can safely write yourself; for multi-producer multi-consumer (MPMC) lock-free queues, use a proven library in production (such as `moodycamel::ConcurrentQueue` or `folly::MPMCQueue`). In an interview, explaining "when to use it, why it's correct, and when you shouldn't write it yourself" is more convincing than reciting code.

## 7. Back to the project: lock or atomic for config hot-swapping?

Lesson 9's `Handler` protects the current config pointer with a `shared_mutex`; lesson 14 mentioned that C++20's `std::atomic<std::shared_ptr>` can do the same with shorter code:

```cpp
class LockedHandler {                 // what the project does now
    std::shared_ptr<const ConfigStore> current() const { std::shared_lock lock(mutex_); return configs_; }
    void swap(std::shared_ptr<const ConfigStore> next) { std::unique_lock lock(mutex_); configs_ = std::move(next); }
};
class AtomicHandler {                 // the C++20 version
    std::shared_ptr<const ConfigStore> current() const { return configs_.load(); }
    void swap(std::shared_ptr<const ConfigStore> next) { configs_.store(std::move(next)); }
};
```

Which is faster? Measured (swapping the config once per millisecond while reader threads read as fast as they can):

```
atomic<shared_ptr>::is_lock_free() = 0
1 reader thread:  shared_mutex 21.6–30.5 reads/us, atomic<shared_ptr> 35.2–37.9 reads/us
3 reader threads: shared_mutex  4.8–5.0  reads/us, atomic<shared_ptr>  4.9–7.3  reads/us
```

A few surprising conclusions:

1. **`std::atomic<std::shared_ptr>` isn't lock-free in libstdc++**; it uses a spinlock bit internally. "atomic" doesn't mean "lock-free".
2. With one reader thread the atomic version is slightly faster; **with 3 reader threads both drop to about 5 reads per microsecond**, with almost no difference.
3. The bottleneck isn't the lock at all but **copying the `shared_ptr` on every read**: every reader thread atomically modifies **the same** reference count (the true sharing measured in lesson 14).

For this project the conclusion is **no change needed**: each battle reads the config once, at the start, at most a few thousand times per second, three orders of magnitude below a bottleneck of a few reads per microsecond.

If you ever do face tens of millions of reads per second, readers need to avoid copying the reference count: for example, each thread caches a snapshot and checks a version number to see whether it needs refreshing, or you use specialized techniques like RCU or hazard pointers. **Measure first, confirm where the bottleneck is, then decide whether and how to optimize** (the topic of lesson 21).

## Summary

| Concept | Key point |
|---|---|
| Memory model | The compiler and CPU reorder reads and writes; `atomic` + `memory_order` specify visibility and ordering |
| `relaxed` | Guarantees only atomicity; for counters |
| `release` / `acquire` | Used in pairs to publish and acquire data; it's how locks work |
| `seq_cst` | The default, strongest and slowest; use it when in doubt |
| The x86 trap | Wrong memory orders often "happen to work" on x86; rely on TSan and reasoning |
| False sharing | Different threads writing different variables in one cache line, measured 4× slower; `alignas(64)` |
| CAS | The foundation of lock-free algorithms; put `weak` in a loop; beware ABA |
| SPSC queue | Each variable has one writer, so release/acquire is enough; measured 3–10× faster than a mutex |
| `atomic<shared_ptr>` | Not lock-free in libstdc++; with many readers the bottleneck is the reference count |

## Interview questions

**Q1: What's the difference between `std::atomic` and `volatile`?**
`atomic` guarantees indivisible operations and, through memory orders, cross-thread visibility and ordering. `volatile` only stops the compiler from optimizing away accesses to the variable; it guarantees neither atomicity nor ordering and can't be used for thread synchronization.

**Q2: Explain `memory_order_relaxed`, `acquire`, `release` and `seq_cst`.**
`relaxed` only guarantees atomicity and doesn't constrain ordering with other memory operations; it suits statistics counters. A `release` write guarantees earlier reads and writes aren't moved after it; an `acquire` read guarantees later reads and writes aren't moved before it; when they pair up, writes before the release are visible to reads after the acquire. `seq_cst` additionally requires a single globally consistent order of all `seq_cst` operations; it's the default and the most expensive.

**Q3: Why is publishing data with relaxed still wrong when it passed the tests?**
x86 uses the TSO memory model, and the hardware never reorders an ordinary write after a later write, so this kind of bug is often masked on x86. But the compiler can still reorder, and weakly ordered CPUs like ARM do reorder. Measured: the relaxed version printed correct output, but ThreadSanitizer reported a data race.

**Q4: What is false sharing? How do you fix it?**
Several threads frequently writing different variables that sit in the same cache line (64 bytes), so the line is repeatedly invalidated and transferred between cores. The fix: use `alignas(64)` or padding so each thread's frequently written variable gets its own cache line. Measured, two independent counters side by side were about 4× slower than separated ones.

**Q5: What's the difference between `compare_exchange_weak` and `strong`? What is the ABA problem?**
`weak` may fail even when the values are equal (spurious failure); it's faster on some architectures and suits loops. `strong` fails only when the values differ. ABA: a value changes from A to B and back to A, and CAS can't tell anything happened in between; version numbers or hazard pointers are the usual fix.

**Q6: Explain how an SPSC lock-free queue works.**
A ring buffer plus two atomic indices: `head` is written only by the producer and `tail` only by the consumer. The producer writes the data, then stores the new `head` with release; the consumer reads `head` with acquire, reads the data once it knows there is some, then updates `tail` with release. There are no shared writes, so no CAS is needed. The two indices are aligned apart to avoid false sharing, and the capacity is a power of 2 so a bitwise AND replaces the modulo.

**Q7: Is `std::atomic<std::shared_ptr>` lock-free?**
Not necessarily. libstdc++'s implementation uses a lock bit internally, and `is_lock_free()` returns false (measured). Also, when several threads `load()` frequently, the bottleneck is often the atomic increments and decrements of the same reference count, not the lock itself.

**Q8: When should you use lock-free data structures?**
When lock contention really is the bottleneck, critical sections are extremely short, and you're sensitive to latency jitter (passing messages between the network thread and the logic thread, say). Lock-free code is hard to get right and to verify; simple structures like SPSC can be written by hand, while complex MPMC structures should come from mature libraries. And measure first to confirm the lock really is the bottleneck.

Next: [Lesson 19: epoll and a TCP battle server](19-epoll-battle-server.md)
