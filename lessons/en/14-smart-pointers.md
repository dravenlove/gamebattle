# Lesson 14: Smart pointers in depth

[中文](../14-smart-pointers.md) | **English**

> Lesson 2 answered "why `shared_ptr<const BuffSpec>`". This lesson completes the picture of the three smart pointers' internals, costs and use cases, and uses them to improve two places in the project.

## 1. Three smart pointers, one table

| | `std::unique_ptr<T>` | `std::shared_ptr<T>` | `std::weak_ptr<T>` |
|---|---|---|---|
| Ownership | Exclusive | Shared (reference counted) | None, only observes |
| Copyable? | ❌ move only | ✅ copying adds 1 to the count | ✅ |
| Size (measured) | 8 bytes | 16 bytes | 16 bytes |
| Extra overhead | None | Control block + atomic counts | Same as left |
| Typical use | A single owner, e.g. an RAII wrapper | Several owners with uncertain lifetimes | Breaking cycles, caches, observers |

**Default to `unique_ptr`**, and use `shared_ptr` only when there really are several owners. In the project, a `BuffSpec` is held simultaneously by the config store, several skills and the buffs on units: genuine sharing, hence `shared_ptr` (lesson 2).

## 2. `unique_ptr`: zero-overhead exclusive ownership

`unique_ptr<int>` measures 8 bytes, the same as a raw pointer. Compiled, it's just a pointer plus a `delete` at destruction, with no extra overhead.

### Custom deleters: managing any resource

`unique_ptr`'s second template parameter is a **deleter**: it's called when the pointer goes out of scope, instead of `delete`. That lets it manage any resource that "has to be released by hand":

```cpp
auto close_file = [](std::FILE* f) { std::fclose(f); };
std::unique_ptr<std::FILE, decltype(close_file)> file(std::fopen("battle.log", "w"), close_file);
```

The deleter's type affects the size (measured):

```
unique_ptr<FILE, function-pointer deleter>      : 16      ← has to store a function pointer
unique_ptr<FILE, captureless-lambda deleter>    : 8       ← an empty type, optimized away by the compiler
```

**A stateless function object as the deleter adds no overhead at all.**

### Using it to plug the hole from lesson 9

Lesson 9 pointed out that `enif_release_binary` in `nif.cpp` is called by hand and would be skipped if something threw in between. Wrap it in a `unique_ptr`:

```cpp
struct ReleaseBinary {
    void operator()(ErlNifBinary* bin) const noexcept { enif_release_binary(bin); }
};
using BinaryGuard = std::unique_ptr<ErlNifBinary, ReleaseBinary>;

ERL_NIF_TERM dispatch(ErlNifEnv* env, ERL_NIF_TERM request_term) {
    ErlNifBinary request{};
    if (!enif_term_to_binary(env, request_term, &request)) return enif_make_badarg(env);
    BinaryGuard guard(&request);      // from here on, it's released however the function is left
    const auto response = gamebattle::wire::handle_etf(...);
    ...
}
```

I verified this pattern against mock `erl_nif` declarations: throwing while `guard` is alive still calls `enif_release_binary`, and `sizeof(BinaryGuard)` is still 8 bytes.

That's the essence of RAII: **bind the act of "releasing" to an object's destructor and let the compiler guarantee it runs.** Lesson 13's `FileDescriptor` is the hand-written version; `unique_ptr` + a deleter is the ready-made standard-library version.

## 3. Inside `shared_ptr`: the control block

```
shared_ptr object (16 bytes)          control block (on the heap)
┌──────────────────┐               ┌──────────────────────────┐
│ T* to the object ┼──┐            │ strong count (atomic)    │  ← number of shared_ptrs
│ control block ptr┼──┼──────────▶ │ weak count (atomic)      │  ← number of weak_ptrs (+1)
└──────────────────┘  │            │ deleter, allocator       │
                      ▼            └──────────────────────────┘
                  ┌──────────┐
                  │ T object │
                  └──────────┘
```

- When the strong count reaches 0: **the object is destroyed**.
- When the weak count also reaches 0: **the control block is freed**. So as long as any `weak_ptr` exists, the control block stays, which is how it can answer "is the object still alive?"
- Both counts are **atomic variables**, so several threads copying the same `shared_ptr` at once is safe, but it has a cost (see section 6).

### `make_shared`: one allocation

Measured (counting with an overloaded global `operator new`):

```
shared_ptr<T>(new T) : 2 heap allocations      ← one for the object, one for the control block
make_shared<T>()     : 1 heap allocation       ← object and control block share one block of memory
make_unique<T>()     : 1 heap allocation
```

`make_shared` saves an allocation, and keeping the object next to its counts is friendlier to the cache. There's a safety benefit too: before C++17, `f(shared_ptr<T>(new T), g())` could leak if `g()` threw; `make_shared` can't.

One small cost: with `make_shared`, the object and control block are one block of memory, so **as long as any `weak_ptr` remains, none of it is freed** (the object has been destroyed, but the memory is still held). Watch out when the object is large and `weak_ptr`s live a long time.

**Rule: prefer `make_shared` and `make_unique`; don't write `new` directly.**

## 4. `weak_ptr`: breaking cycles

Lesson 2 said that `shared_ptr` cycles leak. Measured:

```cpp
struct Buff {
    std::shared_ptr<Buff> adds;          // another buff its reaction applies
    ~Buff() { std::cout << "~Buff(" << name << ")\n"; }
};
auto poison = std::make_shared<Buff>("poison");
auto burn   = std::make_shared<Buff>("burn");
poison->adds = burn;
burn->adds = poison;                     // a cycle
```
```
use_count before leaving scope: poison=2 burn=2
(no destructor output at all: both objects leaked)
AddressSanitizer: 112 byte(s) leaked in 2 allocation(s).
```

Change one side to a `weak_ptr`:

```cpp
burn->adds_weak = poison;                        // doesn't add to the strong count
if (auto target = burn->adds_weak.lock()) {      // lock() before use to get a temporary shared_ptr
    target->name;                                // the object is guaranteed alive
}
```
```
lock() succeeded, got poison
~Buff(poison)
~Buff(burn)
after destruction expired()=1 lock()==nullptr: 1
```

- A `weak_ptr` can't access the object directly; you must `lock()` first. If the object is alive you get a `shared_ptr`; if it's gone you get a null pointer.
- `lock()` is atomic: the `shared_ptr` it returns guarantees the object won't be destroyed until you're done with it. Don't call `expired()` and then `lock()`; another thread could destroy the object between the two steps.

### Why the project doesn't use `weak_ptr` and simply forbids cycles

If `weak_ptr` can break cycles, why do the project's config compiler and `validate_request` **reject** configs with cycles (lessons 8, 10)?

Because the semantics are wrong. A buff reaction effect like "apply burn to the target" **must** be able to count on burn's definition existing when it runs; with a `weak_ptr` the definition could already be gone at any moment, and what should the effect do when `lock()` fails? There's no sensible answer. And "poison applies burn, burn applies poison" would loop forever in gameplay anyway; it's a designer's config error and should be reported before release.

**`weak_ptr` suits relationships that are "nice to have, fine without"** (caches, observers, a callback referring to a connection that may have dropped). **For relationships that must exist, either use `shared_ptr` and guarantee there are no cycles, or redesign the ownership.**

## 5. `enable_shared_from_this`: keeping an object alive for async callbacks

A very common scenario in network servers: a connection object starts an async operation, and by the time the callback runs, the client may have disconnected and every outside `shared_ptr` is gone. If the callback captured only `this`, the object has already been destroyed.

```cpp
struct Session : std::enable_shared_from_this<Session> {
    void start_battle() {
        auto self = shared_from_this();          // get a shared_ptr from this, count +1
        pending_callbacks.push_back([self] {     // the callback holds it, so Session lives at least until it runs
            self->send_result();
        });
    }
};
```

Measured:

```
client disconnected (the outside shared_ptr is gone), callback still queued
callback runs, Session 1 still valid
~Session(1)                        ← destroyed only after the callback finishes and self is destroyed
```

Two restrictions:
- The object must **already be managed by a `shared_ptr`** (created with `make_shared`, say). Calling `shared_from_this()` on a stack object throws `std::bad_weak_ptr` (measured).
- It can't be called in the constructor: at that point no `shared_ptr` points to the object yet.

Almost all Boost.Asio networking code follows this pattern. `practice/battle_tcp_server.cpp` takes another route: connection objects live in a map, async tasks remember only the **connection ID**, and when a result comes back it's looked up by ID; if it's not found, the connection is gone and the result is dropped (lesson 2's "unique ID + lookup"). Both approaches have trade-offs, and you can discuss either in an interview.

## 6. Thread safety: the count is safe, the variable itself isn't

This is the single most common interview question on the topic: **is `shared_ptr` thread-safe?**

The answer has two layers:

1. **The reference count is thread-safe.** Several threads each holding **their own** copy of a `shared_ptr` can copy and destroy them concurrently without corrupting the count.
2. **The same `shared_ptr` variable** being written by one thread and read by another is a **data race**.

Measured: one thread keeps assigning new values to a global `shared_ptr` while another keeps reading it, and ThreadSanitizer reports:

```
WARNING: ThreadSanitizer: data race
```

With C++20's `std::atomic<std::shared_ptr<T>>`, the same reads and writes produce **0 warnings**:

```cpp
std::atomic<std::shared_ptr<const Config>> g_config;
g_config.store(std::make_shared<Config>(...));   // write
auto snapshot = g_config.load();                  // read: get your own copy
```

That's exactly what the project's `Handler` does (lesson 9): a `shared_mutex` protects a `shared_ptr<const ConfigStore>`; readers take a shared lock and copy it, the writer takes an exclusive lock and replaces it. `std::atomic<std::shared_ptr>` makes it shorter; lesson 18 compares the two.

### The cost of copying a `shared_ptr`

The reference count is updated atomically, and atomic operations aren't cheap, **especially when several threads modify the same count at once**. Measured over 10 million calls (with inlining disabled):

```
1 thread:  pass shared_ptr by value ≈ 200 ms         pass by const& ≈ 10–15 ms
4 threads: pass shared_ptr by value ≈ 2700–3200 ms   pass by const& ≈ 20 ms
```

Single-threaded, passing by value costs about 20 ns extra per call (one atomic increment, one atomic decrement). With 4 threads copying **the same** `shared_ptr`, it climbs to about 300 ns per call: the cache line holding the count bounces between 4 CPU cores (the same mechanism as "false sharing" in lesson 18).

**Rule: when a function only "uses" the object, take `const std::shared_ptr<T>&`, or simply `const T&`; pass by value only when the function stores ownership.**

## 7. A selection guide

```
Need to allocate an object dynamically?
 ├─ Only one owner ──────────────────────────────────────▶ unique_ptr (the default)
 ├─ Several owners with different lifetimes ─────────────▶ shared_ptr (created with make_shared)
 │    └─ some references are "optional" or could cycle ──▶ weak_ptr on that side
 └─ Only used briefly, sure the other side lives longer ─▶ raw pointer T* or reference T& (a borrow, no ownership)

Never: write delete by hand, or create two shared_ptrs from the same raw pointer (it gets deleted twice)
```

## Interview questions

**Q1: What's the difference between `unique_ptr` and `shared_ptr`? What does each cost?**
`unique_ptr` owns exclusively, can only be moved, is the size of a raw pointer and has no extra overhead. `shared_ptr` shares ownership, has a heap control block holding strong and weak counts, is two pointers in size, and does atomic increments and decrements on copy and destruction.

**Q2: What's the difference between `make_shared` and `shared_ptr<T>(new T)`?**
`make_shared` puts the object and control block in one allocation (measured: 1 vs 2), which is faster and more cache-friendly, and before C++17 it also avoided leaks caused by exceptions. The downside is that as long as any `weak_ptr` remains, the whole block can't be freed.

**Q3: Is `shared_ptr` thread-safe?**
Incrementing and decrementing the count is atomic, so several threads each holding a copy is safe. But the same `shared_ptr` variable read and written by several threads at once is a data race, which needs a lock or C++20's `std::atomic<std::shared_ptr<T>>`. Whether the pointed-to object is itself thread-safe has nothing to do with `shared_ptr`.

**Q4: How do you fix a `shared_ptr` reference cycle?**
Change one edge of the cycle to a `weak_ptr`. A `weak_ptr` doesn't add to the strong count; before use, `lock()` turns it into a temporary `shared_ptr`. Another approach is to redesign the ownership, or, as this project does, forbid cycles outright at the config stage.

**Q5: What's the difference between `weak_ptr`'s `lock()` and `expired()`? Why is `lock()` recommended?**
`expired()` only tells you whether the object has been destroyed at this instant. With multiple threads, the object may be destroyed between the check and the actual use. `lock()` atomically "checks and obtains a strong reference", so a non-null result guarantees the object stays alive while you use it.

**Q6: What problem does `enable_shared_from_this` solve? What are its limits?**
It lets an object safely get, from `this`, a new `shared_ptr` that shares the control block with existing ones, typically to extend the object's lifetime in async callbacks. Limits: the object must already be managed by a `shared_ptr`, and it can't be called in the constructor, or it throws `std::bad_weak_ptr`. Writing `shared_ptr<T>(this)` directly would create a second control block and the object would be deleted twice.

**Q7: Does a custom deleter make `unique_ptr` bigger?**
It depends on the deleter type: a stateless function object or captureless lambda adds nothing (empty base optimization), while a function-pointer deleter adds 8 bytes (measured: 16 vs 8).

**Q8: How should smart pointers be passed as function parameters?**
Just using the object: pass `const T&` or `T*`. Sharing ownership (it will be stored): pass the `shared_ptr` by value, then `std::move` it into a member. Transferring exclusive ownership: pass the `unique_ptr` by value. Avoid pointlessly passing `shared_ptr` by value: each call costs an extra pair of atomic operations, and under multithreaded contention that cost grows by more than ten times (measured).

Next: [Lesson 15: Templates, concepts and compile-time computation](15-templates.md)
