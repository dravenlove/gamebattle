# 第 9 课：对接 Erlang

**中文** | [English](en/09-erlang-bridge.md)

> 对应文件：
> - Erlang 侧：`erlang/src/gamebattle_port.erl`、`gamebattle_nif.erl`、`gamebattle_sup.erl`、`gamebattle.erl`
> - C++ 侧：`src/port_main.cpp`、`src/nif.cpp`、`src/term.cpp`（ETF 编解码）、`src/wire.cpp`（ETF ↔ 战斗结构）

前 8 课讲的都是"战斗怎么算"。这一课讲"请求怎么从 Erlang 进来，结果怎么回去"。这里是你最熟悉的一侧（Erlang）和 C++ 交界的地方。

## 1. 全景：两条通道，一个处理函数

```
                    ┌──────────── Port 通道（推荐生产）────────────┐
gamebattle:simulate(port, Req)                                    │
  → gamebattle_port (gen_server)                                  │
  → port_command(Port, term_to_binary(Req))                       │
  → [4 字节长度][ETF 字节] ──stdin──▶ gamebattle_port 进程（port_main.cpp）
                                          │
                                          ▼
                              wire::handle_etf(字节) ──▶ Engine::simulate
                                          ▲
gamebattle:simulate(nif, Req)             │
  → gamebattle_nif:simulate(Req)          │
  → nif.cpp: enif_term_to_binary ─────────┘   （在 BEAM 进程内部，dirty scheduler 线程上）
                    └──────────── NIF 通道（可选）──────────────────┘
```

两条通道最终都调用同一个 `wire::handle_etf`：**输入是 ETF 字节，输出也是 ETF 字节**。所以 Port 和 NIF 不会算出两套结果，README 里的 `true = (Result1 =:= Result2)` 就是这么保证的。

## 2. Erlang 侧：`gamebattle_port` 是怎么管住 C++ 进程的

```erlang
init(Options) ->
    process_flag(trap_exit, true),
    ...
    Port = open_port({spawn_executable, filename:absname(Executable)},
                     [binary, {packet, 4}, use_stdio, exit_status]),
    {ok, #state{port = Port, executable = Executable, timeout = Timeout}}.

handle_call({request, Request}, _From, State = #state{port = Port, timeout = Timeout}) ->
    true = port_command(Port, term_to_binary(Request)),
    receive
        {Port, {data, ResponseBinary}} ->
            {reply, decode_response(ResponseBinary), State};
        {Port, {exit_status, Status}} ->
            {stop, {port_exit, Status}, {error, #{type => port_exit, status => Status}}, State};
        {'EXIT', Port, Reason} ->
            {stop, {port_exit, Reason}, {error, #{type => port_exit, reason => Reason}}, State}
    after Timeout ->
        close_port_safely(Port),
        {stop, port_timeout, {error, #{type => timeout, timeout_ms => Timeout}}, State}
    end;
```

这段你应该很熟，几个设计要点：

| 设计 | 作用 |
|---|---|
| `{packet, 4}` | 每条消息前加 4 字节大端长度头，C++ 必须按同样格式读写 |
| `exit_status` | C++ 进程退出时，Erlang 收到 `{Port, {exit_status, N}}`。N 就是 `main` 的返回值 |
| `handle_call` 里同步 `receive` | **一个 worker 同一时间只处理一场战斗**。这是刻意的背压，不让多个请求争用同一个 Port 的响应 |
| `after Timeout` | C++ 死循环或卡住时，关掉 Port 并 `stop`，监督者重启出一个全新的 C++ 进程 |
| `gamebattle_sup`：`one_for_one`，10 秒内最多重启 5 次 | 偶发崩溃自动恢复；频繁崩溃则让上层知道 |

需要并发时，README 的建议是起多个 worker、按 `battle_id` 分片，而不是让一个 worker 同时处理多个请求。

**C++ 崩溃在这里只是一次普通的进程退出**，这正是推荐 Port 的根本原因（第 8 课）。

## 3. `{packet, 4}` 在 C++ 这边：`port_main.cpp`

我用一个小 Python 脚本模拟 Erlang，直接和编译出来的 `gamebattle_port` 对话（实测，十六进制）：

```
--- ping
  发送: 00 00 00 07 83 77 04 70 69 6e 67
        └─长度 7─┘  └──── term_to_binary(ping) ────┘
  收到: 长度头=13  内容: 83 68 02 77 02 6f 6b 77 04 70 6f 6e 67      → {ok, pong}
--- 非法请求
  发送: 00 00 00 02 83 ff
  收到: 长度头=76  内容: 83 68 02 77 05 65 72 72 6f 72 74 00 00 00 02 ...
        错误信息里可读的部分: unsupported ETF tag: 255
stdin 关闭后 Port 退出码 = 0
```

`main` 就是一个死循环：读 4 字节头 → 读正文 → 处理 → 写回：

```cpp
int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);    // ①
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    while (true) {
        std::array<std::uint8_t, 4> header{};
        if (!read_exact(header)) {
            return std::cin.eof() ? 0 : 2;     // Erlang 关闭 Port → stdin EOF → 正常退出
        }
        const auto length = (static_cast<std::uint32_t>(header[0]) << 24U) |   // ② 大端拼装
                            (static_cast<std::uint32_t>(header[1]) << 16U) |
                            (static_cast<std::uint32_t>(header[2]) << 8U) |
                            static_cast<std::uint32_t>(header[3]);
        if (length == 0 || length > kMaxPacketBytes) {    // ③ 64 MB 上限
            std::cerr << "invalid port packet length: " << length << '\n';
            return 3;
        }
        std::vector<std::uint8_t> request(length);
        if (!read_exact(request)) return 4;
        const auto response = gamebattle::wire::handle_etf(request);
        if (!write_packet(response)) return 5;
    }
}
```

### ① Windows 必须切换到二进制模式

Windows 的标准输入输出默认是"文本模式"，会把 `\n`（0x0A）和 `\r\n` 互相转换。ETF 字节里随时可能出现 0x0A，一转换就坏了。`#ifdef _WIN32` 是**条件编译**：只在 Windows 上编译这几行，Linux 上直接跳过。

### ② 字节序：为什么要手动拼，而不是直接读成 uint32

```
本机内存里的 7      : 07 00 00 00  (小端机器)
移位写出的大端 7    : 00 00 00 07
```

x86 和 ARM 都是**小端**：低位字节存在前面。`{packet, 4}` 规定的是**大端**：高位字节在前（网络字节序）。用移位拼装，结果**与运行在什么机器上无关**。这和 Erlang 的 `<<Length:32/big>>` 是同一件事，只不过 Erlang 帮你写好了，C++ 要自己写。

### ③ 不信任任何外部输入

长度头是外部给的，如果有人发来 `FF FF FF FF`，不检查就会尝试申请 4 GB 内存。返回码 0/2/3/4/5 会作为 `exit_status` 出现在 Erlang 那边，方便定位是哪一步出的错。

### `std::span` 和 `reinterpret_cast`

```cpp
bool read_exact(std::span<std::uint8_t> destination) {
    std::cin.read(reinterpret_cast<char*>(destination.data()),
                  static_cast<std::streamsize>(destination.size()));
    ...
}
```

- **`std::span<T>`**（C++20）是一段连续内存的**视图**：一个指针加一个长度，自己不拥有数据，不复制。`std::array`、`std::vector` 都能隐式转换成 span（第 3 课讲的"含义不变的隐式构造"），所以 `read_exact(header)` 和 `read_exact(request)` 能共用一个函数。它是借用，所以**不能比被借的数组活得久**。
- **`reinterpret_cast<char*>`**：`std::cin.read` 只接受 `char*`，而我们的缓冲区是 `uint8_t*`。`reinterpret_cast` 表示"把这块内存当成另一种类型来看"。一般来说这样做很危险，但把任意对象当成 `char` / `unsigned char` / `std::byte` 来读写，是标准**明确允许**的例外。项目里的 `reinterpret_cast` 都只用于这种"按字节看"的场合。

### 一条铁律：Port 进程里不要往 stdout 打印任何东西

`stdout` 就是协议通道。如果你调试时随手写了 `std::cout << "hello"`，Erlang 会把 `hell` 这 4 个字节当成长度头（0x68656c6c，约 17 亿），然后一直等那么多数据，最后超时。**调试信息一律写 `std::cerr`**，项目里就是这么做的。`write_packet` 最后的 `std::cout.flush()` 也不能省，否则数据可能卡在缓冲区里，Erlang 永远收不到。

## 4. ETF：`term_to_binary` 产出的字节长什么样

ETF（External Term Format）是 Erlang 自己的二进制格式。第一个字节固定是版本号 131（0x83），之后每个值都是"1 字节 tag + 内容"。`term.cpp` 支持的 tag：

| tag | 十进制 | 含义 | 例子 |
|---|---|---|---|
| `0x83` | 131 | 版本号（只在开头出现一次） | |
| `0x61` | 97 | 0~255 的小整数，1 字节 | `42` |
| `0x62` | 98 | 32 位有符号整数，4 字节大端 | `-9` |
| `0x6e` / `0x6f` | 110 / 111 | 大整数（bignum） | `5000000000` |
| `0x46` | 70 | 64 位浮点数 | `1.5` |
| `0x77` / `0x76` | 119 / 118 | UTF-8 原子（短 / 长） | `ok` |
| `0x6d` | 109 | binary，4 字节长度 + 内容 | `<<"中毒">>` |
| `0x68` / `0x69` | 104 / 105 | 元组（短 / 长） | `{ok, pong}` |
| `0x6a` | 106 | 空列表 `[]` | |
| `0x6c` | 108 | 列表，4 字节个数 + 元素 + 尾部 | `[a, b]` |
| `0x6b` | 107 | 字节列表（Erlang 把全是 0~255 整数的列表编码成这样） | `[1,2,3]`、`"abc"` |
| `0x74` | 116 | map，4 字节个数 + 键值对 | `#{a => 1}` |

拿上面实测的响应对照一下：

```
83          版本 131
68 02       元组，2 个元素
77 02 6f 6b   原子，长度 2，"ok"
77 04 70 6f 6e 67   原子，长度 4，"pong"
→ {ok, pong}
```

注意 0x6b 这一行：`term_to_binary([1,2,3])` 产出的**不是**列表格式，而是"字节串"格式。如果 C++ 只实现了 0x6c，Erlang 传来的小整数列表就会解析失败。`term.cpp:130` 专门处理了它。这种细节只有对 Erlang 足够熟悉才会想到。

## 5. `term::Value`：用 `std::variant` 表示任意 Erlang term

Erlang 的 term 是动态类型：一个变量可能是整数、原子、列表、元组……C++ 是静态类型，要表示"可能是这几种之一"，用的是 **`std::variant`**（`term.hpp:25-51`）：

```cpp
struct Value {
    using List = std::vector<Value>;
    using Tuple = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;

    struct ListValue { List value; };
    struct TupleValue { Tuple value; };
    struct ObjectValue { Object value; };

    using Storage = std::variant<std::int64_t, double, Atom, Binary, ListValue, TupleValue, ObjectValue>;
    Storage data;

    Value() : data(Atom{"undefined"}) {}        // 默认值是原子 undefined，很 Erlang
    explicit Value(std::int64_t value) : data(value) {}
    ...
};
```

### `std::variant` 是什么

`std::variant<A, B, C>` 在任一时刻**只装着其中一种类型的值**，并且自己记得当前是哪一种。它就是 C++ 版的"带标签的联合"，本质上和 Erlang term 内部的类型标签是一回事。

```cpp
Storage data = Atom{"hero"};
data.index()                         // 2：当前装的是第 3 种类型（Atom）
std::get_if<Atom>(&data)             // 返回指向 Atom 的指针
std::get_if<std::int64_t>(&data)     // 类型不对，返回 nullptr
std::get<std::int64_t>(data)         // 类型不对，抛 std::bad_variant_access
```

（以上全部实测。）

**`std::get_if` 就是 C++ 版的模式匹配**：

```cpp
if (const auto* atom = std::get_if<Atom>(&value.data)) {
    return atom->value;
}
if (const auto* binary = std::get_if<Binary>(&value.data)) {
    return std::string(binary->value.begin(), binary->value.end());
}
type_error(path, "an atom or binary");
```

对应 Erlang 的：

```erlang
as_string(A) when is_atom(A) -> atom_to_list(A);
as_string(B) when is_binary(B) -> binary_to_list(B);
as_string(_) -> error(badarg).
```

`if (const auto* atom = ...)` 是在 if 的条件里**声明变量**：指针非空才进入分支，而且 `atom` 只在这个分支里可见。

### 为什么要套一层 `ListValue` / `TupleValue`

`List` 和 `Tuple` 都是 `std::vector<Value>`，**是同一个类型**。如果直接放进 variant，`std::get_if<std::vector<Value>>` 就分不清你要的是列表还是元组。套一层不同名字的 struct，就变成了两种不同的类型。Erlang 里列表和元组天然是两种东西，C++ 要靠这种方式造出区别。

### 递归类型：`Value` 里装着 `vector<Value>`

`Value` 定义到一半时，它还是**不完整类型**（第 1 课第 8 节）。能在里面写 `std::vector<Value>`，是因为 C++17 起标准明确允许 `vector` 的元素类型在声明时不完整。`vector` 内部只存一个指向堆内存的指针，大小是固定的，这和第 1 课"用指针打破循环"是同一个道理。

### map 为什么用 `vector<pair>` 而不是 `std::map`

```cpp
using Object = std::vector<std::pair<std::string, Value>>;
```

- **保持插入顺序**：编码结果时，字段按写入顺序输出，同一个结果每次编码出的字节完全一样。测试里 `first_bytes == second_bytes`（`engine_test.cpp:574`）比较的就是这个。
- 请求里的 map 通常只有十几个键，线性查找比哈希更快。

### `std::visit`：根据当前类型自动选择处理函数

编码时（`term.cpp:251`）：

```cpp
std::visit([&](const auto& item) { write_item(item, depth); }, value.data);
```

配合一组同名的重载函数：

```cpp
void write_item(std::int64_t value, std::size_t);
void write_item(double value, std::size_t);
void write_item(const Atom& atom, std::size_t);
void write_item(const Value::ListValue& list, std::size_t depth);
...
```

- `std::visit` 取出 variant 里当前的值，交给 lambda。
- 参数写成 `const auto& item` 的 lambda 叫**泛型 lambda**：编译器会为 variant 里的**每一种**类型各生成一个版本。
- 每个版本里调用 `write_item(item, ...)`，编译器按 `item` 的实际类型挑选对应的重载。

效果就是"按类型分派"，相当于 Erlang 里一个按类型写了多个子句的函数。如果漏写了某种类型的 `write_item`，**编译失败**。

### 为什么到处都是 `static_cast<std::int64_t>`

`wire.cpp` 里有大量这种写法：

```cpp
{"seq", Value(static_cast<std::int64_t>(event.seq))},
```

`event.seq` 是 `uint32_t`，而 `Value` 有 `Value(std::int64_t)` 和 `Value(double)` 两个构造函数。`uint32_t` 转成哪个都需要一次转换，编译器分不出谁更好（实测）：

```
error: call of overloaded 'Value(uint32_t&)' is ambiguous
```

显式转换成 `int64_t` 才能消除歧义。这类报错在 C++ 里叫**重载决议有歧义**，碰到时就显式写出你想要的类型。

## 6. 解码器 `Reader`：处处防御

```cpp
class Reader {
    std::span<const std::uint8_t> bytes_;    // 借用整个输入，不复制
    std::size_t offset_{0};                  // 读到哪儿了

    void require(std::size_t count) const {
        if (count > bytes_.size() - offset_) {    // ← 注意写法
            throw DecodeError("truncated ETF value");
        }
    }
};
```

- **每读一个字节之前都先 `require`**，保证不会读出边界。
- 写成 `count > size - offset` 而不是 `offset + count > size`：后者在 `count` 很大时会**溢出绕回**，检查就失效了（第 7 课"判断条件本身也不能溢出"）。
- **深度上限 128**、**容器元素上限 100 万**：防止恶意输入。比如一个列表声称有 40 亿个元素，`check_count` 会在 `reserve` 之前拦下，不会去申请几十 GB 内存。

### Erlang 的大整数在这里被拒绝

```cpp
Value read_big(std::uint32_t count) {
    if (count > sizeof(std::uint64_t)) throw DecodeError("integer does not fit into 64 bits");
    const auto sign = u8();
    std::uint64_t magnitude = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        magnitude |= static_cast<std::uint64_t>(u8()) << (index * 8U);   // 注意：这里是小端
    }
    if (sign == 0) {
        if (magnitude > INT64_MAX) throw DecodeError("positive integer does not fit into int64");
        return Value(static_cast<std::int64_t>(magnitude));
    }
    ...
    if (magnitude == (std::uint64_t{1} << 63U)) {
        return Value(std::numeric_limits<std::int64_t>::min());     // ← 特殊处理
    }
    return Value(-static_cast<std::int64_t>(magnitude));
}
```

- Erlang 的整数没有上限，C++ 只能装 int64。超出的直接拒绝，返回 `invalid_request`。这是第 7 课"三道防线"之前的**第零道**。
- bignum 的数字部分是**小端**，而 ETF 的长度字段都是大端。同一个格式里两种字节序都有，这是 ETF 的历史设计。
- 负数最小值 `-2^63` 要特殊处理：`2^63` 本身装不进 int64，先转换再取负就是 UB。

### 浮点数：`std::bit_cast`

```cpp
case kNewFloat: {
    const auto bits = u64();
    return Value(std::bit_cast<double>(bits));
}
```

把 64 个比特原样解释成 `double`（实测 `bit_cast<uint64>(1.5) = 0x3ff8000000000000`）。C++20 之前常见的写法是指针强转或 union，那两种都是 UB；`std::bit_cast` 是标准提供的安全做法。

### `[[noreturn]]`

```cpp
[[noreturn]] void type_error(std::string_view path, std::string_view expected) {
    throw DecodeError(...);
}

const Value::List& as_list(const Value& value, std::string_view path) {
    if (const auto* list = std::get_if<Value::ListValue>(&value.data)) {
        return list->value;
    }
    type_error(path, "a list");      // 后面没有 return，也不会警告
}
```

`[[noreturn]]` 告诉编译器"这个函数永远不会正常返回"，所以 `as_list` 最后不写 `return` 也不会报"控制流到达函数末尾"的警告。

## 7. `wire.cpp`：ETF 的 Value ↔ 战斗结构体

### 函数模板 `checked_int<T>`

```cpp
template <typename T>
T checked_int(std::int64_t value, std::string_view path) {
    if (value < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
        value > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
        throw term::DecodeError(std::string(path) + " is outside the supported integer range");
    }
    return static_cast<T>(value);
}

request.max_rounds = checked_int<std::int32_t>(term::get_int(value, "max_rounds", 50), "max_rounds");
```

这是课程里第一次正式出现**模板**。`template <typename T>` 表示 `T` 是一个**编译期的类型参数**：写 `checked_int<std::int32_t>(...)` 时，编译器把 `T` 换成 `int32_t`，生成一个专门的版本；写 `checked_int<BasisPoints>(...)` 又生成另一个。

Erlang 的函数天然是"泛型"的，因为它是动态类型。C++ 用模板实现"写一次，按类型各生成一份"，而且每一份都有完整的类型检查。

### `std::string_view`

```cpp
T checked_int(std::int64_t value, std::string_view path)
```

`string_view` 是字符串的**视图**，和 `span` 一样只借用、不拥有、不复制。用来传字段名这类只读字符串，比 `const std::string&` 更灵活（字面量传进来不需要先构造 `std::string`）。同样的规则：**不能比它借用的字符串活得久**。项目里传给它的基本都是字面量，字面量在整个程序运行期间都存在，所以安全。

### 严格的 schema：拒绝未知字段

```cpp
void require_only_fields(const Value& value, std::string_view path,
                         std::initializer_list<std::string_view> supported) { ... }

require_only_fields(value, "buff.modifier", {"attribute", "operation", "value"});
```

Erlang 的 map 很宽松，多一个字段通常没人管。但如果策划把 `max_stacks` 写成了 `max_stack`，宽松的解析会悄悄使用默认值 1，Bug 很难查。这里**直接拒绝未知字段**，错误信息会写明 `contains unsupported field 'max_stack'`。

`std::initializer_list<T>` 让函数可以直接接收 `{"a", "b", "c"}` 这样的花括号列表。

### 结果编码：枚举变原子，消息变 binary

```cpp
{"winner", Value::atom(winner_name(result.winner))},     // 枚举 → 原子
{"message", Value::binary(message)}                       // 文本 → binary
```

```cpp
Value integer(std::uint64_t value) {
    if (value > INT64_MAX) throw std::runtime_error("result integer exceeds signed 64-bit ETF adapter limit");
    return Value(static_cast<std::int64_t>(value));
}
```

单位 ID 在 C++ 里是 `uint64_t`，但 `term::Value` 只支持 int64。超出范围的不会悄悄变成负数，而是抛出 `internal_error`。

## 8. `Handler`：线程安全的配置热切换

```cpp
class Handler final {
public:
    std::vector<std::uint8_t> handle_etf(std::span<const std::uint8_t> request);
private:
    mutable std::shared_mutex config_mutex_;
    std::shared_ptr<const ConfigStore> configs_;
};

std::vector<std::uint8_t> handle_etf(std::span<const std::uint8_t> request) {
    static Handler handler;            // ← 函数内的 static 对象
    return handler.handle_etf(request);
}
```

### 函数内的 `static`

`static Handler handler;` 在**第一次调用时**构造，之后一直存在到程序退出，所有调用共用这一个对象。C++11 起标准保证：即使多个线程同时第一次调用，它也只会被构造一次。这相当于一个惰性初始化的单例，有点像 Erlang 里一个全局注册的进程，或者 `persistent_term`。

### 读写锁：很多读者，一个写者

```cpp
// 每场战斗（读者）：
std::shared_ptr<const ConfigStore> configs;
{
    std::shared_lock lock(config_mutex_);   // 共享锁：多个读者可以同时持有
    configs = configs_;                      // 只复制一个 shared_ptr，瞬间完成
}                                            // 离开花括号，锁自动释放（RAII）
const BattleRequest battle = parse_request(decoded, configs.get());   // 在锁外慢慢用
```

```cpp
// load_config（写者）：
auto next = std::make_shared<ConfigStore>(ConfigStore::load_file(path));   // 在锁外读文件、校验（慢）
{
    std::unique_lock lock(config_mutex_);   // 独占锁：等所有读者离开
    configs_ = next;                         // 只换一个指针，瞬间完成
}
```

几个关键点：

- **`std::shared_lock`**：多个线程可以同时持有；**`std::unique_lock`**：只能一个线程持有，而且要等所有共享锁释放。
- **只在锁里做最少的事**：读者只复制一个指针，写者只交换一个指针。读文件、解析、整场战斗都在锁外进行。
- **单独的一对花括号 `{ }`** 用来限定锁的作用范围：离开花括号，锁对象被析构，锁自动释放。这又是 RAII。
- **已经开始的战斗不受影响**：它们手里握着旧配置的 `shared_ptr`，旧配置要等它们全部结束才会被释放（第 2 课第 3 节）。
- `load_file` 失败会抛异常，`configs_ = next` 那一行根本不会执行，**旧配置继续服务**。
- `mutable` 表示"即使在 const 成员函数里也允许修改"，互斥锁通常都声明成 `mutable`，因为加锁解锁不算修改对象的逻辑状态。

Port 模式下只有一个线程，这把锁没有竞争；它真正发挥作用的是 NIF 模式。

## 9. NIF：`nif.cpp`

```cpp
ERL_NIF_TERM dispatch(ErlNifEnv* env, ERL_NIF_TERM request_term) {
    ErlNifBinary request{};
    if (!enif_term_to_binary(env, request_term, &request)) {      // ① term → ETF 字节
        return enif_make_badarg(env);
    }
    const auto response = gamebattle::wire::handle_etf(            // ② 和 Port 完全同一条路径
        std::span<const std::uint8_t>(request.data, request.size));
    enif_release_binary(&request);                                 // ③ 手动释放

    ERL_NIF_TERM result;
    if (enif_binary_to_term(env, response.data(), response.size(), &result, 0) == 0) {
        return enif_make_tuple2(env, enif_make_atom(env, "error"),
                                enif_make_atom(env, "response_decode_failed"));
    }
    return result;                                                 // ④ ETF 字节 → term
}

ErlNifFunc functions[] = {
    {"simulate", 1, simulate, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"load_config", 1, load_config, ERL_NIF_DIRTY_JOB_IO_BOUND}
};

ERL_NIF_INIT(gamebattle_nif, functions, nullptr, nullptr, nullptr, nullptr)
```

Erlang 侧：

```erlang
-module(gamebattle_nif).
-on_load(init/0).

init() ->
    erlang:load_nif(filename:join(resolve_priv_dir(), "gamebattle_nif"), 0).

simulate(_Request) ->
    erlang:nif_error(nif_not_loaded).      % 加载成功后被 C++ 实现替换
```

### ① ④ 为什么 NIF 还要转一圈 ETF

NIF 本可以直接用 `enif_get_map_value` 之类的 API 逐个读取 term，省掉编码解码。项目故意不这么做：先把 term 转成 ETF 字节，走和 Port **完全相同**的 `handle_etf`，再把结果字节转回 term。多花一点序列化开销，换来"只有一套解析代码、两种模式结果必然一致"。

### ③ C API 没有 RAII

`enif_release_binary` 必须手动调用，忘了就泄漏。如果 `handle_etf` 抛出异常，这一行就会被跳过。之所以没出问题，是因为 `handle_etf` 内部用 `catch (...)` 兜住了所有异常（第 8 课）。更稳妥的做法是写一个小的 RAII 包装类，在析构函数里调用 `enif_release_binary`。这是 C 风格 API 和现代 C++ 交界处的常见问题。

### `ERL_NIF_DIRTY_JOB_CPU_BOUND`：为什么战斗必须跑在 dirty scheduler 上

BEAM 的普通调度器线程靠"每个进程执行一小段就让出"来实现公平调度。普通 NIF 调用期间**无法被打断**，官方建议普通 NIF 在大约 1 毫秒内返回。

一场战斗可能要算几百回合、上万个事件，远超 1 毫秒。如果放在普通调度器上：

- 这个调度器线程被整个占住，排在它上面的所有 Erlang 进程都得等；
- 表现出来就是整个节点的延迟抖动，消息处理变慢、心跳超时等。

标记成 `DIRTY_JOB_CPU_BOUND` 后，BEAM 会把这次调用交给**专门的 dirty CPU 调度器线程池**（默认数量与普通调度器相同，通常就是逻辑 CPU 数），普通调度器继续正常工作。`load_config` 要读磁盘文件，所以标成 `IO_BOUND`，交给 dirty IO 线程池。

### NIF 的并发

多个 Erlang 进程可以同时调用 `gamebattle_nif:simulate/1`，这些调用会在**不同的 dirty 线程上并行执行**，共用同一个 `static Handler`。所以：

- `Engine` 必须没有状态（第 1 课：`simulate` 是 `const` 成员函数）；
- 共享的配置必须是只读的 `shared_ptr<const ConfigStore>`，切换时加读写锁（第 8 节）。

另外，Port 和 NIF 各自运行在不同的操作系统进程里，**各有一份独立的配置**。这就是 README 说"NIF 使用独立的进程内配置仓库，需要单独加载"的原因。

## 10. Port 与 NIF 对比

| | Port | NIF |
|---|---|---|
| C++ 运行在哪里 | 独立的操作系统进程 | BEAM 进程内部 |
| C++ 崩溃或异常泄漏 | Port 进程退出，监督者重启 | **整个 Erlang 节点崩溃** |
| C++ 死循环 | `after Timeout` 关掉 Port，重启 | 一个 dirty 线程被永久占住，无法从 Erlang 侧杀掉 |
| 调用开销 | 管道读写 + ETF 编解码 | 只有 ETF 编解码，没有进程间通信 |
| 并发 | 一个 worker 串行处理，靠多个 worker 并发 | dirty 线程池天然并行 |
| 部署要求 | 一个可执行文件 | 必须用和生产环境**同一个 OTP 主版本**的 `erl_nif.h` 编译 |
| 调试 | 可以单独启动、单独附加调试器 | 要附加到整个 BEAM 进程上 |

结论和 README 一致：**生产默认用 Port**；NIF 只在充分压测、模糊测试、确实需要省掉进程间通信开销时才启用。

## 小结

| 概念 | 要点 | Erlang 里对应的东西 |
|---|---|---|
| `{packet, 4}` | 4 字节大端长度头，用移位拼装，与机器字节序无关 | `<<Len:32/big, Data/binary>>` |
| stdout 是协议通道 | 调试信息只能写 stderr，写完要 flush | 无 |
| `#ifdef _WIN32` | 条件编译，Windows 要切二进制模式 | `-ifdef` |
| `std::span` / `std::string_view` | 只借用、不拥有、不复制的视图 | 子 binary 引用 |
| `reinterpret_cast<char*>` | 只用于"按字节看"这种标准允许的场合 | 无 |
| ETF tag | 版本 131 + "tag + 内容"；小整数列表编码成 0x6b | `term_to_binary/1` |
| `std::variant` | 带类型标签的联合，C++ 版的动态类型值 | Erlang term 本身 |
| `std::get_if` | 类型匹配就返回指针，否则 nullptr | 带 guard 的函数子句 |
| `std::visit` + 泛型 lambda | 按当前类型自动分派到重载函数 | 多子句函数 |
| 重载歧义 | `uint32_t` 传给 `int64_t`/`double` 两个构造函数 → 显式转换 | 无 |
| 函数模板 | `template <typename T>`，按类型各生成一份 | 函数天然泛型 |
| 函数内 `static` | 第一次调用时构造，线程安全，程序结束时销毁 | 注册进程 / `persistent_term` |
| 读写锁 | 锁里只换指针，重活在锁外做 | ETS 读写并发选项 |
| dirty scheduler | 长时间运行的 NIF 必须用，否则卡住普通调度器 | 同 |

下一课：[第 10 课：配置管线](10-config-pipeline.md)
