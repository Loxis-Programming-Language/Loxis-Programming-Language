"""Check that internal runtime operations require a file-local explicit opt-in."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    exe = parser.parse_args().exe.resolve()
    env = dict(os.environ, LOXIS_STDLIB=str(ROOT / "stdlib"))
    cases = {
        "heap": ('fun main() { halloc(9) }', "internal runtime API"),
        "alias": ('fun main() { val convert = i2s; print(convert(1)) }', "internal runtime API"),
        "native": ('fun main() { print(nativeFileExists("missing")) }', "internal runtime API"),
        "helper": ('fun main() { __loxis_arrayNew(2) }', "internal runtime API"),
        "package_spoof": ('package loxis\nfun main() { halloc(9) }', "internal runtime API"),
        "transitive": ('import helper\nfun main() { halloc(9) }', "internal runtime API"),
        "transitive_namespace": ('import helper\nfun main() { Unsafe.arrayNew(2) }', "internal runtime API"),
        "retired_list": ('import loxis.collections.list\nfun main() { listMap<int,int>(list<int>(), { x -> x }) }', "listMap"),
        "retired_conversion": ('import loxis.text\nfun main() { intToString(1) }', "intToString"),
        "byte_range": ('import loxis.text\nfun main() { Text.fromByte(256) }', "byte out of range"),
    }
    with tempfile.TemporaryDirectory(prefix="loxis-api-") as work:
        Path(work, "helper.lx").write_text('import loxis.unsafe\nfun helper() -> int { return halloc(9) }\n', encoding="utf-8")
        for name, (source, diagnostic) in cases.items():
            file = Path(work, name + ".lx")
            file.write_text(source + "\n", encoding="utf-8")
            result = subprocess.run([str(exe), str(file)], cwd=work, env=env, capture_output=True, text=True, timeout=30)
            assert result.returncode != 0 and diagnostic in result.stderr, (name, result.returncode, result.stderr)
            print("PASS " + name)
        for gc in ("nogc", "arc", "aarc"):
            public = Path(work, "public.lx")
            public.write_text('import loxis.refl\nclass Payload(val n: int) {}\nclass Other() {}\nfun main() { require(Reflection.typeOf(Payload(1)) == Reflection.typeOf(Payload(2)), "same type"); require(Reflection.typeOf(Payload(1)) != Reflection.typeOf(Other()), "different type"); print("reflection ok") }\n', encoding="utf-8")
            result = subprocess.run([str(exe), str(public), "--gc=" + gc], cwd=work, env=env, capture_output=True, text=True, timeout=30)
            assert result.returncode == 0 and result.stdout == "reflection ok\n" and not result.stderr, (gc, result.stdout, result.stderr)
            print("PASS scoped reflection/" + gc)
            file = Path(work, "unsafe.lx")
            file.write_text('import loxis.unsafe\nfun main() { val h = halloc(9); hset(h, 0, 11); print(hread(h, 0)); hfree(h) }\n', encoding="utf-8")
            result = subprocess.run([str(exe), str(file), "--gc=" + gc], cwd=work, env=env, capture_output=True, text=True, timeout=30)
            assert result.returncode == 0 and result.stdout == "11\n" and not result.stderr, (gc, result.stdout, result.stderr)
            print("PASS explicit unsafe/" + gc)


if __name__ == "__main__":
    main()
