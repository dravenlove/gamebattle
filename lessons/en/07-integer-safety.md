# Lesson 7: Integer safety

[中文](../07-integer-safety.md) | **English**

> Files: `src/battle_state.cpp:21-43` (`saturating_multiply`), `:321-336` (`saturating_add`, `scale`)

These three small functions are only a few dozen lines, yet they are the foundation of the whole damage calculation.

## 1. Why an Erlang programmer needs a whole lesson on this

Erlang integers are **bignums**: however large they get, they never overflow; they just use more memory. C++'s `std::int64_t` tops out at `9223372036854775807` (about 9.2×10¹⁸), and **signed integer overflow is undefined behavior (UB)**. The experiment below shows how scary "undefined" is.

## 2. The most counterintuitive experiment: compute-then-check gets deleted by the compiler

```cpp
bool overflowed_after(std::int64_t hp, std::int64_t heal) {
    std::int64_t result = hp + heal;
    return heal > 0 && result < hp;       // adding a positive number made it smaller → overflow
}
```

Passing `hp = int64 max`, `heal = 10` (measured):

```
-O0: overflowed_after = 1      ← no optimization: detected
-O2: overflowed_after = 0      ← with optimization: not detected!
```

The assembly generated at `-O2`:

```
_Z16overflowed_afterll:
	xorl	%eax, %eax        ← set the return value to 0 (false)
	ret
```

**The whole check was deleted; the function always returns false.** The standard says signed overflow is UB, so the compiler **is entitled to assume it never happens**; under that assumption "adding a positive number makes it smaller" can never be true, so the condition is always false.

This is why everything passes in Debug (`-O0`) testing and only breaks after shipping a Release (`-O2`) build.

**Conclusion: overflow checks must happen before the computation, making sure the operation can't overflow at all. Checking afterwards is already too late.**

UBSan catches it at run time:

```bash
g++ -std=c++20 -fsanitize=undefined ...
# runtime error: signed integer overflow: 9223372036854775807 + 10 cannot be represented in type 'long int'
```

During development, turn on `-fsanitize=address,undefined` together (lesson 11).

## 3. `saturating_add`: check before adding

```cpp
std::int64_t saturating_add(std::int64_t left, std::int64_t right) {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();   // would exceed the max → return the max
    }
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) {
        return std::numeric_limits<std::int64_t>::min();   // would go below the min → return the min
    }
    return left + right;                                   // only add once it's known to be safe
}
```

"`left + right > max`" is rewritten as "`left > max - right`": when `right > 0`, `max - right` can't overflow; when `right < 0`, neither can `min - right`. **The check itself must not overflow either**; that's the core trick for writing functions like this.

"Saturating" means that out-of-range results stop at the boundary instead of wrapping around. Measured: `saturating_add(max, 1) = 9223372036854775807`.

## 4. `saturating_multiply`: predict the multiplication with a division

```cpp
if (left == 0 || right == 0) return 0;          // rule out 0 first so the divisions below are safe
if (left > 0) {
    if (right > 0 && left > maximum / right) return maximum;   // positive × positive > max ?
    if (right < 0 && right < minimum / left) return minimum;   // positive × negative < min ?
} else {
    if (right > 0 && left < minimum / right) return minimum;   // negative × positive < min ?
    if (right < 0 && left < maximum / right) return maximum;   // negative × negative > max ?
}
return left * right;
```

"`a × b > max`" is rewritten as "`a > max / b`". The sign of the product depends on the signs of both numbers, so there are four cases. Measured: `saturating_multiply(4e18, 3) = 9223372036854775807`.

GCC/Clang have `__builtin_mul_overflow`, and C++26 brings `std::add_sat` / `std::mul_sat`. The project writes its own so that MSVC and GCC behave exactly the same.

## 5. `scale`: basis-point multiplication, split up to avoid overflow

```
result = value × bp / 10000
```

Written directly as `value * bp / 10000`, the intermediate `value * bp` may overflow even when the final result is small. The project's version:

```cpp
std::int64_t scale(std::int64_t value, std::int64_t basis_points) {
    return saturating_add(
        saturating_multiply(value / kBasisPoints, basis_points),                   // the whole-10000s part
        saturating_multiply(value % kBasisPoints, basis_points) / kBasisPoints);   // the remainder part
}
```

The math:

```
value = q × 10000 + r         (q = value / 10000, r = value % 10000)
value × bp / 10000 = q × bp  +  r × bp / 10000
```

The remainder `r` is always below 10000; the whole-10000s part **divides first, then multiplies**. Measured with attack 1e15 and a 200% ratio:

```
naive attack*bp/10000 (two's-complement wraparound) = 155325592629044      ← completely wrong
scale(attack, bp)                                   = 2000000000000000     ← correct: 2e15
```

### Rounding in integer division

```
scale(12345, 15000)  = 18517    exact value 18517.5
scale(-15001, 5000)  = -7500    exact value -7500.5
-7 / 2 = -3     -7 % 2 = -1
```

C++ integer division **truncates toward zero**, and `%` takes the sign of the dividend. That matches Erlang's `div` / `rem` (`-7 div 2` is `-3`, `-7 rem 2` is `-1`). To reproduce the same calculation in Erlang, use `div` and `rem`, **not `/`** (which returns a float).

The truncation rule is fixed and identical on every platform, so it doesn't hurt determinism. That's exactly why floating point isn't used.

## 6. Narrowing: clamp first, then cast

```cpp
std::int64_t v = 3'000'000'000LL;
static_cast<std::int32_t>(v)                            // -1294967296   ← 3 billion became negative
static_cast<std::int32_t>(std::clamp<std::int64_t>(     //  2147483647   ← stops at the int32 max
    v, INT32_MIN, INT32_MAX))
```

A bare `static_cast` keeps only the low 32 bits. Throughout the project the pattern is **`std::clamp` into the target range first, then convert**, for example in `set_attribute_value` (`battle_state.cpp:61`) and when scaling `attack_bp` (`effect_system.cpp:84`). `std::clamp<std::int64_t>` names the type explicitly for the same reason as `std::max`: the argument types must match.

## 7. Mixing signed and unsigned

```cpp
std::vector<int> events(5);
std::int32_t max_events = -1;
events.size() >= max_events      // the result is false!
```

For the comparison, `-1` is converted to the unsigned number `18446744073709551615`. `-Wextra` warns `comparison of integer expressions of different signedness`.

How `emit` writes it (`battle_state.cpp:522`):

```cpp
if (result.events.size() >= static_cast<std::size_t>(request.max_events)) {
```

This is safe because `validate_request` has already guaranteed that `max_events` is between 100 and 1000000. The explicit `static_cast` both silences the warning and says "I've checked that this is safe".

## 8. Three lines of defense

| Defense | Where | What it does |
|---|---|---|
| ① Input validation | `validate_request` | HP and attack ≤ 1e12, ratios ≤ 1e6: blocks absurd input |
| ② Saturating arithmetic | `saturating_add`, `saturating_multiply`, `scale` | When stacked buffs exceed expectations, values stop at the boundary instead of becoming UB |
| ③ Narrowing protection | `clamp` first, then `static_cast` | Converting 64-bit to 32-bit never yields garbage |

Also: the single quotes in `1'000'000'000'000LL` are C++14 **digit separators**, ignored by the compiler; the `LL` suffix guarantees the literal is 64-bit, otherwise some platforms would treat it as a 32-bit `int` and overflow.

The transport layer adds one more: Erlang can send integers larger than int64, and `read_big` in `term.cpp` rejects them outright (lesson 9).

## Summary

| Concept | Key point |
|---|---|
| Signed overflow | UB; the compiler assumes it never happens, and compute-then-check code gets **deleted outright** |
| When to check | **Before computing**, and the check itself must not overflow |
| Saturating arithmetic | Out-of-range values stop at the boundary |
| `scale` | Split into "whole 10000s + remainder" so intermediates don't overflow |
| Integer division | Truncates toward zero, matching Erlang's `div` / `rem` |
| Narrowing | `clamp` first, then `static_cast` |
| Mixed signedness | A negative number becomes a huge positive one |
| Tools | `-fsanitize=address,undefined` |

Next: [Lesson 8: Validation and exceptions](08-validation-exceptions.md)
