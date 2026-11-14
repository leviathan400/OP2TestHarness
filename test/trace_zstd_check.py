#!/usr/bin/env python3
"""Check of run.py's --trace-zstd (tracezstd.py): trace.jsonl becomes trace.jsonl.zst with the same content, through
Python's compression.zstd (3.14+) and through the zstd command line tool, and a failure keeps the plain file.
Needs neither the game nor Wine:

    python3 test/trace_zstd_check.py
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))
import tracezstd  # noqa: E402

TRACE = b"".join(b'{"tick":%d,"units":[{"id":1,"px":%d,"py":976,"hp":300}]}\n' % (t, 1680 + t) for t in range(5000))


def decompressed(zst: Path) -> bytes:
    try:
        from compression import zstd
        with zstd.open(zst, "rb") as f:
            return f.read()
    except ImportError:
        return subprocess.run(["zstd", "-dc", str(zst)], check=True, capture_output=True).stdout


def case(name, backends, expect_ok):
    with tempfile.TemporaryDirectory() as d:
        out = Path(d)
        (out / "trace.jsonl").write_bytes(TRACE)
        saved = tracezstd._with_module, tracezstd._with_cli
        tracezstd._with_module, tracezstd._with_cli = backends
        try:
            ok = tracezstd.compress_trace(out)
        finally:
            tracezstd._with_module, tracezstd._with_cli = saved
        names = sorted(p.name for p in out.iterdir())
        if expect_ok:
            assert ok and names == ["trace.jsonl.zst"], (name, ok, names)
            assert decompressed(out / "trace.jsonl.zst") == TRACE, name
            assert len(TRACE) > (out / "trace.jsonl.zst").stat().st_size * 5, name
        else:
            assert not ok and names == ["trace.jsonl"], (name, ok, names)
            assert (out / "trace.jsonl").read_bytes() == TRACE, name
    print(f"{name}: OK")


def main() -> int:
    def no_module(*_):
        raise ImportError

    def corrupt(plain, zst):  # A read-back that differs from the plain file.
        zst.write_bytes(b"")
        return "0" * 64

    def fail(*_):
        raise RuntimeError("boom")

    real = tracezstd._with_module, tracezstd._with_cli
    try:
        import compression.zstd  # noqa: F401
        case("compression.zstd", real, True)
    except ImportError:
        print("compression.zstd: skipped (Python before 3.14)")
    if shutil.which("zstd"):
        case("zstd command", (no_module, real[1]), True)
    else:
        print("zstd command: skipped (not installed)")
    case("read-back differs", (corrupt, fail), False)
    case("no backend", (no_module, fail), False)
    with tempfile.TemporaryDirectory() as d:
        assert tracezstd.compress_trace(Path(d)) is False and not list(Path(d).iterdir())
    print("no trace.jsonl: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
