# Loxis Roadmap

## ✅ Done

### Compiler (v2)
- [x] Lexer — tokenizer with ~80 token kinds, string/char/number literals, comments, significant newlines
- [x] Parser — recursive-descent, full expression precedence, patterns, types, generics
- [x] AST — rich node types: 30+ expressions, 8 patterns, 9 types, 8 items
- [x] Scope builder — two-pass declaration then resolution
- [x] Type checker — full expression/statement checking, subtype relation, method/field lookup
- [x] MIR lowering — AST → MIR (MirBody, BasicBlock, Operand, Rvalue)
- [x] MIR compiler — MIR → bytecode
- [x] VM — register-based (256 int64 regs), JIT instruction decode, call stack, heap (9-byte cells)

### Language Features (v2)
- [x] Functions (`fun`) — expression body shorthand, return type annotation
- [x] Classes — fields, methods, inheritance (single superclass), interfaces (multiple), `open`/`abstract`/`override`/`data`
- [x] Interfaces — method declarations with optional default bodies
- [x] Objects — singleton with fields, methods, init blocks
- [x] Enum classes — variants (tuple-style and struct-style), properties, methods
- [x] Variables — `val` (immutable), `var` (mutable), `const` (compile-time)
- [x] Patterns — wild, bind, literal, tuple, struct, enum, ref, rest
- [x] Generics — type and const params, where bounds
- [x] Nullable types — `T?`, safe calls (`?.`), elvis (`?:`), force unwrap (`!!`)
- [x] Control flow — `if/else`, `when` (pattern matching), `while`, `for..in`, `loop`, `break`/`continue` with labels
- [x] Closures / lambdas
- [x] String templates (`"Hello, $name"`, `"${expr}"`)
- [x] Extension functions, companion objects, `init` blocks
- [x] Imports — `import path::Module`, `import { Item, Item as Alias }`
- [x] Packages — `package path::to`
- [x] Visibility — `public`/`internal`/`private`
- [x] Type casting — `as`, `is`, `!is`
- [x] Primitive types — `int`, `long`, `float`, `double`, `bool`, `char`, `str`, `unit`, `void`, `noreturn`

### Tooling
- [x] **LSP (TypeScript)** — `D:\Development\Loxis-LSP` — language server with:
  - Lexer + parser ported from v2 compiler
  - Diagnostics (parse errors, bracket matching)
  - Completions (keywords, builtins, user symbols)
  - Hover (markdown docs for keywords, types, functions)
  - Document symbols (hierarchical)
  - Go-to-definition (same-file)
  - VS Code extension with syntax highlighting

---

## 🚧 In Progress

### Tooling
- [ ] LSP — add source positions to AST nodes for precise ranges
- [ ] LSP — cross-file go-to-definition
- [ ] LSP — find references
- [ ] LSP — rename
- [ ] LSP — signature help
- [ ] LSP — code actions
- [ ] LSP — formatting

---

## 📋 Planned

### Compiler
- [ ] Standard library (`std::io`, `std::collections`, etc.)
- [ ] Module system — multi-file compilation
- [ ] Incremental compilation
- [ ] Optimization passes (MIR level)
- [ ] Debug info / source maps
- [ ] WASM backend
- [ ] Self-hosting (Loxis compiler in Loxis)

### Language
- [ ] Operator overloading
- [ ] Async/await
- [ ] Coroutines
- [ ] Macros / metaprogramming
- [ ] Type aliases (`type MyInt = int`)
- [ ] Trait/typeclass system (replace `where` bounds with proper traits)
- [ ] Exhaustiveness checking for `when` expressions
- [ ] Smart casts after `is` checks

### Tooling
- [ ] VS Code extension — publish to marketplace
- [ ] Tree-sitter grammar
- [ ] REPL
- [ ] Package manager (`loxpm`)
- [ ] Documentation generator
- [ ] Test framework
- [ ] Debugger (DAP)
- [ ] Formatter (`loxfmt`)
