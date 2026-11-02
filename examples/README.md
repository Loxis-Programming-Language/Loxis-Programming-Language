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

- `todo_cli.lx`: a persistent command-line task list that exercises repeated
  process invocation, application arguments, text parsing and file updates.
  Try `Loxis run examples/todo_cli.lx -- tasks.txt add "Buy milk"`, followed by
  `... list`, `... done 1`, or `... clear`.
- `call_arguments_demo.lx`: an editable checkout playground for generic `*rest`
  and `**options` parameters, named arguments, `*array` / `**map` expansion,
  methods and forwarding.
  Run `./build-stdlib/Loxis.exe ./examples/call_arguments_demo.lx`, or customize
  customer, quantity and discount with
  `./build-stdlib/Loxis.exe ./examples/call_arguments_demo.lx -- Alex 3 8`.
  It prints results and does not write files. Commented-out calls at the bottom
  demonstrate invalid types and duplicate arguments.
- `snake.lx`: a playable game, and the only example that needs a real console —
  it redraws the board in place with ANSI escapes and reads keys with `readChar`.
  Input is blocking, so the snake takes one step per keypress rather than moving
  on a timer; `w`/`a`/`s`/`d` or the arrow keys steer, `q` quits. The first
  argument is the instruction budget and a number after `--` seeds the game, so
  `snake.lx 50000000 -- 12345` replays the same food sequence every time. The
  default budget of 1,000,000 lasts roughly 450 steps.
- `list_demo.lx`: a short `list<int>()` and lazy iterator introduction.
- `json_report.lx -- input.json output.json`: reads a JSON array and wraps it with
  its element count. Both paths are required, and the output is overwritten.

See the [stdlib API reference](../docs/stdlib.md) for module contracts.
