# Lesson 23: Interview question bank

[中文](../23-interview-questions.md) | **English**

> Part 6, "Job hunting". This lesson collects the interview questions from the previous 22 lessons, and adds topics the main lessons didn't expand on but that C++ game-server roles often ask about: operating systems, databases and caching, and algorithms.
>
> How to use it: cover the answer and say it yourself first; if you can't explain it clearly, go back and reread the matching lesson. Questions marked "(lesson N)" are explained in full, with measured data, in lesson N; numbers marked "measured" can all be quoted in interviews.

## A. C++ basics

**1. What's the difference between `struct` and `class`?**
Only the default access differs: `struct` defaults to public, `class` to private. By convention `struct` is used for plain data and `class` for types with invariants to protect. (lesson 3)

**2. What's `sizeof` an empty class?**
1. Every object must have a unique address. But as a base class it can take 0 bytes (the **empty base optimization**): measured, `sizeof(Empty)=1`, a class deriving from an empty base plus one `int` is 4, and an empty class as a member plus an `int` is 8 (1 byte plus 3 bytes of alignment padding). C++20's `[[no_unique_address]]` lets members enjoy the same optimization.

**3. Struct memory alignment? How do you reduce padding?**
Each member is placed according to its own alignment requirement, and the struct's total size is a multiple of its largest alignment. Measured, `{char, int64, char}` is 24 bytes, while reordered as `{int64, char, char}` it's 16. **Ordering members from largest to smallest** reduces padding.

**4. What are the uses of `const`?**
On variables (can't be modified); on pointers (`const T*`: the pointee can't be changed; `T* const`: the pointer itself can't be changed); on reference parameters (a read-only borrow); on member functions (doesn't modify the object; `const` objects can only call `const` member functions). (lesson 2)

**5. What's the difference between `const` and `constexpr`?**
`const` means "can't be modified at run time", and the value may be determined only at run time; `constexpr` requires evaluation at compile time. `constexpr` functions can be called at compile time, for generating lookup tables and the like. (lesson 15)

**6. What are the meanings of `static`?**
A `static` variable in a function: initialized once, lives until the program ends, and since C++11 its initialization is thread-safe (lesson 9's `static Handler`); a `static` class member: belongs to the class, not to objects (lesson 5's `TargetSelector::select`); a `static` function or variable at file scope: visible only in that file, written today with an anonymous `namespace` (lesson 1).

**7. What's the difference between `#define` and `const` / `constexpr`?**
Macros are pure text substitution at the preprocessing stage, with no type, no scope, invisible to debuggers, and prone to precedence problems. Constants and `constexpr` have type checking and scope. Apart from conditional compilation (`#ifdef _WIN32`, lesson 9) and the few cases needing `__FILE__` or `#` stringizing (lesson 11's `CHECK` macro), use constants instead of macros.

**8. What does the `inline` keyword mean today?**
Its main meaning has become "may be defined in several translation units (as long as the definitions are identical)", which is why functions defined in headers need `inline`, or linking fails with duplicate definitions. Whether a call is actually inlined is up to the compiler. C++17 `inline` variables work the same way.

**9. What is `extern "C"` for?**
It gives a function C naming rules, without C++ name mangling. Measured, `nm` shows the C++ function `cpp_linkage(int)` as the symbol `_Z11cpp_linkagei`, while the `extern "C"` function `c_linkage` is just `c_linkage`. You need it when calling to and from C libraries and when exporting shared-library interfaces; Erlang's NIF interface (`erl_nif.h`) is a C interface.

**10. What's the difference between `new` / `delete` and `malloc` / `free`?**
`new` allocates memory and calls the constructor, throwing `std::bad_alloc` on failure; `malloc` only allocates raw memory and returns `NULL` on failure. `delete` calls the destructor and then frees. They can't be mixed; `new[]` must be paired with `delete[]`, and measured, `new std::string[3]` paired with `delete` is reported as an error by ASan. Modern C++ should use `make_unique` / `make_shared`, with almost no need to write `new` by hand. (lesson 14)

**11. What is placement new?**
Constructing an object in existing memory: `new (buffer) std::string("...")`. Measured, the object's address is exactly `buffer`'s address. When done you must **call the destructor by hand**, not `delete`. Memory pools and `std::vector`'s internals depend on it.

**12. What's the difference between references and pointers?**
A reference must be initialized when defined, can't be rebound and can't be null; syntactically it's an alias for a variable. A pointer can be null and can be re-pointed, and members are accessed with `->`. For function parameters, use a reference for "always present" and a pointer for "may be absent". (lesson 2)

**13. Can you return a reference to a local variable?**
No. The local is destroyed when the function returns, and the returned reference dangles immediately. `-Wall` warns: measured, `warning: reference to local variable 'name' returned`. Return by value, and the compiler elides the copy (lesson 13).

**14. The four casts?**
`static_cast`: ordinary conversions checked at compile time. `dynamic_cast`: polymorphic downcasts checked at run time (lesson 12). `const_cast`: removes `const` (modifying an object that was originally `const` is undefined behavior). `reinterpret_cast`: bitwise reinterpretation, used essentially only for "viewing an object as bytes" (lesson 9); for bitwise conversion of values, use C++20's `std::bit_cast`.

**15. What is undefined behavior? Give some examples.**
Behavior whose result the standard doesn't specify; the compiler may assume it never happens and optimize accordingly. Examples: signed integer overflow (measured: a "compute then check" overflow test was deleted entirely at `-O2`, lesson 7), out-of-bounds access, use of freed memory, data races (lesson 17), null pointer dereference, deleting a derived object through a base pointer without a virtual destructor (lesson 12).

**16. What is `explicit` for?**
It stops a single-argument constructor from being used for implicit conversion. Measured, without `explicit`, writing `simulate(seed, battle_id)` instead of `simulate(battle_id, Random(seed))` compiles, and the two arguments are silently swapped. (lesson 3)

**17. Lambda capture modes? What are the traps?**
`[x]` by value, `[&x]` by reference, `[=]` / `[&]` automatic capture, `[this]` captures the current object. The trap: a lambda capturing by reference that's stored or handed to another thread and outlives the captured variables will dangle. (lessons 5, 17)

## B. Object model and move semantics

**18. How are virtual functions implemented?** (lesson 12)
**19. Why should a base-class destructor be virtual?** (lesson 12: measured, the derived destructor wasn't called, and ASan reported `new-delete-type-mismatch`)
**20. What happens when a constructor calls a virtual function?** (lesson 12)
**21. What is object slicing?** (lessons 8, 12)
**22. How do you solve diamond inheritance?** (lesson 12)
**23. The rules of zero, three and five?** (lesson 13)
**24. What happens if you declare only a destructor?** (lesson 13: measured, a "move" became 100 copies)
**25. What does `std::move` do?** (lesson 13)
**26. What's the difference between `std::move` and `std::forward`?** (lesson 13)
**27. Why should move constructors be `noexcept`?** (lesson 13: measured, 1023 copies during vector growth)
**28. RVO and NRVO? Is `return std::move(local)` good?** (lesson 13)
**29. What's the difference between `push_back` and `emplace_back`?** (lesson 13)
**30. What's the performance trap of `std::initializer_list`?** (lessons 13, 21: its elements are `const` and can only be copied, making this project's result encoding about 4× slower)

## C. Smart pointers and RAII

**31. What is RAII?**
Binding the acquisition and release of a resource to an object's construction and destruction, so the compiler guarantees release when the scope is left, exceptions included. `std::lock_guard`, `std::unique_ptr` and `FileDescriptor` are all examples. (lessons 8, 13, 14)

**32. What's the difference between `unique_ptr`, `shared_ptr` and `weak_ptr`?** (lesson 14)
**33. What makes `make_shared` better than `shared_ptr(new T)`?** (lesson 14: measured, 1 allocation vs 2)
**34. Is `shared_ptr` thread-safe?** (lesson 14: the count is safe, reads and writes of the same variable aren't; measured, TSan reports a data race)
**35. How do you fix reference cycles?** (lessons 2, 14)
**36. What does `enable_shared_from_this` do, and what are its limits?** (lesson 14)
**37. How should smart pointers be passed as function parameters?** (lesson 14: measured, passing a `shared_ptr` by value costs about 20 ns extra per call, about 300 ns under multithreaded contention)

**38. The three levels of exception safety?**
The basic guarantee: after an error the object is still valid with nothing leaked, but its state may have changed. The strong guarantee: it either succeeds or behaves as if nothing happened (lesson 10's `load_file`, `assign_loadout`). The no-throw guarantee: promises never to throw, marked `noexcept` (destructors, move operations, `swap`).

## D. Templates

**39. Why must templates go in headers?** (lesson 15: measured, `undefined reference`)
**40. Full vs partial specialization?** (lesson 15)
**41. SFINAE and concepts?** (lesson 15: measured, errors went from 109 lines to 16)
**42. What's the difference between `if constexpr` and an ordinary `if`?** (lesson 15)
**43. Variadic templates and fold expressions?** (lesson 15)
**44. What is CRTP?** (lesson 15)

## E. The STL

**45. How does `vector` grow?** (lesson 16: measured, GCC grows by 2×)
**46. `size` vs `capacity`; does `clear` free memory?** (lesson 16)
**47. What is SSO?** (lesson 16: measured, no allocation up to 15 characters)
**48. How is `unordered_map` implemented, how does it handle collisions, and when does it rehash?** (lesson 16)
**49. How do you choose between `map` and `unordered_map`?** (lesson 16)
**50. The iterator invalidation rules?** (lesson 16's table)
**51. What's special about `vector<bool>`?** (lesson 16)
**52. Why is `list` often slower than `vector` in practice?** (lesson 16)
**53. What are the requirements on a `std::sort` comparator?** (lesson 5: it must be a strict weak ordering; measured, using `<=` ran out of bounds)
**54. Why must `remove_if` be paired with `erase`?** (lesson 6)

## F. Concurrency

**55. What's the difference between a process and a thread?**
A process has its own address space, processes don't affect each other when they crash, and switching between them is expensive; threads in one process share the address space (heap, globals, file descriptors), each has its own stack and registers, switching is cheap, but one thread crashing takes the whole process down. An Erlang "process" is a lightweight process scheduled by the VM: conceptually closer to an isolated process, yet cheaper than a thread. (lesson 17)

**56. What is a data race? How do you find one?** (lesson 17: measured, about 20,000 updates lost; TSan pinpoints it)
**57. The conditions for deadlock and how to avoid it?** (lesson 17)
**58. What's the difference between `lock_guard`, `unique_lock` and `scoped_lock`?** (lesson 17)
**59. Why do condition variables need a check in a loop?** (lesson 17)
**60. What do you need to watch out for when writing a thread pool?** (lesson 17)
**61. How many threads?** (lesson 17: measured, 4 threads on 4 cores give 3.5×; more threads mean more lock contention)
**62. What's the difference between `atomic` and `volatile`?** (lesson 18)
**63. The memory orders relaxed / acquire / release / seq_cst?** (lesson 18)
**64. Why do wrong memory orders often pass tests?** (lesson 18: x86's TSO model masks the problem)
**65. What is false sharing?** (lesson 18: measured, 4× slower)
**66. CAS and the ABA problem?** (lesson 18)
**67. How does an SPSC lock-free queue work?** (lesson 18: measured, 3–10× faster than a mutex queue)

## G. Networking

**68. What's the difference between TCP and UDP? Which should a game use?**
TCP: connection-oriented, reliable, ordered, with flow and congestion control; when a packet is lost, later data waits for the retransmission (head-of-line blocking). UDP: connectionless, with no guarantee of reliability or order, low overhead and low latency. Turn-based games and most MMOs are fine with TCP; extremely latency-sensitive action and shooter games often use UDP with their own reliability on top (KCP, for example: a reliable UDP protocol that trades more bandwidth for lower latency).

**69. The three-way handshake and four-way teardown? Why does teardown take four steps?** (lesson 19)
**70. TIME_WAIT and CLOSE_WAIT?** (lesson 19: lots of CLOSE_WAIT usually means a forgotten `close`)
**71. What's the difference between select, poll and epoll?** (lesson 19)
**72. epoll's LT and ET?** (lesson 19)
**73. Coalesced and partial packets?** (lesson 19: measured, a frame sent byte by byte is reassembled correctly)
**74. Reactor and Proactor?** (lesson 19)
**75. How do you wake a thread sleeping in `epoll_wait`?** (lesson 19: eventfd; measured, multiple writes merge into one read)
**76. Nagle's algorithm and `TCP_NODELAY`?** (lesson 19)
**77. What is SIGPIPE?** (lesson 19)
**78. How does a server apply backpressure?** (lesson 19)

**79. How do you implement heartbeats? Why not rely only on TCP keepalive?**
Send heartbeat packets periodically at the application level, and treat a connection as dead and clean it up when nothing arrives for a while. TCP keepalive only starts probing after 2 hours by default, and it only shows the peer's kernel is alive, not that the application isn't hung. On the server, a timing wheel manages timeout checks for large numbers of connections (lesson 20).

## H. Operating systems

**80. What is virtual memory? What are its benefits?**
Each process sees its own contiguous virtual address space, mapped to physical memory by the MMU through page tables. Benefits: isolation between processes; only pages actually used need to be in physical memory (demand paging); memory can be overcommitted. Lesson 22's problem 2 is an example: 3.8 GB of virtual memory reserved while only 4 MB of physical memory was used.

**81. What is a page fault?**
When a virtual page accessed hasn't been mapped to physical memory yet, the CPU raises a page-fault exception; the kernel allocates a physical page (or reads it back from disk), sets up the mapping, and execution resumes. The first writes to a freshly allocated large block cause lots of page faults, which is one reason for "warm-up" (lesson 21).

**82. User mode and kernel mode? The cost of a system call?**
Applications run in user mode; accessing hardware, files or the network requires a system call into kernel mode, switching privilege levels and saving and restoring registers, typically a few hundred nanoseconds to a few microseconds per call. High-performance servers try to make fewer system calls: epoll returning several events at once, eventfd merging notifications, batched reads and writes, `io_uring` (lesson 19's strace statistics).

**83. What are the ways processes can communicate?**
Pipes (this project's Erlang Port communicates through standard input/output pipes, lesson 9), Unix domain sockets, TCP, shared memory (the fastest, but you must synchronize yourself), message queues, signals.

**84. What is copy-on-write in `fork`?**
After `fork`, parent and child share the same physical pages, all marked read-only; a page is copied only when either side writes to it. That's why `fork` is fast, and Redis's RDB persistence uses it to save a snapshot in a child process.

**85. What is a context switch? Why aren't more threads always better?**
The CPU switching from one thread to another must save and restore registers and may flush caches and the TLB. When threads far outnumber cores, lots of time goes to switching and lock contention. (lesson 17: measured, with 4 worker threads the scheduling overhead of empty tasks is 10× that of 1 thread)

**86. What are signals? How are they handled in a multithreaded program?**
The kernel's mechanism for notifying a process asynchronously, such as SIGSEGV (segmentation fault), SIGPIPE, SIGTERM. In a multithreaded program a signal may be delivered to any thread. The recommended approach: block signals before creating threads and handle them uniformly in the event loop with `signalfd` (lesson 19).

**87. What usually causes a segmentation fault?**
Accessing memory that isn't mapped or that you lack permission for: null pointers, out-of-bounds access, use of freed memory, stack overflow (recursion too deep; lesson 6's depth limit exists to prevent this). Locate it with a core dump + gdb (lesson 22).

## I. Databases and caching

**88. Why do MySQL indexes use B+ trees?**
A B+ tree is short and wide, with one node per disk page, so a few levels hold millions of records and a lookup takes only a few disk reads; all data lives in the leaves, which are linked into a list, making range queries fast. Hash indexes don't support range queries, and binary trees are too tall.

**89. Clustered vs secondary indexes? What is a "back-to-table" lookup?**
The leaves of InnoDB's primary key index store whole rows (the clustered index); the leaves of a secondary index store primary key values, so after finding the key you go back to the primary index to fetch the row, which is the "back-to-table" lookup. When all the queried columns are in the secondary index (a covering index), no back-to-table lookup is needed.

**90. Transaction ACID? What isolation levels are there?**
Atomicity, consistency, isolation, durability. Isolation levels from lowest to highest: read uncommitted, read committed, repeatable read (InnoDB's default), serializable, which progressively solve dirty reads, non-repeatable reads and phantom reads. InnoDB implements repeatable read with MVCC and gap locks.

**91. Why is Redis fast? Its common data structures?**
Data lives in memory, commands execute on a single thread (no lock contention), and networking uses I/O multiplexing. Common structures: String, Hash, List, Set, ZSet (skiplist + hash table, for leaderboards, lesson 20).

**92. Redis's persistence options?**
RDB: periodically `fork` a child process to write a memory snapshot to disk; fast to recover, but data after the last snapshot may be lost. AOF: log every write command, optionally flushed to disk every second; safer data, bigger files. Production often combines both.

**93. Cache penetration, breakdown and avalanche?**
Penetration: queries for data that doesn't exist at all hit the database every time; solved with a Bloom filter or by caching empty values. Breakdown: at the instant a hot key expires, lots of requests hit the database at once; solved with a mutex around rebuilding the cache, or by never expiring hot keys. Avalanche: many keys expire at once or the cache service goes down; add random jitter to expiry times, use multi-level caches, rate-limit and degrade.

**94. How do games persist player data?**
Load it into memory when the player logs in, mark it dirty when modified, and write it back to the database in batches periodically or at logout; operations involving currency or items write a transaction log first; settlement is made idempotent with `battle_id`. (lesson 20)

## J. Game servers

**95. Describe your server architecture.** (lesson 20)
**96. State sync vs lockstep?** (lesson 20)
**97. How does lockstep guarantee determinism?** (lesson 20's table: no floating point, your own RNG, no reliance on hash-table order, no undefined behavior)
**98. How are timers implemented?** (lesson 20: measured, a timing wheel's firing phase is about 5× faster)
**99. How do you do AOI?** (lesson 20: measured, the grid is about 140× faster)
**100. How do you build a leaderboard?** (lesson 20: measured, a skiplist rank lookup takes about 2 µs)
**101. Consistent hashing and virtual nodes?** (lesson 20: measured, 24% moved vs 75%)
**102. ECS and data-oriented design?** (lesson 20: measured, SoA about 13× faster)
**103. How do you do hot reloading?** (lessons 10, 20)
**104. How do you do battle replays?** (lesson 20: storing the request instead of the result saves about 17×)

**105. How do you prevent client cheating?**
Server authority: every computation that affects the outcome happens on the server, and the client only submits input; results reported by the client are verified by recomputing on the server with the same input (requires determinism, lesson 20); check the frequency and plausibility of key actions (movement speed, say); encrypt and sign the protocol.

## K. Performance and debugging

**106. Tell me about a performance optimization you did.** (lesson 21)
**107. How do you write a trustworthy benchmark?** (lesson 21: this course itself fell into the "deleted by the optimizer" trap twice)
**108. Common profiling tools?** (lesson 21)
**109. Amdahl's law?** (lesson 21)
**110. Average latency vs p99?** (lesson 21)
**111. How do you investigate a production crash?** (lesson 22)
**112. How do you investigate a hung program?** (lesson 22)
**113. How do you investigate a memory leak?** (lesson 22)
**114. What is fuzzing?** (lesson 22)

## L. Project deep-dives (about this project)

When an interviewer sees the project on your résumé, they will dig in. You should be able to talk about each of these for 2–3 minutes without notes:

**115. Why write battles in C++ instead of just using Erlang?**
Battles are compute-heavy: thousands of events, recursive triggers, constant arithmetic. Erlang excels at concurrency and fault tolerance, not at this kind of tight numeric computation. Writing battles in C++ while Erlang handles connections, business logic and scheduling plays to each one's strengths.

**116. How do you choose between Port, NIF and TCP?** (the comparison tables in lessons 9, 19)

**117. How do you guarantee the same seed gives exactly the same result?** (lessons 3, 5, 7, 17, 20)

**118. Is your engine thread-safe? How do you prove it?** (lesson 17: state isolation by design; measured, 4 threads give byte-identical results, 0 TSan warnings)

**119. How does config hot reloading work? Are battles in progress affected?** (lessons 9, 10: load outside the lock, swap the pointer inside it; battles already started hold a snapshot of the old `shared_ptr`; if loading fails, the old config keeps serving)

**120. How is the buff system designed? Why split it into four components?** (lessons 1, 6, `docs/buff-v2-design.md`: four orthogonal components, lifetime, stacking, attribute modification and event reactions, so new gameplay needs only config, not code)

**121. How do you stop passive skills from chain-triggering forever?** (lesson 6: a depth limit of 32 + an event limit + per-round trigger limits)

**122. Why no floating point?** (lessons 5, 7)

**123. What bugs did you find in the project?** (lesson 22's problems 1 and 2)

**124. If battle volume grew 10×, how would you scale?**
First do lesson 21's optimization to double single-thread throughput; use multiple cores with a thread pool or the NIF's dirty thread pool (measured: 3.5× on 4 cores); split battles into a standalone TCP service (lesson 19), sharded across machines by consistent hashing on `battle_id` (lesson 20); apply backpressure and timeout control on the Erlang side.

## M. Erlang vs C++

**125. Erlang processes vs C++ threads?** (lesson 17)
**126. How do you implement Erlang's "let it crash" in C++?**
C++ has no process isolation; a crash anywhere takes the whole process down. The approach is to put the parts that might crash in a separate OS process (this project's Port) and have an external supervisor (Erlang's supervision tree, systemd, k8s) restart it. Inside the process, exceptions plus a `catch (...)` at the boundary turn errors into return values (lesson 8).

**127. Erlang's GC vs C++ memory management?**
Each Erlang process has its own heap, collected per process with no global pauses; large binaries live on a shared heap with reference counting. C++ has no GC and frees deterministically through RAII and smart pointers; `shared_ptr` resembles large-binary reference counting but can't handle cycles (lessons 2, 14).

**128. Erlang's hot code loading vs C++ hot reloading?** (lesson 20)

**129. Why are NIFs dangerous?** (lessons 8, 9: they run inside the BEAM process, so a crash or a leaked exception takes the whole node down; long-running work needs a dirty scheduler)

## N. Algorithms

This course doesn't systematically cover algorithm problems, but nearly every company tests them in written exams and interviews. Advice:

- **What to practice**: LeetCode's "Top 100 Liked" covers most interviews; focus on arrays and two pointers, hash tables, linked lists, stacks and queues, binary trees, heaps, binary search, dynamic programming, graph BFS/DFS, and union-find. Practice in C++ and get fluent with the STL along the way.
- **Algorithms common in games**, which interviews may ask about directly:
  - **Weighted random** (loot tables): prefix sums + binary search, O(log n); the alias method gets O(1).
  - **Shuffling**: the Fisher-Yates algorithm; `std::shuffle` with a fixed-seed random engine reproduces results (but remember lesson 3: engine algorithms are fixed, while "distributions" may differ between standard libraries).
  - **Pathfinding**: A* (Dijkstra plus a heuristic); large maps use hierarchical pathfinding or navigation meshes.
  - **Top K**: a heap (`std::priority_queue`) or `std::nth_element` (lesson 16).
  - **Rate limiting**: token bucket, leaky bucket.
  - **LRU cache**: hash table + doubly linked list.

Next: [Lesson 24: Résumé and project pitch](24-resume-and-pitch.md)
