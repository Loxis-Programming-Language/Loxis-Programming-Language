"""Calls: parameter binding, generic collectors, evaluation order and GC."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FAILURES = {
    "duplicate": ('fun f(x: int) {}\nfun main() { f(1, x = 2) }', "supplied more than once"),
    "unknown": ('fun f(x: int) {}\nfun main() { f(wrong = 1) }', "unknown named argument"),
    "missing": ('fun f(x: int, **rest: int) {}\nfun main() { f(extra = 1) }', "missing argument"),
    "position_after_name": ('fun f(x: int, y: int) {}\nfun main() { f(x = 1, 2) }', "positional argument cannot follow"),
    "vararg_type": ('fun f(*xs: int) {}\nfun main() { f(1, "bad") }', "array element type mismatch"),
    "kwargs_type": ('fun f(**xs: int) {}\nfun main() { f(a = 1, b = "bad") }', "map value type mismatch"),
    "generic_conflict": ('fun f<T>(x: T, *xs: T) {}\nfun main() { f(1, "bad") }', "cannot infer generic"),
    "generic_empty": ('fun f<T>(*xs: T) {}\nfun main() { f() }', "cannot infer generic"),
    "duplicate_keyword": ('fun f(**xs: int) {}\nfun main() { f(a = 1, a = 2) }', "supplied more than once"),
    "spread_type": ('fun f(*xs: int) {}\nfun main() { f(*1) }', "positional spread requires a non-null array"),
    "keyword_spread_type": ('fun f(**xs: int) {}\nfun main() { f(**1) }', "keyword spread requires HashMap"),
    "collector_order": ('fun f(*xs: int, x: int) {}\nfun main() {}', "only a '**' parameter may follow"),
    "kwargs_order": ('fun f(**xs: int, x: int) {}\nfun main() {}', "'**' parameter must be last"),
    "duplicate_positional_collector": ('fun f(*xs: int, *ys: int) {}\nfun main() {}', "only a '**' parameter may follow"),
    "duplicate_keyword_collector": ('fun f(**xs: int, **ys: int) {}\nfun main() {}', "'**' parameter must be last"),
    "too_many_stars": ('fun f(***xs: int) {}\nfun main() {}', "expected identifier"),
    "old_vararg_syntax": ('fun f(vararg xs: int) {}\nfun main() {}', "expected ':'"),
    "old_kwargs_syntax": ('fun f(kwargs xs: int) {}\nfun main() {}', "expected ':'"),
    "duplicate_param": ('fun f(x: int, x: int) {}\nfun main() {}', "duplicate parameter"),
    "spread_collision": ('fun f(**xs: int) {}\nfun main() { val xs = {"a": 2}; f(a = 1, **xs) }', "duplicate keyword argument"),
    "spread_fixed_collision": ('fun f(a: int, **xs: int) {}\nfun main() { val xs = {"a": 2}; f(1, **xs) }', "keyword spread duplicates a fixed parameter"),
    "spread_missing": ('fun f(a: int, **xs: int) {}\nfun main() { val xs = {"other": 2}; f(**xs) }', "missing argument 'a' from keyword spread"),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    exe = parser.parse_args().exe.resolve()
    env = dict(os.environ, LOXIS_STDLIB=str(ROOT / "stdlib"))
    for gc in ("nogc", "arc", "aarc"):
        with tempfile.TemporaryDirectory(prefix="loxis-calls-") as work:
            command = [str(exe), str(ROOT / "src/test/call_arguments.lx"), "10000000", "--gc=" + gc]
            if gc != "nogc":
                command.append("--gc-stats")
            result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", cwd=work, env=env, timeout=30)
            clean = not result.stderr if gc == "nogc" else (
                result.stderr.startswith("gc=") and re.search(r"\blive=0\b", result.stderr)
                and re.search(r"\binvalidOps=0\b", result.stderr))
            assert result.returncode == 0 and result.stdout == "call arguments ok\n" and clean, (
                f"{gc}: {result.returncode}\n{result.stdout}\n{result.stderr}")
            print("PASS calls/" + gc)
            for name, (source, diagnostic) in FAILURES.items():
                path = Path(work) / (name + ".lx")
                path.write_text("import loxis.collections.map\n" + source + "\n", encoding="utf-8")
                result = subprocess.run([str(exe), str(path), "10000000", "--gc=" + gc],
                                        capture_output=True, text=True, encoding="utf-8", cwd=work, env=env, timeout=30)
                assert result.returncode != 0 and diagnostic in result.stderr, (
                    f"{name}/{gc}: {result.returncode}\n{result.stdout}\n{result.stderr}")
                print(f"PASS {name}/{gc}")


if __name__ == "__main__":
    main()
