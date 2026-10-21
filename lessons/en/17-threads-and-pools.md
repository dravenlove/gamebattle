# Lesson 17: Threads, locks and thread pools

[中文](../17-threads-and-pools.md) | **English**

> The first lesson of part 3, "Concurrency". Practice code: `practice/thread_pool.hpp`, `practice/battle_thread_pool.cpp`.
>
> You've written concurrent code in Erlang for years, but Erlang's concurrency and C++'s are two completely different models. This lesson first makes the differences clear, then uses a thread pool to run real battles concurrently.

## 1. Two concurrency models

| | Erlang | C++ |
|---|---|---|
| Unit of concurrency | Processes (lightweight, a few hundred bytes, millions of them possible) | Threads (OS threads, each with a stack of several MB by default; usually dozens) |
| Shared data | **Nothing shared**; processes can only send messages (copies) | **Shared by default**: one memory space, any thread can read and write any variable |
| Synchronization | Message passing, mailboxes | Mutexes, condition variables, atomics |
| When one fails | Only that process exits, and the supervisor restarts it | An uncaught exception or illegal memory access crashes **the whole process** |
| Data races | Impossible | One of the most common bugs, and very hard to reproduce |

The core difficulty in C++ is the second row: **threads share memory**. Whenever two threads access the same variable at the same time and at least one writes, it's a **data race**, which is undefined behavior.

This project's design already avoids most of the risk:
- all of a battle's state lives in one `BattleRunner` object, not shared with other battles (lesson 4);
- the shared config is a read-only `shared_ptr<const ConfigStore>` (lesson 2);
- the only shared state that needs modifying is "the current config pointer", protected by a reader-writer lock (lesson 9).

So the engine can safely be called by several threads at once. First, let's see what happens without such a design.

## 2. Data races

```cpp
long g_plain = 0;
std::mutex g_mutex; long g_locked = 0;
std::atomic<long> g_atomic{0};

auto work = [] {
    for (int i = 0; i < 1'000'000; ++i) {
        ++g_plain;                                          // unprotected
        { std::lock_guard lock(g_mutex); ++g_locked; }      // a mutex
        g_atomic.fetch_add(1, std::memory_order_relaxed);   // an atomic operation
    }
};
std::jthread a(work), b(work);                              // two threads, 1 million increments each
```

Measured (two runs):

```
expected 2000000 | plain long: 1984529 | mutex: 2000000 | atomic: 2000000
expected 2000000 | plain long: 1977717 | mutex: 2000000 | atomic: 2000000
```

`++g_plain` looks like one statement but is really three steps: "read, add 1, write back". Two threads both read 100, both add 1, both write back 101, and one increment is lost. How many are lost differs every run, which is why data races are so hard to track down: **tests may happen to pass, and production fails now and then**.

Compile with ThreadSanitizer (`-fsanitize=thread`) and it pinpoints the two conflicting accesses:

```
WARNING: ThreadSanitizer: data race
    #0 operator() race.cpp:8
  Previous write of size 8 at 0x555f461a51f0 by thread T1:
    #0 operator() race.cpp:8
```

**Rule: always run multithreaded code under TSan.** This lesson's thread pool and lesson 19's TCP server have both been run under TSan with 0 warnings.

## 3. Mutexes

```cpp
std::mutex mutex;

{
    std::lock_guard lock(mutex);     // locks on construction
    ...critical section...
}                                    // unlocks on destruction: RAII, unlocks even if an exception is thrown
```

| Tool | Purpose |
|---|---|
| `std::mutex` | The most basic mutex |
| `std::lock_guard` | Held for the whole scope; the simplest |
| `std::unique_lock` | Can unlock and relock midway, can be moved; **required by condition variables** |
| `std::scoped_lock` | Locks several mutexes at once without deadlocking |
| `std::shared_mutex` + `std::shared_lock` | Reader-writer lock: several readers or one writer (lesson 9's `Handler`) |

**Never call `mutex.lock()` / `unlock()` by hand**: if anything throws or returns early in between, the lock is never released.

### Deadlock

Two guilds transfer gold to each other, and each transfer has to lock both accounts:

```cpp
void transfer_naive(Account& from, Account& to, long amount) {
    std::lock_guard a(from.mutex);
    std::lock_guard b(to.mutex);      // thread 1 holds A and waits for B; thread 2 holds B and waits for A
    ...
}
```

Measured, two threads transferring in opposite directions:

```
-- two threads locking in opposite order (2-second timeout):
   exit code 124 (124 = killed by timeout, meaning it hung)
-- with std::scoped_lock:
done, total gold 2000
```

`std::scoped_lock lock(from.mutex, to.mutex);` acquires several locks at once using a deadlock-avoidance algorithm internally. The other general approach is to **fix a global lock order**, such as always locking the account with the smaller ID first.

Erlang can "deadlock" too: two `gen_server`s `call` each other. But the default 5-second timeout turns it into an error. C++ locks have no timeout and stay stuck forever.

### Lock granularity

**Hold a lock for as short a time as possible.** Look at how the project and the practice code do it:

```cpp
// handle_etf: only copy a pointer inside the lock; the whole battle runs outside it (lesson 9)
{ std::shared_lock lock(config_mutex_); configs = configs_; }

// thread pool: only take the task out inside the lock; run it outside
{ std::unique_lock lock(mutex_); ...; task = std::move(tasks_.front()); tasks_.pop(); }
task();

// TCP server: only one swap inside the lock
{ std::lock_guard lock(completions_mutex_); ready.swap(completions_); }
```

**Inside a lock, don't do I/O, don't call functions that might be slow, and don't call callbacks someone handed you** (you don't know whether they'll go and take another lock).

## 4. Condition variables: waiting for a condition to hold

When a thread pool's worker has nothing to do, it should sleep, not spin and burn CPU. `std::condition_variable` makes a thread wait until some condition holds:

```cpp
// worker thread
std::unique_lock lock(mutex_);
ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });   // sleep while the condition is false
// on waking: the lock has been reacquired, and the condition definitely holds

// thread submitting a task
{ std::lock_guard lock(mutex_); tasks_.emplace(...); }
ready_.notify_one();                                                   // wake one up
```

`wait(lock, predicate)` is equivalent to:

```cpp
while (!predicate()) {
    ready_.wait(lock);    // atomically: release the lock + go to sleep; relock after waking, then return
}
```

Two traps you must know:

1. **Spurious wakeups**: a thread may wake up without anyone calling `notify`. So you must re-check the condition in a loop (or use the `wait` overload with a predicate).
2. **Lost wakeups**: if `notify` happens before `wait`, and `wait` doesn't check the condition, the notification is lost and the thread sleeps forever. The `wait` with a predicate checks the condition first and returns immediately if it already holds.

**The condition must be modified while holding the lock** (`tasks_.emplace` above is inside the lock); otherwise a modification plus notification could slip in between checking the condition and going to sleep, and the wakeup is lost again. `notify` itself can be called outside the lock, so the woken thread doesn't immediately have to wait for the lock.

This is a lot like Erlang's `receive`: a process sleeps when its mailbox has no matching message, wakes when a message arrives, and matches again. The difference is that Erlang makes "check the condition" and "go to sleep" one atomic operation, while in C++ you guarantee it yourself with "lock + checking in a loop".

## 5. A thread pool: `practice/thread_pool.hpp`

Why not start a thread per battle? Creating and destroying an OS thread costs tens of microseconds, and each thread has a stack of several MB. A thread pool creates a fixed number of threads up front and reuses them to run tasks.

```cpp
class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count) {
        for (std::size_t index = 0; index < thread_count; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    template <typename Function>
    auto submit(Function&& function) -> std::future<std::invoke_result_t<Function>> {
        using Result = std::invoke_result_t<Function>;
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Function>(function));
        auto future = task->get_future();
        {
            std::lock_guard lock(mutex_);
            if (stopping_) throw std::runtime_error("submit on a stopping thread pool");
            tasks_.emplace([task] { (*task)(); });
        }
        ready_.notify_one();
        return future;
    }

    ~ThreadPool() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        ready_.notify_all();
        // the jthreads join on destruction: tasks left in the queue finish first
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();        // run outside the lock
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    bool stopping_{false};
    std::vector<std::jthread> workers_;   // the last member
};
```

The key design decisions, one by one:

- **`std::packaged_task` + `std::future`**: `packaged_task` wraps a function and, after running it, stores its return value (or thrown exception) in a shared state; `future` is the end that reads that state, and `future.get()` waits until the result is ready. It's like Erlang's "send a request, get a reference, and later `receive` the reply".
- **Why wrap it in a `shared_ptr`**: `std::function` requires what it holds to be **copyable**, but `packaged_task` is move-only. So it's put in a `shared_ptr`, and `std::function` copies the pointer. (C++23's `std::move_only_function` can hold move-only objects directly.)
- **`std::forward<Function>`**: perfect forwarding (lesson 13).
- **`std::invoke_result_t<Function>`**: computes at compile time "what type calling this function returns" (lesson 15's type traits).
- **`std::jthread`** (C++20): joins automatically on destruction. An old `std::thread` that's neither `join`ed nor `detach`ed when destroyed calls `std::terminate`.
- **`workers_` must be the last member**: members are constructed in declaration order and destroyed in reverse (lesson 2). `workers_` is constructed last, guaranteeing `mutex_` and `tasks_` exist when the threads start; it's destroyed first, guaranteeing the threads have exited before `mutex_` and `tasks_` are destroyed.
- **Finish the remaining tasks on destruction**: it exits only when `stopping_ && tasks_.empty()`. To "stop immediately and drop the remaining tasks", check only `stopping_`.

## 6. Running real battles concurrently

`practice/battle_thread_pool.cpp` first runs 2000 battles single-threaded (each with a different seed), recording a hash of each result (FNV-1a over the encoded ETF bytes); then it runs the same 2000 battles on pools of 1, 2 and 4 threads, comparing hashes battle by battle. Measured (4-core machine):

```
hardware_concurrency = 4, battles = 2000
serial        : 2.80 s
pool 1 thread : 2.64 s  speedup x1.06  results identical: yes
pool 2 threads: 1.65 s  speedup x1.70  results identical: yes
pool 4 threads: 0.79 s  speedup x3.53  results identical: yes
exception crossed threads via future: max_rounds must be between 1 and 10000
```

These numbers show three things:

1. **The results are byte-identical**: whichever thread it runs on and however many run at once, the same seed always produces the same battle report. This demonstrates the engine's determinism and thread safety. Rerun under ThreadSanitizer: 0 data race warnings.
2. **Four threads give about a 3.5× speedup**: close to linear. It falls short of 4× because the main thread also competes for CPU, and there's some contention in the memory allocator (lesson 21).
3. **Exceptions can cross threads**: the `std::invalid_argument` thrown in a task is stored in the `future` and rethrown when the main thread calls `get()`.

Also, when submitting tasks the lambda captures `request` by reference:

```cpp
futures.push_back(pool.submit([&request] { ... }));
```

That's safe because the `requests` vector lives until every `future.get()` has returned. If `submit` were inside a function that returned before the task ran, `request` would be a dangling reference (lesson 5: lambdas that get carried away should capture by value).

### The pool's own overhead

I measured the scheduling overhead of submitting 200,000 **empty tasks**:

```
1 worker thread:  about 2 us per empty task
4 worker threads: about 20 us per empty task
```

More threads made the overhead 10× bigger. The reason is that every thread fights over **the same lock and the same queue**, and each `notify_one` may trigger a system call to wake a thread.

For battles that's no problem: a battle takes about 1 ms, and 20 µs of scheduling overhead is only 2%. But if tasks were tiny (say, one damage calculation per task), this pool would be dragged down by its lock. What industrial thread pools do:
- **Batch submission**: one task handles a batch of data;
- **A local queue per thread + work stealing**: when your own queue is empty, steal from someone else's, which greatly reduces lock contention;
- Use a lock-free queue (lesson 18).

**Match task granularity to scheduling overhead**: a basic judgment call when designing concurrent systems.

## 7. Other common tools

### `std::jthread` and cooperative cancellation

```cpp
std::jthread ticker([](std::stop_token stop) {
    while (!stop.stop_requested()) { ...; std::this_thread::sleep_for(10ms); }
});
// when ticker is destroyed: request_stop() automatically, then join()
```

Measured output: `stop requested, ticked 6 times, exited cleanly`.

C++ can't forcibly kill a thread from outside (completely unlike Erlang's `exit(Pid, kill)`); it can only **ask** the thread to stop, and the thread checks periodically and exits on its own.

### `thread_local`: one per thread

```cpp
std::vector<int>& scratch() {
    thread_local std::vector<int> buffer;     // created on each thread's first call, destroyed when the thread ends
    return buffer;
}
```

Measured: the two threads got different addresses, and each kept its capacity for reuse. Lesson 16 mentioned that `TargetSelector::select` returns a new `vector` every time; switching to a `thread_local` buffer would avoid repeated allocation when running battles on many threads, without any locking. It's like Erlang's process dictionary: private to each process (thread).

### `std::async`

```cpp
auto future = std::async(std::launch::async, [] { return simulate(request); });
```

The simplest "run this on another thread". But each call may create a new thread, and if you don't keep the returned `future`, its destructor **blocks until the task finishes**, which makes it easy to write accidentally serial code. In server code, a thread pool is more controllable.

## Interview questions

**Q1: What is a data race? How do you find one?**
Two threads accessing the same memory location at the same time, at least one writing, with no synchronization: that's a data race, and it's undefined behavior. Measured, two threads each doing 1 million `++` came up about 20,000 short. ThreadSanitizer (`-fsanitize=thread`) pinpoints the two conflicting accesses.

**Q2: What are the four necessary conditions for deadlock? How do you avoid it?**
Mutual exclusion, hold and wait, no preemption, circular wait. The most common practical remedy is to break "circular wait": fix a global lock order, or acquire several locks at once with `std::scoped_lock`. Also, don't call external code or do I/O while holding a lock.

**Q3: What's the difference between `lock_guard`, `unique_lock` and `scoped_lock`?**
`lock_guard` holds the lock for the whole scope and is the lightest. `unique_lock` can lock lazily, unlock midway and be moved, and condition variables must use it. `scoped_lock` (C++17) can lock several mutexes at once while avoiding deadlock.

**Q4: Why does a condition variable need a condition check? What's a spurious wakeup?**
A thread may wake without having been notified (a spurious wakeup), or the notification may be sent before it starts waiting (a lost wakeup). So check the condition in a loop while holding the lock, or use `wait(lock, predicate)`.

**Q5: What do you need to watch out for when writing a thread pool by hand?**
One lock protects the task queue, and a condition variable lets idle threads sleep; tasks run outside the lock; `packaged_task` + `future` return results and exceptions; on destruction set the stop flag, `notify_all` and `join`; member declaration order must make the threads start last and stop first. Further optimizations: per-thread local queues, work stealing, batch submission.

**Q6: How many threads is right?**
For CPU-bound tasks (like battle computation), usually the number of cores (`std::thread::hardware_concurrency()`); I/O-bound work can use more. Measured, this project gets a 3.5× speedup with 4 threads on a 4-core machine. More threads mean more lock contention and context switching: measured, with 4 worker threads the scheduling overhead of empty tasks is 10× that of 1 thread.

**Q7: Why can your battle engine be called concurrently from multiple threads?**
Each battle's mutable state is encapsulated in its own `BattleRunner` and not shared; the shared config is a read-only `shared_ptr<const ConfigStore>`; the only shared state that needs modifying (the current config pointer) is protected by a reader-writer lock, and the lock covers only copying and replacing the pointer. Measured, 4 threads running 2000 battles concurrently produce results byte-identical to a single thread, with no ThreadSanitizer warnings.

**Q8: What's the difference between Erlang processes and C++ threads?**
Erlang processes are lightweight processes scheduled by the VM; they share no memory, communicate by copying messages, and don't affect each other when they crash. C++ threads are OS threads that share the whole address space by default, need locks and atomics to synchronize, and one thread crashing takes down the whole process.

Next: [Lesson 18: Atomics and the memory model](18-atomics-memory-model.md)
