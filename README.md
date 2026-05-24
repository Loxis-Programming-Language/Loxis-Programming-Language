<<<<<<< HEAD
# Loxis-Programming-Language
The Loxis Programming Language.
=======
# Loxis Programming Language

## Architecture

Loxis is a custom language ("Loixs") compiler and register-based VM, implemented as a classic 4-phase pipeline:

```
.lx source → Lexer → Parser → IRGen → Compiler → Chunk (bytecode) → VM
```

**src/ip/frontend/** — Lexer (tokenizer) and recursive-descent/Pratt parser. Produces an AST (`AST.hpp`) with `FunDeclNode` and `ClassDeclNode` as top-level nodes, `Stmt` variants for statements, and `Expr` variants for expressions. Newlines are statement separators (significant whitespace, Python-style). `Token.hpp` defines 26 token kinds.

**src/ip/ir/** — IR generation (`IRGen.cpp`). Lowers the AST into a CFG of `BasicBlock`s containing three-address-code `IRInstruction`s. Uses an infinite virtual register file (x0=zero/return, x16+ for variables, temporaries allocated from the same range). `OpCode.hpp` defines the IR opcode set and value types (`RegId`, `LabelRef`, `CondRef`). Types (`int`, `str`, `list<T>`, class types) are non-erased and serialized into a type pool.

**src/ip/backend/** — `Compiler.cpp` lowers IR to bytecode (`Chunk`). Handles label→offset resolution via a patch table. `Chunk.hpp` defines the `Chunk` struct bundling bytecode, an interned string pool, and a type pool (serialized `TypeNode`s).

**src/ip/vm/** — Stackless register VM: 256 int64 registers with type tags (TAG_HEAP=2, TAG_FLOAT=3), a data stack, a call stack, and a `Heap` (9-byte cells: 8B value + 1B tag). At startup the VM JIT-decodes bytecode into a linear `JitInstruction` array for faster dispatch.

**src/ip/Error.hpp** — Exception hierarchy: `LexError`, `ParseError`, `IRError`, `RuntimeError`, each carrying a `SourceLocation`.

**src/main.cpp** — Stale "Hello World" stub, not in CMakeLists.txt. The real entry point is `src/ip/main.cpp`.

## Language syntax (.lx files)

Kotlin/Python-inspired hybrid. Mandatory type annotations on parameters and return types.

```
fun add(a: int, b: int) -> int { return a + b }

// Expression body shorthand
fun double(x: int) -> int = x * 2

// Variables: `val` (immutable, default) and `var` (mutable)
val x: int = 42
var y = 100          // type inference from initializer

// Control flow
if x > 0 { print(1) } else if x < 0 { print(-1) } else { print(0) }
while y > 0 { y -= 1 }

// Compound assignment: += -= *= /=
z += 5

// Unary negation
print(-5)
```

**Types**: `int` (i32), `long` (i64), `float` (f32), `double` (f64), `str`, `bool`, `None`, class types, `list<T>`.

**Classes** — fields with defaults, methods with static dispatch:

```
class Point {
    var x: int = 0
    var y: int = 0
    fun move(dx: int, dy: int) -> None {
        this.x += dx
        this.y += dy
    }
}

val p = Point_new()    // constructor pattern: ClassName_new()
p.x = 10
p.move(5, 3)           // static dispatch → Point_move(p, 5, 3)
```

Method calls desugar via static dispatch: `obj.method(args)` → `ClassName_method(obj, args)`. Constructors follow the `ClassName_new()` convention. Field access uses `obj.field`. Method lookup is registered in `m_classMethods`.

**Built-in functions**: `print()`, `alloc("type", count)`, `hstore(base, offset, value)`, `hload(base, offset)`, `hfree(base)`, `ftoi(f_val)`.

**Heap**: 9-byte cells (8B value + 1B tag). `alloc` returns a heap reference. All heap addresses must be freed with `hfree`.

**Float literals**: `3.14` (double) or `3.14f` (float).

## Type system (`Type.hpp`)

- `TypeNode`: kind + optional elementType (for List) + className (for Class) + variance
- Declaration-site variance: `list<out T>` (covariant/read-only), `list<in T>` (contravariant/write-only), `list<T>` (invariant/read-write)
- `isSubtypeOf`: same-type equality, list variance rules; class inheritance not yet implemented
- `serialize`/`deserialize` to/from byte stream for the type pool

## Testing

There is no test framework. `.lx` files in the repo root are manual test scripts — `demo_final.lx` is the most comprehensive. `src/test_tokens.cpp` is a standalone lexer test harness (not in CMakeLists.txt — compile manually with `g++ -std=c++20 src/test_tokens.cpp src/ip/frontend/Lexer.cpp -I src/ip`).
>>>>>>> 855d759 (Initial commit)
