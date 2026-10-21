# Lesson 9: Talking to Erlang

[中文](../09-erlang-bridge.md) | **English**

> Files:
> - Erlang side: `erlang/src/gamebattle_port.erl`, `gamebattle_nif.erl`, `gamebattle_sup.erl`, `gamebattle.erl`
> - C++ side: `src/port_main.cpp`, `src/nif.cpp`, `src/term.cpp` (ETF encoding/decoding), `src/wire.cpp` (ETF ↔ battle structs)

The first eight lessons were about "how a battle is computed". This one is about "how a request comes in from Erlang and how the result goes back". This is where the side you know best (Erlang) meets C++.

## 1. The panorama: two channels, one handler

```
                    ┌──────────── Port channel (recommended for production) ────────────┐
gamebattle:simulate(port, Req)                                                          │
  → gamebattle_port (gen_server)                                                        │
  → port_command(Port, term_to_binary(Req))                                             │
  → [4-byte length][ETF bytes] ──stdin──▶ gamebattle_port process (port_main.cpp)
                                          │
                                          ▼
                              wire::handle_etf(bytes) ──▶ Engine::simulate
                                          ▲
gamebattle:simulate(nif, Req)             │
  → gamebattle_nif:simulate(Req)          │
  → nif.cpp: enif_term_to_binary ─────────┘   (inside the BEAM process, on a dirty scheduler thread)
                    └──────────── NIF channel (optional) ───────────────────────────────┘
```

Both channels end up calling the same `wire::handle_etf`: **ETF bytes in, ETF bytes out**. So Port and NIF can't compute two different sets of results; that's how the README's `true = (Result1 =:= Result2)` is guaranteed.

## 2. The Erlang side: how `gamebattle_port` keeps the C++ process in check

```erlang
init(Options) ->
    process_flag(trap_exit, true),
    ...
    Port = open_port({spawn_executable, filename:absname(Executable)},
                     [binary, {packet, 4}, use_stdio, exit_status]),
    case load_startup_config(Port, Timeout) of
        ok -> {ok, #state{port = Port, executable = Executable, timeout = Timeout}};
        {error, Reason} -> close_port_safely(Port), {stop, Reason}
    end.

handle_call({request, Request}, _From, State = #state{port = Port, timeout = Timeout}) ->
    case exchange(Port, Request, Timeout) of
        {reply, Response} ->
            remember_config(Request, Response),
            {reply, Response, State};
        {port_exit, Reason, Error} ->
            {stop, {port_exit, Reason}, {error, Error}, State};
        timeout ->
            {stop, port_timeout, {error, #{type => timeout, timeout_ms => Timeout}}, State}
    end;

exchange(Port, Request, Timeout) ->
    true = port_command(Port, term_to_binary(Request)),
    receive
        {Port, {data, ResponseBinary}} ->
            {reply, decode_response(ResponseBinary)};
        {Port, {exit_status, Status}} ->
            {port_exit, Status, #{type => port_exit, status => Status}};
        {'EXIT', Port, Reason} ->
            {port_exit, Reason, #{type => port_exit, reason => Reason}}
    after Timeout ->
        close_port_safely(Port),
        timeout
    end.
```

You should know this well. The key design points:

| Design | Purpose |
|---|---|
| `{packet, 4}` | Every message is prefixed with a 4-byte big-endian length header; C++ must read and write the same format |
| `exit_status` | When the C++ process exits, Erlang receives `{Port, {exit_status, N}}`, where N is `main`'s return value |
| `handle_call` waits for the reply synchronously (`exchange/3`) | **One worker handles only one battle at a time.** This is deliberate backpressure so that several requests don't compete for the same Port's responses |
| `after Timeout` | When the C++ side loops forever or hangs, close the Port and `stop`; the supervisor restarts a brand-new C++ process |
| `load_startup_config` in `init` | A freshly started C++ process has no config. When `config_path` (or the `GAMEBATTLE_CONFIG` environment variable) is set, it is loaded before any request is served; a successful `load_config/1` records the new path (`remember_config`), so a restarted Port gets the most recently loaded config |
| `gamebattle_sup`: `one_for_one`, at most 5 restarts in 10 seconds | Occasional crashes recover automatically; frequent crashes are escalated |

When you need concurrency, the README recommends starting several workers and sharding by `battle_id`, rather than letting one worker handle several requests at once.

**Here a C++ crash is just an ordinary process exit**, which is the fundamental reason Port is recommended (lesson 8).

## 3. `{packet, 4}` on the C++ side: `port_main.cpp`

I used a small Python script to play Erlang and talk directly to the compiled `gamebattle_port` (measured, in hex):

```
--- ping
  sent:     00 00 00 07 83 77 04 70 69 6e 67
            └length 7─┘  └──── term_to_binary(ping) ────┘
  received: length header=13  body: 83 68 02 77 02 6f 6b 77 04 70 6f 6e 67      → {ok, pong}
--- invalid request
  sent:     00 00 00 02 83 ff
  received: length header=76  body: 83 68 02 77 05 65 72 72 6f 72 74 00 00 00 02 ...
            readable part of the error message: unsupported ETF tag: 255
Port exit code after stdin closed = 0
```

`main` is just an infinite loop: read the 4-byte header → read the body → handle it → write back:

```cpp
int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);    // ①
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    while (true) {
        std::array<std::uint8_t, 4> header{};
        if (!read_exact(header)) {
            return std::cin.eof() ? 0 : 2;     // Erlang closes the Port → stdin EOF → normal exit
        }
        const auto length = (static_cast<std::uint32_t>(header[0]) << 24U) |   // ② assemble big-endian
                            (static_cast<std::uint32_t>(header[1]) << 16U) |
                            (static_cast<std::uint32_t>(header[2]) << 8U) |
                            static_cast<std::uint32_t>(header[3]);
        if (length == 0 || length > kMaxPacketBytes) {    // ③ 64 MB limit
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

### ① Windows must switch to binary mode

On Windows, standard input and output default to "text mode", which converts between `\n` (0x0A) and `\r\n`. ETF bytes can contain 0x0A at any point, and one conversion corrupts them. `#ifdef _WIN32` is **conditional compilation**: these lines are compiled only on Windows and skipped entirely on Linux.

### ② Byte order: why assemble by hand instead of reading straight into a uint32

```
7 in native memory          : 07 00 00 00  (little-endian machine)
7 written big-endian by shifts : 00 00 00 07
```

x86 and ARM are both **little-endian**: the low-order byte comes first. `{packet, 4}` specifies **big-endian**: the high-order byte first (network byte order). Assembling with shifts gives a result **independent of the machine it runs on**. It's the same thing as Erlang's `<<Length:32/big>>`; Erlang just writes it for you, and in C++ you write it yourself.

### ③ Trust no external input

The length header comes from outside. If someone sends `FF FF FF FF` and nothing checks it, the code tries to allocate 4 GB. The return codes 0/2/3/4/5 show up on the Erlang side as `exit_status`, making it easy to tell which step failed.

### `std::span` and `reinterpret_cast`

```cpp
bool read_exact(std::span<std::uint8_t> destination) {
    std::cin.read(reinterpret_cast<char*>(destination.data()),
                  static_cast<std::streamsize>(destination.size()));
    ...
}
```

- **`std::span<T>`** (C++20) is a **view** of contiguous memory: a pointer plus a length; it owns no data and copies nothing. Both `std::array` and `std::vector` convert to a span implicitly (the "meaning-preserving implicit construction" from lesson 3), so `read_exact(header)` and `read_exact(request)` share one function. It's a borrow, so it **must not outlive the array it borrows**.
- **`reinterpret_cast<char*>`**: `std::cin.read` only accepts `char*`, but our buffer is `uint8_t*`. `reinterpret_cast` means "look at this memory as another type". In general that's dangerous, but reading or writing any object as `char` / `unsigned char` / `std::byte` is an exception the standard **explicitly allows**. Every `reinterpret_cast` in the project is used only for this kind of "view it as bytes".

### An iron rule: never print anything to stdout in a Port process

`stdout` *is* the protocol channel. If you casually write `std::cout << "hello"` while debugging, Erlang takes the 4 bytes `hell` as a length header (0x68656c6c, about 1.7 billion), waits for that much data and eventually times out. **Debug output always goes to `std::cerr`**, and that's what the project does. The `std::cout.flush()` at the end of `write_packet` can't be skipped either, or the data may sit in a buffer and Erlang never receives it.

## 4. ETF: what the bytes from `term_to_binary` look like

ETF (External Term Format) is Erlang's own binary format. The first byte is always the version number 131 (0x83), and after that every value is "a 1-byte tag + contents". The tags `term.cpp` supports:

| tag | Decimal | Meaning | Example |
|---|---|---|---|
| `0x83` | 131 | Version number (appears once, at the start) | |
| `0x61` | 97 | Small integer 0–255, 1 byte | `42` |
| `0x62` | 98 | 32-bit signed integer, 4 bytes big-endian | `-9` |
| `0x6e` / `0x6f` | 110 / 111 | Big integer (bignum) | `5000000000` |
| `0x46` | 70 | 64-bit float | `1.5` |
| `0x77` / `0x76` | 119 / 118 | UTF-8 atom (short / long) | `ok` |
| `0x6d` | 109 | binary, 4-byte length + contents | `<<"poison">>` |
| `0x68` / `0x69` | 104 / 105 | Tuple (small / large) | `{ok, pong}` |
| `0x6a` | 106 | Empty list `[]` | |
| `0x6c` | 108 | List, 4-byte count + elements + tail | `[a, b]` |
| `0x6b` | 107 | Byte list (Erlang encodes a list made entirely of integers 0–255 this way) | `[1,2,3]`, `"abc"` |
| `0x74` | 116 | map, 4-byte count + key/value pairs | `#{a => 1}` |

Compare against the response measured above:

```
83          version 131
68 02       tuple, 2 elements
77 02 6f 6b   atom, length 2, "ok"
77 04 70 6f 6e 67   atom, length 4, "pong"
→ {ok, pong}
```

Note the 0x6b row: `term_to_binary([1,2,3])` produces **not** the list format but the "byte string" format. If the C++ side implemented only 0x6c, lists of small integers sent from Erlang would fail to parse. `term.cpp:130` handles this specifically. Only someone who knows Erlang well would think of a detail like that.

## 5. `term::Value`: representing any Erlang term with `std::variant`

Erlang terms are dynamically typed: a variable may be an integer, an atom, a list, a tuple… C++ is statically typed, and to express "one of these several types" it uses **`std::variant`** (`term.hpp:25-51`):

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

    Value() : data(Atom{"undefined"}) {}        // the default is the atom undefined, very Erlang
    explicit Value(std::int64_t value) : data(value) {}
    ...
};
```

### What `std::variant` is

At any moment, `std::variant<A, B, C>` **holds a value of exactly one of those types**, and it remembers which one. It's C++'s "tagged union", essentially the same thing as the type tag inside an Erlang term.

```cpp
Storage data = Atom{"hero"};
data.index()                         // 2: it currently holds the 3rd type (Atom)
std::get_if<Atom>(&data)             // returns a pointer to the Atom
std::get_if<std::int64_t>(&data)     // wrong type, returns nullptr
std::get<std::int64_t>(data)         // wrong type, throws std::bad_variant_access
```

(All of the above measured.)

**`std::get_if` is C++'s pattern matching**:

```cpp
if (const auto* atom = std::get_if<Atom>(&value.data)) {
    return atom->value;
}
if (const auto* binary = std::get_if<Binary>(&value.data)) {
    return std::string(binary->value.begin(), binary->value.end());
}
type_error(path, "an atom or binary");
```

The Erlang equivalent:

```erlang
as_string(A) when is_atom(A) -> atom_to_list(A);
as_string(B) when is_binary(B) -> binary_to_list(B);
as_string(_) -> error(badarg).
```

`if (const auto* atom = ...)` **declares a variable** inside the if condition: the branch is entered only if the pointer is non-null, and `atom` is visible only inside that branch.

### Why wrap `ListValue` / `TupleValue`

`List` and `Tuple` are both `std::vector<Value>`, **the same type**. Put directly into the variant, `std::get_if<std::vector<Value>>` couldn't tell whether you want a list or a tuple. Wrapping each in a differently named struct makes them two distinct types. In Erlang lists and tuples are naturally different things; C++ has to manufacture the difference this way.

### A recursive type: `Value` holding `vector<Value>`

Halfway through its definition, `Value` is still an **incomplete type** (lesson 1, section 8). You can write `std::vector<Value>` inside it because since C++17 the standard explicitly allows a `vector`'s element type to be incomplete at the point of declaration. A `vector` only stores a pointer to heap memory internally, so its size is fixed: the same reasoning as "break the cycle with a pointer" in lesson 1.

### Why maps use `vector<pair>` instead of `std::map`

```cpp
using Object = std::vector<std::pair<std::string, Value>>;
```

- **It preserves insertion order**: when results are encoded, fields come out in the order they were written, so the same result encodes to exactly the same bytes every time. `first_bytes == second_bytes` in the tests (`engine_test.cpp:574`) compares exactly that.
- Maps in requests usually have only a dozen or so keys, and a linear search is faster than hashing.

### `std::visit`: automatically pick a handler for the current type

When encoding (`term.cpp:251`):

```cpp
std::visit([&](const auto& item) { write_item(item, depth); }, value.data);
```

Together with a set of overloaded functions with the same name:

```cpp
void write_item(std::int64_t value, std::size_t);
void write_item(double value, std::size_t);
void write_item(const Atom& atom, std::size_t);
void write_item(const Value::ListValue& list, std::size_t depth);
...
```

- `std::visit` takes the variant's current value and passes it to the lambda.
- A lambda whose parameter is written `const auto& item` is a **generic lambda**: the compiler generates a version for **every** type in the variant.
- Each version calls `write_item(item, ...)`, and the compiler picks the overload matching `item`'s actual type.

The effect is "dispatch by type", like an Erlang function with one clause per type. If a `write_item` for some type is missing, **compilation fails**.

### Why there's `static_cast<std::int64_t>` everywhere

`wire.cpp` is full of code like this:

```cpp
{"seq", Value(static_cast<std::int64_t>(event.seq))},
```

`event.seq` is a `uint32_t`, and `Value` has two constructors, `Value(std::int64_t)` and `Value(double)`. Converting `uint32_t` to either takes one conversion, and the compiler can't decide which is better (measured):

```
error: call of overloaded 'Value(uint32_t&)' is ambiguous
```

Only an explicit conversion to `int64_t` removes the ambiguity. In C++ this kind of error is called **ambiguous overload resolution**; when you hit it, spell out the type you want.

## 6. The `Reader` decoder: defensive everywhere

```cpp
class Reader {
    std::span<const std::uint8_t> bytes_;    // borrows the whole input, no copy
    std::size_t offset_{0};                  // how far we've read

    void require(std::size_t count) const {
        if (count > bytes_.size() - offset_) {    // ← note how this is written
            throw DecodeError("truncated ETF value");
        }
    }
};
```

- **Every read is preceded by `require`**, guaranteeing it never reads past the end.
- It's written `count > size - offset` rather than `offset + count > size`: the latter **overflows and wraps** when `count` is huge, and the check stops working (lesson 7: "the check itself must not overflow").
- **A depth limit of 128** and **a container element limit of 1 million** guard against malicious input. For example, a list that claims 4 billion elements is stopped by `check_count` before `reserve`, so it never tries to allocate tens of GB.

### Erlang's big integers are rejected here

```cpp
Value read_big(std::uint32_t count) {
    if (count > sizeof(std::uint64_t)) throw DecodeError("integer does not fit into 64 bits");
    const auto sign = u8();
    std::uint64_t magnitude = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        magnitude |= static_cast<std::uint64_t>(u8()) << (index * 8U);   // note: little-endian here
    }
    if (sign == 0) {
        if (magnitude > INT64_MAX) throw DecodeError("positive integer does not fit into int64");
        return Value(static_cast<std::int64_t>(magnitude));
    }
    ...
    if (magnitude == (std::uint64_t{1} << 63U)) {
        return Value(std::numeric_limits<std::int64_t>::min());     // ← special case
    }
    return Value(-static_cast<std::int64_t>(magnitude));
}
```

- Erlang integers are unbounded; C++ can only hold int64. Anything larger is rejected outright with `invalid_request`. This is the **zeroth line** before lesson 7's "three lines of defense".
- A bignum's digits are **little-endian**, while ETF's length fields are all big-endian. Both byte orders live in one format, a historical quirk of ETF.
- The most negative value `-2^63` needs special handling: `2^63` itself doesn't fit in int64, so converting first and then negating would be UB.

### Floats: `std::bit_cast`

```cpp
case kNewFloat: {
    const auto bits = u64();
    return Value(std::bit_cast<double>(bits));
}
```

Reinterpret the 64 bits as a `double` exactly as they are (measured: `bit_cast<uint64>(1.5) = 0x3ff8000000000000`). Before C++20 the common approaches were pointer casts or unions, and both are UB; `std::bit_cast` is the safe way the standard provides.

### `[[noreturn]]`

```cpp
[[noreturn]] void type_error(std::string_view path, std::string_view expected) {
    throw DecodeError(...);
}

const Value::List& as_list(const Value& value, std::string_view path) {
    if (const auto* list = std::get_if<Value::ListValue>(&value.data)) {
        return list->value;
    }
    type_error(path, "a list");      // no return after this, and no warning
}
```

`[[noreturn]]` tells the compiler "this function never returns normally", so `as_list` can end without a `return` and not get a "control reaches end of non-void function" warning.

## 7. `wire.cpp`: ETF Values ↔ battle structs

### The function template `checked_int<T>`

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

This is the first proper appearance of **templates** in the course. `template <typename T>` says `T` is a **compile-time type parameter**: when you write `checked_int<std::int32_t>(...)`, the compiler substitutes `int32_t` for `T` and generates a dedicated version; `checked_int<BasisPoints>(...)` generates another.

Erlang functions are naturally "generic" because Erlang is dynamically typed. C++ uses templates to "write once, generate one copy per type", and every copy is fully type-checked.

### `std::string_view`

```cpp
T checked_int(std::int64_t value, std::string_view path)
```

`string_view` is a **view** of a string; like `span` it only borrows, owns nothing and copies nothing. For passing read-only strings like field names it's more flexible than `const std::string&` (a literal can be passed without first constructing a `std::string`). The same rule applies: **it must not outlive the string it borrows**. In the project what's passed to it is almost always a literal, and literals exist for the whole run of the program, so it's safe.

### A strict schema: reject unknown fields

```cpp
void require_only_fields(const Value& value, std::string_view path,
                         std::initializer_list<std::string_view> supported) { ... }

require_only_fields(value, "buff.modifier", {"attribute", "operation", "value"});
```

Erlang maps are lenient; an extra field usually goes unnoticed. But if a designer writes `max_stack` instead of `max_stacks`, a lenient parser silently uses the default of 1, and the bug is hard to track down. Here **unknown fields are rejected outright**, with an error message saying `contains unsupported field 'max_stack'`.

`std::initializer_list<T>` lets a function accept a braced list like `{"a", "b", "c"}` directly.

### Encoding results: enums become atoms, messages become binaries

```cpp
{"winner", Value::atom(winner_name(result.winner))},     // enum → atom
{"message", Value::binary(message)}                       // text → binary
```

```cpp
Value integer(std::uint64_t value) {
    if (value > INT64_MAX) throw std::runtime_error("result integer exceeds signed 64-bit ETF adapter limit");
    return Value(static_cast<std::int64_t>(value));
}
```

Unit IDs are `uint64_t` in C++, but `term::Value` only supports int64. An out-of-range value doesn't quietly become negative; it throws an `internal_error`.

## 8. `Handler`: thread-safe config hot-swapping

```cpp
class Handler final {
public:
    std::vector<std::uint8_t> handle_etf(std::span<const std::uint8_t> request);
private:
    mutable std::shared_mutex config_mutex_;
    std::shared_ptr<const ConfigStore> configs_;
};

std::vector<std::uint8_t> handle_etf(std::span<const std::uint8_t> request) {
    static Handler handler;            // ← a function-local static object
    return handler.handle_etf(request);
}
```

### A function-local `static`

`static Handler handler;` is constructed **on the first call**, then lives until the program exits, and every call shares this one object. Since C++11 the standard guarantees that even if several threads make the first call simultaneously, it's constructed exactly once. It's a lazily initialized singleton, a bit like a globally registered process in Erlang, or `persistent_term`.

### A reader-writer lock: many readers, one writer

```cpp
// each battle (a reader):
std::shared_ptr<const ConfigStore> configs;
{
    std::shared_lock lock(config_mutex_);   // shared lock: many readers can hold it at once
    configs = configs_;                      // copies just one shared_ptr, instantly
}                                            // leaving the braces releases the lock automatically (RAII)
const BattleRequest battle = parse_request(decoded, configs.get());   // used at leisure outside the lock
```

```cpp
// load_config (the writer):
auto next = std::make_shared<ConfigStore>(ConfigStore::load_file(path));   // read the file and validate outside the lock (slow)
{
    std::unique_lock lock(config_mutex_);   // exclusive lock: waits for all readers to leave
    configs_ = next;                         // swaps just one pointer, instantly
}
```

The key points:

- **`std::shared_lock`**: many threads can hold it at once; **`std::unique_lock`**: only one thread can hold it, and it waits for every shared lock to be released.
- **Do as little as possible inside the lock**: a reader copies one pointer, the writer swaps one pointer. Reading files, parsing and whole battles all happen outside the lock.
- **A standalone pair of braces `{ }`** limits the lock's scope: leaving the braces destroys the lock object, releasing the lock automatically. RAII again.
- **Battles already in progress aren't affected**: they hold a `shared_ptr` to the old config, which is freed only after all of them finish (lesson 2, section 3).
- If `load_file` fails it throws, the `configs_ = next` line never runs, and **the old config keeps serving**.
- `mutable` means "may be modified even inside a const member function". Mutexes are usually declared `mutable`, because locking and unlocking don't count as changing the object's logical state.

In Port mode there's only one thread and this lock is never contended; it really earns its keep in NIF mode.

## 9. The NIF: `nif.cpp`

```cpp
ERL_NIF_TERM dispatch(ErlNifEnv* env, ERL_NIF_TERM request_term) {
    ErlNifBinary request{};
    if (!enif_term_to_binary(env, request_term, &request)) {      // ① term → ETF bytes
        return enif_make_badarg(env);
    }
    const auto response = gamebattle::wire::handle_etf(            // ② exactly the same path as the Port
        std::span<const std::uint8_t>(request.data, request.size));
    enif_release_binary(&request);                                 // ③ freed by hand

    ERL_NIF_TERM result;
    if (enif_binary_to_term(env, response.data(), response.size(), &result, 0) == 0) {
        return enif_make_tuple2(env, enif_make_atom(env, "error"),
                                enif_make_atom(env, "response_decode_failed"));
    }
    return result;                                                 // ④ ETF bytes → term
}

ErlNifFunc functions[] = {
    {"simulate", 1, simulate, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"load_config", 1, load_config, ERL_NIF_DIRTY_JOB_IO_BOUND}
};

ERL_NIF_INIT(gamebattle_nif, functions, nullptr, nullptr, nullptr, nullptr)
```

The Erlang side:

```erlang
-module(gamebattle_nif).
-on_load(init/0).

init() ->
    case erlang:load_nif(filename:join(resolve_priv_dir(), "gamebattle_nif"), 0) of
        ok -> ok;
        {error, Reason} -> persistent_term:put(?LOAD_ERROR, Reason)
    end.

simulate(_Request) ->                      % replaced by the C++ implementation once loaded
    erlang:nif_error({nif_not_loaded, persistent_term:get(?LOAD_ERROR, undefined)}).
```

`init` returns `ok` even when loading fails. An `on_load` that returns an error makes the module fail to load, and a release loads every module at boot, so a node built with only the Port and no NIF library would not start at all. The reason is kept in `persistent_term` and raised when the NIF is actually called.

### ① ④ Why the NIF still takes a detour through ETF

The NIF could read terms field by field with APIs like `enif_get_map_value`, skipping encoding and decoding. The project deliberately doesn't: it converts the term to ETF bytes first, goes through **exactly the same** `handle_etf` as the Port, then converts the result bytes back into a term. A little extra serialization cost buys "only one set of parsing code, so the two modes are bound to agree".

### ③ The C API has no RAII

`enif_release_binary` must be called by hand; forget it and you leak. If `handle_etf` threw an exception, this line would be skipped. It doesn't go wrong only because `handle_etf` catches everything internally with `catch (...)` (lesson 8). The sturdier approach is a small RAII wrapper class that calls `enif_release_binary` in its destructor. This is a common problem wherever a C-style API meets modern C++.

### `ERL_NIF_DIRTY_JOB_CPU_BOUND`: why battles must run on a dirty scheduler

BEAM's normal scheduler threads achieve fairness by having every process run for a short slice and then yield. A normal NIF call **can't be interrupted**, and the official guidance is that a normal NIF should return within about 1 millisecond.

A battle may compute hundreds of rounds and tens of thousands of events, far beyond 1 millisecond. On a normal scheduler:

- that scheduler thread is completely occupied, and every Erlang process queued on it has to wait;
- what you see is latency jitter across the whole node: slower message handling, heartbeat timeouts and so on.

Marked `DIRTY_JOB_CPU_BOUND`, the call is handed to **a dedicated pool of dirty CPU scheduler threads** (by default as many as normal schedulers, usually the number of logical CPUs), and the normal schedulers keep working as usual. `load_config` reads a file from disk, so it's marked `IO_BOUND` and goes to the dirty IO pool.

### NIF concurrency

Several Erlang processes can call `gamebattle_nif:simulate/1` at the same time, and those calls **run in parallel on different dirty threads**, sharing the same `static Handler`. Therefore:

- `Engine` must be stateless (lesson 1: `simulate` is a `const` member function);
- the shared config must be a read-only `shared_ptr<const ConfigStore>`, swapped under a reader-writer lock (section 8).

Also, Port and NIF run in different OS processes, so **each has its own independent config**. That's why the README says "the NIF uses its own in-process config store and must be loaded separately".

## 10. Port vs NIF

| | Port | NIF |
|---|---|---|
| Where the C++ runs | A separate OS process | Inside the BEAM process |
| A C++ crash or leaked exception | The Port process exits; the supervisor restarts it | **The whole Erlang node crashes** |
| A C++ infinite loop | `after Timeout` closes the Port and restarts it | A dirty thread is occupied forever and can't be killed from Erlang |
| Call overhead | Pipe I/O + ETF encoding/decoding | Only ETF encoding/decoding, no inter-process communication |
| Concurrency | One worker processes serially; concurrency comes from several workers | The dirty thread pool is naturally parallel |
| Deployment | A single executable | Must be compiled against an `erl_nif.h` from **the same OTP major version** as production |
| Debugging | Can be started on its own and have a debugger attached on its own | You attach to the entire BEAM process |

The conclusion matches the README: **use Port by default in production**; enable the NIF only after thorough load testing and fuzzing, and only when you genuinely need to save the IPC overhead.

## Summary

| Concept | Key point | Erlang counterpart |
|---|---|---|
| `{packet, 4}` | 4-byte big-endian length header, assembled with shifts, independent of machine byte order | `<<Len:32/big, Data/binary>>` |
| stdout is the protocol channel | Debug output goes only to stderr; flush after writing | None |
| `#ifdef _WIN32` | Conditional compilation; Windows must switch to binary mode | `-ifdef` |
| `std::span` / `std::string_view` | Views that borrow without owning or copying | Sub-binary references |
| `reinterpret_cast<char*>` | Only for the standard-sanctioned "view it as bytes" case | None |
| ETF tags | Version 131 + "tag + contents"; lists of small integers are encoded as 0x6b | `term_to_binary/1` |
| `std::variant` | A type-tagged union; C++'s dynamically typed value | The Erlang term itself |
| `std::get_if` | Returns a pointer if the type matches, otherwise nullptr | Function clauses with guards |
| `std::visit` + generic lambda | Dispatches automatically to an overload by the current type | Multi-clause functions |
| Overload ambiguity | `uint32_t` passed to `int64_t`/`double` constructors → convert explicitly | None |
| Function templates | `template <typename T>`, one copy generated per type | Functions are naturally generic |
| Function-local `static` | Constructed on first call, thread-safe, destroyed at program exit | Registered process / `persistent_term` |
| Reader-writer lock | Only swap a pointer inside the lock; do the heavy work outside | ETS read/write concurrency options |
| Dirty schedulers | Required for long-running NIFs, or they block the normal schedulers | Same |

Next: [Lesson 10: The config pipeline](10-config-pipeline.md)
