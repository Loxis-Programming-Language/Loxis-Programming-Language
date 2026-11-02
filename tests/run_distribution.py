#!/usr/bin/env python3
"""Verify that the relocatable binary-plus-stdlib layout works in isolation."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    source_exe = parser.parse_args().exe.resolve()

    with tempfile.TemporaryDirectory(prefix="loxis-dist-") as directory:
        temp = Path(directory)
        package = temp / "package"
        project = temp / "project"
        working = temp / "working"
        package.mkdir()
        project.mkdir()
        working.mkdir()

        exe = package / source_exe.name
        shutil.copy2(source_exe, exe)
        shutil.copytree(ROOT / "stdlib", package / "stdlib")

        program = project / "main.lx"
        program.write_text(
            "import loxis.text\n"
            "fun main() { print(\"41\".toInt() + 1) }\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            [str(exe), "run", str(program)],
            cwd=working,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )

    if result.returncode != 0:
        raise SystemExit(f"relocated distribution failed:\n{result.stderr}")
    if result.stdout != "42\n":
        raise SystemExit(f"unexpected relocated output: {result.stdout!r}")
    print("Relocatable distribution layout passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
