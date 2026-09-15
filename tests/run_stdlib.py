"""End-to-end stdlib tests, isolated per process and GC backend."""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
import socket
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--filter", default="")
    args = parser.parse_args()
    exe = args.exe.resolve()
    cases = [
        ("stdlib_foundation", b"", b"foundation ok\n", ""),
        ("stdlib_system", b"", b"system ok\n", ""),
        ("stdlib_stdin", b"\r\nhello\r\nlast\r", b"stdin ok\n", ""),
        ("stdlib_readall", b"\r\n\0\x1a\x80\xff", b"A readall ok\n", ""),
        ("stdlib_bounds_fail", b"", b"", "list index out of bounds"),
        ("stdlib_capacity_fail", b"", b"", "array capacity out of range"),
        ("stdlib_capacity_overflow_fail", b"", b"", "array capacity out of range"),
        ("stdlib_collections_complete", b"", b"collections ok\n", ""),
        ("stdlib_json", b"", b"json ok\n", ""),
        ("stdlib_iter_edges", b"", b"iterator edges ok\n", ""),
        ("stdlib_net", b"", b"net api ok\n", ""),
        ("stdlib_failures:index", b"0", b"", "list index out of bounds"),
        ("stdlib_failures:split", b"1", b"", "split separator must not be empty"),
        ("stdlib_failures:absolute", b"2", b"", "absolute value overflow"),
        ("stdlib_failures:exponent", b"3", b"", "negative integer exponent"),
        ("stdlib_failures:multiply", b"4", b"", "integer multiplication overflow"),
        ("stdlib_failures:random", b"5", b"", "random bound out of range"),
        ("stdlib_failures:iterator", b"6", b"", "negative iterator count"),
        ("stdlib_failures:unicode", b"7", b"", "invalid Unicode scalar"),
        ("stdlib_failures:unwrap", b"8", b"", "null unwrap or unreachable code"),
    ]
    failed = 0
    for name, stdin, stdout, diagnostic in cases:
        if args.filter and args.filter not in name:
            continue
        for gc in ("nogc", "arc", "aarc"):
            with tempfile.TemporaryDirectory(prefix="loxis-stdlib-") as work:
                env = dict(os.environ, LOXIS_STDLIB=str(ROOT / "stdlib"),
                           LOXIS_STDLIB_TEST="present")
                command = [str(exe), str(ROOT / "src/test" / (name.split(":")[0] + ".lx")),
                           "10000000", "--gc=" + gc, "--slots=65536", "--", "alpha", "two words"]
                check_gc = not diagnostic and gc != "nogc"
                if check_gc:
                    command.insert(5, "--gc-stats")
                try:
                    result = subprocess.run(command, input=stdin, capture_output=True,
                                            cwd=work, env=env, timeout=30)
                    actual = result.stdout.replace(b"\r\n", b"\n")
                    error = result.stderr.decode("utf-8", errors="replace")
                    clean_error = not error
                    if check_gc:
                        clean_error = (error.startswith("gc=") and
                                       re.search(r"\binvalidOps=0\b", error) is not None and
                                       re.search(r"\blive=0\b", error) is not None and
                                       "Runtime error" not in error and "[gc-check]" not in error)
                    ok = (result.returncode != 0 and diagnostic in error) if diagnostic else (
                        result.returncode == 0 and actual == stdout and clean_error)
                    if not ok:
                        print(f"FAIL {name}/{gc}: exit={result.returncode}\n{actual!r}\n{error}")
                        failed += 1
                    else:
                        print(f"PASS {name}/{gc}")
                except subprocess.TimeoutExpired:
                    print(f"FAIL {name}/{gc}: timed out")
                    failed += 1
    if not args.filter or "report" in args.filter:
        for gc in ("nogc", "arc", "aarc"):
            with tempfile.TemporaryDirectory(prefix="loxis-report-") as work:
                source = Path(work) / "input.json"
                output = Path(work) / "output.json"
                values = [1, True, None, "Unicode: \u4e2d\U0001f63a",
                          {"large": 9223372036854775808, "nested": [[], {}, -2.5e12, 0.0]}]
                source.write_text(json.dumps(values, ensure_ascii=False), encoding="utf-8")
                result = subprocess.run([str(exe), str(ROOT / "examples/json_report.lx"),
                                         "10000000", "--gc=" + gc, "--", str(source), str(output)],
                                        capture_output=True, cwd=work,
                                        env=dict(os.environ, LOXIS_STDLIB=str(ROOT / "stdlib")), timeout=30)
                ok = (result.returncode == 0 and not result.stderr and output.exists() and
                      json.loads(output.read_text(encoding="utf-8")) == {"count": len(values), "values": values})
                print(f"{'PASS' if ok else 'FAIL'} json report/{gc}")
                if not ok:
                    print(result.stdout, result.stderr)
                    failed += 1
    if not args.filter or "playground" in args.filter:
        samples = [
            ("default", None, {"loxis": 3, "makes": 2, "fun": 2, "code": 1, "lists": 1}),
            ("custom", "cat\tcat\ncode", {"cat": 2, "code": 1}),
            ("empty", "", {}),
        ]
        for name, sample, expected in samples:
            for gc in ("nogc", "arc", "aarc"):
                with tempfile.TemporaryDirectory(prefix="loxis-playground-") as work:
                    output = Path(work) / "stdlib-demo.json"
                    command = [str(exe), str(ROOT / "examples/stdlib_demo.lx"),
                               "10000000", "--gc=" + gc]
                    if sample is not None:
                        output = Path(work) / "custom report.json"
                        command += ["--", sample, str(output)]
                    result = subprocess.run(command, capture_output=True, cwd=work,
                                            env=dict(os.environ, LOXIS_STDLIB=str(ROOT / "stdlib")),
                                            timeout=30)
                    ok = result.returncode == 0 and not result.stderr and output.exists()
                    if ok:
                        rows = json.loads(output.read_text(encoding="utf-8"))
                        ok = (dict(rows) == expected and len(rows) == len(expected)
                              and [row[1] for row in rows] == sorted(expected.values(), reverse=True)
                              and b"Saved and read back:" in result.stdout)
                    print(f"{'PASS' if ok else 'FAIL'} playground {name}/{gc}")
                    if not ok:
                        print(result.stdout, result.stderr)
                        failed += 1
    if not args.filter or "net_loopback" in args.filter:
        for gc in ("nogc", "arc", "aarc"):
            env = dict(os.environ, LOXIS_STDLIB=str(ROOT / "stdlib"))

            # Loxis client -> Python TCP peer.
            server = socket.socket()
            server.bind(("127.0.0.1", 0))
            server.listen(1)
            port = server.getsockname()[1]
            def serve():
                conn, _ = server.accept()
                with conn:
                    conn.sendall(b"python: " + conn.recv(4096))
                server.close()
            worker = threading.Thread(target=serve, daemon=True)
            worker.start()
            result = subprocess.run(
                [str(exe), str(ROOT / "examples/tcp_client.lx"), "10000000", "--gc=" + gc,
                 "--", "127.0.0.1", str(port), "ping"],
                capture_output=True, cwd=ROOT, env=env, timeout=15)
            worker.join(timeout=5)
            if worker.is_alive():
                server.close()
            ok = result.returncode == 0 and result.stdout.replace(b"\r\n", b"\n") == b"python: ping\n" and not result.stderr
            print(f"{'PASS' if ok else 'FAIL'} tcp client/{gc}")
            if not ok:
                print(result.stdout, result.stderr)
                failed += 1

            # Python TCP peer -> Loxis listener.
            probe = socket.socket()
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
            probe.close()
            process = subprocess.Popen(
                [str(exe), str(ROOT / "examples/tcp_echo_server.lx"), "10000000", "--gc=" + gc,
                 "--", str(port)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=ROOT, env=env)
            peer = None
            for _ in range(100):
                try:
                    peer = socket.create_connection(("127.0.0.1", port), timeout=.1)
                    break
                except OSError:
                    time.sleep(.03)
            response = b""
            if peer:
                with peer:
                    peer.sendall(b"pong")
                    response = peer.recv(4096)
            try:
                stdout, stderr = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, stderr = process.communicate()
            ok = process.returncode == 0 and response == b"echo: pong" and not stderr
            print(f"{'PASS' if ok else 'FAIL'} tcp listener/{gc}")
            if not ok:
                print(stdout, stderr, response)
                failed += 1

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
