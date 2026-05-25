# Loxis Language Specification v2.0

## Overview
Loxis is a systems programming language with C++-like performance and Rust-like type safety.

## Lexical Structure
- **Identifiers**: `[A-Za-z_][A-Za-z0-9_]*`
- **Keywords**: `mod use fn let mut struct enum trait impl for while if else match return break continue loop self super extern unsafe const static as where type ref box pub priv pub(crate)`
- **Primitives**: `i8 i16 i32 i64 isize u8 u16 u32 u64 usize f32 f64 bool char str String ()`
- **Literals**: integers (`0x` hex, `0b` bin, `0o` oct, `_` separators), floats, strings, chars
- **Comments**: `// line`, `/* block */`
- **Significant tokens**: `;` separates statements. Newlines are whitespace.

## Types
### Primitive
`i8 i16 i32 i64 isize u8 u16 u32 u64 usize f32 f64 bool char str String () !`

### Composite
- Tuple: `(T, U, V)`
- Array: `[T; N]`
- Slice: `&[T]` or `&mut [T]`
- Reference: `&T`, `&mut T`
- Raw pointer: `*const T`, `*mut T`
- Function: `fn(T) -> U`
- Never: `!`

### User-defined
- `struct Point<T> { x: T, y: T }`
- `enum Option<T> { Some(T), None }`
- `trait Drawable { fn draw(&self); }`
- `impl<T> Drawable for Point<T> { ... }`

## Items
```
mod math;

pub fn add<T>(a: T, b: T) -> T where T: Add {
    a + b
}

pub struct Vec<T> {
    ptr: *mut T,
    len: usize,
    cap: usize,
}
```

## Expressions
- Literals, paths, blocks `{ stmts; expr }`
- Operators: full precedence table matching Rust
- Control flow: `if`, `while`, `for`, `loop`, `match`, `break`, `continue`, `return`
- Closures: `|x: i32| -> i32 { x * 2 }`
- Try: `expr?`
- Cast: `expr as Type`

## Patterns
- `_` wildcard
- `x` / `mut x` binding
- `Variant(pat)` enum/struct
- `&pat` reference
- `(a, b)` tuple
- `S { a, b }` struct
- `..` rest

## Memory Model
- `let` = immutable binding
- `let mut` = mutable binding
- `&T` = shared reference
- `&mut T` = exclusive reference
- `*const T` / `*mut T` = raw pointers (unsafe dereference)
- `Box<T>` = heap allocation

## Error Handling
- `Result<T, E>` and `Option<T>`
- `?` operator for propagation
- `panic!("msg")` for unrecoverable errors

## Modules
- Planned: file = module
- Planned: `mod foo;` loads `foo.lxs`
- Planned: `use path::Item;` imports
- Visibility: `pub`, `pub(crate)`, `priv` (default)

## Standard Library
Not implemented yet. A standard library depends on the module loader and import resolver, so it should be added after `mod` and `use` are wired end-to-end.

Planned modules:
- `std::core`: traits (Clone, Copy, Default, PartialEq, PartialOrd, Add, Sub, ...)
- `std::option`: Option<T>
- `std::result`: Result<T, E>
- `std::vec`: Vec<T>
- `std::string`: String
- `std::io`: print, println
