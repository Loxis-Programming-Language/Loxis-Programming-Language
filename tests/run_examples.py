#!/usr/bin/env python3
"""Compile every checked-in example without executing it."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    examples = sorted((root / "examples").glob("*.lx"))
    failures: list[tuple[Path, str]] = []

    for example in examples:
        result = subprocess.run(
            [str(args.exe.resolve()), str(example), "--dump-bytecode"],
            cwd=root,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        if result.returncode == 0:
            print(f"PASS {example.name}")
        else:
            failures.append((example, result.stdout + result.stderr))

    for example, output in failures:
        print(f"\nFAIL {example.name}\n{output.rstrip()}", file=sys.stderr)

    if failures:
        print(
            f"\n{len(failures)} of {len(examples)} examples failed to compile",
            file=sys.stderr,
        )
        return 1

    print(f"All {len(examples)} examples compile")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
