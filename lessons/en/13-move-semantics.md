# Lesson 13: Special member functions, value categories and move semantics

[中文](../13-move-semantics.md) | **English**

> Lesson 2 covered how to use `std::move`. This lesson covers the rules behind it: when the compiler generates copies and moves for you, when copies are elided, and when you think you're moving but are actually copying. At the end these rules explain a **4× performance problem** I measured in this project.
>
> Every count in this lesson comes from a `Tracer` type whose copy constructor and move constructor each bump a counter, so we can count exactly how many copies and moves happen.

## 1. The six special member functions

```cpp
struct T {
    T();                          // default constructor
    ~T();                         // destructor
    T(const T&);                  // copy constructor
    T& operator=(const T&);       // copy assignment
    T(T&&) noexcept;              // move constructor
    T& operator=(T&&) noexcept;   // move assignment
};
```

If you don't write them, the compiler generates them when needed, and the generated version simply "does the same operation on each member". But the generation rules depend on each other, which is a hotbed for interview questions and real bugs:

| You declared by hand | Does the compiler still generate the move operations? |
|---|---|
| Nothing | ✅ Yes |
| A destructor | ❌ **No** (the copy operations are still generated, but that behavior is deprecated) |
| A copy constructor or copy assignment | ❌ No |
| A move constructor or move assignment | The copy operations are **deleted** |

The second row is the nastiest. Measured, a class whose only difference is an empty destructor:

```cpp
struct WithDtor   { std::vector<Tracer> events = std::vector<Tracer>(100); ~WithDtor() {} };
struct RuleOfZero { std::vector<Tracer> events = std::vector<Tracer>(100); };

WithDtor   b = std::move(a);   // copies=100 moves=0   ← you wrote std::move, it copied 100 elements
RuleOfZero d = std::move(c);   // copies=0   moves=0   ← the vector just hands over its internal array
```

No warning whatsoever. **Add an empty destructor just to log something, and the class loses its ability to move.**

### Three rules

- **The Rule of Zero**: write none of them if you can, and let the members (`vector`, `string`, `shared_ptr`…) manage resources themselves. **Every struct in the project's `engine.hpp` follows the rule of zero**, so they can all be moved efficiently.
- **The Rule of Five**: if you must write one of them by hand (usually because the class directly manages a resource), spell out all five, or state your intent explicitly with `= default` / `= delete`.
- **`= delete`** forbids an operation. In `practice/thread_pool.hpp`:

```cpp
ThreadPool(const ThreadPool&) = delete;              // a thread pool can't be copied
ThreadPool& operator=(const ThreadPool&) = delete;
```

### A complete rule-of-five example: a file descriptor

`FileDescriptor` in `practice/battle_tcp_server.cpp` manages a system resource directly, so it has to write them itself:

```cpp
class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    FileDescriptor(const FileDescriptor&) = delete;                  // can't be copied, or it would be closed twice
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}                        // take the other's fd and set the other to -1
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {                                         // guard against self-assignment
            reset();                                                  // close our own first
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~FileDescriptor() { reset(); }
private:
    int fd_{-1};
};
```

- `std::exchange(x, new_value)`: sets `x` to the new value and returns the old one. It's very common in move operations.
- After a move, the source's fd is -1 and its destructor does nothing, so the resource is closed exactly once.
- This kind of "exclusive, movable, non-copyable" resource-managing class is the idea behind the standard library's `std::unique_ptr` (lesson 14).

## 2. Value categories: lvalues, rvalues, and "a named rvalue reference is an lvalue"

Every C++ expression has a **value category**, which determines whether it can be "taken from":

| Category | Intuition | Examples |
|---|---|---|
| lvalue | Has a name and an address, and will be used again later | `name`, `units[0]`, `*ptr` |
| prvalue (pure rvalue) | A temporary value, gone once used | `42`, `T{}`, `make_result()` |
| xvalue (expiring value) | Has a name, but you've declared "I don't need it any more" | `std::move(name)` |

The last two together are called **rvalues**. `T&&` binds only to rvalues, so in overload resolution an rvalue selects the move version.

`std::move` itself does nothing; it's just `static_cast<T&&>(x)`, **marking** an lvalue as an xvalue.

The easiest thing to get wrong: **a variable of rvalue-reference type is itself an lvalue**, because it has a name. Measured:

```cpp
void take(const std::string&);   // copy version
void take(std::string&&);        // move version

void forward_wrong(std::string&& s) { take(s); }              // → take(const std::string&), a copy!
void forward_right(std::string&& s) { take(std::move(s)); }   // → take(std::string&&), a move
```

`s` has type `std::string&&`, but the expression `s` is an lvalue. Even though the parameter is an "rvalue reference", you must write `std::move` again inside the function to keep moving it along.

## 3. Perfect forwarding: `T&&` and `std::forward`

A `T&&` in a template isn't an rvalue reference but a **forwarding reference**: pass an lvalue and `T` deduces to `X&`; pass an rvalue and `T` deduces to `X`. Combined with `std::forward<T>`, it passes the argument on **with its value category preserved**:

```cpp
template <typename T>
void forward_generic(T&& s) { take(std::forward<T>(s)); }

forward_generic(name);                    // lvalue in → take(const std::string&)
forward_generic(std::string("poison"));   // rvalue in → take(std::string&&)
```

(Measured: the two cases reached the copy and move versions respectively.)

`submit` in `practice/thread_pool.hpp` is written this way:

```cpp
template <typename Function>
auto submit(Function&& function) -> std::future<std::invoke_result_t<Function>> {
    auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Function>(function));
    ...
}
```

If the lambda passed in is a temporary, it's moved into the task; if it's a named variable, it's copied. The caller doesn't have to care.

**Rule: use `std::move` on named objects you're sure you no longer need; use `std::forward` only on forwarding references `T&&`.**

## 4. `noexcept` move constructors: the big trap in vector growth

```cpp
std::vector<Tracer> v;
for (int i = 0; i < 1000; ++i) { Tracer t; v.push_back(std::move(t)); }
```

Measured with two versions of `Tracer` whose only difference is whether the move constructor is marked `noexcept`:

```
move constructor is noexcept    : copies=0    moves=2023
move constructor isn't noexcept : copies=1023 moves=1000
```

The 1000 `push_back`s are 1000 moves themselves. The difference comes from growth: the vector has to relocate the old elements into new memory. If the move constructor **might throw**, an exception halfway through would leave the old array partially emptied, with no way to restore it. To guarantee "if growth fails, the original vector is intact" (the strong exception guarantee, lesson 10), vector uses moves only when the move constructor is marked `noexcept`, and otherwise **falls back to copying**.

So all 1023 relocations during growth became copies.

- **Rule: mark the move constructors and move assignments you write yourself `noexcept`.**
- A compiler-generated move operation is automatically `noexcept` as long as every member's move is `noexcept`. Another benefit of the rule of zero.
- In the project, `RuntimeUnit` and `Event` both follow the rule of zero, so when `units.push_back(std::move(runtime))` grows the vector, it moves.

## 5. Copy elision: RVO and NRVO

```cpp
T make_prvalue() { return T{}; }                              // return a temporary
T make_named()   { T result; ...; return result; }            // return a named local
T make_moved()   { T result; ...; return std::move(result); } // gilding the lily
T make_branchy(bool flag) { T a, b; if (flag) return a; return b; }
```

Measured:

```
return T{}                    : copies=0 moves=0     ← copy elision guaranteed by C++17
return result (NRVO)          : copies=0 moves=0     ← the compiler constructs result directly in the caller's slot
return std::move(result)      : copies=0 moves=1     ← one extra move instead
two candidates, no NRVO       : copies=0 moves=1     ← the compiler can't tell which to put in the caller's slot, so it moves
```

- **RVO**: when returning a prvalue, C++17 **guarantees** no copy and no move; the object is constructed directly in the caller's memory.
- **NRVO**: when returning a named local, the compiler **usually** elides the copy (the standard allows but doesn't require it). When it can't, it automatically treats the variable as an rvalue and **moves** it, never copies.
- Writing `return std::move(result);` actually **prevents** NRVO. GCC warns: `moving a local object in a return statement prevents copy elision [-Wpessimizing-move]`.

Compare with lesson 2's conclusion: **don't write `std::move` when returning a local; do write it when returning a member** (such as `result` in `BattleState::finish()`), because a member isn't a local and the rules above don't apply.

## 6. `std::initializer_list` can only copy

```cpp
std::vector<T> v{T{}, T{}, T{}};                              // copies=3
std::vector<T> v; v.reserve(3); v.emplace_back(); ×3          // copies=0
```

When you initialize a container with a braced list, the compiler first constructs those 3 elements in a hidden array, and the `std::initializer_list` points at that array. The problem: **the elements of that array are `const`**, so the container can only **copy** from them, not move. Even if you wrote temporaries, you can't escape one copy.

A sneakier case:

```cpp
std::vector<T> inner(1000);
std::vector<std::vector<T>> outer{std::move(inner)};          // copies=1000 !
```

You clearly wrote `std::move(inner)`. It did move `inner` into that hidden array, but then `outer` can only **copy** the element out of the `const` array, so all 1000 elements get copied one by one.

### A real case: the project's result encoding was 4× too slow

`encode_result` in `wire.cpp` is written exactly this way:

```cpp
Value::List events;
... // put 880 events into events, 12 fields each
return Value::object({
    {"battle_id", integer(result.battle_id)},
    ...
    {"events", Value::list(std::move(events))},   // ← thought this was a move
    {"units", Value::list(std::move(units))}
});
```

`Value::object` takes a braced list, so **the entire event list (880 maps, tens of thousands of fields) gets deep-copied once**. The `Value::object({...})` inside each event does the same, copying each of its 12 fields once.

I profiled the full pipeline with valgrind's callgrind (lesson 21), and the top entries were:

```
21.3%   copy constructor of std::vector<std::pair<std::string, Value>>
11.3%   copy constructor of std::vector<Value>
```

The fix is just to stop using braced lists: `reserve` first, then `emplace_back` each element by moving it in. As written in `practice/encode_bench.cpp`:

```cpp
template <typename... Fields>
Value object_of(Fields&&... fields) {
    Value::Object object;
    object.reserve(sizeof...(fields));
    (object.emplace_back(std::forward<Fields>(fields)), ...);   // a fold expression, lesson 15
    return Value::object(std::move(object));
}
```

Measured: after first verifying that the output for 1000 battles is **byte-identical to the original implementation**, then timing, it went from about 1100 µs per battle to about 600 µs, **about 1.9× faster**. Skipping the intermediate `Value` tree altogether and writing bytes directly is about 4× faster (lesson 21).

> **Rule: braced lists are fine for constants and small objects. When holding large objects that need to be moved, use `reserve` + `emplace_back`.**

## 7. `push_back` vs `emplace_back`

```cpp
std::vector<std::pair<std::string, T>> v;
v.push_back({"a", t});                         // copies=1 moves=1
v.push_back({"b", std::move(t)});              // copies=0 moves=2
v.emplace_back("c", T{});                      // copies=0 moves=1
v.emplace_back(std::piecewise_construct,
               std::forward_as_tuple("d"),
               std::forward_as_tuple());       // copies=0 moves=0
```

(Measured.)

- `push_back(x)` takes an **already constructed** element and copies or moves it into the container.
- `emplace_back(args...)` **forwards** the arguments to the element's constructor and **constructs it directly in the container's memory**, skipping the intermediate temporary.
- For small elements like `int` or pointers there's no difference; when constructing an element is expensive, `emplace_back` is better.
- Note: `emplace_back` will call `explicit` constructors and `push_back` won't. So `emplace_back` is more "permissive", and a type mistake may quietly construct an object you didn't want.

## Interview questions

**Q1: What are the rule of zero / three / five?**
Rule of zero: let members manage resources, and the class itself writes no special member functions. Rule of three (C++98): destructor, copy constructor, copy assignment: write one and you should write all three. Rule of five (C++11): add the move constructor and move assignment.

**Q2: What happens if you write only a destructor?**
The compiler stops generating the move constructor and move assignment, and `std::move` on the object silently falls back to copying. Measured, a member with 100 elements turned a "move" into 100 copies, with no warning at all.

**Q3: What does `std::move` do?**
Nothing; it's just a `static_cast<T&&>` that converts an lvalue into an xvalue so overload resolution can pick the move version. The actual "move" happens in the move constructor or move assignment that gets called.

**Q4: What's the difference between `std::move` and `std::forward`?**
`std::move` converts to an rvalue unconditionally. `std::forward<T>` is conditional: it converts only when `T` was deduced from an rvalue, and it's used in templates to forward an argument with its value category intact. `std::forward` is only used with forwarding references `T&&`.

**Q5: Why should a move constructor be `noexcept`?**
When containers like `std::vector` grow, to keep the strong exception guarantee they move only if the move constructor can't throw, and copy otherwise. Without `noexcept`, every element is copied during growth (measured: 1000 push_backs produced 1023 copies).

**Q6: What are RVO and NRVO? Is `return std::move(local)` a good idea?**
RVO: when returning a prvalue, the object is constructed directly in the caller's memory; mandatory since C++17. NRVO: when returning a named local, the compiler may elide the copy, and when it can't, it moves automatically. `return std::move(local)` prevents NRVO and adds a move; GCC gives a `-Wpessimizing-move` warning.

**Q7: What makes `emplace_back` better than `push_back`?**
`emplace_back` forwards its arguments to the constructor and builds the element directly in the container's memory, saving a temporary and a move (or copy). The cost is that it will call `explicit` constructors, so type checking is looser.

**Q8: Tell me about a performance problem you hit in your project.**
Battle result encoding took 77% of the full pipeline's time. Callgrind showed most of it was spent in `vector` copy constructors, because when maps were built from `std::initializer_list` the elements were `const` and could only be copied, so the entire event list was deep-copied. Switching to `reserve` + `emplace_back` with moves made it about 1.9× faster; switching to streaming bytes directly made it about 4× faster. The output of 1000 battles was compared byte for byte before and after to confirm the results were identical.

Next: [Lesson 14: Smart pointers in depth](14-smart-pointers.md)
