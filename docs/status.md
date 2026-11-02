# Loxis 0.1 stability contract

Loxis is currently **0.1.0-alpha.1**. The alpha is suitable for language
experiments and small command-line programs, but it does not promise source or
binary compatibility between alpha releases.

This document is the release boundary. The broader language specification also
contains designs that are not executable yet. A construct is supported only
when it is listed as stable or experimental here and covered by an end-to-end
test.

## Stable for the 0.1 series

These features are expected to keep their source-level meaning throughout the
0.1 series. Fixes may make previously rejected programs compile, but should not
silently change the result of valid programs.

- Primitive values, local `val`/`var`, functions and structured control flow.
- Classes, constructors, fields, ordinary methods and interfaces for the
  combinations exercised by the smoke suite.
- Generic functions and classes with call-site inference and monomorphization.
- Function values, non-capturing lambdas and capturing closures.
- Nullable values, `?.`, `?:`, `!!`, `is` and `!is` smart casts.
- Enum tuple variants and `when` matching with an explicit fallback when the
  compiler cannot prove exhaustiveness.
- Typed exceptions with ordered catches and `finally`.
- File modules, imports, visibility and the `loxis.*` standard-library namespace.
- Named arguments, `*args`, `**kwargs`, array/map spreading and trailing lambdas.
- The public collection, text, JSON, file, path, system, time and random APIs
  documented in [stdlib.md](stdlib.md).
- `Loxis run`, `Loxis check`, `Loxis version`, and direct `Loxis file.lx`
  compatibility invocation.

## Experimental

These features execute in covered cases, but their syntax, ABI or edge-case
semantics may change before 0.2.

- Operator overloading.
- Top-level `val`, `var` and `const` initialization.
- Virtual/interface dispatch outside the combinations in the regression suite.
- TCP sockets in `loxis.net`.
- NoGC, ARC and AARC selection and GC tuning flags.
- Direct use of `loxis.unsafe`; it is explicitly outside compatibility promises.

## Unsupported or reserved

The parser may recognize some of these constructs so diagnostics and tooling can
understand future syntax. Recognition is not an execution guarantee.

- Singleton and companion-object execution outside existing standard-library
  objects.
- Generated data-class members.
- User-defined extension-function lowering.
- Struct-style and nested enum patterns, and complete exhaustiveness analysis.
- Default arguments, async/await, coroutines and macros.
- Stable native/bytecode ABI, separate compilation and third-party packages.
- WASM, self-hosting, debugger and REPL workflows.

Unsupported constructs should fail during compilation with a diagnostic. A
successful compile followed by a backend failure is a compiler bug.

## Alpha release gates

An alpha release is eligible only when all of the following are true:

1. Debug and Release builds succeed on Windows.
2. Every registered CTest passes in both configurations.
3. Every checked-in example compiles through bytecode generation.
4. `hello_world`, the call-arguments demo, trailing-lambda demo and kitchen-sink
   example execute successfully in Release mode.
5. `git diff --check` reports no malformed patch whitespace.
6. The source tree is clean and the release commit has an annotated version tag.

The test gates establish coverage, not production readiness. Performance,
untrusted-code isolation and long-running service reliability are not yet release
claims.
