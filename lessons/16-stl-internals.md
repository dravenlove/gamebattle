# 第 16 课：STL 容器内部与迭代器失效

**中文** | [English](en/16-stl-internals.md)

> 面试官问 STL，问的从来不是"vector 怎么用"，而是"vector 扩容时发生了什么""unordered_map 冲突了怎么办""哪些操作会让迭代器失效"。这一课把这些讲清楚，并用本项目一场真实战斗的数据说明它们为什么重要。

## 1. 先看一组真实数据

我把引擎和一个"计数版"的全局 `operator new` 链接在一起，跑了一场 5v5 示例战斗（`practice/sample_battle.hpp`），实测：

```
一场战斗: 912 个事件, sizeof(Event)=136
Engine::simulate: 1093 次堆分配, 共 318 KB
结果编码(当前实现): 1879 次堆分配, 共 2034 KB, 输出 148 KB
```

- 模拟一场战斗，平均**每个事件对应约 1.2 次堆分配**。
- 编码结果时，为了输出 148 KB，**分配了 2 MB 内存**（第 13 课讲过原因：`initializer_list` 导致深复制）。
- 每个 `Event` 占 136 字节。

这一课结束时，你应该能解释这三个数字是怎么来的，以及怎么把它们降下来。

## 2. `vector`：连续内存与扩容

实测 GCC 的扩容序列：

```
capacity 变化: 1 2 4 8 16 32 64 128 256 512 1024 2048
reserve(1000) 后 push_back 1000 次，capacity=1000（没有扩容）
```

- GCC（libstdc++）和 Clang（libc++）每次扩容到 **2 倍**；MSVC 是 **1.5 倍**。
- 扩容 = 申请新内存 + 把所有元素移动（或复制，第 13 课的 `noexcept`）过去 + 释放旧内存。单次扩容是 O(n)，但均摊到每次 `push_back` 是 **O(1)**。
- 扩容后所有指针、引用、迭代器都失效（第 2 课实测过）。

**`events` 为什么占了很多分配**：一场战斗 912 个事件，`result.events` 从 1 开始翻倍扩容到 1024，一共扩容 11 次。每次扩容都要把所有 `Event`（每个 136 字节）搬一遍。如果能预估事件数，提前 `reserve` 就能省掉这些。项目里 `result.units.reserve(units.size())`（`battle_state.cpp:579`）就是这么做的，因为单位数是已知的。

其他要点：
- `shrink_to_fit()` 请求释放多余容量，但标准不保证一定释放。
- `clear()` 只销毁元素，**不释放内存**，capacity 不变。这其实是好事：反复使用同一个 vector 时，避免重复分配。
- `std::vector<bool>` 是一个**特化**，每个元素只占 1 比特，`operator[]` 返回的是一个代理对象，而不是 `bool&`。实测 `bool& first = alive[0];` 编译失败：`cannot bind non-const lvalue reference of type 'bool&' to an rvalue`。需要真正的 bool 数组时，用 `std::vector<char>` 或 `std::vector<std::uint8_t>`。

## 3. `std::string`：短字符串优化（SSO）

实测：

```
sizeof(std::string)=32  空字符串 capacity=15
  "damage" 长度 6 → 堆分配 0 次
  "buff_reaction" 长度 13 → 堆分配 0 次
  "direct_damage__" 长度 15 → 堆分配 0 次
  "direct_damage___" 长度 16 → 堆分配 1 次
```

libstdc++ 的 `std::string` 是 32 字节，其中 16 字节的内部缓冲区可以直接存放**最多 15 个字符**（加一个结尾的 `\0`），这时完全不需要堆分配。超过 15 个字符，才去堆上申请内存。这叫**短字符串优化**（Small String Optimization）。

不同标准库的阈值不同：libstdc++ 是 15，libc++ 是 22，MSVC 是 15。

**放到项目里**：`Event` 有两个字符串字段 `phase` 和 `type`。项目里最长的事件类型是 `buff_reaction` 和 `direct_damage`（13 个字符），最长的阶段名是 `second_side`（11 个字符），全部在 15 以内，所以**创建事件时不会因为字符串而分配内存**。只有结果里的 `reason`（比如 `all_units_defeated`，18 个字符）会分配一次，每场战斗只有一次，可以忽略。

但 SSO 不是免费的：两个字符串一共占了 64 字节，这就是 `sizeof(Event)` 达到 136 的主要原因。如果把 `phase` 和 `type` 改成 `enum class : std::uint8_t`（编码时再转成原子），`Event` 可以缩小一半左右，同样大小的缓存能装下多一倍的事件。第 21 课会讨论这个优化的取舍：它要改动 `Event` 这个公开结构，影响所有使用者。

## 4. `unordered_map`：哈希表

### 结构：桶数组 + 链表

```
bucket_count = 13
 [0] → nullptr
 [1] → (1001, ...) → (2014, ...) → nullptr       同一个桶里的元素用链表串起来
 [2] → (2002, ...) → nullptr
 ...
```

- 查找：算出 `hash(key) % bucket_count`，到对应的桶里顺着链表比较。平均 O(1)，**最坏 O(n)**（所有 key 都落进同一个桶）。
- **负载因子** = 元素个数 / 桶数。超过 `max_load_factor()`（默认 1.0）时**重新哈希**（rehash）：桶数组扩大，所有元素重新分配到新的桶里。

实测插入 1000 个元素：

```
初始 bucket_count=13 max_load_factor=1
bucket_count 变化: 29 59 127 257 541 1109
插入 1000 个元素、多次 rehash 之后，元素地址不变: 1
```

libstdc++ 的桶数总是取**质数**，让取模的分布更均匀。

### 关键性质：rehash 让迭代器失效，但引用和指针不失效

每个元素都是一个**单独分配的链表节点**。rehash 只是把节点重新挂到不同的桶上，节点本身不移动。所以：
- **迭代器失效**（迭代器记录了"在哪个桶的哪个位置"）；
- **指向元素的引用和指针仍然有效**（实测地址不变）。

这和 `vector` 正好相反。`practice/battle_tcp_server.cpp` 正依赖了这一点：`connections_` 是 `unordered_map<连接ID, Connection>`，在 `accept_all` 里插入新连接时，别处拿着的 `Connection&` 不会失效。

### 代价和陷阱

- 每个元素一个节点、一次堆分配，节点分散在内存各处，遍历时缓存命中率低。
- **遍历顺序不确定**，会随插入顺序和 rehash 变化。第 1 课和第 10 课都强调过：不能让遍历 `unordered_map` 的顺序影响战斗结果或输出文件。
- **哈希洪泛攻击**：如果 key 来自不可信的外部输入，攻击者可以故意构造大量哈希冲突的 key，把 O(1) 变成 O(n)。处理外部输入的哈希表要用带随机种子的哈希函数。
- `reserve(n)` 可以预先分配足够的桶，避免插入过程中反复 rehash。

### 自定义类型做 key

标准库没有为 `std::pair` 提供哈希函数。实测直接用 `pair` 做 key 会编译失败：`use of deleted function 'std::unordered_map<...>::unordered_map()'`。要自己提供：

```cpp
struct PairHash {
    std::size_t operator()(const std::pair<std::uint32_t, std::uint32_t>& key) const noexcept {
        return std::hash<std::uint64_t>{}((std::uint64_t{key.first} << 32U) | key.second);  // 两个 32 位拼成 64 位
    }
};
std::unordered_map<std::pair<std::uint32_t, std::uint32_t>, int, PairHash> modifiers;
```

项目里 `ConfigStore` 和配置编译器需要以 `(buff_id, sequence)` 作为 key 时，用的是有序的 `std::set<std::pair<...>>` 和 `std::map<std::pair<...>>`，正好绕开了这个问题，同时还保证了遍历顺序确定（第 10 课）。

## 5. `map` / `set`：红黑树

- 底层是**红黑树**（一种自平衡二叉搜索树），查找、插入、删除都是 **O(log n)**。
- 按 key 有序遍历，比较函数必须满足严格弱序（第 5 课）。
- 每个元素也是一个节点，**插入删除不影响其他元素的迭代器**（只有被删除的那个失效）。
- `lower_bound` / `upper_bound` 可以做范围查询，比如"ID 在 800~899 之间的所有 Buff"。

| | `unordered_map` | `map` |
|---|---|---|
| 查找 | 平均 O(1)，最坏 O(n) | O(log n) |
| 有序遍历 | ❌ | ✅ |
| 范围查询 | ❌ | ✅ |
| 内存 | 节点 + 桶数组 | 节点（每个带 3 个指针 + 颜色） |
| 项目里用在 | 按 ID 查单位（`unit_index`）、配置仓库 | 配置编译器（保证输出确定）、环检测 |

## 6. 其他容器

| 容器 | 结构 | 特点 | 项目 / 实战代码里 |
|---|---|---|---|
| `std::array<T, N>` | 栈上固定数组 | 零开销 | `std::array<Side, 2> order`，`passives_by_trigger` |
| `std::deque<T>` | 分段连续的数组块 | 两端 O(1) 插入；`push_back` **不移动已有元素** | TCP 服务器的 `pending` 请求队列 |
| `std::list<T>` | 双向链表 | 任意位置 O(1) 插入删除，但遍历很慢 | 未使用 |
| `std::span<T>` | 视图 | 不拥有数据（第 9 课） | `term::Reader` |

关于 `deque` 实测：

```
push_back 10 万次后  deque 首元素地址不变: 1   vector 首元素地址不变: 0
```

`deque` 由多个固定大小的内存块组成，加满一块就新开一块，已有的块不动，所以指向已有元素的引用不会失效（但迭代器会）。

关于 `list`：理论上任意位置插入删除都是 O(1)，但每个节点单独分配、散落在内存各处，遍历时几乎每一步都是缓存未命中。实践中，**即使需要在中间插入删除，元素不太多时 `vector` 往往也比 `list` 快**。

## 7. 迭代器失效总表

这是面试的高频题。

| 容器 | 插入 | 删除 |
|---|---|---|
| `vector` | 不扩容：插入点之后的全部失效；**扩容：全部失效** | 删除点之后的全部失效 |
| `deque` | 两端插入：**迭代器全部失效，引用不失效**；中间插入：全部失效 | 两端删除：只有被删的失效；中间删除：全部失效 |
| `list` | 都不失效 | 只有被删的失效 |
| `map` / `set` | 都不失效 | 只有被删的失效 |
| `unordered_map` / `unordered_set` | **rehash 时迭代器全部失效，引用和指针不失效**；不 rehash 时都不失效 | 只有被删的失效 |

对照项目里的写法：
- `units`（`vector`）：战斗中不增不删，所以可以长期持有下标和引用（第 2 课）。
- `buffs`（`vector`）：会增删，所以用唯一 ID 重新查找，删除时用 `it = erase(it)`（第 6 课）。
- `connections_`（`unordered_map`）：插入不影响已有元素的引用，删除时只影响被删的那个（第 19 课）。

## 8. 常用算法的复杂度

| 算法 | 复杂度 | 说明 |
|---|---|---|
| `std::sort` | O(n log n) | 内省排序（快排 + 堆排序 + 插入排序），**不稳定** |
| `std::stable_sort` | O(n log n) | 归并排序，稳定，需要额外内存 |
| `std::nth_element` | 平均 O(n) | 只找出第 k 小的元素，比如"伤害前 3 名" |
| `std::partial_sort` | O(n log k) | 只排出前 k 个 |
| `std::lower_bound` | O(log n) | 在**已排序**的区间里二分查找 |
| `std::find_if` | O(n) | 线性查找 |
| `std::remove_if` + `erase` | O(n) | 第 6 课 |

`TargetSelector` 对候选人排序后只取前 N 个，严格来说可以用 `std::partial_sort` 或 `std::nth_element`。但候选人最多只有几个到十几个，`std::sort` 已经足够快。**知道有更优的算法，同时知道这里不值得换**，面试时这样回答最好。

## 9. 回到开头的三个数字

| 现象 | 原因 | 能怎么改 |
|---|---|---|
| 模拟一场战斗 1093 次分配 | `events` 翻倍扩容；每次执行效果时 `TargetSelector::select` 都返回一个新 `vector`；每轮行动 `acting_order` 返回新 `vector`；有 Buff 反应的触发会填充一个新的 `pending` 列表（空 vector 本身不分配） | 预估事件数后 `reserve`；把候选人列表改成复用的成员缓冲区 |
| 编码分配 2 MB、输出 148 KB | `initializer_list` 深复制；构建了一棵完整的 `Value` 树 | 用移动构建（约快 1.9 倍）或直接流式写字节（约快 4 倍），第 13、21 课 |
| `sizeof(Event)` = 136 | 两个 `std::string` 占了 64 字节 | 改成 `enum : uint8_t`，编码时再转原子 |

第 21 课会实测其中一部分，看看改了之后到底能快多少。

## 面试题

**Q1：`vector` 的扩容机制？为什么是 2 倍（或 1.5 倍）？**
容量不足时申请更大的内存（GCC 2 倍、MSVC 1.5 倍），把元素移动过去，再释放旧内存。按倍数增长让 `push_back` 的均摊复杂度是 O(1)。1.5 倍的好处是之前释放的几块内存加起来有机会被复用，2 倍则永远拼不出下一次需要的大小。

**Q2：`vector` 的 `size` 和 `capacity` 有什么区别？`clear()` 会释放内存吗？**
`size` 是元素个数，`capacity` 是已分配的空间能放下多少元素。`clear()` 只销毁元素，`capacity` 不变，内存不释放。要释放可以用 `shrink_to_fit()`（不保证），或者和一个空 vector `swap`。

**Q3：什么是 SSO？**
短字符串优化：`std::string` 对象内部自带一个小缓冲区，短字符串（libstdc++ 是 15 个字符以内）直接存在对象里，不做堆分配。实测长度 15 的字符串分配 0 次，长度 16 分配 1 次。

**Q4：`unordered_map` 怎么解决哈希冲突？什么时候 rehash？**
标准库实现用链地址法：每个桶一个链表。元素数 / 桶数超过 `max_load_factor`（默认 1.0）时 rehash，桶数组扩大（libstdc++ 取质数），所有元素重新分桶。rehash 会让迭代器失效，但指向元素的引用和指针仍然有效。

**Q5：`map` 和 `unordered_map` 怎么选？**
需要有序遍历、范围查询或稳定的最坏复杂度，用 `map`（红黑树，O(log n)）。只需要按 key 查找、追求平均速度，用 `unordered_map`（平均 O(1)）。遍历顺序会影响结果的场合（比如要保证确定性），不能用 `unordered_map` 遍历。

**Q6：各容器的迭代器失效规则？**
见第 7 节的表。最常考的三条：`vector` 扩容时全部失效；`list` / `map` 只有被删除的元素失效；`unordered_map` rehash 时迭代器全部失效但引用不失效。

**Q7：`std::vector<bool>` 有什么特别的？**
它是按比特压缩存储的特化版本，`operator[]` 返回代理对象而不是 `bool&`，不能取元素地址，也不能当作普通的 bool 数组传给需要 `bool*` 的接口。

**Q8：`std::list` 理论上插入删除 O(1)，为什么实际中常常比 `vector` 慢？**
每个节点单独分配，散落在内存各处，遍历时频繁缓存未命中；而 `vector` 内存连续，CPU 预取效果好。元素数量不大时，`vector` 在中间插入删除需要移动的元素虽然多，总体仍然更快。

下一课：[第 17 课：线程、锁与线程池](17-threads-and-pools.md)
