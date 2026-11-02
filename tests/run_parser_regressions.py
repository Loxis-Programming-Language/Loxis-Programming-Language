"""Parser regressions: statement boundaries and diagnostic locations.

Two bugs are covered here, both rooted in the fact that `Parser::Parser` drops
every Tk::Newline token, so the parser has to infer statement boundaries:

  * a '(' / '[' that opens a new line must start a statement rather than
    continue the previous expression (parser_newline_*);
  * a postfix node must carry its own location, so a diagnostic for a dotted
    chain points at the member and not at the chain's start
    (parser_member_span, parser_newline_forEach).

The failing cases assert the exact `-->` location, which catches a diagnostic
that regressed to reporting the chain start one line earlier.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

# name -> (expected stdout, expected exit code, required stderr substrings,
#          forbidden stderr substrings)
CASES = {
    "parser_newline_paren": ("6\n", 0, [], []),
    "parser_newline_bracket": ("5\n", 0, [], []),
    "parser_member_span": (
        None, None,
        ["parser_member_span.lx:9:10", "'missing'", "not found on type Foo"],
        ["parser_member_span.lx:8:"],
    ),
    "parser_newline_forEach": (
        None, None,
        ["parser_newline_forEach.lx:14:12", "'forEach'"],
        ["parser_newline_forEach.lx:13:"],
    ),
    "data_class_unsupported_property": (
        None, None,
        ["data_class_unsupported_property.lx:1:28",
         "generated data-class members do not support property 'value' of type float",
         "use int, str, bool, char or double"],
        ["Fatal error", "numeric op on non-numeric"],
    ),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    args = parser.parse_args()
    exe = args.exe.resolve()

    failed = 0
    for name, (expected_out, expected_exit, want, forbid) in CASES.items():
        with tempfile.TemporaryDirectory(prefix="loxis-parser-") as work:
            result = subprocess.run(
                [str(exe), str(ROOT / "src/test" / (name + ".lx")), "200000"],
                capture_output=True, text=True, encoding="utf-8", cwd=work,
                env=dict(os.environ, LOXIS_STDLIB=str(ROOT / "stdlib")), timeout=30)

        problems = []
        if expected_out is not None:
            if result.returncode != expected_exit:
                problems.append(f"exit={result.returncode} expected {expected_exit}")
            if result.stdout != expected_out:
                problems.append(f"stdout={result.stdout!r} expected {expected_out!r}")
            if result.stderr:
                problems.append(f"unexpected stderr: {result.stderr!r}")
        else:
            if result.returncode == 0:
                problems.append("expected a compile error, but compilation succeeded")
        for needle in want:
            if needle not in result.stderr:
                problems.append(f"stderr is missing {needle!r}")
        for needle in forbid:
            if needle in result.stderr:
                problems.append(f"stderr still reports {needle!r} (wrong location)")

        if problems:
            failed += 1
            print(f"[FAIL] {name}")
            for problem in problems:
                print(f"       {problem}")
            print("       --- stderr ---")
            print("       " + result.stderr.replace("\n", "\n       "))
        else:
            print(f"[PASS] {name}")

    if failed:
        raise SystemExit(f"{failed} parser regression(s) failed")
    print("All parser regressions passed.")


if __name__ == "__main__":
    main()
