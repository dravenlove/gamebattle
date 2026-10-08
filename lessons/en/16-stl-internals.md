# Lesson 16: STL container internals and iterator invalidation

[中文](../16-stl-internals.md) | **English**

> When interviewers ask about the STL, they never ask "how do you use a vector"; they ask "what happens when a vector grows", "what does unordered_map do on a collision", "which operations invalidate iterators". This lesson makes those clear, and uses data from a real battle in this project to show why they matter.

## 1. First, some real data

I linked the engine with a "counting" global `operator new` and ran a 5v5 sample battle (`practice/sample_battle.hpp`). Measured:

```
one battle: 912 events, sizeof(Event)=136
Engine::simulate: 1093 heap allocations, 318 KB total
result encoding (current implementation): 1879 heap allocations, 2034 KB total, 148 KB of output
```

- Simulating a battle makes on average **about 1.2 heap allocations per event**.
- Encoding the result **allocates 2 MB of memory to output 148 KB** (lesson 13 explained why: `initializer_list` forces deep copies).
- Each `Event` takes 136 bytes.

By the end of this lesson you should be able to explain where these three numbers come from and how to bring them down.

## 2. `vector`: contiguous memory and growth

GCC's growth sequence, measured:

```
capacity changes: 1 2 4 8 16 32 64 128 256 512 1024 2048
after reserve(1000), 1000 push_backs leave capacity=1000 (no growth)
```

- GCC (libstdc++) and Clang (libc++) grow by **2×** each time; MSVC by **1.5×**.
- Growth = allocate new memory + move (or copy, see lesson 13's `noexcept`) every element over + free the old memory. A single growth is O(n), but amortized over every `push_back` it's **O(1)**.
- After growth, every pointer, reference and iterator is invalid (measured in lesson 2).

**Why `events` accounts for many allocations**: a battle has 912 events, and `result.events` doubles from 1 up to 1024, growing 11 times. Each growth relocates every `Event` (136 bytes each). If the event count can be estimated, a `reserve` up front saves all of that. The project's `result.units.reserve(units.size())` (`battle_state.cpp:543`) does exactly this, because the unit count is known.

Other points:
- `shrink_to_fit()` requests that excess capacity be released, but the standard doesn't guarantee it.
- `clear()` only destroys the elements and **doesn't free memory**; capacity stays the same. That's actually a good thing: reusing the same vector avoids repeated allocation.
- `std::vector<bool>` is a **specialization** in which each element takes 1 bit, and `operator[]` returns a proxy object, not a `bool&`. Measured: `bool& first = alive[0];` fails to compile with `cannot bind non-const lvalue reference of type 'bool&' to an rvalue`. When you need a real array of bools, use `std::vector<char>` or `std::vector<std::uint8_t>`.

## 3. `std::string`: the small string optimization (SSO)

Measured:

```
sizeof(std::string)=32  empty string capacity=15
  "damage" length 6 → 0 heap allocations
  "buff_reaction" length 13 → 0 heap allocations
  "direct_damage__" length 15 → 0 heap allocations
  "direct_damage___" length 16 → 1 heap allocation
```

libstdc++'s `std::string` is 32 bytes, including a 16-byte internal buffer that can hold **up to 15 characters** directly (plus the trailing `\0`), with no heap allocation at all. Only beyond 15 characters does it allocate on the heap. This is the **small string optimization**.

The threshold differs between standard libraries: 15 for libstdc++, 22 for libc++, 15 for MSVC.

**In the project**: `Event` has two string fields, `phase` and `type`. The longest event types in the project are `buff_reaction` and `direct_damage` (13 characters), and the longest phase name is `second_side` (11 characters), all within 15, so **creating an event never allocates because of its strings**. Only the result's `reason` (for example `all_units_defeated`, 18 characters) allocates once, once per battle, which is negligible.

But SSO isn't free: the two strings together take 64 bytes, which is the main reason `sizeof(Event)` reaches 136. Changing `phase` and `type` to `enum class : std::uint8_t` (converted to atoms at encoding time) would shrink `Event` by about half, so the same cache would hold twice as many events. Lesson 21 discusses the trade-off: it changes `Event`, a public struct, affecting every user.

## 4. `unordered_map`: the hash table

### Structure: an array of buckets + linked lists

```
bucket_count = 13
 [0] → nullptr
 [1] → (1001, ...) → (2014, ...) → nullptr       elements in the same bucket are chained in a linked list
 [2] → (2002, ...) → nullptr
 ...
```

- Lookup: compute `hash(key) % bucket_count`, then walk that bucket's list comparing keys. O(1) on average, **O(n) in the worst case** (every key lands in the same bucket).
- **Load factor** = number of elements / number of buckets. When it exceeds `max_load_factor()` (1.0 by default), the table **rehashes**: the bucket array grows and every element is redistributed into the new buckets.

Measured, inserting 1000 elements:

```
initial bucket_count=13 max_load_factor=1
bucket_count changes: 29 59 127 257 541 1109
after inserting 1000 elements and several rehashes, element addresses unchanged: 1
```

libstdc++ always picks a **prime** bucket count, which spreads the modulo more evenly.

### A key property: rehashing invalidates iterators, but not references or pointers

Each element is **a separately allocated list node**. A rehash just hangs the nodes on different buckets; the nodes themselves don't move. So:
- **iterators are invalidated** (an iterator records "which position in which bucket");
- **references and pointers to elements stay valid** (measured: addresses unchanged).

This is exactly the opposite of `vector`. `practice/battle_tcp_server.cpp` relies on it: `connections_` is an `unordered_map<connection ID, Connection>`, and inserting a new connection in `accept_all` doesn't invalidate the `Connection&` held elsewhere.

### Costs and traps

- One node and one heap allocation per element; nodes are scattered around memory, so iteration has poor cache hit rates.
- **Iteration order is unspecified** and changes with insertion order and rehashes. Lessons 1 and 10 both stressed that the order of iterating an `unordered_map` must never affect battle results or output files.
- **Hash flooding attacks**: if keys come from untrusted external input, an attacker can deliberately craft lots of colliding keys, turning O(1) into O(n). Hash tables fed by external input should use a hash function with a random seed.
- `reserve(n)` allocates enough buckets in advance, avoiding repeated rehashes while inserting.

### Custom types as keys

The standard library provides no hash function for `std::pair`. Measured: using a `pair` directly as a key fails to compile with `use of deleted function 'std::unordered_map<...>::unordered_map()'`. You have to provide one:

```cpp
struct PairHash {
    std::size_t operator()(const std::pair<std::uint32_t, std::uint32_t>& key) const noexcept {
        return std::hash<std::uint64_t>{}((std::uint64_t{key.first} << 32U) | key.second);  // two 32-bit values packed into 64 bits
    }
};
std::unordered_map<std::pair<std::uint32_t, std::uint32_t>, int, PairHash> modifiers;
```

Where `ConfigStore` and the config compiler need `(buff_id, sequence)` as a key, they use the ordered `std::set<std::pair<...>>` and `std::map<std::pair<...>>`, which sidesteps the problem while also guaranteeing deterministic iteration order (lesson 10).

## 5. `map` / `set`: red-black trees

- The underlying structure is a **red-black tree** (a self-balancing binary search tree); lookup, insertion and deletion are all **O(log n)**.
- Iteration is in key order, and the comparator must be a strict weak ordering (lesson 5).
- Each element is also a node, so **insertion and deletion don't affect other elements' iterators** (only the erased one is invalidated).
- `lower_bound` / `upper_bound` support range queries, such as "every buff with an ID between 800 and 899".

| | `unordered_map` | `map` |
|---|---|---|
| Lookup | O(1) average, O(n) worst | O(log n) |
| Ordered iteration | ❌ | ✅ |
| Range queries | ❌ | ✅ |
| Memory | Nodes + bucket array | Nodes (each with 3 pointers + a color) |
| Used in the project for | Looking up units by ID (`unit_index`), the config store | The config compiler (deterministic output), cycle detection |

## 6. Other containers

| Container | Structure | Characteristics | In the project / practice code |
|---|---|---|---|
| `std::array<T, N>` | Fixed array on the stack | Zero overhead | `std::array<Side, 2> order`, `passives_by_trigger` |
| `std::deque<T>` | Contiguous blocks in segments | O(1) insertion at both ends; `push_back` **doesn't move existing elements** | The TCP server's `pending` request queue |
| `std::list<T>` | Doubly linked list | O(1) insertion and deletion anywhere, but very slow iteration | Not used |
| `std::span<T>` | A view | Owns no data (lesson 9) | `term::Reader` |

On `deque`, measured:

```
after 100,000 push_backs  deque first-element address unchanged: 1   vector first-element address unchanged: 0
```

A `deque` is made of several fixed-size memory blocks; when one fills up a new one is opened and the existing blocks stay put, so references to existing elements aren't invalidated (but iterators are).

On `list`: in theory insertion and deletion anywhere are O(1), but every node is allocated separately and scattered around memory, so nearly every step of iteration is a cache miss. In practice, **even when you need to insert and delete in the middle, `vector` is often faster than `list` as long as there aren't too many elements**.

## 7. The iterator invalidation table

A high-frequency interview question.

| Container | Insertion | Deletion |
|---|---|---|
| `vector` | Without growth: everything after the insertion point is invalidated; **with growth: everything is invalidated** | Everything after the deletion point is invalidated |
| `deque` | At either end: **all iterators invalidated, references not**; in the middle: everything invalidated | At either end: only the erased element; in the middle: everything invalidated |
| `list` | Nothing invalidated | Only the erased element |
| `map` / `set` | Nothing invalidated | Only the erased element |
| `unordered_map` / `unordered_set` | **On rehash, all iterators invalidated, references and pointers not**; without rehash, nothing invalidated | Only the erased element |

Mapped to the project:
- `units` (`vector`): never grows or shrinks during a battle, so indices and references can be held for a long time (lesson 2).
- `buffs` (`vector`): grows and shrinks, so it's looked up again by unique ID, and erasure uses `it = erase(it)` (lesson 6).
- `connections_` (`unordered_map`): insertion doesn't affect references to existing elements, and deletion affects only the erased one (lesson 19).

## 8. Complexity of common algorithms

| Algorithm | Complexity | Notes |
|---|---|---|
| `std::sort` | O(n log n) | Introsort (quicksort + heapsort + insertion sort), **not stable** |
| `std::stable_sort` | O(n log n) | Merge sort, stable, needs extra memory |
| `std::nth_element` | O(n) average | Finds only the k-th smallest element, e.g. "the top 3 damage dealers" |
| `std::partial_sort` | O(n log k) | Sorts only the first k |
| `std::lower_bound` | O(log n) | Binary search in a **sorted** range |
| `std::find_if` | O(n) | Linear search |
| `std::remove_if` + `erase` | O(n) | Lesson 6 |

`TargetSelector` sorts the candidates and keeps only the first N, so strictly speaking it could use `std::partial_sort` or `std::nth_element`. But there are at most a handful to a dozen or so candidates, and `std::sort` is fast enough. **Knowing there's a better algorithm while also knowing it isn't worth switching here** is the best answer in an interview.

## 9. Back to the three numbers from the start

| Observation | Cause | What can be done |
|---|---|---|
| 1093 allocations to simulate one battle | `events` grows by doubling; every effect execution's `TargetSelector::select` returns a new `vector`; every round's `acting_order` returns a new `vector`; triggers with buff reactions fill a new `pending` list (an empty vector allocates nothing itself) | `reserve` after estimating the event count; make the candidate list a reused member buffer |
| 2 MB allocated to output 148 KB | `initializer_list` deep copies; a complete `Value` tree is built | Build with moves (about 1.9× faster) or stream bytes directly (about 4× faster), lessons 13, 21 |
| `sizeof(Event)` = 136 | Two `std::string`s take 64 bytes | Change them to `enum : uint8_t` and convert to atoms when encoding |

Lesson 21 measures some of these to see how much faster the changes really make things.

## Interview questions

**Q1: How does `vector` grow? Why 2× (or 1.5×)?**
When capacity runs out it allocates larger memory (2× for GCC, 1.5× for MSVC), moves the elements over and frees the old memory. Growing geometrically makes `push_back` O(1) amortized. The advantage of 1.5× is that several previously freed blocks combined may be reusable, whereas with 2× they can never add up to the next size needed.

**Q2: What's the difference between a `vector`'s `size` and `capacity`? Does `clear()` free memory?**
`size` is the number of elements; `capacity` is how many elements the allocated space can hold. `clear()` only destroys elements; `capacity` stays the same and memory isn't freed. To free it, use `shrink_to_fit()` (not guaranteed) or `swap` with an empty vector.

**Q3: What is SSO?**
The small string optimization: a `std::string` object has a small built-in buffer, and short strings (up to 15 characters in libstdc++) are stored directly inside the object with no heap allocation. Measured: a string of length 15 allocates 0 times, length 16 allocates once.

**Q4: How does `unordered_map` resolve hash collisions? When does it rehash?**
The standard library implementations use separate chaining: one linked list per bucket. When elements / buckets exceeds `max_load_factor` (1.0 by default), it rehashes: the bucket array grows (to a prime in libstdc++) and every element is redistributed. A rehash invalidates iterators, but references and pointers to elements stay valid.

**Q5: How do you choose between `map` and `unordered_map`?**
For ordered iteration, range queries or a stable worst case, use `map` (a red-black tree, O(log n)). For lookup by key only, chasing average speed, use `unordered_map` (O(1) on average). Wherever iteration order could affect the result (when you need determinism, say), don't iterate an `unordered_map`.

**Q6: What are the iterator invalidation rules for each container?**
See the table in section 7. The three most commonly tested: `vector` invalidates everything on growth; `list` / `map` invalidate only the erased element; `unordered_map` invalidates all iterators on rehash, but not references.

**Q7: What's special about `std::vector<bool>`?**
It's a specialization stored as packed bits; `operator[]` returns a proxy object rather than a `bool&`, you can't take an element's address, and you can't pass it as an ordinary bool array to an interface expecting `bool*`.

**Q8: In theory `std::list` inserts and deletes in O(1); why is it often slower than `vector` in practice?**
Each node is allocated separately and scattered around memory, so iteration causes frequent cache misses, whereas `vector`'s memory is contiguous and benefits from CPU prefetching. When there aren't many elements, `vector` is still faster overall, even though inserting or deleting in the middle moves more elements.

Next: [Lesson 17: Threads, locks and thread pools](17-threads-and-pools.md)
