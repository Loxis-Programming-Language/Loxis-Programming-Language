# Loxis Executable Subset

This document names the language surface that is expected to work end-to-end
today, from parse through VM execution. It is intentionally narrower than the
full v2 spec.

## In The Smoke Baseline

- Top-level functions with block bodies or expression bodies.
- Return types written as either `-> T` or `: T`.
- Local `val` and `var` bindings with optional annotations.
- Integer arithmetic, comparisons, recursion, `if`/`else`, `while`, and basic
  `for .. in` over integer ranges.
- Direct function calls, `print`, simple local imports, and generic function
  monomorphization for the covered cases.
- Prelude functions written in Loxis can wrap VM primitives such as heap,
  string, syscall, and float conversion helpers.
- Generic classes and methods are baseline-covered for simple standard-library
  containers such as `loxis.collections.list<T>`.
- `for .. in` works over iterable classes that provide `size()` and `get(index)`,
  including generic standard-library containers.
- Standard-library `list<T>` supports fluent higher-order methods including
  `map`, `filter`, `forEach`, `fold`, `count`, `any`, and `all`.
  These accept **capturing** lambdas that
  reference outer local variables through heap-allocated closure
  environments. Non-capturing lambdas continue to work as before.
  Standard-library helpers also include algorithm helpers such as
  `listAppend`, `listReverse`, `listReversed`, and `listSortBy`.
- Integer math helpers and text helpers can be authored directly in
  standard-library modules, including length, concatenation, integer parsing,
  integer formatting, character-code lookup, slicing, substring search, and
  containment checks.
- `loxis.fileio` provides text-file read, write, append, and remove wrappers
  over VM syscalls.
- Top-level functions can be used as function values, stored in locals, passed
  through generic standard-library helpers, and invoked through indirect calls.
- Non-capturing lambda expressions can be passed to function-typed parameters,
  stored in function-typed locals, lowered as synthetic functions, and invoked
  through indirect calls. Lambda literals use the v2 form
  `{ arg: Type -> expression }`; parameter types may be inferred from context.
- `loxis.functional` provides baseline high-order helpers such as `apply` and
  `twice` for non-capturing and capturing function values.
- Enum classes can be constructed and discriminated at runtime.
  `Enum::Variant(args)` constructs a tagged heap object; `when (scrut) { … }`
  performs discriminant-based branching with payload binding.
- Nullable types have stable runtime semantics: `null` literal, `?:` (elvis),
  `!!` (force-unwrap), and safe-call/safe-field access (`?.`) with resolved
  nullable HIR types.
- Module visibility is enforced: `private` items are not leaked across
  module boundaries, and `from Lib import { name }` only exposes the
  explicitly named symbols. Transitive re-export chains work: public
  symbols from a dependency's own imports are visible to downstream
  importers. Multi-file package resolution supports dotted paths
  (`pkg.sub.Mod` → `pkg/sub/Mod.lx`) and directory index files
  (`pkg/mod.lx`, `pkg/package.lx`, `pkg/index.lx`).
- Type aliases — `type Name = Type` works in type annotations and function
  signatures; aliases resolve through scope to the underlying type.
- Top-level `val`/`var`/`const` — lowered to synthetic getter functions;
  references compile to calls against the getter.
- Smart casts — `if x != null { … }` refines `x` to non-nullable in the
  then-branch; `if x is T { … }` / `if x !is T { … }` refine based on
  runtime type-id checks.
- Typed exceptions execute end-to-end. `throw` accepts `Exception` subclasses;
  ordered `catch` clauses use subtype matching; `finally` runs through normal
  completion and abrupt control flow; VM unwind restores caller registers and
  GC roots. Uncaught exceptions include a Python-style traceback with source
  file, line, function, original statement, caret marker, dynamic type, and
  message.
- Parsing and type-checking class/interface/object/enum declarations used by
  `demo.lx`, even where later execution support is still partial.

The regression entry point is:

```powershell
.\tests\run_smoke.ps1 -Exe .\build\Debug\Loxis.exe
```

CTest also exposes the same baseline as `loxis_smoke`.

## Standard-library regression coverage

`tests/run_stdlib.py --exe build-stdlib/Loxis.exe` (also CTest's `loxis_stdlib`)
checks list bounds and reference cleanup, Optional/Result helpers, strict numeric
parsing, Unicode code points, double math, hash maps/sets, queues, lazy iterators,
filesystem and binary I/O, stdin, arguments/environment, clocks, seeded random,
and JSON parsing/serialization. It includes a complete JSON file-processing example
and runs each case with NoGC, ARC and AARC. Successful ARC/AARC unit programs
also check zero live heap objects and zero invalid handle operations at shutdown.

Iterator-driven `for` accepts `next() -> Optional<T>` and supports nullable
payloads, `break`, and `continue`. Generic callback fields and interface dispatch
are exercised by these tests. The VM supports 4096 tagged registers using wide
register operands; programs exceeding the compiler's local limit fail explicitly.

See [the API contracts and compatibility notes](stdlib.md). This coverage does
not imply support for all untested combinations of language features below.

## Not Yet Baseline

- Full class construction and field/method behavior across the old `Point_new`
  style tests.
- Interface and object execution.
- Full `when` exhaustiveness checks and complete pattern coverage (the current
  `when` supports enum-variant discriminant branching with tuple-payload binding;
  struct-variant patterns and nested patterns are not yet supported).
- Complete operator-overloading coverage beyond the currently tested binary
  `+`, unary, and equality operators; async/await, coroutines, macros, and the
  trait/typeclass system.

## Generic argument inference

Calls can omit all generic arguments when argument types determine them:
`identity(42)`, `Box(42)`, and `xs.map({ x -> x * 2 })`.
Inference matches nested generic types and function signatures, checks lambda
inputs using the other arguments, and infers lambda result types. Each call and
overload candidate has independent bindings; inferred arguments are retained for
monomorphization.

A declared result type supplies missing information, for example
`val xs: list<int> = list()` and `val value: Optional<int> = optionalEmpty()`.
Return expressions, branch results and concrete function parameter types also
provide this context. A call without enough information, such as `list()`
without a result annotation, needs explicit type arguments. Conflicting bindings
are rejected; inference does not choose a common supertype for unrelated inputs.
Bare `null` alone cannot determine a type parameter. Partial explicit argument
lists and whole-program inference from later assignments are not supported.
