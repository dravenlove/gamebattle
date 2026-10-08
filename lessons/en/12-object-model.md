# Lesson 12: Object model and polymorphism

[中文](../12-object-model.md) | **English**

> The first lesson of part 2, "C++ in depth". From here on the material goes beyond what the project itself uses, covering what C++ job interviews always ask about and this project happens to use little of. Every topic comes back to the project to discuss "why the project is designed this way, and what would happen with a different approach".

## 1. A design question first: why does the project barely use virtual functions?

This is how the project represents an `Effect` (lesson 6):

```cpp
enum class EffectKind : std::uint8_t { damage, heal, add_buff, remove_buff, direct_damage, negate };
struct Effect { EffectKind kind; TargetRule target; ... };

switch (executable->kind) {
case EffectKind::damage: ...
case EffectKind::heal: ...
}
```

The textbook object-oriented version would be:

```cpp
class Effect {
public:
    virtual ~Effect() = default;
    virtual void apply(EffectContext& context) const = 0;   // pure virtual function
};
class DamageEffect : public Effect { void apply(EffectContext&) const override; };
class HealEffect   : public Effect { void apply(EffectContext&) const override; };
std::vector<std::unique_ptr<Effect>> effects;
```

Both have their merits, and interviews often ask "why did you design it this way":

| | `enum` + `switch` (the project's approach) | Inheritance + virtual functions |
|---|---|---|
| Adding a new effect | Change the `switch` (the compiler flags the places you missed, lesson 6) | Write a new subclass without touching old code |
| Adding a new "operation on every effect" (say serialization or validation) | Write one new function with one `switch` | Add a virtual function to every subclass |
| Loading from config tables / ETF | Fill in the fields directly | Need a factory that "news up the right subclass from a type string" |
| Memory layout | A contiguous `vector<Effect>` | `vector<unique_ptr>`, objects scattered across the heap |
| Sending across processes, writing to binary files | Works naturally | Pointers and vtables can't be serialized |

This is the **expression problem**: when the set of types changes often, inheritance fits; when the set of operations changes often, `switch` / `variant` fits. This project's effect types rarely change (and must stay in step with the config file format), but there are many operations on effects (parsing, validation, execution, encoding, scaling by stacks), so it chose the data-driven approach.

I measured the speed of three dispatch methods (1 million random effects, 20 passes, `-O2`):

```
enum + switch (contiguous array)          : 7.3 ns/op
virtual (unique_ptr scattered on the heap): 10.5 ns/op
variant + visit (contiguous array)        : 7.0 ns/op
```

The virtual version is about 45% slower, because of "indirect calls + objects scattered on the heap". But the main cost in all three is really **random effect types causing branch mispredictions**. So the right thing to say in an interview: the performance difference exists, but the project chose the data-driven approach mainly for **config-driven design and serializability**, not speed.

Below is a systematic pass over the inheritance and polymorphism knowledge interviews require.

## 2. Basic inheritance syntax

The only inheritance in the project is the exception class (lesson 8):

```cpp
class DecodeError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
```

- `public` inheritance means "is a": `DecodeError` is a `runtime_error`, and can be passed anywhere a `runtime_error&` is expected.
- `protected` / `private` inheritance is rare and means "implemented in terms of"; the inheritance relationship is invisible from outside.
- Access control: `public` is accessible to anyone; `protected` only to the class itself and its subclasses; `private` only to the class itself.
- `final` on a class forbids further derivation; on a virtual function it forbids subclasses from overriding it again.

## 3. Virtual functions: calling the subclass implementation through a base pointer

```cpp
struct Effect {
    virtual ~Effect() = default;
    virtual const char* name() const { return "effect"; }
    const char* plain_name() const { return "effect"; }        // non-virtual
};
struct Damage : Effect {
    const char* name() const override { return "damage"; }
    const char* plain_name() const { return "damage"; }        // merely "hides" the base function of the same name
};

std::unique_ptr<Effect> effect = std::make_unique<Damage>();
effect->name();         // "damage": virtual, depends on the object's actual type
effect->plain_name();   // "effect": non-virtual, depends on the pointer's declared type
```

(Measured output: `virtual name(): damage   non-virtual plain_name(): effect`.)

- **Dynamic dispatch**: a virtual function picks its implementation at run time from the object's **actual type**.
- **Static dispatch**: a non-virtual function is decided at compile time from the **declared type** of the pointer or reference.
- Polymorphism only works through **pointers or references**. Passing by value causes **object slicing** (lesson 8, section 5): the subclass part is sliced off, so virtual calls can only reach the base version.

### `override`: always write it

If the subclass function's signature differs from the base **even slightly** (a missing `const`, say), it isn't an override but a brand-new function. Without `override` (measured):

```cpp
struct Effect { virtual std::int64_t apply(std::int64_t hp) const; };
struct Heal : Effect { std::int64_t apply(std::int64_t hp) { return hp + 10; } };   // forgot the const

const Effect& e = Heal{};
e.apply(100);    // the result is 100, expected 110: still calls the base version
```

It compiles. With `-Wall` there's only an easy-to-miss warning, `'virtual ... Effect::apply(int64_t) const' was hidden`. With `override` it fails to compile outright:

```
error: 'int64_t Heal::apply(int64_t)' marked 'override', but does not override
```

**Rule: always write `override` when overriding a virtual function.**

## 4. How virtual functions are implemented: the vptr and the vtable

Measured:

```
sizeof(Plain)=8 sizeof(Virtual)=16 sizeof(TwoVirtual)=24
```

`Plain` has a single `long`: 8 bytes. `Virtual` also has a single `long` but is 16 bytes: the extra 8 bytes are a **virtual table pointer** (vptr) the compiler quietly adds.

```
object memory                virtual function table (one per class, shared by all objects, in read-only data)
┌────────────┐              ┌─────────────────────────┐
│ vptr ──────┼────────────▶ │ &TwoVirtual::~TwoVirtual │
│ hp         │              │ &TwoVirtual::damage      │
│ shield     │              └─────────────────────────┘
└────────────┘
```

For `effect->damage()`, the compiler generates code that:

1. reads the vptr from the start of the object;
2. takes entry N from the vtable (N is fixed at compile time);
3. makes an **indirect call** to that address.

There are three costs:
- 8 extra bytes per object;
- an extra memory read plus an indirect jump the CPU may not predict;
- **the compiler usually can't inline a virtual function**, so the optimizations that would follow are lost too. That's often a bigger loss than the indirect jump itself.

When the compiler can be sure of the object's actual type (say the class is marked `final`, or the object is right there in view), it can **devirtualize**, turning the virtual call into a normal call or even inlining it.

## 5. Virtual destructors

```cpp
struct Base { ~Base() { std::cout << "~Base\n"; } };               // the destructor isn't virtual
struct Derived : Base { std::vector<int> events = std::vector<int>(1000); ~Derived() { ... } };

Base* p = new Derived;
delete p;
```

Measured, a normal run prints only:

```
  ~Base
```

`~Derived` **is never called**, and the memory in `events` leaks. The standard calls this **undefined behavior**. AddressSanitizer catches it:

```
ERROR: AddressSanitizer: new-delete-type-mismatch
```

**Rule: if a class is meant to be a base class and subclass objects may be deleted through a base pointer, its destructor must be `virtual`.** Conversely, mark classes that aren't meant to be inherited from `final`. The project's `DecodeError final` does exactly that.

`std::exception`'s destructor is itself virtual, so handling all kinds of exceptions through `const std::exception&` in the project is fine.

## 6. Calling a virtual function in a constructor doesn't reach the subclass

```cpp
struct Unit {
    Unit() { std::cout << kind(); }                    // calling a virtual function in the constructor
    virtual const char* kind() const { return "unit"; }
};
struct Hero : Unit { const char* kind() const override { return "hero"; } };

Hero h;     // prints "unit", not "hero"
```

(Measured: `kind() called during Unit construction: unit`; called after construction finishes, it's `hero`.)

The reason: constructing a `Hero` **constructs the base `Unit` first**. While the `Unit` constructor runs, the `Hero` part doesn't exist yet; at that moment the object is just a `Unit`, and its vptr points to `Unit`'s vtable. Destruction runs in reverse order, for the same reason.

**Rule: don't call virtual functions in constructors or destructors.**

## 7. Pure virtual functions, abstract classes, interfaces

```cpp
class BattleObserver {
public:
    virtual ~BattleObserver() = default;
    virtual void on_event(const Event& event) = 0;    // = 0: a pure virtual function
};
```

- A class with a pure virtual function is **abstract**: you can't create objects of it directly, only inherit from it.
- A class with only pure virtual functions and no data is what other languages call an "interface".
- It's a lot like an Erlang **behaviour**: `-callback on_event(Event) -> ok.` specifies which functions a callback module must implement. `gen_server` is a behaviour.

## 8. Multiple inheritance and diamond inheritance

Interviews ask about it a lot; real projects avoid it as much as they can.

```cpp
struct Entity { long id = 0; };
struct Movable : Entity {};
struct Attackable : Entity {};
struct Hero : Movable, Attackable {};          // a diamond: Hero contains two Entity subobjects

Hero h;
h.id = 1;    // error: request for member 'id' is ambiguous
```

**Virtual inheritance** solves it:

```cpp
struct VMovable : virtual Entity {};
struct VAttackable : virtual Entity {};
struct VHero : VMovable, VAttackable {};       // only one Entity
```

Measured:

```
plain diamond:       sizeof=16, two ids: 1,2
virtual inheritance: sizeof=24, only one id: 3
```

The extra space in virtual inheritance is used to locate the single base subobject at run time. The cost is a bigger object and an extra indirection when accessing base members. If modeling game entities ever gets to diamond inheritance, it usually means it's time to switch to **composition** or **ECS** (lesson 20).

## 9. RTTI: `typeid` and `dynamic_cast`

```cpp
catch (const std::exception& e) {
    typeid(e).name();                               // "11DecodeError" (the compiler's mangled name)
    dynamic_cast<const DecodeError*>(&e);           // succeeds: returns a pointer
    dynamic_cast<const std::logic_error*>(&e);      // fails: returns nullptr
    dynamic_cast<const std::logic_error&>(e);       // fails: throws std::bad_cast
}
```

(All of the above measured.)

- `dynamic_cast` checks the object's actual type at run time and performs a downcast safely. It relies on **run-time type information** (RTTI) and has its own cost.
- Code that needs lots of `dynamic_cast` usually signals a design problem: it should have used virtual functions.
- Some game engines turn RTTI off with `-fno-rtti` to shrink binaries, and then neither `dynamic_cast` nor `typeid` is available.
- A downcast with `static_cast` is **unchecked**; if the type is wrong, it's undefined behavior.

## 10. Polymorphism without virtual functions

| Approach | When the implementation is chosen | In the project |
|---|---|---|
| Virtual functions | At run time (vtable lookup) | Exception classes |
| `enum` + `switch` | At run time (branches) | `EffectKind`, `TargetRule`, `Trigger` |
| `std::variant` + `std::visit` | At run time (jump by index) | `term::Value` (lesson 9) |
| Templates / CRTP | At compile time | `checked_int<T>` (lesson 15) |

CRTP is covered in lesson 15.

## Interview questions

**Q1: How are virtual functions implemented? What do they cost?**
Each class with virtual functions has a vtable (an array of function pointers), and each object starts with a vptr pointing to it. A call reads the vptr, fetches the function address at an index fixed at compile time, and calls it indirectly. Costs: 8 extra bytes per object, an extra memory read and an indirect jump, and usually no inlining.

**Q2: Why should a base-class destructor be virtual?**
When you `delete` a subclass object through a base pointer and the destructor isn't virtual, only the base destructor runs and the subclass part is never destroyed. That's undefined behavior (measured: the subclass's members leak). Whenever a class may be deleted polymorphically, its destructor must be virtual.

**Q3: Can a constructor be virtual? What happens if a constructor calls a virtual function?**
A constructor can't be virtual: creating an object requires knowing its type exactly, and the vptr is set during construction anyway. A virtual call inside a constructor reaches the version of the layer currently being constructed, never the subclass.

**Q4: What are `override` and `final` for?**
`override` has the compiler check that the function really overrides a base virtual function, so a wrong signature fails to compile; without it the function silently becomes a new one. `final` forbids further overriding or inheritance, and gives the compiler a chance to devirtualize.

**Q5: What is object slicing?**
When a subclass object is assigned by value to a base object, only the base part is copied; the subclass's data and virtual behavior are lost. Polymorphism must go through pointers or references. It's also why exceptions should be caught by reference.

**Q6: What's the problem with diamond inheritance? How do you solve it?**
The bottom class contains two copies of the top base class's data, and accessing it is ambiguous. Virtual inheritance keeps only one copy, at the cost of a bigger object and an extra indirection. In practice, composition is preferred over multiple inheritance.

**Q7: What's the difference between `dynamic_cast` and `static_cast`?**
`dynamic_cast` checks the type at run time; on failure it returns `nullptr` for pointers and throws `std::bad_cast` for references, and it requires RTTI and a polymorphic type. `static_cast` checks only at compile time; a downcast has no run-time check, and a wrong type is undefined behavior.

**Q8: Why does your project use `enum` + `switch` instead of virtual functions?**
Effect types are tied to the config file format and rarely change, while there are many operations on effects (parsing, validation, execution, encoding); the data is loaded from config tables and sent over ETF, and an `enum` plus plain structs serializes naturally, with objects stored contiguously. If a `switch` misses a new type, the compiler warns. Measured, virtual dispatch is about 45% slower, but that isn't the main reason.

Next: [Lesson 13: Special member functions, value categories and move semantics](13-move-semantics.md)
