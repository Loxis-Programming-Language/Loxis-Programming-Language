# The Loxis Programming Language v2

Loxis v2 is a statically typed programming language with a C++20 compiler,
standard library, and register-based virtual machine.

## Architecture

```text
.lx source -> Lexer -> Parser -> AST -> TypeChecker -> HIR -> MIR -> Bytecode -> VM
```

- `src/cpp/v2/frontend/` contains the lexer and recursive-descent parser.
- `src/cpp/v2/type/` builds scopes, resolves names, and checks types.
- `src/cpp/v2/hir/` produces typed HIR and monomorphizes generics.
- `src/cpp/v2/backend/` lowers HIR to MIR and compiles MIR to bytecode.
- `src/cpp/v2/driver/` loads modules and coordinates the compiler pipeline.
- `src/cpp/vm/` contains the register VM and heap implementation.
- `stdlib/loxis/` contains the Loxis v2 standard library.
- `src/cpp/main_v2.cpp` is the compiler entry point.

All compiler code uses the `loxis::v2` namespace. The CMake library target is
`loxis_v2`, and the command-line executable is `Loxis`.

## Build

Requirements:

- CMake 3.26 or newer
- A C++20 compiler
- PowerShell for the smoke test suite
- Python 3 for the standard-library regression suite

Visual Studio example:

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

MinGW or Ninja example:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Run

For a Visual Studio build:

```powershell
.\build\Debug\Loxis.exe run .\examples\hello_world.lx
```

For a single-configuration build:

```powershell
.\build\Loxis.exe run .\examples\hello_world.lx
```

Use `Loxis check file.lx` to compile and type-check without running the
program, and `Loxis version` to print the installed version. Direct
`Loxis file.lx` invocation remains supported for compatibility.

Run every `.lx` file below a project test directory with:

```powershell
.\build\Debug\Loxis.exe test .\tests
```

Each test is a normal Loxis program: exit normally to pass, or produce a
compile/runtime error to fail. Files are discovered recursively and run in
sorted path order; the command returns a nonzero status if any test fails.

## Package

After a Release build, create a relocatable Windows ZIP with:

```powershell
cpack --config .\build\CPackConfig.cmake -C Release
```

The archive contains `Loxis.exe`, the standard library, runnable examples,
documentation and the license. Keep `Loxis.exe` beside the bundled `stdlib` directory; the executable
discovers that directory even when invoked from another working directory. The
generated archive is written to `build/packages`.

The compiler resolves `loxis.*` imports from `stdlib/loxis/`. Set
`LOXIS_STDLIB` to override the standard-library location.

## Test

The v2 smoke suite exercises the compiler and VM end to end:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\run_smoke.ps1 `
  -Exe .\build\Debug\Loxis.exe
```

It is also registered with CTest as `loxis_smoke`:

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

When Python is available at configuration time, CTest also registers
`loxis_stdlib`. It exercises collections, JSON, host services and error paths
under NoGC, ARC and AARC. Both harnesses use this repository's standard library.

See the [standard-library API reference](docs/stdlib.md) and the complete
[JSON file-processing example](examples/json_report.lx). Pass application
arguments after `--`, for example `Loxis examples/json_report.lx -- input.json output.json`.

For a runnable tour of text, lists, iterators, Optional/Result, math, random and
JSON file I/O, try `Loxis examples/stdlib_demo.lx 10000000`.
See [example instructions](examples/README.md) for custom input and output paths.

## Language

Built-in type names use lowercase. For example, `val xs: list<int> = list<int>()`
creates an empty list after importing `loxis.collections.list`.
Try `Loxis examples/list_demo.lx` for list construction, lazy filtering and mapping.

Loxis v2 uses Kotlin-inspired declarations and expressions:

```text
fun add(a: int, b: int) -> int = a + b

class Counter(var value: int) {
    fun increment() {
        value = value + 1
    }
}

fun main() {
    val counter = Counter(41)
    counter.increment()
    print(counter.value)
}
```

The current executable subset includes functions, classes, interfaces,
generics, closures, enums, nullable operations, modules, operator overloads,
and standard-library collections. See [the language specification](docs/spec.md)
and [the executable-subset notes](docs/executable-subset.md) for details.

## Project status

- [0.1 stability contract](docs/status.md)
- [Roadmap](ROADMAP.md)
- [Language specification](docs/spec.md)
- [Executable subset](docs/executable-subset.md)

Loxis is licensed under the [MIT License](LICENSE).
