# 第 12 课：对象模型与多态

**中文** | [English](en/12-object-model.md)

> 第二部分「C++ 语言深入」的第一课。从这一课开始，内容超出了项目本身用到的范围，覆盖的是 C++ 岗位面试必问、而这个项目恰好没怎么用的部分。每一处都会落回项目，讨论"项目为什么这么设计、换一种写法会怎样"。

## 1. 先问一个设计问题：项目为什么几乎不用虚函数

`Effect` 在项目里是这样表示的（第 6 课）：

```cpp
enum class EffectKind : std::uint8_t { damage, heal, add_buff, remove_buff, direct_damage };
struct Effect { EffectKind kind; TargetRule target; ... };

switch (executable->kind) {
case EffectKind::damage: ...
case EffectKind::heal: ...
}
```

教科书式的面向对象写法会是：

```cpp
class Effect {
public:
    virtual ~Effect() = default;
    virtual void apply(EffectContext& context) const = 0;   // 纯虚函数
};
class DamageEffect : public Effect { void apply(EffectContext&) const override; };
class HealEffect   : public Effect { void apply(EffectContext&) const override; };
std::vector<std::unique_ptr<Effect>> effects;
```

两种写法各有道理，面试时经常被问"你为什么这么设计"：

| | `enum` + `switch`（项目的写法） | 继承 + 虚函数 |
|---|---|---|
| 新增一种效果 | 要改 `switch`（编译器会提醒漏改的地方，第 6 课） | 新写一个子类，不碰旧代码 |
| 新增一种"对所有效果的操作"（比如序列化、校验） | 新写一个函数，一个 `switch` | 每个子类都要加一个虚函数 |
| 从配置表 / ETF 加载 | 直接填字段 | 要写"根据类型字符串 new 出对应子类"的工厂 |
| 内存布局 | 连续的 `vector<Effect>` | `vector<unique_ptr>`，对象分散在堆上 |
| 跨进程传输、写进二进制文件 | 天然可以 | 指针和虚表都不能序列化 |

这叫**表达式问题**（expression problem）：类型集合经常变，适合继承；操作集合经常变，适合 `switch` / `variant`。这个项目的效果类型很少变（而且要跟配置文件格式保持一致），但对效果的操作很多（解析、校验、执行、编码、按层数放大），所以选了数据驱动的写法。

我实测了三种分派方式的速度（100 万个随机效果，跑 20 遍，`-O2`）：

```
enum + switch (连续数组)       : 7.3 ns/op
virtual (unique_ptr 分散在堆上): 10.5 ns/op
variant + visit (连续数组)     : 7.0 ns/op
```

虚函数版本慢了约 45%，原因是"间接调用 + 对象分散在堆上"。但三者的主要开销其实都是**效果类型随机、分支预测失败**。所以面试时的正确说法是：性能差异存在，但这个项目选数据驱动的主要原因是**配置驱动和可序列化**，不是速度。

下面把面试需要的继承和多态知识系统地过一遍。

## 2. 继承的基本写法

项目里唯一的继承是异常类（第 8 课）：

```cpp
class DecodeError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
```

- `public` 继承表示"是一个"（is-a）：`DecodeError` 是一个 `runtime_error`，凡是需要 `runtime_error&` 的地方都能传它。
- `protected` / `private` 继承很少用，表示"用它来实现"，外部看不到继承关系。
- 访问控制：`public` 谁都能访问；`protected` 只有自己和子类能访问；`private` 只有自己能访问。
- `final` 用在类上表示禁止再派生；用在虚函数上表示禁止子类再覆盖。

## 3. 虚函数：通过基类指针调用子类的实现

```cpp
struct Effect {
    virtual ~Effect() = default;
    virtual const char* name() const { return "effect"; }
    const char* plain_name() const { return "effect"; }        // 非虚
};
struct Damage : Effect {
    const char* name() const override { return "damage"; }
    const char* plain_name() const { return "damage"; }        // 只是"隐藏"了基类的同名函数
};

std::unique_ptr<Effect> effect = std::make_unique<Damage>();
effect->name();         // "damage"：虚函数，看对象实际是什么类型
effect->plain_name();   // "effect"：非虚函数，看指针声明成什么类型
```

（实测输出：`virtual name(): damage   non-virtual plain_name(): effect`。）

- **动态分派**：虚函数在运行时根据对象的**实际类型**选择实现。
- **静态分派**：非虚函数在编译时根据指针或引用的**声明类型**决定。
- 多态只能通过**指针或引用**发挥作用。按值传递会发生**对象切片**（第 8 课第 5 节），子类部分被切掉，虚函数也就只能调到基类版本。

### `override`：一定要写

子类函数的签名和基类**有一点点不同**（比如少了一个 `const`），它就不是覆盖，而是一个全新的函数。不写 `override` 时（实测）：

```cpp
struct Effect { virtual std::int64_t apply(std::int64_t hp) const; };
struct Heal : Effect { std::int64_t apply(std::int64_t hp) { return hp + 10; } };   // 少写了 const

const Effect& e = Heal{};
e.apply(100);    // 结果 100，期望 110：调用的还是基类版本
```

编译能过。开了 `-Wall` 才有一条不太起眼的警告 `'virtual ... Effect::apply(int64_t) const' was hidden`。写上 `override` 后直接编译失败：

```
error: 'int64_t Heal::apply(int64_t)' marked 'override', but does not override
```

**规则：覆盖虚函数时一律写 `override`。**

## 4. 虚函数是怎么实现的：vptr 和 vtable

实测：

```
sizeof(Plain)=8 sizeof(Virtual)=16 sizeof(TwoVirtual)=24
```

`Plain` 只有一个 `long`，8 字节。`Virtual` 同样只有一个 `long`，却是 16 字节：多出来的 8 字节是编译器偷偷加上的**虚表指针**（vptr）。

```
对象内存                     虚函数表（每个类一张，所有对象共享，存在只读数据段）
┌────────────┐              ┌─────────────────────────┐
│ vptr ──────┼────────────▶ │ &TwoVirtual::~TwoVirtual │
│ hp         │              │ &TwoVirtual::damage      │
│ shield     │              └─────────────────────────┘
└────────────┘
```

调用 `effect->damage()` 时，编译器生成的代码是：

1. 从对象开头读出 vptr；
2. 在虚表里取第 N 项（N 在编译时就确定了）；
3. **间接调用**那个地址。

代价有三个：
- 每个对象多 8 字节；
- 一次额外的内存读取，外加一次间接跳转，CPU 不一定能预测；
- **编译器通常无法内联虚函数**，后续优化也跟着做不了。这往往是比间接跳转本身更大的损失。

如果编译器能确定对象的实际类型（比如类标了 `final`，或者对象就在眼前），它可以**去虚化**（devirtualization），把虚调用变成普通调用甚至内联。

## 5. 虚析构函数

```cpp
struct Base { ~Base() { std::cout << "~Base\n"; } };               // 析构函数不是虚的
struct Derived : Base { std::vector<int> events = std::vector<int>(1000); ~Derived() { ... } };

Base* p = new Derived;
delete p;
```

实测，普通运行只打印了：

```
  ~Base
```

`~Derived` **根本没被调用**，`events` 里的内存泄漏了。这在标准里是**未定义行为**。AddressSanitizer 能抓到：

```
ERROR: AddressSanitizer: new-delete-type-mismatch
```

**规则：一个类只要打算被当成基类、通过基类指针删除子类对象，析构函数就必须是 `virtual`。** 反过来，不打算被继承的类，就标上 `final`。项目的 `DecodeError final` 就是这样。

`std::exception` 的析构函数本身是虚的，所以项目里通过 `const std::exception&` 处理各种异常没有问题。

## 6. 构造函数里调用虚函数，调不到子类

```cpp
struct Unit {
    Unit() { std::cout << kind(); }                    // 构造函数里调用虚函数
    virtual const char* kind() const { return "unit"; }
};
struct Hero : Unit { const char* kind() const override { return "hero"; } };

Hero h;     // 打印 "unit"，不是 "hero"
```

（实测：`Unit 构造中调用 kind(): unit`，构造完成后调用才是 `hero`。）

原因：构造 `Hero` 时，**先构造基类 `Unit`**。执行 `Unit` 构造函数的那一刻，`Hero` 的部分还不存在，对象此时就只是一个 `Unit`，vptr 指向的也是 `Unit` 的虚表。析构时顺序相反，同样的道理。

**规则：不要在构造函数和析构函数里调用虚函数。**

## 7. 纯虚函数、抽象类、接口

```cpp
class BattleObserver {
public:
    virtual ~BattleObserver() = default;
    virtual void on_event(const Event& event) = 0;    // = 0：纯虚函数
};
```

- 含有纯虚函数的类是**抽象类**，不能直接创建对象，只能被继承。
- 只有纯虚函数、没有数据的类就相当于其他语言里的"接口"。
- 它和 Erlang 的 **behaviour** 很像：`-callback on_event(Event) -> ok.` 规定了回调模块必须实现哪些函数。`gen_server` 就是一个 behaviour。

## 8. 多重继承与菱形继承

面试常问，实际项目里尽量避免。

```cpp
struct Entity { long id = 0; };
struct Movable : Entity {};
struct Attackable : Entity {};
struct Hero : Movable, Attackable {};          // 菱形：Hero 里有两份 Entity

Hero h;
h.id = 1;    // error: request for member 'id' is ambiguous
```

用**虚继承**解决：

```cpp
struct VMovable : virtual Entity {};
struct VAttackable : virtual Entity {};
struct VHero : VMovable, VAttackable {};       // 只有一份 Entity
```

实测：

```
普通菱形: sizeof=16，两份 id: 1,2
虚继承:   sizeof=24，只有一份 id: 3
```

虚继承多出来的空间用来在运行时定位那唯一一份基类。代价是对象更大，访问基类成员要多一次间接寻址。游戏实体建模如果走到菱形继承这一步，通常说明该换成**组合**或 **ECS**（第 20 课）了。

## 9. RTTI：`typeid` 和 `dynamic_cast`

```cpp
catch (const std::exception& e) {
    typeid(e).name();                               // "11DecodeError"（编译器修饰过的名字）
    dynamic_cast<const DecodeError*>(&e);           // 成功：返回指针
    dynamic_cast<const std::logic_error*>(&e);      // 失败：返回 nullptr
    dynamic_cast<const std::logic_error&>(e);       // 失败：抛 std::bad_cast
}
```

（以上全部实测。）

- `dynamic_cast` 在运行时检查对象的实际类型，安全地做向下转换。它依赖**运行时类型信息**（RTTI），本身有开销。
- 需要大量 `dynamic_cast` 的代码通常说明设计有问题：本该用虚函数解决。
- 一些游戏引擎会用 `-fno-rtti` 关掉 RTTI 来减小体积，这时 `dynamic_cast` 和 `typeid` 都不能用。
- `static_cast` 做向下转换**不检查**，类型不对就是未定义行为。

## 10. 不用虚函数的多态

| 方式 | 何时确定调哪个实现 | 项目里 |
|---|---|---|
| 虚函数 | 运行时（查虚表） | 异常类 |
| `enum` + `switch` | 运行时（分支） | `EffectKind`、`TargetRule`、`Trigger` |
| `std::variant` + `std::visit` | 运行时（按 index 跳转） | `term::Value`（第 9 课） |
| 模板 / CRTP | 编译时 | `checked_int<T>`（第 15 课） |

CRTP 留到第 15 课讲。

## 面试题

**Q1：虚函数是怎么实现的？有什么开销？**
每个含虚函数的类有一张虚表（函数指针数组），每个对象开头有一个指向它的 vptr。调用时读 vptr、按编译期确定的下标取函数地址、间接调用。开销：对象多 8 字节，多一次内存读取和间接跳转，而且通常不能内联。

**Q2：为什么基类析构函数要声明为虚函数？**
通过基类指针 `delete` 子类对象时，析构函数不是虚的，就只会调用基类析构，子类部分不会被析构，这是未定义行为（实测会泄漏子类的成员）。只要类会被多态地删除，析构函数就要是虚的。

**Q3：构造函数能是虚函数吗？构造函数里调用虚函数会怎样？**
构造函数不能是虚的：创建对象时必须明确知道类型，而且 vptr 正是在构造过程中设置的。在构造函数里调用虚函数，调到的是当前正在构造的那一层的版本，不会调到子类。

**Q4：`override` 和 `final` 有什么用？**
`override` 让编译器检查这个函数确实覆盖了基类的虚函数，签名写错会编译失败；不写的话会悄悄变成一个新函数。`final` 禁止继续覆盖或继承，也给编译器去虚化的机会。

**Q5：什么是对象切片？**
把子类对象按值赋给基类对象时，只复制基类部分，子类的数据和虚函数行为都丢失。多态必须通过指针或引用。`catch` 异常要按引用捕获也是这个原因。

**Q6：菱形继承有什么问题？怎么解决？**
最底层的类会包含两份最顶层基类的数据，访问时有歧义。用虚继承可以只保留一份，代价是对象更大、访问多一次间接寻址。实践中更推荐用组合代替多重继承。

**Q7：`dynamic_cast` 和 `static_cast` 的区别？**
`dynamic_cast` 在运行时检查类型，失败时指针返回 `nullptr`、引用抛 `std::bad_cast`，需要 RTTI 和多态类型。`static_cast` 只在编译时检查，向下转换不做运行时检查，类型不对就是未定义行为。

**Q8：你的项目为什么用 `enum` + `switch` 而不用虚函数？**
效果类型和配置文件格式绑定，很少变化，而对效果的操作很多（解析、校验、执行、编码）；数据要从配置表加载、经 ETF 传输，`enum` + 普通结构体天然可以序列化，对象还能连续存放。新增类型时漏改 `switch`，编译器会给出警告。实测虚函数分派慢约 45%，但这不是主要原因。

下一课：[第 13 课：特殊成员函数、值类别与移动语义](13-move-semantics.md)
