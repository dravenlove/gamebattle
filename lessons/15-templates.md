# 第 15 课：模板、Concepts 与编译期计算

> 第 9、10 课已经见过 `checked_int<T>` 和 `checked_enum<Enum>` 这两个函数模板。这一课讲模板的工作方式、C++20 的 concepts，以及"让编译器替你提前算好"的编译期计算。

## 1. 模板是"生成代码的配方"

```cpp
template <typename T>
T checked_int(std::int64_t value, std::string_view path) { ... }

checked_int<std::int32_t>(x, "max_rounds");   // 编译器生成一个 T = int32_t 的版本
checked_int<std::uint32_t>(x, "buff.id");     // 再生成一个 T = uint32_t 的版本
```

模板本身不是函数，而是**生成函数的配方**。编译器在某个 `.cpp` 里看到 `checked_int<std::int32_t>` 被使用时，才会用 `int32_t` 替换 `T`，生成一份真正的函数，这叫**实例化**。

这带来两个直接后果：

### 后果一：模板的定义通常必须写在头文件里

把声明放在头文件、定义放在 `.cpp` 里（实测）：

```
use.cpp:(.text+0xe): undefined reference to `int checked_int<int>(long)'
```

编译 `use.cpp` 时，编译器只看到了声明，没法实例化，只能留一个"等链接时再找"的引用。编译 `checked.cpp` 时，那里又没人用 `checked_int<int>`，所以什么也没生成。结果链接时谁都找不到。

两种解决办法：
- **把定义写进头文件**（最常见）。项目里 `checked_int` 定义在 `wire.cpp` 内部，只在这个文件里用，所以没问题。
- **显式实例化**：在 `.cpp` 末尾写 `template std::int32_t checked_int<std::int32_t>(std::int64_t);`，告诉编译器"在这里生成这个版本"。实测加上这一行后链接成功。适合类型集合已知、又不想把实现暴露在头文件里的情况。

### 后果二：代码膨胀和编译时间

每用一种新类型就多一份代码，模板用得多会让可执行文件变大、编译变慢。`extern template` 可以告诉其他 `.cpp`"这个版本别处已经生成了，你不用再生成"。

## 2. Concepts：给模板参数加约束

C++20 之前，模板对参数类型没有任何约束。传错类型，错误会在模板**内部**深处爆发：

```cpp
template <typename T> T checked_int(std::int64_t value) { ... static_cast<T>(value) ... }
checked_int<std::string>(5);
```

实测：**109 行**报错，第一条是 `invalid 'static_cast' from type 'std::string' to type 'int64_t'`，指向模板内部某一行，看不出是调用方写错了。

加上 concept：

```cpp
template <std::integral T>          // T 必须是整数类型
T checked_int(std::int64_t value) { ... }
```

实测：**16 行**，直接指出 `no matching function for call to 'checked_int<std::string>(int)'` 和 `constraints not satisfied`。错误出现在调用处，一眼就能看懂。

常用的标准 concept（`<concepts>`）：

| concept | 含义 |
|---|---|
| `std::integral<T>` | 整数类型 |
| `std::floating_point<T>` | 浮点类型 |
| `std::same_as<T, U>` | 完全相同的类型 |
| `std::convertible_to<T, U>` | 能转换成 U |
| `std::invocable<F, Args...>` | 能用这些参数调用 |
| `std::ranges::range<T>` | 能用于范围 for |

也可以自己定义：

```cpp
template <typename T>
concept EtfEncodable = requires(const T& value) {
    { encode_to_etf(value) } -> std::same_as<std::vector<std::uint8_t>>;
};

template <EtfEncodable T> void send_reply(const T& value);
```

在 concepts 出现之前，同样的约束要用 **SFINAE**（`std::enable_if`）来写，可读性很差：

```cpp
template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
T checked_int(std::int64_t value);
```

面试可能会问 SFINAE 是什么（"替换失败不是错误"：模板参数替换出错时，编译器只是把这个候选从重载集合里去掉，而不报错）。新代码直接用 concepts。

## 3. 特化：为某些类型单独写一份

```cpp
template <typename T> struct EtfTag { static constexpr const char* name = "unsupported"; };  // 主模板
template <> struct EtfTag<std::int64_t> { static constexpr const char* name = "INTEGER_EXT"; };    // 全特化
template <> struct EtfTag<std::string>  { static constexpr const char* name = "BINARY_EXT"; };
template <typename T> struct EtfTag<T*> { static constexpr const char* name = "pointer: not encodable"; }; // 偏特化
```

实测：`INTEGER_EXT | BINARY_EXT | unsupported | pointer: not encodable`。

- **全特化**：给某一个具体类型单独写实现。
- **偏特化**：给一类类型（这里是"所有指针"）单独写实现。**只有类模板能偏特化**，函数模板不能（函数用重载代替）。
- 标准库的 `std::hash<T>`、`std::numeric_limits<T>` 都是靠特化实现的。第 16 课给自定义类型写哈希，就是去特化 `std::hash`。

## 4. 类型特征与 `if constexpr`

`<type_traits>` 提供了一组在**编译期**查询类型信息的工具：`std::is_signed_v<T>`、`std::is_unsigned_v<T>`、`std::is_same_v<T, U>`……配合 `if constexpr`，可以按类型在编译期选择不同的代码：

```cpp
template <std::integral T>
std::int64_t to_etf_integer(T value) {
    if constexpr (std::is_unsigned_v<T> && sizeof(T) >= sizeof(std::int64_t)) {
        if (value > static_cast<T>(std::numeric_limits<std::int64_t>::max())) {
            throw std::out_of_range("exceeds int64");
        }
    }
    return static_cast<std::int64_t>(value);
}
```

实测：`to_etf_integer(std::uint32_t{7})` 返回 7，`to_etf_integer(std::uint64_t{1} << 63)` 抛出异常。

和普通 `if` 的区别：`if constexpr` 的条件在编译期求值，**不成立的分支根本不会被编译**。对 `uint32_t`，那段溢出检查完全不存在，零开销；而且不成立的分支里即使有对当前类型非法的代码，也不会报错。`wire.cpp` 的 `integer()` 函数（`uint64` 转 ETF 时检查溢出）就可以用这种方式写成一个通用模板。

`static_assert(条件, "消息")` 在编译期检查条件，不满足就编译失败：

```cpp
static_assert(sizeof(Event) <= 128, "Event 变大了，检查一下是否影响性能");
```

## 5. 可变参数模板与折叠表达式

```cpp
template <typename... Ts>                       // Ts 是一"包"类型
auto sum(Ts... values) { return (values + ... + 0); }   // 折叠表达式：v1 + (v2 + (v3 + 0))

template <typename... Ts>
void log_line(const Ts&... parts) { ((std::cout << parts << ' '), ...); std::cout << '\n'; }

sizeof...(Ts)                                   // 包里有几个元素
```

实测：`sum(35, 35, 35) = 105`，`log_line("battle", 3001, "winner", "attacker", "rounds", 19)` 输出 `battle 3001 winner attacker rounds 19`。

这是第 13 课修复编码性能问题时用到的写法（`practice/encode_bench.cpp`）：

```cpp
template <typename... Fields>
Value object_of(Fields&&... fields) {
    Value::Object object;
    object.reserve(sizeof...(fields));                          // 编译期就知道有几个字段
    (object.emplace_back(std::forward<Fields>(fields)), ...);   // 对每个参数执行一次 emplace_back
    return Value::object(std::move(object));
}
```

`Fields&&...` 是一包**转发引用**，`std::forward<Fields>(fields)...` 把每个参数原样转发（第 13 课）。和 `std::initializer_list` 相比，它不要求所有参数类型相同，也不会强制复制。`std::make_shared`、`emplace_back`、`std::thread` 的构造函数内部都是这么实现的。

## 6. `constexpr`：让编译器提前算好

`constexpr` 函数既可以在运行时调用，也可以在编译期调用。用它生成一张编译期的查找表：

```cpp
constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < 256; ++index) {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) value = (value & 1U) ? (value >> 1U) ^ 0xEDB88320U : value >> 1U;
        table[index] = value;
    }
    return table;
}
inline constexpr auto kCrcTable = make_crc_table();      // 编译时就算好，存进只读数据段

static_assert(crc32_table("123456789") == 0xCBF43926U);  // CRC-32 的标准校验值：算法写错就编译不过
```

第 10 课讲过，项目的 CRC32 是**逐位**计算的：每个字节循环 8 次。查表法每个字节只查一次表。实测 16 MB 数据：

```
两种算法结果相同: 1
16 MB 逐位计算: 182 ms
16 MB 查表计算: 42 ms
```

快了约 4.3 倍，查找表在编译期生成，运行时零初始化成本。

实事求是地说：当前示例配置只有 413 字节，这个优化**对它毫无意义**。但配置包允许最大 64 MB，按逐位算法要花约 0.7 秒，热更新期间这段时间都在占用 CPU。在面试中，"知道什么时候值得优化、什么时候不值得"和"知道怎么优化"同样重要。

- `consteval`（C++20）：函数**只能**在编译期调用。
- `constinit`（C++20）：保证全局变量在编译期完成初始化，避免"静态初始化顺序问题"。

## 7. CRTP：编译期多态

第 12 课提到，除了虚函数，还可以用模板实现多态。**CRTP**（奇异递归模板模式）让基类以子类自身作为模板参数：

```cpp
template <typename Derived>
struct EventSink {
    void on_damage(std::uint64_t target, std::int64_t value) {
        static_cast<Derived*>(this)->handle_damage(target, value);   // 编译期就知道调用谁
    }
};
struct DamageMeter : EventSink<DamageMeter> {
    std::int64_t total = 0;
    void handle_damage(std::uint64_t, std::int64_t value) { total += value; }
};
struct Logger : EventSink<Logger> {
    void handle_damage(std::uint64_t target, std::int64_t value) { std::cout << "damage " << target << " -" << value << '\n'; }
};
```

实测：`DamageMeter total = 265  sizeof(DamageMeter) = 8（没有 vptr）`。

- 没有虚表，`on_damage` 可以完全内联。
- 代价：`EventSink<DamageMeter>` 和 `EventSink<Logger>` 是两个**不相关**的类型，不能放进同一个容器里统一处理。所以 CRTP 适合"编译期就确定用哪种实现"的场景，比如战报统计、策略类。运行时才能确定的场景，还是用虚函数或 `variant`。

## 小结

| 工具 | 什么时候用 |
|---|---|
| 函数模板 / 类模板 | 同一套逻辑适用于多种类型 |
| concepts | 约束模板参数，让报错出现在调用处（实测 109 行 → 16 行） |
| 特化 / 偏特化 | 某些类型需要不同的实现 |
| `if constexpr` + type traits | 在一个函数里按类型选择不同代码，不成立的分支零开销 |
| 可变参数 + 折叠表达式 | 参数个数不定，又要保留各自的类型和值类别 |
| `constexpr` / `static_assert` | 能在编译期算的提前算好，能在编译期查的提前查出来 |
| CRTP | 编译期确定的多态，不要虚表开销 |

## 面试题

**Q1：模板为什么一般要写在头文件里？**
模板在使用处实例化，编译器必须在那个翻译单元里看到完整定义。定义放在 `.cpp` 里，其他文件只能生成一个未解析的引用，链接时找不到（实测报 `undefined reference`）。可以用显式实例化解决，但前提是类型集合已知。

**Q2：什么是 SFINAE？C++20 用什么代替它？**
"替换失败不是错误"：模板参数替换导致的错误不会报错，只是让这个候选从重载集合中移除，常配合 `std::enable_if` 按类型选择重载。C++20 的 concepts 和 `requires` 子句表达同样的约束，可读性好得多，报错也更清楚。

**Q3：全特化和偏特化的区别？函数模板能偏特化吗？**
全特化为一组完全确定的模板参数提供实现，偏特化为满足某种模式的一类参数（比如所有指针类型）提供实现。函数模板只能全特化，不能偏特化，应该用重载来代替。

**Q4：`if constexpr` 和普通 `if` 有什么区别？**
`if constexpr` 的条件必须是编译期常量，不成立的分支不会被实例化：没有运行时开销，而且里面即使有对当前类型非法的代码也不会报错。

**Q5：`constexpr`、`consteval`、`constinit` 的区别？**
`constexpr` 函数可以在编译期或运行时调用，`constexpr` 变量必须在编译期初始化且之后不可修改。`consteval` 函数只能在编译期调用。`constinit` 只要求变量在编译期初始化，之后仍然可以修改，用来避免静态初始化顺序问题。

**Q6：CRTP 是什么？和虚函数相比有什么优缺点？**
基类模板以子类作为模板参数，通过 `static_cast<Derived*>(this)` 在编译期调用子类实现。优点：没有虚表指针，可以内联。缺点：不同子类的基类是不同类型，不能放进同一个容器做运行时多态，而且模板代码会膨胀。

**Q7：可变参数模板和 `std::initializer_list` 有什么区别？**
`initializer_list` 要求所有元素类型相同，元素是 `const` 的，只能复制。可变参数模板的每个参数可以是不同类型，配合转发引用和 `std::forward` 可以保留值类别、实现移动。

下一课：[第 16 课：STL 容器内部与迭代器失效](16-stl-internals.md)
