# 第 8 课：校验与异常

> 对应文件：`src/battle_state.cpp:97-313`（`validate_request`，**抛出**）、`src/wire.cpp:608-658`（`handle_etf`，**捕获**）

## 1. 全局：错误是怎么一路传回 Erlang 的

```
Erlang: gamebattle:simulate(port, Request)
   │
   ▼  ETF 字节
handle_etf  ── try { ─────────────────────────────────────────────┐
   ├─ term::decode           → 字节格式不对：  throw DecodeError   │
   ├─ parse_request          → 字段缺失或枚举非法：throw DecodeError │
   └─ Engine::simulate                                             │
        └─ BattleState 构造函数                                     │
             └─ validate_request → 数值越界：throw invalid_argument │
   } catch ── 按异常类型翻译 ◀────────────────────────────────────────┘
   ▼
{error, #{type => invalid_request, message => <<"max_rounds must be between 1 and 10000">>}}
```

无论错误发生在多深的地方，都会一路"弹"回 `handle_etf`，被翻译成 Erlang 能理解的元组。

## 2. throw 之后发生了什么

```cpp
void validate() { Guard g{"validate 里的局部对象"}; throw std::invalid_argument("max_rounds ..."); }
void build()    { Guard g{"build 里的局部对象"};    validate(); std::cout << "这行不会执行\n"; }
int main() {
    try { build(); }
    catch (const std::invalid_argument& e) { std::cout << "捕获: " << e.what() << '\n'; }
}
```

实测：

```
  析构 validate 里的局部对象
  析构 build 里的局部对象
捕获: max_rounds must be between 1 and 10000
```

1. 当前函数**立刻停止**；
2. 沿调用链一层层往回退，**自动销毁每一层的局部对象**（栈展开）；
3. 直到遇到类型匹配的 `catch`。

第 2 步就是 **RAII**：把资源释放写在析构函数里，无论正常返回还是被异常打断，析构函数都会执行。vector 的内存、`shared_ptr` 的引用计数、锁（`std::unique_lock`）全部自动释放。

## 3. 和 Erlang 对比：最大的差别是"没有进程隔离"

| | Erlang | C++ |
|---|---|---|
| 抛出 | `throw(R)` / `error(R)` / `exit(R)` | `throw 异常对象;` |
| 捕获 | `try ... catch Class:Reason -> ... end` | `try { ... } catch (const 类型& e) { ... }` |
| 按什么匹配 | 模式匹配 | 按**类型**匹配（包括父类） |
| 没人捕获 | **只有当前进程**退出，监督者重启 | **整个操作系统进程**终止 |

实测没人捕获时：

```
terminate called after throwing an instance of 'std::runtime_error'
  what():  nobody catches me
Aborted          进程退出码=134
```

- **Port 模式**：挂掉的是 `gamebattle_port` 这个独立进程，监督者重启它。
- **NIF 模式**：C++ 运行在 **BEAM 进程内部**，挂掉的是**整个 Erlang 节点**。

所以 `handle_etf` 最外层有 `catch (...)`：它是 C++ 和 Erlang 之间的**防火墙**。

## 4. 异常类型的继承关系，以及 catch 的顺序

```
std::exception                      所有标准异常的基类，提供 what()
 ├─ std::logic_error
 │   ├─ std::invalid_argument       ← validate_request 抛这个
 │   └─ std::out_of_range           ← ConfigStore::require_* 找不到 ID
 └─ std::runtime_error
     └─ term::DecodeError           ← 项目自定义：ETF 解析失败
```

`handle_etf` 的 catch 链：

```cpp
} catch (const term::DecodeError& error) {         // ① 最具体的放前面
    return term::encode(error_value("invalid_request", error.what()));
} catch (const std::invalid_argument& error) {
    return term::encode(error_value("invalid_request", error.what()));
} catch (const std::exception& error) {            // ② 其他标准异常
    return term::encode(error_value("internal_error", error.what()));
} catch (...) {                                    // ③ 连标准异常都不是的任何东西
    return term::encode(error_value("internal_error", "unknown C++ exception"));
}
```

catch **从上往下匹配，子类能被父类接住**，所以具体的写前面。写反了 GCC 会警告：

```
warning: exception of type 'DecodeError' will be caught by earlier handler [-Wexceptions]
```

两类错误让 Erlang 采取不同策略：
- `invalid_request`：**调用方的错**，不应重试，应修正请求。
- `internal_error`：**C++ 这边出了意外**，需要报警、排查。

## 5. 一定要按引用捕获

```cpp
catch (const std::exception& e)   // ✅
catch (std::exception e)          // ❌
```

实测抛出 `DecodeError("unit.kind must be hero")`：

```
按值:   what()=std::exception                ← 具体信息丢了
按引用: what()=unit.kind must be hero
```

按值捕获只复制出父类那一部分，子类数据被"切掉"（**对象切片**）。GCC 会警告 `catching polymorphic type by value`。

## 6. 自定义异常只需要三行

```cpp
class DecodeError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;   // 继承构造函数
};
```

为什么不直接用 `runtime_error`？为了**能在 catch 里区分**：`DecodeError` 归为 `invalid_request`，其他 `runtime_error` 是 `internal_error`。**异常的类型就是它携带的分类信息**，相当于 Erlang 里用不同 atom 标记错误原因。

## 7. 异常翻译：捕获、补充上下文、再抛出

`wire.cpp:432-444`：

```cpp
try {
    ...unit.skills.push_back(configs->require_skill(id));
} catch (const std::out_of_range& error) {
    throw term::DecodeError(std::string("unit loadout: ") + error.what());
}
```

`ConfigStore` 找不到技能 ID 抛 `out_of_range`，按分类会变成 `internal_error`。但"请求里引用了不存在的技能 ID"明明是请求的错，所以这里加上上下文、以 `DecodeError` 重新抛出。**底层不知道自己被谁调用，没法判断是谁的错；上层知道，由上层负责翻译。**

## 8. `validate_request` 的结构

### 小 lambda 当局部判断函数

```cpp
const auto valid_probability = [](BasisPoints value) {
    return value >= 0 && value <= kBasisPoints;
};
```

### 同时遍历两个对象

```cpp
for (const auto* formation : {&request.attacker, &request.defender}) {
    for (const auto& unit : formation->units) { ... }
}
```

`{&a, &b}` 临时构造一个包含两个指针的列表，相当于 `lists:foreach(F, [Attacker, Defender])`。存指针避免复制阵型。

### `emplace` / `insert` 的返回值：顺手检查重复

```cpp
if (unit.id == 0 || !configs.emplace(unit.id, &unit).second) {
    throw std::invalid_argument("unit ids must be non-zero and unique across both sides");
}
```

返回一对值 `(迭代器, 是否真的插入了)`。**key 已存在时不覆盖，返回 `false`**，插入和查重一步完成。

C++17 的**结构化绑定**把这一对值拆开：

```cpp
const auto [known, inserted] = buff_definitions.emplace(buff->id, buff);
```

实测：`第一次 inserted=1  第二次 inserted=0  map 里保留的是 poison-A`。形式上就是 Erlang 的 `{Known, Inserted} = ...`，只能按位置拆开，不能同时匹配具体值。

## 9. 互相递归的 lambda：必须借助 `std::function`

```
validate_effect(效果) ──效果是 add_buff──▶ validate_buff(Buff)
       ▲                                      │
       └───────── Buff 的每个 reaction 里的每个效果 ┘
```

普通 lambda 不能调用自己（实测）：

```cpp
auto depth_of = [&](std::size_t n) { return n == 0 ? 0 : 1 + depth_of(n - 1); };
// error: use of 'depth_of' before deduction of 'auto'
```

编译器要**看完整个 lambda** 才能推断 `depth_of` 的类型，而内部已经要用它了。项目的解决办法（`battle_state.cpp:123-127`）：

```cpp
std::function<void(const Effect&, std::size_t)> validate_effect;                       // ① 先声明，类型写明
std::function<void(const std::shared_ptr<const BuffSpec>&, std::size_t)> validate_buff;

validate_buff = [&](const std::shared_ptr<const BuffSpec>& definition, std::size_t depth) {
    ...validate_effect(effect, depth + 1);    // ② 按引用捕获了盒子本身
};
validate_effect = [&](const Effect& effect, std::size_t depth) {
    ...validate_buff(effect.buff, depth + 1);
};
```

`std::function<返回类型(参数...)>` 是"能装任何可调用对象的盒子"，类型事先写明。先声明两个空盒子，再装 lambda；真正调用时两个盒子都已装好。实测互相递归正常工作。

Erlang 模块内的函数天然可以互相调用。`std::function` 比直接调用稍慢，但校验每个请求只执行一次，不影响性能。

## 10. 用三色标记检测 Buff 引用环

```cpp
std::unordered_set<const BuffSpec*> validating_buffs;   // 灰色：正在检查
std::unordered_set<const BuffSpec*> validated_buffs;    // 黑色：检查完了
                                                        // 白色：都不在，还没碰过
validate_buff = [&](...) {
    const auto* buff = definition.get();
    if (validated_buffs.contains(buff))  return;                         // 黑色：跳过
    if (validating_buffs.contains(buff)) throw ...("ownership cycle");   // 灰色：绕回来了 → 有环！
    validating_buffs.insert(buff);                                       // 标灰
    for (反应里的每个效果) validate_effect(effect, depth + 1);
    validating_buffs.erase(buff);
    validated_buffs.insert(buff);                                        // 标黑
};
```

例：中毒 A 的反应加灼烧 B，灼烧 B 的反应又加中毒 A：

```
检查 A → A 标灰 → 发现 add_buff B
  → 检查 B → B 标灰 → 发现 add_buff A
    → A 是灰色的！→ throw "ownership cycle"
```

- 集合存**指针**，因为要判断"是不是同一个对象"。`definition.get()` 取出普通指针，是校验期间的借用。
- `contains()` 是 C++20 新方法，以前写 `find(x) != end()`。
- 配置编译器和 `ConfigStore` 也会做环检测，用的是另一种算法（拓扑排序，第 10 课）。请求里的内联 Buff 不经过编译器，所以运行时还要再查一次。

## 11. 什么时候用异常，什么时候不用

- **用异常**：输入非法、配置损坏。一旦发生，整个请求作废。
- **不用异常**：战斗中的正常分支（没命中、目标已死、找不到 Buff），用返回值、`optional`、`end()`、提前 return。

C++ 异常**不抛时几乎零开销，一旦抛出代价很大**，适合"很少发生、发生了就放弃整个操作"。

`noexcept` 向调用方承诺不抛异常：

```cpp
const BuffSpec* find_buff(std::uint32_t id) const noexcept;   // 找不到返回 nullptr
```

> **进阶观察**：`handle_etf` 的 catch 块里调用了 `term::encode(...)`，它要申请内存。极端情况下（内存耗尽）这一步本身也可能抛 `std::bad_alloc`，而外面已经没有保护了。Port 模式下只会让 Port 进程退出、由监督者重启；NIF 模式下会波及整个 BEAM。概率极低，但说明 **NIF 的边界必须做到绝对不漏出异常**，这比 Port 要求高得多，也是 README 说"NIF 只在充分压测和模糊测试后启用"的原因之一。

## 小结

| 概念 | 要点 | Erlang |
|---|---|---|
| `throw` / `catch` | 按类型匹配，子类能被父类接住 | `try ... catch Class:Reason` |
| 栈展开 / RAII | 一层层退出时自动析构局部对象 | 进程退出时释放资源 |
| 没人捕获 | `std::terminate`，**整个进程**终止 | 只有当前进程退出 |
| catch 顺序 | 具体的写前面 | 子句顺序 |
| 按引用捕获 | 按值会切片 | 无 |
| 自定义异常 | 类型就是分类 | 不同的 atom |
| 异常翻译 | 上层补上下文再抛出 | `catch ... -> {error, ...}` |
| `emplace().second` | 插入和查重一步完成 | `maps:is_key` + `maps:put` |
| 结构化绑定 | `auto [a, b] = ...` | `{A, B} = ...` |
| 互相递归的 lambda | 先声明 `std::function` 再赋值 | 天然可以 |
| 三色标记 | 灰色节点被再次访问 → 有环 | `digraph:get_cycle/2` |

下一课：[第 9 课：对接 Erlang](09-erlang-bridge.md)
