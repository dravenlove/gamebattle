# 第 17 课：线程、锁与线程池

**中文** | [English](en/17-threads-and-pools.md)

> 第三部分「并发」的第一课。实战代码：`practice/thread_pool.hpp`、`practice/battle_thread_pool.cpp`。
>
> 你在 Erlang 里写了多年的并发，但 Erlang 的并发和 C++ 完全是两种模型。这一课先讲清楚差别，再用一个线程池并发跑真实的战斗。

## 1. 两种并发模型

| | Erlang | C++ |
|---|---|---|
| 并发单位 | 进程（轻量，几百字节，可以开几百万个） | 线程（操作系统线程，每个默认几 MB 栈，通常开几十个） |
| 共享数据 | **不共享**，进程之间只能发消息（复制） | **默认共享**同一块内存，任何线程都能读写任何变量 |
| 同步方式 | 消息传递、邮箱 | 互斥锁、条件变量、原子变量 |
| 一个出错 | 只有那个进程退出，监督者重启 | 未捕获的异常或非法内存访问会让**整个进程**崩溃 |
| 数据竞争 | 不可能发生 | 最常见的 Bug 之一，而且很难复现 |

C++ 的核心难点在第二行：**线程共享内存**。只要两个线程同时访问同一个变量，且至少有一个在写，就是**数据竞争**，属于未定义行为。

这个项目在设计上已经避开了大部分风险：
- 一场战斗的所有状态都在一个 `BattleRunner` 对象里，不和别的战斗共享（第 4 课）；
- 共享的配置是只读的 `shared_ptr<const ConfigStore>`（第 2 课）；
- 唯一需要修改的共享状态是"当前配置指针"，用读写锁保护（第 9 课）。

所以引擎可以安全地被多个线程同时调用。下面先看不这样设计会发生什么。

## 2. 数据竞争

```cpp
long g_plain = 0;
std::mutex g_mutex; long g_locked = 0;
std::atomic<long> g_atomic{0};

auto work = [] {
    for (int i = 0; i < 1'000'000; ++i) {
        ++g_plain;                                          // 没有保护
        { std::lock_guard lock(g_mutex); ++g_locked; }      // 互斥锁
        g_atomic.fetch_add(1, std::memory_order_relaxed);   // 原子操作
    }
};
std::jthread a(work), b(work);                              // 两个线程各加 100 万次
```

实测（两次运行）：

```
期望 2000000 | 普通 long: 1984529 | mutex: 2000000 | atomic: 2000000
期望 2000000 | 普通 long: 1977717 | mutex: 2000000 | atomic: 2000000
```

`++g_plain` 看起来是一条语句，实际上是"读出来、加 1、写回去"三步。两个线程同时读到 100，各自加 1，都写回 101，就丢了一次。丢多少每次都不一样，这就是数据竞争难查的原因：**测试时可能碰巧正确，上线后偶尔出错**。

用 ThreadSanitizer 编译（`-fsanitize=thread`），它会精确指出冲突的两次访问：

```
WARNING: ThreadSanitizer: data race
    #0 operator() race.cpp:8
  Previous write of size 8 at 0x555f461a51f0 by thread T1:
    #0 operator() race.cpp:8
```

**规则：多线程代码一定要用 TSan 跑一遍。** 本课的线程池和第 19 课的 TCP 服务器都在 TSan 下跑过，0 警告。

## 3. 互斥锁

```cpp
std::mutex mutex;

{
    std::lock_guard lock(mutex);     // 构造时加锁
    ...临界区...
}                                    // 析构时解锁：RAII，抛异常也会解锁
```

| 工具 | 用途 |
|---|---|
| `std::mutex` | 最基本的互斥锁 |
| `std::lock_guard` | 作用域内一直持有，最简单 |
| `std::unique_lock` | 可以中途解锁、再加锁，可以移动；**条件变量必须用它** |
| `std::scoped_lock` | 一次锁住多个互斥锁，并且不会死锁 |
| `std::shared_mutex` + `std::shared_lock` | 读写锁：多个读者或一个写者（第 9 课的 `Handler`） |

**永远不要手动调用 `mutex.lock()` / `unlock()`**：中间一旦抛异常或提前 return，锁就永远不会释放了。

### 死锁

两个公会互相转账，每次转账要同时锁住两个账户：

```cpp
void transfer_naive(Account& from, Account& to, long amount) {
    std::lock_guard a(from.mutex);
    std::lock_guard b(to.mutex);      // 线程 1 锁了 A 等 B，线程 2 锁了 B 等 A
    ...
}
```

实测，两个线程以相反方向转账：

```
-- 两个线程以相反顺序加锁（2 秒超时）:
   退出码 124（124 = 被 timeout 杀掉，说明卡死了）
-- 改用 std::scoped_lock:
完成，总金币 2000
```

`std::scoped_lock lock(from.mutex, to.mutex);` 内部用一种避免死锁的算法同时获取多个锁。另一种通用做法是**全局规定加锁顺序**，比如总是先锁 ID 小的账户。

Erlang 里也能"死锁"：两个 `gen_server` 互相 `call` 对方。但默认 5 秒超时会把它变成一个错误。C++ 的锁没有超时，会一直卡下去。

### 锁的粒度

**持有锁的时间越短越好。** 看项目和实战代码里的写法：

```cpp
// handle_etf：锁里只复制一个指针，整场战斗在锁外跑（第 9 课）
{ std::shared_lock lock(config_mutex_); configs = configs_; }

// 线程池：锁里只取出任务，执行在锁外
{ std::unique_lock lock(mutex_); ...; task = std::move(tasks_.front()); tasks_.pop(); }
task();

// TCP 服务器：锁里只做一次 swap
{ std::lock_guard lock(completions_mutex_); ready.swap(completions_); }
```

**锁里不要做 I/O、不要调用可能很慢的函数、不要调用别人传进来的回调**（你不知道它会不会再去加另一把锁）。

## 4. 条件变量：等待某个条件成立

线程池的工作线程没事做时，应该睡眠，而不是空转占用 CPU。`std::condition_variable` 让线程在某个条件成立之前一直等待：

```cpp
// 工作线程
std::unique_lock lock(mutex_);
ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });   // 条件不成立就睡
// 醒来时：锁已经重新拿到，而且条件一定成立

// 提交任务的线程
{ std::lock_guard lock(mutex_); tasks_.emplace(...); }
ready_.notify_one();                                                   // 叫醒一个
```

`wait(lock, 条件)` 等价于：

```cpp
while (!条件()) {
    ready_.wait(lock);    // 原子地：释放锁 + 开始睡眠；被叫醒后重新加锁再返回
}
```

两个必须知道的陷阱：

1. **虚假唤醒**（spurious wakeup）：线程可能在没人 `notify` 的情况下醒来。所以一定要用循环（或者带谓词的 `wait`）重新检查条件。
2. **丢失唤醒**：如果先 `notify` 后 `wait`，而 `wait` 不检查条件，这次通知就丢了，线程会永远睡下去。带谓词的 `wait` 会先检查条件，条件已经成立就直接返回。

**修改条件必须在持有锁的情况下进行**（上面 `tasks_.emplace` 在锁里），否则检查条件和进入睡眠之间可能插进一次修改加通知，又会丢失唤醒。`notify` 本身可以在锁外调用，这样被叫醒的线程不用马上又等锁。

这和 Erlang 的 `receive` 很像：进程在邮箱里没有匹配的消息时睡眠，有消息到达时被唤醒，再重新匹配。区别在于 Erlang 把"检查条件"和"睡眠"做成了一个原子操作，C++ 要靠"锁 + 循环检查"自己保证。

## 5. 线程池：`practice/thread_pool.hpp`

为什么不每场战斗开一个线程？创建和销毁操作系统线程的代价是几十微秒，每个线程还有几 MB 的栈。线程池预先创建固定数量的线程，反复用来执行任务。

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
        // jthread 在析构时 join：队列里剩下的任务会先执行完
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
            task();        // 在锁外执行
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    bool stopping_{false};
    std::vector<std::jthread> workers_;   // 最后一个成员
};
```

逐个解释关键设计：

- **`std::packaged_task` + `std::future`**：`packaged_task` 包装一个函数，执行后把返回值（或抛出的异常）存进一个共享状态；`future` 是读取这个状态的一端，`future.get()` 会等到结果就绪。这相当于 Erlang 里"发一个请求，拿一个引用，之后用 `receive` 等回复"。
- **为什么包在 `shared_ptr` 里**：`std::function` 要求它装的东西**可以复制**，而 `packaged_task` 只能移动。所以把它放进 `shared_ptr`，`std::function` 复制的是指针。（C++23 的 `std::move_only_function` 可以直接装只能移动的对象。）
- **`std::forward<Function>`**：完美转发（第 13 课）。
- **`std::invoke_result_t<Function>`**：在编译期算出"调用这个函数会返回什么类型"（第 15 课的类型特征）。
- **`std::jthread`**（C++20）：析构时自动 `join`。旧的 `std::thread` 如果析构时还没 `join` 或 `detach`，会直接 `std::terminate`。
- **`workers_` 必须是最后一个成员**：成员按声明顺序构造、按相反顺序析构（第 2 课）。`workers_` 最后构造，保证线程启动时 `mutex_`、`tasks_` 都已经存在；最先析构，保证线程在 `mutex_`、`tasks_` 被销毁之前就已经退出。
- **析构时先把剩余任务做完**：`stopping_ && tasks_.empty()` 才退出。如果想"立刻停止、丢弃剩余任务"，改成只判断 `stopping_` 即可。

## 6. 并发跑真实的战斗

`practice/battle_thread_pool.cpp` 先单线程跑 2000 场战斗（每场不同的 seed），记下每场结果的哈希值（对编码后的 ETF 字节做 FNV-1a）；再分别用 1、2、4 个线程的线程池跑同样的 2000 场，逐场比较哈希。实测（4 核机器）：

```
hardware_concurrency = 4, battles = 2000
serial        : 2.80 s
pool 1 thread : 2.64 s  speedup x1.06  results identical: yes
pool 2 threads: 1.65 s  speedup x1.70  results identical: yes
pool 4 threads: 0.79 s  speedup x3.53  results identical: yes
exception crossed threads via future: max_rounds must be between 1 and 10000
```

这组数据说明了三件事：

1. **结果逐字节相同**：不管在哪个线程上跑、多少个线程同时跑，同一个 seed 永远得到同一份战报。这证明了引擎的确定性和线程安全性。在 ThreadSanitizer 下重跑，0 条数据竞争警告。
2. **4 个线程加速约 3.5 倍**：接近线性。达不到 4 倍，因为主线程也在争 CPU，内存分配器也有一定的竞争（第 21 课）。
3. **异常可以跨线程传递**：任务里抛出的 `std::invalid_argument` 被存进 `future`，在主线程调用 `get()` 时重新抛出。

另外，提交任务时 lambda 按引用捕获了 `request`：

```cpp
futures.push_back(pool.submit([&request] { ... }));
```

这样做安全，是因为 `requests` 这个 vector 一直活到所有 `future.get()` 都返回之后。如果把 `submit` 放进一个函数、函数返回了但任务还没执行，`request` 就成了悬空引用（第 5 课：被带走的 lambda 要按值捕获）。

### 线程池本身的开销

我测了提交 20 万个**空任务**的调度开销：

```
1 个工作线程: 每个空任务约 2 us
4 个工作线程: 每个空任务约 20 us
```

线程多了，开销反而大了 10 倍。原因是所有线程争抢**同一把锁、同一个队列**，每次 `notify_one` 还可能触发一次系统调用来唤醒线程。

对战斗来说这不是问题：一场战斗约 1 ms，20 µs 的调度开销只占 2%。但如果任务非常小（比如每个任务只算一次伤害），这个线程池就会被锁拖垮。工业级线程池的做法：
- **批量提交**：一个任务处理一批数据；
- **每个线程一个本地队列 + 工作窃取**（work stealing）：自己的队列空了，再去偷别人的，大大减少锁竞争；
- 用无锁队列（第 18 课）。

**任务粒度要和调度开销匹配**，这是设计并发系统时的基本判断。

## 7. 其他常用工具

### `std::jthread` 与协作式取消

```cpp
std::jthread ticker([](std::stop_token stop) {
    while (!stop.stop_requested()) { ...; std::this_thread::sleep_for(10ms); }
});
// ticker 析构时：自动 request_stop()，然后 join()
```

实测输出：`收到停止请求，共 tick 6 次，干净退出`。

C++ 不能从外部强行杀死一个线程（这一点和 Erlang 的 `exit(Pid, kill)` 完全不同），只能**请求**它停下，由线程自己定期检查并退出。

### `thread_local`：每个线程一份

```cpp
std::vector<int>& scratch() {
    thread_local std::vector<int> buffer;     // 每个线程第一次调用时创建，线程结束时销毁
    return buffer;
}
```

实测两个线程拿到的是不同的地址，各自的容量保留下来反复使用。第 16 课提到 `TargetSelector::select` 每次都返回一个新的 `vector`，改成用 `thread_local` 的缓冲区，就能在多线程跑战斗时避免重复分配，又不需要加锁。这相当于 Erlang 的进程字典：每个进程（线程）私有。

### `std::async`

```cpp
auto future = std::async(std::launch::async, [] { return simulate(request); });
```

最简单的"在另一个线程跑一下"。但每次调用可能都新建一个线程，而且如果你不保存返回的 `future`，它的析构函数会**阻塞等待**任务完成，容易写出意外的串行代码。服务器代码里用线程池更可控。

## 面试题

**Q1：什么是数据竞争？怎么发现？**
两个线程同时访问同一内存位置，至少一个是写，且没有同步，就是数据竞争，属于未定义行为。实测两个线程各自 `++` 100 万次，结果少了约 2 万。用 ThreadSanitizer（`-fsanitize=thread`）能精确定位冲突的两次访问。

**Q2：死锁的四个必要条件？怎么避免？**
互斥、持有并等待、不可抢占、循环等待。实践中最常用的办法是破坏"循环等待"：全局规定加锁顺序，或者用 `std::scoped_lock` 一次获取多把锁。另外，持锁时不调用外部代码、不做 I/O。

**Q3：`lock_guard`、`unique_lock`、`scoped_lock` 的区别？**
`lock_guard` 在作用域内一直持有，最轻量。`unique_lock` 可以延迟加锁、中途解锁、移动，条件变量必须配合它使用。`scoped_lock`（C++17）可以同时锁多个互斥锁并避免死锁。

**Q4：条件变量为什么要配合一个条件判断？什么是虚假唤醒？**
线程可能在没有被通知的情况下醒来（虚假唤醒），也可能在开始等待之前通知就已经发出（丢失唤醒）。所以要在持锁状态下用循环检查条件，或者使用 `wait(lock, predicate)`。

**Q5：手写一个线程池，要注意什么？**
一把锁保护任务队列，条件变量让空闲线程睡眠；任务在锁外执行；用 `packaged_task` + `future` 返回结果和异常；析构时设置停止标志、`notify_all`、`join`；成员声明顺序要保证线程最后启动、最先停止。进一步优化：每线程本地队列、工作窃取、批量提交。

**Q6：线程数设置多少合适？**
CPU 密集型任务（比如战斗计算）一般等于核数（`std::thread::hardware_concurrency()`）；I/O 密集型可以多一些。实测本项目在 4 核机器上用 4 个线程加速 3.5 倍。线程越多，锁竞争和上下文切换越多：实测 4 个工作线程时，空任务的调度开销是 1 个线程的 10 倍。

**Q7：你的战斗引擎为什么可以多线程并发调用？**
每场战斗的可变状态都封装在独立的 `BattleRunner` 里，不共享；共享的配置是只读的 `shared_ptr<const ConfigStore>`；唯一需要修改的共享状态（当前配置指针）用读写锁保护，锁里只做指针的复制和替换。实测 4 线程并发跑 2000 场，结果与单线程逐字节相同，ThreadSanitizer 无警告。

**Q8：Erlang 进程和 C++ 线程的区别？**
Erlang 进程是虚拟机调度的轻量进程，不共享内存，通过消息复制通信，崩溃互不影响。C++ 线程是操作系统线程，默认共享整个地址空间，需要锁和原子变量同步，一个线程崩溃会导致整个进程退出。

下一课：[第 18 课：原子操作与内存模型](18-atomics-memory-model.md)
