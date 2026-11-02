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
    require(version.stdout == "Loxis 0.1.0-alpha.2-dev\n", f"unexpected version: {version.stdout!r}")

    checked = invoke(exe, "check", str(ROOT / "examples" / "hello_world.lx"))
    require(checked.returncode == 0, f"check failed: {checked.stderr}")
    require(checked.stdout == "", f"check executed the program: {checked.stdout!r}")

    run = invoke(exe, "run", str(ROOT / "examples" / "hello_world.lx"))
    require(run.returncode == 0, f"run failed: {run.stderr}")
    require(run.stdout == "Hello, Loxis!\n", f"unexpected run output: {run.stdout!r}")

    legacy = invoke(exe, str(ROOT / "examples" / "hello_world.lx"))
    require(legacy.returncode == 0, f"legacy invocation failed: {legacy.stderr}")
    require(legacy.stdout == run.stdout, "legacy invocation changed behavior")

    missing = invoke(exe, "check", "does-not-exist.lx")
    require(missing.returncode != 0, "missing source was accepted")
    require("Source file does not exist" in missing.stderr, "missing source diagnostic regressed")

    wrong_extension = invoke(exe, "check", str(ROOT / "README.md"))
    require(wrong_extension.returncode != 0, "non-Loxis source was accepted")
    require("must use the .lx or .lxs extension" in wrong_extension.stderr, "extension diagnostic regressed")

    bad_slots = invoke(
        exe, "run", str(ROOT / "examples" / "hello_world.lx"), "--slots=nope"
    )
    require(bad_slots.returncode != 0, "invalid slot count was accepted")
    require("Invalid heap slot count: nope" in bad_slots.stderr, "slot diagnostic regressed")
    require("Fatal error" not in bad_slots.stderr, "invalid slot count escaped validation")

    huge_limit = invoke(
        exe, "run", str(ROOT / "examples" / "hello_world.lx"), "999999999999999999999"
    )
    require(huge_limit.returncode != 0, "overflowing instruction limit was accepted")
    require("Invalid instruction limit" in huge_limit.stderr, "limit diagnostic regressed")
    require("Fatal error" not in huge_limit.stderr, "instruction limit escaped validation")

    with tempfile.TemporaryDirectory(prefix="loxis-cli-") as directory:
        invalid = Path(directory) / "invalid.lx"
        invalid.write_text("fun main() { missing() }\n", encoding="utf-8")
        rejected = invoke(exe, "check", str(invalid))
    require(rejected.returncode != 0, "check accepted an invalid program")
    require("missing" in rejected.stderr, "check did not report the invalid symbol")

    with tempfile.TemporaryDirectory(prefix="loxis-test-") as directory:
        suite = Path(directory)
        (suite / "passes.lx").write_text(
            "fun main() { require(2 + 2 == 4, \"math\") }\n",
            encoding="utf-8",
        )
        tested = invoke(exe, "test", str(suite))
        require(tested.returncode == 0, f"test command failed: {tested.stderr}")
        require("1 passed, 0 failed, 1 total" in tested.stdout, "missing test summary")

        (suite / "fails.lx").write_text(
            "fun main() { missing() }\n",
            encoding="utf-8",
        )
        failed_suite = invoke(exe, "test", str(suite))
        require(failed_suite.returncode != 0, "test command ignored a failing test")
        require("1 passed, 1 failed, 2 total" in failed_suite.stdout, "wrong failure summary")

    print("CLI run/check/version regressions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
