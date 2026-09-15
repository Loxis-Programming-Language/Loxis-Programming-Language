# Loxis examples

Run these commands in the repository root with the existing Ninja build:

```powershell
$env:LOXIS_STDLIB = "$PWD\stdlib"
.\build-stdlib\Loxis.exe .\examples\stdlib_demo.lx 10000000
```

`stdlib_demo.lx` runs without input. It counts words, filters and sorts lists,
uses capturing callbacks, handles Optional/Result values, compares UTF-8 byte
and code-point counts, evaluates double math, makes a seeded random selection,
and writes and reads a JSON report. Construction uses `list<T>()`.

It creates or overwrites `stdlib-demo.json` in the current directory. The default
text has 9 words and 5 unique words; `loxis` occurs 3 times and `makes` and `fun`
occur twice each. (The remaining words occur once.)

Supply your own text and output path after `--`:

```powershell
.\build-stdlib\Loxis.exe .\examples\stdlib_demo.lx 10000000 -- "cat cat code loxis" .\my-report.json
Get-Content .\my-report.json -Encoding utf8
```

The report is a JSON array of `[word, count]` rows, sorted by descending count.
Words are separated by spaces, tabs, CR or LF. Counting is case-sensitive and
preserves punctuation; empty input produces an empty report. The output's parent
directory must already exist. The text and output path are optional, in that order.
Elapsed time varies between runs; the seeded selection is reproducible.
Non-ASCII command-line arguments currently depend on the Windows terminal and
runtime encoding. The built-in Unicode sample uses `fromCodePoint` so its byte
and code-point counts do not depend on argument encoding.

Other examples:

- `list_demo.lx`: a short `list<int>()` and lazy iterator introduction.
- `json_report.lx -- input.json output.json`: reads a JSON array and wraps it with
  its element count. Both paths are required, and the output is overwritten.

See the [stdlib API reference](../docs/stdlib.md) for module contracts.
