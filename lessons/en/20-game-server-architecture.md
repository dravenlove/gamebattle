# Lesson 20: Game server architecture and common data structures

[中文](../20-game-server-architecture.md) | **English**

> Practice code: `practice/ds/` (a skiplist leaderboard, a timing wheel, grid AOI, consistent hashing, an AoS/SoA comparison), all with correctness checks and measured data.
>
> Beyond C++ itself, game-server interviews always ask "how are your servers partitioned", "how are battles synchronized", "how do you build a leaderboard", "how are timers implemented". This lesson ties those high-frequency questions to this project.

## 1. A typical game server process layout

```
               ┌──────────┐
  client ─────▶│ gateway  │  long-lived connections, encryption, protocol encoding, rate limiting
               └────┬─────┘
       ┌────────────┼───────────────┬──────────────────┐
       ▼            ▼               ▼                  ▼
  ┌──────────┐ ┌─────────────┐  ┌──────────────────┐ ┌──────────┐
  │ login    │ │ logic/scene │  │ match/social     │ │ chat     │
  └──────────┘ └──────┬──────┘  └──────────────────┘ └──────────┘
                      │  battle request (battle_id, both lineups, seed)
                      ▼
            ┌────────────────┐
            │ battle service ◀── this project: Port / NIF / TCP battle server (lesson 19)
            └──────┬─────────┘
                   │  battle result → settlement
                   ▼
            ┌──────────────────┐      ┌────────┐
            │ data proxy/cache │─────▶│ MySQL  │  persistence
            │     (Redis)      │      └────────┘
            └──────────────────┘
```

A common combination at many Chinese game companies: **Erlang for gateways, logic, social and other services with many connections, lots of state and high availability requirements**; **C++ for battle, scenes, pathfinding and other compute-heavy services**. Knowing both Erlang and C++, you can explain exactly how the two sides divide the work and connect (the trade-offs of the three approaches, Port, NIF and TCP, lessons 9, 19), which is a strong selling point.

## 2. How battles are synchronized: three models

| Model | How | Suits | Determinism requirement |
|---|---|---|---|
| **Server-authoritative resolution** (this project) | The client submits only lineups and commands; the server computes the whole battle and sends the event stream to the client for playback | Turn-based, card games, idle games | The server only needs to reproduce itself |
| **State sync** | The server runs the game logic and periodically broadcasts each entity's state (position, HP…) to clients | MMOs, shooters | Low: the client only displays |
| **Lockstep (frame sync)** | The server only relays every player's input for each frame, and every client runs exactly the same logic | MOBAs, RTS, fighting games | **Extremely high**: every client must compute exactly the same result, and any difference means "desync" |

The determinism required by lockstep and battle replays is exactly what this project already achieves. A recap of every safeguard covered so far:

| Requirement | How | Lessons |
|---|---|---|
| No floating point | Everything in integers and basis points | Lessons 5, 7 |
| Cross-platform identical random numbers | A hand-written SplitMix64, not the standard library's distributions | Lesson 3 |
| No reliance on unspecified order | Never iterate an `unordered_map`; sorts always end by comparing a unique ID | Lessons 1, 5 |
| No undefined behavior | Saturating arithmetic so signed values never overflow | Lesson 7 |
| Multithreading doesn't affect results | Each battle's state is independent | Lesson 17 (measured: 4 threads, byte-identical results) |

### A direct payoff of determinism: store the request, not the result

This project's measurements: a 5v5 battle's **request** encodes to 8.6 KB, and its **result** (912 events) to 148 KB. To keep battle reports for players to rewatch, you only need to store "the request + the engine version" and recompute at playback, **cutting storage about 17×**.

The same idea works for **anti-cheat**: the client reports "I won", the server recomputes with the same input, and if the results differ, it's cheating or a tampered client.

The precondition is what lesson 3 stressed: **the engine version must match**. Change a single random roll and old requests no longer compute old results. So battle reports must record the engine version, and the server must be able to find that version of the engine.

## 3. Timers: min-heap vs timing wheel

Game servers are full of timers: buff expiry, skill cooldowns, event start times, disconnect timeouts, heartbeat checks… possibly hundreds of thousands at once.

**Min-heap** (`std::priority_queue`): ordered by expiry time, looking only at the top. Insert and pop are O(log n).

**Timing wheel**: time is cut into fixed slots (say 10 ms each) arranged in a ring. A timer goes into the slot for its expiry time, and each tick processes only the current slot:

```
          the tick pointer advances one slot every 10 ms
                 │
   ┌───┬───┬───┬─▼─┬───┬───┬─── ... ───┐
   │ 0 │ 1 │ 2 │ 3 │ 4 │ 5 │   1023    │   1024 slots, one lap is 10.24 seconds
   └───┴───┴───┴─┬─┴───┴───┴─── ... ───┘
                 └─▶ [timer A, rounds=0] [timer B, rounds=2]   timers beyond one lap record how many more laps to go
```

Insertion is O(1); each tick processes one slot. Measured with 1 million timers whose expiry times are spread randomly between 1 and 30000 ticks (`practice/ds/timers.cpp`):

```
min-heap     : insert 36 ms, advance and fire 506 ms
timing wheel : insert 26 ms, advance and fire 103 ms
both fired exactly the same timers at exactly the same ticks: 1
```

In the firing phase the timing wheel is about **5× faster**, and the program verified with a checksum that the two implementations fire at exactly the same moments.

- When delays span a huge range (milliseconds to days), timers in a single-level wheel that "still have many laps to go" get scanned over and over. Industrial implementations use **hierarchical timing wheels** (like a clock's second, minute and hour hands): the Linux kernel, Kafka and Netty all do this.
- **Cancelling timers**: a timing wheel can give each timer a handle and remove it straight from its slot on cancellation (O(1) with an intrusive linked list); a min-heap can't conveniently remove middle elements, so "mark it and skip it when popped" lazy deletion is common.
- The Erlang VM's timers (`erlang:send_after`, `receive ... after`) are also built on a timing wheel internally.

## 4. AOI: only tell players about nearby things

AOI (Area of Interest): in an MMO, a player only needs to know about the other players and monsters within view. The most direct approach iterates over every entity computing distances each time, O(N).

**The nine-cell grid**: cut the map into cells whose side equals the view radius, each recording which entities it contains. A query only needs to look at its own cell and the 8 around it:

```
┌────┬────┬────┐
│    │    │    │
├────┼────┼────┤      view radius = cell side
│    │ me │    │      only these 9 cells need checking
├────┼────┼────┤
│    │    │    │
└────┴────┴────┘
```

Measured with 20,000 entities and 20,000 view queries (`practice/ds/aoi.cpp`):

```
brute force: 400.7 ms
grid       : 2.8 ms
both methods found the same total number of entities: 1 (125096)
```

About **140× faster**, with exactly the same results.

A real AOI system also has to handle **enter-view and leave-view events**: after an entity moves, compare the old and new view sets, sending "enter" for additions and "leave" for removals. Another common implementation is the **cross-linked list**: an ordered list per x coordinate and per y coordinate, which suits maps where entities are very unevenly distributed.

## 5. Leaderboards: skiplists

The requirement: hundreds of thousands to millions of players with frequently changing scores, quickly answering "what's my rank" and "who's number 100".

`std::set` can keep scores sorted, but finding a rank means counting from the start one by one (`std::distance`), O(n).

**Redis's ZSET** uses a **skiplist + hash table**. A skiplist is a multi-level ordered linked list where upper levels are "express lanes" over lower ones:

```
level 3: head ──────────────────────────▶ 90 ──────────────────────▶ nil
level 2: head ─────────▶ 120 ───────────▶ 90 ─────────▶ 60 ────────▶ nil
level 1: head ─▶ 150 ─▶ 120 ─▶ 100 ─▶ 95 ─▶ 90 ─▶ 80 ─▶ 60 ─▶ 40 ─▶ nil
```

Each node randomly decides how many levels it has (each extra level with probability 1/4); a search starts at the top level and jumps whenever it can, expected O(log n). **The key trick**: every forward pointer also records a `span`, the number of nodes that step skips. Summing the spans along the search path gives the rank.

`practice/ds/ranklist.hpp` implements a skiplist on the same lines as Redis (about 150 lines), supporting insert, delete, rank lookup by (score, id), and id lookup by rank. Measured:

```
60,000 random inserts/deletes/updates, 8571 rank queries all match std::set exactly
200,000 players: one skiplist rank lookup 2.0–2.5 us, std::set + distance 18000 us
```

Correctness was checked query by query against `std::set` as a reference, and run under AddressSanitizer + UBSan. For performance, with 200,000 players it's about **8000×** faster.

Common follow-up questions in interviews:
- **Why does Redis use a skiplist rather than a red-black tree?** A skiplist is much simpler to implement; a range query ("ranks 100–200") just finds the starting point and walks the bottom list; rank lookup only needs the `span` field, whereas a red-black tree would have to maintain subtree sizes.
- **How are ties ordered?** This implementation orders by ascending id, making the order total and the result deterministic (lesson 5's rule). You could also put "whoever reached the score first" ahead by encoding a timestamp into the score.
- **What about hundreds of millions of players?** Exact ranks usually aren't needed: maintain the top 10,000 exactly in a skiplist, and for the rest count players per score bucket to give approximate ranks like "you beat 87% of players".

## 6. Consistent hashing: how battle servers are sharded

With 3 battle servers, pick one by `battle_id`. The simplest is `hash(battle_id) % 3`. The problem: after scaling to 4, the results of `% 4` almost all change. If the servers hold caches or state (say consecutive gauntlet battles must land on the same server), they're invalidated wholesale.

**Consistent hashing**: hash both servers and keys onto a ring; a key belongs to the first server found going clockwise. Adding a server only takes over the keys in the stretch "just before" it on the ring.

```cpp
class HashRing {
    std::map<std::uint64_t, int> ring_;            // position on the ring → server
public:
    void add(int server) {
        for (int v = 0; v < virtual_nodes_; ++v) ring_[mix((server << 32) | v)] = server;   // several virtual nodes per server
    }
    int pick(std::uint64_t battle_id) const {
        auto it = ring_.lower_bound(mix(battle_id));   // the first node clockwise: std::map's ordering comes in handy
        return it == ring_.end() ? ring_.begin()->second : it->second;
    }
};
```

Measured with 300,000 battles (`practice/ds/consistent_hash.cpp`):

```
consistent hashing (1 virtual node per server):    distribution over 3 servers 35029/258472/6499, adding a 4th moves 40.5% of battles
consistent hashing (160 virtual nodes per server): distribution over 3 servers 94458/103955/101587, adding a 4th moves 24.1% of battles
modulo hash % N: going from 3 to 4 servers moves 74.9% of battles
```

- The modulo approach moved 75% of keys; consistent hashing moved only 24%, close to the theoretical optimum of 25% (the new server should take a quarter).
- **Virtual nodes** are crucial: with only 1 node per server, the gaps on the ring are very uneven and one server carries 86% of the traffic. With 160 virtual nodes each, the three servers' loads are roughly balanced.

## 7. Data-oriented design and ECS

Lesson 16 noted that `sizeof(Event)` is 136 bytes. If you only need to process one of its fields at a time, the CPU still pulls all 136 bytes into the cache, wasting most of the bandwidth.

Measured with 2 million units, subtracting 1 from every unit's hp per pass, for 20 passes (`practice/ds/aos_soa.cpp`):

```
sizeof(UnitAoS)=144
AoS (array of structs)       : 377 ms
SoA (one array per field)    : 29 ms   results match: 1
```

**About 13× faster.**

- **AoS** (Array of Structures): `vector<Unit>`, with all of a unit's fields next to each other. The natural object-oriented layout.
- **SoA** (Structure of Arrays): one array per field, with all values of the same field next to each other. When only hp is processed, every byte pulled into the cache is useful, and the compiler can use SIMD instructions to process several values at once.

**ECS** (Entity-Component-System) is an architecture that systematizes this idea, popular in both game clients and servers:
- **Entity**: just an ID;
- **Component**: pure data (position, HP, movement speed…), with components of the same type stored in contiguous arrays;
- **System**: iterates over entities that have certain components and runs one piece of logic (a movement system, a combat system, a buff system).

It's the same direction as lesson 12's "why not a virtual-function inheritance hierarchy": **organize data by how it's accessed, not by "what kind of thing it is"**. This project's battles have only about ten units, where AoS is perfectly fine; but in an MMO scene with thousands of entities, this difference decides how many players one server can carry.

## 8. Hot reloading

| Layer | How | This project |
|---|---|---|
| Config | Reload the config file and swap the pointer atomically; requests already started keep the old config | ✅ `load_config` (lesson 10) |
| Script logic | Write business logic in a script language like Lua with C++ providing engine APIs; reload the scripts to update the logic | Not used |
| Native code | `dlopen` a new version of the shared library; but existing objects' vtables, static variables and functions mid-execution can all break, so the risk is high | Not used |
| Process level | A new-version process starts and takes over traffic while the old one finishes in-flight requests and exits | Restarting the Port process in Port mode works this way |

Erlang supports hot code loading natively (old and new versions of the same module can run side by side), one of the reasons Erlang is popular for game servers and something C++ struggles to do. Being able to explain this comparison in an interview is your advantage.

## 9. Storage and settlement

- **Redis for caching, MySQL for persistence**: player data is loaded into memory (or Redis) and modified there, marked "dirty", and written back to MySQL in batches periodically or at logout.
- **Settlement must be idempotent**: after a network timeout, the client or logic server may resend the settlement request for the same battle. Use `battle_id` as a unique key to guarantee each battle is settled only once. This project's gauntlet (`gamebattle_gauntlet.erl`), when it fails partway, returns the completed results and the last `carryover` so the run can resume from that point, which is the same idea.
- **Write the log first, then change the state**: for operations involving items or currency, write the transaction log first, then modify the player data; crash recovery trusts the log.

## Interview questions

**Q1: Describe your server architecture.**
(Answer using section 1.) A gateway holds long-lived connections, logic servers handle player business, and battle is a standalone service; the battle service takes only "lineups + seed" and returns the event stream and result. Erlang handles connections and business logic, C++ handles battle computation, with three ways to connect: Port, NIF and TCP.

**Q2: What's the difference between state sync and lockstep? What do you need to watch out for with lockstep?**
In state sync the server computes and broadcasts state and the client only displays it; in lockstep only inputs are synchronized and every client runs the same logic itself. Lockstep demands strict determinism: no floating point (or use fixed point), the same random algorithm and seed, no reliance on hash-table iteration order, no undefined behavior.

**Q3: How are timers implemented in games?**
For a few timers a min-heap is fine, with O(log n) insert and pop. For many timers use a timing wheel, with O(1) insertion and one slot processed per tick; for large delay ranges, use hierarchical wheels. Measured with 1 million timers, the timing wheel's firing phase is about 5× faster than a min-heap.

**Q4: How is AOI implemented?**
The nine-cell grid: cut the map into cells about the size of the view radius, and look only at the surrounding 9 cells when querying; measured about 140× faster than brute force. Cross-linked lists are an alternative. You also generate enter-view and leave-view events from the difference between the old and new view sets.

**Q5: How do you build a leaderboard? Why does Redis use a skiplist?**
A skiplist + hash table: the hash table finds a player's current score by id, the skiplist keeps scores sorted, and the `span` field on its pointers gives ranks in O(log n). A skiplist is simpler to implement than a red-black tree and makes range queries easy. Measured with 200,000 players: a skiplist rank lookup takes about 2 µs, counting through a `std::set` takes 18 ms.

**Q6: What is consistent hashing? Why virtual nodes?**
Map both nodes and keys onto a hash ring; a key belongs to the first node clockwise, and adding or removing a node affects only the adjacent stretch. Virtual nodes give each machine several positions on the ring so load is even. Measured, adding a 4th server moves only 24% of keys (modulo moves 75%); without virtual nodes, one server carried 86% of the traffic.

**Q7: What is ECS? Why is SoA faster than AoS?**
An entity is just an ID, components are pure data stored in contiguous arrays, and systems iterate over entities with particular components to run logic. SoA stores the same field's data contiguously, so processing one field uses the cache efficiently and is SIMD-friendly. Measured, updating one field is about 13× faster with SoA than AoS.

**Q8: How do you do battle replays?**
If the battle logic is deterministic, store only the input (lineups, seed) and the engine version, and recompute at playback. In this project the request is 8.6 KB and the result 148 KB, saving about 17× storage; the same mechanism works for anti-cheat verification. The precondition is keeping the matching engine version.

**Q9: What if a settlement request is duplicated?**
Make it idempotent with `battle_id` as a unique key: check whether it's already been processed before settling, and write the result and the transaction log in the same transaction.

Next: [Lesson 21: Performance analysis and optimization](21-performance.md)
