#!/usr/bin/env python3
"""Run an Outpost 2 mission headless under Xvfb via the Harness op2ext module and collect its output.

Example:
    ./run.sh --mission cHello.dll --ticks 3000 --screenshot-every 500 --out /tmp/op2run

    ./run.sh --scenario $ORACLE/scenarios/line_fast.json --out /tmp/op2scn

Outputs in --out: telemetry.jsonl, trace.jsonl, gamedata.json, shot_<tick>.png, harness.log, wine.log, summary.json.
"""
import argparse
import fcntl
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import prefixes  # noqa: E402

# Harness runs use their own Wine prefix when one exists (prefix-harness/, a copy of prefix/), so they get their own
# wineserver and can't steal focus from, or swap DLLs under, a game being played from prefix/. OP2_HARNESS_PREFIX
# (an absolute path) chooses another (prefixes.py).
PLAY_PREFIX = ROOT / "prefix"
try:
    PREFIX = prefixes.harness_prefix(ROOT)
except ValueError as e:
    raise SystemExit(f"run.py: {e}") from None
GAME_DIR = PREFIX / "drive_c" / "GOG Games" / "Outpost 2"
OPU_DIR = GAME_DIR / "OPU"


def sync_missions_from_play_prefix():
    """Mission DLLs get installed into prefix/ (missions/Makefile); mirror OPU/maps into the harness prefix."""
    if PREFIX.resolve() == PLAY_PREFIX.resolve():
        return
    src = PLAY_PREFIX / "drive_c" / "GOG Games" / "Outpost 2" / "OPU" / "maps"
    if src.is_dir():
        shutil.copytree(src, OPU_DIR / "maps", dirs_exist_ok=True)


def winepath(flag: str, *paths: str) -> list:
    """`winepath FLAG PATHS...` in the harness prefix: one converted path per argument, or a clean exit when winepath is
    missing, hangs, fails or answers short."""
    env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG="-all")
    try:
        r = subprocess.run(["winepath", flag, *paths], env=env, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        sys.exit(f"run.py: winepath {flag} failed: {e}")
    out = [l for l in r.stdout.splitlines() if l]
    if r.returncode != 0 or len(out) != len(paths):
        sys.exit(f"run.py: winepath {flag} {' '.join(paths)} exited {r.returncode}: {out} {r.stderr.strip()[-300:]}")
    return out


def fit_script_name(mission: str) -> str:
    """The mission as the game's GameStartInfo.scriptName (char[32]) can hold it. A DLL given by its path in the game
    folder (OPU/maps/<folder>/<stem>.dll: the multiplayer start reads the descriptor by that path) longer than 31
    characters is named by Wine's 8.3 short path instead; PrepareGame fails on the truncated name (ml4_42 and 19 other
    community maps, 2026-11-04)."""
    rel = mission.replace("/", "\\")
    if len(rel) <= 31 or "\\" not in rel:
        return rel
    (game,) = winepath("-w", str(GAME_DIR))
    short_game, short_full = winepath("-s", game, game.rstrip("\\") + "\\" + rel)
    if not short_full.lower().startswith(short_game.rstrip("\\").lower() + "\\"):
        sys.exit(f"run.py: no short path for {mission} under {short_game} (winepath -s: {short_full})")
    short = short_full[len(short_game.rstrip("\\")) + 1:]
    if len(short) > 31:
        sys.exit(f"run.py: {mission} doesn't fit the game's 31-character script name even as {short}")
    return short


# GameTermReasons from TethysAPI Game/GameStartInfo.h.
END_REASONS = {0: "Running", 3: "MissionAccomplished", 4: "MissionFailed", 6: "MissionAborted", 7: "Victory",
               8: "Defeat", 9: "ControlTerminated", 12: "GameOver"}


# CellType names from TethysAPI Game/MapImpl.h, for scenario "setcell" overrides given by name.
CELL_TYPES = ["FastPassible1", "Impassible2", "SlowPassible1", "SlowPassible2", "MediumPassible1", "MediumPassible2",
              "Impassible1", "FastPassible2", "NorthCliffs", "CliffsHighSide", "CliffsLowSide", "VentsAndFumaroles",
              *[f"ZPad{i}" for i in range(12, 21)], "DozedArea", "Rubble", "NormalWall", "MicrobeWall", "LavaWall",
              *[f"Tube{i}" for i in range(6)]]

# The scenario mission ships next to harness.dll rather than in OPU/maps, so it never shows up in the game's lists.
# The game reads a mission's MapName export from the DLL file itself, so each map gets a patched copy there.
MAPNAME_MARKER = b"@@OP2H_MAPNAME_SLOT@@"


TECHTREE_DEFAULT = b"multitek.txt\0"


def scenario_mission(map_name: str, techtree: str = "multitek.txt") -> str:
    """Returns the mission name (relative to the game folder) of zScenario.dll patched to load `map_name`, and the tech
    tree `techtree` (its TechtreeName export, 13 bytes: `edentek.txt` and `ply_tek.txt` fit)."""
    src = OPU_DIR / "Harness" / "zScenario.dll"
    if not src.exists():
        sys.exit("scenario mission not installed: run `make install`")
    data = bytearray(src.read_bytes())
    at = data.find(MAPNAME_MARKER) - len(b"cm01.map\0")
    if at < 0 or data[at:at + 9] != b"cm01.map\0":
        sys.exit("zScenario.dll has no MapName slot; rebuild with `make install`")
    name = map_name.encode("ascii")
    if len(name) > 63:
        sys.exit(f"map name too long for the mission's MapName: {map_name}")
    data[at:at + 64] = name + bytes(64 - len(name))
    stem = Path(map_name).stem.lower()
    if techtree.lower() != "multitek.txt":
        tree = techtree.encode("ascii")
        at = data.find(TECHTREE_DEFAULT)
        if at < 0 or data.count(TECHTREE_DEFAULT) != 1 or len(tree) > 12:
            sys.exit(f"can't patch the scenario mission's tech tree to {techtree}")
        data[at:at + 13] = tree + bytes(13 - len(tree))
        stem = f"{stem}_{Path(techtree).stem.lower()}"
    # GameStartInfo::scriptName holds 31 chars + NUL: "OPU\\Harness\\" + "zs_" + up to 12 + ".dll".
    if len(stem) > 12 or not re.fullmatch(r"[a-z0-9_-]+", stem):
        stem = format(zlib.crc32(map_name.encode()), "08x")
    dll = f"zs_{stem}.dll"
    dst = OPU_DIR / "Harness" / dll
    if not dst.exists() or dst.read_bytes() != data:
        tmp = dst.with_suffix(f".{os.getpid()}.tmp")
        tmp.write_bytes(data)
        os.replace(tmp, dst)  # Atomic, so parallel runs on the same map never see a partial file.
    return f"OPU\\Harness\\{dll}"


def scenario_script(scn: dict) -> str:
    """Translates a scenario JSON (ENGINE_PLAN.md format) into a harness script in file tile coordinates."""
    lines = ["# generated from a scenario by run.py", "0 coords file"]
    for c in scn.get("setcell", []):
        cell = c["cell"] if isinstance(c["cell"], int) else CELL_TYPES.index(c["cell"])
        x1, y1, x2, y2 = c["rect"]
        lines.append(f"0 setcell {x1} {y1} {x2} {y2} {cell}")
    for s in scn.get("spawn", []):
        x, y = s["tile"]
        lines.append(f"0 spawn {s['type']} {s.get('owner', 0)} {x} {y} {s.get('weapon') or '-'} {s.get('dir', 0)}")
    # Centre the view (for screenshots) on the scenario's "view" tile, else on the spawns.
    spawns = scn.get("spawn", [])
    view = scn.get("view") or (spawns and [sum(s["tile"][i] for s in spawns) // len(spawns) for i in (0, 1)])
    if view:
        lines.append(f"0 view {view[0]} {view[1]}")
    for o in scn.get("orders", []):
        cmd, unit, tick = o["cmd"].lower(), f"@{o['unit']}", o["tick"]
        # Waypoints are [x, y] or [x, y, range] (op2-sim's Waypoint {tile, range} is accepted too).
        points = [[w["tile"][0], w["tile"][1], w.get("range", 0)] if isinstance(w, dict) else w
                  for w in o.get("waypoints") or ([o["tile"]] if "tile" in o else [])]
        wps = " ".join(",".join(str(v) for v in w) for w in points)
        if cmd == "move" and "waypoints" in o:
            lines.append(f"{tick} movewp {unit} {wps}")
        elif cmd in ("move", "dock", "dockeg"):
            lines.append(f"{tick} {cmd} {unit} {o['tile'][0]} {o['tile'][1]}")
        elif cmd == "standground":
            lines.append(f"{tick} standground {unit} {wps}")
        elif cmd == "patrol":
            lines.append(f"{tick} patrol {unit} {o.get('loop', 0)} {wps}")
        elif cmd == "attack" and "tile" in o:
            lines.append(f"{tick} attackground {unit} {o['tile'][0]} {o['tile'][1]}")
        elif cmd in ("attack", "guard"):
            lines.append(f"{tick} {cmd} {unit} @{o['target']}")
        elif cmd == "lights":
            lines.append(f"{tick} lights {unit} {int(bool(o['on']))}")
        elif cmd in ("stop", "selfdestruct"):
            lines.append(f"{tick} {cmd} {unit}")
        else:
            sys.exit(f"scenario: unsupported order {o}")
    # Raw script lines (e.g. input verbs: "20 click tile 40 30"), still in file coordinates.
    lines += scn.get("script", [])
    return "\n".join(lines) + "\n"


def win_path(p: Path) -> str:
    return "Z:" + str(p).replace("/", "\\")


def check_installed() -> None:
    ini = (OPU_DIR / "outpost2.ini").read_text(errors="replace")
    if not (OPU_DIR / "Harness" / "harness.dll").exists() or "Harness=yes" not in ini:
        sys.exit("harness module not installed: run `make install`")
    # All checkouts share one installed copy. Builds are reproducible (no PE timestamps), so a differing file means
    # another checkout installed its own version, or this checkout's changes aren't installed yet.
    for dll in ("harness.dll", "zScenario.dll"):
        built = Path(__file__).resolve().parent / "build" / dll
        if built.exists() and built.read_bytes() != (OPU_DIR / "Harness" / dll).read_bytes():
            print(f"run.py: warning: the installed {dll} differs from this checkout's build/{dll}; the run uses "
                  "the installed one (another checkout's, or yours needs `make install`)", file=sys.stderr)


def start_xvfb(width: int, height: int) -> tuple[subprocess.Popen, str]:
    r, w = os.pipe()
    proc = subprocess.Popen(
        ["Xvfb", "-displayfd", str(w), "-screen", "0", f"{width + 256}x{height + 256}x24", "-nolisten", "tcp"],
        pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.close(w)
    with os.fdopen(r) as f:
        num = f.readline().strip()
    if not num:
        proc.kill()
        sys.exit("Xvfb failed to start")
    return proc, ":" + num


def grab_screen(display: str, path: Path) -> None:
    try:
        from PIL import ImageGrab
        ImageGrab.grab(xdisplay=display).save(path)
    except Exception as e:  # Diagnostics only.
        print(f"screen grab failed: {e}", file=sys.stderr)


def convert_shots(out: Path, keep_bmp: bool) -> int:
    try:
        from PIL import Image
    except ImportError:
        print("PIL not available; leaving BMP screenshots", file=sys.stderr)
        return len(list(out.glob("shot_*.bmp")))
    n = 0
    for bmp in sorted(out.glob("shot_*.bmp")):
        Image.open(bmp).save(bmp.with_suffix(".png"), optimize=False)
        if not keep_bmp:
            bmp.unlink()
        n += 1
    return n


def lock_saves(holder: str):
    """Takes OPU/saves's lock (shell.py --clean-saves moves the folder aside under it), saying who holds it while it
    waits, and writes `holder` into it for the next one. Returns the open lock file: the lock lasts until it closes."""
    saves = OPU_DIR / "saves"
    saves.mkdir(parents=True, exist_ok=True)
    f = open(saves.parent / "saves.harness-lock", "a+")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        f.seek(0)
        print(f"waiting for the saves folder's lock, held by: {f.read().strip() or 'an older harness run'}", file=sys.stderr)
        fcntl.flock(f, fcntl.LOCK_EX)
    f.seek(0)
    f.truncate()
    f.write(f"pid {os.getpid()} since {time.strftime('%H:%M:%S')}: {holder}\n")
    f.flush()
    return f


def end_game(game: subprocess.Popen, out_win: str, desktop: bool) -> None:
    """Kills what is left of a run's game: its process group while it is alive (prefix-harness's wineserver and its
    services run in their own sessions, so they aren't in it), and with a virtual desktop also this run's Outpost2.exe
    and desktop explorer, which Wine starts outside the group (found by the run's OP2H_OUT in their environment)."""
    if game.poll() is None:
        try:
            os.killpg(game.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if not desktop:
        return
    marker = f"OP2H_OUT={out_win}".encode()
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            cmd = (proc / "cmdline").read_bytes()
            if b"Outpost2.exe" not in cmd or b"wineserver" in cmd or marker not in (proc / "environ").read_bytes().split(b"\0"):
                continue
            os.kill(int(proc.name), signal.SIGKILL)
        except (OSError, ProcessLookupError):
            continue


def keep_previous_recording(out: Path) -> None:
    """A new recording session starts over in OUT: the previous session's recording and telemetry are kept beside it
    as recording-<time>.txt and telemetry-<time>.jsonl (a session resumed by loading a save gets its history back from
    the save's sidecar anyway)."""
    rec = out / "recording.txt"
    if not rec.exists():
        return
    stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(rec.stat().st_mtime))
    for name in ("recording.txt", "telemetry.jsonl", "harness.log"):
        f = out / name
        if f.exists():
            f.rename(out / f"{f.stem}-{stamp}{f.suffix}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mission", help="mission DLL file name, e.g. cHello.dll (searched like the game does)")
    ap.add_argument("--scenario", type=Path,
                    help="scenario JSON (map, seed, ticks, spawn, orders); runs the built-in scenario mission")
    ap.add_argument("--out", required=True, type=Path, help="output directory (created; existing outputs overwritten)")
    ap.add_argument("--ticks", type=int, help="stop after this game tick (100 ticks = 1 mark); 0 = never. "
                    "Default 3000, or the scenario's ticks")
    ap.add_argument("--screenshot-every", type=int, default=0, metavar="N", help="screenshot every N ticks (0 = off)")
    ap.add_argument("--telemetry-every", type=int, metavar="N",
                    help="telemetry every N ticks (0 = off); default 100, or the scenario's telemetryEvery")
    ap.add_argument("--trace-every", type=int, metavar="N",
                    help="trace.jsonl (sub-tile positions) every N ticks; default 1 with --scenario, else 0 (off)")
    ap.add_argument("--trace-raw", action="store_true", help="add raw MapObject/PathContext bytes to trace lines")
    ap.add_argument("--trace-from", type=int, default=0, metavar="TICK",
                    help="no trace lines before this tick, rounded up to a multiple of --trace-every (a long "
                         "--trace-raw run's trace stays small)")
    ap.add_argument("--trace-map", action="store_true",
                    help="add the map tiles changed since the previous trace line (default: the scenario's traceMap)")
    ap.add_argument("--campaign", type=Path, metavar="FILE",
                    help="start as a campaign mission with this carry-over (key value lines, see harness.cpp)")
    ap.add_argument("--audio", action="store_true",
                    help="audio log: every sound, Savant line, message and music playlist the game asks for (also "
                         "scenario \"audio\"), see README \"Audio log\"")
    ap.add_argument("--state", action="store_true",
                    help="session state: events.jsonl through the run and state.json at the end (README \"Session state\")")
    ap.add_argument("--stop", metavar="KINDS",
                    help="end the run at the first event of these kinds (comma separated: new, enemy, gone, hit, idle, "
                         "tech, objective, msg, disaster, end, any; `kind:<unit id>`) after --stop-from; implies --state")
    ap.add_argument("--stop-from", type=int, default=0, metavar="TICK", help="--stop only counts events after this tick")
    ap.add_argument("--eco", action="store_true",
                    help="economy telemetry: colony state per player, building/vehicle internals (also scenario \"eco\")")
    ap.add_argument("--speed", type=int, default=10, help="in-game speed setting 1-10 (only matters with --no-turbo)")
    ap.add_argument("--no-turbo", action="store_true", help="keep the game's frame limiter (real-time at --speed)")
    ap.add_argument("--presents", action="store_true", help="log pane presents per game tick and real second to present.log")
    ap.add_argument("--rng-log", metavar="FROM-TO",
                    help="log each gameplay RNG draw in these ticks (or one tick) with its caller's address to rng.log")
    ap.add_argument("--draw-every", type=int, default=0, metavar="N",
                    help="turbo: render every N ticks (default: only right before screenshots; 1 = always)")
    ap.add_argument("--seed", type=int, help="random number seed (default 1, or the scenario's seed)")
    ap.add_argument("--difficulty", type=int, choices=(0, 1, 2),
                    help="0 easy, 1 normal, 2 hard (default 1, or the scenario's difficulty)")
    ap.add_argument("--players", type=int, choices=range(1, 7), default=1, metavar="N",
                    help="human players in a multiplayer mission start (2-6); default 1 = existing single-player setup")
    ap.add_argument("--script", type=Path,
                    help="scripted command file (see README); with --scenario, appended to the scenario's script")
    ap.add_argument("--bot", type=Path, help="bot DLL exporting HarnessBotTick(int tick)")
    ap.add_argument("--size", default="1024x768", help="game client area WxH")
    ap.add_argument("--timeout", type=float, default=600, help="wall-clock seconds before the game is killed")
    ap.add_argument("--keep-bmp", action="store_true", help="keep raw BMP screenshots next to the PNGs")
    ap.add_argument("--display", help="use this X display instead of starting a private Xvfb (e.g. to watch)")
    ap.add_argument("--record", action="store_true",
                    help="record a person playing (README \"Recording your own play\"): real time, the real mouse and "
                         "keys, sound on, no tick limit or timeout; every order goes to OUT/recording.txt, a replayable "
                         "script; saves keep the recording beside them. Needs --display or $DISPLAY")
    ap.add_argument("--desktop", metavar="WxH",
                    help="run the game in a Wine virtual desktop of this size (dialogs stay inside it, for gamescope)")
    ap.add_argument("--silent", action="store_true", help="with --record: sound and music off, no audio device")
    ap.add_argument("--resume", metavar="SAVE",
                    help="load this checkpoint save (OPU/saves/SAVE, or a path) at the first game cycle and play the "
                         "script from its tick: every script line must be at that tick or later (README \"Checkpoints\")")
    ap.add_argument("--checkpoint", metavar="SAVE",
                    help="when the run stops with the game going on (--ticks, --stop), save it to SAVE first, with the "
                         "mission DLL's state beside it (README \"Checkpoints\")")
    ap.add_argument("--fast", action="store_true",
                    help="with --record, for tests of the recording itself: turbo on a private Xvfb, nobody playing "
                         "(the script's `savegame`/`loadgame` lines stand in for the dialogs)")
    args = ap.parse_args()
    scn = json.loads(args.scenario.read_text()) if args.scenario else None
    if bool(args.mission) == bool(scn):
        ap.error("give exactly one of --mission and --scenario")
    if args.record:
        if scn:
            ap.error("--record plays a mission, not a scenario")
        if PREFIX.resolve() == PLAY_PREFIX.resolve():
            ap.error(f"--record only runs in the harness's own prefix, and {PREFIX} is the play prefix "
                     "(create prefix-harness/, a copy of prefix/, or point OP2_HARNESS_PREFIX at another copy)")
        if not args.fast:
            args.display = args.display or os.environ.get("DISPLAY")
            if not args.display:
                ap.error("--record needs a display to play on: --display :N, or $DISPLAY (gamescope sets it)")
            args.no_turbo = True
        args.ticks = 0 if args.ticks is None else args.ticks
        if args.fast:
            args.silent = True
        elif args.timeout == 600:
            args.timeout = 0
    if args.ticks is None:
        args.ticks = scn.get("ticks", 3000) if scn else 3000
    if args.seed is None:
        args.seed = scn.get("seed", 1) if scn else 1
    if args.trace_every is None:
        args.trace_every = scn.get("traceEvery", 1) if scn else 0
    if args.telemetry_every is None:
        args.telemetry_every = scn.get("telemetryEvery", 100) if scn else 100
    if args.difficulty is None:
        args.difficulty = scn.get("difficulty", 1) if scn else 1
    args.eco = args.eco or bool(scn and scn.get("eco"))
    args.trace_map = args.trace_map or bool(scn and scn.get("traceMap"))
    args.audio = args.audio or bool(scn and scn.get("audio"))
    args.state = args.state or bool(args.stop)

    sync_missions_from_play_prefix()
    check_installed()
    width, height = (int(v) for v in args.size.lower().split("x"))
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    saves_lock = None  # Held (open) until the run ends.
    if args.record:
        keep_previous_recording(out)
        # The terminal or gamescope going away (SIGTERM, SIGHUP) ends the game too (the `except` around the wait).
        for sig in (signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, lambda signum, _frame: sys.exit(128 + signum))
    # The saves (and their recordings) stay put while someone plays: shell.py --clean-saves moves the folder aside, under
    # this lock (test/record_check.py holds it itself and says so).
    if args.record and not os.environ.get("OP2H_SAVES_LOCK_HELD"):
        saves_lock = lock_saves(f"run.py --record --mission {args.mission} --out {out}")
    for old in [*out.glob("shot_*.bmp"), *out.glob("shot_*.png"), out / "telemetry.jsonl", out / "harness.log",
                out / "timeout_screen.png", out / "summary.json", out / "trace.jsonl", out / "gamedata.json", out / "audio.jsonl",
                out / "scenario_script.txt", out / "state.json", out / "events.jsonl", out / "present.log"]:
        old.unlink(missing_ok=True)
    mission = fit_script_name(args.mission) if args.mission else None
    if scn:
        mission = scenario_mission(scn.get("map", "cm01.map"), scn.get("techtree", "multitek.txt"))
        extra = args.script.read_text() if args.script else ""  # --script lines run after the scenario's own.
        args.script = out / "scenario_script.txt"
        args.script.write_text(scenario_script(scn) + extra)
        # Recorded with the seed and length actually used, so replaying the run's scenario.json reproduces it.
        (out / "scenario.json").write_text(json.dumps(dict(scn, seed=args.seed, ticks=args.ticks), indent=1) + "\n")

    xvfb, display = (None, args.display) if args.display else start_xvfb(width, height)
    env = dict(os.environ,
               DISPLAY=display,
               WINEPREFIX=str(PREFIX),
               WINEDEBUG="-all",
               # No audio device (silent, leaves the user's sound server alone); no winedbg crash dialog (a crash
               # ends the run instead of hanging it until --timeout).
               WINEDLLOVERRIDES="winepulse.drv=d;winealsa.drv=d;winedbg.exe=d",
               OP2H_MISSION=mission,
               OP2H_TRACE_EVERY=str(args.trace_every),
               OP2H_TRACE_RAW="1" if args.trace_raw else "0",
               OP2H_TRACE_FROM=str(args.trace_from),
               OP2H_TRACE_MAP="1" if args.trace_map else "0",
               OP2H_ECO="1" if args.eco else "0",
               OP2H_AUDIO="1" if args.audio else "0",
               OP2H_STATE="1" if args.state else "0",
               OP2H_STOP=args.stop or "",
               OP2H_STOP_FROM=str(args.stop_from),
               OP2H_OUT=win_path(out),
               OP2H_TICKS=str(args.ticks),
               OP2H_SHOT_EVERY=str(args.screenshot_every),
               OP2H_TELEM_EVERY=str(args.telemetry_every),
               OP2H_SPEED=str(args.speed),
               OP2H_TURBO="0" if args.no_turbo else "1",
               OP2H_PRESENTS="1" if args.presents else "0",
               OP2H_RNG_LOG=args.rng_log or "",
               OP2H_DRAW_EVERY=str(args.draw_every),
               OP2H_SEED=str(args.seed),
               OP2H_DIFFICULTY=str(args.difficulty),
               OP2H_PLAYERS=str(args.players),
               OP2H_WIDTH=str(width),
               OP2H_HEIGHT=str(height))
    if args.record:
        env["OP2H_RECORD"] = "1"
        if args.fast:
            env["OP2H_NOBODY"] = "1"  # Nobody to answer the game's questions: logged and answered OK, as in other runs.
        if args.silent:
            env["OP2H_SILENT"] = "1"
        else:
            env["WINEDLLOVERRIDES"] = "winedbg.exe=d"  # Sound on: the person playing hears the game.
    if args.script:
        env["OP2H_SCRIPT"] = win_path(args.script.resolve())
    if args.bot:
        env["OP2H_BOT"] = win_path(args.bot.resolve())
    if args.campaign:
        env["OP2H_CAMPAIGN"] = win_path(args.campaign.resolve())
    if args.resume:
        if args.record or scn:
            ap.error("--resume plays a mission's script on from a checkpoint (not a recording or a scenario)")
        env["OP2H_RESUME"] = args.resume if "/" not in args.resume else win_path(Path(args.resume).resolve())
    if args.checkpoint:
        if args.record or scn:
            ap.error("--checkpoint saves a mission's scripted run (not a recording or a scenario)")
        env["OP2H_CHECKPOINT"] = args.checkpoint if "/" not in args.checkpoint else win_path(Path(args.checkpoint).resolve())
    env.pop("WAYLAND_DISPLAY", None)  # Make sure Wine uses the X display we picked.

    start = time.monotonic()
    timed_out = False
    command = ["wine", "Outpost2.exe", "/OPU"]
    if args.desktop:
        # The program needs its full Windows path: `explorer /desktop=... Outpost2.exe` silently starts nothing.
        exe = "C:\\" + str((GAME_DIR / "Outpost2.exe").relative_to(PREFIX / "drive_c")).replace("/", "\\")
        command = ["wine", "explorer", f"/desktop=op2h{os.getpid()},{args.desktop}", exe, "/OPU"]
    with open(out / "wine.log", "w") as log:
        game = subprocess.Popen(command, cwd=GAME_DIR, env=env,
                                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = game.wait(timeout=args.timeout or None)
        except (KeyboardInterrupt, SystemExit):  # A recording's terminal closed (Ctrl+C, SIGTERM, SIGHUP).
            end_game(game, env["OP2H_OUT"], args.desktop)
            raise
        except subprocess.TimeoutExpired:
            timed_out = True
            grab_screen(display, out / "timeout_screen.png")  # Usually shows the modal dialog that hung the game.
            end_game(game, env["OP2H_OUT"], args.desktop)
            code = game.wait()
        if args.desktop:  # The game and the desktop's explorer can outlive `wine explorer`'s start.exe.
            end_game(game, env["OP2H_OUT"], True)
    elapsed = time.monotonic() - start
    if xvfb:
        xvfb.terminate()
        xvfb.wait()

    shots = convert_shots(out, args.keep_bmp)
    hlog = (out / "harness.log").read_text(errors="replace") if (out / "harness.log").exists() else ""
    last_tick, end = 0, None
    telem = out / "telemetry.jsonl"
    if telem.exists():
        for line in reversed(telem.read_text(errors="replace").splitlines()):
            try:
                last = json.loads(line)  # The final line can be truncated if the game crashed mid-write.
            except json.JSONDecodeError:
                continue
            last_tick, end = last["tick"], last["end"]
            break
    quit_line = next((l for l in hlog.splitlines() if l.startswith("quit at tick")), None)
    summary = {
        "platform": "wine",
        "mission": mission, "scenario": str(args.scenario) if scn else None, "exit_code": code, "timed_out": timed_out, "wall_seconds": round(elapsed, 1),
        "last_telemetry_tick": last_tick, "game_term_reason": END_REASONS.get(end, end), "screenshots": shots,
        "dialogs": [l[len("dialog: "):] for l in hlog.splitlines() if l.startswith("dialog: ")],
        "quit": quit_line, "ok": bool(quit_line) and not timed_out and code == 0,
    }
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
