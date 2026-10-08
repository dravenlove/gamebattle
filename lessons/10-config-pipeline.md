# 第 10 课：配置管线

**中文** | [English](en/10-config-pipeline.md)

> 对应文件：
> - 策划表：`config/example/*.csv`、`config/README.md`
> - 编译器：`tools/config_compiler.cpp`（CSV → `.gbcfg`）
> - 加载器：`src/config_store.cpp`、`include/gamebattle/config_store.hpp`（`.gbcfg` → 内存）
> - 设计文档：`docs/buff-v2-design.md`

## 1. 为什么要"编译"配置

```
策划用 Excel 编辑 6 张 UTF-8 CSV
            │
            ▼
gamebattle_config_compiler        ← 构建时运行一次：解析字符串、查枚举、查引用、查环
            │
            ▼
battle.gbcfg（二进制，几百字节）   ← 固定魔数 + 版本 + 长度 + CRC32，内容按 ID 排序
            │  gamebattle:load_config(port, Path)
            ▼
ConfigStore（只读内存）           ← 运行时：只做"读数字 + 再校验一遍"，不碰字符串解析
            │  skill_ids => [501]
            ▼
UnitConfig 里的 Skill / Passive / Effect / BuffSpec
```

这样分工的好处：

- **出错尽量早**：枚举写错、引用了不存在的 ID、Buff 互相引用成环，在**发布之前**就被编译器拦下，错误信息带文件名和行号。
- **运行时简单**：服务器不解析 CSV、不处理引号转义和编码问题，只读一个格式固定的二进制包。
- **请求变小**：线上请求只传 `skill_ids => [501]`，不必每次把完整的技能结构发给 C++。

## 2. 六张表和它们的关系

```
buff_modifiers.csv ───────────────→ buffs.csv
buff_reactions.csv → effects.csv ─→ buffs.csv
skills.csv ────────→ effects.csv
passives.csv ──────→ effects.csv
```

示例数据（`config/example`）：

```
effects.csv
id,type,target,target_count,attack_bp,flat,buff_id,remove_buff_id,notes
9001,damage,all_enemies,256,11500,20,0,0,烈焰斩伤害
9002,add_buff,trigger_unit,1,0,0,801,0,给受击目标添加中毒
9005,direct_damage,self,1,0,35,0,0,中毒每层直接伤害

buffs.csv
id,name,lifetime,duration,decrement_on,max_stacks,stack_policy,refresh_policy,notes
801,中毒,finite,2,round_end,3,stack,reset,回合结束由reaction结算直接伤害

buff_reactions.csv
buff_id,sequence,trigger,source,stack_scaling,chance_bp,max_triggers_per_round,effect_ids,notes
801,1,round_end,applier,per_stack,10000,0,9005,由施加者作为效果来源并对Buff持有者结算

passives.csv
id,name,trigger,chance_bp,max_triggers_per_round,effect_ids,notes
701,淬毒,on_hit,4000,1,9002,命中后施加中毒
```

读法：被动 701"淬毒"在命中时 40% 概率执行效果 9002 → 给受击者挂 Buff 801"中毒" → 中毒在回合结束时执行效果 9005 → 按层数造成 35 点直接伤害。`effect_ids` 列可以写多个 ID，用 `|` 分隔，比如 `9002|9003`。

这些表是**规范化的关系表**（像数据库一样用 ID 互相引用）。编译器负责把它们"连接"起来并检查。

## 3. 编译器：`tools/config_compiler.cpp`

### 3.1 总流程

```cpp
int run(std::span<const fs::path> args) {
    try {
        const auto options = parse_arguments(args);
        const auto tables = parse_tables(options.input_directory);   // 读 6 张表，逐行校验
        if (options.check_only) { std::cout << "configuration is valid\n"; return 0; }
        const auto pack = build_pack(tables);                        // 拼二进制
        write_pack(fs::absolute(options.output), pack);              // 原子地写文件
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "config error: " << error.what() << '\n';
        return 2;
    }
}
```

实测：

```
$ gamebattle_config_compiler --input-dir config/example --output a.gbcfg
wrote .../a.gbcfg (413 bytes): 2 buffs, 1 modifiers, 1 reactions, 5 effects, 1 skills, 3 passives

$ gamebattle_config_compiler --input-dir tests/fixtures/config_invalid_buff_cycle --check-only
config error: buff_reactions.csv: add_buff reaction graph contains a cycle
退出码=2
```

退出码非 0，CI 和发布脚本就能直接判断失败。

### 3.2 CSV 解析：一个手写的状态机

`parse_csv`（`config_compiler.cpp:263`）逐个字符扫描，用 `quoted`、`quote_closed` 两个布尔变量记录状态，处理了：

- 引号包起来的字段（字段里可以有逗号、换行）；
- `""` 表示一个字面上的引号；
- Windows 的 `\r\n` 和 Unix 的 `\n` 两种换行；
- 每条记录的起始行号，错误信息形如 `buffs.csv:3: ...`。

里面有一个捕获 `[&]` 的局部 lambda `finish_record`，在"遇到换行"和"文件结束"两个地方复用（第 5 课）。Erlang 里你大概会用 binary 模式匹配写同样的解析器，这里是同一件事的命令式写法。

### 3.3 严格地把文本转成数字：`std::from_chars`

```cpp
std::int64_t value = 0;
const auto result = std::from_chars(source.data(), source.data() + source.size(), value);
if (result.ec != std::errc{} || result.ptr != source.data() + source.size()) {
    row_error(row, std::string(key) + " must be an integer");
}
```

实测：

```
"12000" -> ok 12000
"12a" -> 拒绝
" 5" -> 拒绝
"99999999999999999999" -> 拒绝      ← 超出 int64
"-35" -> ok -35
```

两个检查缺一不可：`ec` 判断有没有解析出数字、有没有溢出；`ptr` 判断**是不是整个字符串都用完了**。只检查 `ec` 的话，`"12a"` 会被当成 12。

C 语言遗留的 `atoi("12a")` 会悄悄返回 12，出错时返回 0，你根本分不清"填的是 0"还是"填错了"。`std::from_chars` 不抛异常、不分配内存、不受系统区域设置（locale）影响，是 C++17 起解析数字的首选。它的严格程度和 Erlang 的 `binary_to_integer/1` 差不多。

### 3.4 枚举：字符串 → 数字

```cpp
const std::unordered_map<std::string, std::uint8_t> kEffectKinds{
    {"damage", std::uint8_t{0}}, {"heal", std::uint8_t{1}},
    {"add_buff", std::uint8_t{2}}, {"remove_buff", std::uint8_t{3}},
    {"direct_damage", std::uint8_t{4}}
};
```

这些数字必须和 `engine.hpp` 里 `enum class EffectKind` 的值**完全一致**。这就是第 1 课说 `EffectKind` 要显式写 `= 0, = 1 …` 的原因：数字被写进了二进制文件，是协议的一部分。

> **维护提示**：配置编译器**没有** include `engine.hpp`，两边的数字是人工保持一致的。以后新增一种 `EffectKind`（比如护盾），至少要同时改这几处：
> 1. `engine.hpp` 的 `enum class`；
> 2. `config_compiler.cpp` 的 `kEffectKinds` 表；
> 3. `config_store.cpp` 里 `checked_enum<EffectKind>(reader.u8(), 4, ...)` 的最大值 4；
> 4. `wire.cpp` 的 `parse_effect_kind`（内联请求）；
> 5. `effect_system.cpp` 的 `switch`（漏了会有 `-Wswitch` 警告，第 6 课）；
> 6. 只要旧版本加载器读不懂新文件，就要提升 `.gbcfg` 的格式版本号。

### 3.5 确定性输出：`std::map` 与无时间戳

```cpp
struct Tables {
    std::map<std::uint32_t, BuffRow> buffs;
    std::map<std::pair<std::uint32_t, std::uint32_t>, ModifierRow> modifiers;
    ...
};
```

编译器用的是有序的 `std::map`，不是 `std::unordered_map`。区别（实测，插入顺序是 803, 501, 701, 801, 702）：

```
std::map          : 501 701 702 801 803        ← 永远按 key 排序
std::unordered_map: 702 801 701 501 803        ← 顺序取决于哈希实现
```

按 `std::map` 的顺序写出，再加上文件里不写时间戳，**同样的表格永远编译出同样的字节**（实测连续编译两次，`cmp` 结果逐字节相同）。发布流程就可以先编译、测试、比较文件哈希，确认没有意外改动再上线。

`std::pair<uint32_t, uint32_t>` 当 key：`pair` 自带的 `<` 先比 `first`，相等再比 `second`（字典序），正好满足 `std::map` 对严格弱序的要求（第 5 课）。

### 3.6 拼装二进制：小端、带长度前缀

```cpp
void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));   // 低字节在前 = 小端
    }
}
void append_i32(std::vector<std::uint8_t>& output, std::int32_t value) {
    append_u32(output, std::bit_cast<std::uint32_t>(value));                 // 有符号数按比特原样写出
}
void append_string(std::vector<std::uint8_t>& output, const std::string& value) {
    append_u32(output, static_cast<std::uint32_t>(value.size()));             // 先写长度
    output.insert(output.end(), value.begin(), value.end());                  // 再写内容
}
```

`.gbcfg` 选择了**小端**，而 ETF 和 `{packet, 4}` 是**大端**（第 9 课）。选哪种都行，关键是两端一致，而且都用移位写出，与运行的机器无关。

实际文件的前 16 字节（实测）：

```
47 42 43 46   02 00   00 00   8d 01 00 00   2a a9 9c 13
└─ "GBCF" ─┘  主版本2  次版本0  负载 397 字节  CRC32 = 0x139ca92a
```

文件总长 413 = 16 字节头 + 397 字节负载，对得上。

### 3.7 CRC32：发现文件损坏

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

这是标准 CRC-32 算法，和 zlib、zip、`erlang:crc32/1` 是同一个（实测用 Python 的 `zlib.crc32` 对负载计算，结果正是文件头里的 `0x139ca92a`）。所以 Erlang 那边想在加载前自己先校验一遍，直接用 `erlang:crc32(Payload)` 即可。它用来发现**传输或拷贝时的损坏、截断**，不是安全校验，防不了有人故意篡改。

`mask` 那一行是一个常见技巧：`result & 1U` 是 0 或 1，取负后得到 0 或 -1，-1 的二进制是全 1。于是 `mask` 只可能是 `0x00000000` 或 `0xFFFFFFFF`，用"与"运算代替了 if 分支。

### 3.8 原子地写文件：先写临时文件，再改名

```cpp
void write_pack(const fs::path& output, std::span<const std::uint8_t> bytes) {
    auto temporary = output;
    temporary += ".tmp";
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        ...stream.write(...); stream.close();
        replace_file(temporary, output);        // rename：要么完全是旧文件，要么完全是新文件
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);         // 清理半成品
        throw;                                  // 原样重新抛出
    }
}
```

- 如果直接写 `battle.gbcfg`，写到一半时服务器恰好加载，就会读到一个残缺的文件。先写 `.tmp`，写完再 `rename`：在同一个文件系统上，改名是原子操作。Erlang 里的标准做法也是 `file:write_file` 到临时文件再 `file:rename`。
- `catch (...)` 捕获任何异常，做清理，然后用**不带参数的 `throw;`** 把**同一个异常原样重新抛出**，让上层照常处理。
- `fs::remove(temporary, ignored)` 用的是带 `std::error_code` 的版本：出错时不抛异常，而是写进 `ignored`。清理代码自己绝不能再抛异常，否则会盖掉原来的错误。

### 3.9 Windows 上用 `wmain`

```cpp
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) { ... }
#else
int main(int argc, char* argv[]) { ... }
#endif
```

Windows 的命令行参数原生是 UTF-16。用普通 `main` 拿到的是按系统代码页转换过的 `char*`，路径里的中文可能变成乱码。`wmain` 直接拿到 UTF-16，转成 `std::filesystem::path` 后跨平台处理。

## 4. 加载器：`ConfigStore::load_file`

### 4.1 先做便宜的检查，再做昂贵的

```cpp
std::ifstream input(path, std::ios::binary);
input.seekg(0, std::ios::end);
const auto end = input.tellg();                              // ① 先看文件多大
if (end < 0 || static_cast<std::uint64_t>(end) > kMaxPackBytes) throw ...;   // 超过 64 MB 直接拒绝
input.seekg(0, std::ios::beg);
std::vector<std::uint8_t> bytes;
bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());   // ② 整个读进来

if (bytes.size() < kHeaderBytes) throw ...;                  // ③ 头都不完整
Reader header(std::span<const std::uint8_t>(bytes).first(kHeaderBytes));
if (header.u8() != 'G' || header.u8() != 'B' || ...) throw ...("invalid gamebattle config magic");   // ④ 魔数
if (major != format_major || minor != format_minor) throw ...;                                     // ⑤ 版本
if (payload_size != bytes.size() - kHeaderBytes) throw ...;                                        // ⑥ 长度
const auto payload = std::span<const std::uint8_t>(bytes).subspan(kHeaderBytes);
if (crc32(payload) != expected_crc) throw ...("gamebattle config CRC32 check failed");             // ⑦ 校验和
```

检查顺序是有讲究的：从最便宜的开始，确认整个文件完好无损之后，才开始真正解析内容。拿错文件（魔数不对）、版本不兼容、文件被截断，都会在第一时间给出明确的错误。

`span.first(n)` 取前 n 个字节，`span.subspan(n)` 取第 n 个字节之后的部分。两者都只是新的视图，**不复制数据**。

### 4.2 枚举值先检查范围，再转换

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

C++ 允许把**任意整数** `static_cast` 成枚举，哪怕没有对应的枚举值。比如 `static_cast<EffectKind>(9)` 能编译，也能运行，但第 6 课那个没有 `default` 的 `switch` 不会匹配任何分支，这个效果就会**静悄悄地什么都不做**。所以从外部读进来的数字，必须先检查范围再转换。

这又是一个函数模板（第 9 课）：`Enum` 是类型参数，`checked_enum<Trigger>`、`checked_enum<StackPolicy>` 各生成一份。

### 4.3 两阶段：先读成"原始记录"，再连接

`load_file` 先把每张表读成一组 `RawEffect`、`RawReaction`、`RawSkill`……这些结构里存的是 **ID**（比如 `std::vector<std::uint32_t> effect_ids`），还不是真正的对象。全部读完、所有引用都确认存在之后，才把 ID 替换成真正的 `Effect` 对象。

这和 Erlang 里"先 decode 成扁平的记录列表，再用 map 按 ID 关联"是一样的思路。

### 4.4 先造可变的"空壳"，最后冻结成只读

这是整个文件最值得学的一段：

```cpp
// 阶段一：先把每个 Buff 造成一个可修改的空壳
std::unordered_map<std::uint32_t, std::shared_ptr<BuffSpec>> mutable_buffs;     // 注意：不是 const
auto buff = std::make_shared<BuffSpec>();
buff->id = reader.u32();
...
insert_unique(mutable_buffs, buff_id, std::move(buff), "buffs");

// 阶段二：Effect 指向空壳（这时空壳里还没有 reactions）
if (raw.value.kind == EffectKind::add_buff) {
    raw.value.buff = mutable_buffs.at(raw.buff_id);   // shared_ptr<BuffSpec> → shared_ptr<const BuffSpec>，隐式转换
}

// 阶段三：往空壳里填 modifiers 和 reactions
mutable_buffs.at(raw.buff_id)->reactions.push_back(std::move(reaction));

// 阶段四：冻结
for (auto& [id, buff] : mutable_buffs) {
    std::shared_ptr<const BuffSpec> immutable = std::move(buff);   // 交出唯一的可写句柄
    insert_unique(store.buffs_, id, std::move(immutable), "buffs");
}
```

为什么要这么绕？因为引用关系是一张图：Buff 的 reaction 里有 Effect，Effect 又可能指向另一个 Buff。如果要求"一个 Buff 必须完整之后才能被指向"，就得先算出一个所有 Buff 的构造顺序，很麻烦。

先造空壳就简单了：**指针指向的是对象本身，而不是对象当时的内容**。阶段二时 Effect 就已经拿到了空壳的指针，阶段三往空壳里填内容，通过 Effect 的指针看到的也是填好之后的样子，因为它们是同一个对象。

阶段四是关键：可写的 `shared_ptr<BuffSpec>` 被 `std::move` 交出去，转成 `shared_ptr<const BuffSpec>`。函数返回后，**世界上再也没有任何一个可写的句柄指向这些 Buff**，它们事实上变成了不可变对象（第 2 课：只读的东西才能安全地被多个线程共享）。

代码里的注释概括了这一点：

```cpp
// Phase one creates mutable definition shells. No shell escapes this
// function until every reference and ownership edge has been validated.
```

这在 Erlang 里做不到：数据一旦创建就不能改，你只能用 ID 加查找表来表示这种互相引用。C++ 允许"先搭架子、填内容、再冻结"。

### 4.5 环检测：Kahn 拓扑排序

第 2 课讲过：Buff 之间如果形成引用环，`shared_ptr` 永远释放不了。加载器用的是 **Kahn 算法**（`config_store.cpp:467-502`）：

```cpp
std::map<std::uint32_t, std::uint32_t> indegree;                 // 每个 Buff 被多少条边指向
std::map<std::uint32_t, std::vector<std::uint32_t>> edges;       // A 的 reaction 会 add_buff B → 边 A→B
...
std::vector<std::uint32_t> ready;                                // 入度为 0 的点：没人指向它
for (const auto& [buff_id, degree] : indegree) {
    if (degree == 0) ready.push_back(buff_id);
}
std::size_t visited = 0;
for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) { // 一边遍历一边往 ready 里追加
    const auto buff_id = ready[cursor];
    ++visited;
    for (const auto target : edges[buff_id]) {
        if (--indegree.at(target) == 0) ready.push_back(target); // 拿掉这条边，目标入度归零就加入
    }
}
if (visited != mutable_buffs.size()) {
    throw std::runtime_error("buff reaction add_buff graph contains an ownership cycle");
}
```

思路：反复"摘掉没有人指向的点"。如果图里有环，环上的每个点都至少被一个环上的点指向，入度永远降不到 0，永远摘不掉。所以最后摘掉的点数少于总数，就说明有环。

和第 8 课运行时用的**深度优先 + 三色标记**对比：

| | 三色标记（`validate_request`） | Kahn（`ConfigStore`） |
|---|---|---|
| 写法 | 递归 | 循环，没有递归深度问题 |
| 适合 | 从某个起点出发，边走边查 | 整张图事先完全已知 |
| 额外好处 | 发现环的那一刻就知道是哪个 Buff 绕回来了 | 顺便得到一个拓扑顺序 |

注意那个循环的写法：`for (cursor = 0; cursor < ready.size(); ++cursor)` 在遍历过程中往 `ready` 里 `push_back`。这里用的是**下标**，所以即使 `push_back` 让 vector 扩容也没关系（第 2 课第 4 节）。如果换成范围 for 或迭代器，扩容就会让它们失效。

另外，这里全部用有序的 `std::map` / `std::set`，所以算法的执行过程本身也是确定的。

### 4.6 按 `(buff_id, sequence)` 排序

```cpp
std::sort(raw_reactions.begin(), raw_reactions.end(),
          [](const RawReaction& left, const RawReaction& right) {
              return std::pair{left.buff_id, left.sequence} <
                     std::pair{right.buff_id, right.sequence};
          });
```

一个 Buff 可以有多条 reaction，它们的执行顺序由表里的 `sequence` 列决定，和 CSV 里行的先后无关。用 `std::pair` 的字典序比较，一行就写出了"先按 buff_id，再按 sequence"的严格弱序。`(buff_id, sequence)` 在加载时已经检查过不能重复，所以排序结果唯一（第 5 课规则二）。

### 4.7 强异常保证：失败时不留下任何痕迹

`load_file` 从头到尾都在局部变量里构造，最后一行才 `return store;`。中途任何一步抛出异常，所有局部变量都会被自动销毁（第 8 课的栈展开），调用方什么也拿不到。

再配合第 9 课 `Handler` 里的写法（锁外加载、锁内换指针），整个热更新是**全有或全无**的：

```
load_config(Path)
  ├─ load_file 失败（文件坏了、版本不对、有环……）
  │     → 抛异常 → 返回 {error, #{type => config_load_failed, ...}}
  │     → configs_ 没被碰过，旧配置继续服务
  └─ load_file 成功
        → make_shared<ConfigStore>(...)
        → 加独占锁，configs_ = next（只换一个指针）
        → 新请求用新配置；已经在跑的战斗握着旧的 shared_ptr，跑完后旧配置自动释放
```

这在 C++ 里叫**强异常保证**（strong exception guarantee）：操作要么完全成功，要么就像从没发生过。

`assign_loadout`（`config_store.cpp:614`）也是同样的写法：先把所有技能、被动查到临时 vector 里，全部查到了才 `std::move` 进 `unit`。只要有一个 ID 不存在，就会在修改 `unit` 之前抛出异常，`unit` 保持原样。

### 4.8 两种查询 API

```cpp
const BuffSpec* find_buff(std::uint32_t id) const noexcept;   // 找不到返回 nullptr
const BuffSpec& require_buff(std::uint32_t id) const;         // 找不到抛 std::out_of_range
```

- `find_*`：调用方**预期可能找不到**，自己处理 `nullptr`。
- `require_*`：调用方**认为一定存在**，找不到就是错误。

`wire.cpp` 用的是 `require_skill`，再把 `out_of_range` 翻译成 `invalid_request`（第 8 课第 7 节）。

### 4.9 配置进入战斗：按值复制

```cpp
unit.skills.push_back(configs->require_skill(id));   // wire.cpp:435
```

`require_skill` 返回 `const Skill&`（借用），`push_back` 时**复制**出一份 `Skill` 放进 `unit`。复制 `Skill` 会复制它的 `effects`，复制 `Effect` 会复制里面的 `shared_ptr<const BuffSpec>`，也就是引用计数加 1，而 `BuffSpec` 本身不复制。

所以每场战斗都握着自己需要的那部分配置的快照，跟 `ConfigStore` 之后会不会被换掉完全无关。这就是 README 所说的"已经开始解析的战斗也继续使用它取得的旧配置快照"。

## 小结

| 概念 | 要点 | Erlang 里对应的东西 |
|---|---|---|
| 配置编译 | 字符串解析和引用检查放在构建时，运行时只读二进制 | 构建期生成 `.beam` / 配置 term 文件 |
| `std::from_chars` | 同时检查 `ec` 和 `ptr`，严格解析 | `binary_to_integer/1` |
| 枚举数字 | 写进文件就是协议，多处要人工保持一致 | 原子按名字比较，没这问题 |
| `std::map` | 有序遍历 → 输出字节确定 | `lists:sort` 后再写出 |
| 小端 / 大端 | 都用移位写出，与机器无关 | `<<X:32/little>>` |
| CRC32 | 发现损坏和截断，不防篡改 | `erlang:crc32/1` |
| 临时文件 + rename | 原子替换 | `file:write_file` + `file:rename` |
| `throw;` | 原样重新抛出当前异常 | `erlang:raise/3` |
| `checked_enum` | 外部数字先查范围再转枚举 | 无 |
| 空壳 → 填充 → 冻结 | 用可写 `shared_ptr` 搭好图，最后 `move` 成 `const` | 只能用 ID + 查找表 |
| Kahn 拓扑排序 | 循环实现，入度归零就摘掉，摘不完就有环 | `digraph_utils:topsort/1` |
| 强异常保证 | 局部构造，最后交付；失败不留痕迹 | 进程崩溃前不修改共享状态 |
| `find_*` / `require_*` | 可能找不到返回指针；一定存在返回引用、否则抛异常 | `maps:find/2` / `maps:get/2` |

下一课：[第 11 课：构建、测试与调试](11-build-and-test.md)
