# OP2TestHarness on Windows

The harness DLL is plain Win32, so on Windows it runs in the real game without Wine or Xvfb. This folder holds what
that needs; the rest of OP2TestHarness (src/, test/, run.py's scenario translation, the scenarios) is shared with Linux.

| File | What |
|---|---|
| `build.bat` | Builds every DLL a release ships into `build\win` (32-bit, MSVC, reproducible): `harness.dll`, `zScenario.dll`, `zHarnessEnd.dll` and the seven bots |
| `msvc_compat.h` | Force-included by `build.bat`: C++20, `NOMINMAX`, `__builtin_return_address` -> `_ReturnAddress`, `#undef small` (a Windows SDK macro) |
| `run_win.py` | `setup` makes a private copy of the game with the harness installed; `run` plays a scenario in it (`--bot` adds a bot); `mission` plays a mission DLL; `selftest` is `make check` on Windows |
| `package.py` | packs a release: `build\release\OP2TestHarness-<VERSION>-<toolchain>.zip` (`msvc` or `mingw`) with the DLLs, the scripts, the sources and `SHA256SUMS` |
| `compare.py` | the original vs op2-rs on one scenario: op2-sim's tick-by-tick compare, both pictures side by side |
| `run_port.py` | `install` copies a built outpost2-port into the game copy; `run` plays a scenario in its `--harness` mode |
| `trace_diff.py` | compares two `trace.jsonl` files field by field, tick by tick (MATCH, or each field's first difference) |
| `compare_port.py` | the original vs a port with a `--harness` mode (outpost2-port, marcelomgoncalves/outpost2) and with `--op2rs` op2-rs, on one or more scenarios; pictures side by side (original \| op2-rs \| port) |

## Requirements
- Visual Studio 2022 (Build Tools or any edition) with the C++ workload: `build.bat` finds it with `vswhere`.
  Or skip building: unpack a release's `-msvc.zip` (its DLLs are in `build\win`) or `-mingw.zip` (copy its
  `build\*.dll` into `build\win`).
- TethysAPI at commit 141fd47 in `vendor\include\Tethys`:
  `git clone https://github.com/OutpostUniverse/TethysAPI.git vendor\include\Tethys` then
  `git -C vendor\include\Tethys checkout 141fd47`.
- Python 3.11+ (no extra packages; screenshots are converted to PNG through System.Drawing).
- Outpost 2 (GOG 1.2) with OPU 1.4.1.

## Use (from the OP2TestHarness folder)
The scenarios are op2-rs's, in its `harness\oracle` folder (README.md "Scenarios and recorded runs"):
`set ORACLE=C:\Users\Desktop\GITHUB\op2-rs\harness\oracle` first.
```
windows\build.bat
python -I windows\run_win.py setup --from "C:\Users\Desktop\Outpost 2"
python -I windows\run_win.py selftest
python -I windows\run_win.py run %ORACLE%\scenarios\line_fast.json build\runs\line_fast --shot-every 350
```
`setup` copies the game to `game\` (no .avi movies, no OPU backups or logs), installs the two DLLs into
`game\OPU\Harness` and registers the module in `game\OPU\outpost2.ini`. Run it again after each rebuild. The player's
own install is never written to. Add `--no-turbo` to `run` to watch the game play in real time (with `--show` to
see the window: runs are hidden unless they take screenshots or run a bot).

`mission` plays a mission DLL from the game copy (as the Linux harness's `--mission`) and writes the same outputs:
```
python -I windows\run_win.py mission cde1.dll build\runs\cde1 --ticks 300
```
`--players N` for a multiplayer map, `--telemetry-every`, `--shot-every`, `--no-turbo` and `--show` as for `run`.

To compare with op2-rs's simulation (op2-rs built, `OP2_DIR` set to the game):
```
op2-inspect compare --scenario %ORACLE%\scenarios\line_fast.json --oracle build\runs\line_fast --out build\cmp\line_fast
```

`selftest` is the Makefile's `make check` on Windows: it plays the self-test mission (`zHarnessEnd.dll`, won at tick
800), which must end MissionAccomplished with 4 screenshots, then plays it again with `sample_bot.dll`, which must load.
The mission sits in `game\OPU\maps\HarnessTest` only while the test runs.

## A release
```
windows\build.bat
python -I windows\run_win.py selftest
python -I windows\package.py
```
`build.bat` finds Visual Studio with `vswhere` (`VCVARS` overrides it) and links with `/Brepro`, so the same sources
give byte-identical DLLs. `package.py` refuses uncommitted changes, and writes
`build\release\OP2TestHarness-<VERSION>-msvc.zip` and its `.sha256` (`--toolchain mingw` packs `make`'s build in
`build/` instead). The zip holds one folder laid out like the repository (the DLLs where that toolchain's tools look for
them: `build\win` or `build/`), every file git tracks, and `SHA256SUMS` for every file in it. Entries are sorted and
dated with the commit's time, so the zip is reproducible too.

**CI** (`.github/workflows/build.yml`, GitHub Actions): on every push and pull request, the Python scripts must parse,
both toolchains build all ten DLLs (`make` with MinGW on Ubuntu, `build.bat` with MSVC on Windows), a second clean
build of each must be byte-identical, and each is packaged (the run's artifacts). A tag `vX.Y.Z` matching `VERSION`
publishes a GitHub release with both packages and the version's CHANGELOG.md section. CI can't run the game (its files
can't be in CI), so run `selftest` locally before tagging.

The two builds behave the same: with the MinGW DLLs from CI and with the MSVC DLLs, line_fast, cmb_duel_laser and
cmb_duel_rail_gun give byte-identical `trace.jsonl`, `telemetry.jsonl` and `gamedata.json`, and both pass `selftest`.

## The original vs op2-rs in one command
```
python -I windows\compare.py %ORACLE%\scenarios\cmb_duel_laser.json --tick 200
```
`compare.py` plays the scenario in the original, has op2-sim replay it against the original's trace
(`op2-inspect compare`), has op2-rs's engine draw the same tick at 1024x768, and writes to
`build\compare\<scenario>\`: `side_by_side.png` (original left, op2-rs right), `sim\plot.png` (both trajectories),
`sim_compare.txt` (MATCH, or the first divergence of every field). It needs an op2-rs checkout (`--op2rs`, default
`C:\Users\Desktop\GITHUB\op2-rs`) with `cargo build -p op2-inspect -p op2-engine` done in its `tools\`.

## The original vs outpost2-port
[outpost2-port](https://github.com/leviathan400/outpost2-port) has a `--harness` mode that plays a scenario script and
writes the same `trace.jsonl` as the original's harness. After building the port (`cmake --build build --config Release`):
```
python -I windows\run_port.py install --port C:\Users\Desktop\GITHUB\outpost2-port
python -I windows\compare_port.py %ORACLE%\scenarios\line_fast.json %ORACLE%\scenarios\cmb_duel_laser.json --op2rs
```
`compare_port.py` writes, per scenario, to `build\compare-port\<scenario>\`: `port_diff.txt` (MATCH, or the first tick
each field differs), `side_by_side.png` (original | op2-rs | port) and, with `--op2rs`, `sim_compare.txt`.

The port's upstream, [marcelomgoncalves/outpost2](https://github.com/marcelomgoncalves/outpost2), has the same
`--harness` mode, so it is compared the same way: `run_port.py install --port <its checkout>`, then `compare_port.py ...
--port-label marcel-outpost2` to name it in the pictures. On 2026-11-10 (its commit 1ac3673) it matched the original on
cmb_duel_laser and cmb_duel_rail_gun; on line_fast every tick but one (tick 731, the arrival: x 16/65536 of a pixel
short). In its pictures the damaged tanks have no health bars and the Plymouth tank is drawn in player 0's blue.

`trace_diff.py` compares any two traces on its own. Fields both sides write are compared; `cmd`/`action` aren't (names
in one, numbers in the other). `--ignore lights,rng` (on either script) sets known differences aside, to see what's
under them; `--no-pictures` skips the screenshots for a quicker batch. Several scenarios end in a scoreboard, one line
each.

Results (2026-11-09, port 958323a): 8 of 9 match.

| Scenario | Port vs original |
|---|---|
| cmb_duel_* (8: acid, cannon, EMP, foam, laser, microwave, rail gun, spam) | MATCH: every traced field and the RNG equal on every tick, hits, damage rolls and deaths included |
| line_fast | 1,399 of 1,400 ticks: the port frees the move's path state at tick 731, the original at 732 (`fix` shows the leftover fraction for one tick); position, heading, HP and route equal throughout |

How the first runs (port aca704a, nothing matched) got there:
- `lights` from tick 1: the harness spawns through TethysAPI's `Game::CreateUnit`, which turns a vehicle's headlights
  on after the game's CreateUnit; the port's harness mode now does the same (port de6ac4e).
- turret sweep a phase off, the other tank targeting first in every duel: the original raises its tick after the unit
  loop (0x48A01B call, 0x48A020..0x48A02D increment of `[GameImpl+0x84]`), the port before it (port 958323a).
- `rng` from tick 1 and different damage rolls: the port drew `getRand(100)` every tick for its debug HUD (port
  958323a).

The port's own `--savetest` hangs after its save at tick 600; it did so before the harness work too (port 9db6cd8).

## Verified (1.0.0, 2026-11-10, Windows 10)
- All 456 op2-rs scenarios (`$ORACLE/scenarios` at op2-rs ab5f74dd) run to their tick limit, with no timeout, crash or
  dialog: about 7 minutes for the set. `selftest` passes, alone and with `sample_bot.dll`.
- The MinGW (CI), MSVC and op2-rs-source builds give byte-identical `trace.jsonl`, `telemetry.jsonl` and
  `gamedata.json`; two runs of a scenario are byte-identical too.
- line_fast, cmb_duel_laser and cmb_duel_rail_gun: `MATCH` against op2-sim, every trace field equal on every tick.
- Against op2-rs's recorded-run hashes (`accuracy/manifest.json`): `gamedata.json` and `scenario_script.txt` match on
  all 456 runs and 30 runs match in every file; the other traces and telemetry differ from recordings made by earlier
  harness versions with fewer fields (CHANGELOG.md 1.0.0 has the detail).
- Speed: 1,700-20,000 ticks/s in turbo.

## Known differences from the Linux harness
- The window's client area is 1032x766, not 1024x768: the ini's frame padding (+48, +36) is Wine's. Screenshots
  differ in size from Linux runs; the simulation is unaffected.
- The desktop is left alone: `run_win.py` sets `OP2H_DESKTOP=1`, so the harness doesn't move the real mouse pointer
  (`ParkCursor`) or bring the game to the front. The game window starts hidden (`SW_HIDE`), or shown without taking
  focus (`SW_SHOWNOACTIVATE`) when screenshots, a bot or `--show` need it drawn. Scenario results are the same either
  way (line_fast, cmb_duel_laser, cmb_duel_rail_gun and bgrp_order: byte-identical traces, telemetry and gamedata).
- Mission DLLs are linked at 0x11000000, and some (the OfficialDemos cde1, cdp1 and ml4_dm1) crash when loaded
  anywhere else. Windows can give that address to one of the game's own allocations before OPU loads the mission, so
  on Windows (not under Wine) the harness reserves it at startup and lets it go when OPU loads a DLL linked there.
  The game's block then lands elsewhere, so heap pointers in telemetry's `raw` bytes (a Garage's bays) differ from
  Wine runs; the simulation is unaffected.
- `run_win.py` covers scenarios and missions (`mission`), not `--campaign`, `--record`, checkpoints or shell mode.

## Do not commit
`game\` (a copy of the game's files), `build\` and `vendor\include\Tethys` (an upstream clone) stay out of version
control.
