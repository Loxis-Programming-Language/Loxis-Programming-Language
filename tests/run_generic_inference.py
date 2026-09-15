"""Generic inference regressions through type checking, specialization and VM."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    args = parser.parse_args()
    exe = args.exe.resolve()
    cases = {
        "generic_inference": "42\nhello\n7\n9\n6\n5\n50\n12\noverload\n8\n11\n15\n16\n7\n",
        "generic_inference_conflict_fail": None,
        "generic_inference_missing_fail": None,
        "generic_inference_constructor_fail": None,
        "generic_inference_method_fail": None,
        "generic_inference_null_fail": None,
        "generic_inference_lambda_fail": None,
    }
    for name, expected in cases.items():
        for gc in ("nogc", "arc", "aarc"):
            with tempfile.TemporaryDirectory(prefix="loxis-inference-") as work:
                result = subprocess.run(
                    [str(exe), str(ROOT / "src/test" / (name + ".lx")), "--gc=" + gc],
                    capture_output=True, text=True, encoding="utf-8", cwd=work,
                    env=dict(os.environ, LOXIS_STDLIB=str(ROOT / "stdlib")), timeout=30)
            if expected is None:
                ok = result.returncode != 0 and "cannot infer generic type arguments" in result.stderr
            else:
                ok = result.returncode == 0 and result.stdout == expected and not result.stderr
            if not ok:
                raise AssertionError(f"{name}/{gc}: exit={result.returncode}\n{result.stdout}\n{result.stderr}")
            print(f"PASS {name}/{gc}")

if __name__ == "__main__":
    main()
