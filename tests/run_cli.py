#!/usr/bin/env python3
"""Regression tests for the user-facing Loxis command line."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def invoke(exe: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(exe), *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    exe = parser.parse_args().exe.resolve()

    version = invoke(exe, "version")
    require(version.returncode == 0, f"version failed: {version.stderr}")
    require(version.stdout == "Loxis 0.1.0\n", f"unexpected version: {version.stdout!r}")

    checked = invoke(exe, "check", str(ROOT / "examples" / "hello_world.lx"))
    require(checked.returncode == 0, f"check failed: {checked.stderr}")
    require(checked.stdout == "", f"check executed the program: {checked.stdout!r}")

    run = invoke(exe, "run", str(ROOT / "examples" / "hello_world.lx"))
    require(run.returncode == 0, f"run failed: {run.stderr}")
    require(run.stdout == "Hello, Loxis!\n", f"unexpected run output: {run.stdout!r}")

    legacy = invoke(exe, str(ROOT / "examples" / "hello_world.lx"))
    require(legacy.returncode == 0, f"legacy invocation failed: {legacy.stderr}")
    require(legacy.stdout == run.stdout, "legacy invocation changed behavior")

    with tempfile.TemporaryDirectory(prefix="loxis-cli-") as directory:
        invalid = Path(directory) / "invalid.lx"
        invalid.write_text("fun main() { missing() }\n", encoding="utf-8")
        rejected = invoke(exe, "check", str(invalid))
    require(rejected.returncode != 0, "check accepted an invalid program")
    require("missing" in rejected.stderr, "check did not report the invalid symbol")

    print("CLI run/check/version regressions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
