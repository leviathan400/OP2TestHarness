# OP2TestHarness - Outpost 2 Test Harness

OP2TestHarness turns the original Outpost 2: Divided Destiny into a measuring instrument. A DLL loaded into the real
game (`harness.dll`, an op2ext module) plays a mission or a scenario unattended: no shell or clicking, the game speed
uncapped, scripted orders at given ticks, a fixed seed. It writes what happened as files (unit state every tick as
JSONL, telemetry, PNG screenshots) and quits by itself. Any mission DLL works without changes. Runs are deterministic:
the same recipe gives byte-identical output, so a recipe (scenario JSON or script) stands for its recording.

It was written for [op2-rs](https://gitlab.com/bufke/op2-rs), a reimplementation of the game, which plays the same
recipes in its own simulation and compares every field, tick by tick ("Comparing with op2-rs"). It works for any
reimplementation or tool that can read the outputs. ARCHITECTURE.md has the design, PROVENANCE.md the origin.

**Version:** 1.0.0 (`VERSION`). What other projects' recordings rely on (options, script verbs, unit names, output
fields, the scenario format) changes only with a version bump; CHANGELOG.md has the rules and the history.

**Releases:** https://github.com/leviathan400/OP2TestHarness/releases. Each has two packages,
`OP2TestHarness-<VERSION>-mingw.zip` (built with `make`, as op2-rs's recorded runs were) and `-msvc.zip` (built with
`windows\build.bat`), which behave identically. Each holds the ten DLLs, the Python driver, the sources and
`SHA256SUMS`, and comes with a `.sha256`. CI (GitHub Actions) builds both on every push and checks each is reproducible.

**Licence:** GPL-3.0 (LICENSE). op2-rs, where the harness was written, is GPL-3.0-or-later.

**Windows** (native, no Wine; windows/README.md):

```bat
windows\build.bat                                         :: build the ten DLLs into build\win (MSVC; or unpack a release)
python -I windows\run_win.py setup                        :: private copy of the game in game\, harness installed
python -I windows\run_win.py selftest                     :: make check on Windows
python -I windows\run_win.py run %ORACLE%\scenarios\line_fast.json build\runs\line_fast --shot-every 350
python -I windows\compare.py %ORACLE%\scenarios\cmb_duel_laser.json --tick 200   :: the original vs op2-rs
```

**Linux** (Wine + Xvfb, the full option set below):

```sh
make install                       # once (and after changing src/): build and install into ../prefix-harness
./run.sh --mission cHello.dll --ticks 6000 --screenshot-every 1500 --telemetry-every 500 --out /tmp/op2run
./run.sh --scenario $ORACLE/scenarios/line_fast.json --out /tmp/op2scn   # oracle trace of a scenario
make check                         # end-to-end self-test
```

**Requirements.**
- Outpost 2, the GOG release (1.2), with the **OPU 1.4.1** update: op2ext (part of OPU) loads `harness.dll` as an ini
  module, and every address in `src/harness.cpp` is for that `Outpost2.exe`. Recordings are of GOG 1.2 + OPU 1.4.1; a
  different OPU version can change them, so re-record and review the differences before switching.
- TethysAPI at commit 141fd47 in `vendor/include/Tethys` (`git clone https://github.com/OutpostUniverse/TethysAPI.git
  vendor/include/Tethys`, then `git -C vendor/include/Tethys checkout 141fd47`).
- Windows: Visual Studio 2022 Build Tools (C++), Python 3.11+.
- Linux: `i686-w64-mingw32-g++`, Wine (a wow64 prefix), Xvfb, Python 3.11+ (PIL for PNG screenshots).

**Where things go.** On Linux the Wine prefixes sit beside this folder: `../prefix` is the one you play from, and
harness runs use `../prefix-harness`, a copy of it (`cp -a ../prefix ../prefix-harness`), or the absolute path in
`OP2_HARNESS_PREFIX`. On Windows the harness runs in `game\`, a copy `run_win.py setup` makes; the install you play from
is never written to. Run outputs go where `--out` says; `build/`, `game/` and `vendor/` are not committed (they hold
game data or upstream code).

**Names in this file.** `op2-sim`, `op2-engine`, `op2-inspect`, `op2-missionhost` and `op2-accuracy` are op2-rs's tools,
and paths written as `op2-rs notes/...`, `op2-rs docs/...`, `op2-rs accuracy/...` or `op2-rs tools/...` are in the op2-rs
repository.

**Scenarios and recorded runs (`ORACLE`).** The recipes this harness plays (scenario JSON, mission scripts, input and
network scripts) and the findings measured with them stay in op2-rs, in `harness/oracle/`: they are op2-rs's regression
tests. This file writes that folder as `$ORACLE` (Linux) or `%ORACLE%` (Windows), e.g.
`ORACLE=~/op2-rs/harness/oracle` or `set ORACLE=C:\Users\Desktop\GITHUB\op2-rs\harness\oracle`. Comments inside
those recipe files write this harness's folder as `harness/`.

## Options (`run.sh --help`)

The Linux launcher's options. On Windows, `windows/run_win.py run` covers scenarios so far (`--shot-every`,
`--no-turbo`, `--timeout`, `--bot`; the scenario sets ticks, seed, trace and telemetry intervals) and `run_win.py
selftest` is `make check`; the module itself reads the same `OP2H_*` settings on both.

| option | default | |
|---|---|---|
| `--mission NAME` | | mission DLL file name, found the same way the game finds it (`OPU/maps/*/`, game dir) |
| `--scenario FILE` | | scenario JSON instead of a mission (see "Scenarios"); exactly one of the two is required |
| `--out DIR` | required | output directory; previous outputs in it are replaced |
| `--ticks N` | 3000 | quit after game tick N (100 ticks = 1 mark). 0 = run until the mission ends. Default: the scenario's `ticks` |
| `--trace-every N` | 0, or 1 with `--scenario` | `trace.jsonl` line every N ticks (sub-pixel unit positions, see below) |
| `--trace-map` | | add `map` to each trace line: the tiles whose cell type, graphic or wall-or-building bit changed since the previous line, `[[x, y, cell, graphic, wall], ...]` in file coordinates, row by row (the first line lists none). Default: the scenario's `traceMap` |
| `--trace-from TICK` | 0 | no `trace.jsonl` lines before TICK, rounded up to a multiple of `--trace-every` (so the lines kept are those a full trace has), and the last tick as always: a full per-tick `--trace-raw` trace of a long run reaches 500 MB |
| `--trace-raw` | | add raw MapObject (0x80) and PathContext (0xD4) bytes as hex to each trace unit, and to each `ents` entry its animation, frame and raw MapObject bytes (`[..., px, py, anim, frame, "raw"]`; with `--draw-every 1` the animation and frame are what was drawn, e.g. a structure a ConVec builds) |
| `--screenshot-every N` | 0 (off) | PNG at tick 5, then every multiple of N, plus one at the end |
| `--telemetry-every N` | 100 | JSONL record at tick 1, then every multiple of N, plus one at the end. 0 = off |
| `--script FILE` | | scripted commands, see below |
| `--bot DLL` | | bot DLL called every tick, see below |
| `--seed N` | 1 | random seed (default: the scenario's). Identical seed and inputs give byte-identical telemetry |
| `--difficulty 0/1/2` | 1 | easy / normal / hard |
| `--players N` | 1 | N human players in a multiplayer start (2-6), plus any `DescBlockEx` AI slots; 1 keeps the single-player setup. Aborts when N exceeds the descriptor's maximum; a DLL without `DescBlockEx` has no AI slots, as in the game (0x48958F) |
| `--audio` | | `audio.jsonl`: every sound, status message, Savant line and the music playlist the game asks for (see "Audio log") |
| `--state` | | session state: `events.jsonl` through the run and `state.json` at the end (see "Session state") |
| `--stop KINDS` | | end the run at the first event of these kinds after `--stop-from` (implies `--state`) |
| `--stop-from T` | 0 | `--stop` only counts events after tick T |
| `--checkpoint SAVE` | | when the run stops with the game going on (`--ticks`, `--stop`), save it to SAVE (OPU/saves/SAVE, or a path) first, with the mission DLL's state beside it (see "Checkpoints") |
| `--resume SAVE` | | load that save at the first game cycle and play the script on from its tick; every script line must be at that tick or later (see "Checkpoints") |
| `--campaign FILE` | | start as a campaign mission with this carry-over (MissionResults): `key value` lines `food`, `common`, `rare`, `workers`, `scientists`, `kids`, `satellites`, `moraleLevel`, `morale`, `event`, `techLevel`, `mission` (the finished mission's number; default the DLL's minus one, as the game starts the next one), and `techs` followed by the completed topic ids. Older files without `techs` still start with no carried research |
| `--no-turbo` | | keep the game's frame limiter and render every frame (real time at `--speed`) |
| `--presents` | | `present.log`: detail/minimap/command pane presents per game tick and per real second |
| `--rng-log FROM-TO` | | `rng.log` (one tick: `--rng-log T`; a malformed range is reported in harness.log): each gameplay RNG draw (Random::Rand 0x46F000, Rand(n) 0x46F060) in those ticks as `tick caller n` (`-` for a plain Rand()), the caller being the return address: which code drew where op2-sim's RNG first differs |
| `--speed 1-10` | 10 | in-game speed setting. Only matters with `--no-turbo` |
| `--draw-every N` | 0 | in turbo mode, also render every N ticks. Default: render only just before screenshots |
| `--size WxH` | 1024x768 | requested client size. The game rounds it (1024x768 gives 1032x766) |
| `--timeout S` | 600 | wall-clock limit. On timeout the X screen is saved to `timeout_screen.png` and the game is killed |
| `--display :N` | private Xvfb | use an existing X display instead, e.g. to watch a run |
| `--record` | | record a person playing (see "Recording your own play"): real time, real mouse and keys, sound on, no tick limit or timeout |
| `--desktop WxH` | | run the game in a Wine virtual desktop of that size (dialogs stay inside it; for gamescope) |
| `--silent` | | with `--record`: sound and music off, no audio device |
| `--fast` | | with `--record`, for tests of recording: turbo on a private Xvfb, nobody playing (`savegame`/`loadgame` lines stand in for the dialogs) |

The exit status is 0 when the run reached its tick limit or the mission ended.

## Outputs

- `summary.json`: the platform the game ran on (`wine` from run.py, `windows` from windows/run_win.py), exit code,
  whether it timed out, wall time, last tick, how the game ended (`Running`, `MissionAccomplished`, `MissionFailed`,
  ...), screenshot count, and any game dialogs that were suppressed. A native Windows run's screenshots differ from
  Wine's (Windows' GDI lays out some Arial glyphs differently, e.g. from "g" on), and so do the heap pointers its
  traces hold, so a user can keep such runs out of its references by this field.
- `telemetry.jsonl`: one object per sample:
  ```json
  {"tick":3000,"mark":30,"end":0,
   "players":[{"id":0,"human":1,"eden":1,"food":2528,"commonOre":0,"rareOre":0,"workers":20,"scientists":10,
               "kids":10,"morale":0,"moraleLevel":4,"powerGen":300,"powerUsed":70,"foodNet":-40,"buildings":4,
               "idleBuildings":0,"techs":[0,2099,2101,...]}],
   "units":[{"id":7,"type":"LIGHT_TANK","owner":0,"x":79,"y":61,"hp":300,"maxHp":300,"cmd":"Attack","cargo":"LASER"}]}
  ```
  - `x`/`y` are internal tile coordinates, the same `Location` values mission code uses. On non-wrapping maps the
    in-game display shows x-31, y+1.
  - `moraleLevel` runs from 0 (Great) to 4 (Rotten). `end` is the raw `GameTermReasons` value.
  - `techs` lists the tech IDs the player has completed.
  - `cmd` is the unit's current `CommandType`. `cargo` is the weapon type, or the kit a ConVec carries.
  - `units` holds every live building and vehicle of each player. Gaia objects such as beacons and wreckage are left
    out.
- `trace.jsonl` (with `--trace-every`/`--scenario`): the op2-rs ENGINE_PLAN.md trace format, one line per sampled tick:
  ```json
  {"tick":120,"units":[{"id":1,"spawn":0,"type":"LIGHT_TANK","owner":0,"px":1680,"py":976,"sx":1679.666656,
   "sy":976.0,"fix":[110078635,63963136],"dir":0,"hp":300,"cmd":"Move","action":"Move","busy":0,"ipx":2704,
   "ipy":976,"frame":0,"anim":0,"dest":[42,22],"path":{"start":[1648,976],"moved":[10922,32768],"toMove":[1,0],
   "rotSpeed":0,"wp":0,"numWp":1,"pt":2,"numPts":30,"steps":"0000000000000000000000000000"}}]}
  ```
  - Units spawned by the script come first, in spawn order, with `spawn` = spawn index. Without spawns every vehicle
    is listed.
  - `px`/`py`: the game's integer `pixelX_`/`pixelY_` in **world pixels** (file tile * 32 + offset, see
    "Coordinates"). `fix` is the exact 16.16 fixed-point position and `sx`/`sy` the same as floats (see below).
    `ipx`/`ipy` are the raw internal pixels.
  - `frame`/`anim` are the sprite animation fields. The renderer sets them, so they depend on whether and when
    frames were drawn. Everything else is unaffected by rendering.
  - `dir` is `rotation_`, 0..255, 0 = east, clockwise. `cmd`/`action` are CommandType/ActionType names.
  - `dest` is the current waypoint's tile (file coordinates). `path` is the raw PathContext: `steps` holds the
    directions still planned in the current path fragment (one digit per tile, 0 = E, clockwise), `pt`/`numPts` the
    position in it, `rotSpeed` the per-tick rotation while turning.
  - `tiles`: the file tiles within 3 of the unit whose TileData::unitIndex is the unit (a building's footprint
    without its dock, a moving vehicle's next tile).
  - A spawn is listed until it dies (the dead flag), and never again: the game recycles object indices, so a dead
    spawn's ID can later belong to a shot or an explosion. A vehicle in a Garage bay isn't listed while it's in there
    (the game marks it dead too, with `pNext_` -2) and is listed again once it's out. A spawn that failed (the object
    table full: 2047 objects) is never listed.
  - Armed units (vehicles and Guard Posts; other structures use the weapon word for other things, e.g. a lab's research
    countdown) add `weapon`, `tur`, `reload`, `tgt` (op2-rs ENGINE_PLAN.md). A unit under a weapon status effect adds
    `status`: its [EMP, stickyfoam, ESG] timers (+0x56, +0x54, +0x58, in 8-tick phases; 0 for an effect that is off). Top level: `fx` (weapon fire of spawned
    units, with `timer`), `ents` (every other live object as `[index, type, action, timer, px, py]`: explosions,
    unspawned units), `alloc` (the object index allocator, 0x439A10: highest index so far, the ring of freed
    objects as `[index, tick freed, reference count]` from its head to its tail, and the ring's `[head, tail]` slots;
    the ring has 2048 slots as OPU patches it in memory, 1024 on disk; a slot holding a live object would be
    `[index, 4294967295, 0]`), `rng` (the gameplay RNG).
- `gamedata.json`, written at the first cycle: map geometry (`width`, `height`, `paddingTiles`, `clipRect`...), the
  game's cell type table (`trackSpeed` per track type: Legged, Wheeled, Miner, Tracked, Hover, `blocksLineOfSight`),
  every unit type's player-0 stats (`moveSpeed`, `turnRate`, `track`, `hp`, `armor`, the raw `stats` words, ...;
  buildings: `size`, `dock` per colony, `rubble` [common, rare], `buildingFlags`; weapons: `pixelsSkipped`,
  `radius`, `indirect` = type +0x250), and `cells`: the cell type of every tile as the running game sees it, in file
  coordinates, row-major.
- `shot_<tick>.png`: the game window's client area.
- `present.log` (with `--presents`): one JSON object per game tick and per real second. Tick records count the detail,
  minimap and command surfaces' presents; second records add the interval's `ms`, tick delta and ending tick. The hook is
  the shared DIB present method 0x4A9410 (BitBlt/StretchDIBits), filtered by the three panes' surface pointers.
- `harness.log`: what the in-game module did, including script commands, suppressed dialogs, the quit reason and the
  tick rate.
- `wine.log`, and `outpost2.ini`: the private settings copy used for the run (see below).

## Scripted input (`--script`)

One command per line: `<tick> <verb> <args>`. Text after `#` is ignored. Unit IDs are the `id`s from the telemetry,
or `@N` for the unit made by the Nth `spawn` command, or a name (`$NAME`, "Unit names" below). Coordinates are internal tile coordinates unless a
`coords file` line switched to file coordinates. Example: `test/hello_script.txt`.

```
100  move      7 90 60      # unit 7 to tile (90,60)
100  dock      7 41 41      # Dock order (ctMoDock) to a tile, e.g. a structure's dock
100  dockeg    7 32 42      # ctMoDockEG, the Dock the UI gives at an Eden Garage (ends facing north)
3200 attack    8 11         # unit 8 attacks unit 11
0    coords    file         # later x/y are file tiles (internal x = file x + map padding)
0    spawn     LIGHT_TANK 0 20 30 LASER 64   # type, owner, x, y, weapon (or -), rotation 0-255
0    setcell   10 20 50 24 7                 # x1 y1 x2 y2 cellType: repaint terrain (inclusive)
10   move      @0 40 30     # the first spawned unit
0    view      60 64        # centre the view on a tile (for screenshots; not applied with --draw-every 1)
     stop <id> | selfdestruct <id> | research <labId> <techId> <scientists> | shot | telemetry | trace | quit
     guard <id> <id> | attackground <id> x y | lights <id> 0|1 | scatter <id> <id>...   # scatter: one ctMoScatter
     movewp <id> x,y[,r] ... | standground <id> x,y[,r] ... | patrol <id> <loop> x,y[,r] ...   # waypoints, range r
1236 packet    2 012b0001003007a300    # a recorded UI packet (type, data hex), run inside the cycle starting at 1236
     savegame <file> | loadgame <file>  # TApp::SaveGame/LoadGame on OPU/saves/<file>, once each (recording tests)
     gamespeed <1-10>   # what the in-game Game Speed buttons queue: ctGameOpt 0x14 = speed x 4 (0x4231C0), which runs 2 turns later
```

The last group builds the packets the UI sends (Guard, Attack on a tile, LightToggle, Move/Stand Ground/Patrol with up
to 8 waypoints; `r` is the waypoint's range in its top 3 bits, `loop` the waypoint a patrol returns to). See
op2-rs `notes/findings-commands-audio.md`.

Commands for a tick run after that game cycle. A move at tick T shows `cmd: Move` in tick T's trace line and the unit
starts turning or moving in cycle T+1.

The order verbs send the packets the UI sends, so the game's own checks apply (a ConVec that is building or an EMPed
vehicle ignores them; chaos_sd_mid_dock), except `attack`, which goes through TethysAPI's `Unit::DoAttack`: it sends
nothing when the target isn't live. A command whose first argument is a spawn (`@N`) the trace has seen die is skipped
(logged): the dead unit's object index may belong to a shot by then, and an order to that crashes the game.

**Differential fuzzing.** `op2-inspect fuzz --template $ORACLE/fuzz/colony.json --seeds 1-300 --out DIR`
generates a random but legal script of these verbs per seed for a template (`$ORACLE/fuzz/*.json`: a scenario plus
what to generate), plays it here and in op2-sim, compares the traces, and shrinks each diverging script (and each
crash of the original, by crash site) to a minimal one with ddmin (`DIR/seed-N/minimal.json`). It is op2-inspect's
(op2-rs `tools/crates/op2-inspect/src/fuzz_run.rs`) and runs this harness for the original's side. A template's `extras` mixes in verb groups
beyond the orders (`disaster`, `earthwork`, `repair`, `damage`, `scatter`, `eco`; `extra_percent` of the lines) and
`wrap` (512) lets the area run past a wrapping map's seam. A diverging run whose script names a dead spawn after a
line's first word (an order that reached whatever took the dead unit's index) is played again without those lines
(`dropped` in summary.jsonl). Templates: colony, skirmish, seam, choke, warfare, colony_stress, edges.

## Mouse and keyboard input

Script verbs that drive the game's UI the way a player does (the orders above bypass the UI):

```
10  click    unit @0              # left click on a unit (select it)
20  click    tile 34 28 +shot     # left click on a tile; +shot = screenshot one tick later
30  rclick   432 416              # right click at game window client pixels (as in screenshots)
40  drag     tile 22 22 tile 28 28          # left-button drag (box select); `right`/`middle` for other buttons
50  click    unit @1 shift        # modifiers: shift, ctrl, alt (held while the button goes down)
60  click    mini 50 50 hold 2    # minimap pixel showing tile (50,50); release the button 2 ticks later
70  click    ui tab2              # command pane tab 0-5 (Factories, Labs, Resources, Communications, Space, Game Options)
80  click    ui Self_Destruct     # a control by tooltip substring ('_' = space); `uilist` logs every control
90  key      ctrl+1 alt+c f5 enter          # key presses, through the frame's accelerators like real input
100 type     hello world          # characters (chat: `key enter`, `type ...`, `key enter`)
    mouse <pos> | down <pos> [button] | up <pos> [button] | dblclick <pos> | mclick <pos> | keydown <key> | keyup <key>
    select <unit>                 # left click on the unit, centring the view first if it is off screen
    hotkeys                       # the command pane's hotkey table now: a line of hotkeys.jsonl (key, flags, tooltip)
```

- A `<pos>` is `<x> <y>` (client pixels), `tile <x> <y>` (centre of that tile in the detail pane's current view;
  `coords file` applies), `unit <id|@N>`, `mini <x> <y>`, or `ui <name>` (`tabN`, `minimapN`, or tooltip text).
- Keys: letters, digits, `f1`-`f12`, `enter`, `esc`, `space`, `tab`, `backspace`, `delete`, `home`, `end`, `pageup`,
  `pagedown`, arrows, `pause`, `plus`, `minus`, `numplus`, `numminus`, `vk:0xNN`, with `shift+`/`ctrl+`/`alt+`.
- With `--scenario`, `--script` lines are appended to the scenario's own script (so in file coordinates), and a
  scenario may carry raw script lines as `"script": ["10 click unit @0", ...]`. `$ORACLE/input/` has the controls
  experiments (`ui_base.json` plus one script per behaviour, see $ORACLE/FINDINGS.md "Controls"):
  `./run.sh --scenario $ORACLE/input/ui_base.json --script $ORACLE/input/box_rules.txt
  --telemetry-every 1 --out /tmp/x`.
- When a script uses input verbs, each telemetry line adds `selected` (unit IDs, the game's selection list at
  0x547210) and `view` (the detail pane's top-left corner in file pixels). `harness.log` logs every input with the
  pane and map tile it hit, plus the pane rectangles.

How it works, and what is deterministic:
- Input runs in process right after the game cycle of its tick, like everything else in the script. Mouse messages
  go straight to the pane under the cursor (or the one with capture); key and character messages go through
  `GameFrame::TranslateAccelerators` and `IDlgWnd::PretranslateModeless`, then to the frame, as `TApp::PumpMessage`
  would. The game also polls `GetCursorPos` and `GetAsyncKeyState`; their IAT slots point at the virtual cursor and
  keys, so nothing depends on the X pointer (which is parked off the window). Same script, same output: repeated and
  parallel runs give byte-identical telemetry and traces. With `--no-turbo` the same orders land on the same ticks
  (only the draw-dependent `frame`/`anim` trace fields differ, as always).
- Orders made through the UI are not immediate: a click at tick T executes in the first cycle 4k+1 after T+8 (9 to
  12 ticks later), where the `move` verb executes at once. Selection changes are immediate.
- Some UI only advances when the panes are drawn: the view change from `view` or a minimap click, arrow-key and
  edge scrolling, the bandbox. In turbo mode the harness draws for 4 ticks around every input (and every `view` in an
  input run), so give a `view` a tick before inputs that depend on it, and hold the minimap button over a tick
  (`hold 1+`). Scroll distances depend on how many frames are drawn; use `--draw-every 1` for realistic ones.
- Input runs keep the game window active (parallel runs share a wineserver and take the foreground from each other;
  an inactive game ignores keys and scrolling).
- Don't pause the game (`ctrl+p`, `pause`): no cycles run while paused, so the script can't unpause it and the run
  ends at `--timeout`. To measure input while paused use shell mode, whose driver's real input doesn't wait for
  cycles, and its `state` step (below; runs `shell_pause_input`).

## Economy verbs and telemetry (`--eco`)

Script verbs for colony experiments (oracle scenarios `eco_*`, spec in `$ORACLE/ECONOMY.md`):

```
1    player   0 workers 40          # set a PlayerImpl field: workers scientists kids food common rare morale
                                    # moraleLevel solarsat, or a byte offset (0x94); sets recalculateValues_
0    tube     44 47 69 47           # tubes over an inclusive rectangle (Game::CreateWall); `wall x1 y1 x2 y2 [TYPE]`
0    beacon   40 60 0 1 2           # mining beacon: ore 0 common / 1 rare, bar 0-2 (0 = 3-bar), internal variant 0-2
0    beacon   44 60 vent            # magma vent (`fumarole` for a fumarole)
5    route    @3 @1 @2              # cargo truck @3 on a mine (@1) <-> smelter (@2) route, as the UI sends it
5    produce  @4 CARGO_TRUCK        # factory order (TYPE [weapon]); `idle`/`unidle <bldg>`, `train <university> <n>`
5    kit      @5 TOKAMAK            # put a structure kit in a ConVec; `build <convec> <x> <y>` (the margin's bottom-right tile)
5    bay      @4 0 TOKAMAK          # fill a factory bay; `transfercargo <factory> <bay> [1]` (1: a Spaceport's bay
                                    # into the rocket on its pad); `launch <spaceport> [x y]` launches that rocket
5    cargo    @3 2 1000             # set a truck's cargo (CargoType, amount); `load`/`unload <building>`: the
                                    # building whose dock holds the truck (the load and unload handlers 0x424090/0x424240)
5    poke     @2 0x1e 2 1200        # write a MapObject field (offset, size 1/2/4, value), e.g. damage
1    peek     0x439A10 64           # log game memory as hex (up to 256 bytes): code as OPU patches it in memory
10   tech     0 5317                # _Player::MarkResearchComplete(techID) for player 0, as a mission grants a topic
                                    # (a topic not in the loaded tech tree is logged with the valid ids, not granted)
10   doze     @0 38 36 41 38        # Robo-Dozer: bulldoze an inclusive rectangle (ctMoDoze, as its click sends it)
10   buildwall @1 38 40 44 40 TUBE  # Earthworker: TUBE, WALL, WALL_LAVA or WALL_MICROBE over an inclusive rectangle
                                    # (ctMoBuildWall; the packet's bottom-right corner is exclusive, as the UI's)
10   removewall @2 40 70            # Earthworker: remove the tube or wall on a tile (ctMoRemoveWall, one waypoint)
20   repairobj @4 @1                # repair @1 (ctMoRepairObj, as the Repair command's click sends it; `none` for
                                    # its no-target click; more units after the target join the packet);
                                    # `reprogram` and `dismantle` send ctMoReprogram / ctMoDismantle the same way
```

`--eco` (or `"eco": true` in a scenario) adds to each telemetry player an `eco` object (storage limits, available and
required workers/scientists, scientists working as workers, food production/consumption, growth and death remainders,
capacities, morale inputs) and `pi`, every dword of PlayerImpl 0x00..0x1EF; and to each unit `flags` (MapObject +0x44),
`act`/`timer` (action and its timer), `busy`, `w24` (the cargo/researchTimer word) and `raw` (bytes 0x48..0x7F), plus
`truck` [cargo type, amount] for Cargo Trucks and, for buildings, `hive`: the counters of its hive (+0x38) as
[+0x8 owner, Command Centers, one enabled, enabled DIRTs, members, slot] (the slot: its index in HiveManager's hive array, which tells hives apart), which DoDamage reads live (0x430050). Each `--eco` telemetry line also has a top-level `beacons` array:
`x`/`y` in the same internal tile coordinates as units, plus `ore`, `bar` (yield) and `variant` for every live mining
beacon. `gamedata.json` gets `morale` (levels and modifiers as loaded), `mines`
(the yield table) and `typeExtra` ([mapId, resource priority, research topic]). Scenarios may also set
`telemetryEvery`, `traceEvery` and `difficulty`. Runs without `--eco` produce the same output as before.

## Unit names (`$NAME`, `as NAME`)

Unit ids are handed out as units appear, so a fight that goes differently shifts every later id (p12: miner 154 -> 155,
rare mine 162 -> 168) and a script naming ids can't be replayed under another history. A script can name units
instead; the harness resolves a name when the command runs, in the game, so it follows whatever ids that run has:

```
5    produce  16 BIG_TRUCK as T1     # `as NAME` ends a produce or build: NAME = the unit that order makes
5    build    $MINER2 22 17 MINE as M2  # a build with a TYPE; op2-inspect: `buildat <id> x y as NAME` too
10   name     M1 $MINER@21,18        # bind an existing unit (id, @N, $NAME or selector) to a name now
700  move     $T1 232 40             # a word `$NAME` anywhere a unit goes
700  route    $T1 $MINE1 $SMELTER    # (names are case-insensitive: letters, digits, _)
710  attack   $TOWER_GUARD@12,34 $TANK_2  # `$TYPE@x,y`: the live unit of TYPE nearest to the tile, within 3 tiles
```

- `as NAME` binds a unit that appears after the order: for `produce`, a new vehicle of the produced type within 8 tiles
  of the factory (if several pending names fit, the one whose factory is nearest takes it, ties to the earlier order);
  for `build`, a new structure (any type: a Mine on a rare beacon becomes MINE_ADV) inside the build rectangle plus
  margin. Only that player's units match. A `produce` on a Structure Factory can't bind (it makes kits) and is refused
  with a warning. A name that finds no unit within 4000 ticks (factory busy, site refused) is dropped with a WARN, so a
  later unit doesn't take it. `harness.log` has `script: name T1 = unit 139 (BIG_TRUCK)` when a name binds, and
  `state.json` lists `names` (name -> id) when any exist.
- A name is letters, digits and `_` (not all digits), case-insensitive; anything else is refused with a WARN. It is
  unbound the first cycle its unit isn't live or isn't the bound type any more, so a new unit that reuses the id doesn't
  inherit it (a unit that dies and is replaced by one of the same type inside one cycle would). `$TYPE@x,y` takes any
  owner and the nearest unit of exactly that type (so `$MINE@x,y` doesn't match a MINE_ADV; tiles in the script's
  coordinate mode, `coords file` included); none within 3 tiles is not found.
- op2-missionhost's `play` (and so `game_verify` and the `mission_play` check) and op2-engine's mission `--script`
  (`netpair` and network games too) resolve names the same way (`op2_missionhost::unitnames`, the rules above, the scan
  after every cycle), so a named script replays there too. op2-engine's scenario scripts take `@N` only.

**An order naming something that can't be resolved is skipped, not fatal.** That is a `$name` or `$TYPE@x,y` that
doesn't resolve, or a plain id past the object table (before: reading it crashed the game, exit 5). Every verb whose
first argument is a unit is covered (the first two for `attack`/`guard`/`repairobj`/`reprogram`/`dismantle`, the first
three for `route`; `scatter` leaves such units out). The log gets
`script: WARN tick T line L: `<the command>`: <reason> (order skipped)`, `L` being the line in the script file, and
op2-inspect's game_step prints it as `! SCRIPT WARNING tick T: ...`. A plain id in range that names a dead unit
is sent exactly as before (the packet may still change the actor's state, as in the recorded runs) and only noted in
the log. `@N` spawns keep their own rule (skipped once they have died).

## Audio log (`--audio`)

`audio.jsonl`, one event per line, from hooks on the game's own sound calls (so it works with the run's sound off):

```json
{"tick":0,"ev":"music","when":"start","songs":[12,13],"repeat":0,"pos":0}
{"tick":20,"ev":"msg","id":97,"name":"savant4.wav","x":976,"y":1008,"at":"4a3a7a","pass":1}
{"tick":20,"ev":"savant","id":97,"name":"savant4.wav","x":976,"y":1008,"at":"opu"}
{"tick":86,"ev":"play","id":97,"name":"savant4.wav","x":-1,"y":-1,"at":"47f236"}
{"tick":338,"ev":"sfx","id":64,"name":"doc_grab.wav","x":496,"y":624,"at":"43e55d"}
```

- `sfx`: AddMapSound (0x47EDB0; AddGameSound 0x47EFD0 goes through it with x = y = -1): an effect. `msg`: a status
  message (MessageLog::AddMessage 0x439070) with its SoundID (0 = none, which beeps) and, for Savant ids, `pass`: the
  throttle's verdict (ShouldAddMessage 0x439590; -1 = not throttled). `savant`: a line handed to the Savant queue
  (AddSavantSound 0x47F010). `play`: the queue starting a line (0x47F200; the harness takes the queue's turn itself on
  ticks with `tick & 3 == 2`, as SoundManager's per-frame update would with sound on). `music`: the MusicManager
  playlist (song numbers of op2.clm, the repeat index, the position) after PrepareGame and whenever it changes.
  `tables` (first line): Savant priorities and every sound's length in ms as the game loaded them.
- `x`/`y` are world pixels in file coordinates (like the trace's `px`/`py`). `at` is the return address of the call in
  Outpost2.exe that asked, or `opu` for OPU's own code. `tick` is the cycle the request came from.
- The throttle and the queue run on timeGetTime; in an audio run those call sites read a virtual clock instead,
  `tick * 1000 / 36` ms (real time at speed 10), so runs are deterministic. Nothing else changes: an audio run's other
  outputs are the same as without `--audio`. `harness.log` notes the time from MusicManager::Init to the mission's
  InitProc.
- The message log's throttle slots are stamped before the harness loads (0x438FE0), from the real clock; at the
  mission's InitProc the harness rebases them to what the log's reset (0x439510) writes at time 0, so the verdicts
  don't depend on the machine's uptime (before that fix, past ~27.7 h of uptime no throttled message passed all run).
- The `tables` line's `prio_ctor` isn't all priorities: the game reuses part of those bytes, including a real
  timeGetTime stamp (0x56E89C), so it changes between recordings; op2-rs accuracy/runs.py leaves it out of the manifest hash.

## Session state (`--state`, `--stop`)

For tools that play a mission by replaying a growing script (op2-inspect's `game_*` tools, op2-rs tools/README.md): a
replay costs about 1 s of wall time at any tick (Wine and Xvfb start in ~0.8 s; measured 2026-10-22: e05's whole
23,448-tick win in 1.0-1.3 s, 40,000 ticks of e06 or e12 in 1.1 s, p12 to its end at 37,652 in 1.4 s; with
`--telemetry-every 1` e05 takes 3.6 s, writing 125 MB), so a session replays its script at every step instead of
keeping a paused game, and its history is exactly the script that reproduces it. Long games replay from a checkpoint
instead of tick 0 (next section): with `--eco` a replay runs at 20k-30k ticks/s (c_euf_win to 46,296 in 2.4 s). Reading the state changes nothing:
the telemetry of a run with `--state` is byte-identical to one without (measured on e05_win; `make check` compares them).

- `events.jsonl`, one line per event, file tiles, written during the run:
  ```json
  {"tick":1783,"ev":"new","id":48,"type":"MINE","owner":0,"x":60,"y":42}
  {"tick":1784,"ev":"gone","id":39,"type":"MINER","owner":0,"x":61,"y":43}
  {"tick":3336,"ev":"idle","id":48,"type":"MINE","owner":0,"x":60,"y":42,"was":"Develop","dest":[-1,-1],"emp":0}
  {"tick":2694,"ev":"tech","id":3405,"name":"Metallogeny"}
  {"tick":5600,"ev":"hit","id":24,"type":"BIG_TRUCK","owner":0,"x":5,"y":0,"dmg":30,"hp":370}
  {"tick":8532,"ev":"disaster","type":"EARTHQUAKE","x":10,"y":13,"timer":0}
  {"tick":9543,"ev":"msg","text":"DANGER: Quake detected at 11,14","x":10,"y":13}
  ```
  `new`: a structure or vehicle of player 0 appeared, `enemy`: one of another player's; `gone`: one of any player
  disappeared (`id` is the object index, which the game reuses);
  `hit`: lost hit points; `idle`: a unit of player 0 went from a command to none (`was`; `dest` where a vehicle was
  going, so a stop short of it shows, and whether it is EMPed); `tech`: player 0 got a topic; `objective`: a victory
  condition's trigger changed (`met`, `text`); `disaster`: a disaster object appeared (the DLL creates it before its
  warnings); `msg`: a status message (MessageLog::AddMessage 0x439070, hooked; `x`/`y` -1 for none); `end`.
- `state.json` at the end of the run (any end: tick limit, `--stop`, the mission's end): `tick`, `end`, `stop` (the
  event that stopped it), `map` (file width, height, padding), `players` (the telemetry fields plus storage, required
  and available workers and scientists, food production and use), `techs` (the tech tree: id, name, lab 1-3, Eden and
  Plymouth cost, max scientists, required ids, `has` = bitmask of players), `typeTopics` (per unit and weapon type,
  the topic that makes it available), `objectives` (`met`, `text`: VictoryConditionImpl's text, read from the
  ScStub list), `units` (every player's: id, type, file tile, hp, cmd, action; vehicles: `truck` [CargoType, amount],
  a ConVec's `kit`, `weapon`, `dest`, `emp`; structures: `flags` (MoFlagBld*: 0x1 active, 0x2000 power, 0x4000
  workers, 0x8000 scientists), `dock`, a lab's `research` {tech, team, left, cost} ($ORACLE/ECONOMY.md section 7), a
  factory's `bays` and `producing`), `gaia` (beacons: ore 0 common / 1 rare, bar 0 = 3-bar, variant, surveyed by
  player 0; magma vents; fumaroles; wreckage: tech, discovered; disasters with their timer, action and flags
  (0x2000 started, 0x4000 first warning done, 0x8000 second warning done)) and `cells` (file
  coordinates row by row: cell type | lava << 5 | microbe << 6 | wall or building << 7 | lava possible << 8 |
  lava/Blight expansion marker << 9).
- `--stop KINDS` ends the run (as a tick limit does, with all outputs) at the first event after `--stop-from` of
  the listed kinds: `new` (player 0's), `enemy` (another player's unit appeared), `gone`, `hit`, `idle`, `tech`,
  `objective`, `msg`, `disaster`, `end`, `any` (all but msg and hit); `kind:<unit id>` waits for that unit, e.g.
  `idle:48`. The stop tick is part of the game, so it is deterministic too.

Learned while building it: wreckage is scavenged by a Cargo Truck driving onto it only once it has been discovered,
and a Scout discovers it by coming near (e06: tanks next to it for 3000 ticks didn't).

## Checkpoints (`--checkpoint`, `--resume`)

A replay from tick 0 costs about 1 s plus the game's time, 20k-30k ticks/s with `--eco` and `--state`: 10 s for a
250,000-tick colony game, and a session makes hundreds. op2-inspect's sessions therefore resume from a save of an
earlier run where the script up to it is unchanged (op2-rs tools/README.md "Checkpoints"); the harness's part:

- `--checkpoint SAVE`: when the run stops with the game going on (its tick limit, a `--stop` event), the game is saved
  to SAVE first, at the end of that tick's cycle (after its script lines, samples and session events), as TApp::SaveGame
  saves (0x4877E0, what the Save dialog calls). `checkpoint: saved SAVE at tick T` in harness.log. Only these saves
  write what follows and put the hives' counts back; a `savegame` line saves as the game does, so the original's own
  behaviour after a save stays measurable.
- Beside the save, `SAVE.dll`: the mission DLL's writable sections (from its PE headers in memory) as they are at the
  save, and each dword as the process's DecodePointer reads it.
- `--resume SAVE`: at the first game cycle, before any script line runs, TApp::LoadGame (0x4878F0, what the Load dialog
  calls) loads the save; `resume: load SAVE at tick 1: ok, now tick T` in harness.log. The run then goes on as the
  cycle of tick T: the script's lines (all must be at T or later; earlier ones are skipped with a WARN) at T run right
  after the load, where they would have run after the saved cycle, then the samples at T (telemetry, trace) and the
  session's events. The session state starts from the loaded game without events (no `new` for every unit).
- After the load the harness compares the mission DLL's memory outside its save region (GetSaveRegions, as
  MissionManager keeps it) with `SAVE.dll`. **A load loads the mission DLL again** (its CRT draws a new security
  cookie; measured on c_euf.dll), so every global the DLL doesn't save starts over from the DLL file's values. Each
  dword that differs is classed, in this order:
  - **put back** as saved: the loaded value is still the DLL file's (nothing set it since the load) and the saved one
    is a small number (below 64 KB, or a small negative) or an address in a module mapped here (Outpost2.exe's, the
    DLL's own: the same in every process, e.g. a function pointer TethysAPI looks up on first use): a global the
    mission set during play, so the DLL goes on as the continued game's would. Only for a DLL at its preferred base
    (else the file's pointers aren't the loaded ones);
  - **lost** (mission state not put back): other plain numbers below 4 MB (unless the loaded one is an address
    mapped here: then both are heap addresses, process-only), a value this process set itself (a handle,
    a TLS index, a CRT counter) or possibly a low heap address (Wine allocates below 4 MB), where the other process's
    value would be wrong; and whatever is left after the next two;
  - process-only: the CRT's cookie and values it encodes (XORed, or a pointer encoded with the cookie or with
    EncodePointer, decoding to addresses), and heap addresses (both values addresses: a process allocates elsewhere).

  `checkpoint: dll same | process-only | restored: ... | DIFFERS: ...` says which; with any lost dword op2-inspect
  checks the session against a replay from tick 0 at once and then more often. c_euf.dll keeps two such globals
  (0x11027780 and 0x11027784, which its unit-creating helpers pass on as arguments: 2,1 saved against the file's
  0,6), which a resume puts back.
- **The hives' counts.** A save doesn't hold the tube networks (HiveManager): saving rebuilds them in the game that goes
  on (new slots in HiveManager's array) and loading builds them afresh, both with each hive's counts (Command Centers
  +0xC, an enabled one +0x10, DIRTs +0x14) at zero until its owner's next allocation pass recounts them ($ORACLE/ECONOMY.md
  "Tube network"; a computer player's pass comes every 256 ticks): p12's Eden read [1,0,0,0] instead of [1,2,1,1]
  for 140 ticks after a save at 5000, **with no load**. DoDamage's Damage Prevention reads them in that window
  (0x430050). So a checkpoint save notes each structure's hive counts first, puts them back after SaveGame, and
  writes them beside the save (`SAVE.hive`); a resume puts them back after the load (`checkpoint: hives: the counts
  of N structures' hives put back`). Only the slot numbers stay different (they decide nothing measured).
- What a save doesn't hold of the harness's own state: unit names (`as NAME`, `name`), spawn indices, group names,
  the UI (selection, view) of input verbs, a `packet` queued for the next cycle. op2-inspect doesn't resume past such
  lines.

**Fidelity** (2026-11-06, `python3 test/checkpoint_check.py --mission M --script WIN --at T1,T2,..`: a
reference run of the win script, runs that save at each T, and a run per T resumed in a new process, compared with
the reference from T on: telemetry (`--eco` every 10 ticks), trace (every 4 ticks; every 20, the
checker's default since a whole-run trace at 4 is gigabytes, in the last pass of c_euf, ml4_01 and the split),
events, the final state.json; the checker streams the files and peaked at 24 MB):

| mission | script | resumed at | result |
|---|---|---|---|
| e01 | e01.txt | 1000, 2000 | exact to the win at 3000 |
| cResEsc | cResEsc.txt | 5000, 15000 | exact to the win at 17792 |
| e10 | e10.txt (e10_campaign.txt) | 12000, 30000, 45000 | exact to the win at 48720 |
| p12 | p12_partial.txt (p12_campaign.txt) | 5000, 15000 | exact to 27400 with the hives' counts put back (before: Eden's counts 0 for 140 ticks after each save) |
| c_euf | c_euf_win.txt | 10000, 30000, 45000 | exact to the win at 46296 (the two DLL globals put back) |
| c_euf, `--split` | c_euf_win.txt | 40800 (the assist and that tick's orders run after the load) | exact to the win |
| ml4_01, `--players 2` | ml4_01.txt | 2000, 6000 | exact to the Victory at 12464 |

The Sierra DLLs (e01, e10, p12, ml4_01) each have one dword the check can't class (e.g. p12.dll 0x11012268, 0xF0F03B01
saved against 0xF0F03801): by the code around it (the list at 0x11012258) the MSVC CRT small-block heap's per-page
free counts, not mission state; op2-inspect then checks the session against a replay from tick 0 sooner. p12_win.txt
binds unit names (`as NAME`) before 10000, which a save doesn't hold: resumed there, its `$NAME` orders were skipped
and the game crashed at 0x42324F (an order to a dead unit) at 29240; op2-inspect never resumes past such lines.

"Exact": the trace (positions, actions, every unit field it writes) identical at every sample, the telemetry identical
every 10 ticks, the events identical, the final state.json identical, the win at the same tick, apart from:
- the structure hives' slot numbers in HiveManager's array (telemetry `hive`'s last value): **a save itself renumbers
  them** in the game that goes on (c_euf, from the first sample after a save to the end, with nothing else differing),
  and a load numbers them its own way (their counts are put back, above); and each unit's MapObject +0x7C (a heap address for most units; a Garage's
  goes from -2 to 0x40 when the game is saved) and other heap addresses in the raw bytes (as record_check.py masks);
- the telemetry record at the checkpoint's tick itself, taken right after the load: each player's recalc flag
  (PlayerImpl dword 40) is set until the game's next cycle (from the next record on it is the continued game's);
- status messages (`msg` events): MessageLog's throttle runs on the real clock, so they differ between any two runs of
  different speed, resumed or not (two plain runs of c_euf_win, with and without `--trace-every 4`: 10 messages apart
  by tick 40000).
`test/checkpoint_check.py --split` leaves the script's lines at T out of the save and plays them after the
load (orders given at a checkpoint's tick).

## Recording your own play (`--record`)

You play a mission of the original by hand and your game becomes a harness script: a win script, an oracle run and
a comparison with a reimplementation. Linux only for now (it needs an X display: `--display`, or `$DISPLAY`, which
gamescope sets).

```sh
./run.sh --record --mission p12.dll --campaign $ORACLE/missions/p12_campaign.txt --desktop 1088x816 --out recordings/p12
# -> recordings/p12/recording.txt, plus telemetry.jsonl and harness.log of the session (not committed)
```

op2-rs runs this under gamescope with `make -C missions record MISSION=p12.dll`.

- **How it starts.** `run.py --record` loads the harness as for any scripted run (same `StartMissionHook`: seed 1,
  normal difficulty, `--campaign` carry-over, player name, start info), in **prefix-harness, never prefix/** (the
  prefix you play from isn't touched; mission DLLs are mirrored from it as for every run). The game runs in real time with
  the frame limiter, sound and music as prefix-harness's ini has them (`--silent` turns them off), the real mouse and
  keys (no virtual cursor, no forced focus), the game's own questions answered by the player (the message box isn't
  intercepted), and no tick limit or timeout. It refuses to run when the prefix resolves to prefix/ (no prefix-harness,
  or `OP2_HARNESS_PREFIX` pointing there). It holds OPU/saves's lock for the session (`shell.py --clean-saves` waits for
  it, saying who holds it; the lock file `OPU/saves.harness-lock` names the holder). `--desktop 1088x816` runs it in a Wine virtual desktop
  around the 1024x768 game, so its dialogs stay inside (for gamescope). An
  optional `--script` is a setup script (it runs as in any run and is copied to the top of the recording); its input
  verbs run through the harness's virtual cursor and keys, as in a replay, and the real mouse and keys take over once
  the last of them has run (telemetry then has the player's own `view` and `selected`, which a replay doesn't share).
  A recording played on as a setup isn't recorded twice: packets the `packet` verb injected are left out.
- **What is captured** (harness.cpp "Recording a player's own game"). The UI never runs an order: each command writes its
  packet into the local player's pending slot of the command queue (0x490810), and `GameImpl::ProcessGameCycle`
  hands each player's slot to `PlayerImpl::ProcessCommandPacket` (0x40E300) at 0x489FF5 on every 4th tick, before the
  units act. The harness hooks that call: every packet of the local player that takes effect is logged with its tick,
  after any overwrite in the slot. ctNop and ctMachineSettings (machine speed, memory and window size, which the frame
  sends itself every 32 ticks and only a debug line reads) are left out. Selection, scrolling, the pane's pages and
  other UI that sends nothing don't change the game and aren't recorded.
- **The script.** One line per packet, `T packet TYPE HEX [+TAIL]  # comment`: the command type (1-53) and its
  `dataLength` data bytes, and TAIL, the slot's bytes after them up to its last nonzero one (what earlier packets left
  there; handlers read by their own counts, so the slot's whole 99 data bytes are kept), run by the replay in the cycle
  that starts at tick T (a multiple of 4): the `packet` verb puts those bytes into the slot (the rest zero) just as
  `ProcessCommandPacket` reads it, so the same bytes run at the same point of the same cycle. A `packet` whose cycle
  doesn't hand the slot over (a cycle that skips packet processing) is dropped with a WARN, and shell mode refuses
  `packet` lines (no hook there). The comment says what it
  does with the units as `id(TYPE@x,y)` (file tiles, when it ran): `= verb ...` when a harness verb builds the same
  bytes (one unit, tile centres), `~ verb ...` for the nearest verb (the UI's Move carries the clicked pixel, not the
  tile's centre, and a selection gives one packet for every unit), else a description (`GameOpt 20 = 40` for a game
  speed change). Unit ids are the game's own; they replay because the history is the same. Quitting (ctQuit) is kept as
  a comment only. The header has the replay command line; the last line says how the session ended.
- **Saves and loads.** Save as often as you like: TApp::SaveGame (0x4877E0) and TApp::LoadGame (0x4878F0) are hooked.
  A save writes a sidecar folder beside the save, `OPU/saves/SGAMEn.OP2.rec/` in prefix-harness (packets.txt, the
  telemetry so far, meta.txt with the tick, the save file's size and FNV hash, and the session's identity: mission,
  seed, difficulty, carry-over, setup script without its `savegame`/`loadgame` lines; written last, only when the rest
  was written). A load puts them back, so recording.txt is always the one linear history that led to the loaded game, and the
  abandoned branch is gone (`# load ...` header lines say what each load rolled back; harness.log has every packet that
  ran, abandoned ones included). Slots save the pending orders too: an order clicked just before a save runs after the
  load and is recorded then. Loading a save whose sidecar is missing, of another start, of another file (the slot saved
  over without a recording: the shell, plain play, a run that isn't a recording) or of another tick marks the recording
  `# BROKEN: ...` (it no longer replays). A failed save, and a `savegame` in a run that isn't a recording, delete the
  slot's sidecar. recording.txt is rewritten whole after every change (a temporary file renamed over it). A new session in the same `REC_OUT` keeps the previous files as
  `recording-<time>.txt`; loading a save from an earlier session resumes that history.
- **Turning it into a win script and an oracle run.** Copy recording.txt to
  `$ORACLE/missions/<m>.txt`, write the header (what the mission asks, the plan as played; keep the packet lines),
  then replay it: `./run.sh --mission <m>.dll --campaign <carry-over> --script $ORACLE/missions/<m>.txt
  --ticks 0 --out /tmp/<m>` must end as the session did (MissionAccomplished at the recorded tick), and its
  telemetry.jsonl must equal the session's (same `--telemetry-every`: 100 by default) line for line. Then add `[runs.<m>_win]` to
  op2-rs accuracy/recordings.toml, op2-rs `tools/agent/record <m>_win`, and `op2-inspect game_carry_over` for the next mission's
  carry-over. To play on from the recording's end through op2-inspect (a partial game), `game_start mission=<m>
  script=<recording> tick=<end>` takes it as the session's base script.
- **Check:** `python3 test/record_check.py --out DIR` (about a minute; Xvfb, real X input through shell.py's
  `X`): plays e01 with clicks, SAVE GAME, two more orders, LOAD GAME and a different order, then replays the recording
  and requires the same executed packets, byte-identical telemetry and an identical trace (frame/anim aside), the
  abandoned packets gone, and op2-inspect's game_start to start from the recording at its end with the same units.
  `--script WIN --loads S:L,...` (with `--mission`, `--campaign`, `--ticks`) instead plays a win script in a
  `--record --fast` session that saves at tick S and loads that save at tick L (script verbs `savegame NAME` /
  `loadgame NAME`, which run once each, after the cycle's samples, and stay comments in the recording), and compares
  its `--eco` telemetry with the plain replay: whether saving and loading are faithful for that mission (MapObject +0x7C
  masked, the hive slot numbers reported apart). `--input` checks a setup script with input verbs in a recording.
- **Comparing a recording with op2-rs.** op2-missionhost reads `packet` lines: `op2-missionhost play e01.dll --script
  recording.txt --compare DIR` (DIR: the session's own folder, or a replay of it with `--telemetry-every 10 --trace-every 4`)
  decodes each packet's bytes (`op2_sim::original_packet`: unit ids, internal pixels to file tiles, map ids to unit kinds,
  the research index to a tech id) into the op2-sim command a UI click makes, queues it for the cycle that starts at the
  line's tick and plays the mission on, so the compare tools (`op2-inspect compare_run`, `game_verify missionhost=true`,
  op2-rs `tools/agent/missioncompare`) work on a recording as on a win script. A packet op2-sim has no equivalent for (Transfer,
  Ally, GoAI, Guard on a tile, most GameOpt cheats, a stored-group selection) is listed in the summary as `unsupported
  packet: tick T ...` and not run; chat, machine settings and packets for units that no longer exist are left out
  silently. Packets only reach a cycle that is a multiple of 4 (a `packet` line at another tick is dropped with a note,
  as the harness does). Run `e01_ui_packets` is such a recording (three UI Moves at click pixels) and replays exactly.
  A verb script can stand in for a recording: `python3 tools/packetize.py SCRIPT [--gamedata RUN/gamedata.json]
  [--max-delay N] [--keep VERBS]` rewrites its orders as `packet` lines (each at the first free command cycle after the
  verb's tick); runs `e01_packets`, `e02_packets`, `e03_packets`, `e04_packets`, `e05_packets`, `p02_lose_packets` and
  `p03_packets` are such scripts played in the original and in op2-missionhost, all exact (Move, Dock, LoadCargo,
  TransferCargo, Produce, Research, Train, Idle, Unidle, BuildWall, Attack, ...). Packetized e10, e11 and p10 (with
  their Attack orders) were not exact and aren't kept as runs: leads in op2-rs notes/findings-commands-audio.md.
- **Limits.** Only the local player's packets are recorded (single player). The `= verb` and `~ verb` comments of a
  recording are for reading; op2-missionhost reads the hex.
  A load restores the game as the save holds it; across the p08, e05, e10, p12 and e01 scripts a loaded game went on
  as the saved one would have (the plain telemetry byte for byte, the win at the same tick) apart from internal values
  in `--eco` telemetry: every unit's MapObject +0x7C, a heap address (0x06xxxxxx, 0x78 apart from unit to unit) that a
  loaded game allocates 2 MB away, and the structure hives' slot numbers in HiveManager's array, which stay different to
  the end without anything else differing; and in p12, Eden's hive counters read 0 for up to ~140 ticks after a load
  (after a save too, in the game that goes on: "Checkpoints" puts them back for its own saves; a recording's saves
  are the player's and stay as the game makes them).
  The measured scope is in op2-rs notes/uncertain-claims.md ("a load replays like continuing"). Sound on during a recording hasn't been tested for
  determinism (the checks run silent; replays are always silent): compare the first real recording's replay with its
  telemetry. Other harness runs share prefix-harness's wineserver and can take the foreground (input runs do): don't
  run input-verb runs while someone records. Mission dialogs from a DLL are suppressed as in every run.

## Disasters

Script verbs that schedule a disaster the way a mission does (the TethysGame::Set* exports; $ORACLE/FINDINGS.md
"Disasters"). A trailing `now` skips the two 500-tick warnings, as the API's immediate flag does.

```
5  meteor    30 30 2                # x y size (0 small .. 2 large, -1 random)
5  quake     20 20 1                # x y magnitude
5  eruption  40 40 5                # x y lava speed
5  lightning 20 20 10 60 30         # x1 y1 duration x2 y2
5  tornado   20 20 16 60 30 now     # x1 y1 duration x2 y2
5  blight    40 40 [speed]          # GameMap::SetVirusUL; lavaspeed n; lavapossible x1 y1 x2 y2 0|1
5  empmissile 0 0 1 60 50           # TethysGame::SetEMPMissile: from x y, owner, to x y (an EMP missile, e12's FireEMP1)
```

## Group verbs

`group <name> <op> [args]` calls the mission API's AI group exports on a group the script names (op2-sim's script
runner understands the same verbs, `op2_sim::groups`):

```
1 group g new fight 1               # CreateFightGroup(Player[1]) (also mining, building)
1 group g guardrect                 # DoGuardRect (patrolonly attackenemy guardgroup guardunit attackunit exitmap)
1 group g rect 30 25 36 30          # SetRect; guardedrect x1 y1 x2 y2 = AddGuardedRect; clearguarded
1 group g patrol 0 20,20 34,20      # SetPatrolMode(loop, waypoints); clearpatrol
1 group g take @0 @1                # TakeUnit; remove <unit>; lights 0|1; enable; disable; destroy
1 group g combine 1                 # Set/ClearCombineFire; follow <mode>; targetunit <unit>; targetgroup <name>; attacktype TYPE
1 group m mine @3 @4 x1 y1 x2 y2    # MiningGroup::Setup(Unit, Unit, MAP_RECT)
1 group g targcount TYPE WEAPON n   # SetTargCount; cleartargcount; reinforce <group> <priority>; unreinforce <group>
1 group b building x y TYPE WEAPON [group]   # RecordBuilding; tube x y; wall x y [TYPE]
1 group g dump                      # harness.log: members in list order (unit:class@issue tick), fight group state
```

## Scenarios (`--scenario`): oracle runs of the original game

`--scenario FILE` runs a scenario in the op2-rs ENGINE_PLAN.md format against the real game, so op2-sim can be compared with
it:

```json
{"map": "cm01.map", "seed": 1, "ticks": 1400,
 "setcell": [{"rect": [10, 20, 50, 24], "cell": 7}],
 "spawn": [{"type": "LIGHT_TANK", "owner": 0, "tile": [12, 22], "weapon": "LASER", "dir": 0}],
 "orders": [{"tick": 10, "unit": 0, "cmd": "Move", "tile": [42, 22]}]}
```

- Tiles are file coordinates. `type` and `weapon` are sheet code names. `dir` (optional) is the starting rotation.
  `orders[].unit` is a spawn index. `cmd` is Move, Dock (with `tile`), Attack (with `target`, a spawn index, or a
  `tile`), Guard (`target`), Lights (`on`), StandGround or Patrol (`tile`, or `waypoints` as `[x, y]` / `[x, y, range]`;
  Patrol also `loop`), Stop or SelfDestruct. Move takes `waypoints` too.
  `view` (optional tile, default the spawns' centroid) centres the camera for screenshots.
  `techtree` (optional, default `multitek.txt`) is the tech tree the scenario mission loads (run.py patches its
  TechtreeName; up to 12 characters, e.g. `edentek.txt`).
  `setcell` (optional) repaints cell types before spawning (`cell` is a CellType id or name). `note` is a comment. op2-sim refuses a key that neither it nor the harness reads (op2-sim `Scenario::parse`, `HARNESS_KEYS`), so add a new harness-only key there too.
- run.py turns the scenario into a script (`scenario_script.txt` in the output) and runs the scenario mission
  (`test/Scenario.cpp`): an empty colony on `map`, with player 0 a human Eden player, player 1 an idle Plymouth AI,
  and daylight everywhere. Spawns happen at tick 1, the first harness cycle. The trace starts there and samples
  every tick.
- The game reads a mission's `MapName` from the DLL file without loading it (MissionManager::GetModuleDesc), so
  run.py writes one patched copy of `zScenario.dll` per map, as `OPU/Harness/zs_<map>.dll`. Missions in
  `OPU/Harness` never appear in the game's mission lists.
- A scenario run takes about 1 to 3 s. `$ORACLE/` (in op2-rs) holds the reference scenarios (`make_scenarios.py`
  writes them; op2-rs's `tools/agent/record NAME...` records them into `$ORACLE/runs/`, which is never committed).
  Measured results are in `$ORACLE/FINDINGS.md` and op2-rs `notes/findings-*.md`. On Windows, `windows/run_win.py run`
  plays one. Any scenario file works: the recipes don't have to be op2-rs's.

Recording a scenario as video frames: `--screenshot-every 1` (or 2, 5...) saves a PNG per sampled tick of the view
centred on the units, about 20 frames/s of wall time. Assemble them with e.g.
`ffmpeg -framerate 25 -pattern_type glob -i 'shot_*.png' out.mp4`.

## Shell mode (`shell.sh`): the original's menus and movies

`shell.sh` boots the game normally (intro video, title, OP2Shell menus, briefings, movies) in a Wine virtual desktop
on a private Xvfb, drives it with real X input (XTEST) from a driver script, screenshots the whole screen and logs
what the game does. The startup flow it measured is in op2-rs `notes/findings-shell.md`.

```sh
./shell.sh --drive $ORACLE/shell/eden1.txt --script $ORACLE/missions/e01.txt \
  --ini Music=0 --ini GameSpeed=10 --audio --out /tmp/shell     # about 3 minutes: the movies play in real time
```

| option | |
|---|---|
| `--drive FILE` | driver script (below) |
| `--out DIR` | output directory |
| `--desktop WxH` | virtual desktop and X screen, default 800x600; the game's 640x480 window is centred in it |
| `--ini KEY=VALUE` | a `[Game]` key in the run's private outpost2.ini (repeatable): `IntroAVI`, `ShowMovies`, `Music`, `Sound`, `GameSpeed`, `Demo` ... |
| `--script FILE` | in-game tick script (the order verbs of "Scripted input", e.g. a mission's winning orders); its ticks count from each game's start. Input verbs don't apply: the driver's real input does |
| `--audio`, `--presents`, `--telemetry-every N` | in-game audio.jsonl, present.log and telemetry.jsonl, as in mission runs |
| `--timeout S` | wall-clock limit (default 600) |
| `--clean-saves` | run with an empty `OPU/saves` (the SAVE and LOAD dialogs list SGAME*.OP2 from there): the old saves are set aside and put back after the run, and the run's own go to `<out>/saves` (`shell_saveload`); such runs take turns (a lock file beside the folder) |
| `--window WxH\|max` | the window's size: [GAME_WINDOW] of prefix-harness's OPU/outpost2.ini set for the run (at the desktop's top left, [DESKTOP] the run's desktop; `max`: MAXIMIZED=1) and put back after; such runs take turns. Make `--desktop` at least as large. Wine rounds the window up to a multiple of 32 px, so pick multiples of 32 (`shell_window_*`, op2-rs notes/findings-shell.md "The shell window's size") |

Driver steps, one per line (`#` comments; times in wall-clock seconds):

```
wait <ev> [field=value | field=~regex ...] [timeout=S]   the next shell.jsonl event of kind <ev> whose fields match
sleep <S>                                                 e.g. to let a movie play
shot <name> | shots <name> <count> <every S>             screenshots, cropped to the game's window
click <x> <y> [right] | move <x> <y>                     desktop pixels
unfocus | minimize | restore                              focus another window, minimize, or restore the game window
ctl <text | id=N> [in=<window regex>] [right]             click a control's centre ('_' = space, '&' ignored)
key <keysym> ...                                          X keysyms: Escape Return space n alt+n Down F1 ...
dump [tag] | mark <text> | quit
state [tag] [after=N]                                     the game's tick, pause flag, active/iconic state, cursor handle,
                                                          selection with the units' commands, and view: running, read on
                                                          the game thread N ticks on; paused, at once (unlike telemetry)
```

Outputs: `shell.jsonl` (below), `driver.jsonl` (every step with its time on shell.jsonl's clock), `shot_<name>.png`
(640x480, the game's window), `flow.json` (the deterministic part of shell.jsonl: dialogs and their controls,
layouts from the `dump`s in window coordinates, state messages, files, movies and how they ended, game start and
end, the `state` probes without their ticks; byte-identical between runs), `summary.json` (movies with timings, the window rectangle), the private
`outpost2.ini`, `harness.log`, `wine.log`. Shots of static screens (named `ref_*` by convention) double as reference
images: they reproduce too, except that dialog text (OPU's fonts) occasionally lands 2 px lower in one run than
another, probably a race between OPU loading its private fonts and the first dialog, so compare with a tolerance.

`shell.jsonl`, one event per line with `ms` since the module loaded (harness.cpp "Shell mode"):
- `mci`: every `mciSendCommandA` of OP2Shell.dll (the movies through the `avivideo` device, `music1.wav` through
  `waveaudio`), `notify`: MM_MCINOTIFY as the shell gets it (1 = played to the end).
- `post`/`send`: the shell's state messages (0x720 main menu, 0x72B play the movie in this+0x53C ...).
- `modal`/`modal_end`/`modeless`: IDlgWnd::DoModal/DoModeless (0x431A80/0x431BF0) with the dialog resource and its
  DLL, the caller and the return value (the next state); `dialog` (after WM_INITDIALOG, with every control),
  `show`/`hide`/`destroy` for top-level windows.
- `file`: ResManager::OpenStream/GetFilePath asked by the shell; `lib`: its LoadLibraryA; `input`: key and button
  messages as the game receives them; `start`: TApp::StartSingleGame's GameStartInfo; `activate_shell`:
  TApp::ActivateShell (0x486DB0), the game handing back to the shell with its end reason; `game`: first cycle, every
  500 ticks, the end reason changing; `script`: `done` after every tick-script command has run (the first-cycle event
  precedes these commands, so wait for `script` when the setup matters); `dump`: windows and controls on request;
  `state`: the tick, the pause flag (0x56E9E0) and count, active and iconic flags, the cursor's handle (to compare
  within a run), the selection (0x547210) with each unit's command, and the detail view's top-left corner in file pixels, on request:
  running, by the game thread at the end of a cycle `after` ticks on (one cycle's state, a tick count rather than
  wall time); paused or outside a game, by the control thread at once. Probe what an order did after enough ticks
  for it to be under way and not yet over, so flow.json stays the same from run to run.

Notes: the program must be started by its full Windows path (`wine explorer /desktop=NAME,WxH Outpost2.exe` with a
relative name silently starts nothing). The harness Wine has no audio device, so MCI `waveaudio` fails at once and the
shell's music loop (close and reopen music1.wav on every notification) floods the message queue, starving the main
menu's timers and the driver's input: use `--ini Music=0` (the default ini plays music). Movies play at the right
speed without a device. Each run uses its own desktop name, so parallel runs on other displays don't share it.

## Network games (`netgame.py`, `shell.py --net`)

Two to six copies of the original play a LAN game over 127.0.0.1, each logged from inside (op2-rs docs/multiplayer.md
"Stage 2a"):

```sh
cp -a ../prefix-harness ../prefix-net && make install HARNESS_PREFIX=../prefix-net   # once
OP2_HARNESS_PREFIX=$(realpath ../prefix-net) ./netgame.py --out /tmp/net --ticks 3000 --turbo \
  --script0 $ORACLE/net/ml2_08_p0.txt --script1 $ORACLE/net/ml2_08_p1.txt
OP2_HARNESS_PREFIX=$(realpath ../prefix-net) ./netgame.py --out /tmp/net3 --players 3 --mission 3P --vehicles 4 \
  --seed 3003 --eden 1 --ticks 5000 --turbo --script0 ... --script1 ... --script2 ...      # recordings.toml net_ml3_08
op2-missionhost netlog /tmp/net --bundle /tmp/net/desync.tar.gz       # op2-rs: replay and compare
```

- Shell-mode runs boot the game in their own Xvfb and virtual desktop in **one prefix**: the host creates a TCP/IP
  game, the joiners join 127.0.0.1 one after another and tick Ready, the host starts it; the drivers (written into OUT
  as `host_drive.txt`, `join_drive.txt`, `join2_drive.txt`, ...) wait for each other with shell.py's `touch` and
  `waitfile` steps in `--sync DIR`. Use a prefix of its own (`OP2_HARNESS_PREFIX`; netgame.py refuses prefix/), not
  prefix-harness, whose wineserver other tools share. About a minute per game, most of it the boots and the lobby.
- The lobby: `--players N` (NEW NETWORK GAME's Max Players), `--type` (its Scenario Type: Last One Standing, Midas,
  Resource Race, Space Race, Land Rush), `--mission` (a regex on GAME SETTINGS' scenario list of that type; default its
  first, 2P LoS 'Ambush Alley', ml2_08.dll), `--vehicles` (Initial Vehicles), `--eden 0,2` (who ticks Eden; the rest
  play Plymouth), `--dll` (the mission DLL the scenario must be: netgame.py fails when any copy's start_info.json
  differs from what was asked: seed, DLL, players, Eden boxes, vehicles). shell.py's `pick` step chooses a combo box entry with the keyboard and checks it (the window dumps
  list each combo box's `items` and `sel`). Player k's units are the scenario's slot k; their ids depend on the seed (the
  bases' layout), so write a script for a fixed `--seed` (check the ids with `op2-missionhost play DLL --start-info`).
- `--speed 10,3`: each copy's Game Speed preference (its private ini's GameSpeed; one value for every copy). Every
  shell-mode game writes a `speed` event to shell.jsonl at its first cycle and whenever GameImpl's speed changes, and
  summary.json's `pacing` gives each copy's speed spans with the ticks per wall second it ran at (from net.jsonl's
  datagram times; meaningful without `--turbo`). Run `net_speed` (op2-rs docs/multiplayer.md "Game speed").
- Chat and other input verbs (`key enter`, `type TEXT`, `key enter`) work in the tick scripts as in single player: the
  chat line's ctChat goes through the turns like an order. Every shell-mode game writes each status message to
  shell.jsonl (`msg` events: tick, SoundID, place, text; a chat line is "NAME: text" with SoundID 93), which
  op2-missionhost compares with op2-sim's chat lines per machine. A script with input verbs adds each machine's own
  `view` and `selected` to its telemetry; netgame.py's comparison of the copies leaves those out. Run `net_chat`
  (op2-rs docs/multiplayer.md "Chat").
- `--seed N`: every copy starts from seed N instead of the lobby's (harness OP2H_NET_SEED; the PrepareGame hook in
  harness.cpp "Network games"), so the game can be recorded again from its arguments: in op2-rs, a recordings.toml `netgame` run
  whose `args` hold `--seed` is recorded by its runs.py (`tools/agent/record --replace NAME` with `OP2_HARNESS_PREFIX`
  set to prefix-net).
- `shell.py --net` (harness.cpp "Network games") writes `start_info.json` (the lobby's GameStartInfo at the first
  cycle), `packets.jsonl` (every player's command slot as it executes, every 4th tick) and `net.jsonl` (every UDP
  datagram, sent and received), and makes a `--script`'s orders the local player's own commands: their packets go into
  the player's pending slot, as the UI's do, and cross the network. Orders for another player's units are skipped;
  verbs that change the game directly (spawn, setcell, poke, ...) would make the copies differ. One order per player per
  4-tick turn; an order given at a tick that is a multiple of 32 takes the slot of that turn's machine settings
  (executed at ticks 8 mod 32) and replaces them, harmlessly: harness.log WARNs when an order replaces a pending command
  or a queued order didn't run. `packet` lines are refused (they would run on one
  machine only). `--turbo` lifts the frame limiter (the turn exchange still keeps the copies in lockstep) and draws the
  panes 4 ticks in 256, and every tick from `--draw-from TICK` on (netgame.py passes its `--ticks`, so the closing
  screenshots are current).
- OUT/summary.json says whether every copy executed the same orders as the host and kept the same telemetry on every
  tick both have (they quit a few ticks apart in turbo). Recorded games: the `net_*` runs in op2-rs accuracy/recordings.toml;
  net_ml2_08 (the lobby's own seed) isn't re-recordable: runs.py checks its files and refuses to record it.
  op2-accuracy's missions check replays every one (metric `net.<run>.exact_ticks`, scoreboard row "Network games of
  the original replayed").

## Coordinates

- Internal tile x = file tile x + 32 on non-wrapping maps (`GameMap::GetPaddingWidth()`), and + 0 on 512-wide world
  maps, which wrap. Internal y = file y. Pixel = tile * 32 + offset, with unit centres at +16. The status bar shows
  file + 1 on both axes. This was verified for cm01 and newworld by comparing the game's whole cell grid with the
  .map file.
- Exact position: while a vehicle steps between tiles, `PathContext::numPixelsMoved{X,Y}` hold the 16-bit fraction
  of a 16.16 position, biased by 0x8000, around `pixelX_/pixelY_`. They read 0 when no step has started.

## Bot DLL (`--bot`)

A bot is a DLL that exports `extern "C" void __cdecl HarnessBotTick(int tick)`. The harness calls it after every game
cycle, and it can use all of TethysAPI to read state and give orders. `bots/sample_bot.cpp` sends player 0's idle
tanks at the nearest enemy. Build it with `make`, then run with `--bot build/sample_bot.dll`.

`bots/easter_bot.cpp` reproduces Dan's Dog (`EGG_MODE=dog`, with `test/egg_dandog.json`) and shows the unused Santa
(`EGG_MODE=santa`, with `test/egg_santa.json`) in the original; see op2-rs `notes/findings-easter-eggs.md`.

`bots/hitmask_bot.cpp` writes `hitmask.jsonl`: for every pixel round each unit, what a click there selects and what a
command targets, asked of the game's own point query at the scenario's `hitTicks` (`$ORACLE/input/make_hit.py`,
op2-rs `notes/findings-input.md` "What a click hits").

All orders, from scripts or bots, should use command packets: `Unit::Do*` helpers that build a `CommandPacket`, or
`PlayerImpl::ProcessCommandPacket`. Avoid TethysAPI's `Unit::DoMove()`. It calls `MapObject::CmdMove()`, the AI/ScGroup
path, and on a human player's unit it crashed the game as soon as a frame was rendered. The harness `move` verb builds
the Move packet itself (see `IssueMove`).

Commands like these run immediately on the local player, which is fine in single player. In multiplayer they would
desync: real player input is queued through the network layer (`GameNetLayer`) and runs on every machine at the same
command-processing tick. A multiplayer-safe bot would have to queue packets there.

## Comparing with op2-rs

op2-rs plays the same recipes in op2-sim and compares them with a run of the original, field by field and tick by tick.
Its tools come from a built op2-rs checkout (`cargo build -p op2-inspect -p op2-engine -p op2-missionhost` in its
`tools/`), with `OP2_DIR` set to the game.

- **A scenario:** `op2-inspect compare --scenario $ORACLE/scenarios/S.json --oracle RUN --out DIR`, where RUN is this
  harness's output for that scenario. op2-sim loads RUN's `gamedata.json` as its rules (unit stats, terrain speeds),
  replays the scenario, and prints `MATCH` or each trace field's first divergence, with a trajectory plot
  (`DIR/plot.png`). Without `--oracle`, op2-inspect runs the harness itself (Linux, inside an op2-rs checkout).
- **On Windows, in one command:** `python -I windows/compare.py $ORACLE/scenarios/S.json --tick T` runs the original,
  the compare above, and op2-engine's picture of tick T, and writes `side_by_side.png` (windows/README.md).
- **A mission** (a win script or a recording): `op2-missionhost play M.dll --script S --compare RUN`, RUN being this
  harness's run of the same mission and script (`--telemetry-every 10 --trace-every 4` for a fine comparison).
- **Many random scripts:** `op2-inspect fuzz` ("Differential fuzzing" above).
- **op2-rs's accuracy suite** (`make accuracy` there) compares every recorded run of the original; its
  `accuracy/recordings.toml` lists each run's recipe and `accuracy/manifest.json` the hashes anyone with the game can
  reproduce.

The outputs are plain JSONL and PNG (see "Outputs"), so another reimplementation can be checked the same way: play the
recipe, write the same fields, compare. Marcelo Gonçalves's C++/SDL2 port
([marcelomgoncalves/outpost2](https://github.com/marcelomgoncalves/outpost2), and its fork outpost2-port) does this
through its `--harness` mode: `windows/compare_port.py` compares it with the original (and op2-rs) and draws the
three side by side, and `windows/trace_diff.py` compares any two traces (windows/README.md).

## How it works

`src/harness.cpp` builds `OPU/Harness/harness.dll`, an op2ext ini module listed in `OPU/outpost2.ini`
(`[ExternalModules] Harness=yes`). op2ext calls `InitMod` from its `TApp::Init` hook. If the environment has no
`OP2H_MISSION`, `InitMod` returns at once and the game behaves normally. Otherwise it:

1. Points the game's settings object (`g_configFile.iniPath_`) at a copy of `outpost2.ini` in the output directory,
   with music and sound off and window geometry set. The game writes its window and desktop size back to the ini while
   it runs, and this keeps those writes away from the user's settings.
2. Sets `TApp::bShowShell_ = 0`, the same thing the hidden `/noshell` switch does. `TApp::CreateFrames()` then calls
   `GameImpl::LoadDebugMap()` instead of opening the shell. The harness redirects that call (0x486606) to a function
   that fills a `GameStartInfo` (script name, seed, speed, difficulty) and calls `GameImpl::PrepareGame()`, which is
   what `TApp::StartSingleGame()` does when the shell starts a game.
3. Redirects the call to `GameImpl::ProcessGameCycle()` in `GameFrame::OnIdle()` (0x49C4BD). After each cycle it runs
   due script commands, calls the bot, writes telemetry and screenshots, and quits (`TerminateProcess`) at the tick
   limit or when `gameTermReasons_` leaves `Running`.
4. Turbo: changes the frame limiter's `jae` at 0x49C374 to `jmp`, so a game cycle runs on every idle loop. It also skips
   the three pane `OnIdle()` draw calls (0x49C5FC) except during the first ticks and the 4 ticks before each capture,
   because rendering costs about 10x more than simulating. Skipping draws does not change the simulation: telemetry
   with screenshots matches telemetry without them, byte for byte.
5. Replaces the game's modal message box (0x41E0E0) with a function that logs the message and returns OK. A bad mission
   name therefore ends the run with exit status 2 instead of hanging.

Startup defaults to single-player missions. Missions whose names start with `t` get the Tutorial type, all others
Colony. With `--campaign` the start info is a MissionResults with `isCampaign` set (op2-rs notes/findings-missions.md).
With `--players N` (2-6), startup instead reads the mission's real type and maximum from `DescBlock`, initializes N
human players plus any `DescBlockEx` AI slots, and uses the original multiplayer defaults: high resources, day/night
on, morale steady on, zero initial vehicles, cheats off, and disasters off for Land Rush / Last One Standing but on
otherwise. The lobby's host is Eden; later slots use the original's alternating colony flags. The no-shell path has no
NetTransportLayer, so `PrepareGame` uses the original's playback branch while preserving `isMultiplayer` and the player
setup, then restores playback. This is an offline start, not a network session; `harness.log` records the whole profile.

Measured speed with turbo depends on machine load and map size. Cases seen: a 16-unit map ran at about 500 to 50,000
ticks/s with occasional screenshots, about 100 ticks/s when rendering every tick, and 36 ticks/s at real-time speed 10.
Wine and Xvfb startup add about 3 s per run. Parallel runs work: each gets its own Xvfb and output directory, and they
share the harness prefix's wineserver.

**Own prefix (Linux).** The prefixes sit beside this folder. When `../prefix-harness/` exists (a plain copy of the
prefix you play from: `cp -a ../prefix ../prefix-harness`), `run.py` and `make install` use it (override with
`OP2_HARNESS_PREFIX`, an absolute path, or make's `HARNESS_PREFIX`; prefixes.py). `make install` and `uninstall` refuse
the play prefix `../prefix`, so without `../prefix-harness/` they need one of the overrides. Harness runs then have their
own wineserver, so they can't take focus from, or replace DLLs under, a game being played from `../prefix`. Mission
DLLs you install into `../prefix` (`OPU/maps`) are mirrored into the harness prefix before each run.

**Own game copy (Windows).** `windows/run_win.py setup` copies the game to `game/` and installs the harness there; the
install you play from is never written to (setup refuses to install into it).

**One installed harness per prefix.** `make install` replaces `OPU/Harness/harness.dll` and `zScenario.dll` for every
run from that prefix, including other checkouts of this folder. The builds are reproducible (no PE timestamps), and
`run.py` warns when the installed copy differs from this folder's `build/` (another version is installed, or yours
isn't). Before installing, check that no other runs depend on the installed version.

## Disabling / removing

- Normal play is unaffected while `OP2H_MISSION` is unset. To unregister the module completely (Linux):
  `make uninstall`. This removes the ini lines and `OPU/Harness/`. On Windows, delete the `game/` copy.
- `OPU/outpost2.ini.pre-harness` is a backup of the ini taken before the first install.
- `make check` puts its test mission in `OPU/maps/HarnessTest/` only while the check runs, so it never appears in the
  in-game lists.

## Limitations

- The game rounds the window size, so `--size` is only approximate.
- If a mission ends on its own, the final screenshot may show a frame from up to `--screenshot-every` ticks earlier,
  because rendering was skipped. Add `--draw-every 50` (for example) if the last frame matters.
- Only the game's own message box is intercepted. A `MessageBoxA` from a mission DLL still blocks until `--timeout`,
  and `timeout_screen.png` shows it. A crash ends the run with a non-zero exit (winedbg is disabled for harness runs
  on Linux). On Windows, Windows Error Reporting's crash dialog can hold a crashed run until the timeout.
- Scenario runs are single player only. Spawns are vehicles and buildings of players 0 and 1 via
  `Game::CreateUnit`.
- Telemetry covers what is cheap to read, and is easy to extend in `WriteTelemetry()`. Research progress, factory
  timers and cargo amounts need `--eco` (raw object bytes, decoded in `$ORACLE/eco_check.py`); disasters aren't covered.
- TethysAPI's `Unit::DoBuild` (its `Location - Location` is a distance, not a difference) and `DoMiningRoute` build
  broken packets, and `DoTransferCargo` takes the AI path, which crashed: the `build`, `route` and `transfercargo`
  verbs send their own packets.
- The game's code addresses are hard-coded for Outpost2.exe 1.2 as patched by OPU 1.4.x. They are the same addresses
  TethysAPI and OPUPatch use.

## Gotchas

- Coordinates: internal tile x = file x + 32 on non-wrapping maps, no offset on 512-wide maps (Coordinates, above;
  $ORACLE/FINDINGS.md).
- In OPU 1.4.1, TethysAPI `Unit::DoMove` on human units crashes; send command packets instead. `ScGroup::RemoveUnit` on
  a non-member crashes, and so does `FindEMPMissileTarget`. Triggers naming an AI player fire immediately. Mission
  callback names are `OnLoad/OnUnload/OnEnd`, not the TethysAPI docs' `OnLoadMission`.
- A `build` whose site's 1-tile margin touches a cliff (or anything else the cursor refuses) is never moved: the
  ConVec drives to the margin's bottom-right tile and the order ends there with "can't build here", kit kept; if that
  tile is itself a cliff it waits beside it forever (runs eco_build_cliff, input_build_cliff_*). Only structures a
  mission creates (`spawn`, CreateUnit) are relocated: their spiral search skips the right margin column
  ($ORACLE/FINDINGS.md, op2-rs notes/findings-sim.md "Spawn relocation").
- The harness and every oracle run assume GOG 1.2 + **OPU 1.4.1** (op2ext loads the harness).
- Linux: the harness uses its own Wine prefix, `../prefix-harness/` (a copy of `../prefix/`), so harness runs don't
  disturb a game being played from `../prefix/`. Never run harness installs or `wineserver -k` against `../prefix/`.
- Linux: the Wine prefix is wow64: `WINEARCH=win32` isn't supported. Every run from a prefix shares its wineserver:
  check `ps` for a game being played before `wineserver -k`, and coordinate `make install` (it replaces the installed
  harness DLLs for every run from that prefix).
- Windows: a run's game window opens on the desktop (1032x766 client area: the ini's frame padding is Wine's) and takes
  focus, and the harness parks the real mouse pointer in the screen's corner for the run (windows/README.md).
