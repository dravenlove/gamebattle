# Lesson 15: Templates, concepts and compile-time computation

[中文](../15-templates.md) | **English**

> Lessons 9 and 10 already showed two function templates, `checked_int<T>` and `checked_enum<Enum>`. This lesson covers how templates work, C++20 concepts, and compile-time computation: "letting the compiler work it out in advance".

## 1. A template is "a recipe for generating code"

```cpp
template <typename T>
T checked_int(std::int64_t value, std::string_view path) { ... }

checked_int<std::int32_t>(x, "max_rounds");   // the compiler generates a version with T = int32_t
checked_int<std::uint32_t>(x, "buff.id");     // and another with T = uint32_t
```

A template itself isn't a function but **a recipe for generating functions**. Only when the compiler sees `checked_int<std::int32_t>` used in some `.cpp` does it substitute `int32_t` for `T` and generate a real function. This is called **instantiation**.

Two direct consequences follow:

### Consequence 1: template definitions usually have to be in headers

Put the declaration in a header and the definition in a `.cpp` (measured):

```
use.cpp:(.text+0xe): undefined reference to `int checked_int<int>(long)'
```

While compiling `use.cpp`, the compiler sees only the declaration, can't instantiate it, and leaves a reference "to be found at link time". While compiling `checked.cpp`, nothing there uses `checked_int<int>`, so nothing is generated. At link time nobody can find it.

Two fixes:
- **Put the definition in the header** (the most common). In the project, `checked_int` is defined inside `wire.cpp` and only used in that file, so it's fine.
- **Explicit instantiation**: write `template std::int32_t checked_int<std::int32_t>(std::int64_t);` at the end of the `.cpp`, telling the compiler "generate this version here". Measured: with that line, linking succeeds. It suits cases where the set of types is known and you don't want to expose the implementation in a header.

### Consequence 2: code bloat and compile time

Every new type used means one more copy of the code; heavy template use makes executables bigger and compiles slower. `extern template` tells other `.cpp` files "this version is generated elsewhere, don't generate it again".

## 2. Concepts: constraining template parameters

Before C++20, templates placed no constraints on their parameter types. Pass the wrong type, and the error explodes deep **inside** the template:

```cpp
template <typename T> T checked_int(std::int64_t value) { ... static_cast<T>(value) ... }
checked_int<std::string>(5);
```

Measured: **109 lines** of errors, the first being `invalid 'static_cast' from type 'std::string' to type 'int64_t'`, pointing at a line inside the template, with nothing to say the caller made the mistake.

With a concept:

```cpp
template <std::integral T>          // T must be an integer type
T checked_int(std::int64_t value) { ... }
```

Measured: **16 lines**, stating `no matching function for call to 'checked_int<std::string>(int)'` and `constraints not satisfied` directly. The error appears at the call site and is understandable at a glance.

Common standard concepts (`<concepts>`):

| concept | Meaning |
|---|---|
| `std::integral<T>` | An integer type |
| `std::floating_point<T>` | A floating-point type |
| `std::same_as<T, U>` | Exactly the same type |
| `std::convertible_to<T, U>` | Convertible to U |
| `std::invocable<F, Args...>` | Callable with these arguments |
| `std::ranges::range<T>` | Usable in a range-for |

You can define your own too:

```cpp
template <typename T>
concept EtfEncodable = requires(const T& value) {
    { encode_to_etf(value) } -> std::same_as<std::vector<std::uint8_t>>;
};

template <EtfEncodable T> void send_reply(const T& value);
```

Before concepts, the same constraint had to be written with **SFINAE** (`std::enable_if`), which reads badly:

```cpp
template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
T checked_int(std::int64_t value);
```

Interviews may ask what SFINAE is ("substitution failure is not an error": when substituting template arguments fails, the compiler just removes that candidate from the overload set instead of reporting an error). New code uses concepts directly.

## 3. Specialization: a separate version for certain types

```cpp
template <typename T> struct EtfTag { static constexpr const char* name = "unsupported"; };  // primary template
template <> struct EtfTag<std::int64_t> { static constexpr const char* name = "INTEGER_EXT"; };    // full specialization
template <> struct EtfTag<std::string>  { static constexpr const char* name = "BINARY_EXT"; };
template <typename T> struct EtfTag<T*> { static constexpr const char* name = "pointer: not encodable"; }; // partial specialization
```

Measured: `INTEGER_EXT | BINARY_EXT | unsupported | pointer: not encodable`.

- **Full specialization**: a separate implementation for one specific type.
- **Partial specialization**: a separate implementation for a family of types (here, "all pointers"). **Only class templates can be partially specialized**; function templates can't (use overloading instead).
- The standard library's `std::hash<T>` and `std::numeric_limits<T>` are both implemented through specialization. Writing a hash for a custom type in lesson 16 means specializing `std::hash`.

## 4. Type traits and `if constexpr`

`<type_traits>` provides tools for querying type information **at compile time**: `std::is_signed_v<T>`, `std::is_unsigned_v<T>`, `std::is_same_v<T, U>`… Combined with `if constexpr`, you can choose different code by type at compile time:

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

Measured: `to_etf_integer(std::uint32_t{7})` returns 7, and `to_etf_integer(std::uint64_t{1} << 63)` throws.

The difference from an ordinary `if`: an `if constexpr` condition is evaluated at compile time, and **the branch not taken isn't compiled at all**. For `uint32_t`, that overflow check simply doesn't exist, at zero cost; and code in the untaken branch that's invalid for the current type doesn't cause errors either. The `integer()` function in `wire.cpp` (which checks for overflow when converting `uint64` to ETF) could be written as a generic template this way.

`static_assert(condition, "message")` checks a condition at compile time and fails the build if it doesn't hold:

```cpp
static_assert(sizeof(Event) <= 128, "Event got bigger; check whether it affects performance");
```

## 5. Variadic templates and fold expressions

```cpp
template <typename... Ts>                       // Ts is a "pack" of types
auto sum(Ts... values) { return (values + ... + 0); }   // a fold expression: v1 + (v2 + (v3 + 0))

template <typename... Ts>
void log_line(const Ts&... parts) { ((std::cout << parts << ' '), ...); std::cout << '\n'; }

sizeof...(Ts)                                   // how many elements the pack has
```

Measured: `sum(35, 35, 35) = 105`, and `log_line("battle", 3001, "winner", "attacker", "rounds", 19)` prints `battle 3001 winner attacker rounds 19`.

This is the technique used for the encoding performance fix in lesson 13 (`practice/encode_bench.cpp`):

```cpp
template <typename... Fields>
Value object_of(Fields&&... fields) {
    Value::Object object;
    object.reserve(sizeof...(fields));                          // the field count is known at compile time
    (object.emplace_back(std::forward<Fields>(fields)), ...);   // one emplace_back per argument
    return Value::object(std::move(object));
}
```

`Fields&&...` is a pack of **forwarding references**, and `std::forward<Fields>(fields)...` forwards each argument as it is (lesson 13). Compared with `std::initializer_list`, it doesn't require all arguments to have the same type and doesn't force copies. `std::make_shared`, `emplace_back` and `std::thread`'s constructor are all implemented this way internally.

## 6. `constexpr`: let the compiler work it out in advance

A `constexpr` function can be called at run time or at compile time. Use it to generate a lookup table at compile time:

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
inline constexpr auto kCrcTable = make_crc_table();      // computed at compile time, stored in read-only data

static_assert(crc32_table("123456789") == 0xCBF43926U);  // CRC-32's standard check value: a wrong algorithm won't compile
```

Lesson 10 explained that the project's CRC32 is computed **bit by bit**: 8 loop iterations per byte. The table method does one lookup per byte. Measured on 16 MB of data:

```
both algorithms agree: 1
16 MB bitwise:  182 ms
16 MB table:     42 ms
```

About 4.3× faster, with the lookup table generated at compile time and zero initialization cost at run time.

To be honest: the current sample config is only 413 bytes, and this optimization **means nothing for it**. But config packages can be up to 64 MB, which takes about 0.7 seconds with the bitwise algorithm, all of it CPU time during a hot reload. In an interview, "knowing when an optimization is worth it and when it isn't" matters as much as "knowing how to optimize".

- `consteval` (C++20): the function **can only** be called at compile time.
- `constinit` (C++20): guarantees a global variable is initialized at compile time, avoiding the "static initialization order problem".

## 7. CRTP: compile-time polymorphism

Lesson 12 mentioned that besides virtual functions, templates can also implement polymorphism. **CRTP** (the curiously recurring template pattern) has a base class take the subclass itself as a template parameter:

```cpp
template <typename Derived>
struct EventSink {
    void on_damage(std::uint64_t target, std::int64_t value) {
        static_cast<Derived*>(this)->handle_damage(target, value);   // the callee is known at compile time
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

Measured: `DamageMeter total = 265  sizeof(DamageMeter) = 8 (no vptr)`.

- No vtable, and `on_damage` can be fully inlined.
- The cost: `EventSink<DamageMeter>` and `EventSink<Logger>` are two **unrelated** types and can't be put in the same container and handled uniformly. So CRTP suits cases where "which implementation is decided at compile time", such as battle-report statistics or policy classes. When it's only known at run time, use virtual functions or `variant`.

## Summary

| Tool | When to use it |
|---|---|
| Function templates / class templates | The same logic applies to many types |
| Concepts | Constrain template parameters so errors appear at the call site (measured: 109 lines → 16) |
| Full / partial specialization | Some types need a different implementation |
| `if constexpr` + type traits | Choose different code by type within one function; the untaken branch costs nothing |
| Variadic templates + fold expressions | A variable number of arguments, each keeping its own type and value category |
| `constexpr` / `static_assert` | Compute ahead of time what can be computed at compile time, and catch ahead of time what can be checked at compile time |
| CRTP | Polymorphism fixed at compile time, without vtable overhead |

## Interview questions

**Q1: Why do templates usually have to go in headers?**
Templates are instantiated where they're used, so the compiler must see the full definition in that translation unit. With the definition in a `.cpp`, other files can only generate an unresolved reference, which isn't found at link time (measured: `undefined reference`). Explicit instantiation fixes it, but only when the set of types is known.

**Q2: What is SFINAE? What replaces it in C++20?**
"Substitution failure is not an error": an error caused by substituting template arguments isn't reported; the candidate is just removed from the overload set. It's typically used with `std::enable_if` to select overloads by type. C++20 concepts and `requires` clauses express the same constraints far more readably, with clearer errors.

**Q3: What's the difference between full and partial specialization? Can function templates be partially specialized?**
Full specialization provides an implementation for one fully determined set of template arguments; partial specialization provides one for a family of arguments matching a pattern (all pointer types, say). Function templates can only be fully specialized, not partially; use overloading instead.

**Q4: What's the difference between `if constexpr` and an ordinary `if`?**
The condition of `if constexpr` must be a compile-time constant, and the untaken branch isn't instantiated: no run-time cost, and even code invalid for the current type inside it causes no error.

**Q5: What's the difference between `constexpr`, `consteval` and `constinit`?**
A `constexpr` function can be called at compile time or run time; a `constexpr` variable must be initialized at compile time and can't be modified afterwards. A `consteval` function can only be called at compile time. `constinit` only requires the variable to be initialized at compile time; it can still be modified later, and it's used to avoid the static initialization order problem.

**Q6: What is CRTP? What are its pros and cons compared with virtual functions?**
A base class template takes the subclass as its template parameter and calls the subclass implementation at compile time through `static_cast<Derived*>(this)`. Pros: no vtable pointer, and calls can be inlined. Cons: different subclasses have different base types, so they can't go in one container for run-time polymorphism, and template code bloats.

**Q7: What's the difference between variadic templates and `std::initializer_list`?**
An `initializer_list` requires all elements to have the same type, and its elements are `const` and can only be copied. In a variadic template each argument can be a different type, and with forwarding references and `std::forward` the value category is preserved, so moves work.

Next: [Lesson 16: STL container internals and iterator invalidation](16-stl-internals.md)
