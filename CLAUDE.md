# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```powershell
# Configure (one-time)
cmake -B build -G "Visual Studio 17 2022"

# Build the compiler
cmake --build build

# Run a .lx file
.\build\Debug\Loxis.exe demo.lx

# Show what the compiler is doing:
.\build\Debug\Loxis.exe demo.lx --verbose      # stage banners, modules, timings
.\build\Debug\Loxis.exe demo.lx --trace        # + declarations and lookup candidates

# Inspect one pipeline stage (compile-only; the program is not run):
.\build\Debug\Loxis.exe demo.lx --dump-ast
.\build\Debug\Loxis.exe demo.lx --dump=all     # tokens, ast, hir, mir, bytecode, scopes

.\build\Debug\Loxis.exe --help
```

The legacy positional form still works: `Loxis.exe demo.lx 100000 verbose`.

Any `--dump-*` flag makes the compiler compile-only and writes to stdout, so IR text and
program output never interleave. `--help` lists every flag.

Tests: `tests/run_smoke.ps1` (PowerShell), `tests/run_stdlib.py`,
`tests/run_generic_inference.py` and `tests/run_parser_regressions.py` (Python, also
registered with ctest). They run each `.lx` case from a temp directory and diff
stdout/stderr/exit code — note this means a case's expected output must be updated
whenever its `.lx` file changes.

**There are two independent build trees**, and they do not share objects:

| Directory | Toolchain | Built by |
| --- | --- | --- |
| `build/` | MSVC + Visual Studio 17 2022 | the commands above |
| `cmake-build-debug/` | MinGW g++ 13 + Ninja | CLion |

If the CLI behaves as if your changes are missing — most memorably `Fatal error: stoi`
from an unrecognised flag — you are running a stale binary from the other tree. Both
are gitignored; rebuild the one you actually invoke.

## Architecture

Loxis is a custom statically-typed language with a register-based VM, implemented as a classic compiler pipeline.

### Compiler (`src/cpp/v2/`)

All code lives in namespace `loxis::v2`. The pipeline:

```
.lx source → Lexer → Parser → Scope → TypeChecker → Lowering → MIRCompiler → Chunk → VM
```

- **`frontend/Lexer`** — ~80 token kinds in `Tk` enum. Emits `Tk::Newline` tokens, string/char/number literals
- **`frontend/Parser`** — Recursive-descent parser. Produces a `Module` containing `ItemPtr` variants (functions, classes, interfaces, enums, objects, imports). Entry point: `Parser::parseFile(path)`.
  - **The parser is NOT newline-sensitive.** `Parser::Parser` (Parser.cpp ~line 141) drops every `Tk::Newline`, so the lexer's newline tokens never reach the parser and statement boundaries are inferred from tokens that cannot continue an expression. The surviving guard is `Parser::atLineStart()`, which compares `toks[pos].loc.line` against `toks[pos-1].loc.line`; it is used to stop a `(` that opens a new line from continuing the previous expression as a call (Kotlin reads `\n(` the same way). A line starting with `[` or `<` still continues the previous expression.
  - Postfix nodes (`ExprCall`/`ExprMethodCall`/`ExprField`/`ExprSafeCall`/`ExprSafeField`/`ExprIndex`/`ExprForceUnwrap`) are given their own `loc` at construction in `parsePostfix`, pointing at the member token. They are built with none, and `parseExpr` only stamps the outermost node — so without this, diagnostics for a dotted chain reported the chain's start.
- **`AST.hpp`** — All AST nodes: 30+ expression variants (`ExprLit`, `ExprCall`, `ExprBinary`, `ExprIf`, `ExprWhen`, `ExprClosure`, etc.), 8 pattern variants, 9 type variants, 8 item variants. Uses `std::variant` + `std::shared_ptr` for tree ownership
- **`type/Scope`** — Two-pass scope builder: declaration pass then resolution pass. Handles class scope with `this`-binding, field offsets, vtable construction, inheritance, interface conformance
- **`type/TypeChecker`** — Full expression/statement type checking with subtype relation, method/field lookup, inference variables (`InferVar`), numeric promotion. Stores errors in a `std::vector<std::string>`
- **`mir/MIR.hpp`** — Mid-level IR: `MirBody` contains `BasicBlock`s (labeled with `BlockId`), each block has `Statement`s and a `Terminator`. Instructions are SSA-like with `LocalId`, `Operand` (Copy/Move/Const), `Rvalue` (binary, unary, cast, field, index, aggregate), and `Terminator` (goto, switch, return, call, check-null)
- **`backend/Lowering`** — Lowers AST to MIR. Handles loop context stack (`break`/`continue` targets), scope → `LocalId` mapping, expression lowering with place vs. value distinctions
- **`backend/MIRCompiler`** — Compiles MIR to `Chunk` bytecode. Resolves label→offset via patch table. Uses the `OpCode` set for VM execution
- **`driver/Driver`** — Top-level orchestrator. Resolves imports (`import path::Module`) by crawling `.lx` files from the source directory, runs the full pipeline, then invokes the VM. Owns the `--verbose`/`--trace` banners and the `--dump-*` dispatch
- **`dump/`** — the `--dump-*` printers (`AstDump`, `HirDump`, `MirDump`, `TokensDump`), the `Printer` indent helper, and `Trace` (null pointer = tracing off, so hooks cost one pointer test). Selected via `DumpOptions` in `Driver.hpp`

### VM (`src/cpp/vm/`)

- **`VM`** — Stackless register VM: 256 `int64` registers with type tags (`TAG_HEAP=2`, `TAG_FLOAT=3`, `TAG_NULL=4`), a data stack (for expression temporaries), a call stack, flags (`FLAG_ZERO`, `FLAG_NEG`). At startup, JIT-decodes bytecode into a linear `JitInstruction` array for faster dispatch (no switching on opcodes at runtime)
- **`Heap`** — 9-byte cells: 8B value + 1B tag. Allocations have an 8-byte header (4B payload size + 4B type_id). Pointers to heap data use the payload offset (after the header)
- **`backend/Chunk.hpp`** — Bytecode container: `std::vector<uint8_t> code`, interned string pool, serialized type pool. `OperandTag` encodes register/immediate/string-index/address operands inline in the bytecode stream
- **`backend/Disasm`** — the single bytecode decoder. `opcodeName()` and `decodeBytecode()` back both `VM::tryBuildJit` and `--dump-bytecode`, so the VM and the disassembler cannot disagree about operand lengths. The opcode-name table used to be a file-local `static` in `VM.cpp`

### Language spec

The language spec is in `docs/spec.md` — Kotlin-inspired syntax with systems-level aspirations. Key features: nullable types (`T?`, `?.`, `?:`, `!!`), classes with single inheritance + multiple interfaces, data classes, enum classes with payloads, `when` pattern matching, closures, generics with `where` bounds, packages and imports.

The committed `demo.lx` exercises classes, interfaces, inheritance, data classes, enums, objects, recursion, and control flow.

### LSP

A separate TypeScript LSP lives at `D:\Development\Loxis-LSP`. It ports the v2 lexer/parser and provides diagnostics, completions, hover, document symbols, and go-to-definition. The VS Code extension includes syntax highlighting for `.lx` files.

## Key conventions

- **Naming**: types PascalCase, variables/functions camelCase. Namespace `loxis::v2`
- **Error handling**: Uses `ErrorReporter` with `CompileError` structs (non-throwing when possible)
- **AST ownership**: Uses `std::shared_ptr` for AST nodes
- **`std::variant`** is used extensively for expression/statement/type/pattern variants — pattern match with `std::visit` or `std::holds_alternative`
- **Registers**: Maps MIR `LocalId` directly to VM registers (0-255)
- **`std::cout`** for program output, **`std::cerr`** for compiler diagnostics and tracing
