# Loxis Roadmap

## ✅ Done — Compiler Pipeline (v2)

- [x] **Lexer** — ~80 token kinds, string/char/number literals, comments, Kotlin-style significant newlines (`src/cpp/v2/frontend/Lexer.cpp`)
- [x] **Parser** — recursive-descent, full expression precedence, patterns, types, generics, imports (`src/cpp/v2/frontend/Parser.cpp`)
- [x] **AST** — 30+ expression variants, 8 pattern variants, 9 type variants, 8 item variants (`src/cpp/v2/AST.hpp`)
- [x] **Scope builder** — two-pass: declaration then resolution; class/interface/object/enum scopes; vtable + itable construction (`src/cpp/v2/type/Scope.cpp`)
- [x] **Type checker** — full expression/statement typing, subtype relation, method/field lookup, inference variables, numeric promotion, smart-cast refinements (`src/cpp/v2/type/TypeChecker.cpp`)
- [x] **HIR builder** — AST → typed HIR with resolved generics, closure lowering, enum-ctor lowering, for-desugaring (`src/cpp/v2/hir/HirBuilder.cpp`)
- [x] **Monomorphizer** — generic instantiation scan + substitution pass; name mangling (`src/cpp/v2/hir/Monomorphizer.cpp`)
- [x] **MIR lowering** — HIR → CFG (MirBody, BasicBlock, Operand, Rvalue, Terminator); loop context stack, field/store dispatch (`src/cpp/v2/backend/Lowering.cpp`)
- [x] **MIR compiler** — MIR → bytecode; patch table for label resolution, vtable/itable metadata (`src/cpp/v2/backend/MIRCompiler.cpp`)
- [x] **VM** — register-based (4096 int64 regs + type tags), JIT instruction decode, call stack, data stack, heap (9-byte cells, type-id header), syscall dispatch (`src/cpp/vm/VM.cpp`, `src/cpp/vm/Heap.hpp`)
- [x] **Module loader** — recursive import resolution, stdlib discovery, visibility filtering, selective import enforcement (`src/cpp/v2/driver/ModuleLoader.cpp`)

## ✅ Done — Language Features (executable end-to-end)

### Functions & Control Flow
- [x] Top-level functions — block bodies and expression-body shorthand
- [x] Local `val` / `var` bindings — with optional type annotations
- [x] Top-level immutable `val` declarations — lowered to synthetic getter functions
- [x] Function values and canonical-signature function overloading with structural name mangling
- [x] Operator overloading baseline — binary `+`, unary operators, and equality
- [x] Integer arithmetic, comparisons, recursion
- [x] `if` / `else`, `while`, `loop`, `break` / `continue` with labels
- [x] `for .. in` over integer ranges and iterable classes

### Types & Generics
- [x] Primitive types — `int`, `long`, `float`, `double`, `bool`, `char`, `str`, `unit`, `noreturn`
- [x] Generic functions — monomorphized at HIR level
- [x] Generic classes — `list<T>` with monomorphized methods
- [x] Type aliases — including aliases for function types
- [x] Type casting — `as`, `is`, `!is` with runtime type-id checks

### Classes
- [x] Class construction with heap allocation and field initialization
- [x] Field access and mutation (heap store/load)
- [x] Methods with `this`/`self` binding
- [x] Constructor parameters as properties (`val`/`var` in primary ctor)
- [x] Virtual dispatch (`VCall`) and interface dispatch (`ICall`) via vtable/itable
- [x] Interface default methods and overrides through interface dispatch
- [x] Singleton objects with persistent fields and method dispatch
- [x] User-defined extension functions
- [x] Data classes with generated component/copy/equality/hash/text members

### Closures
- [x] Non-capturing lambdas — v2 `{ arg: Type -> expression }` syntax, lowered as synthetic top-level functions
- [x] **Capturing closures** — heap-allocated environment objects; captured locals stored in env slots; closure call loads env pointer into r1 before indirect dispatch

### Enums & Pattern Matching
- [x] Enum class declaration — variants with optional tuple payloads
- [x] Enum construction — `Enum::Variant(args)` produces tagged heap object (discriminant at slot 0, payload fields from slot 9)
- [x] `when (scrut)` — discriminant-based branching with payload binding for tuple-payload variants

### Nullable Types
- [x] Nullable type syntax — `T?` parsed and type-checked
- [x] `null` literal — typed as `TyNull` throughout HIR
- [x] Elvis operator (`?:`) — null-check → fallback block lowering
- [x] Force unwrap (`!!`) — null-check → panic block lowering
- [x] Safe call / safe field (`?.`) — null-check → nullable-result block lowering
- [x] Resolved nullable HIR types — no more placeholder `mkInt()` for nullable nodes
- [x] Smart casts after null, `is`, and `!is` checks

### Modules & Visibility
- [x] `import path::Module` — recursive dependency resolution
- [x] `from Module import { name, name as alias }` — selective symbol exposure
- [x] `private` visibility — items not exposed across module boundaries
- [x] `public` / `internal` visibility — exposed to importers
- [x] Stdlib discovery — `LOXIS_STDLIB` env var or ancestor-directory search
- [x] Re-export chains — selective and full imports propagate public symbols
- [x] Multi-file package resolution — dotted paths and directory index files

### Standard Library (`stdlib/loxis/`)
- [x] `prelude.lx` — VM primitive wrappers (heap, string, syscall, float)
- [x] `collections/list.lx` — fluent `list<T>` collection, HOF, search, ordering, slicing, and reversal methods
- [x] `collections/stream.lx` — fluent lazy `Stream<T>` pipelines with generic `map`, `zip`, and `fold`
- [x] `text.lx` — string extensions for splitting, words, replacement, parsing, Unicode, slicing, and search
- [x] `math.lx` — organized `Math` integer and floating-point helpers
- [x] `fileio.lx`, `fs.lx`, `path.lx` — `File`, `Files`, and `Path` host APIs
- [x] `functional.lx` — `apply`, `twice` (works with capturing closures)
- [x] `option.lx` — `Option` factories and fluent `Optional<T>` mapping, defaults, and nullable conversions
- [x] `result.lx` — `Result<T,E>` variants plus fluent mapping, defaults, and Optional conversions

### VM Builtins
- [x] Console: `print`, `println`, `write`, `error`
- [x] Heap: `alloc`/`halloc`, `hload`/`hread`, `hstore`/`hset`, `hfree`
- [x] String: `strlen`, `s2i`, `i2s`, `strchar`, `strslice`, `strindexof`
- [x] Float: `itof`, `ftoi`, `fadd`, `fsub`, `fmul`, `fdiv`, `fneg`
- [x] Syscall: generic `syscall(num, args…)`, file-I/O syscalls (40–43)
- [x] Indirect call: `CallInd` with closure-object dispatch (env in r1)
- [x] Virtual/interface call: `VCall`, `ICall` with vtable/itable resolution

### Testing & Validation
- [x] Smoke test harness — `tests/run_smoke.ps1` with 39 cases
- [x] CTest integration — `loxis_smoke` and `loxis_stdlib` targets

## ✅ Done — Tooling

- [x] **LSP** (`D:\Development\Loxis-LSP`) — TypeScript port of v2 lexer/parser:
  - Diagnostics, completions, hover, document symbols, go-to-definition
  - VS Code extension with `.lx` syntax highlighting

---

## 🚧 In Progress / Partial

### Language Features (parse + type-check done, execution partial)
- [ ] **Enum classes — full coverage** — struct-style variants, nested patterns, exhaustiveness checking not yet supported at runtime
- [ ] **Top-level mutable `var` and compile-time `const` semantics** — immutable `val` getters execute; mutation and constant evaluation need full coverage
- [ ] **Companion objects** — parsed; not scoped for execution
- [ ] **String templates** — parsed and lowered (concat chain); edge cases not exhaustively tested

---

## 📋 Planned

### Compiler
- [ ] Optimization passes (MIR level)
- [ ] Debug info / source maps
- [ ] WASM backend
- [ ] Incremental compilation
- [ ] Self-hosting (Loxis compiler in Loxis)

### Language
- [ ] Async/await
- [ ] Coroutines
- [ ] Macros / metaprogramming
- [ ] Trait/typeclass system (replace `where` bounds with proper traits)
- [ ] Full exhaustiveness checking for `when` expressions
- [ ] Full enum pattern coverage (struct variants, nested patterns)

### Standard Library
- [x] `HashMap<K, V>` / `HashSet<T>` and `Queue<T>`
- [x] Optional-based `Iterator<T>`, lazy adapters, streams and iterator-driven `for`
- [x] JSON parsing/serialization and file-processing example
- [x] Filesystem/path/binary I/O, arguments/environment, clocks and seeded random
- [x] Strict numeric parsing, Unicode helpers and double math
- [x] TCP client/server sockets (`loxis.net`); HTTP remains planned

### Tooling
- [ ] VS Code extension — publish to marketplace
- [ ] Tree-sitter grammar
- [ ] REPL
- [ ] Package manager (`loxpm`)
- [ ] Documentation generator
- [ ] Test framework
- [ ] Debugger (DAP)
- [ ] Formatter (`loxfmt`)

### LSP
- [ ] Source positions on AST nodes for precise ranges
- [ ] Cross-file go-to-definition
- [ ] Find references
- [ ] Rename
- [ ] Signature help
- [ ] Code actions
- [ ] Formatting
