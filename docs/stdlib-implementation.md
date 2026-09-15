# Standard library implementation tracking

Scope: a dependable standard library for command-line programs, collections,
file processing and JSON. Networking/HTTP and an asynchronous runtime remain a
separate milestone.

## Required milestones

- [x] Build current sources and record the baseline; define public API contracts.
- [x] Correct file errors, existence, integer parsing, container bounds and GC.
- [x] Complete list, text, optional/result and numeric utilities.
- [x] Optional-based iterators, lazy adapters, iterator-driven for loops,
      HashMap/HashSet and queue.
- [x] Filesystem/path/binary I/O, arguments/environment, clocks and seeded random.
- [x] JSON parser/serializer, API documentation and file-processing example.
- [x] Regression tests for failures, stdin, timeouts, temporary files and ARC/AARC.

## Compatibility and contracts

Existing text indices and lengths count UTF-8 bytes. Unicode code-point helpers
are separate APIs. Recoverable parse and I/O failures use Result; missing values
use Optional. Invalid collection indices and impossible capacities fail with a
runtime diagnostic. Iterator termination uses Optional so nullable elements are
representable. Iterators and streams are single-pass. Mutating a collection while
iterating is unsupported unless explicitly documented. Hash maps take explicit
hash/equality functions; equal keys must have equal hashes. Standard containers
are GC-managed; callers do not own raw storage. Compatibility wrappers remain
where feasible, with legacy failure behavior documented.

## Build observations

The existing `build/` cache refers to an unavailable Visual Studio installation.
The implementation uses `build-stdlib/` with Ninja and GNU C++ 13.1 instead.
The working tree already contained extensive changes before this task; they are
retained and form the source baseline.

## Verification

All CMake targets built successfully with GNU C++ 13.1. The final CTest run
passed both targets: 39 smoke cases and 60 standard-library executions (19
cases plus the JSON report example, each under NoGC, ARC and AARC). Positive
ARC/AARC unit cases additionally require zero live heap objects and zero invalid
handle operations after shutdown. `git diff --check` passed.

The initial environment selected an external `LOXIS_STDLIB` installation; both
test harnesses now explicitly select this repository's library. With that fixed,
the original 39-case smoke baseline passed.

Library workloads exposed prerequisite compiler/VM defects in generic AST/HIR
sharing, signature resolution, callback fields, short-circuit expressions,
null/float tags, register overflow, and virtual/interface return addresses.
These were corrected and covered by the expanded end-to-end tests. Windows
standard streams now preserve binary bytes. Iterator termination changed from
nullable values to Optional; see the API reference for migration contracts.

Known limits remain documented: byte-oriented text indexing, single-pass
iterators, caller-supplied hash/equality, bounded JSON nesting, and low-level
public JSON parser/writer helpers required by current module visibility lowering.
Networking/HTTP and asynchronous execution remain outside this milestone.
