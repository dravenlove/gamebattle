# Lesson 10: The config pipeline

[中文](../10-config-pipeline.md) | **English**

> Files:
> - Designer tables: `config/example/*.csv`, `config/README.md`
> - Compiler: `tools/config_compiler.cpp` (CSV → `.gbcfg`)
> - Loader: `src/config_store.cpp`, `include/gamebattle/config_store.hpp` (`.gbcfg` → memory)
> - Design doc: `docs/buff-v2-design.md`

## 1. Why "compile" config

```
Designers edit 6 UTF-8 CSV files in Excel
            │
            ▼
gamebattle_config_compiler        ← runs once at build time: parse strings, look up enums, check references, check cycles
            │
            ▼
battle.gbcfg (binary, a few hundred bytes)   ← fixed magic + version + length + CRC32, contents sorted by ID
            │  gamebattle:load_config(port, Path)
            ▼
ConfigStore (read-only memory)    ← at run time: just "read numbers + validate once more", no string parsing
            │  skill_ids => [501]
            ▼
Skill / Passive / Effect / BuffSpec inside UnitConfig
```

The benefits of splitting the work this way:

- **Fail as early as possible**: a misspelled enum, a reference to an ID that doesn't exist, buffs referring to each other in a cycle: all are stopped by the compiler **before release**, with file names and line numbers in the error messages.
- **A simple runtime**: the server doesn't parse CSV or deal with quote escaping and encodings; it only reads a binary package with a fixed format.
- **Smaller requests**: production requests send only `skill_ids => [501]` instead of the full skill structure to C++ every time.

## 2. The six tables and how they relate

```
buff_modifiers.csv ───────────────→ buffs.csv
buff_reactions.csv → effects.csv ─→ buffs.csv
skills.csv ────────→ effects.csv
passives.csv ──────→ effects.csv
```

Sample data (`config/example`; the `name` and `notes` columns are in Chinese in the real files and are translated here):

```
effects.csv
id,type,target,target_count,attack_bp,flat,buff_id,remove_buff_id,notes
9001,damage,all_enemies,256,11500,20,0,0,Flame Slash damage
9002,add_buff,trigger_unit,1,0,0,801,0,apply Poison to the target that was hit
9005,direct_damage,self,1,0,35,0,0,Poison direct damage per stack

buffs.csv
id,name,lifetime,duration,decrement_on,max_stacks,stack_policy,refresh_policy,notes
801,Poison,finite,2,round_end,3,stack,reset,a reaction deals direct damage at round end

buff_reactions.csv
buff_id,sequence,trigger,source,stack_scaling,chance_bp,max_triggers_per_round,effect_ids,notes
801,1,round_end,applier,per_stack,10000,0,9005,the applier is the effect source; resolved against the buff holder

passives.csv
id,name,trigger,chance_bp,max_triggers_per_round,effect_ids,notes
701,Envenom,on_hit,4000,1,9002,apply Poison on hit
```

How to read it: passive 701 "Envenom" has a 40% chance on hit to run effect 9002 → which applies buff 801 "Poison" to the target → at round end Poison runs effect 9005 → dealing 35 direct damage per stack. The `effect_ids` column can list several IDs separated by `|`, such as `9002|9003`.

These tables are **normalized relational tables** (referring to each other by ID, like a database). The compiler's job is to "join" them and check them.

## 3. The compiler: `tools/config_compiler.cpp`

### 3.1 The overall flow

```cpp
int run(std::span<const fs::path> args) {
    try {
        const auto options = parse_arguments(args);
        const auto tables = parse_tables(options.input_directory);   // read the 6 tables, validating row by row
        if (options.check_only) { std::cout << "configuration is valid\n"; return 0; }
        const auto pack = build_pack(tables);                        // assemble the binary
        write_pack(fs::absolute(options.output), pack);              // write the file atomically
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "config error: " << error.what() << '\n';
        return 2;
    }
}
```

Measured:

```
$ gamebattle_config_compiler --input-dir config/example --output a.gbcfg
wrote .../a.gbcfg (413 bytes): 2 buffs, 1 modifiers, 1 reactions, 5 effects, 1 skills, 3 passives

$ gamebattle_config_compiler --input-dir tests/fixtures/config_invalid_buff_cycle --check-only
config error: buff_reactions.csv: add_buff reaction graph contains a cycle
exit code=2
```

A non-zero exit code lets CI and release scripts detect the failure directly.

### 3.2 CSV parsing: a hand-written state machine

`parse_csv` (`config_compiler.cpp:263`) scans character by character, tracking state in two booleans, `quoted` and `quote_closed`, and handles:

- quoted fields (which may contain commas and newlines);
- `""` meaning one literal quote;
- both Windows `\r\n` and Unix `\n` line endings;
- the starting line number of each record, so errors look like `buffs.csv:3: ...`.

Inside is a local lambda `finish_record` capturing `[&]`, reused in two places: "hit a newline" and "end of file" (lesson 5). In Erlang you'd probably write the same parser with binary pattern matching; this is the imperative version of the same thing.

### 3.3 Turning text into numbers strictly: `std::from_chars`

```cpp
std::int64_t value = 0;
const auto result = std::from_chars(source.data(), source.data() + source.size(), value);
if (result.ec != std::errc{} || result.ptr != source.data() + source.size()) {
    row_error(row, std::string(key) + " must be an integer");
}
```

Measured:

```
"12000" -> ok 12000
"12a" -> rejected
" 5" -> rejected
"99999999999999999999" -> rejected      ← exceeds int64
"-35" -> ok -35
```

Both checks are required: `ec` tells you whether a number was parsed and whether it overflowed; `ptr` tells you **whether the whole string was consumed**. Checking only `ec` would accept `"12a"` as 12.

C's legacy `atoi("12a")` silently returns 12, and returns 0 on error, so you can't tell "they wrote 0" from "they wrote garbage". `std::from_chars` doesn't throw, doesn't allocate and isn't affected by the system locale; it's the preferred way to parse numbers since C++17. It's about as strict as Erlang's `binary_to_integer/1`.

### 3.4 Enums: string → number

```cpp
const std::unordered_map<std::string, std::uint8_t> kEffectKinds{
    {"damage", std::uint8_t{0}}, {"heal", std::uint8_t{1}},
    {"add_buff", std::uint8_t{2}}, {"remove_buff", std::uint8_t{3}},
    {"direct_damage", std::uint8_t{4}}
};
```

These numbers must match the values of `enum class EffectKind` in `engine.hpp` **exactly**. That's why lesson 1 said `EffectKind` spells out `= 0, = 1 …`: the numbers are written into a binary file, so they're part of the protocol.

> **Maintenance note**: the config compiler does **not** include `engine.hpp`; the two sides' numbers are kept in sync by hand. When you add a new `EffectKind` later (a shield, say), you must change at least these places together:
> 1. the `enum class` in `engine.hpp`;
> 2. the `kEffectKinds` table in `config_compiler.cpp`;
> 3. the maximum value 4 in `checked_enum<EffectKind>(reader.u8(), 4, ...)` in `config_store.cpp`;
> 4. `parse_effect_kind` in `wire.cpp` (inline requests);
> 5. the `switch` in `effect_system.cpp` (miss it and you get a `-Wswitch` warning, lesson 6);
> 6. if an older loader can't read the new file, bump the `.gbcfg` format version.

### 3.5 Deterministic output: `std::map` and no timestamps

```cpp
struct Tables {
    std::map<std::uint32_t, BuffRow> buffs;
    std::map<std::pair<std::uint32_t, std::uint32_t>, ModifierRow> modifiers;
    ...
};
```

The compiler uses the ordered `std::map`, not `std::unordered_map`. The difference (measured, inserting in the order 803, 501, 701, 801, 702):

```
std::map          : 501 701 702 801 803        ← always sorted by key
std::unordered_map: 702 801 701 501 803        ← order depends on the hash implementation
```

Writing in `std::map` order, plus not putting a timestamp in the file, means **the same tables always compile to the same bytes** (measured: two consecutive compiles, `cmp` reports them byte-identical). The release process can then compile, test and compare file hashes to confirm there are no unexpected changes before going live.

`std::pair<uint32_t, uint32_t>` as a key: `pair`'s built-in `<` compares `first`, then `second` on a tie (lexicographic order), which satisfies `std::map`'s strict weak ordering requirement (lesson 5).

### 3.6 Assembling the binary: little-endian, length-prefixed

```cpp
void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));   // low byte first = little-endian
    }
}
void append_i32(std::vector<std::uint8_t>& output, std::int32_t value) {
    append_u32(output, std::bit_cast<std::uint32_t>(value));                 // signed values written bit for bit
}
void append_string(std::vector<std::uint8_t>& output, const std::string& value) {
    append_u32(output, static_cast<std::uint32_t>(value.size()));             // length first
    output.insert(output.end(), value.begin(), value.end());                  // then the contents
}
```

`.gbcfg` chose **little-endian**, while ETF and `{packet, 4}` are **big-endian** (lesson 9). Either works; what matters is that both ends agree and both write with shifts, independent of the machine.

The first 16 bytes of an actual file (measured):

```
47 42 43 46   02 00          00 00          8d 01 00 00          2a a9 9c 13
└─ "GBCF" ─┘  major ver. 2   minor ver. 0   payload 397 bytes    CRC32 = 0x139ca92a
```

Total file length 413 = 16-byte header + 397-byte payload. It adds up.

### 3.7 CRC32: detecting corrupted files

```cpp
std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t result = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        result ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(-static_cast<std::int32_t>(result & 1U));
            result = (result >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~result;
}
```

This is the standard CRC-32 algorithm, the same one used by zlib, zip and `erlang:crc32/1` (measured: Python's `zlib.crc32` over the payload gives exactly the `0x139ca92a` in the header). So if the Erlang side wants to verify a file itself before loading, `erlang:crc32(Payload)` does it. It detects **corruption and truncation in transfer or copying**; it's not a security check and doesn't stop deliberate tampering.

The `mask` line is a common trick: `result & 1U` is 0 or 1; negated, that's 0 or -1, and -1 in binary is all ones. So `mask` can only be `0x00000000` or `0xFFFFFFFF`, and an AND replaces an if branch.

### 3.8 Writing files atomically: write a temp file, then rename

```cpp
void write_pack(const fs::path& output, std::span<const std::uint8_t> bytes) {
    auto temporary = output;
    temporary += ".tmp";
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        ...stream.write(...); stream.close();
        replace_file(temporary, output);        // rename: either entirely the old file or entirely the new one
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);         // clean up the half-written file
        throw;                                  // rethrow unchanged
    }
}
```

- If you wrote `battle.gbcfg` directly and the server happened to load it halfway through, it would read a truncated file. Write `.tmp` first, then `rename`: on the same filesystem, renaming is atomic. The standard Erlang approach is also `file:write_file` to a temp file, then `file:rename`.
- `catch (...)` catches any exception, cleans up, and then **`throw;` with no operand rethrows the same exception unchanged**, so the upper layer handles it as usual.
- `fs::remove(temporary, ignored)` uses the overload taking a `std::error_code`: on failure it writes into `ignored` instead of throwing. Cleanup code must never throw itself, or it would mask the original error.

### 3.9 `wmain` on Windows

```cpp
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) { ... }
#else
int main(int argc, char* argv[]) { ... }
#endif
```

Windows command-line arguments are natively UTF-16. A plain `main` receives `char*` converted through the system code page, and non-ASCII characters in paths (Chinese, say) may turn into garbage. `wmain` receives UTF-16 directly, which is converted to `std::filesystem::path` and handled portably.

## 4. The loader: `ConfigStore::load_file`

### 4.1 Cheap checks first, expensive ones later

```cpp
std::ifstream input(path, std::ios::binary);
input.seekg(0, std::ios::end);
const auto end = input.tellg();                              // ① check the file size first
if (end < 0 || static_cast<std::uint64_t>(end) > kMaxPackBytes) throw ...;   // reject anything over 64 MB
input.seekg(0, std::ios::beg);
std::vector<std::uint8_t> bytes;
bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());   // ② read it all in

if (bytes.size() < kHeaderBytes) throw ...;                  // ③ not even a full header
Reader header(std::span<const std::uint8_t>(bytes).first(kHeaderBytes));
if (header.u8() != 'G' || header.u8() != 'B' || ...) throw ...("invalid gamebattle config magic");   // ④ magic
if (major != format_major || minor != format_minor) throw ...;                                     // ⑤ version
if (payload_size != bytes.size() - kHeaderBytes) throw ...;                                        // ⑥ length
const auto payload = std::span<const std::uint8_t>(bytes).subspan(kHeaderBytes);
if (crc32(payload) != expected_crc) throw ...("gamebattle config CRC32 check failed");             // ⑦ checksum
```

The order is deliberate: start with the cheapest checks, and only start parsing the contents once the whole file is confirmed intact. The wrong file (bad magic), an incompatible version or a truncated file each produce a clear error right away.

`span.first(n)` takes the first n bytes; `span.subspan(n)` takes everything after byte n. Both are just new views and **copy no data**.

### 4.2 Range-check enum values before converting

```cpp
template <typename Enum>
Enum checked_enum(std::uint8_t value, std::uint8_t maximum, const char* field) {
    if (value > maximum) {
        throw std::runtime_error(std::string(field) + " contains an unknown enum value");
    }
    return static_cast<Enum>(value);
}

raw.value.kind = checked_enum<EffectKind>(reader.u8(), 4, "effect.kind");
```

C++ lets you `static_cast` **any integer** to an enum, even one with no matching enumerator. `static_cast<EffectKind>(9)` compiles and runs, but the `switch` without a `default` from lesson 6 matches no case, and the effect **silently does nothing**. So numbers read from outside must be range-checked before conversion.

Another function template (lesson 9): `Enum` is a type parameter, and `checked_enum<Trigger>` and `checked_enum<StackPolicy>` each get their own copy.

### 4.3 Two phases: read into "raw records" first, then link

`load_file` first reads each table into a set of `RawEffect`, `RawReaction`, `RawSkill`… These structs hold **IDs** (for example `std::vector<std::uint32_t> effect_ids`), not the real objects yet. Only after everything has been read and every reference confirmed to exist are the IDs replaced by real `Effect` objects.

It's the same idea as in Erlang: "decode into flat record lists first, then join them by ID with a map".

### 4.4 Build mutable "shells" first, freeze them read-only at the end

This is the most instructive part of the file:

```cpp
// Phase one: build each buff as a mutable shell
std::unordered_map<std::uint32_t, std::shared_ptr<BuffSpec>> mutable_buffs;     // note: not const
auto buff = std::make_shared<BuffSpec>();
buff->id = reader.u32();
...
insert_unique(mutable_buffs, buff_id, std::move(buff), "buffs");

// Phase two: effects point at the shells (which have no reactions yet)
if (raw.value.kind == EffectKind::add_buff) {
    raw.value.buff = mutable_buffs.at(raw.buff_id);   // shared_ptr<BuffSpec> → shared_ptr<const BuffSpec>, implicit conversion
}

// Phase three: fill modifiers and reactions into the shells
mutable_buffs.at(raw.buff_id)->reactions.push_back(std::move(reaction));

// Phase four: freeze
for (auto& [id, buff] : mutable_buffs) {
    std::shared_ptr<const BuffSpec> immutable = std::move(buff);   // give up the only writable handle
    insert_unique(store.buffs_, id, std::move(immutable), "buffs");
}
```

Why the detour? Because the references form a graph: a buff's reactions contain effects, and an effect may point at another buff. Requiring "a buff must be complete before anything can point to it" would mean first working out a construction order for all buffs, which is a pain.

Shells make it simple: **a pointer points at the object itself, not at the object's contents at that moment**. In phase two an effect already holds a pointer to the shell; phase three fills the shell, and what you see through the effect's pointer is the filled-in version, because it's the same object.

Phase four is the key: the writable `shared_ptr<BuffSpec>` is handed over with `std::move` and converted to `shared_ptr<const BuffSpec>`. After the function returns, **no writable handle to these buffs exists anywhere**, so they are effectively immutable objects (lesson 2: only read-only things can be shared safely between threads).

The comment in the code sums it up:

```cpp
// Phase one creates mutable definition shells. No shell escapes this
// function until every reference and ownership edge has been validated.
```

You can't do this in Erlang: once data is created it can't change, so mutual references can only be expressed with IDs plus lookup tables. C++ lets you "put up the scaffolding, fill it in, then freeze it".

### 4.5 Cycle detection: Kahn's topological sort

Lesson 2 explained that if buffs reference each other in a cycle, the `shared_ptr`s can never be freed. The loader uses **Kahn's algorithm** (`config_store.cpp:467-502`):

```cpp
std::map<std::uint32_t, std::uint32_t> indegree;                 // how many edges point at each buff
std::map<std::uint32_t, std::vector<std::uint32_t>> edges;       // A's reaction does add_buff B → edge A→B
...
std::vector<std::uint32_t> ready;                                // nodes with in-degree 0: nothing points at them
for (const auto& [buff_id, degree] : indegree) {
    if (degree == 0) ready.push_back(buff_id);
}
std::size_t visited = 0;
for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) { // appends to ready while iterating it
    const auto buff_id = ready[cursor];
    ++visited;
    for (const auto target : edges[buff_id]) {
        if (--indegree.at(target) == 0) ready.push_back(target); // remove the edge; if the target hits 0, add it
    }
}
if (visited != mutable_buffs.size()) {
    throw std::runtime_error("buff reaction add_buff graph contains an ownership cycle");
}
```

The idea: repeatedly "remove nodes that nothing points at". If the graph has a cycle, every node on it is pointed at by at least one other node on the cycle, so its in-degree never reaches 0 and it's never removed. If fewer nodes are removed than exist, there's a cycle.

Compared with the **depth-first search + three-color marking** used at run time in lesson 8:

| | Three-color marking (`validate_request`) | Kahn (`ConfigStore`) |
|---|---|---|
| Style | Recursive | A loop, no recursion-depth concerns |
| Suited to | Starting from some node and checking as you walk | The whole graph is fully known in advance |
| Bonus | The moment a cycle is found, you know which buff came back around | You get a topological order for free |

Note how that loop is written: `for (cursor = 0; cursor < ready.size(); ++cursor)` calls `push_back` on `ready` during iteration. It uses an **index**, so it doesn't matter if `push_back` reallocates the vector (lesson 2, section 4). With a range-for or an iterator, reallocation would invalidate them.

Also, everything here uses the ordered `std::map` / `std::set`, so even the algorithm's execution is deterministic.

### 4.6 Sorting by `(buff_id, sequence)`

```cpp
std::sort(raw_reactions.begin(), raw_reactions.end(),
          [](const RawReaction& left, const RawReaction& right) {
              return std::pair{left.buff_id, left.sequence} <
                     std::pair{right.buff_id, right.sequence};
          });
```

A buff can have several reactions, and their execution order is set by the table's `sequence` column, not by the order of the CSV rows. `std::pair`'s lexicographic comparison expresses "by buff_id, then by sequence" as a strict weak ordering in one line. `(buff_id, sequence)` has already been checked for duplicates at load time, so the sort result is unique (lesson 5, rule 2).

### 4.7 The strong exception guarantee: leave no trace on failure

`load_file` builds everything in local variables from start to finish, and only the last line does `return store;`. If any step throws, all the locals are destroyed automatically (stack unwinding from lesson 8), and the caller gets nothing.

Combined with the pattern in lesson 9's `Handler` (load outside the lock, swap the pointer inside it), the whole hot reload is **all or nothing**:

```
load_config(Path)
  ├─ load_file fails (corrupt file, wrong version, a cycle…)
  │     → throws → returns {error, #{type => config_load_failed, ...}}
  │     → configs_ is untouched; the old config keeps serving
  └─ load_file succeeds
        → make_shared<ConfigStore>(...)
        → take the exclusive lock, configs_ = next (swap one pointer)
        → new requests use the new config; running battles hold the old shared_ptr, and the old config is freed when they finish
```

In C++ this is called the **strong exception guarantee**: an operation either succeeds completely or behaves as if it never happened.

`assign_loadout` (`config_store.cpp:614`) follows the same pattern: look up every skill and passive into temporary vectors first, and only `std::move` them into `unit` once all are found. If any ID doesn't exist, it throws before `unit` is modified, and `unit` stays as it was.

### 4.8 Two lookup APIs

```cpp
const BuffSpec* find_buff(std::uint32_t id) const noexcept;   // returns nullptr when not found
const BuffSpec& require_buff(std::uint32_t id) const;         // throws std::out_of_range when not found
```

- `find_*`: the caller **expects it might not be there** and handles `nullptr` itself.
- `require_*`: the caller **believes it must exist**, so not finding it is an error.

`wire.cpp` uses `require_skill` and translates `out_of_range` into `invalid_request` (lesson 8, section 7).

### 4.9 Config entering a battle: copied by value

```cpp
unit.skills.push_back(configs->require_skill(id));   // wire.cpp:435
```

`require_skill` returns a `const Skill&` (a borrow), and `push_back` **copies** a `Skill` into `unit`. Copying a `Skill` copies its `effects`; copying an `Effect` copies the `shared_ptr<const BuffSpec>` inside it, which just adds 1 to the reference count; the `BuffSpec` itself isn't copied.

So each battle holds a snapshot of the part of the config it needs, entirely independent of whether `ConfigStore` is swapped out later. That's what the README means by "battles that have already started parsing keep using the old config snapshot they obtained".

## Summary

| Concept | Key point | Erlang counterpart |
|---|---|---|
| Config compilation | String parsing and reference checks happen at build time; the runtime only reads binary | Generating `.beam` / config term files at build time |
| `std::from_chars` | Check both `ec` and `ptr` for strict parsing | `binary_to_integer/1` |
| Enum numbers | Once written to a file they're protocol; several places must be kept in sync by hand | Atoms compare by name, so no such problem |
| `std::map` | Ordered iteration → deterministic output bytes | `lists:sort` before writing |
| Little / big endian | Both written with shifts, independent of the machine | `<<X:32/little>>` |
| CRC32 | Detects corruption and truncation, not tampering | `erlang:crc32/1` |
| Temp file + rename | Atomic replacement | `file:write_file` + `file:rename` |
| `throw;` | Rethrows the current exception unchanged | `erlang:raise/3` |
| `checked_enum` | Range-check external numbers before converting to an enum | None |
| Shell → fill → freeze | Build the graph with writable `shared_ptr`s, then `move` them to `const` | Only possible with IDs + lookup tables |
| Kahn's topological sort | A loop; remove nodes as their in-degree reaches 0; leftovers mean a cycle | `digraph_utils:topsort/1` |
| Strong exception guarantee | Build locally, hand over at the end; failure leaves no trace | Don't touch shared state before a process might crash |
| `find_*` / `require_*` | Might be missing → return a pointer; must exist → return a reference or throw | `maps:find/2` / `maps:get/2` |

Next: [Lesson 11: Building, testing and debugging](11-build-and-test.md)
