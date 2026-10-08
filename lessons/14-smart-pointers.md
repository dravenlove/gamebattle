# 第 14 课：智能指针深入

> 第 2 课回答了"为什么用 `shared_ptr<const BuffSpec>`"。这一课把三种智能指针的内部实现、代价和使用场景讲完整，并用它们回头改进项目里的两处代码。

## 1. 三种智能指针，一张表

| | `std::unique_ptr<T>` | `std::shared_ptr<T>` | `std::weak_ptr<T>` |
|---|---|---|---|
| 所有权 | 独占 | 共享（引用计数） | 不拥有，只观察 |
| 能复制吗 | ❌ 只能移动 | ✅ 复制时计数 +1 | ✅ |
| 大小（实测） | 8 字节 | 16 字节 | 16 字节 |
| 额外开销 | 无 | 控制块 + 原子计数 | 同左 |
| 典型用途 | 唯一的主人，比如 RAII 包装 | 多个主人，生命周期不确定 | 打破环、缓存、观察者 |

**默认用 `unique_ptr`**，确实有多个主人时才用 `shared_ptr`。项目里 `BuffSpec` 被配置仓库、多个技能、单位身上的 Buff 同时持有，是真正的共享，所以用了 `shared_ptr`（第 2 课）。

## 2. `unique_ptr`：零开销的独占所有权

`unique_ptr<int>` 实测 8 字节，和普通指针一样大。编译后它就是一个指针加一次析构时的 `delete`，没有任何额外开销。

### 自定义删除器：管理任何资源

`unique_ptr` 的第二个模板参数是**删除器**：离开作用域时调用它，而不是 `delete`。这样就能管理任何"需要手动释放"的资源：

```cpp
auto close_file = [](std::FILE* f) { std::fclose(f); };
std::unique_ptr<std::FILE, decltype(close_file)> file(std::fopen("battle.log", "w"), close_file);
```

删除器的类型会影响大小（实测）：

```
unique_ptr<FILE, 函数指针删除器>      : 16      ← 要存一个函数指针
unique_ptr<FILE, 无捕获 lambda 删除器>: 8       ← 空类型，编译器把它优化掉了
```

**用无状态的函数对象当删除器，不会增加任何开销。**

### 用它补上第 9 课的那个漏洞

第 9 课指出，`nif.cpp` 里的 `enif_release_binary` 是手动调用的，如果中间抛异常就会被跳过。用 `unique_ptr` 包一层：

```cpp
struct ReleaseBinary {
    void operator()(ErlNifBinary* bin) const noexcept { enif_release_binary(bin); }
};
using BinaryGuard = std::unique_ptr<ErlNifBinary, ReleaseBinary>;

ERL_NIF_TERM dispatch(ErlNifEnv* env, ERL_NIF_TERM request_term) {
    ErlNifBinary request{};
    if (!enif_term_to_binary(env, request_term, &request)) return enif_make_badarg(env);
    BinaryGuard guard(&request);      // 从这里开始，不管怎么离开这个函数都会释放
    const auto response = gamebattle::wire::handle_etf(...);
    ...
}
```

我用模拟的 `erl_nif` 声明验证了这个写法：在 `guard` 存活期间抛出异常，`enif_release_binary` 照样被调用，`sizeof(BinaryGuard)` 仍然是 8 字节。

这就是 RAII 的本质：**把"释放"这个动作绑定到对象的析构函数上，让编译器保证它一定执行。** 第 13 课的 `FileDescriptor` 是手写的版本，`unique_ptr` + 删除器是标准库现成的版本。

## 3. `shared_ptr` 的内部：控制块

```
shared_ptr 对象（16 字节）            控制块（堆上）
┌──────────────────┐               ┌──────────────────────────┐
│ T* 指向对象 ──────┼──┐            │ 强引用计数 (atomic)       │  ← shared_ptr 的个数
│ 控制块指针 ───────┼──┼──────────▶ │ 弱引用计数 (atomic)       │  ← weak_ptr 的个数（+1）
└──────────────────┘  │            │ 删除器、分配器            │
                      ▼            └──────────────────────────┘
                  ┌────────┐
                  │ T 对象  │
                  └────────┘
```

- 强计数降到 0：**析构对象**。
- 弱计数也降到 0：**释放控制块**。所以只要还有 `weak_ptr` 存在，控制块就还在，它才能回答"对象还活着吗"。
- 两个计数都是**原子变量**，多个线程同时复制同一个 `shared_ptr` 是安全的，但有代价（见第 6 节）。

### `make_shared`：一次分配

实测（重载全局 `operator new` 计数）：

```
shared_ptr<T>(new T) : 2 次堆分配      ← 对象一次，控制块一次
make_shared<T>()     : 1 次堆分配      ← 对象和控制块放在同一块内存里
make_unique<T>()     : 1 次堆分配
```

`make_shared` 少一次分配，对象和计数挨在一起，对缓存也更友好。还有一个安全上的好处：C++17 之前，`f(shared_ptr<T>(new T), g())` 这种写法在 `g()` 抛异常时可能泄漏，`make_shared` 不会。

一个小代价：用 `make_shared` 时对象和控制块是同一块内存，**只要还有 `weak_ptr`，整块内存都不会释放**（对象已经析构了，但内存还占着）。对象很大、`weak_ptr` 又活得很久时要注意。

**规则：优先用 `make_shared` 和 `make_unique`，不要直接写 `new`。**

## 4. `weak_ptr`：打破环

第 2 课说过，`shared_ptr` 成环会泄漏。实测：

```cpp
struct Buff {
    std::shared_ptr<Buff> adds;          // 反应里会挂上的另一个 Buff
    ~Buff() { std::cout << "~Buff(" << name << ")\n"; }
};
auto poison = std::make_shared<Buff>("poison");
auto burn   = std::make_shared<Buff>("burn");
poison->adds = burn;
burn->adds = poison;                     // 成环
```
```
离开作用域前 use_count: poison=2 burn=2
（没有任何析构输出：两个对象都泄漏了）
AddressSanitizer: 112 byte(s) leaked in 2 allocation(s).
```

把其中一边改成 `weak_ptr`：

```cpp
burn->adds_weak = poison;                        // 不增加强计数
if (auto target = burn->adds_weak.lock()) {      // 用之前先 lock()，拿到一个临时的 shared_ptr
    target->name;                                // 对象一定活着
}
```
```
lock() 成功，拿到 poison
~Buff(poison)
~Buff(burn)
对象销毁后 expired()=1 lock()==nullptr: 1
```

- `weak_ptr` 不能直接访问对象，必须先 `lock()`。对象还活着就得到一个 `shared_ptr`，已经销毁就得到空指针。
- `lock()` 是原子的：拿到的 `shared_ptr` 保证对象在你用完之前不会被销毁。不要先 `expired()` 再 `lock()`，两步之间对象可能被别的线程销毁。

### 为什么项目不用 `weak_ptr`，而是直接禁止成环

既然 `weak_ptr` 能打破环，为什么项目的配置编译器和 `validate_request` 要**拒绝**带环的配置（第 8、10 课）？

因为语义不对。Buff 反应里"给目标挂上灼烧"这个效果，**必须**保证灼烧的定义在执行时存在；用 `weak_ptr` 的话，定义随时可能已经被释放，`lock()` 失败时这个效果该怎么办？没有合理的答案。而且"中毒挂灼烧、灼烧又挂中毒"在玩法上本来就会无限循环，这是策划配置错误，应该在发布前就报出来。

**`weak_ptr` 适合"有了更好，没有也行"的关系**（缓存、观察者、回调里引用一个可能已经断开的连接）。**必须存在的关系，要么用 `shared_ptr` 并保证无环，要么重新设计所有权。**

## 5. `enable_shared_from_this`：让对象在异步回调里活下去

网络服务器里很常见的场景：一个连接对象发起了一个异步操作，回调执行时，客户端可能已经断开，外部持有的 `shared_ptr` 都没了。如果回调里只捕获了 `this`，对象就已经被销毁了。

```cpp
struct Session : std::enable_shared_from_this<Session> {
    void start_battle() {
        auto self = shared_from_this();          // 从 this 得到一个 shared_ptr，计数 +1
        pending_callbacks.push_back([self] {     // 回调持有它，Session 至少活到回调执行完
            self->send_result();
        });
    }
};
```

实测：

```
客户端已断开（外部的 shared_ptr 没了），回调还在队列里
回调执行，Session 1 仍然有效
~Session(1)                        ← 回调执行完、self 被销毁后才析构
```

两个限制：
- 对象必须**已经由 `shared_ptr` 管理**（比如用 `make_shared` 创建）。对一个栈上的对象调用 `shared_from_this()`，实测抛出 `std::bad_weak_ptr`。
- 不能在构造函数里调用：那时还没有任何 `shared_ptr` 指向它。

Boost.Asio 的网络代码几乎都是这个模式。`practice/battle_tcp_server.cpp` 走的是另一条路：连接对象都存在一个 map 里，异步任务只记**连接 ID**，结果回来时按 ID 查找，查不到就说明连接已经断开，直接丢弃（第 2 课"唯一 ID + 查找"）。两种方案各有取舍，面试时都能讲。

## 6. 线程安全：计数安全，变量本身不安全

这是面试最常问的一个点：**`shared_ptr` 是线程安全的吗？**

答案分两层：

1. **引用计数是线程安全的。** 多个线程各自持有**自己的** `shared_ptr` 副本，同时复制、销毁，计数不会出错。
2. **同一个 `shared_ptr` 变量**被一个线程写、另一个线程读，是**数据竞争**。

实测：一个线程不断给全局 `shared_ptr` 赋新值，另一个线程不断读它，ThreadSanitizer 报告：

```
WARNING: ThreadSanitizer: data race
```

换成 C++20 的 `std::atomic<std::shared_ptr<T>>`，同样的读写，**0 条警告**：

```cpp
std::atomic<std::shared_ptr<const Config>> g_config;
g_config.store(std::make_shared<Config>(...));   // 写
auto snapshot = g_config.load();                  // 读：得到一份自己的副本
```

这正好是项目 `Handler` 做的事（第 9 课）：用 `shared_mutex` 保护一个 `shared_ptr<const ConfigStore>`，读者加共享锁复制一份，写者加独占锁替换。用 `std::atomic<std::shared_ptr>` 可以写得更短，第 18 课会对比两种写法。

### 复制 `shared_ptr` 的代价

引用计数是原子操作，原子操作并不便宜，**多个线程同时修改同一个计数时尤其贵**。实测调用 1000 万次（函数禁止内联）：

```
1 个线程：按值传 shared_ptr ≈ 200 ms   按 const& 传 ≈ 10~15 ms
4 个线程：按值传 shared_ptr ≈ 2700~3200 ms   按 const& 传 ≈ 20 ms
```

单线程时，按值传每次调用多花约 20 ns（一次原子加、一次原子减）。4 个线程同时复制**同一个** `shared_ptr` 时，每次涨到约 300 ns：计数所在的缓存行在 4 个 CPU 核之间来回传递（第 18 课"伪共享"同理）。

**规则：函数只是"用一下"对象时，参数写 `const std::shared_ptr<T>&`，或者干脆写 `const T&`；只有要把所有权存下来时才按值传。**

## 7. 选择指南

```
需要动态分配一个对象？
 ├─ 只有一个主人 ─────────────────────────────▶ unique_ptr（默认选择）
 ├─ 多个主人，生命周期互不相同 ───────────────▶ shared_ptr（用 make_shared 创建）
 │    └─ 其中有"可有可无"的引用，或可能成环 ──▶ 那一边用 weak_ptr
 └─ 只是临时用一下，确定对方活得更久 ─────────▶ 普通指针 T* 或引用 T&（借用，不拥有）

永远不要：手写 delete、对同一个裸指针创建两个 shared_ptr（会 delete 两次）
```

## 面试题

**Q1：`unique_ptr` 和 `shared_ptr` 的区别？各自的开销？**
`unique_ptr` 独占所有权，只能移动，大小和普通指针一样，没有额外开销。`shared_ptr` 共享所有权，有一个堆上的控制块存放强、弱引用计数，大小是两个指针，复制和销毁时要做原子加减。

**Q2：`make_shared` 和 `shared_ptr<T>(new T)` 有什么区别？**
`make_shared` 把对象和控制块放在一次分配里（实测 1 次 vs 2 次），更快、缓存更友好，C++17 之前还能避免异常导致的泄漏。缺点是只要还有 `weak_ptr`，整块内存都不能释放。

**Q3：`shared_ptr` 是线程安全的吗？**
引用计数的增减是原子的，所以多个线程各自持有副本是安全的。但同一个 `shared_ptr` 变量被多个线程同时读写是数据竞争，需要加锁或用 C++20 的 `std::atomic<std::shared_ptr<T>>`。指向的对象本身是否线程安全，和 `shared_ptr` 无关。

**Q4：`shared_ptr` 循环引用怎么解决？**
把环上某一条边改成 `weak_ptr`。`weak_ptr` 不增加强计数，使用前用 `lock()` 换成临时的 `shared_ptr`。另一个思路是重新设计所有权，或者像本项目那样在配置阶段直接禁止成环。

**Q5：`weak_ptr` 的 `lock()` 和 `expired()` 有什么区别？为什么推荐用 `lock()`？**
`expired()` 只告诉你对象此刻是否已经销毁。多线程下，判断完到真正使用之间对象可能被销毁。`lock()` 原子地完成"检查并获取一份强引用"，拿到非空结果就保证对象在使用期间存活。

**Q6：`enable_shared_from_this` 解决什么问题？有什么限制？**
让对象能从 `this` 安全地得到一个与已有 `shared_ptr` 共享控制块的新 `shared_ptr`，常用于异步回调里延长对象生命周期。限制：对象必须已经被 `shared_ptr` 管理，不能在构造函数里调用，否则抛 `std::bad_weak_ptr`。如果直接写 `shared_ptr<T>(this)`，会创建第二个控制块，导致对象被 delete 两次。

**Q7：`unique_ptr` 的自定义删除器会增加大小吗？**
看删除器类型：无状态的函数对象或无捕获 lambda 不增加大小（空基类优化），函数指针删除器会多 8 字节（实测 16 vs 8）。

**Q8：函数参数应该怎么传智能指针？**
只是使用对象：传 `const T&` 或 `T*`。需要共享所有权（会存下来）：按值传 `shared_ptr` 再 `std::move` 进成员。需要转移独占所有权：按值传 `unique_ptr`。避免无意义地按值传 `shared_ptr`：每次多一对原子操作，多线程竞争时代价放大十几倍（实测）。

下一课：[第 15 课：模板、Concepts 与编译期计算](15-templates.md)
