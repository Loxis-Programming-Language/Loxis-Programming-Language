#!/usr/bin/env python3
"""End-to-end workflow for the persistent todo CLI example."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    exe = parser.parse_args().exe.resolve()
    program = ROOT / "examples" / "todo_cli.lx"

    with tempfile.TemporaryDirectory(prefix="loxis-todo-") as directory:
        work = Path(directory)
        database = work / "tasks.txt"

        def run(*arguments: str, expected: int = 0) -> subprocess.CompletedProcess[str]:
            result = subprocess.run(
                [str(exe), "run", str(program), "--", str(database), *arguments],
                cwd=work,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
            )
            if result.returncode != expected:
                raise AssertionError(
                    f"todo {arguments} exited {result.returncode}, expected {expected}\n"
                    f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
                )
            return result

        assert run("list").stdout == "No tasks.\n"
        assert run("add", "Buy milk").stdout == "Added: Buy milk\n"
        assert run("add", "Write tests").stdout == "Added: Write tests\n"
        assert run("list").stdout == "1. [ ] Buy milk\n2. [ ] Write tests\n"
        assert run("done", "1").stdout == "Completed: Buy milk\n"
        assert run("list").stdout == "1. [x] Buy milk\n2. [ ] Write tests\n"

        invalid = run("done", "9", expected=1)
        assert "task number out of range" in invalid.stderr
        assert run("clear").stdout == "Cleared.\n"
        assert run("list").stdout == "No tasks.\n"
        assert database.read_text(encoding="utf-8") == ""

    print("Persistent todo CLI workflow passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
