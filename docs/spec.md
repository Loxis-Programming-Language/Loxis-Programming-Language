# Loxis Language Specification v2

## Overview
Loxis is a statically-typed language with Kotlin-inspired syntax and systems-level performance.

## Lexical Structure
- **Identifiers**: `[A-Za-z_][A-Za-z0-9_]*` (types PascalCase, others camelCase)
- **Keywords**: `package import class abstract open data object enum when for while if else return break continue loop super this extern unsafe const static as where type ref is in`
- **Primitive type names**: `int long float double bool char str unit noreturn null`
- **Literals**: integers (`0x` hex, `0b` bin, `0o` oct, `_` separators), floats, strings (`"..."`, `"""..."""`), chars (`'x'`)
- **Comments**: `// line`, `/* block */`
- **Semicolons**: optional (newline-sensitive, like Kotlin)

## Types

### Primitive
`int long float double bool char str unit noreturn null`

### Composite
- Tuple: `(T, U)`
- Array: `[T; N]`
- Slice: `[T]`
- Reference: `&T`, `&mut T`
- Raw pointer: `*T`, `*mut T` (unsafe)
- Function: `(T) -> U`
- Nullable: `T?`
- Never: `noreturn`

### Nullable Types
- `T?` = `T` or `null`
- `?.` safe call: `obj?.method()` returns `U?`
- `?:` elvis: `x ?: default`
- `!!` force unwrap: `x!!` panics if null
- No `Option<T>` — nullable is built-in

## Classes

### Simple class
```
class Point(val x: int, val y: int)
```

### Full class with body
```
class Counter(init: int) {
    val start: int = init
    var count: int = init

    fun inc() -> int {
        count = count + 1
        return count
    }
}
```

### Constructor
- **Primary constructor**: declared in class header
- `val` = property + getter (immutable)
- `var` = property + getter + setter (mutable)
- Plain param = constructor param only (no property)
- **init block**: initialization code
```
class Foo(val name: str, size: int) {
    val upper = name.toUpper()
    init {
        require(size > 0)
    }
}
```
- **Secondary constructors**: `constructor(...) : this(...)`

### Visibility
- `public` (default) — visible everywhere
- `internal` — visible in same package
- `private` — visible in same class/file

### Inheritance
```
open class Animal(val name: str) {
    open fun speak() -> str = "..."
}

class Dog(name: str, val breed: str) : Animal(name) {
    override fun speak() -> str = "Woof!"
}
```
- `open` — class can be subclassed, method can be overridden
- `abstract class` — cannot be instantiated, may have abstract methods
- `override` — required when overriding
- Single inheritance only

### Interface
```
interface Flyable {
    fun fly() -> str                // abstract method
    fun wings() -> int = 2             // default implementation
}

interface Swimmable {
    fun swim() -> str
}

class Duck(val name: str) : Flyable, Swimmable {
    override fun fly() = "$name flying"
    override fun swim() = "$name swimming"
}
```
- Multiple interfaces can be implemented
- Methods can have default implementations
- No constructors, no backing fields (no state)
- Properties can be declared but must be abstract or computed

### Abstract class
```
abstract class Shape {
    abstract fun area() -> double
    fun describe() -> str = "I'm a shape"  // concrete method
}

class Circle(val radius: double) : Shape() {
    override fun area() -> double = 3.14159 * radius * radius
}
```

### Data class
```
data class User(val id: int, val name: str)
```
Auto-generates: `equals()`, `hashCode()`, `toString()`, `copy()`, component accessors.
Requirements: primary constructor has at least one `val`/`var` param.

### Object (singleton)
```
object Config {
    val version = "1.0"
    fun load() { ... }
}
```

### Companion object
```
class Foo {
    companion object {
        fun create() -> Foo = Foo()
    }
}
```

## Enum class
```
enum class Color { RED, GREEN, BLUE }

enum class Result<T, E> {
    Ok(val value: T),
    Err(val error: E)
}
```

## Functions

### Top-level
```
fun add(a: int, b: int) -> int = a + b
fun greet(name: str) -> str {
    return "Hello, $name"
}
```

Top-level functions may overload by canonical parameter type. Parameter names
and return types do not distinguish overloads:

```
fun show(value: int) -> str = "integer"
fun show(value: str) -> str = value
```

Backend symbols use structural descriptors containing parameter and return
types. A declaration that differs only by return type is rejected.

### Argument collection and expansion

```text
fun checkout(base: int, *prices: int, **extras: int) -> int {
    var result = base
    for price in prices { result += price }
    return result - extras.get("discount").getOrElse(0)
}

checkout(0, 12, 18, discount = 5)
checkout(0, *prices, **extras)
```

`*rest: T` collects extra positional arguments into an array of `T`.
`**options: T` collects extra named arguments into `HashMap<str, T>` and requires
`import loxis.collections.map`. The annotations describe individual elements and
values. Fixed parameters precede `*`; `**` comes last. On calls, `*array` and
`**map` expand values. Named arguments use `name = value`. Source arguments are
evaluated once, from left to right; duplicate bindings are errors. `vararg` and
`kwargs` are ordinary identifiers. See [the executable subset](executable-subset.md#named-arguments-varargs-and-kwargs)
for supported call forms and current restrictions.

### Extension functions
```
fun str.isLong() -> bool = length > 10
```

## Expressions

- Literals, paths, blocks `{ stmts... expr }`
- Array literals: `[1, 2, 3]`
- Map literals: `{"one": 1, "two": 2}` or `{1: "one", 2: "two"}`; keys currently support `str` and `int`
- String templates: every string interpolates, so there is no separate template
  syntax. `$name` interpolates a name and `${expr}` an arbitrary expression,
  e.g. `"Hello, $name"`, `"total ${a + b}"`. A `$` only starts an interpolation
  when `{` or an identifier follows it, so `"cost $5"`, `"100%"` and a lone `$`
  stay literal without escaping; `$$` is an explicit literal `$`. Escape
  sequences (`\n`, `\t`, `\x1b`, ...) resolve as usual, and `${...}` may contain
  nested braces and string literals.
- Operators: arithmetic, comparison, logical, elvis (`?:`), safe call (`?.`), force unwrap (`!!`)
- Control flow: `if`, `when`, `while`, `for`, `loop`, `break`, `continue`, `return`, `throw`, `try/catch/finally`
- Loops: `for pat in iterable`, where the iterable is a range (`0..5`, upper
  bound excluded; `0..=5` inclusive), an array literal, or anything with
  `size()`/`get(i)`. A loop may be labelled with `name:` before the keyword, and
  `break name` / `continue name` then apply to that loop rather than the
  innermost one:
  ```
  outer: for i in 0..3 {
      for j in 0..3 {
          if j == 1 { continue }
          if i == 2 { break outer }
      }
  }
  ```
- Closures: `{ x: int -> x * 2 }`. A closure captures variables from its
  enclosing scopes (including values held by an enclosing closure, which are
  forwarded down) and may assign to captured `var` bindings through its captured
  environment. Function values — both bare functions and closures — are
  first-class and can be called directly or from inside another closure.
- Trailing lambda: a lambda argument may sit outside the call parentheses
  (`xs.fold(0) { acc, x -> acc + x }`), and a single-lambda method call may
  elide the parentheses entirely (`xs.map { x -> x * 2 }`).
- Cast: `expr as Type`
- Type check: `expr is Type`, `expr !is Type`
- Try: `expr?` (error propagation)

## Exceptions

Exceptions are typed objects derived from the auto-imported `Exception` class.
`RuntimeException` is the standard concrete exception, and `error(message)`
throws one.

```lox
try {
    dangerousOperation()
} catch (failure: RuntimeException) {
    print(failure.message)
} catch (failure: Exception) {
    print("other exception: " + failure.message)
} finally {
    cleanup()
}
```

Catch clauses are tested in source order. A catch shadowed by an earlier base
type is a compile error. `finally` runs on normal completion, exception unwind,
`return`, `break`, and `continue`; an abrupt exit from `finally` replaces the
pending exit. Uncaught exceptions terminate with their dynamic type and message,
preceded by a Python-style traceback. Every mapped frame includes its source
file, line number, function name, original source line, and a caret marker.
Runtime failures that valid source programs can cause, including integer
division by zero, strict numeric parsing failures, and checked heap access,
raise catchable `RuntimeException` values through the same unwind path. Invalid
bytecode, impossible operand tags, corrupt handles, and broken VM/GC invariants
instead terminate with an uncatchable `Loxis VM panic` diagnostic.

## `when` Expression
```
when (x) {
    0 -> "zero"
    1, 2 -> "one or two"
    in 3..10 -> "three to ten"
    is str -> "a string"
    else -> "something else"
}
```
Without argument (like if-else chain):
```
when {
    x > 0 -> "positive"
    x < 0 -> "negative"
    else -> "zero"
}
```
When the scrutinee is an enum class and the result is used as a value, the arms
must cover every variant, either by listing them or with an `else`/wildcard arm.
An unhandled variant would otherwise fall off the end and yield whatever the
last matched arm left behind. A `when` whose value is discarded — a bare
statement — is exempt, as in Kotlin.

## Patterns
- `_` wildcard
- `name` binding
- `(a, b)` tuple destructure
- `Type(variant)` enum destructure
- `{ field }` class destructure

## Memory Model
- `val` = immutable binding
- `var` = mutable binding
- `&T` = shared reference
- `&mut T` = exclusive reference
- `*T` / `*mut T` = raw pointers (unsafe)

Assigning to a `val` is a compile error. Immutability applies to the binding,
not the referent: `val a = [1, 2]; a[0] = 3` and `val o = Foo(); o.field = 1` are
allowed, because the array or object the binding points at is itself mutable.

A top-level `var` is a real global. Its initializer runs once at startup into a
VM global slot, and reads and writes go through that slot rather than
re-evaluating the initializer, so a global can hold mutable state:

```
var counter = 0
fun bump() -> unit { counter = counter + 1 }
```


## Packages & Modules

### File = module
- `.lx` = regular module file
- `.lxs` = script file (has entry point, later)

### Package declaration
```
package com.example.mylib
```
If omitted, defaults to the root (unnamed) package.

### Imports
```
import com.example.foo.Bar            // single class
import com.example.foo.Bar as B       // with alias
import com.example.foo.*              // everything from package
```

### Standard library namespace: `loxis`
```
loxis              — core types, auto-imported (int, str, bool, unit, noreturn, Array...)
loxis.collections  — list<T>, HashMap<K,V>, HashSet<T>, Queue<T>
loxis.io           — Console, Lines
loxis.math         — math utilities
loxis.text         — string utilities
```

Standard-library APIs prefer receiver methods and cohesive singleton entry
points over type-prefixed free functions. Generic methods take explicit result
type arguments when inference would be ambiguous, for example
`stream.map<str>({ value: int -> value.toString() })`.

### Prelude (auto-imported)
`loxis` package is auto-imported into every file:
- `int`, `long`, `float`, `double`, `bool`, `char`, `str`
- `unit`, `noreturn`
- Array literals; standard-library collections require explicit imports
- `println`, `print`, `write`
- `fun require(condition: bool, message: str)`
- `fun TODO() -> noreturn`
- `fun error(message: str) -> noreturn`

VM bridge functions and `__loxis_*` helpers are not public prelude APIs.
Low-level experiments must explicitly import `loxis.unsafe` in the calling file.
See [the migration table and API boundary](stdlib.md#public-api-boundary-and-migration).
- `open class Exception(val message: str)`
- `class RuntimeException(message: str) : Exception(message)`

### Directory layout
```
src/
  main.lx
  mylib/
    utils.lx          # package mylib
    internal/
      helpers.lx      # package mylib.internal

stdlib/
  loxis/
    prelude.lx
    collections/
      list.lx
      map.lx
    io/
      console.lx
```

## Error Handling
- `T?` for nullable (replaces `Option<T>`)
- `Result<T, E>` — success or error (enum class)
- `expr?` operator — propagation sugar for `Result`
- `panic("msg")` — unrecoverable

## Standard Library

Written in Loxis (`.lx` files), located in `stdlib/` directory.
The compiler resolves `loxis.*` imports against the stdlib directory.

| Module | Provides |
|--------|----------|
| `loxis` (prelude) | `int`, `long`, `float`, `double`, `bool`, `char`, `str`, `Array`, `List`, `println`, `print`, functions |
| `loxis.collections` | `MutableList<T>`, `Map<K,V>`, `Set<T>`, `MutableSet<T>` |
| `loxis.io` | `readln`, file I/O |
| `loxis.math` | math functions, `Random` |
| `loxis.text` | `StringBuilder`, string utilities |

## Implementation Roadmap

The compiler now has an executable smoke baseline that runs end-to-end through
parsing, typing, lowering, bytecode generation, and the VM. The roadmap below
tracks what is already baseline-covered versus the larger v2 surface still in
progress.

| Phase | Component | Status | Notes |
|-------|-----------|--------|-------|
| 1 | AST / Token / Type surface | ✅ done | Core language surface is modeled, including classes, interfaces, enums, objects, `when`, and nullable types. |
| 2 | Lexer + Parser | ✅ done | Kotlin-style syntax, imports, generics, class/interface/object/enum forms, and nullable syntax are parsed. |
| 3 | Scope & Name Resolution | ✅ done | Declaration + resolution passes handle `this` binding, field offsets, vtables, inheritance, and interface conformance. |
| 4 | TypeChecker | ✅ baseline | The executable subset type-checks end-to-end, including imports, generic smoke cases, and class `this` method access. Remaining work is broader nullable analysis, pattern exhaustiveness, and richer OO dispatch rules. |
| 5 | Lowering (AST→MIR) | ✅ baseline | The current smoke subset lowers successfully through MIR, including closures, captured-variable writes, nested capture forwarding, and indirect/closure calls. Remaining work includes fuller constructor/field behavior and richer nullable/pattern lowering. |
| 6 | Backend (MIR→Bytecode) | ✅ baseline | The current subset emits runnable bytecode for arithmetic, control flow, imports, generics, closures, and stdlib-backed helpers. Remaining work includes fuller object dispatch and missing runtime op coverage. |
| 7 | VM Runtime | ✅ baseline | The VM runs the demo and smoke suite successfully, including first-class function values and closure calls. Remaining work includes full object runtime semantics, nullable edge cases, and remaining dispatch/storage support. |
| 8 | Module System | 🔶 partial | Local multi-file imports work and are smoke-covered. Remaining work is package visibility, alias/re-export behavior, and more robust stdlib module resolution. |
| 9 | Standard Library | 🔶 partial | `loxis` prelude, core wrappers, and `loxis.collections.List<T>` are present and smoke-covered. `io`, `mem`, `ptr`, `refl`, and broader collections/text/math coverage are still incomplete. |
| 10 | Verification | 🔶 manual smoke | `tests/run_smoke.ps1` and the `loxis_smoke` CTest entry cover the executable subset, but there is still no full automated unit/integration test framework. |

### Goal

Expand the current executable subset into a broader, predictable v2 baseline: keep the existing smoke path stable while bringing object construction, function values, richer control-flow typing, and stdlib-backed multi-module programs online end-to-end.

### Plan

1. Complete class construction and field initialization.
   - Restore `Point_new`-style constructor coverage end-to-end.
   - Lower instance field initialization and method access consistently.
   - Close remaining gaps between class parsing/type-checking support and runtime behavior.

2. Add closure and function-value execution.
   - Type-check closure literals and function-valued locals more broadly.
   - Lower callable values and indirect calls through MIR and bytecode.
   - Extend the VM/runtime path so non-direct calls execute correctly.

   Function values and closures are now executable end-to-end: a closure can
   call a function-typed parameter, assign to a captured `var`, and forward
   captures through nested closures. Recursive closures (a closure that names
   itself) are not yet supported.

3. Expand `when` and nullable semantics.
   - Broaden `when` lowering and checking beyond the current smoke cases.
   - Improve nullable flow analysis, unwrap/default behavior, and runtime null handling.
   - Move more of the written spec into the executable baseline instead of parse-only support.

4. Strengthen modules and the standard library.
   - Harden package/import behavior beyond simple local imports.
   - Grow stdlib coverage past the current prelude and `List<T>` smoke path.
   - Add smoke scripts for each newly promoted module/runtime capability.
