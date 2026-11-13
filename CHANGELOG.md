# Changelog

All notable changes to OP2TestHarness. Versions follow [Semantic Versioning](https://semver.org/); the version is in
`VERSION`, and each release is tagged `vX.Y.Z`. The rules below hold from 1.0.0 on: the contract is op2-rs's, which
its recordings already depend on.

## Versioning rules

op2-rs and other users replay recorded runs against this harness, and those recordings depend on **the contract**:

- **Environment and options:** the `OP2H_*` environment variables and the `run.sh` / `run.py` options.
- **Script verbs and their meaning:** README.md "Scripted input" and the sections after it.
- **Unit names:** `$NAME`, `as NAME` and `$TYPE@x,y`.
- **Output fields:** the telemetry and trace JSONL fields, `gamedata.json`, `start_info.json`, the audio log and the
  screenshots.
- **The scenario JSON format.**

How a change moves the version:

- **MAJOR:** a change to the contract that can change a recorded run's output or break an existing script or option:
  a renamed or removed variable, option, verb or field, a verb or field whose meaning changes, a different screenshot.
- **MINOR:** additions that leave every existing recording replaying identically: a new verb, option, variable or output
  field, a new launcher or platform (e.g. a Windows launcher).
- **PATCH:** fixes and changes that don't touch the contract: docs, build scripts, launcher fixes, crash fixes in paths no
  recording uses.

Every release notes its contract changes, if any, under **Contract**. A release is only made from a commit whose
`harness.dll` replays op2-rs's recorded runs identically (op2-rs's `make accuracy`, 0 failures) unless its changelog
says which runs change and why.

## Unreleased

**Added** (MINOR: every existing recording replays identically)
- `netgame.py --lobby-chat "HOST|JOINER"`: the host and the first joiner chat in GAME SETTINGS once every joiner is
  ready; each machine shoots and dumps its lobby after each line.
- `netgame.py --join-ticks T`: the first joiner plays only to tick T (its script leaves the game there: End Mission, a
  quit), then waits for the host to end.
- `netgame.py --ends`: every copy waits for its game to end by itself (a quit, a victory) after `--ticks` (or
  `--join-ticks`) instead of quitting there.
- After a player quits (command 49), the copies' telemetry is compared up to the tick the Quit ran: from then on each
  machine keeps its own view of who is human.
- `OP2H_DESKTOP=1`: the harness leaves the desktop alone (doesn't move the mouse pointer or bring the game to the
  front). `windows/run_win.py` sets it and starts the game hidden, or shown without taking focus when screenshots, a bot
  or the new `--show` need the window drawn.
- `windows/run_win.py mission DLL OUT_DIR`: plays a mission DLL on Windows (`--ticks`, `--players`,
  `--telemetry-every`, `--shot-every`, `--no-turbo`, `--show`).
- `summary.json` says which platform the game ran on: `"platform": "wine"` (run.py) or `"windows"`
  (windows/run_win.py), so a native run can't be taken for a Wine reference by accident.

**Fixed**
- Native Windows: the OfficialDemos cde1, cdp1 and ml4_dm1 crashed at load (0xC0000005). They lack relocations and
  must load at 0x11000000, which Windows could give to one of the game's allocations first. On Windows (not under
  Wine) the harness now reserves that address at startup, releases it when OPU loads a DLL linked there, and takes it
  again when that DLL is unloaded. Wine runs are unchanged; on Windows, heap pointers in telemetry's `raw` bytes move.
- The `-Wmissing-braces` warning in `src/harness.cpp` (MinGW).

## 1.0.0 (2026-11-10)

The first version as a repository of its own (op2-rs issue #34), shown to work for the three projects it measures for:
the original game, op2-rs and outpost2-port (see **Verified**). It was developed as 0.9.0, which was never released.

**Contract:** identical to op2-rs's `harness/` at commit ab5f74dd (2026-11-09): the same `OP2H_*` variables, `run.sh`
options, script verbs, unit names, output fields and scenario format. `src/harness.cpp` behaves as op2-rs's; it differs
only in comments and in the command line written into a recording's header.

**History:** op2-rs's `harness/` history, filtered with `git filter-repo` and without `oracle/` (PROVENANCE.md).

**Added**
- `windows/`: an MSVC build of every DLL a release ships, reproducible with `/Brepro` (`build.bat`, `msvc_compat.h`:
  `harness.dll`, `zScenario.dll`, `zHarnessEnd.dll` and the seven bots); a native Windows launcher that plays scenarios
  in a private copy of the game, without Wine, with an optional bot (`run_win.py`), and its `selftest`, the Makefile's
  `make check` on Windows; the release zip with `SHA256SUMS` (`package.py`); the original vs op2-rs on one
  scenario, with op2-sim's tick-by-tick compare and both pictures side by side (`compare.py`); the original vs
  outpost2-port (`run_port.py`, `trace_diff.py`, `compare_port.py`).
- CI (GitHub Actions): both toolchains build all ten DLLs on every push (`make` with MinGW on Ubuntu, `build.bat` with
  MSVC on Windows), each build must be reproducible, and a `vX.Y.Z` tag publishes a GitHub release with
  `OP2TestHarness-<VERSION>-mingw.zip` and `-msvc.zip`, their `.sha256` and this changelog's section.
- `ARCHITECTURE.md`, `PROVENANCE.md`, this changelog, `VERSION`, `.gitignore`, `.gitattributes`.

**Changed**
- README.md describes a project of its own: Windows and Linux quick starts, requirements, where prefixes and the game
  copy live, and "Comparing with op2-rs". op2-rs's scenarios stay in op2-rs (`harness/oracle/`), written `$ORACLE`.
- `test/session_check.py` reads the canonical campaign scripts from `$ORACLE` (default `oracle/` in this folder, as
  inside op2-rs).
- The Makefile reads TethysAPI from `vendor/include` in this folder first; `run.py` compares the installed DLLs with this
  folder's `build/`.
- The Windows scripts write their text files (`scenario.json`, `scenario_script.txt`, summaries, reports) with LF, as
  `run.py` does on Linux, so they hash the same as op2-rs's recordings.

**Verified** on Windows 10:
- **The original game.** All 456 of op2-rs's scenarios (its `harness/oracle/scenarios`, at ab5f74dd) run to their tick
  limit with no timeout, crash or dialog. `run_win.py selftest` passes (the self-test mission ends MissionAccomplished,
  alone and with `sample_bot.dll`).
- **Both toolchains.** The MinGW DLLs (CI) and the MSVC DLLs give byte-identical `trace.jsonl`, `telemetry.jsonl` and
  `gamedata.json` on line_fast, cmb_duel_laser and cmb_duel_rail_gun, and so does op2-rs's own harness source at
  ab5f74dd built with MSVC. Each toolchain's build is reproducible (CI checks this on every push).
- **op2-rs.** op2-sim matches line_fast, cmb_duel_laser and cmb_duel_rail_gun on every trace field of every tick.
  Against the hashes of op2-rs's recorded runs (`accuracy/manifest.json`, hashed with its `accuracy/runs.py`), over the
  456 scenarios: `gamedata.json` and `scenario_script.txt` match on every run, `scenario.json` on 452 (the other 4
  scenario files were edited after their runs were recorded), `audio.jsonl` on all 8 that have one, and 30 runs match
  in every file. The other `trace.jsonl` and `telemetry.jsonl` files differ from recordings made by earlier harness
  versions, which wrote fewer fields: no scenario added before 2026-10-31 matches and most added since do, and the
  differing telemetry is the economy runs' newer fields (hives, beacons). op2-rs's own `runs.py check --fresh` accepts
  such runs as `fresh_newer_fields`.
- **outpost2-port.** Compared with `windows/compare_port.py`: 8 of 9 scenarios match the original (outpost2-port
  958323a). Its upstream, marcelomgoncalves/outpost2 (1ac3673), matches cmb_duel_laser and cmb_duel_rail_gun on every
  compared field, and line_fast on every tick but its arrival (tick 731, x 16/65536 of a pixel short); op2-rs matches
  all three.

Not verified here: the Linux harness under Wine (`run.sh`, `make check`) since the move (CI builds it but can't run the
game), and op2-rs's `runs.py check --fresh` / `make accuracy` against this version's DLLs, which needs op2-rs's stored
recordings.
