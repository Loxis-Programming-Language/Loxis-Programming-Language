# Loxis Language Specification v2.1

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

### Extension functions
```
fun str.isLong() -> bool = length > 10
```

## Expressions

- Literals, paths, blocks `{ stmts... expr }`
- String templates: `"Hello, $name"`, `"${expr}"`
- Operators: arithmetic, comparison, logical, elvis (`?:`), safe call (`?.`), force unwrap (`!!`)
- Control flow: `if`, `when`, `while`, `for`, `loop`, `break`, `continue`, `return`
- Closures: `{ x: int -> x * 2 }`
- Cast: `expr as Type`
- Type check: `expr is Type`, `expr !is Type`
- Try: `expr?` (error propagation)

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
loxis.collections  — List<T>, MutableList<T>, Map<K,V>, Set<T>
loxis.io           — println, print, readln
loxis.math         — math utilities
loxis.text         — string utilities
```

### Prelude (auto-imported)
`loxis` package is auto-imported into every file:
- `int`, `long`, `float`, `double`, `bool`, `char`, `str`
- `unit`, `noreturn`
- `Array<T>`, `List<T>`, `MutableList<T>`
- `println`, `print`
- `fun require(condition: bool, message: str = "")`
- `fun TODO() -> noreturn`
- `fun error(message: str) -> noreturn`

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

| Phase | Component | Status | Notes |
|-------|-----------|--------|-------|
| 1 | AST / Token / Type | ✅ done | Class, Interface, Enum, Object, When, Nullable types |
| 2 | Lexer + Parser | ✅ done | Full Kotlin-style syntax: class/interface/object/when/nullable |
| 3 | Scope & Name Resolution | ✅ done | Class scope, this-binding, field offsets, vtable, inheritance, interface conformance |
| 4 | TypeChecker | 🔶 stubbed | Basic types/expr work. Needs: class method dispatch, nullable/smart cast, interface resolution |
| 5 | Lowering (AST→MIR) | 🔶 stubbed | Basic expr lower. Needs: constructor, virtual dispatch, interface dispatch, null checks |
| 6 | Backend (MIR→Bytecode) | 🔶 partial | Cmp→boolean fix, Print support. Needs: VCall/ICall/NullChk opcodes, vtable init |
| 7 | VM Runtime | 🔶 partial | Existing VM works. Needs: TAG_NULL, indirect call, vtable storage |
| 8 | Module System | ⬜ todo | Multi-file compilation, prelude auto-import, ModuleLoader |
| 9 | Standard Library | ⬜ todo | loxis.* .lx files: Any, collections, io, math, text |
