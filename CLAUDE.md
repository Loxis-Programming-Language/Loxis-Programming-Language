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
# With verbose tracing:
.\build\Debug\Loxis.exe demo.lx 100000 verbose
```

There is no test framework — `.lx` files in `src/test/` and `demo.lx` are manual test scripts.

## Architecture

Loxis is a custom statically-typed language with a register-based VM, implemented as a classic compiler pipeline.

### Compiler (`src/cpp/v2/`)

All code lives in namespace `loxis::v2`. The pipeline:

```
.lx source → Lexer → Parser → Scope → TypeChecker → Lowering → MIRCompiler → Chunk → VM
```

- **`frontend/Lexer`** — ~80 token kinds in `Tk` enum, significant-newline handling (Kotlin/Python style), string/char/number literals
- **`frontend/Parser`** — Recursive-descent parser. Produces a `Module` containing `ItemPtr` variants (functions, classes, interfaces, enums, objects, imports). Entry point: `Parser::parseFile(path)`
- **`AST.hpp`** — All AST nodes: 30+ expression variants (`ExprLit`, `ExprCall`, `ExprBinary`, `ExprIf`, `ExprWhen`, `ExprClosure`, etc.), 8 pattern variants, 9 type variants, 8 item variants. Uses `std::variant` + `std::shared_ptr` for tree ownership
- **`type/Scope`** — Two-pass scope builder: declaration pass then resolution pass. Handles class scope with `this`-binding, field offsets, vtable construction, inheritance, interface conformance
- **`type/TypeChecker`** — Full expression/statement type checking with subtype relation, method/field lookup, inference variables (`InferVar`), numeric promotion. Stores errors in a `std::vector<std::string>`
- **`mir/MIR.hpp`** — Mid-level IR: `MirBody` contains `BasicBlock`s (labeled with `BlockId`), each block has `Statement`s and a `Terminator`. Instructions are SSA-like with `LocalId`, `Operand` (Copy/Move/Const), `Rvalue` (binary, unary, cast, field, index, aggregate), and `Terminator` (goto, switch, return, call, check-null)
- **`backend/Lowering`** — Lowers AST to MIR. Handles loop context stack (`break`/`continue` targets), scope → `LocalId` mapping, expression lowering with place vs. value distinctions
- **`backend/MIRCompiler`** — Compiles MIR to `Chunk` bytecode. Resolves label→offset via patch table. Uses the `OpCode` set for VM execution
- **`driver/Driver`** — Top-level orchestrator. Resolves imports (`import path::Module`) by crawling `.lx` files from the source directory, runs the full pipeline, then invokes the VM

### VM (`src/cpp/vm/`)

- **`VM`** — Stackless register VM: 256 `int64` registers with type tags (`TAG_HEAP=2`, `TAG_FLOAT=3`, `TAG_NULL=4`), a data stack (for expression temporaries), a call stack, flags (`FLAG_ZERO`, `FLAG_NEG`). At startup, JIT-decodes bytecode into a linear `JitInstruction` array for faster dispatch (no switching on opcodes at runtime)
- **`Heap`** — 9-byte cells: 8B value + 1B tag. Allocations have an 8-byte header (4B payload size + 4B type_id). Pointers to heap data use the payload offset (after the header)
- **`Chunk.hpp`** — Bytecode container: `std::vector<uint8_t> code`, interned string pool, serialized type pool. `OperandTag` encodes register/immediate/string-index/address operands inline in the bytecode stream

### Language spec

The language spec is in `docs/spec.md` — Kotlin-inspired syntax with systems-level aspirations. Key features: nullable types (`T?`, `?.`, `?:`, `!!`), classes with single inheritance + multiple interfaces, data classes, enum classes with payloads, `when` pattern matching, closures, generics with `where` bounds, packages and imports.

The current `demo.lx` exercises classes, interfaces, inheritance, data classes, enums, objects, recursion, and control flow.

### LSP

A separate TypeScript LSP lives at `D:\Development\Loxis-LSP`. It ports the v2 lexer/parser and provides diagnostics, completions, hover, document symbols, and go-to-definition. The VS Code extension includes syntax highlighting for `.lx` files.

## Key conventions

- **Naming**: types PascalCase, variables/functions camelCase. Namespace `loxis::v2`
- **Error handling**: Uses `ErrorReporter` with `CompileError` structs (non-throwing when possible)
- **AST ownership**: Uses `std::shared_ptr` for AST nodes
- **`std::variant`** is used extensively for expression/statement/type/pattern variants — pattern match with `std::visit` or `std::holds_alternative`
- **Registers**: Maps MIR `LocalId` directly to VM registers (0-255)
- **`std::cout`** for program output, **`std::cerr`** for compiler diagnostics and tracing
