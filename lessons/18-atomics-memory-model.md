# 第 18 课：原子操作与内存模型

> 这是 C++ 面试里公认最难的一块，也是中高级岗位一定会问的。这一课的每个结论都有实测，并且特别说明哪些现象**在 x86 上看不出来**、必须靠工具才能发现。

## 1. 为什么需要"内存模型"

第 17 课说过，`++counter` 是"读、加、写"三步，多线程下会丢失更新。互斥锁能解决，但还有一个更隐蔽的问题：**编译器和 CPU 都会重新排列你的读写顺序**。

```cpp
// 线程 A                          // 线程 B
report.rounds = 19;                while (!ready) {}
ready = true;                      use(report.rounds);   // 一定能读到 19 吗？
```

单看线程 A，先写 `rounds` 再写 `ready`，交换顺序对 A 自己没有任何影响，所以编译器可以交换，CPU 也可能让 `ready` 先被别的核看到。线程 B 就可能看到 `ready == true`，却读到旧的 `rounds`。

**内存模型**就是 C++ 标准对"一个线程的写，什么时候、以什么顺序被另一个线程看到"的规定。`std::atomic` 加上 `memory_order` 参数，就是你和编译器、CPU 之间的约定。

Erlang 程序员从来不用想这些：进程不共享内存，消息发出去时数据已经完整复制好了。

## 2. `std::atomic` 的基本操作

```cpp
std::atomic<long> counter{0};
counter.fetch_add(1);                 // 原子地加 1，返回旧值
counter.load();                       // 原子地读
counter.store(5);                     // 原子地写
counter.exchange(7);                  // 原子地写入新值，返回旧值
counter.compare_exchange_weak(expected, desired);   // CAS，见第 5 节
```

- 原子操作**不可分割**：其他线程要么看到操作之前的值，要么看到之后的值，不会看到"一半"。
- `is_lock_free()` 告诉你它是用硬件原子指令实现的，还是内部偷偷用了锁。实测 `std::atomic<long>` 是 lock-free 的，而 `std::atomic<std::shared_ptr<T>>` **在 libstdc++ 里不是**（见第 7 节）。
- **`volatile` 和多线程没有关系。** `volatile` 只保证编译器不优化掉对这个变量的读写（用于内存映射的硬件寄存器），既不保证原子性，也不保证顺序。Java 的 `volatile` 和 C++ 的完全是两回事。

## 3. 六种内存序，实际只需要掌握三种

| `memory_order` | 含义 | 什么时候用 |
|---|---|---|
| `relaxed` | 只保证这个操作本身是原子的，**不保证和其他读写之间的顺序** | 纯计数器、统计 |
| `release`（用于写） | 这次写之前的所有读写，都不会被排到它后面 | 发布数据 |
| `acquire`（用于读） | 这次读之后的所有读写，都不会被排到它前面 | 获取数据 |
| `acq_rel` | 同时具有两者，用于读-改-写操作 | CAS、`fetch_add` 做同步时 |
| `seq_cst`（默认） | 在 acquire/release 的基础上，所有线程看到的所有 `seq_cst` 操作顺序一致 | 不确定时用它 |
| `consume` | 实际上没有编译器真正实现，都当 `acquire` 处理 | 别用 |

核心是 **release-acquire 配对**：

```
线程 A                                      线程 B
report.rounds = 19;          ─┐
report.reason = "...";        │ 这些写
ready.store(true, release); ──┘──同步──▶ ready.load(acquire) 读到 true
                                         ──┐
                                           │ 之后的读一定能看到 A 在 release 之前写的所有内容
                                         use(report.rounds);  ──┘
```

当 B 的 acquire 读到了 A 的 release 写入的值，A 在 release 之前的所有写，对 B 在 acquire 之后的所有读都可见。这叫"**先行发生**"（happens-before）关系。互斥锁的 `unlock` 就是一次 release，`lock` 就是一次 acquire，这就是锁能保护数据的原理。

### 实测：x86 上"错误的代码碰巧是对的"

```cpp
BattleReport g_report;                // 普通数据
std::atomic<bool> g_ready{false};

// 生产者
g_report.rounds = 19; g_report.reason = "all_units_defeated";
g_ready.store(true, order);

// 消费者
while (!g_ready.load(order)) {}
std::cout << g_report.rounds << g_report.reason;
```

用 ThreadSanitizer 编译，分别使用两种内存序（实测）：

```
-- relaxed 发布:
  读到 rounds=19 reason=all_units_defeated
WARNING: ThreadSanitizer: data race
-- release / acquire 发布:
  读到 rounds=19 reason=all_units_defeated
```

**两次输出都是对的**，但 relaxed 版本被 TSan 判定为数据竞争。原因是 x86 的硬件内存模型比较强（叫 TSO），普通的写本来就不会被重排到后面的写之后，所以错误的代码在 x86 上**碰巧**能工作。但是：
- 编译器仍然有权重排它们，换个优化级别、换个编译器版本，就可能出错；
- 拿到 ARM 服务器（现在很多云服务器和所有手机都是 ARM）上，硬件本身就会重排，大概率出错。

这就是为什么**内存序的 Bug 无法靠测试发现，必须靠正确的推理加上 TSan**。

## 4. 伪共享：互不相关的变量也会互相拖慢

CPU 缓存的最小单位是**缓存行**，x86 上是 64 字节。两个核同时修改**同一个缓存行**里的不同变量，这个缓存行会在两个核之间来回传递，叫**伪共享**（false sharing）。

```cpp
struct Packed { std::atomic<long> a{0}; std::atomic<long> b{0}; };                    // 16 字节，同一个缓存行
struct Padded { alignas(64) std::atomic<long> a{0}; alignas(64) std::atomic<long> b{0}; };   // 各占一个缓存行
```

两个线程各自只加自己的那个计数器 5000 万次，彼此没有任何逻辑关系。实测：

```
sizeof(Packed)=16 sizeof(Padded)=128
两个计数器挨在一起: 1317 ms   各占一个缓存行: 329 ms
两个计数器挨在一起: 1498 ms   各占一个缓存行: 351 ms
```

**慢了约 4 倍**，仅仅因为两个变量挨得太近。

- `alignas(64)` 让变量按 64 字节对齐，独占一个缓存行。C++17 提供了 `std::hardware_destructive_interference_size` 表示这个值（部分编译器会对它给出可移植性警告，很多项目直接写 64）。
- 第 14 课里"4 个线程复制同一个 `shared_ptr` 慢了 15 倍"，本质是**真共享**：大家确实在改同一个计数。
- 下面的无锁队列里，生产者写的 `head_` 和消费者写的 `tail_` 也是分开放的，就是为了避免伪共享。

## 5. CAS：无锁算法的基本构件

**比较并交换**（Compare-And-Swap）：如果当前值等于期望值，就换成新值；否则把期望值更新成当前值，返回失败。整个过程是原子的。

用它实现"并发记录最高单次伤害"：

```cpp
std::atomic<long> g_max_damage{0};

void record(long damage) {
    long current = g_max_damage.load(std::memory_order_relaxed);
    while (damage > current &&
           !g_max_damage.compare_exchange_weak(current, damage, std::memory_order_relaxed)) {
        // 失败：说明别的线程刚改过，current 已经被更新成最新值，重新比较
    }
}
```

实测 4 个线程各记录 10 万个随机值：`4 个线程并发记录最大值: 999986  期望: 999986`。

- `compare_exchange_weak` 允许**伪失败**（值明明相等也可能返回失败），但在某些平台上更快，适合放在循环里。不在循环里用时，选 `compare_exchange_strong`。
- **ABA 问题**：线程 1 读到 A，被打断；线程 2 把 A 改成 B，又改回 A；线程 1 的 CAS 成功了，但这期间数据已经被动过。在无锁链表这类结构里会导致严重错误，常见解法是给指针附加一个版本号。

## 6. 实战：无锁 SPSC 队列

**单生产者单消费者**（SPSC）队列是最简单、也最常用的无锁结构：只有一个线程 push，只有一个线程 pop。游戏服务器里，网络线程把消息交给逻辑线程、逻辑线程把日志交给日志线程，都是这种场景。

```cpp
template <typename T, std::size_t Capacity>      // Capacity 必须是 2 的幂
class SpscQueue {
public:
    bool try_push(T value) {
        const auto head = head_.load(std::memory_order_relaxed);                   // 只有生产者写 head_，读自己的值用 relaxed
        if (head - tail_.load(std::memory_order_acquire) == Capacity) return false;   // 满了
        slots_[head & (Capacity - 1)] = std::move(value);                         // ① 先写数据
        head_.store(head + 1, std::memory_order_release);                         // ② 再发布：消费者看到新 head 时，数据一定已经写好
        return true;
    }
    std::optional<T> try_pop() {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return std::nullopt;   // 空了；acquire 与生产者的 release 配对
        T value = std::move(slots_[tail & (Capacity - 1)]);
        tail_.store(tail + 1, std::memory_order_release);                         // 告诉生产者这个槽可以复用了
        return value;
    }
private:
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::array<T, Capacity> slots_{};
};
```

逐个说明：
- `head_` 只被生产者写，`tail_` 只被消费者写，**没有任何变量被两个线程同时写**，所以不需要 CAS。
- 每个线程读**自己写的**那个变量用 `relaxed` 就够了；读**对方写的**那个变量用 `acquire`，和对方的 `release` 配对。
- 下标一直递增，用 `& (Capacity - 1)` 代替 `% Capacity`，这就是要求容量是 2 的幂的原因（`static_assert` 在编译期检查）。`size_t` 递增到溢出也没关系：无符号数会绕回（第 3 课），`head - tail` 的差值依然正确。
- `head_` 和 `tail_` 分别对齐到 64 字节，避免伪共享。

实测传递 1000 万个整数，和"`std::mutex` + `std::queue`"对比：

```
SPSC 无锁队列: 129 ms, 12.9 ns/条      （另一次运行：37.6 ns/条）
mutex 队列   : 1265 ms, 126.5 ns/条    （另一次运行：124.4 ns/条）
```

快 3~10 倍（无锁版的波动取决于两个线程被调度到哪两个核上）。校验和正确，TSan 0 条警告。

**提醒**：无锁代码极难写对。SPSC 是少数可以放心手写的；多生产者多消费者（MPMC）的无锁队列，生产环境请用经过验证的库（比如 `moodycamel::ConcurrentQueue`、`folly::MPMCQueue`）。面试时说清楚"什么场景用、为什么正确、什么时候不该自己写"，比背一段代码更有说服力。

## 7. 回到项目：配置热切换用锁还是用原子

第 9 课的 `Handler` 用 `shared_mutex` 保护当前配置指针；第 14 课提到 C++20 的 `std::atomic<std::shared_ptr>` 也能做同样的事，代码更短：

```cpp
class LockedHandler {                 // 项目现在的写法
    std::shared_ptr<const ConfigStore> current() const { std::shared_lock lock(mutex_); return configs_; }
    void swap(std::shared_ptr<const ConfigStore> next) { std::unique_lock lock(mutex_); configs_ = std::move(next); }
};
class AtomicHandler {                 // C++20 写法
    std::shared_ptr<const ConfigStore> current() const { return configs_.load(); }
    void swap(std::shared_ptr<const ConfigStore> next) { configs_.store(std::move(next)); }
};
```

哪个更快？实测（每毫秒换一次配置，读线程疯狂读取）：

```
atomic<shared_ptr>::is_lock_free() = 0
1 个读线程: shared_mutex 21.6~30.5 次读/us, atomic<shared_ptr> 35.2~37.9 次读/us
3 个读线程: shared_mutex  4.8~5.0  次读/us, atomic<shared_ptr>  4.9~7.3  次读/us
```

几个出乎意料的结论：

1. **`std::atomic<std::shared_ptr>` 在 libstdc++ 里不是 lock-free 的**，内部用了一个自旋锁位。"atomic"不等于"无锁"。
2. 单个读线程时原子版本稍快；**3 个读线程时两者都跌到每微秒约 5 次**，几乎没有区别。
3. 瓶颈根本不在锁上，而在**每次读取都要复制 `shared_ptr`**：所有读线程都在原子地修改**同一个**引用计数（第 14 课实测过的真共享）。

对这个项目来说，结论是**不需要改**：每场战斗只在开始时读取一次配置，每秒最多上千次，离每微秒几次的瓶颈差了三个数量级。

如果真的遇到每秒千万次读取的场景，就需要让读者不复制引用计数，比如每个线程缓存一份快照、配合版本号检查是否需要更新，或者使用 RCU、hazard pointer 这类专门的技术。**先测量，确认瓶颈在哪里，再决定要不要优化、怎么优化**（第 21 课的主题）。

## 小结

| 概念 | 要点 |
|---|---|
| 内存模型 | 编译器和 CPU 会重排读写；`atomic` + `memory_order` 规定可见性和顺序 |
| `relaxed` | 只保证原子性，用于计数器 |
| `release` / `acquire` | 配对使用，发布和获取数据；锁的原理就是它 |
| `seq_cst` | 默认值，最强也最慢，不确定时用它 |
| x86 的陷阱 | 错误的内存序在 x86 上常常"碰巧正确"，要靠 TSan 和推理 |
| 伪共享 | 不同线程写同一缓存行的不同变量，实测慢 4 倍；`alignas(64)` |
| CAS | 无锁算法的基础；`weak` 放循环里；注意 ABA |
| SPSC 队列 | 每个变量只有一个写者，release/acquire 即可，实测比 mutex 快 3~10 倍 |
| `atomic<shared_ptr>` | libstdc++ 里不是 lock-free；多读者时瓶颈在引用计数 |

## 面试题

**Q1：`std::atomic` 和 `volatile` 的区别？**
`atomic` 保证操作不可分割，并通过内存序保证跨线程的可见性和顺序。`volatile` 只阻止编译器优化掉对该变量的访问，不保证原子性和顺序，不能用于线程同步。

**Q2：讲一下 `memory_order_relaxed`、`acquire`、`release`、`seq_cst`。**
`relaxed` 只保证原子性，不约束与其他内存操作的顺序，适合统计计数。`release` 写保证之前的读写不会排到它之后；`acquire` 读保证之后的读写不会排到它之前；两者配对时，release 之前的写对 acquire 之后的读可见。`seq_cst` 在此基础上要求所有 `seq_cst` 操作有一个全局一致的顺序，是默认值，开销最大。

**Q3：为什么用 relaxed 发布数据在测试里没出问题，却仍然是错的？**
x86 是 TSO 内存模型，硬件不会把普通写重排到后面的写之后，所以这类错误在 x86 上经常被掩盖。但编译器仍可以重排，ARM 等弱内存模型的 CPU 也会重排。实测 relaxed 版本输出正确，但 ThreadSanitizer 报告数据竞争。

**Q4：什么是伪共享？怎么解决？**
多个线程频繁写入位于同一缓存行（64 字节）的不同变量，导致缓存行在核之间反复失效和传递。解决办法：用 `alignas(64)` 或填充，让各线程频繁写的变量各占一个缓存行。实测两个独立计数器挨在一起比分开慢约 4 倍。

**Q5：`compare_exchange_weak` 和 `strong` 的区别？什么是 ABA 问题？**
`weak` 允许在值相等时也返回失败（伪失败），在某些架构上更快，适合放在循环里；`strong` 只在值不等时失败。ABA：一个值从 A 变成 B 又变回 A，CAS 无法察觉这期间发生过变化；常用版本号或 hazard pointer 解决。

**Q6：讲一下 SPSC 无锁队列的实现原理。**
环形数组加两个原子下标：`head` 只由生产者写，`tail` 只由消费者写。生产者先写数据再用 release 存入新的 `head`；消费者用 acquire 读 `head`，确认有数据后再读取，读完用 release 更新 `tail`。没有共享写入，所以不需要 CAS。两个下标分开对齐避免伪共享，容量取 2 的幂用位与代替取模。

**Q7：`std::atomic<std::shared_ptr>` 是无锁的吗？**
不一定。libstdc++ 的实现内部用了一个锁位，`is_lock_free()` 返回 false（实测）。另外，多个线程频繁 `load()` 时，瓶颈往往是对同一个引用计数的原子增减，而不是锁本身。

**Q8：什么场景应该用无锁数据结构？**
锁竞争确实是瓶颈、临界区极短、对延迟抖动敏感时（比如网络线程和逻辑线程之间传递消息）。无锁代码很难写对和验证，SPSC 这类简单结构可以手写；复杂的 MPMC 结构应该用成熟的库。而且要先测量，确认锁真的是瓶颈。

下一课：[第 19 课：epoll 与 TCP 战斗服务器](19-epoll-battle-server.md)
