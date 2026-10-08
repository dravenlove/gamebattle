# 第 13 课：特殊成员函数、值类别与移动语义

> 第 2 课讲了 `std::move` 的用法。这一课讲它背后的规则：编译器什么时候替你生成复制和移动、什么时候复制会被省掉、什么时候你以为在移动其实在复制。最后用这些规则解释我在本项目里实测到的一个 **4 倍性能问题**。
>
> 本课所有计数都来自一个 `Tracer` 类型：它的复制构造和移动构造各自给计数器加一，这样就能精确数出发生了几次复制、几次移动。

## 1. 六个特殊成员函数

```cpp
struct T {
    T();                          // 默认构造
    ~T();                         // 析构
    T(const T&);                  // 复制构造
    T& operator=(const T&);       // 复制赋值
    T(T&&) noexcept;              // 移动构造
    T& operator=(T&&) noexcept;   // 移动赋值
};
```

你不写，编译器会在需要时替你生成，生成的版本就是"对每个成员分别做同样的操作"。但生成规则之间有牵连，这是面试和实际 Bug 的高发区：

| 你手动声明了 | 编译器还会自动生成移动操作吗 |
|---|---|
| 什么都没声明 | ✅ 会 |
| 析构函数 | ❌ **不会**（复制操作照样生成，但这种行为已被标为过时） |
| 复制构造或复制赋值 | ❌ 不会 |
| 移动构造或移动赋值 | 复制操作会被**删除** |

第二行最坑。实测一个只多写了空析构函数的类：

```cpp
struct WithDtor   { std::vector<Tracer> events = std::vector<Tracer>(100); ~WithDtor() {} };
struct RuleOfZero { std::vector<Tracer> events = std::vector<Tracer>(100); };

WithDtor   b = std::move(a);   // copies=100 moves=0   ← 你写了 std::move，实际复制了 100 个元素
RuleOfZero d = std::move(c);   // copies=0   moves=0   ← vector 直接交出内部数组
```

没有任何警告。**只是为了打个日志而加了一个空析构函数，这个类就失去了移动能力。**

### 三条规则

- **零法则**（Rule of Zero）：能不写就都不写，让成员（`vector`、`string`、`shared_ptr`……）自己管理资源。**项目里 `engine.hpp` 的所有结构体都遵守零法则**，所以它们都能被高效移动。
- **五法则**（Rule of Five）：如果你必须手写其中一个（通常因为类直接管理一个资源），那就五个都写清楚，或者用 `= default` / `= delete` 显式说明。
- **`= delete`** 禁止某个操作。`practice/thread_pool.hpp` 里：

```cpp
ThreadPool(const ThreadPool&) = delete;              // 线程池不能复制
ThreadPool& operator=(const ThreadPool&) = delete;
```

### 一个完整的五法则例子：文件描述符

`practice/battle_tcp_server.cpp` 的 `FileDescriptor` 直接管理一个系统资源，所以必须自己写：

```cpp
class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    FileDescriptor(const FileDescriptor&) = delete;                  // 不能复制：否则会 close 两次
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}                        // 拿走对方的 fd，把对方置为 -1
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {                                         // 防止自己赋值给自己
            reset();                                                  // 先关掉自己原来的
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~FileDescriptor() { reset(); }
private:
    int fd_{-1};
};
```

- `std::exchange(x, new_value)`：把 `x` 设成新值，返回旧值。写移动操作时很常用。
- 移动之后，源对象的 fd 是 -1，它的析构函数什么也不做，所以资源只会被关闭一次。
- 这种"独占、可移动、不可复制"的资源管理类，就是标准库 `std::unique_ptr` 的思路（第 14 课）。

## 2. 值类别：左值、右值，以及"有名字的右值引用是左值"

C++ 的每个表达式都有一个**值类别**，决定了它能不能被"拿走"：

| 类别 | 直观理解 | 例子 |
|---|---|---|
| 左值（lvalue） | 有名字、有地址、之后还会被用到 | `name`、`units[0]`、`*ptr` |
| 纯右值（prvalue） | 临时的值，用完就没 | `42`、`T{}`、`make_result()` |
| 将亡值（xvalue） | 有名字，但你声明"我不要了" | `std::move(name)` |

后两种合称**右值**。`T&&` 只能绑定到右值，所以重载决议时，右值会选中移动版本。

`std::move` 本身什么都不做，它就是 `static_cast<T&&>(x)`：把一个左值**标记**成将亡值。

最容易错的一点：**右值引用类型的变量，本身是一个左值**，因为它有名字。实测：

```cpp
void take(const std::string&);   // 复制版本
void take(std::string&&);        // 移动版本

void forward_wrong(std::string&& s) { take(s); }              // → take(const std::string&)，复制！
void forward_right(std::string&& s) { take(std::move(s)); }   // → take(std::string&&)，移动
```

`s` 的类型是 `std::string&&`，但表达式 `s` 是左值。参数虽然是"右值引用"，在函数里要再写一次 `std::move` 才能继续移动下去。

## 3. 完美转发：`T&&` 和 `std::forward`

模板里的 `T&&` 不是右值引用，而是**转发引用**（forwarding reference）：传左值进来，`T` 推导成 `X&`；传右值进来，`T` 推导成 `X`。配合 `std::forward<T>`，可以**原样保留**参数的值类别继续往下传：

```cpp
template <typename T>
void forward_generic(T&& s) { take(std::forward<T>(s)); }

forward_generic(name);                    // 左值进来 → take(const std::string&)
forward_generic(std::string("poison"));   // 右值进来 → take(std::string&&)
```

（实测两种情况分别调到了复制和移动版本。）

`practice/thread_pool.hpp` 的 `submit` 就是这么写的：

```cpp
template <typename Function>
auto submit(Function&& function) -> std::future<std::invoke_result_t<Function>> {
    auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Function>(function));
    ...
}
```

传进来的 lambda 如果是临时对象，就被移动进任务里；如果是一个具名变量，就被复制。调用方不用关心。

**规则：`std::move` 用在你确定不再需要的具名对象上；`std::forward` 只用在转发引用 `T&&` 上。**

## 4. `noexcept` 的移动构造：vector 扩容时的大坑

```cpp
std::vector<Tracer> v;
for (int i = 0; i < 1000; ++i) { Tracer t; v.push_back(std::move(t)); }
```

实测两个版本的 `Tracer`，唯一区别是移动构造有没有标 `noexcept`：

```
移动构造是 noexcept    : copies=0    moves=2023
移动构造不是 noexcept  : copies=1023 moves=1000
```

1000 次 `push_back` 本身是 1000 次移动。差别出在扩容：vector 要把旧元素搬到新内存。如果移动构造**可能抛异常**，搬到一半抛出来，旧数组已经被搬空了一部分，就没法恢复原状。为了保证"扩容失败时原 vector 完好无损"（强异常保证，第 10 课），vector 只有在移动构造标了 `noexcept` 时才用移动，否则**退回到复制**。

所以 1023 次扩容搬运全变成了复制。

- **规则：自己写的移动构造和移动赋值，都要标 `noexcept`。**
- 编译器自动生成的移动操作，只要所有成员的移动都是 `noexcept`，它自动就是 `noexcept`。这又是零法则的一个好处。
- 项目里 `RuntimeUnit`、`Event` 都遵守零法则，所以 `units.push_back(std::move(runtime))` 扩容时用的是移动。

## 5. 复制省略：RVO 与 NRVO

```cpp
T make_prvalue() { return T{}; }                              // 返回临时对象
T make_named()   { T result; ...; return result; }            // 返回具名局部变量
T make_moved()   { T result; ...; return std::move(result); } // 画蛇添足
T make_branchy(bool flag) { T a, b; if (flag) return a; return b; }
```

实测：

```
return T{}              : copies=0 moves=0     ← C++17 保证的复制省略
return result（NRVO）    : copies=0 moves=0     ← 编译器直接在调用方的位置构造 result
return std::move(result): copies=0 moves=1     ← 反而多了一次移动
两个候选，无法 NRVO      : copies=0 moves=1     ← 编译器不知道该把谁放在调用方位置，退回移动
```

- **RVO**：返回纯右值时，C++17 起**保证**不复制也不移动，直接在调用方的内存里构造。
- **NRVO**：返回具名局部变量时，编译器**通常会**省略复制（标准允许但不强制）。省不掉时，也会自动当成右值来**移动**，不会复制。
- 写 `return std::move(result);` 反而**阻止了** NRVO。GCC 会警告：`moving a local object in a return statement prevents copy elision [-Wpessimizing-move]`。

对照第 2 课的结论：**返回局部变量不要写 `std::move`；返回成员变量（比如 `BattleState::finish()` 里的 `result`）要写**，因为成员不是局部变量，不适用上面的规则。

## 6. `std::initializer_list` 只能复制

```cpp
std::vector<T> v{T{}, T{}, T{}};                              // copies=3
std::vector<T> v; v.reserve(3); v.emplace_back(); ×3          // copies=0
```

用花括号列表初始化容器时，编译器先在一个隐藏的数组里构造出这 3 个元素，`std::initializer_list` 指向这个数组。问题在于：**这个数组的元素是 `const` 的**，容器只能从里面**复制**，不能移动。即使你写的是临时对象，也逃不过一次复制。

更隐蔽的情况：

```cpp
std::vector<T> inner(1000);
std::vector<std::vector<T>> outer{std::move(inner)};          // copies=1000 ！
```

你明明写了 `std::move(inner)`。它确实把 `inner` 移动进了那个隐藏数组，但随后 `outer` 只能从 `const` 数组里**复制**这个元素，于是 1000 个元素被逐个复制了一遍。

### 真实案例：项目的结果编码慢了 4 倍

`wire.cpp` 的 `encode_result` 正是这样写的：

```cpp
Value::List events;
... // 往 events 里放 880 个事件，每个事件 12 个字段
return Value::object({
    {"battle_id", integer(result.battle_id)},
    ...
    {"events", Value::list(std::move(events))},   // ← 以为移动了
    {"units", Value::list(std::move(units))}
});
```

`Value::object` 接收的是一个花括号列表，于是**整个事件列表（880 个 map、上万个字段）被深复制了一遍**。每个事件内部的 `Value::object({...})` 也一样，12 个字段各被复制一次。

我用 valgrind 的 callgrind 分析了完整链路（第 21 课），排在最前面的是：

```
21.3%   std::vector<std::pair<std::string, Value>> 的复制构造
11.3%   std::vector<Value> 的复制构造
```

修复只需要不用花括号列表，改成先 `reserve`，再逐个 `emplace_back` 移动进去。`practice/encode_bench.cpp` 里的写法：

```cpp
template <typename... Fields>
Value object_of(Fields&&... fields) {
    Value::Object object;
    object.reserve(sizeof...(fields));
    (object.emplace_back(std::forward<Fields>(fields)), ...);   // 折叠表达式，第 15 课
    return Value::object(std::move(object));
}
```

实测：先验证 1000 场战斗的输出**与原实现逐字节相同**，再计时，结果从约 1100 µs/场降到约 600 µs/场，**快了约 1.9 倍**。如果连中间的 `Value` 树都不建、直接写字节，可以快约 4 倍（第 21 课）。

> **规则：花括号列表适合常量和小对象。装大对象、需要移动的场合，用 `reserve` + `emplace_back`。**

## 7. `push_back` 和 `emplace_back`

```cpp
std::vector<std::pair<std::string, T>> v;
v.push_back({"a", t});                         // copies=1 moves=1
v.push_back({"b", std::move(t)});              // copies=0 moves=2
v.emplace_back("c", T{});                      // copies=0 moves=1
v.emplace_back(std::piecewise_construct,
               std::forward_as_tuple("d"),
               std::forward_as_tuple());       // copies=0 moves=0
```

（实测。）

- `push_back(x)` 接收一个**已经构造好的**元素，再复制或移动进容器。
- `emplace_back(args...)` 把参数**转发**给元素的构造函数，**直接在容器的内存里构造**，省掉中间那个临时对象。
- 元素是 `int`、指针这类小东西时，两者没有区别；元素构造代价高时，`emplace_back` 更好。
- 注意：`emplace_back` 会调用 `explicit` 构造函数，`push_back` 不会。所以 `emplace_back` 更"宽松"，类型写错时可能悄悄构造出你不想要的对象。

## 面试题

**Q1：什么是零法则 / 三法则 / 五法则？**
零法则：让成员自己管理资源，类本身不写任何特殊成员函数。三法则（C++98）：析构、复制构造、复制赋值，写了一个就要写全三个。五法则（C++11）：再加上移动构造和移动赋值。

**Q2：只写了析构函数，会有什么后果？**
编译器不再自动生成移动构造和移动赋值，`std::move` 这个对象时会悄悄退回到复制。实测一个含 100 个元素的成员，"移动"变成了 100 次复制，而且没有任何警告。

**Q3：`std::move` 做了什么？**
什么也没做，它只是 `static_cast<T&&>`，把左值转换成将亡值，让重载决议能选中移动版本。真正的"移动"发生在被调用的移动构造或移动赋值里。

**Q4：`std::move` 和 `std::forward` 的区别？**
`std::move` 无条件转换成右值。`std::forward<T>` 有条件：只有当 `T` 推导自右值时才转换，用来在模板里原样转发参数的值类别。`std::forward` 只配合转发引用 `T&&` 使用。

**Q5：移动构造为什么要标 `noexcept`？**
`std::vector` 等容器扩容时，为了保证强异常保证，只有在移动构造不会抛异常时才用移动，否则用复制。不标 `noexcept`，扩容时所有元素都会被复制（实测 1000 次 push_back 产生了 1023 次复制）。

**Q6：什么是 RVO 和 NRVO？`return std::move(local)` 好不好？**
RVO：返回纯右值时直接在调用方内存里构造，C++17 起是强制的。NRVO：返回具名局部变量时编译器可以省略复制，省不掉时也会自动移动。`return std::move(local)` 会阻止 NRVO，多一次移动，GCC 会给出 `-Wpessimizing-move` 警告。

**Q7：`emplace_back` 比 `push_back` 好在哪？**
`emplace_back` 把参数转发给构造函数，直接在容器内存里构造元素，省掉一个临时对象和一次移动（或复制）。代价是它会调用 `explicit` 构造函数，类型检查更宽松。

**Q8：说一个你在项目里遇到的性能问题。**
战斗结果编码占了完整链路 77% 的耗时。用 callgrind 定位到大量时间花在 `vector` 的复制构造上，原因是用 `std::initializer_list` 构造 map 时元素是 `const` 的、只能复制，导致整个事件列表被深复制。改成 `reserve` + `emplace_back` 移动后快了约 1.9 倍；改成直接流式写字节后快了约 4 倍。修改前后用 1000 场战斗的输出逐字节对比，确认结果完全一致。

下一课：[第 14 课：智能指针深入](14-smart-pointers.md)
