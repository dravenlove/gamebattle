# 第 2 课：值、引用、指针、const、std::move

**中文** | [English](en/02-ownership.md)

> 对应文件：`src/battle_runtime.hpp`、`src/battle_state.cpp`
> 练习代码：`lessons/lesson2.cpp`（可选）

## 1. 核心问题：这个变量是我自己的，还是借来的？

在 Erlang 里，所有数据都是不可变的值。你把一个 map 传给别的函数，不用担心它被改掉，也不用关心它什么时候被回收。

C++ 不一样。每声明一个变量、每写一个参数，你都在回答一个问题：**这份数据是我自己拥有的一份，还是借用别人的？** 答错了，要么白白复制了一大堆数据，要么读到已经被释放的内存。

### 五种写法

| 写法 | 含义 | 在项目里的例子 |
|---|---|---|
| `T` | 我自己拥有一份（复制或移动过来的） | `Side other(Side side)`，`RuntimeUnit::config` |
| `const T&` | 借来的，只能读 | `validate_request(const BattleRequest&)` |
| `T&` | 借来的，可以修改 | `EffectSystem(BattleState& state)` |
| `T*` / `const T*` | 借来的，**可能为空** | `const Skill* selected = nullptr` |
| `shared_ptr<T>` | 多人共同拥有 | `Effect::buff` |

**选择规则**：
- 小类型（整数、enum、`size_t`）直接按值传。
- 大对象只读用 `const T&`，要修改用 `T&`。
- 可能"没有"的借用，用 `T*`。
- 要接管所有权时，按值接收，再配合 `std::move`。

`&` 引用是"变量的别名"：必须一出生就绑定，之后不能改绑，也不能为空。指针 `*` 是一个存地址的变量：可以为空，可以改指向，访问成员要写 `->`。

## 2. 项目里的三个所有权决定

```cpp
struct RuntimeUnit {
    UnitConfig config;              // ① 值：复制一份
};
class BattleState {
    const BattleRequest& request;   // ② const 引用：只读借用
};
class EffectSystem {
    BattleState& state_;            // ③ 引用：可写借用
};
```

**① `RuntimeUnit` 为什么复制 `config`？** `battle_state.cpp:374` 会对技能按优先级排序，排序会修改数据，但 request 是 `const`，不能动。所以每场战斗先复制一份，只在自己那份上排序。这和 Erlang 进程在自己的 State 里存一份数据是同一个思路。

**② `BattleState` 为什么借用 request？** 请求很大，复制浪费。借用的前提是：**被借的 request 必须比 `BattleState` 活得更久**。`engine.cpp:132` 的 `return runtime::BattleRunner(request).run();` 保证了这一点。

**③ `EffectSystem` 借用了它的"兄弟成员"。** 在 `BattleRunner` 里，`effects_` 持有对 `state_` 的引用。这样写没问题，但有隐藏的顺序要求，见坑 3。

## 3. 为什么用 `shared_ptr<const BuffSpec>`，而不用 `BuffSpec*`

这个问题拆成两半：`shared_ptr` 管**谁负责释放**，`const` 管**谁能修改**。

### 前半：`shared_ptr` 解决"谁来 delete"

C++ 没有垃圾回收，堆上的每一块内存都必须有人负责释放，而且**只能释放一次**：释放早了会读到已释放的内存；忘了释放会泄漏；释放两次会崩溃。普通指针只是一个地址，**不记录谁是主人**。

README 里写过：`load_config/2` 会切换到新配置包，**已经开始的战斗继续使用旧配置快照**。用普通指针模拟这个场景（用 AddressSanitizer 检测）：

```cpp
auto* old_config = new BuffSpec{801, "poison"};   // 旧配置包里的"中毒"
Effect effect{old_config};                        // 一场战斗正在用它
delete old_config;                                // 热更新：换新包，释放旧包
std::cout << effect.buff->name;                   // 战斗继续读
```
```
ERROR: AddressSanitizer: heap-use-after-free
```

不开检测工具时，它可能碰巧正确、可能输出乱码、可能崩溃，也可能静悄悄算出错误伤害，破坏确定性。NIF 模式下还会带崩整个 BEAM。

换成 `shared_ptr`：

```
use_count after load   = 1     配置仓库持有一份
use_count in battle    = 2     战斗又拿了一份
store released, battle still reads: poison (use_count=1)
battle done, freed             最后一个持有者放手，这时才真正释放
```

`shared_ptr` 内部有**引用计数**：复制加 1，销毁减 1，减到 0 自动 delete。这和 Erlang 的大 binary（>64 字节）是同一套机制。

项目里共同持有同一个 `BuffSpec` 的有：`ConfigStore::buffs_`、多个技能的 `Effect`、单位身上的 `ActiveBuff`、被动的 `Effect`……生命周期各不相同，找不出一个"最后才走的主人"，这正是 `shared_ptr` 的用武之地。

### 后半：`const` 保证"谁也改不了"

```cpp
auto spec = std::make_shared<const BuffSpec>();
spec->duration = 99;
// error: assignment of member 'BuffSpec::duration' in read-only object
```

1. **定义是共享的，改一处等于改全部。** 有了 `const`，"让这次中毒多持续一回合"这种直接改定义的写法根本编译不过。
2. **只读的数据才能安全地跨线程共享。** NIF 模式下多个线程同时跑战斗，读的是同一个 `ConfigStore`。只读不需要锁。
3. **会变的状态放在别处。** 剩余回合数、层数、来源都放在 `ActiveBuff` 里：

```cpp
struct ActiveBuff {
    std::shared_ptr<const BuffSpec> definition;  // 共享的、只读的"模板"
    std::int32_t remaining{0};                    // 这一场、这个单位自己的可变状态
    std::int32_t stacks{1};
};
```

### 代价

- **环形引用会泄漏。** A 持有 B，B 持有 A，引用计数永远降不到 0。所以配置编译器和 `validate_request` 都要拒绝 Buff 引用环（第 8、10 课）。Erlang 的 GC 能处理环，`shared_ptr` 不能。
- **有开销。** 16 字节，复制时要原子地修改引用计数。所以传参通常写 `const std::shared_ptr<const BuffSpec>&`。

### 什么时候用普通指针

| | 含义 | 什么时候用 |
|---|---|---|
| `shared_ptr<T>` | **共同拥有**：我在，它就一定在 | 生命周期不确定、有多个持有者 |
| `T*` 或 `T&` | **借用**：只看一眼，不管它的死活 | 能确定对象比我活得久 |

`execute_action` 里的 `const Skill* selected` 只活在函数内，而它指向的技能属于单位的 config，肯定活得更久，借用就够了。

**判断方法：问自己"如果对方先被释放，我会不会还在用它？"** 会，就用 `shared_ptr`；不会，就用普通指针或引用。

## 4. 下标、指针、引用：vector 扩容和删除之后谁还有效

项目里单位之间的关联一律用**下标**（`std::size_t actor_index`），而不存指针或引用。原因是：`vector` 装满后会**整体搬到一块新内存**。

### vector 是一排连续的座位

```
地址 0x...040          0x...050
     ┌──────────────┬──────────────┐
     │ 1001 / 1800  │ 2001 / 1500  │     capacity = 2（已坐满）
     └──────────────┴──────────────┘
```

`size()` 是坐了几个人，`capacity()` 是一共有几个座位。座位不够时，vector 会申请更大的新内存、把元素搬过去、**释放旧内存**。实测：

```
扩容前: 数组起始地址=0x503000000040  ptr=0x503000000050  capacity=2
扩容后: 数组起始地址=0x506000000020  ptr=0x503000000050  capacity=4
units[index].id = 2001  (下标仍然正确)
ERROR: AddressSanitizer: heap-use-after-free on address 0x503000000050
```

- **指针（引用本质上也是地址）存的是绝对地址**，搬家时没人通知它。
- **下标存的是"第几个"**，每次 `units[index]` 都用**当前的**起始地址重新算：`起始地址 + index × 元素大小`。

打个比方：全班从 A 楼搬到 B 楼。指针记的是"A 楼 205 室"，下标记的是"班里的 2 号同学"。

Erlang 根本不允许你拿到内存地址，你手里永远只有值、Pid 或 ETS 的 key。C++ 把地址交到你手里，"它还有没有效"由你负责。

### 下标也不是万能的：删除会让它错位

```cpp
std::vector<Buff> buffs{{1, 801}, {2, 802}, {3, 803}};
buffs.erase(buffs.begin());          // 801 过期被删
```
```
index=2 仍然合法吗? 否，已越界
原来 index=1 指 802，现在 buffs[1].buff_id=803     ← 静悄悄地指到了别的 Buff
```

### 所以项目对两种容器用了两种方案

- **`units` 用下标。** 单位构造时一次性加入，战斗中**只会死亡（`hp = 0`），不会被删除或新增**，下标整场稳定。
- **`buffs` 用唯一 ID。** Buff 会过期、被驱散，要 `erase`。每个实例有只增不减的 `instance_id`，之后靠它重新查找；被删的查不到，能检测出来（第 6 课）。

| 你记住的是 | 扩容（`push_back`） | 从中间删除（`erase`） | 项目里用在哪 |
|---|---|---|---|
| 指针、引用 | ❌ 读到已释放的内存 | ❌ 之后的全部失效 | 不发生这两种操作的一小段代码里 |
| 下标 | ✅ | ❌ 静悄悄错位 | `units` |
| 唯一 ID + 查找 | ✅ | ✅ 被删的查不到 | `buffs` |

> **以后实现召唤物要注意**：`take_side_turn` 里的 `auto& actor = state_.units[actor_index];` 在整个行动过程中一直被使用，安全全靠"战斗中 `units` 不增不删"。往 `units` 里 `push_back` 的那一刻，这类引用就可能失效。要么预留空位，要么改成每次用下标重新取。

## 5. 五个坑（全部实测）

### 坑 1：`auto` 会复制，`auto&` 才是引用

```cpp
auto  copy = units[0];  copy.hp -= 30;   // units[0].hp = 100  ← 改的是副本
auto& ref  = units[0];  ref.hp  -= 30;   // units[0].hp = 70
```

伤害"扣了"，但扣在一个马上被丢掉的副本上。编译不报错。项目里：要修改写 `auto&`，只读写 `const auto&`。

### 坑 2：往 vector 里加元素后，之前的引用可能失效

见第 4 节。

### 坑 3：成员按声明顺序初始化，和初始化列表的书写顺序无关

```cpp
struct Runner {
    Effects effects_;   // 声明在前 → 先构造
    State   state_;     // 声明在后 → 后构造
    Runner() : state_(), effects_(state_) {}
};
```
```
warning: 'Runner::state_' will be initialized after [-Wreorder]
effects saw round = 0 (expected 7)
```

`battle_runtime.hpp:155-156` 里 `state_` 一定写在 `effects_` 前面。**`-Wall` 的警告不要忽略。**

### 坑 4：引用成员绑定到临时对象

```cpp
Runner runner(Request{});         // 临时的 Request 在这一行结束时就被销毁
runner.first_hp();                // 💥 stack-use-after-scope
```

`BattleRunner(request).run()` 把构造和调用放在**同一个表达式**里，所以安全。拆成两行并传入临时对象，就会出问题，而且编译器**不会警告**。

### 坑 5：`std::move` 本身不移动任何东西

```cpp
std::vector<int> result = std::move(events);
// result.size=10000  events.size=0  same buffer=1
std::vector<int> copy = result;
// copy same buffer=0
```

`std::move` 只是类型转换，意思是"我不再需要它了，你可以拿走它的内部数据"。接收方直接接管那块内存，一个元素都不复制。被拿走的对象仍然合法，但**不要再读它的内容**。

项目里的三个典型位置：

```cpp
units.push_back(std::move(runtime));      // ① 局部变量交出所有权
void emit(std::string type, ...) {        // ② 按值接收，再移进去
    result.events.push_back(Event{.type = std::move(type), ...});
}
BattleResult finish() {
    return std::move(result);             // ③ 返回成员时必须显式 move
}
```

第 ③ 点：`return 局部变量;` 编译器会自动移动；但 `result` 是**成员**，编译器不敢擅自拿走，不写 `std::move` 就会复制整个 events 数组。

## 6. const 成员函数

```cpp
bool alive() const { return hp > 0; }          // 保证不修改对象自身
Stats effective_stats(std::size_t index);      // 没有 const
```

`effective_stats` 听起来像查询，却**没有** `const`，因为它会写入缓存字段 `cached_stats` 和 `stats_dirty`。`const` 描述的是"会不会修改对象"，不是"看起来像不像 getter"。`const` 对象上只能调用 `const` 成员函数。

## 可选练习

```bash
g++ -std=c++20 -Wall -Wextra -Wpedantic -g -fsanitize=address lessons/lesson2.cpp -o lesson2 && ./lesson2
```

每一题都是故意把代码改坏，观察后果：
1. 把 `apply_damage` 里的 `auto& target` 改成 `auto target`。
2. 交换 `BattleRunner` 里 `state_` 和 `effects_` 的声明顺序。
3. 在 `run()` 里先拿 `auto& actor = state_.units[0];`，再 `push_back`，再用 `actor`。
4. 删掉 `finish()` 里的 `std::move`，想想多做了什么。

## 小结

| 概念 | 要点 |
|---|---|
| 值 / `const&` / `&` / `*` / `shared_ptr` | 自己拥有 / 只读借用 / 可写借用 / 可空借用 / 共同拥有 |
| `shared_ptr<const T>` | 引用计数管释放，`const` 管修改；小心环 |
| 下标 vs 指针 | 扩容后下标有效、指针失效；删除后两者都不可靠，用唯一 ID |
| `auto` vs `auto&` | `auto` 是副本 |
| 成员初始化顺序 | 按声明顺序 |
| `std::move` | 只是"允许拿走"；返回成员时要显式写 |

下一课：[第 3 课：类与确定性随机数](03-class-and-random.md)
