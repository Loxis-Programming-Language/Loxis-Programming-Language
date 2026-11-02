# Loxis standard library

Import modules explicitly, for example `import loxis.collections.map`.
`loxis` prelude functions are loaded automatically. This document describes the
repository's library; set `LOXIS_STDLIB` to this repository's `stdlib` directory
when another installation is configured in your environment.

## Public API boundary and migration

Ordinary code uses values, constructors and scoped service objects. VM bridge
names (`native*`, heap operations, `syscall`, numeric conversion opcodes) and
`__loxis_*` helpers are internal APIs. The compiler checks the resolved source
file, not its `package` declaration. Calling or taking an alias to these APIs
from an ordinary file is rejected.

For runtime experiments only, explicitly `import loxis.unsafe` in the file
using low-level calls. This opt-in does not propagate to importing files.
`Unsafe.arrayNew/Get/Set/Len/Free` provide the old raw integer-handle array
operations. Normal programs use array literals or `list<T>()`, without `free`.
This is an API boundary, not a sandbox for untrusted standard libraries.

| Removed public spelling | Use instead |
| --- | --- |
| `i2s`, `intToString`, free `toString` | `value.toString()` or `${value}` |
| `s2i`, `parseInt`, `tryParseInt` | `text.toInt()` / `text.toIntResult()` |
| `strlen`, `strslice`, `strchar` | `text.length()`, `text.slice(...)`, `text.byteAt(...)` |
| `concat`, `strcat`, `fadd/fsub/fmul/fdiv/fneg` | ordinary operators |
| `listXxx(items, ...)`, `listFree(items)` | `items.xxx(...)`; remove explicit freeing |
| `listWithCapacity`, `flatten`, `chunked` | `Lists.withCapacity`, `Lists.flatten`, `Lists.chunked` |
| `optionalXxx`, `resultXxx` | value methods, `Option` and `Results` factories |
| `iterXxx`, `listIter`, `emptyIter` | `Iter` methods |
| `pathXxx`, `arguments`, `environment`, clock functions | `Path`, `System`, `Clock` |
| bare math and file operations | `Math`, `File`, `Files`, `Console`, `Lines` |
| duplicate `typeof` wrappers | `Reflection.typeOf(value)` |

The common `print/println/write`, `require`, `error` and `TODO` entry points
remain. Explicitly imported `apply`/`twice`, the `stream` factory, and hash/equality
callbacks also remain free functions; ordinary user-defined functions are unaffected.
There are no public compatibility aliases for the removed spellings.

## Errors and values

`Optional<T>` has `Empty` and `Value(T)` variants. `Result<T, E>` has `Success(T)`
and `Failure(E)` variants. An optional payload may itself be nullable.

| Module | APIs |
| --- | --- |
| `loxis.option` | `Option.some<T>`, `Option.none<T>`; `Optional<T>` with `hasValue`, `isEmpty`, `map`, `flatMap`, `getOrElse`, `getOrElseGet`, `valueOrFail` |
| `loxis.result` | `Result::<T, E>::Success(value)`, `Result::<T, E>::Failure(error)`; `Result<T, E>` with `isSuccess`, `isFailure`, `map`, `flatMap`, `mapError`, `getOrElse`, `getOrElseGet`, `toOptional` |

Use `Option.some/none/fromNullable`, `Results.success/failure/fromOptional`,
`optional.isEmpty()`, `optional.orNull()`, `optional.getOrElse(value)`,
`result.isSuccess()`, and `result.getOrElse(value)`. The old prefixed free
functions have been removed from the public API. `getOrElseGet` invokes its fallback only on an empty/failure value. `require`
and `error` report a runtime failure with a nonzero process exit status.

## Collections

Builtin type names use lowercase: `int`, `str`, and `list<T>`. Create an empty,
GC-managed list with `list<int>()`, or use `val values: list<int> = list()` and
let the expected type infer `T`; it grows on demand.
The former `List<T>` type is now spelled `list<T>` throughout the library.
Use `Lists.withCapacity<T>(capacity)` only when preallocated storage matters.
Methods: `size`,
`capacity`, `isEmpty`, `get`, `set`, `push`, `insert`, `removeAt`, `pop`, `clear`.
Algorithms are methods too: `map`, `filter`, `fold`, `forEach`, `count`, `any`,
`all`, `contains`, `find`, `first`, `lastOrNull`, `sortBy`, `take`, `append`,
`reverse`, and `reversed`. `contains(value)` compares with `==` and needs no
callback, unlike `any`.

The Kotlin-style helpers are `mapIndexed`, `filterIndexed`, `forEachIndexed`
(position first: `{ i, value -> ... }`), `flatMap`, `distinct`, `distinctBy`,
`indexOfFirst`, `indexOfLast`, `none`, `reduce`, and the slicing set `drop`,
`dropLast`, `takeLast`, `takeWhile`, `dropWhile`. Out-of-range slice counts
clamp rather than fail; `reduce` on an empty list fails, so use `fold` there.
`Text.joinToString(items, separator, render)` renders as it joins.
`Lists.flatten(items)` and `Lists.chunked(items, size)` handle nested lists
whose constraints cannot yet be expressed as ordinary generic list methods.

Lambdas may be called directly or from inside another closure, and a closure may
assign to a captured local (the write goes through the closure's environment, so
a counter closure keeps state across calls). `get/set/removeAt` require
`0 <= index < size`; `insert` also accepts `size`.
`pop` returns `Optional<T>`. Negative or unrepresentable capacities fail.
Clearing/removing elements releases their stored references.

The old `listXxx` mirrors and `listFree` are no longer public. GC owns storage
reclamation. `sortBy` mutates its input; its comparator must define a strict
ordering. It currently uses selection sort.

Create a map directly with `HashMap<K, V>(hash, equal)`.
Equal keys must have equal hashes, and keys must not change in ways affecting
either callback while stored. Methods: `size`, `isEmpty`, `containsKey`, `get`,
`put`, `remove`, `keys`, `clear`. `put/remove` return the previous optional value.
Iteration order is unspecified. `hashInt/equalInt` and `hashString/equalString`
are supplied. Hashes are not intended to resist adversarial collision attacks.
Non-empty map literals use `{key: value, ...}` and produce a `HashMap<K, V>`.
Their keys currently support `str` and `int`; duplicate keys keep the last value.

Create a set with `HashSet<T>(hash, equal)`; it provides
`size/isEmpty/contains/add/remove/values/clear`.
`add/remove` report whether the set changed. `Queue<T>()` provides
`enqueue/dequeue/size/isEmpty/clear`; dequeue returns `Optional<T>`.

## Iterators and streams

`Iterator<T>.next()` returns `Optional<T>`. `Empty` means exhausted, while
`Value(null)` is an ordinary nullable element. This replaces the earlier `T?`
protocol. `for value in iterator` supports `break` and `continue`.

Use the overloaded `stream<T>()`: pass a list or iterator, or no argument for an
empty stream. Streams compose through `map<U>`, `filter`,
`take`, `drop`, `zip<U>`, `fold<U>`, `forEach`, `count`, `toList`, `any`, `all`,
and `find`. Zip produces `Pair<A, B>` and stops at the
shorter source; when the right side ends it may consume one extra left element.
`Iter` supplies `fromList`, `empty`, `map`, `filter`, `take`, `drop`, `zip`,
`forEach`, `count`, `any`, `all`, `find`, `fold`, and `toList`.
Counts for take/drop must be nonnegative.

The `iterXxx` and `streamXxx` free functions are no longer public. Prefer stream
methods; `Streams.map` is also available for callers needing a scoped helper.
Iterators and streams are lazy and single-pass. Repeating a terminal
operation continues from the current position. Mutating an underlying collection
during iteration is unsupported.

## Text and numbers

Text indices and `text.length()` count UTF-8 **bytes**, not code points or graphemes.
`text.slice(start, count)` clamps its range. `text.byteAt(index)` returns a byte
or -1 out of bounds. `Text.fromByte` accepts 0..255. `Unicode.fromCodePoint`
encodes a Unicode scalar; `text.codePoints()` validates UTF-8 and returns `Result<list<int>, str>`.

`split` preserves empty fields; `split/replace` reject an empty separator.
`Text.join` and the string methods `trim` (ASCII whitespace), `startsWith`,
`endsWith`, `contains`, `indexOf`, `repeat`, and `toString` are available in `loxis.text`.
Use `+` or interpolation for concatenation. `text.repeat(n)` builds by doubling, so it stays
linear rather than copying the accumulator once per repetition.
`toString()` is available on `int`, `double`, `bool`, and `str`; `bool.toString()` yields
`"true"`/`"false"`. A bool carries no VM tag of its own, so both `print(bool)`
and `"${flag}"` select that text statically rather than stringifying by tag;
`print(flag)` says `true`, not `1`.
Prefer value-oriented calls such as `text.split(" ")`, `text.replace(old, new)`,
`text.length()`, `text.codePoints()`, and `number.toString()`.
`text.words()` splits ordinary text on ASCII whitespace without exposing numeric
control-character codes or producing empty fields.

`text.toIntResult()` parses an entire signed 64-bit decimal string and returns a
Result. It rejects whitespace, missing digits, trailing input and overflow.
`text.toInt()` throws on invalid input. `int.toDouble()` and `double.toInt()`
replace the old `itof` / `ftoi` primitives in ordinary code.

`text.toDoubleResult()` returns a Result for whole-string floating-point parsing;
`double.toString()` formats double-precision values.

`loxis.math` supplies `clamp`, `gcd`, checked `powInt`,
plus double-precision `sqrt/floor/ceil/round/sin/cos/log/exp/pow/isFinite`.
These floating-point operations are grouped under `Math`, for example
`Math.sqrt(81.0)`.
Negative integer exponents and invalid clamp bounds fail. `abs` rejects the
minimum signed integer, whose magnitude cannot be represented. Floating-point
domain errors follow IEEE behavior and may produce NaN/infinity.

## I/O and host services

`File.readText`, `File.writeText`, `File.appendText`, `File.readBytes`, and
`File.writeBytes` return `Result` values. Byte lists contain integers in 0..255. Binary reads
preserve NUL and all byte values. `exists` checks filesystem metadata, including
empty files. Write/append results contain the number of bytes written;
`File.remove` returns a bool.

`loxis.io` supplies `Console.readChar/writeChar`, `Console.readLineOptional`,
`Console.readLine`, `Console.readAll`, `Console.print/println`, `Lines.read`
(stdin or a file path), and `Lines.write(path, lines)`.
`readLineOptional` distinguishes EOF from an empty line.
LF terminates a line; a CR immediately preceding LF is stripped. `readAll`
preserves every byte. `writeLines` inserts LF between lines without a final LF.

`Files` provides `isDirectory`, `createDirectories`, `removeDirectory`, and `entries`.
Directory removal is nonrecursive and requires an empty directory. Directory
listings contain sorted entry names. Paths follow the host filesystem's rules.
`Path` provides `join`, `normalize`, `parent`, `name`, `extension`, and
`currentDirectory`; normalization is lexical, without resolving
symlinks. An absolute child in `Path.join` follows host path replacement semantics.

`System.arguments()` contains only arguments after the CLI `--` separator.
`System.environment(name)` returns `Optional<str>`. `Clock` offers Unix epoch
milliseconds and monotonic nanoseconds; monotonic timestamps are for differences
within one host session. `Random.seeded(seed)` creates a reproducible Park-Miller
generator with `nextInt`, unbiased `nextBounded`, and `nextDouble`. This generator
is not cryptographic.

## Networking

`loxis.net` provides blocking TCP networking through `Tcp.connect(host, port)`
and `Tcp.listen(host, port)`. Sockets support `read`, `write`, `localPort`, and
idempotent `close`; listeners support `accept`, `localPort`, and `close`.
Failures throw `SocketException`; networking does not wrap ordinary operations
in `Result`. An empty read means the peer closed its sending side. DNS resolution
is performed by the host resolver. The first version intentionally does not include
UDP, TLS, HTTP, timeouts, or asynchronous I/O.

See `examples/tcp_client.lx` and `examples/tcp_echo_server.lx`.

## JSON

`Json.parse(str) -> Result<JsonValue, str>` and `value.stringify()` support null,
bool, string, number,
array, and object. Numbers store their original decimal lexemes, preserving
integers beyond 64 bits and exponent spelling. Object values use
`HashMap<str, JsonValue>`; key order is unspecified. Duplicate object keys are
rejected. Errors include a byte position where available.

UTF-8 and Unicode surrogate pairs are validated. Trailing tokens, trailing commas,
leading zeros, unescaped controls, invalid escapes, and non-JSON numbers are
rejected. Nesting is bounded at 128 for parsing and serialization; cyclic manually
constructed values eventually hit the serialization depth limit. `JsonParser`
and `JsonWriter` are low-level implementation classes; use the Result-returning
entry points in application code.

See `examples/json_report.lx` for file reading, JSON parsing, collection mapping
and output serialization in one executable program.

## Validation

```powershell
cmake -S . -B build-stdlib -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-stdlib --target Loxis
python tests/run_stdlib.py --exe build-stdlib/Loxis.exe
powershell -ExecutionPolicy Bypass -File tests/run_smoke.ps1 -Exe build-stdlib/Loxis.exe
```

The stdlib harness runs in isolated temporary directories with stdin fixtures,
30-second process timeouts and expected-failure diagnostics under NoGC, ARC and
AARC. Test reports, rather than this API reference, determine verified coverage.
