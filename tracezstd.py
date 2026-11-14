"""Compress a run's trace.jsonl to trace.jsonl.zst (zstd level 3), for run.py's --trace-zstd.

The plain file is removed only after the compressed one has been read back and found identical, byte for byte; on any
failure the plain file stays and a warning says why. Uses Python 3.14's `compression.zstd` when there is one, else the
`zstd` command line tool. Traces reach gigabytes: everything streams.
"""
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

LEVEL = 3
CHUNK = 1 << 20


def _digest(stream) -> str:
    h = hashlib.sha256()
    while chunk := stream.read(CHUNK):
        h.update(chunk)
    return h.hexdigest()


def _with_module(plain: Path, zst: Path) -> str:
    """Compress with compression.zstd, return the sha256 of what reading `zst` back gives."""
    from compression import zstd  # Python 3.14+; the ImportError is the caller's cue for the command line tool.
    with open(plain, "rb") as src, zstd.open(zst, "wb", level=LEVEL) as dst:
        shutil.copyfileobj(src, dst, CHUNK)
    with zstd.open(zst, "rb") as back:
        return _digest(back)


def _with_cli(plain: Path, zst: Path) -> str:
    exe = shutil.which("zstd")
    if not exe:
        raise RuntimeError("no Python 3.14 compression.zstd and no zstd command on PATH")
    subprocess.run([exe, f"-{LEVEL}", "-q", "-f", "-o", str(zst), str(plain)], check=True)
    with subprocess.Popen([exe, "-dc", str(zst)], stdout=subprocess.PIPE) as p:
        digest = _digest(p.stdout)
    if p.returncode:
        raise RuntimeError(f"zstd -d failed ({p.returncode})")
    return digest


def compress_trace(out: Path) -> bool:
    """Replace out/trace.jsonl with out/trace.jsonl.zst. True when done; False (plain file kept) when there was
    nothing to do or it failed. Never raises."""
    plain, zst = out / "trace.jsonl", out / "trace.jsonl.zst"
    if not plain.exists():
        return False
    tmp = out / "trace.jsonl.zst.tmp"
    try:
        try:
            back = _with_module(plain, tmp)
        except ImportError:
            back = _with_cli(plain, tmp)
        with open(plain, "rb") as f:
            if back != _digest(f):
                raise RuntimeError("the compressed trace does not read back identical")
        tmp.replace(zst)
        plain.unlink()
        return True
    except (OSError, RuntimeError, subprocess.SubprocessError) as e:
        print(f"warning: --trace-zstd: keeping {plain.name}: {e}", file=sys.stderr)
        tmp.unlink(missing_ok=True)
        return False
