// OP2 test harness: an op2ext ini module that, when OP2H_MISSION is set in the environment, skips the shell and
// starts that mission directly, optionally runs uncapped, writes JSONL telemetry and BMP screenshots every N ticks,
// replays a scripted command file, calls an optional bot DLL every tick, and exits after a tick limit.
// With OP2H_SHELL=1 it lets the game boot normally instead and only logs what the shell and game do (shell.py).
// With neither set the module does nothing, so normal play is unaffected.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
#include <mmsystem.h>
#include "Tethys/API/API.h"
#include "Tethys/Game/TApp.h"
#include "Tethys/Game/GameImpl.h"
#include "Tethys/Game/Research.h"
#include "Tethys/Game/MissionManager.h"
#include "Tethys/Game/MapObjectType.h"
#include "Tethys/Game/MapImpl.h"
#include "Tethys/Game/MapObjectManager.h"
#include "Tethys/Game/PathFinder.h"
#include "Tethys/Game/Random.h"
#include "Tethys/UI/GameFrame.h"
#include "Tethys/Resource/CConfig.h"
#include "Tethys/Resource/ResManager.h"

using namespace Tethys;
using namespace TethysAPI;

namespace {

struct Config {
  std::string mission, out, script, bot;
  std::string campaign;  // OP2H_CAMPAIGN: campaign carry-over file (see StartMissionHook).
  std::string resume;    // OP2H_RESUME: a checkpoint save to load at the first game cycle ("Checkpoints").
  std::string checkpoint;  // OP2H_CHECKPOINT: where to save the game when the run stops with the game going on.
  int ticks = 0, shotEvery = 0, telemEvery = 100, speed = 10, seed = 1, difficulty = 1, width = 1024, height = 768;
  int players = 1;  // OP2H_PLAYERS: >1 selects the original's multiplayer defaults.
  int drawEvery = 0;  // Turbo only: render every N ticks (0 = only just before screenshots).
  int traceEvery = 0;  // trace.jsonl sample interval (0 = off).
  // OP2H_DESKTOP: a person's real Windows desktop, not a private display: leave their pointer and focus alone (the
  // harness feeds the game its own cursor; an off-screen or hidden window draws nothing, so screenshots need it shown).
  bool desktop = false;
  bool turbo = true;
  bool presents = false;  // OP2H_PRESENTS: count pane presents per tick and per real second.
} cfg;

std::string Env(const char* name, const char* def = "") {
  char buf[MAX_PATH * 2];
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string(def);
}
int EnvInt(const char* name, int def) { const auto s = Env(name);  return s.empty() ? def : atoi(s.c_str()); }

FILE* g_log   = nullptr;
FILE* g_telem = nullptr;
bool g_record = false;  // OP2H_RECORD: a recording of a person's play ("Recording a player's own game").
bool g_net = false;     // OP2H_NET in shell mode: a network game is logged ("Network games").
std::string g_recEnd;   // The recording's last line, once the session has ended.
void WriteRecording();
// A `packet` line: its type, the data bytes (dataLength) and the slot's bytes after them up to the last nonzero one.
struct RawPacket { int type = 0;  std::vector<uint8_t> data, tail; };
std::map<int, RawPacket> g_packetDue;  // `packet` lines: the tick of the cycle that runs it -> the packet

void Log(const char* fmt, ...) {
  if (!g_log) return;
  va_list args;
  va_start(args, fmt);
  vfprintf(g_log, fmt, args);
  va_end(args);
  fputc('\n', g_log);
  fflush(g_log);
}

// ---------------------------------------------------------------------------------------------------------------------
// Code patching. Outpost2.exe always loads at 0x400000 (no relocations), so absolute addresses are stable.

bool WriteCode(uintptr_t addr, const void* data, size_t size) {
  DWORD old;
  if (!VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, &old)) return false;
  memcpy(reinterpret_cast<void*>(addr), data, size);
  VirtualProtect(reinterpret_cast<void*>(addr), size, old, &old);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), size);
  return true;
}

// Redirects a rel32 CALL instruction to `target`; returns the previous callee (which may be another mod's hook).
void* RelinkCall(uintptr_t callAddr, void* target) {
  auto* p = reinterpret_cast<uint8_t*>(callAddr);
  if (p[0] != 0xE8) { Log("patch: no CALL at %08x", unsigned(callAddr));  return nullptr; }
  int32_t rel;
  memcpy(&rel, p + 1, 4);
  void* prev = reinterpret_cast<void*>(callAddr + 5 + rel);
  rel = int32_t(reinterpret_cast<uintptr_t>(target) - (callAddr + 5));
  uint8_t code[5] = { 0xE8 };
  memcpy(code + 1, &rel, 4);
  return WriteCode(callAddr, code, 5) ? prev : nullptr;
}

constexpr uintptr_t CallLoadDebugMap     = 0x486606;  // TApp::CreateFrames(): no-shell path calls GameImpl::LoadDebugMap()
constexpr uintptr_t CallProcessGameCycle = 0x49C4BD;  // GameFrame::OnIdle(): calls GameImpl::ProcessGameCycle()
constexpr uintptr_t OnIdleFrameLimiter   = 0x49C374;  // GameFrame::OnIdle(): `jae` taken when a tick is due
constexpr uintptr_t OnIdleDrawPanes      = 0x49C5FC;  // GameFrame::OnIdle(): detail/minimap/command pane OnIdle()
constexpr uint8_t   SkipDrawPanes[2]     = { 0xEB, 0x2B };  // jmp 0x49C629
uint8_t g_drawPanesCode[2];
bool    g_drawing = true;

// Rendering dominates the cost of a game cycle, so turbo runs skip it except right before a screenshot.
void SetDrawing(bool on) {
  if (on == g_drawing) return;
  WriteCode(OnIdleDrawPanes, on ? g_drawPanesCode : SkipDrawPanes, 2);
  g_drawing = on;
}

constexpr uintptr_t FnOP2MessageBox      = 0x41E0E0;  // int __fastcall(HWND, const char* msg, const char* title, uint flags)

void JumpTo(uintptr_t from, void* to) {
  uint8_t code[5] = { 0xE9 };
  const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(to) - (from + 5));
  memcpy(code + 1, &rel, 4);
  WriteCode(from, code, 5);
}

// The game's modal error/confirmation box would block an unattended run forever: log it and answer OK instead.
int __fastcall MessageBoxHook(HWND, const char* msg, const char* title, uint32_t) {
  Log("dialog: [%s] %s", title ? title : "", msg ? msg : "");
  return IDOK;
}

using GameCycleFn = void(__thiscall*)(GameImpl*);
GameCycleFn g_origGameCycle = nullptr;

// ---------------------------------------------------------------------------------------------------------------------
// Screenshots: copy the game window's client area into a 24-bit BMP.

void Screenshot(const char* path) {
  HWND hWnd = g_gameFrame.hWnd_;
  RECT rc;
  if (!hWnd || !GetClientRect(hWnd, &rc)) return;
  const int w = rc.right, h = rc.bottom;
  if (w <= 0 || h <= 0) return;

  HDC hdc = GetDC(hWnd), mem = CreateCompatibleDC(hdc);
  BITMAPINFO bi = {};
  bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, h, 1, 24, BI_RGB };
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(mem, bmp);
  BitBlt(mem, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
  GdiFlush();

  const DWORD stride = ((w * 3 + 3) & ~3), imageSize = stride * h;
  BITMAPFILEHEADER fh = { 0x4D42, DWORD(sizeof(fh) + sizeof(BITMAPINFOHEADER) + imageSize), 0, 0,
                          DWORD(sizeof(fh) + sizeof(BITMAPINFOHEADER)) };
  bi.bmiHeader.biSizeImage = imageSize;
  if (FILE* f = fopen(path, "wb")) {
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(bits, imageSize, 1, f);
    fclose(f);
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(hWnd, hdc);
}

// ---------------------------------------------------------------------------------------------------------------------
// Telemetry: one JSON object per line.

const char* CommandName(int c) {
  static const char* const names[] = {
    "Nop", "Doze", "Move", "Dock", "DockEG", "Stop", "Build", "BuildWall", "RemoveWall", "Produce", "TransferCargo",
    "LoadCargo", "UnloadCargo", "Recycle", "DumpCargo", "Scavenge", "SpecialWait", "Survey", "Idle", "Unidle",
    "SelfDestruct", "Scatter", "Research", "TrainScientists", "Transfer", "Launch", "FlyInSpace", "Repair", "RepairObj",
    "Reprogram", "Dismantle", "Salvage", "Create", "Develop", "UnDevelop", "LightToggle", "Attack", "Guard",
    "StandGround", "CargoRoute", "Patrol", "MapChange", "Poof", "GameOpt", "GodWeapon", "ChatText", "ChatSFX", "Death",
    "Chat", "Quit", "Ally", "GoAI", "MachineSettings", "InvalidCommand" };
  return (c >= 0 && c < int(std::size(names))) ? names[c] : "?";
}

const char* TypeName(MapID id) {
  if (id == MapID::None) return "None";
  auto* t = MapObjectType::GetInstance(int(id));
  return t ? t->GetCodeName() : "?";
}

bool g_eco = false;  // OP2H_ECO: economy telemetry (player colony state, building/vehicle internals). See ECONOMY.md.
const char* ActionName(int a);
int HighestObjectIndex();
void Hex(FILE* f, const void* p, size_t n);

void WriteUnit(FILE* f, bool& first, Unit u) {
  if (!u.IsLive()) return;
  const auto loc = u.GetLocation();
  fprintf(f, "%s{\"id\":%d,\"type\":\"%s\",\"owner\":%d,\"x\":%d,\"y\":%d,\"hp\":%d,\"maxHp\":%d,\"cmd\":\"%s\"",
          first ? "" : ",", u.GetID(), TypeName(u.GetType()), u.GetOwner(), loc.x, loc.y,
          u.GetMaxHitpoints() - u.GetDamage(), u.GetMaxHitpoints(), CommandName(int(u.GetCommand())));
  if (const MapID w = u.GetWeapon(); u.IsVehicle() && w != MapID::None && int(w) > 0)
    fprintf(f, ",\"cargo\":\"%s\"", TypeName(w));
  if (g_eco) {
    // flags_ (MoFlagBld*: 0x1 active, 0x2000 power, 0x4000 workers, 0x8000 scientists, 0x40000 infected, 0x80000
    // EMPed), action and its timer, the 16-bit cargo/researchTimer word (+0x24), and the subclass bytes 0x48..0x7F.
    const MapObject* mo = u.GetMapObject();
    const uint8_t* b = reinterpret_cast<const uint8_t*>(mo);
    fprintf(f, ",\"flags\":%u,\"act\":\"%s\",\"timer\":%d,\"busy\":%d,\"w24\":%u,\"raw\":\"", unsigned(mo->flags_),
            ActionName(int(mo->action_)), mo->actionTimer_, mo->isBusy_, unsigned(*reinterpret_cast<const uint16_t*>(b + 0x24)));
    Hex(f, b + 0x48, 0x38);
    fputc('"', f);
    if (u.IsVehicle() && u.GetType() == MapID::CargoTruck)
      fprintf(f, ",\"truck\":[%d,%d]", int(mo->truckCargoType_), int(mo->truckCargoAmount_));
    if (u.IsBuilding())
      if (const Location d = u.GetDockLocation(); d) fprintf(f, ",\"dock\":[%d,%d]", d.x, d.y);
    // A building's hive (+0x38), the tube network HiveManager keeps it in (0x42F150 and the rest), which DoDamage's Damage
    // Prevention reads when the building is hit (0x4A39EB, 0x430050): [+0x8, Command Centers +0xC, one enabled +0x10,
    // DIRTs +0x14, members +0x18, and the hive's slot in HiveManager 0x547390's array of 0x24-byte hives at +0x18,
    // which tells hives apart (a merge keeps the first one, 0x42F8C0)].
    if (u.IsBuilding()) {
      const int* h = *reinterpret_cast<const int* const*>(b + 0x38);
      if (h && !IsBadReadPtr(h, 0x1C))
        fprintf(f, ",\"hive\":[%d,%d,%d,%d,%d,%d]", h[2], h[3], h[4], h[5], h[6],
                int((reinterpret_cast<uintptr_t>(h) - 0x5473A8) / 0x24));
    }
  }
  fputc('}', f);
  first = false;
}

// Economy telemetry per player: named PlayerImpl fields (offsets in $ORACLE/ECONOMY.md) plus every dword of
// PlayerImpl 0x00..0x1EF as `pi`, so fields can be read by offset.
void WritePlayerEco(FILE* f, const PlayerImpl* p) {
  const auto& m = p->moraleState_;
  fprintf(f, ",\"eco\":{\"maxCommon\":%d,\"maxRare\":%d,\"maxFood\":%d,\"availWorkers\":%d,\"availScientists\":%d,"
             "\"workersReq\":%d,\"scientistsReq\":%d,\"sciAsWorkers\":%d,\"sciResearching\":%d,\"powerAvail\":%d,"
             "\"powerInactive\":%d,\"foodProd\":%d,\"foodCons\":%d,\"foodLacking\":%d,\"active\":%d,\"unpowered\":%d,"
             "\"recalc\":%d,\"starveCase\":%d,\"growth\":[%d,%d,%d],\"death\":[%d,%d,%d],\"caps\":[%d,%d,%d,%d],"
             "\"activeCC\":%d,\"nurseries\":%d,\"universities\":%d,\"moraleEvent\":%d,\"morale08\":%d,\"morale10\":%d,"
             "\"resDemand\":%d,\"foodSupply\":%d,\"disabledRatio\":%d,\"recForumDemand\":%d,\"morale24\":%d,"
             "\"medDemand\":%d,\"unoccupied\":%d,\"sciAsWorkersPct\":%d,\"rcc\":%d},\"pi\":[",
          p->maxCommonOre_, p->maxRareOre_, p->maxFood_, p->numAvailableWorkers_, p->numAvailableScientists_,
          p->numWorkersRequired_, p->numScientistsRequired_, p->numScientistsAsWorkers_, p->numScientistsResearching_,
          p->amountPowerAvailable_, p->inactivePowerCapacity_, p->totalFoodProduction_, p->totalFoodConsumption_,
          p->foodLacking_, p->numActiveBuildings_, p->numUnpoweredStructures_, p->recalculateValues_, p->starvationCase_,
          p->kidGrowthRemainder_, p->workerGrowthRemainder_, p->scientistGrowthRemainder_, p->kidDeathRemainder_,
          p->workerDeathRemainder_, p->scientistDeathRemainder_, p->totalResidenceCapacity_, p->totalMedCenterCapacity_,
          p->totalRecFacilityCapacity_, p->totalForumCapacity_, p->numActiveCommandCenters_, p->numActiveNurseries_,
          p->numActiveUniversities_, m.eventMoraleModifier, m.field_08, m.field_10, m.residenceDemandPercent,
          int(m.foodSupply), m.disabledBuildingRatio, m.recForumDemandPercent, m.field_24, m.medCenterDemandPercent,
          m.unoccupiedColonistsPercent, m.scientistsAsWorkersPercent, p->rccOperational_);
  const int* d = reinterpret_cast<const int*>(p);
  for (int k = 0; k < 0x1F0 / 4; ++k) fprintf(f, "%s%d", k ? "," : "", d[k]);
  fputc(']', f);
}

// Every player's structures, then vehicles, player by player (the order telemetry, traces and the unit
// scans have always used).
template <class F> void ForEachPlayerUnit(F&& visit) {
  for (int p = 0; p < g_gameImpl.numPlayers_; ++p) {
    for (Unit u : PlayerBuildingEnum(p)) visit(u);
    for (Unit u : PlayerVehicleEnum(p))  visit(u);
  }
}

int g_lastTelemTick = -1;
bool g_inputUsed = false;  // The script has input verbs: telemetry adds the selection and the view.
void WriteInputState(FILE* f);

void WriteTelemetry(int tick) {
  FILE* f = g_telem;
  if (!f || tick == g_lastTelemTick) return;
  g_lastTelemTick = tick;
  auto& game = g_gameImpl;
  fprintf(f, "{\"tick\":%d,\"mark\":%d,\"end\":%d,\"players\":[", tick, tick / 100, int(game.gameTermReasons_));
  for (int i = 0; i < game.numPlayers_; ++i) {
    PlayerImpl* p = game.GetPlayer(i);
    const auto& m = p->moraleState_;
    uint32_t satellites;
    memcpy(&satellites, &p->satellites_, sizeof(satellites));
    fprintf(f, "%s{\"id\":%d,\"human\":%d,\"eden\":%d,\"food\":%d,\"commonOre\":%d,\"rareOre\":%d,\"workers\":%d,"
               "\"scientists\":%d,\"kids\":%d,\"morale\":%d,\"moraleLevel\":%d,\"event\":%d,\"satellites\":%u,"
               "\"powerGen\":%d,\"powerUsed\":%d,\"foodNet\":%d,\"buildings\":%d,\"idleBuildings\":%d,\"techs\":[",
            i ? "," : "", i, p->isHuman_ != 0, p->isEden_ != 0, p->foodStored_, p->commonOre_, p->rareOre_,
            p->numWorkers_, p->numScientists_, p->numKids_, m.morale, int(m.moraleLevel), m.eventMoraleModifier, satellites,
            p->amountPowerGenerated_, p->amountPowerConsumed_, p->netFoodProduction_, p->numBuildings_, p->numIdleBuildings_);
    bool first = true;
    for (int t = 0; t < g_research.numTechs_; ++t) {
      if (g_research.HasTech(i, t)) { fprintf(f, "%s%d", first ? "" : ",", g_research.ppTechInfos_[t]->techID);  first = false; }
    }
    fputc(']', f);
    if (g_eco) WritePlayerEco(f, p);
    fputc('}', f);
  }
  fputs("],\"units\":[", f);
  bool first = true;
  ForEachPlayerUnit([&](Unit u) { WriteUnit(f, first, u); });
  fputc(']', f);
  if (g_eco) {
    fputs(",\"beacons\":[", f);
    first = true;
    const int last = HighestObjectIndex();
    for (int i = 1; i <= last; ++i) {
      const MapObject* mo = MapObject::GetInstance(i);
      if (!mo || !mo->IsLive() || mo->GetTypeID() != MapID::MiningBeacon) continue;
      const auto* mb = static_cast<const MapObj::MiningBeacon*>(mo);
      fprintf(f, "%s{\"x\":%d,\"y\":%d,\"ore\":%d,\"bar\":%d,\"variant\":%d}", first ? "" : ",",
              mo->pixelX_ / 32, mo->pixelY_ / 32, int(mb->mineType_), int(mb->mineYield_), mb->mineVariant_);
      first = false;
    }
    fputc(']', f);
  }
  if (g_inputUsed) WriteInputState(f);
  fputs("}\n", f);
  fflush(f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Map coordinates. Internal tile x = file tile x + padding (32 on non-wrapping maps, 0 on 512-wide world maps);
// internal y = file y. Internal pixel = internal tile * 32 + offset. See README "Coordinates".

int PadTiles() { return GameMap::GetPaddingWidth(); }

// ---------------------------------------------------------------------------------------------------------------------
// Trace: one JSONL line per sampled tick with sub-tile positions read straight from the MapObjects (ENGINE_PLAN trace
// format). Units spawned by the script are listed first, in spawn order, with their spawn index.

FILE* g_trace     = nullptr;
bool  g_traceRaw  = false;    // Also dump raw MapObject/PathContext bytes (for reverse engineering).
bool  g_traceMap  = false;    // Also list the map tiles that changed since the previous trace line (`--trace-map`).
std::vector<uint32_t> g_mapPrev;  // The map's cell, graphic and wall bits at the previous trace line.

// `"map":[[x, y, cell, graphic, wall], ...]`: the tiles (file coordinates) whose cell type, tile graphic or
// wall-or-building bit (TileData) changed since the previous trace line; the first line lists none.
void WriteTraceMap(FILE* f) {
  const int w = GameMap::GetWidth(), h = GameMap::GetHeight(), pad = PadTiles();
  const bool fresh = g_mapPrev.size() != size_t(w) * h;
  if (fresh) g_mapPrev.assign(size_t(w) * h, 0);
  fputs(",\"map\":[", f);
  bool first = true;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint32_t t = GameMap::GetImpl()->Tile(x + pad, y).u32All & 0x8000FFFFu;
      uint32_t& prev = g_mapPrev[size_t(y) * w + x];
      if (!fresh && t != prev)
        fprintf(f, "%s[%d,%d,%u,%u,%u]", first ? "" : ",", x, y, t & 0x1F, (t >> 5) & 0x7FF, t >> 31), first = false;
      prev = t;
    }
  fputc(']', f);
}

std::vector<int> g_spawned;   // Spawn index -> unit ID.
// Spawns that have died. The game recycles object indices, so a dead spawn's ID may later belong to something else.
// A spawn that failed (no free object index) is gone from the start.
std::vector<bool> g_spawnGone;
std::vector<MapID> g_spawnType;
// A gone spawn's object index: 0 still held by the dead object, 1 freed (+0x4 == -1), 2 handed out again (the trace's
// `reused`: an order naming the spawn now reaches another object), 3 never had one (a spawn that failed).
std::vector<uint8_t> g_spawnSlot;

const char* ActionName(int a) {
  static const char* const names[] = {
    "Done", "Move", "Build", "BuildMine", "Repair", "Develop", "ObjWait", "OperationalWait", "EMPWait", "Landing",
    "ObjFroze", "DelSelf", "DoResearch", "ObjDocking", "Scavenge", "WeaponMove", "WeatherMove", "EMPRecover",
    "WeaponAimCoarse", "WeaponAimFine", "SantaWalking", "Invalid" };
  return (a >= 0 && a < int(std::size(names))) ? names[a] : "?";
}

void Hex(FILE* f, const void* p, size_t n) {
  for (size_t i = 0; i < n; ++i) fprintf(f, "%02x", reinterpret_cast<const uint8_t*>(p)[i]);
}

void WriteTraceUnit(FILE* f, bool& first, Unit u, int spawnIndex) {
  if (!u.IsLive()) return;
  const MapObject* mo = u.GetMapObject();
  const int pad = PadTiles() * 32;
  const PathContext* pc = u.IsVehicle() ? mo->pPathContext_ : nullptr;
  if (pc && IsBadReadPtr(pc, sizeof(PathContext))) pc = nullptr;
  // Sub-pixel position: while a vehicle steps between tiles, PathContext::numPixelsMoved{X,Y} hold the 16-bit fraction
  // of a 16.16 fixed-point position, biased so 0x8000 means "exactly on pixelX_". They read 0 before the first step.
  // fixX/fixY are that position in exact 16.16 fixed point (file-relative), sx/sy the same as floats; px/py are the
  // game's own integer pixelX_/pixelY_ (the sub-pixel position rounded to nearest).
  int64_t fixX = int64_t(mo->pixelX_ - pad) << 16, fixY = int64_t(mo->pixelY_) << 16;
  if (pc && (pc->numPixelsMovedX | pc->numPixelsMovedY)) {
    fixX += int(pc->numPixelsMovedX) - 0x8000;
    fixY += int(pc->numPixelsMovedY) - 0x8000;
  }
  const double fx = fixX / 65536.0, fy = fixY / 65536.0;
  fprintf(f, "%s{\"id\":%d,", first ? "" : ",", u.GetID());
  if (spawnIndex >= 0) fprintf(f, "\"spawn\":%d,", spawnIndex);
  fprintf(f, "\"type\":\"%s\",\"owner\":%d,\"px\":%d,\"py\":%d,\"sx\":%.6f,\"sy\":%.6f,\"fix\":[%lld,%lld],\"dir\":%d,\"hp\":%d,\"cmd\":\"%s\",\"action\":\"%s\","
             "\"busy\":%d,\"ipx\":%d,\"ipy\":%d,\"frame\":%d,\"anim\":%d",
          TypeName(u.GetType()), u.GetOwner(), mo->pixelX_ - pad, mo->pixelY_, fx, fy, (long long)fixX, (long long)fixY, mo->rotation_,
          u.GetMaxHitpoints() - u.GetDamage(), CommandName(mo->command_), ActionName(int(mo->action_)), mo->isBusy_,
          mo->pixelX_, mo->pixelY_, mo->frameIndex_, mo->animationIndex_);
  if (u.IsVehicle()) {
    // Headlights (MoFlagVecHeadlights, flags bit 0).
    fprintf(f, ",\"lights\":%d", int(mo->flags_ & 1));
    if (pc) {
      fprintf(f, ",\"dest\":[%d,%d],\"path\":{\"start\":[%d,%d],\"moved\":[%d,%d],\"toMove\":[%d,%d],\"rotSpeed\":%d,"
                 "\"wp\":%d,\"numWp\":%d,\"pt\":%d,\"numPts\":%d,\"steps\":\"",
              pc->dstTileX - PadTiles(), pc->dstTileY, pc->startPixelX - pad, pc->startPixelY, pc->numPixelsMovedX,
              pc->numPixelsMovedY, pc->numPixelsToMoveX, pc->numPixelsToMoveY, pc->rotationSpeed,
              pc->currentWaypointIndex, pc->numWaypoints, pc->currentPathfinderPoint, pc->numPathfinderPoints);
      // Remaining planned steps of the current path fragment, one UnitDirection digit (0 = E, clockwise) per tile.
      for (int i = pc->currentPathfinderPoint; i >= 0 && i < pc->numPathfinderPoints && i < 64; ++i)
        fputc('0' + (pc->direction[i] & 7), f);
      fputs("\"}", f);
      if (g_traceRaw) { fputs(",\"rawPath\":\"", f);  Hex(f, pc, 0xD4);  fputc('"', f); }
    }
  }
  // Turret state of armed units (vehicles, guard posts; other structures use weapon_ for other things, e.g. a lab's
  // research countdown, chaos_same_topic). Offsets learned from Outpost2.exe: the turret rotation is the
  // byte at +0x2D (TankVehicle::Draw 0x484E5A passes it to SelectTurretGraphic; the aim steps at 0x43E6C5 slew it),
  // +0x2E is reloadTimer_, and +0x4C is the weapon target (low word = unit index when the high word is 0xFFFF,
  // high word 0x7FFF = none; else a ground tile), used by the fire handlers at 0x4A7650..0x4A7BC0.
  if (const int w = mo->weapon_; w >= int(MapID::AcidCloud) && w <= int(MapID::BFG) && (u.IsVehicle() || u.GetType() == MapID::GuardPost)) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(mo);
    const uint16_t tlo = *reinterpret_cast<const uint16_t*>(b + 0x4C), thi = *reinterpret_cast<const uint16_t*>(b + 0x4E);
    fprintf(f, ",\"weapon\":\"%s\",\"tur\":%d,\"reload\":%d,\"tgt\":", TypeName(MapID(w)), b[0x2D], mo->reloadTimer_);
    if (thi == 0x7FFF)      fputs("null", f);
    else if (thi == 0xFFFF) fprintf(f, "%d", int(tlo));
    else                    fprintf(f, "[%d,%d]", int(int16_t(tlo)) - PadTiles(), int(int16_t(thi)));
  }
  // Weapon status effects, only while one is on: [EMP, stickyfoam, ESG] timers (+0x56, +0x54, +0x58; the game counts
  // them down once per 8-tick phase, 0x43DF7A), each 0 unless its flag (MoFlagEMPed, MoFlagVecStickyfoamed, MoFlagESGed)
  // is set. Buildings keep only the EMP timer (bit 18 is MoFlagBldInfected on them).
  {
    const uint32_t fl = mo->flags_;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(mo);
    const bool veh = u.IsVehicle();
    const bool emp = fl & MoFlagEMPed, foam = veh && (fl & MoFlagVecStickyfoamed), esg = veh && (fl & MoFlagESGed);
    if (emp || foam || esg)
      fprintf(f, ",\"status\":[%d,%d,%d]", emp ? *reinterpret_cast<const uint16_t*>(b + 0x56) : 0,
              foam ? *reinterpret_cast<const uint16_t*>(b + 0x54) : 0, esg ? *reinterpret_cast<const uint16_t*>(b + 0x58) : 0);
  }
  // Tiles that hold this unit's index (TileData::unitIndex) within 3 tiles of its pixel, file coordinates: a moving
  // vehicle's reserved tile, a building's footprint.
  fputs(",\"tiles\":[", f);
  bool firstTile = true;
  for (int dy = -3; dy <= 3; ++dy)
    for (int dx = -3; dx <= 3; ++dx) {
      int x = mo->pixelX_ / 32 + dx;
      if (PadTiles() == 0) x = (x + GameMap::GetWidth()) % GameMap::GetWidth();  // 512-wide maps wrap
      const Location at(x, mo->pixelY_ / 32 + dy);
      if (at.x < 0 || at.y < 0 || at.y >= GameMap::GetHeight() || GameMap::GetUnitOnTile(at).GetID() != u.GetID()) continue;
      fprintf(f, "%s[%d,%d]", firstTile ? "" : ",", at.x - PadTiles(), at.y);
      firstTile = false;
    }
  fputc(']', f);
  if (g_traceRaw) { fputs(",\"raw\":\"", f);  Hex(f, mo, 0x80);  fputc('"', f); }
  fputc('}', f);
  first = false;
}

// Live weapon-fire entities (lasers, projectiles: MapChildEntity, flag bit 8; bit 6 is not set on them) whose parent is a traced unit. Their action shows
// the aiming phases (WeaponAimCoarse tracks the target, WeaponAimFine holds the fire bearing, see 0x43E898).
void WriteTraceFx(FILE* f) {
  MapImpl* map = MapImpl::GetInstance();
  bool first = true;
  fputs(",\"fx\":[", f);
  for (size_t i = 1; i < map->MaxNumUnits(); ++i) {
    const MapObject* mo = MapObject::GetInstance(int(i));
    if (!mo || !mo->IsLive() || !(mo->flags_ & MoFlagEntChild)) continue;
    if (const int t = int(mo->GetTypeID()); t < int(MapID::AcidCloud) || t > int(MapID::BFG)) continue;
    const auto* ce = static_cast<const MapChildEntity*>(mo);
    int spawn = -1;
    for (size_t k = 0; k < g_spawned.size(); ++k) if (g_spawned[k] == ce->parentIndex_) spawn = int(k);
    if (spawn < 0) continue;
    fprintf(f, "%s{\"id\":%d,\"type\":\"%s\",\"parent\":%d,\"action\":\"%s\",\"dir\":%d,\"px\":%d,\"py\":%d,\"timer\":%d",
            first ? "" : ",", int(i), TypeName(mo->GetTypeID()), spawn, ActionName(int(mo->action_)), mo->rotation_,
            mo->pixelX_ - PadTiles() * 32, mo->pixelY_, mo->actionTimer_);
    // An armed ESG field (flag 0x8000, set by its first DoEvent after landing, 0x481120): the mines left, a bit each
    // (+0x6E, 13 bits).
    if (mo->GetTypeID() == MapID::ESG && (mo->flags_ & 0x8000))
      fprintf(f, ",\"mines\":%d", int(*reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(mo) + 0x6E)));
    fputc('}', f);
    first = false;
  }
  fputc(']', f);
  // Every other live object (explosions, death animations, other weapon fire, unspawned units) as
  // [index, type, action, timer, px, py]: they
  // hold object indices, which decide the heartbeat phase of objects created later (0x439A10).
  fputs(",\"ents\":[", f);
  first = true;
  for (size_t i = 1; i < map->MaxNumUnits(); ++i) {
    const MapObject* mo = MapObject::GetInstance(int(i));
    if (!mo || !mo->IsLive()) continue;
    if (std::find(g_spawned.begin(), g_spawned.end(), int(i)) != g_spawned.end()) continue;
    if (mo->flags_ & MoFlagEntChild) {
      const auto* ce = static_cast<const MapChildEntity*>(mo);
      if (const int t = int(mo->GetTypeID()); t >= int(MapID::AcidCloud) && t <= int(MapID::BFG))
        if (std::find(g_spawned.begin(), g_spawned.end(), ce->parentIndex_) != g_spawned.end()) continue;
    }
    fprintf(f, "%s[%d,\"%s\",\"%s\",%d,%d,%d", first ? "" : ",", int(i), TypeName(mo->GetTypeID()),
            ActionName(int(mo->action_)), mo->actionTimer_, mo->pixelX_ - PadTiles() * 32, mo->pixelY_);
    // --trace-raw: also the animation and frame drawn (set when the object is drawn: use --draw-every 1) and the raw
    // MapObject bytes, e.g. of the structure a ConVec builds as it develops.
    if (g_traceRaw) {
      fprintf(f, ",%d,%d,\"", mo->animationIndex_, mo->frameIndex_);
      Hex(f, mo, 0x80);
      fputc('"', f);
    }
    fputc(']', f);
    first = false;
  }
  fputc(']', f);
}

int g_lastTraceTick = -1;

// The object index ring's mask, from the allocator scan's `and ebx, imm32` (81 E3 at 0x439AA0): 0x3FF on disk, 0x7FF
// as OPU patches it. Anything else there (another patch) falls back to 0x3FF, logged once.
int RingMask() {
  static int mask = 0;
  if (mask) return mask;
  const uint8_t* op = reinterpret_cast<const uint8_t*>(0x439AA0);
  const int m = *reinterpret_cast<const int*>(0x439AA2);
  if (op[0] == 0x81 && op[1] == 0xE3 && m > 0 && m <= 0x7FF && ((m + 1) & m) == 0) {
    mask = m;
  } else {
    mask = 0x3FF;
    Log("trace: unexpected allocator scan at 0x439AA0 (%02x %02x, imm %#x); the alloc ring is walked with 0x3FF", op[0], op[1], m);
  }
  return mask;
}

void WriteTrace(int tick) {
  FILE* f = g_trace;
  if (!f || tick == g_lastTraceTick) return;
  g_lastTraceTick = tick;
  fprintf(f, "{\"tick\":%d,\"units\":[", tick);
  bool first = true;
  for (size_t i = 0; i < g_spawned.size(); ++i) {
    const Unit u(g_spawned[i]);
    // Gone for good once dead (the flag stays set after the object is freed, until its index is reused) or replaced. A
    // vehicle in a Garage bay is marked dead too, with pNext_ -2 (0x44D000): it isn't listed while it's in there, and
    // is again once it comes out (chaos_garage_full).
    if (g_spawnGone[i]) {  // (a spawn that failed has no object at all)
      if (g_spawnSlot[i] < 2) {
        const int held = *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(u.GetMapObject()) + 0x4);
        if (held == -1) g_spawnSlot[i] = 1;
        else if (g_spawnSlot[i] == 1 || u.GetType() != g_spawnType[i]) g_spawnSlot[i] = 2;
      }
      continue;
    }
    const MapObject* mo = u.GetMapObject();
    const bool inBay = reinterpret_cast<uintptr_t>(mo->pNext_) == 0xFFFFFFFEu;
    if (((mo->flags_ & MoFlagDead) && !inBay) || u.GetType() != g_spawnType[i]) g_spawnGone[i] = true;
    else if (u.IsLive()) WriteTraceUnit(f, first, u, int(i));
  }
  if (g_spawned.empty()) {  // No scenario spawns: trace every vehicle.
    for (int p = 0; p < g_gameImpl.numPlayers_; ++p)
      for (Unit u : PlayerVehicleEnum(p)) WriteTraceUnit(f, first, u, -1);
  }
  fputc(']', f);
  // Spawns whose object index belongs to another object now (only when there are any).
  bool anyReused = false;
  for (size_t i = 0; i < g_spawnSlot.size(); ++i)
    if (g_spawnSlot[i] == 2) {
      fprintf(f, "%s%u", anyReused ? "," : ",\"reused\":[", unsigned(i));
      anyReused = true;
    }
  if (anyReused) fputc(']', f);
  WriteTraceFx(f);
  // Object index allocator (0x439A10): highest index handed out so far (0x54F838) and the ring of freed objects
  // (0x54F844, head 0x54F840, tail 0x54F83C) as [index, tick freed, reference count +0x32], every slot from the head to
  // the tail, then [head, tail]. The scan's mask is read from its `and ebx, imm32` (0x439AA0): 0x3FF in Outpost2.exe on
  // disk, 0x7FF as OPU patches it in memory (a 2048-object table). A slot between head and tail can be stale: a live
  // object there (+0x4 not -1; its +0x8 is a list pointer) is written [index, 4294967295, 0].
  {
    const int last = *reinterpret_cast<const int*>(0x54F838), head = *reinterpret_cast<const int*>(0x54F840),
              tail = *reinterpret_cast<const int*>(0x54F83C);
    const int mask = RingMask();
    const uint8_t* const* ring = *reinterpret_cast<const uint8_t* const* const*>(0x54F844);
    fprintf(f, ",\"alloc\":[%d,[", last);
    for (int k = head, n = 0; k != tail && n <= mask; k = (k + 1) & mask, ++n) {
      const uint8_t* o = ring[k];
      const bool live = *reinterpret_cast<const int*>(o + 0x4) != -1;
      fprintf(f, "%s[%d,%u,%d]", n ? "," : "", *reinterpret_cast<const int*>(o + 0x10),
              live ? 0xFFFFFFFFu : *reinterpret_cast<const unsigned*>(o + 0x8), live ? 0 : o[0x32]);
    }
    fprintf(f, "],[%d,%d]]", head, tail);
  }
  if (g_traceMap) WriteTraceMap(f);
  // Main gameplay RNG state (two 32-bit LFSRs at 0x56BE20, stepped by Random::Rand 0x46F000/0x46F060).
  const uint64 seed = g_gameRNG.GetSeed();
  fprintf(f, ",\"rng\":[%u,%u]}\n", unsigned(seed & 0xFFFFFFFF), unsigned(seed >> 32));
  fflush(f);
}

// Economy tables as loaded (eco runs only): the morale levels (MoraleManager 0x565280: name, M_PTS_TOT, then the
// power/research/factory/food bonuses in 1/256 and the defect/fertility/death rates), the derived maxima at +0xF0..
// +0x104, every morale modifier (0x4E1F10: name, value, percent flag), the mine yield table (MineManager 0x5651A0),
// and per unit type the resource priority (+0x2A0, the building list order) and research topic (+0x1E4).
void WriteEcoData(FILE* f) {
  const uint8_t* mm = reinterpret_cast<const uint8_t*>(0x565280);
  fputs(",\n\"morale\":{\"levels\":[", f);
  for (int k = 0; k < 5; ++k) {
    const uint8_t* row = mm + k * 0x30;
    char name[16] = {};
    memcpy(name, row, 15);
    fprintf(f, "%s{\"name\":\"%s\",\"v\":[", k ? "," : "", name);
    for (int j = 0; j < 8; ++j) fprintf(f, "%s%d", j ? "," : "", reinterpret_cast<const int*>(row + 0x10)[j]);
    fputs("]}", f);
  }
  fputs("],\"maxima\":[", f);
  for (int j = 0; j < 6; ++j) fprintf(f, "%s%d", j ? "," : "", reinterpret_cast<const int*>(mm + 0xF0)[j]);
  fputs("],\"modifiers\":[", f);
  struct Mod { const char* name;  int value;  int pct; };
  const Mod* mods = reinterpret_cast<const Mod*>(0x4E1F10);
  for (int k = 0; k < 0x43; ++k)
    fprintf(f, "%s[\"%s\",%d,%d]", k ? "," : "", mods[k].name ? mods[k].name : "", mods[k].value, mods[k].pct);
  fputs("]},\n\"mines\":[", f);
  const int* mine = reinterpret_cast<const int*>(0x5651A0);
  for (int k = 0; k < 9; ++k)
    fprintf(f, "%s{\"variant\":%d,\"bar\":%d,\"trucks\":[%d,%d,%d],\"yield\":[%d,%d,%d]}", k ? "," : "", k / 3, k % 3,
            mine[k * 3], mine[k * 3 + 1], mine[k * 3 + 2], mine[27 + k * 3], mine[27 + k * 3 + 1], mine[27 + k * 3 + 2]);
  // Tech tree as loaded, in techNum order: [techID, plymouthCost, edenCost, maxScientists, lab, category].
  fputs("],\n\"techTree\":[", f);
  for (int k = 0; k < g_research.numTechs_; ++k) {
    const TechInfo* ti = g_research.ppTechInfos_[k];
    fprintf(f, "%s[%d,%d,%d,%d,%d,%d]", k ? "," : "", ti->techID, ti->plymouthCost, ti->edenCost, ti->maxScientists,
            int(ti->lab), int(ti->category));
  }
  fputs("],\n\"typeExtra\":[", f);
  bool first = true;
  for (size_t i = 1; i < MapObjectManager::NumMapObjTypes(); ++i) {
    const uint8_t* t = reinterpret_cast<const uint8_t*>(MapObjectType::GetInstance(int(i)));
    if (!t) continue;
    fprintf(f, "%s[%u,%d,%d]", first ? "" : ",", unsigned(i), t[0x2A0], *reinterpret_cast<const int*>(t + 0x1E4));
    first = false;
  }
  fputc(']', f);
}

// Game constants the engine reimplements: map geometry, cell type speed table, and unit type stats, as loaded by the
// game (sheets + patches), written once at the first game cycle.
void WriteGameData(const char* path) {
  FILE* f = fopen(path, "w");
  if (!f) return;
  MapImpl* map = MapImpl::GetInstance();
  fprintf(f, "{\"map\":{\"width\":%d,\"height\":%d,\"paddedWidth\":%d,\"paddingTiles\":%d,\"tileXMask\":%u,"
             "\"pixelWidth\":%d,\"clipRect\":[%d,%d,%d,%d]},\n",
          GameMap::GetWidth(), GameMap::GetHeight(), map->tileWidth_, map->paddingOffsetTileX_, map->tileXMask_,
          map->pixelWidth_, map->clipRect_.x1, map->clipRect_.y1, map->clipRect_.x2, map->clipRect_.y2);
  fputs("\"trackTypes\":[\"Legged\",\"Wheeled\",\"Miner\",\"Tracked\",\"Hover\"],\n\"cellTypes\":[", f);
  for (int c = 0; c < int(CellType::Count); ++c) {
    const CellTypeInfo* ci = MapImpl::GetCellTypeInfo(CellType(c));
    fprintf(f, "%s\n {\"id\":%d,\"name\":\"%s\",\"trackSpeed\":[", c ? "," : "", c, ci && ci->pName ? ci->pName : "");
    for (int t = 0; ci && t < int(TrackType::Count); ++t) fprintf(f, "%s%d", t ? "," : "", ci->trackSpeed[t]);
    fprintf(f, "],\"blocksLineOfSight\":%d,\"blightSpeed\":%d}", ci ? ci->blocksLineOfSight : 0, ci ? ci->blightSpeed : 0);
  }
  fputs("],\n\"unitTypes\":[", f);
  bool first = true;
  for (size_t i = 1; i < MapObjectManager::NumMapObjTypes(); ++i) {
    MapObjectType* t = MapObjectType::GetInstance(int(i));
    if (!t) continue;
    const auto& s = t->playerStats_[0];
    fprintf(f, "%s\n {\"mapId\":%u,\"code\":\"%s\",\"name\":\"%s\",\"track\":%d,\"hp\":%d,\"armor\":%d,\"sight\":%d,"
               "\"moveSpeed\":%d,\"turnRate\":%d,\"reload\":%d,\"vehicleFlags\":%u",
            first ? "" : ",", unsigned(i), t->GetCodeName(), t->unitName_, int(t->trackType_), s.hp, int(s.armor),
            s.sightRange, s.vehicle.moveSpeed, s.vehicle.turnRate, s.vehicle.reloadTime, t->stats_.vehicle.flags);
    // Raw per-player stats words (PerPlayerUnitStats, 17 ints) so weapon fields can be read by offset: for weapons
    // +0x18 is the range in tiles, +0x24 the turret turn rate (aim step 0x43E76C), +0x30 the reload time (0x4A7DA7).
    fputs(",\"stats\":[", f);
    for (int k = 0; k < 17; ++k) fprintf(f, "%s%d", k ? "," : "", reinterpret_cast<const int*>(&s)[k]);
    fputc(']', f);
    // Buildings: footprint in tiles (GlobalUnitStats::building).
    if (i >= size_t(MapID::CommonOreMine) && i <= size_t(MapID::Tokamak))
      fprintf(f, ",\"size\":[%d,%d],\"dock\":[%d,%d],\"rubble\":[%d,%d],\"buildingFlags\":%u", t->stats_.building.width,
              t->stats_.building.height, t->stats_.building.edenDockLocation, t->stats_.building.plyDockLocation,
              t->stats_.building.commonRubble, t->stats_.building.rareRubble, t->stats_.building.flags);
    // Weapons: the shortest shot in pixels (type +0x24C, read by DoFire 0x4A3F8A) and the damage radius in pixels
    // (type +0x248, DoDamage 0x4A36C3); +0x250 is 1 for weapons that don't need line of sight (IsInRange 0x485297).
    if (i >= size_t(MapID::AcidCloud) && i <= size_t(MapID::BFG))
      fprintf(f, ",\"pixelsSkipped\":%d,\"radius\":%d,\"indirect\":%d",
              *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(t) + 0x24C),
              *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(t) + 0x248),
              *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(t) + 0x250));
    fputc('}', f);
    first = false;
  }
  fputc(']', f);
  if (g_eco) WriteEcoData(f);
  // Cell type of every map tile in file coordinates (row-major), to check the coordinate conversion against the .map.
  fputs(",\n\"cells\":[", f);
  for (int y = 0; y < GameMap::GetHeight(); ++y)
    for (int x = 0; x < GameMap::GetWidth(); ++x)
      fprintf(f, "%s%d", (x | y) ? "," : "", int(GameMap::GetCellType(Location(x + PadTiles(), y))));
  fputs("]}\n", f);
  fclose(f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Scripted input. One command per line: `<tick> <verb> <args...>`; '#' starts a comment.
//   move <unit> <x> <y> | dock <unit> <x> <y> | attack <unit> <target> | stop <unit> | selfdestruct <unit>
//   research <labId> <techId> <scientists> | shot | telemetry | trace | quit
//   spawn <TYPE> <owner> <x> <y> [weapon|-] [dir 0-255]   (TYPE/weapon are sheet code names, e.g. LIGHT_TANK LASER)
//   setcell <x1> <y1> <x2> <y2> <cellTypeId>                 (overwrite terrain cell types in a rect, inclusive)
//   coords file|internal                                     (how later x/y are read; default internal)
//   view <x> <y>                                             (centre player 0's view on a tile, for screenshots)
// A <unit> is an internal unit ID as telemetry reports it, or @N for the unit spawned by the Nth spawn command.

struct ScriptCmd { int tick;  std::string verb;  std::vector<std::string> args;  bool fileCoords;  int line = 0;  bool ran = false; };
std::vector<ScriptCmd> g_script;
size_t g_scriptPos = 0;

bool IsInputVerb(const std::string& v);

void LoadScript(const std::string& path) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f) { Log("script: cannot open %s", path.c_str());  return; }
  char line[4096];  // A recording's `packet` lines carry up to 198 hex digits and a comment.
  bool fileCoords = false;
  int lineNo = 0;
  while (fgets(line, sizeof(line), f)) {
    ++lineNo;
    if (char* hash = strchr(line, '#')) *hash = 0;
    std::vector<std::string> tok;
    for (char* t = strtok(line, " \t\r\n"); t; t = strtok(nullptr, " \t\r\n")) tok.emplace_back(t);
    if (tok.size() < 2) continue;
    if (tok[1] == "coords") { fileCoords = tok.size() > 2 && tok[2] == "file";  continue; }
    g_script.push_back({ atoi(tok[0].c_str()), tok[1], std::vector<std::string>(tok.begin() + 2, tok.end()), fileCoords, lineNo });
  }
  fclose(f);
  std::stable_sort(g_script.begin(), g_script.end(), [](auto& l, auto& r) { return l.tick < r.tick; });
  for (const auto& c : g_script) g_inputUsed |= IsInputVerb(c.verb);
  Log("script: %u commands from %s", unsigned(g_script.size()), path.c_str());
}

void Quit(const char* why);
void TakeShot(int tick);

// ---------------------------------------------------------------------------------------------------------------------
// Virtual mouse and keyboard for the input verbs (see README "Mouse and keyboard input").
//
// Input is delivered in process, right after a game cycle, the same way TApp::PumpMessage (0x4867B0) would deliver it:
// mouse messages go straight to the pane window under the cursor (or the one holding capture), key and character
// messages go through GameFrame::TranslateAccelerators (0x49B3B0) and IDlgWnd::PretranslateModeless before being
// dispatched. Everything is synchronous, so an input at tick T is fully handled before cycle T+1 runs.
// The game also polls the mouse and keys: GetCursorPos (edge scrolling 0x407BBA, the cursor shape, mouse-over) and
// GetAsyncKeyState (Shift/Ctrl in MouseCommandFilter 0x41B50D/0x41BA60, arrow-key scrolling 0x407BCE). Their IAT slots
// are pointed at this virtual state, so nothing depends on the X server's pointer.

constexpr uintptr_t IatGetCursorPos      = 0x583898;
constexpr uintptr_t IatGetAsyncKeyState  = 0x58389C;
constexpr uintptr_t SelectedCount        = 0x547210;  // Selected units: count, then up to 32 unit indices (0x41ADF0).
constexpr uintptr_t SelectedUnits        = 0x547214;

struct VirtualInput {
  bool    active = false;  // Set once the cursor is parked at the first cycle; until then the real functions answer.
  POINT   pos    = {};     // Client coordinates of the game window (the same pixels as screenshots).
  uint8_t keys[256] = {};  // Held keys, by virtual-key code (VK_LBUTTON/VK_RBUTTON/VK_MBUTTON for mouse buttons).
} g_in;
int  g_lastInputTick = -1000;

using GetCursorPosFn     = BOOL(WINAPI*)(LPPOINT);
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
GetCursorPosFn     g_realGetCursorPos     = nullptr;
GetAsyncKeyStateFn g_realGetAsyncKeyState = nullptr;

BOOL WINAPI VirtualGetCursorPos(LPPOINT p) {
  if (!g_in.active) return g_realGetCursorPos(p);
  if (!p) return FALSE;
  *p = g_in.pos;
  ClientToScreen(g_gameFrame.hWnd_, p);
  return TRUE;
}

SHORT WINAPI VirtualGetAsyncKeyState(int vk) {
  if (!g_in.active) return g_realGetAsyncKeyState(vk);
  return (vk >= 0 && vk < 256 && g_in.keys[vk]) ? SHORT(0x8000) : 0;
}

void* PatchIat(uintptr_t slot, void* fn) {
  void* prev = *reinterpret_cast<void**>(slot);
  WriteCode(slot, &fn, sizeof(fn));
  return prev;
}

// TPane::GetAbsolutePos (vtable slot 10): pane client point -> internal map pixel. Returns 0 outside the map.
int PaneToMap(TPane* pane, int x, int y, POINT* out) {
  using Fn = int(__thiscall*)(TPane*, int, int, POINT*);
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(pane))[10])(pane, x, y, out);
}

// The pane's top-left corner in game window client coordinates.
POINT PaneOrigin(const IWnd& pane) {
  POINT p = {0, 0};
  MapWindowPoints(pane.hWnd_, g_gameFrame.hWnd_, &p, 1);
  return p;
}

TPane* PaneAt(POINT client) {
  TPane* panes[] = { &g_gameFrame.detailPane_, &g_gameFrame.miniMapPane_, &g_gameFrame.commandPane_ };
  for (TPane* p : panes) {
    RECT rc;
    if (!p->hWnd_ || !GetClientRect(p->hWnd_, &rc)) continue;
    const POINT o = PaneOrigin(*p);
    if (client.x >= o.x && client.y >= o.y && client.x < o.x + rc.right && client.y < o.y + rc.bottom) return p;
  }
  return nullptr;
}

const char* PaneName(const IWnd* p) {
  return p == &g_gameFrame.detailPane_ ? "detail" : p == &g_gameFrame.miniMapPane_ ? "minimap" :
         p == &g_gameFrame.commandPane_ ? "command" : "frame";
}

// Internal map pixel -> game window client point through the detail pane's current view. The view origin comes from the
// game's own pane->map conversion, so zoom-free 1:1 pixels are the only assumption (checked in the log).
POINT MapToClient(int mapX, int mapY) {
  TPane* dp = &g_gameFrame.detailPane_;
  POINT o0 = {}, o1 = {};
  PaneToMap(dp, 0, 0, &o0);
  PaneToMap(dp, 64, 64, &o1);
  const int scale = std::max(1L, (o1.x - o0.x) / 64);  // 1 at normal zoom.
  int dx = mapX - o0.x;
  if (PadTiles() == 0) {  // World maps wrap: take the nearest copy.
    const int w = GameMap::GetWidth() * 32;
    dx = ((dx % w) + w + w / 2) % w - w / 2;
  }
  const POINT org = PaneOrigin(*dp);
  return { org.x + dx / scale, org.y + (mapY - o0.y) / scale };
}

// Finds the minimap pane point that shows internal tile `t`, searching the pane with MiniMap::TranslateCoordinates
// (0x48D930): the minimap's scale and centring depend on the map size and zoom.
bool MinimapToClient(Location t, POINT* out) {
  TPane* mp = &g_gameFrame.miniMapPane_;
  RECT rc;
  if (!mp->hWnd_ || !GetClientRect(mp->hWnd_, &rc)) return false;
  const int wantX = t.x * 32 + 16, wantY = t.y * 32 + 16;
  long best = LONG_MAX;
  for (int y = 0; y < rc.bottom; ++y)
    for (int x = 0; x < rc.right; ++x) {
      POINT m;
      if (!g_miniMap.TranslateCoordinates({ x, y }, &m)) continue;  // What the minimap pane's click handler calls.
      const long d = labs(m.x - wantX) + labs(m.y - wantY);
      if (d < best) { best = d;  *out = { x, y }; }
    }
  if (best == LONG_MAX) return false;
  const POINT o = PaneOrigin(*mp);
  out->x += o.x;
  out->y += o.y;
  return true;
}

// UI controls of the minimap and command panes (TPane::pControls_): UIElement objects with pane-relative rectangles.
// Buttons (UIButton::GetHelpText at vtable slot 7, checked against the known implementations) have a tooltip text.
std::string ControlHelp(UIElement* e) {
  void** vtbl = *reinterpret_cast<void***>(e);
  const uintptr_t f = reinterpret_cast<uintptr_t>(vtbl[7]);
  if (f != 0x40B2B0 && f != 0x462940) return std::string();  // UIGraphicalButton, BayButton
  char buf[256] = {};
  using Fn = void(__thiscall*)(UIElement*, char*, int);
  reinterpret_cast<Fn>(vtbl[7])(e, buf, sizeof(buf) - 1);
  return buf;
}

struct UiControl { TPane* pane;  int index;  UIElement* e;  RECT rc;  std::string help; };

std::vector<UiControl> UiControls() {
  std::vector<UiControl> out;
  TPane* panes[] = { &g_gameFrame.miniMapPane_, &g_gameFrame.commandPane_ };
  for (TPane* p : panes) {
    const POINT o = PaneOrigin(*p);
    for (int i = 0; i < p->numControls_ && i < 20; ++i) {
      auto* e = static_cast<UIElement*>(p->pControls_[i]);
      if (!e || IsBadReadPtr(e, sizeof(UIElement))) continue;
      RECT rc = e->position_;
      OffsetRect(&rc, o.x, o.y);
      out.push_back({ p, i, e, rc, ControlHelp(e) });
    }
  }
  return out;
}

void LogUiControls() {
  for (const auto& c : UiControls())
    Log("ui: %s[%d] (%ld,%ld)-(%ld,%ld) flags %x \"%s\"", PaneName(c.pane), c.index, c.rc.left, c.rc.top, c.rc.right,
        c.rc.bottom, unsigned(c.e->flags_), c.help.c_str());
}

// The `hotkeys` verb: one line of hotkeys.jsonl with the buttons' hotkey table, the filter object at 0x546BB0 (vtable
// 0x4CF890, its OnUIEvent 0x40A710) that sends a WM_KEYDOWN or WM_SYSKEYDOWN to the button registered for that virtual
// key while its +0x404 counter is 0 (Pause and the chat line raise it, 0x40A6E0). A button takes its key from the
// letter after its tooltip's '&' (UIGraphicalButton::SetDisplayInfo 0x40AD70, UIButton::SetHotKey 0x40A9F0, upper
// case) and registers it when added to a pane, unless another button already holds that key (UIButton::OnAdd
// 0x40A920). Each entry: [vk, flags (UIButton +4: 2 disabled), tooltip, vtable].
void WriteHotkeys(int tick) {
  static FILE* f = fopen((cfg.out + "\\hotkeys.jsonl").c_str(), "w");
  if (!f) return;
  auto* filter = reinterpret_cast<const uint8_t*>(0x546BB0);
  auto* table = reinterpret_cast<UIElement* const*>(filter + 4);
  fprintf(f, "{\"tick\":%d,\"off\":%d,\"keys\":[", tick, *reinterpret_cast<const int*>(filter + 0x404));
  bool first = true;
  for (int vk = 0; vk < 256; ++vk) {
    UIElement* e = table[vk];
    if (!e || IsBadReadPtr(e, 0x20)) continue;
    std::string help;
    for (char ch : ControlHelp(e)) {
      if (ch == '"' || ch == '\\') help += '\\';
      if (uint8_t(ch) >= 0x20) help += ch;
    }
    fprintf(f, "%s[%d,%u,\"%s\",%u]", first ? "" : ",", vk, unsigned(e->flags_), help.c_str(),
            unsigned(*reinterpret_cast<uintptr_t*>(e)));
    first = false;
  }
  fputs("]}\n", f);
  fflush(f);
}

// `name`: "tabN" (command pane report tab N, 0 = top), "minimapN" (minimap button N), or a case-insensitive substring of
// a control's tooltip with '_' for spaces (e.g. "Mission_Objectives").
bool UiToClient(const std::string& name, POINT* out) {
  auto centre = [&](const IWnd& pane, const RECT& r) {
    const POINT o = PaneOrigin(pane);
    *out = { o.x + (r.left + r.right) / 2, o.y + (r.top + r.bottom) / 2 };
    return true;
  };
  if (name.size() == 4 && _strnicmp(name.c_str(), "tab", 3) == 0 && name[3] >= '0' && name[3] <= '5')
    return centre(g_gameFrame.commandPane_, g_gameFrame.commandPane_.reportButtons_[name[3] - '0'].position_);
  if (name.size() == 8 && _strnicmp(name.c_str(), "minimap", 7) == 0 && name[7] >= '0' && name[7] <= '3')
    return centre(g_gameFrame.miniMapPane_, g_gameFrame.miniMapPane_.buttons_[name[7] - '0'].position_);
  std::string want = name;
  for (char& ch : want) ch = ch == '_' ? ' ' : char(tolower(ch));
  for (const auto& c : UiControls()) {
    std::string h;
    for (char ch : c.help) if (ch != '&') h += char(tolower(ch));  // '&' marks the hotkey letter.
    if (!want.empty() && h.find(want) != std::string::npos) {
      *out = { (c.rc.left + c.rc.right) / 2, (c.rc.top + c.rc.bottom) / 2 };
      return true;
    }
  }
  return false;
}

// Telemetry extension for input runs, `,"selected":[ids],"view":[x,y]`: the selection (0x547210) and the detail
// view's top-left corner in file pixels; with `cmds`, also `,"cmds":[...]`, each selected unit's command. Telemetry
// and the shell's `state` probe share it.
std::string InputStateJson(bool cmds) {
  const int n = std::min(*reinterpret_cast<const int*>(SelectedCount), 32);
  std::string ids, names;
  for (int k = 0; k < n; ++k) {
    const int id = reinterpret_cast<const int*>(SelectedUnits)[k];
    ids += (k ? "," : "") + std::to_string(id);
    names += std::string(k ? ",\"" : "\"") + CommandName(int(Unit(id).GetCommand())) + "\"";
  }
  POINT o = {};
  PaneToMap(&g_gameFrame.detailPane_, 0, 0, &o);
  return ",\"selected\":[" + ids + "]" + (cmds ? ",\"cmds\":[" + names + "]" : "") + ",\"view\":[" +
         std::to_string(o.x - PadTiles() * 32) + "," + std::to_string(o.y) + "]";
}

void WriteInputState(FILE* f) { fputs(InputStateJson(false).c_str(), f); }

WPARAM MouseKeyFlags() {
  return (g_in.keys[VK_LBUTTON] ? MK_LBUTTON : 0) | (g_in.keys[VK_RBUTTON] ? MK_RBUTTON : 0) |
         (g_in.keys[VK_MBUTTON] ? MK_MBUTTON : 0) | (g_in.keys[VK_SHIFT] ? MK_SHIFT : 0) |
         (g_in.keys[VK_CONTROL] ? MK_CONTROL : 0);
}

// Mirrors the held keys into the thread's keyboard state, which TranslateAccelerator reads through GetKeyState.
void SyncKeyboardState() {
  BYTE ks[256] = {};
  for (int vk = 0; vk < 256; ++vk) if (g_in.keys[vk]) ks[vk] = 0x80;
  ks[VK_LSHIFT] = ks[VK_SHIFT];  ks[VK_LCONTROL] = ks[VK_CONTROL];  ks[VK_LMENU] = ks[VK_MENU];
  SetKeyboardState(ks);
}

// Sends a mouse message at client point `at`, to the pane holding capture or else the pane under the point.
void SendMouse(UINT msg, POINT at) {
  g_in.pos = at;
  HWND target = GetCapture();
  if (!target || (target != g_gameFrame.detailPane_.hWnd_ && target != g_gameFrame.miniMapPane_.hWnd_ &&
                  target != g_gameFrame.commandPane_.hWnd_)) {
    TPane* p = PaneAt(at);
    target = p ? p->hWnd_ : g_gameFrame.hWnd_;
  }
  POINT local = at;
  MapWindowPoints(g_gameFrame.hWnd_, target, &local, 1);
  SendMessageA(target, msg, MouseKeyFlags(), MAKELPARAM(int16_t(local.x), int16_t(local.y)));
}

// One message through the same steps as TApp::PumpMessage. Returns true when an accelerator took it.
bool PumpOne(UINT msg, WPARAM w, LPARAM l) {
  MSG m = {};
  m.hwnd    = g_gameFrame.hWnd_;  // The frame has the keyboard focus in play.
  m.message = msg;
  m.wParam  = w;
  m.lParam  = l;
  m.time    = DWORD(g_gameImpl.tick_);
  m.pt      = g_in.pos;
  ClientToScreen(g_gameFrame.hWnd_, &m.pt);
  if (g_gameFrame.TranslateAccelerators(&m)) return true;
  if (IDlgWnd::PretranslateModeless(&m)) return false;
  DispatchMessageA(&m);
  return false;
}

struct KeySpec { int vk = 0;  bool shift = false, ctrl = false, alt = false; };

// "a", "7", "enter", "f5", "ctrl+1", "alt+c", "shift+tab", "vk:0x41" ...
bool ParseKey(std::string s, KeySpec* k) {
  for (;;) {
    const size_t plus = s.find('+', 1);
    if (plus == std::string::npos) break;
    const std::string mod = s.substr(0, plus);
    if      (_stricmp(mod.c_str(), "shift") == 0) k->shift = true;
    else if (_stricmp(mod.c_str(), "ctrl") == 0)  k->ctrl = true;
    else if (_stricmp(mod.c_str(), "alt") == 0)   k->alt = true;
    else return false;
    s = s.substr(plus + 1);
  }
  static const std::pair<const char*, int> names[] = {
    {"enter", VK_RETURN}, {"return", VK_RETURN}, {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE}, {"space", VK_SPACE},
    {"tab", VK_TAB}, {"backspace", VK_BACK}, {"delete", VK_DELETE}, {"insert", VK_INSERT}, {"home", VK_HOME},
    {"end", VK_END}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"left", VK_LEFT}, {"right", VK_RIGHT},
    {"up", VK_UP}, {"down", VK_DOWN}, {"pause", VK_PAUSE}, {"shift", VK_SHIFT}, {"ctrl", VK_CONTROL},
    {"alt", VK_MENU}, {"numplus", VK_ADD}, {"numminus", VK_SUBTRACT}, {"plus", VK_OEM_PLUS}, {"minus", VK_OEM_MINUS} };
  for (const auto& [n, vk] : names) if (_stricmp(s.c_str(), n) == 0) { k->vk = vk;  return true; }
  if ((s[0] == 'f' || s[0] == 'F') && s.size() > 1 && isdigit(uint8_t(s[1]))) {
    const int n = atoi(s.c_str() + 1);
    if (n >= 1 && n <= 12) { k->vk = VK_F1 + n - 1;  return true; }
  }
  if (_strnicmp(s.c_str(), "vk:", 3) == 0) { k->vk = int(strtol(s.c_str() + 3, nullptr, 0)) & 0xFF;  return true; }
  if (s.size() == 1) {
    const SHORT v = VkKeyScanA(s[0]);
    if (v == -1) return false;
    k->vk = v & 0xFF;
    k->shift |= (v & 0x100) != 0;
    return true;
  }
  return false;
}

void KeyEvent(int vk, bool down) {
  const bool sys = g_in.keys[VK_MENU] || vk == VK_MENU || vk == VK_F10;
  g_in.keys[vk] = down;
  if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
    g_in.keys[vk == VK_SHIFT ? VK_LSHIFT : vk == VK_CONTROL ? VK_LCONTROL : VK_LMENU] = down;
  }
  SyncKeyboardState();
  const UINT scan = MapVirtualKeyA(vk, 0);
  const LPARAM l = 1 | (LPARAM(scan) << 16) | (sys && g_in.keys[VK_MENU] ? (1 << 29) : 0) |
                   (down ? 0 : (LPARAM(3) << 30));
  const UINT msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
  if (PumpOne(msg, vk, l) || !down) return;
  // What TranslateMessage would post: the character, pumped the same way (some accelerators are characters: + - = _).
  BYTE ks[256];
  GetKeyboardState(ks);
  WORD ch = 0;
  if (ToAscii(vk, scan, ks, &ch, 0) == 1) PumpOne(sys ? WM_SYSCHAR : WM_CHAR, ch & 0xFF, l);
}

void PressKey(const KeySpec& k) {
  const int mods[3][2] = { {VK_CONTROL, k.ctrl}, {VK_MENU, k.alt}, {VK_SHIFT, k.shift} };
  bool pressed[3] = {};
  for (int i = 0; i < 3; ++i) if (mods[i][1] && !g_in.keys[mods[i][0]]) { KeyEvent(mods[i][0], true);  pressed[i] = true; }
  KeyEvent(k.vk, true);
  KeyEvent(k.vk, false);
  for (int i = 2; i >= 0; --i) if (pressed[i]) KeyEvent(mods[i][0], false);
}

// All of these go through the unit command packet path the game uses for local input (single-player only; a
// multiplayer-safe version would have to queue packets through the network layer instead).
// Unit::DoMove() goes through MapObject::CmdMove() (the ScGroup/AI path), which crashed the renderer when used on a
// human player's unit, so build the same Move packet that clicking produces.
// The packet builders are also what a recording's comments compare a recorded packet with (DescribePacket).
CommandPacket MovePacket(int id, Location where, CommandType type) {
  CommandPacket packet = { type, sizeof(MoveCommand) };
  packet.data.move.numUnits     = 1;
  packet.data.move.unitID[0]    = uint16(id);
  packet.data.move.numWaypoints = 1;
  packet.data.move.waypoint[0]  = where.AsWaypoint();
  return packet;
}

void IssueMove(Unit u, Location where, CommandType type = CommandType::Move) {
  if (!u.IsLive()) return;
  g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(MovePacket(u.GetID(), where, type));
}

// Unit orders with more than a Move's arguments, built the way PlayerImpl::ProcessCommandPacket (0x40E300) parses
// them: Move/Dock/StandGround/Patrol at 0x40E3B0, Attack/Guard at 0x40FAB7. `waypoints` are tiles, each with a range
// in the waypoint's top 3 bits (how near counts as reached, 0x44BFD0; the UI uses 2 for Ctrl+click waypoints and 3 for
// patrol points); Patrol appends the waypoint index the patrol loops back to (stored at MapObject +0x5E).
CommandPacket WaypointPacket(int id, CommandType type, const std::vector<Location>& waypoints, const std::vector<int>& ranges,
                             int patrolLoop) {
  CommandPacket packet = { type, 0 };
  auto& m = packet.data.move;
  m.numUnits     = 1;
  m.unitID[0]    = uint16(id);
  m.numWaypoints = uint16(waypoints.size());
  Waypoint* wp = m.waypoint;
  for (size_t i = 0; i < waypoints.size(); ++i) {
    wp[i] = waypoints[i].AsWaypoint();
    wp[i].u32All = (wp[i].u32All & 0x1FFFFFFF) | (uint32(i < ranges.size() ? ranges[i] & 7 : 0) << 29);
  }
  size_t size = sizeof(MoveCommand) + (waypoints.size() - 1) * sizeof(Waypoint);
  if (patrolLoop >= 0) {
    const uint16 loop = uint16(patrolLoop);
    memcpy(reinterpret_cast<uint8*>(&m) + size, &loop, 2);
    size += 2;
  }
  packet.dataLength = uint16(size);
  return packet;
}

void IssueWaypoints(Unit u, CommandType type, const std::vector<Location>& waypoints, const std::vector<int>& ranges,
                    int patrolLoop = -1) {
  if (!u.IsLive() || waypoints.empty() || waypoints.size() > 8) return;
  g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(WaypointPacket(u.GetID(), type, waypoints, ranges, patrolLoop));
}

// Attack or Guard on a unit (`tile` false: x = unit id) or on a tile.
CommandPacket TargetedPacket(int id, CommandType type, int x, int y, bool tile) {
  CommandPacket packet = { type, sizeof(AttackCommand) };
  packet.data.attack.numUnits  = 1;
  packet.data.attack.unitID[0] = uint16(id);
  packet.data.attack.unknown   = 0;
  packet.data.attack.target    = { { uint16(x) }, uint16(tile ? y : 0xFFFF) };
  return packet;
}

void IssueTargeted(Unit u, CommandType type, int x, int y, bool tile) {
  if (!u.IsLive()) return;
  g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(TargetedPacket(u.GetID(), type, x, y, tile));
}

MapID TypeByName(const std::string& name) {
  if (name.empty() || name == "-") return MapID::None;
  for (size_t i = 1; i < MapObjectManager::NumMapObjTypes(); ++i)
    if (auto* t = MapObjectType::GetInstance(int(i)); t && _stricmp(t->GetCodeName(), name.c_str()) == 0) return MapID(i);
  return MapID::None;
}

// Unit names (README "Unit names"). Unit ids are handed out as units appear, so they differ between runs whose history
// differs; a script that must survive that names units instead. A word `$NAME` stands for the unit bound to NAME; a
// word `$TYPE@x,y` for the live unit of that type nearest to the tile (x,y) (within NameNearTiles), both resolved when
// the command runs. NAME is bound by `name NAME <unit>` or by `as NAME` ending a `produce` or `build`: the unit that
// order makes. That unit is found by watching the objects each cycle (NameScan): a new vehicle of the produced type
// within NameFactoryTiles of the factory, or a new structure within the build rectangle. The binding keeps the unit's
// type and owner, and NameScan unbinds a name the first cycle its unit is no longer live (or no longer that type), so
// a name whose unit died resolves to nothing even after the game reuses the object index. (A unit that dies and is
// replaced by one of the same type within a single cycle would not be noticed.) An `as NAME` that finds no unit within
// NamePendingTicks is dropped with a warning. NAME is letters, digits and _ (not all digits).
constexpr int NameNearTiles = 3, NameFactoryTiles = 8, NamePendingTicks = 4000;

struct NameBinding { int id, type, owner; };
std::map<std::string, NameBinding> g_names;
std::map<std::string, int> g_goneNames;  // Unbound because the unit died: name -> its id (for the warning).
// An `as NAME` waiting for its unit: a vehicle of `type` near (fx,fy), or any structure inside the rectangle.
struct PendingName { std::string name;  bool structure;  int owner, type, fx, fy, x1, y1, x2, y2, tick; };
std::vector<PendingName> g_pendingNames;
std::vector<int> g_nameSeen;  // Per object index: the type NameScan saw there last (-1 none).
bool g_scriptFileCoords = false;  // The coordinate mode of the command being run (for `$TYPE@x,y`).

std::string UpperName(std::string s) {
  for (auto& ch : s) ch = char(toupper(uint8_t(ch)));
  return s;
}

// Whether an object index is inside the object table: reading the table past its end crashes the game.
bool InObjectTable(int id) { return id >= 0 && size_t(id) < MapImpl::GetInstance()->MaxNumUnits(); }

// Whether the unit's object index is in range and holds a live object. Unit::IsLive alone reads the object table
// at any index, so an id past its end (a script from another army history) crashes the game.
bool Exists(Unit u) {
  const int id = u.GetID();
  return id > 0 && InObjectTable(id) && u.IsLive();
}

bool ValidName(const std::string& n) {
  if (n.empty()) return false;
  bool digits = true;
  for (char ch : n) {
    if (!isalnum(uint8_t(ch)) && ch != '_') return false;
    digits &= isdigit(uint8_t(ch)) != 0;
  }
  return !digits;
}

void BindName(const std::string& name, Unit u) {
  g_names[UpperName(name)] = { u.GetID(), int(u.GetType()), u.GetOwner() };
  g_goneNames.erase(UpperName(name));
  Log("script: name %s = unit %d (%s)", UpperName(name).c_str(), u.GetID(), TypeName(u.GetType()));
}

// Each cycle (before the script's commands): unbinds names whose unit is gone, expires pending names, and matches the
// objects that appeared since the last scan against the pending ones. The object scan only runs while some wait (a
// script without `as` costs nothing), and it only reads the game.
void NameScan(bool baseline = false) {
  for (auto it = g_names.begin(); it != g_names.end();) {
    const Unit u(it->second.id);
    if (Exists(u) && int(u.GetType()) == it->second.type) { ++it;  continue; }
    Log("script: name %s: unit %d is gone, unbound", it->first.c_str(), it->second.id);
    g_goneNames[it->first] = it->second.id;
    it = g_names.erase(it);
  }
  const int now_tick = g_gameImpl.tick_;
  for (size_t k = 0; k < g_pendingNames.size();) {
    if (now_tick - g_pendingNames[k].tick > NamePendingTicks) {
      Log("script: WARN tick %d: `as %s` from tick %d found no unit in %d ticks: dropped", now_tick, g_pendingNames[k].name.c_str(),
          g_pendingNames[k].tick, NamePendingTicks);
      g_pendingNames.erase(g_pendingNames.begin() + k);
    } else ++k;
  }
  if (g_pendingNames.empty() && !baseline) { g_nameSeen.clear();  return; }  // Nothing waits: forget the baseline too.
  const size_t n = MapImpl::GetInstance()->MaxNumUnits();
  if (g_nameSeen.size() < n) g_nameSeen.resize(n, -1);
  std::vector<uint8_t> now(g_nameSeen.size(), 0);
  ForEachPlayerUnit([&](Unit u) {
    if (!u.IsLive()) return;
    const int id = u.GetID(), type = int(u.GetType());
    if (id <= 0 || size_t(id) >= g_nameSeen.size()) return;
    now[id] = 1;
    if (g_nameSeen[id] == type) return;
    g_nameSeen[id] = type;
    const Location at = u.GetLocation();
    // The pending name that fits best: a structure inside its rectangle, a vehicle of the type nearest its factory
    // (within NameFactoryTiles; ties go to the earlier order).
    int best = -1, bestDist = INT_MAX;
    for (size_t k = 0; k < g_pendingNames.size(); ++k) {
      const PendingName& w = g_pendingNames[k];
      if (w.owner != u.GetOwner() || w.structure != u.IsBuilding()) continue;
      int dist = 0;
      if (w.structure) {
        if (at.x < w.x1 || at.x > w.x2 || at.y < w.y1 || at.y > w.y2) continue;
      } else {
        dist = std::max(abs(at.x - w.fx), abs(at.y - w.fy));
        if (type != w.type || dist > NameFactoryTiles) continue;
      }
      if (dist < bestDist) best = int(k), bestDist = dist;
    }
    if (best >= 0) {
      BindName(g_pendingNames[best].name, u);
      g_pendingNames.erase(g_pendingNames.begin() + best);
    }
  });
  for (size_t id = 1; id < g_nameSeen.size(); ++id) if (!now[id]) g_nameSeen[id] = -1;
}

// The unit a `$` word names, or none; `why` says what was wrong.
Unit ResolveDollar(const std::string& word, std::string* why) {
  const std::string body = word.substr(1);
  const size_t at = body.find('@');
  if (at == std::string::npos) {
    const std::string key = UpperName(body);
    const auto it = g_names.find(key);
    if (it == g_names.end()) {
      bool waiting = false;
      for (const auto& w : g_pendingNames) waiting |= w.name == key;
      const auto gone = g_goneNames.find(key);
      if (why) *why = waiting ? "name " + body + " is bound to nothing yet (its unit hasn't appeared)"
                    : gone != g_goneNames.end() ? "the unit named " + body + " (id " + std::to_string(gone->second) + ") is gone"
                    : "no name " + body;
      return Unit();
    }
    const Unit u(it->second.id);
    if (!Exists(u) || int(u.GetType()) != it->second.type || u.GetOwner() != it->second.owner) {
      if (why) *why = "the unit named " + body + " (id " + std::to_string(it->second.id) + ") is gone";
      return Unit();
    }
    return u;
  }
  int x = 0, y = 0;
  const MapID type = TypeByName(body.substr(0, at));
  if (type == MapID::None || sscanf(body.c_str() + at + 1, "%d,%d", &x, &y) != 2) {
    if (why) *why = "bad selector " + word + " (expected $TYPE@x,y)";
    return Unit();
  }
  x += g_scriptFileCoords ? PadTiles() : 0;
  int best = INT_MAX, bestId = 0;
  ForEachPlayerUnit([&](Unit u) {
    if (!u.IsLive() || u.GetType() != type) return;
    const Location l = u.GetLocation();
    const int d = std::max(abs(l.x - x), abs(l.y - y));
    if (d <= NameNearTiles && d < best) best = d, bestId = u.GetID();
  });
  if (!bestId && why) *why = "no live " + body.substr(0, at) + " within " + std::to_string(NameNearTiles) + " tiles of " + body.substr(at + 1);
  return Unit(bestId);
}

Unit ScriptUnit(const std::string& s, std::string* why = nullptr) {
  if (!s.empty() && s[0] == '@') { const size_t n = atoi(s.c_str() + 1);  return Unit(n < g_spawned.size() ? g_spawned[n] : 0); }
  if (!s.empty() && s[0] == '$') return ResolveDollar(s, why);
  const int id = atoi(s.c_str());
  return InObjectTable(id) ? Unit(id) : Unit();
}

// A plain numeric unit word whose object index is past the table (the case that used to crash the game).
bool IdOutOfRange(const std::string& s) {
  return !s.empty() && s[0] != '@' && s[0] != '$' && !InObjectTable(atoi(s.c_str()));
}

bool IsInputVerb(const std::string& v) {
  static const char* const verbs[] = { "mouse", "click", "rclick", "mclick", "dblclick", "down", "up", "drag", "key",
                                       "keydown", "keyup", "type", "select", "uilist", "hotkeys" };
  for (const char* n : verbs) if (v == n) return true;
  return false;
}

std::vector<int> g_pendingShots;      // Ticks of screenshots requested with `+shot`.
std::vector<ScriptCmd> g_deferred;    // Button releases scheduled by `hold N`.

// Input verbs. A <pos> is `<x> <y>` (game window client pixels, as in screenshots), `tile <x> <y>` (that tile's centre in
// the detail pane), `unit <id>` (the unit's centre), `mini <x> <y>` (the minimap pixel showing that tile) or
// `ui <name>` (a control's centre, see UiToClient). Mouse verbs take optional `left|right|middle` and `shift`/`ctrl`/`alt`
// held while the button goes down; `hold N` on click/rclick/mclick/drag releases the button N ticks later instead of in
// the same tick (the minimap only pans while the button is held during a drawn frame). Any verb takes `+shot`
// (screenshot one tick later, showing the frame drawn after the input).
void RunInputCmd(const ScriptCmd& c, int tick) {
  g_lastInputTick = tick;
  std::vector<std::string> a;
  bool shot = false;
  for (const auto& s : c.args) { if (s == "+shot") shot = true;  else a.push_back(s); }
  if (shot) g_pendingShots.push_back(tick + 1);
  size_t i = 0;
  auto num = [&](size_t k) { return k < a.size() ? atoi(a[k].c_str()) : 0; };
  auto pos = [&](POINT* p) -> bool {
    if (i >= a.size()) return false;
    const std::string kind = a[i];
    if (kind == "tile" || kind == "mini") {
      const Location t(num(i + 1) + (c.fileCoords ? PadTiles() : 0), num(i + 2));
      i += 3;
      if (kind == "mini") return MinimapToClient(t, p);
      *p = MapToClient(t.x * 32 + 16, t.y * 32 + 16);
      return true;
    }
    if (kind == "unit") {
      const Unit u = ScriptUnit(i + 1 < a.size() ? a[i + 1] : "");
      i += 2;
      if (!u.IsLive()) return false;
      *p = MapToClient(u.GetMapObject()->pixelX_, u.GetMapObject()->pixelY_);
      return true;
    }
    if (kind == "ui") { i += 2;  return UiToClient(i - 1 < a.size() ? a[i - 1] : "", p); }
    if (!isdigit(uint8_t(kind[0])) && kind[0] != '-') return false;
    *p = { num(i), num(i + 1) };
    i += 2;
    return true;
  };
  // Trailing words: button and modifiers.
  int button = 0;  // 0 left, 1 right, 2 middle
  bool mod[3] = {};  // shift, ctrl, alt
  int hold = 0;
  auto words = [&] {
    for (size_t k = i; k < a.size(); ++k) {
      if      (a[k] == "hold" && k + 1 < a.size()) hold = std::max(0, atoi(a[++k].c_str()));
      else if (a[k] == "left")   button = 0;
      else if (a[k] == "right")  button = 1;
      else if (a[k] == "middle") button = 2;
      else if (a[k] == "shift")  mod[0] = true;
      else if (a[k] == "ctrl")   mod[1] = true;
      else if (a[k] == "alt")    mod[2] = true;
    }
  };
  const int modVk[3] = { VK_SHIFT, VK_CONTROL, VK_MENU };
  bool pressedMod[3] = {};
  auto holdMods = [&] {
    for (int k = 0; k < 3; ++k) if (mod[k] && !g_in.keys[modVk[k]]) { KeyEvent(modVk[k], true);  pressedMod[k] = true; }
  };
  auto releaseMods = [&] { for (int k = 2; k >= 0; --k) if (pressedMod[k]) KeyEvent(modVk[k], false); };
  static const UINT downMsg[3] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN };
  static const UINT upMsg[3]   = { WM_LBUTTONUP,   WM_RBUTTONUP,   WM_MBUTTONUP   };
  static const int  btnVk[3]   = { VK_LBUTTON,     VK_RBUTTON,     VK_MBUTTON     };
  auto press   = [&](POINT p) { g_in.keys[btnVk[button]] = 1;  SendMouse(downMsg[button], p); };
  auto release = [&](POINT p) {
    if (hold > 0) {
      static const char* const names[3] = { "left", "right", "middle" };
      g_deferred.push_back({ tick + hold, "up", { std::to_string(p.x), std::to_string(p.y), names[button] }, false });
      return;
    }
    g_in.keys[btnVk[button]] = 0;
    SendMouse(upMsg[button], p);
  };
  auto logAt = [&](POINT p) {
    TPane* pane = PaneAt(p);
    POINT m = {}, local = p;
    if (pane) MapWindowPoints(g_gameFrame.hWnd_, pane->hWnd_, &local, 1);
    const bool onMap = pane == &g_gameFrame.detailPane_ && PaneToMap(pane, local.x, local.y, &m);
    Log("input: tick %d %s at (%ld,%ld): %s pane (%ld,%ld)%s", tick, c.verb.c_str(), p.x, p.y, PaneName(pane), local.x,
        local.y, onMap ? (" map tile (" + std::to_string(m.x / 32 - PadTiles()) + "," + std::to_string(m.y / 32) +
                          ") file").c_str() : "");
  };

  POINT p = g_in.pos, q;
  const std::string& v = c.verb;
  if (v == "mouse" || v == "click" || v == "rclick" || v == "mclick" || v == "dblclick" || v == "down" || v == "up") {
    if (!pos(&p)) { Log("input: tick %d %s: bad position", tick, v.c_str());  return; }
    words();
    if (v == "rclick") button = 1;
    if (v == "mclick") button = 2;
    if (v == "dblclick" || v == "down" || v == "up" || v == "mouse") hold = 0;
    logAt(p);
    holdMods();
    SendMouse(WM_MOUSEMOVE, p);
    if (v == "down" || v == "click" || v == "rclick" || v == "mclick" || v == "dblclick") press(p);
    if (v == "up"   || v == "click" || v == "rclick" || v == "mclick" || v == "dblclick") release(p);
    // No CS_DBLCLKS on the game's window classes (IWnd::RegisterClassA 0x4314D0 uses style 0x20), so Windows never sends
    // WM_LBUTTONDBLCLK to them: a double click arrives as a second down/up pair.
    if (v == "dblclick") { press(p);  release(p); }
    releaseMods();
  } else if (v == "drag") {
    if (!pos(&p) || !pos(&q)) { Log("input: tick %d drag: bad position", tick);  return; }
    words();
    logAt(p);
    logAt(q);
    holdMods();
    SendMouse(WM_MOUSEMOVE, p);
    press(p);
    for (int s = 1; s <= 4; ++s) SendMouse(WM_MOUSEMOVE, { p.x + (q.x - p.x) * s / 4, p.y + (q.y - p.y) * s / 4 });
    release(q);
    releaseMods();
  } else if (v == "select") {
    // A left click on the unit, centring the view on it first if it is off screen.
    const Unit u = ScriptUnit(a.empty() ? "" : a[0]);
    if (!u.IsLive()) { Log("input: tick %d select: no unit %s", tick, a.empty() ? "" : a[0].c_str());  return; }
    p = MapToClient(u.GetMapObject()->pixelX_, u.GetMapObject()->pixelY_);
    if (PaneAt(p) != &g_gameFrame.detailPane_) {
      Player[0].CenterViewOn(u.GetLocation());
      p = MapToClient(u.GetMapObject()->pixelX_, u.GetMapObject()->pixelY_);
    }
    logAt(p);
    SendMouse(WM_MOUSEMOVE, p);
    press(p);
    release(p);
  } else if (v == "key" || v == "keydown" || v == "keyup") {
    for (const auto& s : a) {
      KeySpec k;
      if (!ParseKey(s, &k)) { Log("input: tick %d %s: unknown key %s", tick, v.c_str(), s.c_str());  continue; }
      Log("input: tick %d %s %s (vk 0x%02x%s%s%s)", tick, v.c_str(), s.c_str(), k.vk, k.shift ? " shift" : "",
          k.ctrl ? " ctrl" : "", k.alt ? " alt" : "");
      if (v == "key") PressKey(k);
      else KeyEvent(k.vk, v == "keydown");
    }
  } else if (v == "type") {
    std::string text;
    for (const auto& s : a) text += (text.empty() ? "" : " ") + s;
    Log("input: tick %d type \"%s\"", tick, text.c_str());
    for (char ch : text) {
      KeySpec k;
      if (ParseKey(std::string(1, ch), &k)) PressKey(k);
    }
  } else if (v == "uilist") {
    LogUiControls();
  } else if (v == "hotkeys") {
    WriteHotkeys(tick);
  }
}

// Economy verbs (README "Economy verbs"):
//   player <p> <field> <value>        set a PlayerImpl field: workers scientists kids food common rare morale
//                                     moraleLevel solarsat, or a byte offset (0x94); marks colony values for recalculation
//   poke <unit> <offset> <1|2|4> <value>   write a MapObject field (e.g. damage at 0x1E)
//   beacon <x> <y> <ore 0|1> <bar 0-2> <variant 0-2> | beacon <x> <y> vent|fumarole
//   route <truck> <mine> <smelter>    cargo route (ctMoCargoRoute), as the UI builds it
//   produce <factory> <TYPE> [weapon] | idle <bldg> | unidle <bldg> | train <university> <n>
//   kit <convec> <TYPE> [weapon]      put a structure kit in a ConVec
//   build <convec> <x> <y> [TYPE]     ctMoBuild to the bottom-right tile of footprint+margin (TYPE: Robo-Miner, GeoCon)
//   bay <factory> <bay> <TYPE> [weapon] | transfercargo <factory> <bay>
//   cargo <truck> <type 0-10> <amount> | load <truck> | unload <truck>
bool RunEcoCmd(const ScriptCmd& c, int tick) {
  auto arg = [&](size_t i) -> std::string { return i < c.args.size() ? c.args[i] : std::string(); };
  auto num = [&](size_t i) { return int(strtol(arg(i).c_str(), nullptr, 0)); };
  auto loc = [&](size_t i) { return Location(num(i) + (c.fileCoords ? PadTiles() : 0), num(i + 1)); };
  const std::string& v = c.verb;
  if (v == "tube" || v == "wall") {  // tube|wall x1 y1 x2 y2 [TYPE]: Game::CreateWall over the inclusive rectangle
    const Location a = loc(0), b = loc(2);
    const MapID type = v == "tube" ? MapID::Tube : (c.args.size() > 4 ? TypeByName(arg(4)) : MapID::Wall);
    for (int y = a.y; y <= b.y; ++y)
      for (int x = a.x; x <= b.x; ++x) Game::CreateWall(type, Location(x, y));
    return true;
  }
  if (v == "mem") {  // mem <address> <1|2|4> <value>: write game memory, e.g. the forced-morale masks at 0x56EAEC
    uint8_t* b = reinterpret_cast<uint8_t*>(uintptr_t(strtoul(arg(0).c_str(), nullptr, 0)));
    const int size = num(1), value = num(2);
    if (IsBadWritePtr(b, size)) { Log("script: mem: bad address %s", arg(0).c_str());  return true; }
    if (size == 1) *b = uint8_t(value); else if (size == 2) *reinterpret_cast<uint16_t*>(b) = uint16_t(value);
    else *reinterpret_cast<int*>(b) = value;
    return true;
  }
  if (v == "peek") {  // peek <address> <bytes>: log game memory as hex, e.g. code as patched in memory (OPU, op2ext)
    const uint8_t* b = reinterpret_cast<const uint8_t*>(uintptr_t(strtoul(arg(0).c_str(), nullptr, 0)));
    const int size = std::min(num(1), 256);
    if (size <= 0) { Log("script: peek: needs a size of 1-256 bytes (got '%s')", arg(1).c_str());  return true; }
    if (IsBadReadPtr(b, size)) { Log("script: peek: bad address %s", arg(0).c_str());  return true; }
    std::string hex;
    char buf[4];
    for (int k = 0; k < size; ++k) { snprintf(buf, sizeof buf, "%02x", b[k]);  hex += (k ? " " : "") + std::string(buf); }
    Log("script: peek %s: %s", arg(0).c_str(), hex.c_str());
    return true;
  }
  if (v == "tech") {  // tech <player> <techID>: _Player::MarkResearchComplete (0x477510), as a mission DLL grants one
    // A topic the loaded tech tree doesn't have would end the mission, so it's logged with the valid ids instead.
    bool known = false;
    for (int t = 0; t < g_research.numTechs_ && !known; ++t) known = g_research.ppTechInfos_[t]->techID == num(1);
    if (!known || num(0) < 0 || num(0) >= int(MaxPlayers)) {
      std::string ids;
      for (int t = 0; t < g_research.numTechs_; ++t) ids += (ids.empty() ? "" : " ") + std::to_string(g_research.ppTechInfos_[t]->techID);
      Log("script: tech %s %s: no such player or topic in this tech tree (topics: %s)", arg(0).c_str(), arg(1).c_str(), ids.c_str());
      return true;
    }
    Player[num(0)].MarkResearchComplete(num(1));
    return true;
  }
  if (v == "player") {
    PlayerImpl* p = g_gameImpl.GetPlayer(num(0));
    static const std::pair<const char*, int> fields[] = {
      {"food", 0x10}, {"common", 0x20}, {"rare", 0x24}, {"moraleLevel", 0x34}, {"morale", 0x40}, {"workers", 0x94},
      {"scientists", 0x98}, {"kids", 0x9C}, {"solarsat", 0xEC} };
    int off = -1;
    for (const auto& [n, o] : fields) if (arg(1) == n) off = o;
    if (off < 0 && isdigit(uint8_t(arg(1)[0]))) off = num(1);
    if (!p || off < 0 || off >= int(sizeof(PlayerImpl))) { Log("script: player: bad field %s", arg(1).c_str());  return true; }
    if (arg(1) == "solarsat") p->satellites_.numSolarSatellites = num(2);
    else *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(p) + off) = num(2);
    p->recalculateValues_ = 1;
    return true;
  }
  // Disasters, as a mission DLL schedules them (the TethysGame::Set* exports 0x4782E0..0x4783E0, which create the
  // disaster object at the tile's centre); a trailing `now` skips the two warning waits (Disaster::StartDisaster).
  //   meteor x y size | quake x y magnitude | eruption x y speed | lightning x1 y1 duration x2 y2
  //   tornado x1 y1 duration x2 y2 | blight x y [speed] | lavaspeed n | lavapossible x1 y1 x2 y2 0|1
  if (v == "meteor" || v == "quake" || v == "eruption" || v == "lightning" || v == "tornado") {
    const bool now = arg(c.args.size() - 1) == "now";
    Unit d;
    if (v == "meteor")    d = Game::CreateMeteor(loc(0), MeteorSize(num(2)), now);
    if (v == "quake")     d = Game::CreateEarthquake(loc(0), num(2), now);
    if (v == "eruption")  d = Game::CreateEruption(loc(0), num(2), now);
    // An end x or y of -1 is left for the game to pick when the disaster strikes.
    const Location end(num(3) == -1 ? -1 : loc(3).x, num(4));
    if (v == "lightning") d = Game::CreateLightning(loc(0), end, num(2), now);
    if (v == "tornado")   d = Game::CreateTornado(loc(0), end, num(2), now);
    Log("%s at (%s,%s): unit %d", v.c_str(), arg(0).c_str(), arg(1).c_str(), d.GetID());
    return true;
  }
  if (v == "empmissile") {
    // TethysGame::SetEMPMissile (0x478420): an EMP missile of `owner` from a tile to a tile, as a mission launches
    // one (e12's FireEMP1).   empmissile launchX launchY owner targetX targetY
    using SetEMPMissileFn = void(__fastcall*)(int launchX, int launchY, int owner, int targetX, int targetY);
    const Location from = loc(0), to = loc(3);
    reinterpret_cast<SetEMPMissileFn>(0x478420)(from.x, from.y, num(2), to.x, to.y);
    Log("empmissile from (%s,%s) owner %d to (%s,%s)", arg(0).c_str(), arg(1).c_str(), num(2), arg(3).c_str(), arg(4).c_str());
    return true;
  }
  if (v == "blight") {
    // Game::CreateBlight: the speed (BlightManager::SetSpreadSpeed), then GameMap::SetVirusUL (0x476EA0), which takes
    // its flag in ecx and the Location on the stack (MinGW's fastcall passes TethysAPI's (Location, ibool) otherwise).
    if (c.args.size() > 2 && num(2) >= 0) Game::SetBlightSpeed(num(2));
    using SetVirusFn = void(__fastcall*)(int flag, int unused, int x, int y);
    reinterpret_cast<SetVirusFn>(0x476EA0)(1, 0, loc(0).x, loc(0).y);
    return true;
  }
  if (v == "lavaspeed") { Game::SetLavaSpeed(num(0));  return true; }
  if (v == "lavapossible") {
    const Location a = loc(0), b = loc(2);
    // GameMap::SetLavaPossible (0x476F20) takes the flag in ecx and the Location on the stack; MinGW's fastcall
    // passes TethysAPI's (Location, ibool) differently, so call it with that layout spelled out.
    using SetLavaPossibleFn = void(__fastcall*)(int flag, int unused, int x, int y);
    for (int y = a.y; y <= b.y; ++y)
      for (int x = a.x; x <= b.x; ++x) reinterpret_cast<SetLavaPossibleFn>(0x476F20)(num(4) != 0, 0, x, y);
    return true;
  }
  Unit u = ScriptUnit(arg(0));
  if (v == "poke") {
    if (!u.IsLive()) return true;
    uint8_t* b = reinterpret_cast<uint8_t*>(u.GetMapObject()) + num(1);
    const int size = num(2), value = num(3);
    if (size == 1) *b = uint8_t(value); else if (size == 2) *reinterpret_cast<uint16_t*>(b) = uint16_t(value);
    else *reinterpret_cast<int*>(b) = value;
    return true;
  }
  if (v == "beacon") {
    // Game::CreateMine's internal call (0x478940), with the internal variant number, so no random draw is made.
    const Location at = loc(0);
    const std::string kind = arg(2);
    const MapID type = kind == "vent" ? MapID::MagmaVent : kind == "fumarole" ? MapID::Fumarole : MapID::MiningBeacon;
    const int mineType = type == MapID::MiningBeacon ? num(2) : -1;  // as CreateMine: max(type, RandomOre)
    const ibool ok = OP2Thunk<0x478940, ibool FASTCALL(MapID, int, int, int, int, int)>(
      type, at.x, at.y, mineType, type == MapID::MiningBeacon ? num(3) : -1, type == MapID::MiningBeacon ? num(4) : -1);
    Log("beacon %s at (%d,%d): %d", kind.c_str(), at.x, at.y, ok);
    return true;
  }
  if (v == "route") {
    const Unit mine = ScriptUnit(arg(1)), smelter = ScriptUnit(arg(2));
    if (!u.IsLive() || !mine.IsLive() || !smelter.IsLive()) return true;
    CommandPacket packet = { CommandType::CargoRoute, sizeof(CargoRouteCommand) };
    auto& r = packet.data.cargoRoute;
    r.numUnits = 1;
    r.unitID[0] = uint16(u.GetID());
    r.numWaypoints = 3;
    r.waypoint[0] = mine.GetDockLocation().AsWaypoint();
    r.waypoint[1] = smelter.GetDockLocation().AsWaypoint();
    r.waypoint[2] = mine.GetDockLocation().AsWaypoint();
    r.mineWaypointIndex = 0;
    r.smelterWaypointIndex = 1;
    r.mineUnitId = uint16(mine.GetID());
    r.smelterUnitId = uint16(smelter.GetID());
    g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    return true;
  }
  if (v == "produce")  { if (u.IsLive()) u.DoProduce(TypeByName(arg(1)), TypeByName(arg(2)), 0xFFFF, false);  return true; }
  if (v == "idle")     { u.DoIdle();  return true; }
  if (v == "unidle")   { u.DoUnidle();  return true; }
  if (v == "train")    { u.DoTrainScientists(num(1));  return true; }
  if (v == "load")     { u.DoLoadCargo();  return true; }
  if (v == "unload")   { u.DoUnloadCargo();  return true; }
  if (v == "kit")      { if (u.IsLive()) u.SetCargo(TypeByName(arg(1)), TypeByName(arg(2)));  return true; }
  if (v == "cargo")    { if (u.IsLive()) u.SetCargo(CargoType(num(1)), num(2));  return true; }
  if (v == "bay")      { if (u.IsLive()) u.SetFactoryCargo(num(1), TypeByName(arg(2)), TypeByName(arg(3)));  return true; }
  if (v == "transfercargo") {  // a ctMoTransferCargo packet (TethysAPI's DoTransferCargo is the AI path; it crashed)
    if (!u.IsLive()) return true;
    CommandPacket packet = { CommandType::TransferCargo, sizeof(TransferCargoCommand) };
    packet.data.transferCargo.unitID  = uint16(u.GetID());
    packet.data.transferCargo.bay     = uint16(num(1));
    // The third argument is the packet's second word: a Spaceport's "load bay into the rocket" sends 1
    // (Spaceport::ActivateDock 0x47F420 swaps the bay with the rocket's cargo); 0 is a transfer to the dock.
    packet.data.transferCargo.unknown = uint16(c.args.size() > 2 ? num(2) : 0);
    g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    return true;
  }
  if (v == "launch") {  // a ctMoLaunch packet: launch the rocket on a Spaceport's pad (optional target pixel x y)
    if (!u.IsLive()) return true;
    CommandPacket packet = { CommandType::Launch, sizeof(LaunchCommand) };
    packet.data.launch.unitID       = uint16(u.GetID());
    packet.data.launch.targetPixelX = uint16(c.args.size() > 2 ? num(1) : 0);
    packet.data.launch.targetPixelY = uint16(c.args.size() > 2 ? num(2) : 0);
    g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    return true;
  }
  if (v == "build") {
    // The ctMoBuild packet of the legacy Unit::DoBuild export (0x476350): waypoint = the centre of the bottom-right tile,
    // rect = the footprint plus its 1-tile margin.
    // A Robo-Miner or GeoCon has no kit: name the structure (MINE, GEOTHERMAL) as the 4th argument for its size.
    const MapID kit = c.args.size() > 3 ? TypeByName(arg(3)) : MapID(u.IsLive() ? u.GetMapObject()->cargo_ : 0);
    if (!u.IsLive() || kit == MapID::None) { Log("script: build: no ConVec kit");  return true; }
    const auto& s = MapObjectType::GetInstance(kit)->stats_.building;
    const Location br = loc(1);
    CommandPacket packet = { CommandType::Build, sizeof(BuildCommand) };
    packet.data.build.numUnits     = 1;
    packet.data.build.unitID[0]    = uint16(u.GetID());
    packet.data.build.numWaypoints = 1;
    packet.data.build.waypoint[0]  = br.AsWaypoint(true, true);  // as the legacy export 0x476350 (TethysAPI uses y = tile top)
    packet.data.build.rect         = MapRect(br.x - s.width - 1, br.y - s.height - 1, br.x, br.y).AsPacked();
    packet.data.build.unknown      = -1;
    const ibool ok = g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    Log("build: %s at (%d,%d) -> %d", TypeName(kit), br.x, br.y, ok);
    return true;
  }
  // The Robo-Dozer's and the Earthworker's orders, as their commands' clicks build the packets (0x4538A0, 0x452780,
  // 0x452E30). Rectangles are inclusive here; ctMoBuildWall's bottom-right is exclusive in the packet (its handler
  // 0x4235E0 takes one off), and ctMoRemoveWall carries one waypoint (the tile's centre here) besides its tile.
  if (v == "doze" || v == "buildwall") {  // doze <dozer> x1 y1 x2 y2 | buildwall <earthworker> x1 y1 x2 y2 [TYPE]
    if (!u.IsLive()) return true;
    const Location a = loc(1), b = loc(3);
    CommandPacket packet = {};
    if (v == "doze") {
      packet = { CommandType::Doze, sizeof(DozeCommand) };
      packet.data.doze.numUnits  = 1;
      packet.data.doze.unitID[0] = uint16(u.GetID());
      packet.data.doze.rect      = MapRect(a.x, a.y, b.x, b.y).AsPacked();
    } else {
      packet = { CommandType::BuildWall, sizeof(BuildWallCommand) };
      auto& w = packet.data.buildWall;
      w.numUnits     = 1;
      w.unitID[0]    = uint16(u.GetID());
      w.rect.x1      = uint16(a.x);
      w.rect.y1      = uint16(a.y);
      w.rect.x2      = uint16(b.x + 1);
      w.rect.y2      = uint16(b.y + 1);
      w.tubeWallType = uint16(c.args.size() > 5 ? TypeByName(arg(5)) : MapID::Tube);
      w.unknown      = 0;
    }
    const ibool ok = g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    Log("%s: (%d,%d)-(%d,%d) -> %d", v.c_str(), a.x, a.y, b.x, b.y, ok);
    return true;
  }
  if (v == "removewall") {  // removewall <earthworker> x y
    if (!u.IsLive()) return true;
    const Location at = loc(1);
    CommandPacket packet = { CommandType::RemoveWall, sizeof(RemoveWallCommand) };
    auto& r = packet.data.removeWall;
    r.numUnits     = 1;
    r.unitID[0]    = uint16(u.GetID());
    r.numWaypoints = 1;
    r.waypoint[0]  = at.AsWaypoint();
    r.rect         = MapRect(at.x, at.y, at.x, at.y).AsPacked();
    const ibool ok = g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    Log("removewall: (%d,%d) -> %d", at.x, at.y, ok);
    return true;
  }
  // The targeted orders of the Repair, Reprogram and Dismantle commands, as CommandRepair::OnClick (0x4549F0) builds
  // its packet: the unit, no waypoints and the target's index (the click sends 0 when no valid target is under it;
  // `none` sends that). One packet per verb; more units after the target go into the same packet.
  if (v == "repairobj" || v == "reprogram" || v == "dismantle") {  // repairobj <unit> <target|none> [unit...]
    if (!u.IsLive()) return true;
    const Unit target = arg(1) == "none" ? Unit() : ScriptUnit(arg(1));
    CommandPacket packet = { v == "repairobj" ? CommandType::RepairObj : v == "reprogram" ? CommandType::Reprogram
                                                                       : CommandType::Dismantle, 0 };
    auto& r = packet.data.repair;
    r.numUnits  = 1;
    r.unitID[0] = uint16(u.GetID());
    for (size_t i = 2; i < c.args.size() && r.numUnits < 32; ++i) {
      const Unit more = ScriptUnit(arg(i));
      if (Exists(more)) r.unitID[r.numUnits++] = uint16(more.GetID());
    }
    // SimpleCommand's unit list is followed by the waypoint count (0) and the target.
    uint8_t* after = reinterpret_cast<uint8_t*>(&r.unitID[r.numUnits]);
    *reinterpret_cast<uint16_t*>(after) = 0;
    *reinterpret_cast<uint32_t*>(after + 2) = uint32_t(arg(1) == "none" ? 0 : target.GetID()) | 0xFFFF0000u;
    packet.dataLength = uint16(after + 6 - reinterpret_cast<uint8_t*>(&packet.data));
    const ibool ok = g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    Log("%s: %d -> %s (%d units) -> %d", v.c_str(), u.GetID(), arg(1).c_str(), int(r.numUnits), ok);
    return true;
  }
  (void)tick;
  return false;
}

// Mission AI groups (README "Group verbs"): `group <name> <op> [args]`, the mission API's ScGroup calls on a named group.
//   new fight|mining|building <player> | take <unit>... | remove <unit> | lights 0|1 | enable | disable | destroy
//   rect x1 y1 x2 y2 (FightGroup or BuildingGroup SetRect) | guardedrect x1 y1 x2 y2 | clearguarded
//   guardrect | patrolonly | attackenemy | guardgroup | guardunit | attackunit | exitmap      (the Do* objectives)
//   patrol <loop> x,y ... | clearpatrol | combine 0|1 | follow <mode> | targetunit <unit> | targetgroup <name>
//   attacktype <TYPE> | mine <mine unit> <smelter unit> x1 y1 x2 y2 | targcount <TYPE> <WEAPON|-> <n> | cleartargcount
//   reinforce <group> <priority> | unreinforce <group> | building x y <TYPE> <WEAPON|-> [group] | tube x y | wall x y [TYPE]
//   dump  (log the group's members in list order, and a fight group's state)
std::vector<std::pair<std::string, int>> g_groups;

int GroupId(const std::string& name) {
  for (const auto& [n, id] : g_groups) if (n == name) return id;
  return -1;
}

void RunGroupCmd(const ScriptCmd& c) {
  auto arg = [&](size_t i) -> std::string { return i < c.args.size() ? c.args[i] : std::string(); };
  auto num = [&](size_t i) { return int(strtol(arg(i).c_str(), nullptr, 0)); };
  auto loc = [&](size_t i) { return Location(num(i) + (c.fileCoords ? PadTiles() : 0), num(i + 1)); };
  auto rect = [&](size_t i) { const Location a = loc(i), b = loc(i + 2);  return MapRect(a.x, a.y, b.x, b.y); };
  const std::string name = arg(0), op = arg(1);
  if (op == "new") {
    const std::string kind = arg(2);
    const _Player p = Player[num(3)];
    const int id = kind == "mining" ? CreateMiningGroup(p).GetID() : kind == "building" ? CreateBuildingGroup(p).GetID()
                 : CreateFightGroup(p).GetID();
    g_groups.emplace_back(name, id);
    Log("group %s: %s group of player %d -> stub %d", name.c_str(), kind.c_str(), num(3), id);
    return;
  }
  const int id = GroupId(name);
  if (id < 0) { Log("script: group %s: unknown group", name.c_str());  return; }
  ScGroup g(id);
  FightGroup f(id);
  BuildingGroup b(id);
  MiningGroup m(id);
  auto* impl = g.GetImpl();
  const bool fight = impl && *reinterpret_cast<uintptr_t*>(impl) == 0x4CFF00;  // FightGroupImpl's vtable
  if      (op == "take")       for (size_t i = 2; i < c.args.size(); ++i) g.TakeUnit(ScriptUnit(arg(i)));
  else if (op == "remove")     g.RemoveUnit(ScriptUnit(arg(2)));
  else if (op == "lights")     g.SetLights(num(2) != 0);
  else if (op == "enable")     g.Enable();
  else if (op == "disable")    g.Disable();
  else if (op == "destroy")    g.Destroy();
  else if (op == "rect")       { if (fight) f.SetRect(rect(2));  else b.SetRect(rect(2)); }
  else if (op == "guardedrect") f.AddGuardedRect(rect(2));
  else if (op == "clearguarded") f.ClearGuardedRects();
  else if (op == "guardrect")  f.DoGuardRect();
  else if (op == "patrolonly") f.DoPatrolOnly();
  else if (op == "attackenemy") f.DoAttackEnemy();
  else if (op == "guardgroup") f.DoGuardGroup();
  else if (op == "guardunit")  f.DoGuardUnit();
  else if (op == "attackunit") f.DoAttackUnit();
  else if (op == "exitmap")    f.DoExitMap();
  else if (op == "clearpatrol") f.ClearPatrolMode();
  else if (op == "combine")    { if (num(2)) f.SetCombineFire();  else f.ClearCombineFire(); }
  else if (op == "follow")     f.SetFollowMode(num(2));
  else if (op == "targetunit") f.SetTargetUnit(ScriptUnit(arg(2)));
  else if (op == "targetgroup") {
    // SetTargetGroup takes ScGroup by value: MSVC copies its one int onto the stack, but MinGW passes a class with
    // a destructor by reference, so call the export with the stub index itself.
    using Fn = void(__thiscall*)(FightGroup*, int);
    reinterpret_cast<Fn>(0x479F90)(&f, GroupId(arg(2)));
  }
  else if (op == "attacktype") f.SetAttackType(TypeByName(arg(2)));
  else if (op == "patrol") {
    static Location points[9];
    int n = 0;
    for (size_t i = 3; i < c.args.size() && n < 8; ++i) {
      int x = 0, y = 0;
      if (sscanf(c.args[i].c_str(), "%d,%d", &x, &y) == 2) points[n++] = Location(x + (c.fileCoords ? PadTiles() : 0), y);
    }
    points[n] = Location(-1, -1);
    PatrolRoute route = { num(2), points };
    f.SetPatrolMode(route);
  }
  else if (op == "mine")       m.Setup(ScriptUnit(arg(2)), ScriptUnit(arg(3)), rect(4));
  else if (op == "targcount")  g.SetTargCount(TypeByName(arg(2)), TypeByName(arg(3)), num(4));
  else if (op == "cleartargcount") g.ClearTargCount();
  else if (op == "reinforce")  b.RecordVehReinforceGroup(ScGroup(GroupId(arg(2))), num(3));
  else if (op == "unreinforce") b.UnRecordVehGroup(ScGroup(GroupId(arg(2))));
  else if (op == "building") {
    if (c.args.size() > 6) b.RecordBuilding(loc(2), TypeByName(arg(4)), TypeByName(arg(5)), ScGroup(GroupId(arg(6))));
    else b.RecordBuilding(loc(2), TypeByName(arg(4)), TypeByName(arg(5)));
  }
  else if (op == "tube")       b.RecordTube(loc(2));
  else if (op == "wall")       b.RecordWall(loc(2), c.args.size() > 4 ? TypeByName(arg(4)) : MapID::Wall);
  else if (op == "dump") {
    std::string members;
    if (impl) for (auto* n = impl->pUnitListHead_; n; n = n->pNext)
      members += " " + std::to_string(n->pUnit->index_) + ":" + std::to_string(int(n->classification)) + "@" +
                 std::to_string(n->issueCommandTick);
    Log("group %s (stub %d): %d units:%s", name.c_str(), id, impl ? impl->numUnits_ : -1, members.c_str());
    if (fight) {
      const auto* fi = f.GetImpl();
      Log("group %s: objective %d combine %d/%d@%d patrol %d follow %d rect (%ld,%ld,%ld,%ld) guarded %d", name.c_str(),
          fi->field_3E4, fi->combineFire_, fi->combineFireUnitIndex_, fi->field_310, fi->patrolMode_, fi->followMode_,
          fi->pixelRect_.left, fi->pixelRect_.top, fi->pixelRect_.right, fi->pixelRect_.bottom, fi->numGuardedRects_);
    }
  }
  else Log("script: group: unknown op %s", op.c_str());
}

// Which of a verb's arguments are units that must exist (`first`..`last`, inclusive; `list`: every argument from
// `first`, the missing ones dropped instead of the command skipped). Verbs not listed take no units there.
struct UnitArgs { const char* verb;  size_t first, last;  bool list; };
const UnitArgs* UnitArgsOf(const std::string& v) {
  static const UnitArgs table[] = {
    {"move",0,0,0},{"dock",0,0,0},{"dockeg",0,0,0},{"attackground",0,0,0},{"lights",0,0,0},{"movewp",0,0,0},{"standground",0,0,0},
    {"patrol",0,0,0},{"stop",0,0,0},{"selfdestruct",0,0,0},{"research",0,0,0},{"poke",0,0,0},{"produce",0,0,0},{"idle",0,0,0},
    {"unidle",0,0,0},{"train",0,0,0},{"load",0,0,0},{"unload",0,0,0},{"kit",0,0,0},{"cargo",0,0,0},{"bay",0,0,0},
    {"transfercargo",0,0,0},{"launch",0,0,0},{"build",0,0,0},{"doze",0,0,0},{"buildwall",0,0,0},{"removewall",0,0,0},
    {"attack",0,1,0},{"guard",0,1,0},{"route",0,2,0},{"scatter",0,0,1},{"repairobj",0,1,0},{"reprogram",0,1,0},{"dismantle",0,1,0} };
  for (const auto& e : table) if (v == e.verb) return &e;
  return nullptr;
}

std::string OrderText(const ScriptCmd& c) {
  std::string t = c.verb;
  for (const auto& a : c.args) t += " " + a;
  return t;
}

void QueuePacket(const ScriptCmd& c, int tick);
void SaveLoadVerb(const ScriptCmd& c, int tick);

void RunScriptCmd(const ScriptCmd& orig, int tick) {
  ScriptCmd c = orig;  // Names resolve into ids in this copy.
  g_scriptFileCoords = c.fileCoords;
  // `as NAME` ends a produce or build: the unit it makes gets the name.
  std::string asName;
  if (c.args.size() >= 2 && c.args[c.args.size() - 2] == "as" && (c.verb == "produce" || c.verb == "build")) {
    asName = c.args.back();
    c.args.resize(c.args.size() - 2);
  }
  // Unit arguments. A `$name` or `$TYPE@x,y` becomes the id it resolves to now; one that doesn't resolve, or a plain id
  // past the object table (reading it crashes the game), skips the order with a warning. Any other id is sent as
  // before, a dead unit's included: the game's own checks apply, and its packet may still change the actor's state.
  if (const UnitArgs* ua = UnitArgsOf(c.verb)) {
    std::vector<std::string> ids;
    for (size_t i = 0; i < c.args.size(); ++i) {
      const std::string& a = c.args[i];
      const bool isUnit = i >= ua->first && (ua->list || i <= ua->last) && !(a == "none" && i == 1) && !(!a.empty() && a[0] == '@');
      if (!isUnit) { ids.push_back(a);  continue; }
      std::string why;
      if (!a.empty() && a[0] == '$') {
        const Unit u = ScriptUnit(a, &why);
        if (Exists(u)) { ids.push_back(std::to_string(u.GetID()));  continue; }
      } else if (!IdOutOfRange(a)) {
        ids.push_back(a);
        if (!Exists(ScriptUnit(a))) Log("script: tick %d line %d: `%s`: no live unit %s (sent anyway)", tick, c.line, OrderText(orig).c_str(), a.c_str());
        continue;
      } else why = "no unit " + a + " (past the object table)";
      if (why.empty()) why = "no unit " + a;
      Log("script: WARN tick %d line %d: `%s`: %s (%s)", tick, c.line, OrderText(orig).c_str(), why.c_str(),
          ua->list ? "unit left out" : "order skipped");
      if (!ua->list) return;
    }
    if (ids.empty() && ua->list) return;
    c.args = ids;
  } else if (c.verb == "name") {
    // name NAME <unit>: bind an existing unit now.
    std::string why;
    const Unit u = c.args.size() > 1 ? ScriptUnit(c.args[1], &why) : Unit();
    if (c.args.empty() || !ValidName(c.args[0])) Log("script: WARN tick %d line %d: `%s`: a name is letters, digits and _ (not a number) (not bound)", tick, c.line, OrderText(orig).c_str());
    else if (Exists(u)) BindName(c.args[0], u);
    else Log("script: WARN tick %d line %d: `%s`: %s (not bound)", tick, c.line, OrderText(orig).c_str(),
             why.empty() ? "no such unit" : why.c_str());
    return;
  }
  // A `produce`/`build` to a dead spawn is skipped below; it binds no name either.
  bool deadSpawn = false;
  if (!c.args.empty() && !c.args[0].empty() && c.args[0][0] == '@') {
    const size_t n = atoi(c.args[0].c_str() + 1);
    deadSpawn = n >= g_spawned.size() || (n < g_spawnGone.size() && g_spawnGone[n]);
  }
  if (!asName.empty() && !deadSpawn) {
    const Unit f = c.args.empty() ? Unit() : ScriptUnit(c.args[0]);
    const bool isProduce = c.verb == "produce";
    if (!ValidName(asName)) Log("script: WARN tick %d line %d: `%s as %s`: a name is letters, digits and _ (not a number) (not bound)", tick, c.line, OrderText(orig).c_str(), asName.c_str());
    else if (!Exists(f) || c.args.size() < (isProduce ? 2u : 3u)) Log("script: WARN tick %d line %d: `%s`: `as %s` needs a live unit and the order's arguments (not bound)", tick, c.line, OrderText(orig).c_str(), asName.c_str());
    else if (isProduce && (strcmp(TypeName(f.GetType()), "FACT_STRUCTURE") == 0 || TypeByName(c.args[1]) == MapID::None))
      Log("script: WARN tick %d line %d: `%s`: `as %s` can't bind: a Structure Factory makes kits, not units (not bound)", tick, c.line, OrderText(orig).c_str(), asName.c_str());
    else {
      NameScan(true);  // Bring the scan up to now, so only units that appear after this order count.
      PendingName w = { UpperName(asName), false, f.GetOwner(), 0, 0, 0, 0, 0, 0, 0, tick };
      if (isProduce) {
        w.type = int(TypeByName(c.args[1]));
        w.fx = f.GetLocation().x, w.fy = f.GetLocation().y;
      } else {  // build <convec> x y [TYPE]: the structure appears inside the footprint plus its margin
        const Location br(atoi(c.args[1].c_str()) + (c.fileCoords ? PadTiles() : 0), atoi(c.args[2].c_str()));
        const MapID kit = c.args.size() > 3 ? TypeByName(c.args[3]) : MapID(f.GetMapObject()->cargo_);
        int wd = 3, ht = 3;
        if (kit != MapID::None) { const auto& st = MapObjectType::GetInstance(int(kit))->stats_.building;  wd = st.width, ht = st.height; }
        w.structure = true, w.x1 = br.x - wd - 1, w.y1 = br.y - ht - 1, w.x2 = br.x + 1, w.y2 = br.y + 1;
      }
      g_pendingNames.push_back(w);
    }
  }
  auto arg = [&](size_t i) -> std::string { return i < c.args.size() ? c.args[i] : std::string(); };
  auto num = [&](size_t i) { return atoi(arg(i).c_str()); };
  auto unit = [&](size_t i) { return ScriptUnit(arg(i)); };
  auto loc = [&](size_t i) { return Location(num(i) + (c.fileCoords ? PadTiles() : 0), num(i + 1)); };
  std::string all;
  for (auto& a : c.args) all += " " + a;
  Log("script: tick %d: %s%s", tick, c.verb.c_str(), all.c_str());
  // A spawn that has died (or failed) gets no more orders: its object index may since belong to a shot or an
  // explosion, and an order to that crashes the game (Vehicle::SetDestinationWaypoints 0x42324F; fuzz seed 5, a Move to
  // a Scout 180 ticks after it died). The UI can't give such an order.
  if (const std::string a0 = arg(0); !a0.empty() && a0[0] == '@') {
    const size_t n = atoi(a0.c_str() + 1);
    if (n < g_spawnGone.size() && g_spawnGone[n]) { Log("script: spawn %u is gone: skipped", unsigned(n));  return; }
  }

  if      (c.verb == "move")         IssueMove(unit(0), loc(1));
  else if (c.verb == "dock")         IssueMove(unit(0), loc(1), CommandType::Dock);  // ctMoDock, to a dock tile
  else if (c.verb == "dockeg")       IssueMove(unit(0), loc(1), CommandType::DockEG);  // ctMoDockEG, an Eden Garage's dock
  else if (c.verb == "attack")       unit(0).DoAttack(unit(1));
  else if (c.verb == "guard")        IssueTargeted(unit(0), CommandType::Guard, unit(1).GetID(), 0, false);
  else if (c.verb == "attackground") IssueTargeted(unit(0), CommandType::Attack, loc(1).x, loc(1).y, true);
  else if (c.verb == "lights")       unit(0).DoSetLights(num(1) != 0);
  else if (c.verb == "movewp" || c.verb == "standground" || c.verb == "patrol") {
    // movewp/standground <unit> x,y[,range] ...; patrol <unit> <loop index> x,y[,range] ...
    const bool patrol = c.verb == "patrol";
    std::vector<Location> wps;
    std::vector<int> ranges;
    for (size_t i = patrol ? 2 : 1; i < c.args.size(); ++i) {
      int x = 0, y = 0, r = 0;
      if (sscanf(c.args[i].c_str(), "%d,%d,%d", &x, &y, &r) < 2) continue;
      wps.push_back(Location(x + (c.fileCoords ? PadTiles() : 0), y));
      ranges.push_back(r);
    }
    const CommandType type = patrol ? CommandType::Patrol
                           : c.verb == "standground" ? CommandType::StandGround : CommandType::Move;
    IssueWaypoints(unit(0), type, wps, ranges, patrol ? num(1) : -1);
  }
  else if (c.verb == "scatter") {
    // scatter <unit> <unit> ...: one ctMoScatter packet for the units, as the pane's SCATTER sends it (0x4242F0 moves
    // each unit away from their centre). Dead spawns are left out.
    CommandPacket packet = { CommandType::Scatter, 0 };
    auto& s = packet.data.move;
    uint8 n = 0;
    int owner = -1;
    for (size_t i = 0; i < c.args.size() && n < 32; ++i) {
      const std::string a = arg(i);
      if (!a.empty() && a[0] == '@' && size_t(atoi(a.c_str() + 1)) < g_spawnGone.size() && g_spawnGone[atoi(a.c_str() + 1)])
        continue;
      Unit u = unit(i);
      if (!u.IsLive()) continue;
      s.unitID[n++] = uint16(u.GetID());
      owner = u.GetOwner();
    }
    s.numUnits = n;
    packet.dataLength = uint16(1 + 2 * n);
    if (n) g_gameImpl.GetPlayer(owner)->ProcessCommandPacket(packet);
  }
  else if (c.verb == "stop")         unit(0).DoStop();
  else if (c.verb == "selfdestruct") unit(0).DoSelfDestruct();
  else if (c.verb == "research")     unit(0).DoResearch(num(1), num(2));
  else if (c.verb == "shot")         TakeShot(tick);
  else if (c.verb == "telemetry")    WriteTelemetry(tick);
  else if (c.verb == "trace")        WriteTrace(tick);
  else if (c.verb == "quit")         Quit("script quit");
  else if (c.verb == "gamespeed") {
    // The in-game Game Speed MORE/LESS buttons' own call (0x466342, 0x466437): GameImpl's helper 0x4231C0 (fastcall:
    // option, value) writes ctGameOpt (0x2B) option 0x14 = speed * 4 into the local player's pending slot, so in a
    // network game it crosses the wire like any command. The buttons also write the ini's GameSpeed (not done here).
    using QueueGameOptFn = void(__fastcall*)(int option, int value);
    const int speed = std::clamp(num(0), 1, 10);
    reinterpret_cast<QueueGameOptFn>(0x4231C0)(0x14, speed * 4);
    Log("script: tick %d gamespeed %d queued", tick, speed);
  }
  else if (c.verb == "spawn") {
    const MapID type = TypeByName(arg(0)), weapon = TypeByName(arg(4));
    const int dir = c.args.size() > 5 ? (num(5) & 0xFF) : 0;
    Unit u;
    if (type != MapID::None) u = Game::CreateUnit(type, loc(2), num(1), weapon, UnitDirection(dir / 32));
    if (u.IsLive() && dir % 32) u.GetMapObject()->rotation_ = uint8(dir);
    Log("spawn %u: %s -> unit %d", unsigned(g_spawned.size()), arg(0).c_str(), u.GetID());
    g_spawned.push_back(u.GetID());
    g_spawnGone.push_back(!u.IsLive());
    g_spawnSlot.push_back(u.IsLive() ? 0 : 3);
    // The type the game made: a common ore mine on a rare beacon becomes MINE_ADV (the spawn stays listed).
    g_spawnType.push_back(u.IsLive() ? u.GetType() : type);
  }
  else if (c.verb == "view") {
    Player[0].CenterViewOn(loc(0));
    // The detail pane applies the new view when it is next drawn; in input runs draw now, so later inputs see it.
    if (g_inputUsed) g_lastInputTick = tick;
  }
  else if (IsInputVerb(c.verb))      RunInputCmd(c, tick);
  else if (c.verb == "group")        RunGroupCmd(c);
  else if (c.verb == "packet")       QueuePacket(c, tick);
  else if (c.verb == "savegame" || c.verb == "loadgame") SaveLoadVerb(c, tick);
  else if (c.verb == "setcell") {
    const int pad = c.fileCoords ? PadTiles() : 0;
    for (int y = num(1); y <= num(3); ++y)
      for (int x = num(0); x <= num(2); ++x) GameMap::SetCellType(Location(x + pad, y), CellType(num(4)));
  }
  else if (!RunEcoCmd(c, tick)) Log("script: unknown verb %s", c.verb.c_str());
}

// ---------------------------------------------------------------------------------------------------------------------
// Audio log (OP2H_AUDIO, `run.sh --audio`): every sound the game asks for, as `audio.jsonl` (README "Audio log").
// The game's own sound calls are hooked at their entry, so requests are logged whether or not sound is on:
//   AddMapSound (SoundManager 0x47EDB0, thiscall x, y, soundID; x = -1 for a sound without a position): every effect;
//   AddGameSound (0x47EFD0, thiscall soundID, player flag): a sound without a position, through AddMapSound;
//   AddSavantSound (0x47F010, thiscall soundID, x, y, player flag): a Savant line for the queue (0x47F050, priority
//   byte 0x56E7D0[id - 0x5E]);
//   AddMessage (MessageLog 0x439070, thiscall x, y, text, soundID): a status message, with its throttle verdict from
//   0x439590 (Savant ids 0x60..0xB1 are dropped, message and all, while a recent one of their group stands).
// The Savant queue is drained by SoundManager's per-frame update (0x47E9F0 -> 0x47F200: next line when the clock passes
// the last line's length + 750 ms), which only runs with sound on; the harness calls 0x47F200 itself on the same
// ticks (tick & 3 == 2) and logs what it starts as `play`. The throttle and the queue use timeGetTime: their call sites
// read a virtual clock instead, tick * 1000 / 36 ms (real time at speed 10), so the run is deterministic.
// The music playlist (MusicManager 0x565390: +0xC4 count, +0xC8 repeat index, +0xCC song list, +0xD0 position) is
// logged when it changes.

FILE* g_audio = nullptr;
bool  g_inPump = false;
int64_t Now();
int64_t Frequency();
constexpr uint32_t VirtualClockBase = 100000000;  // A machine up for a day: no throttle slot looks recent at start.
DWORD WINAPI VirtualTimeGetTime() { return VirtualClockBase + DWORD(int64_t(g_gameImpl.tick_) * 1000 / 36); }
void* g_virtualClock = reinterpret_cast<void*>(&VirtualTimeGetTime);  // The `call [ptr]` sites point here.

const char* SoundName(int id) {
  const auto* names = reinterpret_cast<const char* const*>(0x4E0170);  // SoundManager's file names, by SoundID.
  if (id == 0) return "";  // A message without a sound (AddMessage plays beep8 for it).
  return (id > 0 && id < 0xFA && names[id]) ? names[id] : "?";
}

// Internal pixel -> file pixel (x); -1 stays -1 (no position).
int FileX(int x) { return x == -1 ? -1 : x - PadTiles() * 32; }

// `at` is the return address in Outpost2.exe of the call that asked for the sound, or "opu" for code outside it (OPU
// replaces some of the game's message code; its DLL's addresses aren't stable between runs).
void AudioEvent(const char* ev, int id, int x, int y, void* caller, const char* extra = "") {
  if (!g_audio) return;
  const uintptr_t a = reinterpret_cast<uintptr_t>(caller);
  char at[16];
  if (a >= 0x401000 && a < 0x4CF000) snprintf(at, sizeof(at), "%06x", unsigned(a));
  else strcpy(at, "opu");
  fprintf(g_audio, "{\"tick\":%d,\"ev\":\"%s\",\"id\":%d,\"name\":\"%s\",\"x\":%d,\"y\":%d,\"at\":\"%s\"%s}\n",
          g_gameImpl.tick_, ev, id, SoundName(id), FileX(x), y, at, extra);
}

// An inline hook: the function's first `len` bytes (whole, position-independent instructions, checked against
// `expect`) move to a trampoline that jumps back, and the function starts with a jump to `hook`.
// A function another mod already starts with a jump (OPU hooks some) is chained: the hook calls that jump's target.
void* HookEntry(uintptr_t fn, const uint8_t* expect, size_t len, void* hook) {
  const auto* code = reinterpret_cast<const uint8_t*>(fn);
  if (code[0] == 0xE9) {
    int32_t rel;
    memcpy(&rel, code + 1, 4);
    void* prev = reinterpret_cast<void*>(fn + 5 + rel);
    Log("audio: %08x already jumps to %p; chaining", unsigned(fn), prev);
    JumpTo(fn, hook);
    return prev;
  }
  if (memcmp(code, expect, len) != 0) {
    Log("audio: unexpected code at %08x: %02x %02x %02x %02x %02x %02x", unsigned(fn), code[0], code[1], code[2], code[3],
        code[4], code[5]);
    return nullptr;
  }
  auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  memcpy(t, reinterpret_cast<void*>(fn), len);
  t[len] = 0xE9;
  const int32_t rel = int32_t(fn + len - (reinterpret_cast<uintptr_t>(t) + len + 5));
  memcpy(t + len + 1, &rel, 4);
  JumpTo(fn, hook);
  return t;
}

using AddMapSoundFn = int(__thiscall*)(void*, int, int, int);
using AddGameSoundFn = int(__thiscall*)(void*, int, int);
using SavantFn      = int(__thiscall*)(void*, int, int, int, int);
using AddMessageFn  = int(__thiscall*)(void*, int, int, const char*, int);
using ThrottleFn    = int(__thiscall*)(void*, int, int, int);
AddMapSoundFn g_addMapSound;
AddGameSoundFn g_addGameSound;
SavantFn      g_savant;
void*         g_outerCaller = nullptr;  // AddGameSound's caller while it calls AddMapSound.
AddMessageFn  g_addMessage;
ThrottleFn    g_throttle;
int g_throttled = -1;  // The verdict of the throttle call made by the AddMessage in progress.

int __fastcall AddMapSoundHook(void* self, void*, int x, int y, int id) {
  AudioEvent(g_inPump ? "play" : "sfx", id, x, y, g_outerCaller ? g_outerCaller : __builtin_return_address(0));
  return g_addMapSound(self, x, y, id);
}

int __fastcall AddGameSoundHook(void* self, void*, int id, int flag) {
  g_outerCaller = __builtin_return_address(0);
  const int r = g_addGameSound(self, id, flag);
  g_outerCaller = nullptr;
  return r;
}

int __fastcall SavantHook(void* self, void*, int id, int x, int y, int flag) {
  AudioEvent("savant", id, x, y, __builtin_return_address(0));
  return g_savant(self, id, x, y, flag);
}

int __fastcall ThrottleHook(void* self, void*, int id, int x, int y) {
  return g_throttled = g_throttle(self, id, x, y);
}

void MessageEvent(int x, int y, const char* text);
void ShellMessage(int x, int y, const char* text, int sound);
int __fastcall AddMessageHook(void* self, void*, int x, int y, const char* text, int sound) {
  g_throttled = -1;
  void* caller = __builtin_return_address(0);
  const int r = g_addMessage(self, x, y, text, sound);
  // pass: 1 shown (and voiced), 0 dropped by the throttle, -1 not a throttled id.
  char extra[32];
  snprintf(extra, sizeof(extra), ",\"pass\":%d", g_throttled);
  AudioEvent("msg", sound, x, y, caller, extra);
  MessageEvent(x, y, text);
  ShellMessage(x, y, text, sound);
  return r;
}

void WriteAudioTables() {
  if (!g_audio) return;
  // Savant priorities as SavantQueue reads them (0x47F180), the table SoundManager's constructor fills (0x47DCE0) and
  // each sound's length in ms (0x56DC00, 12 bytes per id, from the WAV header at load, 0x47F300).
  const auto* b = reinterpret_cast<const uint8_t*>(0x56E7D0);
  fprintf(g_audio, "{\"tick\":%d,\"ev\":\"tables\",\"prio\":[", g_gameImpl.tick_);
  for (int id = 0x5E; id <= 0xE3; ++id) fprintf(g_audio, "%s%d", id > 0x5E ? "," : "", b[id - 0x5E]);
  fprintf(g_audio, "],\"prio_ctor\":[");
  for (int id = 0x5E; id <= 0xE3; ++id) fprintf(g_audio, "%s%d", id > 0x5E ? "," : "", b[id]);
  fprintf(g_audio, "],\"ms\":[");
  for (int id = 0; id < 0xFA; ++id) fprintf(g_audio, "%s%d", id ? "," : "", *reinterpret_cast<const int*>(0x56DC00 + 12 * id));
  fprintf(g_audio, "]}\n");
}

struct MusicState {
  std::vector<int> songs;
  int repeat = -1, pos = -1;
  bool operator==(const MusicState& o) const { return songs == o.songs && repeat == o.repeat && pos == o.pos; }
};
MusicState g_music;

void LogMusic(const char* when) {
  if (!g_audio) return;
  MusicState m;
  const int n = *reinterpret_cast<const int*>(0x565454);
  const int* list = *reinterpret_cast<int* const*>(0x56545C);
  if (list) m.songs.assign(list, list + std::clamp(n, 0, 64));
  m.repeat = *reinterpret_cast<const int*>(0x565458);
  m.pos    = *reinterpret_cast<const int*>(0x565460);
  if (m == g_music) return;
  g_music = m;
  fprintf(g_audio, "{\"tick\":%d,\"ev\":\"music\",\"when\":\"%s\",\"songs\":[", g_gameImpl.tick_, when);
  for (size_t i = 0; i < m.songs.size(); ++i) fprintf(g_audio, "%s%d", i ? "," : "", m.songs[i]);
  fprintf(g_audio, "],\"repeat\":%d,\"pos\":%d}\n", m.repeat, m.pos);
}

// After each game cycle: the Savant queue's turn (as SoundManager's per-frame update would take it), then the music.
void AudioTick(int tick) {
  if (!g_audio) return;
  if ((tick & 3) == 2) {
    g_inPump = true;
    reinterpret_cast<void(__thiscall*)(void*)>(0x47F200)(reinterpret_cast<void*>(0x56D250));
    g_inPump = false;
  }
  LogMusic("tick");
  fflush(g_audio);
}

// PrepareGame starts the music (MusicManager::Init 0x4508D0, called at 0x489530: the default playlist and a 92 ms
// streaming timer) before the mission's InitProc runs (0x48987A), so a mission's SetMusicPlayList replaces a list that
// is already running. Whether the default's first track got to start depends on the time in between: logged here.
using ThisFn = int(__thiscall*)(void*);
ThisFn  g_musicInit, g_initProc;
int64_t g_musicInitAt = 0;
int __fastcall MusicInitHook(void* self, void*) {
  g_musicInitAt = Now();
  return g_musicInit(self);
}
// The message log's six throttle stamps (MessageLog 0x54FCF0 +0x1310..+0x132C) are set by its static initializer
// (0x438FE0) from the real timeGetTime, before the harness hooks the clock; once the machine has been up past the virtual
// clock's base (~27.7 h) they all look recent and no throttled message passes all run. They are rebased here to what
// the message log's reset (0x439510) would write at time 0, so a run's verdicts don't depend on the machine's uptime.
void RebaseMessageThrottle() {
  static const struct { uint32_t addr, offset; } stamps[] = {
    { 0x55101C, 5000 }, { 0x551010, 15000 }, { 0x551000, 25000 }, { 0x551008, 35000 }, { 0x551014, 45000 }, { 0x551018, 55000 } };
  for (const auto& s : stamps) *reinterpret_cast<uint32_t*>(s.addr) = s.offset;
}
int __fastcall InitProcHook(void* self, void*) {
  Log("audio: InitProc starts %.1f ms after MusicManager::Init", (Now() - g_musicInitAt) * 1000.0 / Frequency());
  RebaseMessageThrottle();
  return g_initProc(self);
}

// Session state without the audio log: only the status message hook, for `msg` events (nothing else changes).
void InstallMessageHook() {
  static const uint8_t message[] = { 0x53, 0x56, 0x57, 0x8B, 0xD9 };  // push ebx; push esi; push edi; mov ebx,ecx
  g_addMessage = reinterpret_cast<AddMessageFn>(HookEntry(0x439070, message, 5, reinterpret_cast<void*>(&AddMessageHook)));
  Log("state: message hook %s", g_addMessage ? "installed" : "FAILED");
}

void InstallAudioHooks() {
  g_musicInit = reinterpret_cast<ThisFn>(RelinkCall(0x489530, reinterpret_cast<void*>(&MusicInitHook)));
  g_initProc  = reinterpret_cast<ThisFn>(RelinkCall(0x48987A, reinterpret_cast<void*>(&InitProcHook)));
  static const uint8_t mapSound[] = { 0x83, 0xEC, 0x10, 0x53, 0x56 };  // sub esp,10h; push ebx; push esi
  static const uint8_t gameSound[] = { 0x8B, 0x44, 0x24, 0x08, 0x56 };  // mov eax,[esp+8]; push esi
  static const uint8_t savant[]   = { 0x53, 0x8B, 0xD1, 0x8B, 0x44, 0x24, 0x14 };  // push ebx; mov edx,ecx; mov eax,[esp+14h]
  static const uint8_t message[]  = { 0x53, 0x56, 0x57, 0x8B, 0xD9 };  // push ebx; push esi; push edi; mov ebx,ecx
  static const uint8_t throttle[] = { 0x83, 0xEC, 0x04, 0x53, 0x56 };  // sub esp,4; push ebx; push esi
  g_addMapSound = reinterpret_cast<AddMapSoundFn>(HookEntry(0x47EDB0, mapSound, 5, reinterpret_cast<void*>(&AddMapSoundHook)));
  g_addGameSound = reinterpret_cast<AddGameSoundFn>(HookEntry(0x47EFD0, gameSound, 5, reinterpret_cast<void*>(&AddGameSoundHook)));
  g_savant      = reinterpret_cast<SavantFn>(HookEntry(0x47F010, savant, 7, reinterpret_cast<void*>(&SavantHook)));
  g_addMessage  = reinterpret_cast<AddMessageFn>(HookEntry(0x439070, message, 5, reinterpret_cast<void*>(&AddMessageHook)));
  g_throttle    = reinterpret_cast<ThrottleFn>(HookEntry(0x439590, throttle, 5, reinterpret_cast<void*>(&ThrottleHook)));
  // `call [timeGetTime]` (FF 15 E0 38 58 00) in AddMessage, the throttle, the message log reset and the Savant queue.
  for (uintptr_t site : { 0x4390D1u, 0x439514u, 0x439599u, 0x47F108u, 0x47F206u, 0x47F2DBu }) {
    static const uint8_t call[] = { 0xFF, 0x15, 0xE0, 0x38, 0x58, 0x00 };
    if (memcmp(reinterpret_cast<void*>(site), call, 6) != 0) { Log("audio: no timeGetTime call at %08x", unsigned(site));  continue; }
    void** slot = &g_virtualClock;
    WriteCode(site + 2, &slot, 4);
  }
  Log("audio: hooks %s", g_addMapSound && g_addGameSound && g_savant && g_addMessage && g_throttle ? "installed" : "INCOMPLETE");
}

// ---------------------------------------------------------------------------------------------------------------------
// Optional bot DLL: exports `void __cdecl HarnessBotTick(int tick)`, called after every game cycle.

using BotTickFn = void(__cdecl*)(int);
BotTickFn g_botTick = nullptr;

// ---------------------------------------------------------------------------------------------------------------------
// Session state (OP2H_STATE, `run.sh --state`; README "Session state"): for tools that play a mission by replaying a
// growing script (op2-inspect's game_* tools). Throughout the run `events.jsonl` gets what a player would notice:
//   new / gone   a unit (structure or vehicle of any player) appeared / disappeared (object index, type, owner, tile)
//   hit          a unit lost hit points (the amount and what is left)
//   idle         a unit of player 0 finished its command (`was`: the command it had, e.g. Develop for a new structure)
//   tech         player 0 got a topic; objective: a victory condition's "met" changed; msg: a status message;
//   disaster     a disaster object appeared (scheduled: it warns, then strikes); end: the mission's end reason
// and at the end `state.json` holds the whole state (colony, labs, every unit and Gaia object, the tech tree, the
// objectives, the cell grid). Coordinates are file tiles. Reading state changes nothing in the game: a run with
// --state plays exactly like one without. OP2H_STOP (`--stop`) ends the run at the first event of the listed kinds
// after tick OP2H_STOP_FROM (`kind` or `kind:<unit id>`, comma separated; `any` = every kind but msg and hit).

FILE* g_events = nullptr;
bool  g_eventsWritten = false;  // Something to flush at the end of the session tick.
bool  g_stateOn = false;
std::string g_stopSpec;
int   g_stopFrom = 0;
std::string g_stopReason;  // Set when an event matched OP2H_STOP: the cycle hook quits after this tick.

void JsonStr(FILE* f, const char* s) {
  fputc('"', f);
  for (; s && *s; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c == '"' || c == '\\') { fputc('\\', f);  fputc(c, f); }
    else if (c < 0x20 || c >= 0x80) fprintf(f, "\\u%04x", c);  // Control characters; Latin-1 as its code point.
    else fputc(c, f);
  }
  fputc('"', f);
}

// Whether an event of `kind` (about unit `id`, -1 for none) is one OP2H_STOP asks to stop at.
bool StopMatches(const char* kind, int id) {
  if (g_stopSpec.empty() || g_gameImpl.tick_ <= g_stopFrom) return false;
  const std::string k = kind;
  size_t pos = 0;
  while (pos <= g_stopSpec.size()) {
    const size_t end = std::min(g_stopSpec.find(',', pos), g_stopSpec.size());
    const std::string tok = g_stopSpec.substr(pos, end - pos);
    pos = end + 1;
    const size_t colon = tok.find(':');
    const std::string tk = tok.substr(0, colon);
    if (tk == "any" && k != "msg" && k != "hit") return true;
    if (tk != k) continue;
    if (colon == std::string::npos || atoi(tok.c_str() + colon + 1) == id) return true;
  }
  return false;
}

// Starts an event line; the caller adds fields (",\"x\":1") and calls EventEnd.
bool EventBegin(const char* kind) {
  if (!g_events) return false;
  g_eventsWritten = true;
  fprintf(g_events, "{\"tick\":%d,\"ev\":\"%s\"", g_gameImpl.tick_, kind);
  return true;
}
void EventEnd(const char* kind, int id) {
  fputs("}\n", g_events);
  if (g_stopReason.empty() && StopMatches(kind, id)) g_stopReason = std::string(kind) + (id >= 0 ? " " + std::to_string(id) : "");
}

// A unit event. A unit of another player appearing is an `enemy` event (`new` is player 0's).
void EventUnit(const char* kind, int id, int type, int owner, int x, int y, const char* extra = "") {
  if (owner != 0 && strcmp(kind, "new") == 0) kind = "enemy";
  if (!EventBegin(kind)) return;
  fprintf(g_events, ",\"id\":%d,\"type\":\"%s\",\"owner\":%d,\"x\":%d,\"y\":%d%s", id, TypeName(MapID(type)), owner, x, y, extra);
  EventEnd(kind, id);
}

void MessageEvent(int x, int y, const char* text) {
  if (!EventBegin("msg")) return;
  fputs(",\"text\":", g_events);
  JsonStr(g_events, text);
  // x/y are internal pixels, -1 for a message without a place.
  fprintf(g_events, ",\"x\":%d,\"y\":%d", x < 0 ? -1 : x / 32 - PadTiles(), y < 0 ? -1 : y / 32);
  EventEnd("msg", -1);
}

// What the last session tick saw of each object index.
// `path` is the PathContext last checked readable (IsBadReadPtr is a slow call: only when the pointer changes).
struct SeenUnit {
  int type = -1, owner = -1, cmd = 0, damage = 0, x = 0, y = 0;  bool live = false;  int destX = -1, destY = -1;
  const PathContext* path = nullptr;
};
std::vector<SeenUnit> g_seen;
std::vector<uint8_t> g_seenNow;        // Reused each tick: the object indices seen live this tick.
std::vector<uint8_t> g_seenTech;      // Per tech number: player 0 has it.
std::vector<int>     g_seenObjective;  // Per ScStub index: -1 not a victory condition, else met (0/1).
std::vector<int>     g_seenDisaster;   // Per object index: the disaster type seen there (-1 none).
bool g_sessionStarted = false;
bool g_sessionSilent = false;  // A checkpoint's resume: take in the loaded game without an event (SessionRebaseline).

constexpr uintptr_t VictoryConditionVtbl = 0x4D68C0;  // VictoryConditionImpl (TethysAPI ScBase.h).

// The victory conditions' trigger has fired for player 0: the objective is met now.
int ObjectiveMet(const VictoryConditionImpl* v) {
  const TriggerImpl* t = TriggerImpl::GetInstance(v->victoryTriggerIndex_);
  if (!t) return 0;
  uint32_t mask;
  memcpy(&mask, &t->hasFired_, sizeof(mask));
  return int(mask & 1);
}

// The ScStub indices holding victory conditions, found by a scan of the list (IsBadReadPtr on every slot, slow), redone
// when the list's last created index changes (a mission creating stubs) and every 512 ticks.
std::vector<int> g_objectiveStubs;
int g_objectiveScanTick = -1000000, g_objectiveScanLast = -2;

template <typename F> void ForEachObjective(F f) {
  ScStubList* list = ScStubList::GetInstance();
  const int tick = g_gameImpl.tick_, lastCreated = int(list->lastCreatedIndex_);
  if (lastCreated != g_objectiveScanLast || tick - g_objectiveScanTick >= 512 || tick < g_objectiveScanTick) {
    g_objectiveStubs.clear();
    for (size_t i = 0; i < ScStubList::MaxNumScStubs; ++i) {
      ScBase* s = list->At(i);
      if (s && !IsBadReadPtr(s, sizeof(VictoryConditionImpl)) && *reinterpret_cast<uintptr_t*>(s) == VictoryConditionVtbl)
        g_objectiveStubs.push_back(int(i));
    }
    g_objectiveScanTick = tick, g_objectiveScanLast = lastCreated;
  }
  for (int i : g_objectiveStubs) {
    ScBase* s = list->At(i);
    // A stub destroyed since the scan: its slot is empty or holds something else.
    if (!s || *reinterpret_cast<uintptr_t*>(s) != VictoryConditionVtbl) continue;
    f(i, static_cast<const VictoryConditionImpl*>(static_cast<TriggerImpl*>(s)));
  }
}

int HighestObjectIndex() { return std::min(*reinterpret_cast<const int*>(0x54F838), int(MapImpl::GetInstance()->MaxNumUnits()) - 1); }

bool IsDisaster(MapID t) { return t >= MapID::Lightning && t <= MapID::Meteor; }

// After each cycle (and its script commands): compare with what the previous tick saw and log the differences.
void SessionTick(int tick) {
  if (!g_events) return;
  const bool baseline = !g_sessionStarted;
  g_sessionStarted = true;
  const int pad = PadTiles();
  // Units of every player.
  if (g_seen.size() < MapImpl::GetInstance()->MaxNumUnits()) g_seen.resize(MapImpl::GetInstance()->MaxNumUnits());
  std::vector<uint8_t>& now = g_seenNow;
  now.assign(g_seen.size(), 0);
  ForEachPlayerUnit([&](Unit u) {
    if (!u.IsLive()) return;
    const MapObject* mo = u.GetMapObject();
    const int id = u.GetID();
    if (id <= 0 || size_t(id) >= g_seen.size()) return;
    now[id] = 1;
    SeenUnit& s = g_seen[id];
    const Location at = u.GetLocation();
    const int type = int(u.GetType()), owner = u.GetOwner(), x = at.x - pad, y = at.y;
    if (!s.live || s.type != type || s.owner != owner) {
      if (s.live && !baseline) EventUnit("gone", id, s.type, s.owner, s.x, s.y);
      if (!baseline) EventUnit("new", id, type, owner, x, y);
      s = SeenUnit{ type, owner, mo->command_, mo->damage_, x, y, true };
      // A resume's baseline also takes in where each vehicle is going, as the continued game's last tick had it.
      if (!g_sessionSilent) return;
    }
    if (mo->damage_ > s.damage && !baseline) {
      char extra[64];
      snprintf(extra, sizeof(extra), ",\"dmg\":%d,\"hp\":%d", mo->damage_ - s.damage, u.GetMaxHitpoints() - mo->damage_);
      EventUnit("hit", id, type, owner, x, y, extra);
    }
    const int cmd = mo->command_;
    if (owner == 0 && cmd == int(CommandType::Nop) && s.cmd != cmd && s.cmd != int(CommandType::Idle) && !baseline) {
      // Where it was going (a vehicle stopped short, e.g. by an EMP, isn't "arrived"), and whether it is EMPed.
      char extra[96];
      snprintf(extra, sizeof(extra), ",\"was\":\"%s\",\"dest\":[%d,%d],\"emp\":%d", CommandName(s.cmd), s.destX, s.destY,
               (mo->flags_ & MoFlagEMPed) ? 1 : 0);
      EventUnit("idle", id, type, owner, x, y, extra);
    }
    if (u.IsVehicle() && cmd != int(CommandType::Nop)) {
      const PathContext* pc = mo->pPathContext_;
      if (pc != s.path) s.path = (pc && !IsBadReadPtr(pc, sizeof(PathContext))) ? pc : nullptr;
      if (s.path) s.destX = pc->dstTileX - pad, s.destY = pc->dstTileY;
    } else if (cmd == int(CommandType::Nop)) {
      s.destX = s.destY = -1;
    }
    s.cmd = cmd, s.damage = mo->damage_, s.x = x, s.y = y;
  });
  for (size_t id = 1; id < g_seen.size(); ++id)
    if (g_seen[id].live && !now[id]) {
      if (!baseline) EventUnit("gone", int(id), g_seen[id].type, g_seen[id].owner, g_seen[id].x, g_seen[id].y);
      g_seen[id].live = false;
    }
  // Player 0's research.
  if (g_seenTech.size() != size_t(g_research.numTechs_)) g_seenTech.assign(g_research.numTechs_, 0);
  for (int t = 0; t < g_research.numTechs_; ++t) {
    const uint8_t has = g_research.ppTechInfos_[t]->playerHasTechMask.Get(0) ? 1 : 0;  // What HasTech reads, without the call.
    if (has && !g_seenTech[t] && !baseline && EventBegin("tech")) {
      fprintf(g_events, ",\"id\":%d,\"name\":", g_research.ppTechInfos_[t]->techID);
      JsonStr(g_events, g_research.ppTechInfos_[t]->pTechName);
      EventEnd("tech", g_research.ppTechInfos_[t]->techID);
    }
    g_seenTech[t] = has;
  }
  // Objectives.
  if (g_seenObjective.empty()) g_seenObjective.assign(ScStubList::MaxNumScStubs, -1);
  ForEachObjective([&](int i, const VictoryConditionImpl* v) {
    const int met = ObjectiveMet(v);
    if (g_seenObjective[i] != met && (g_seenObjective[i] >= 0 || met) && !g_sessionSilent && EventBegin("objective")) {
      fprintf(g_events, ",\"met\":%d,\"text\":", met);
      JsonStr(g_events, v->pObjectiveText_);
      EventEnd("objective", -1);
    }
    g_seenObjective[i] = met;
  });
  // Disasters (scheduled ones exist from their first warning on).
  if (g_seenDisaster.size() != g_seen.size()) g_seenDisaster.assign(g_seen.size(), -1);
  const int last = HighestObjectIndex();
  for (int i = 1; i <= last && size_t(i) < g_seenDisaster.size(); ++i) {
    const MapObject* mo = MapObject::GetInstance(i);
    const int t = (mo && mo->IsLive() && IsDisaster(mo->GetTypeID())) ? int(mo->GetTypeID()) : -1;
    if (t >= 0 && g_seenDisaster[i] != t && !g_sessionSilent && EventBegin("disaster")) {
      fprintf(g_events, ",\"type\":\"%s\",\"x\":%d,\"y\":%d,\"timer\":%d", TypeName(MapID(t)), mo->pixelX_ / 32 - pad,
              mo->pixelY_ / 32, mo->actionTimer_);
      EventEnd("disaster", -1);
    }
    g_seenDisaster[i] = t;
  }
  if (g_gameImpl.gameTermReasons_ != GameTermReasons::Running && !g_sessionSilent && EventBegin("end")) {
    fprintf(g_events, ",\"reason\":%d", int(g_gameImpl.gameTermReasons_));
    EventEnd("end", -1);
  }
  (void)tick;
  if (g_eventsWritten) fflush(g_events);
  g_eventsWritten = false;
}

// One unit of state.json: the telemetry fields plus what a player reads off its panel.
void WriteStateUnit(FILE* f, bool& first, Unit u) {
  if (!u.IsLive()) return;
  const MapObject* mo = u.GetMapObject();
  const auto* b = reinterpret_cast<const uint8_t*>(mo);
  const Location at = u.GetLocation();
  fprintf(f, "%s{\"id\":%d,\"type\":\"%s\",\"owner\":%d,\"x\":%d,\"y\":%d,\"hp\":%d,\"maxHp\":%d,\"cmd\":\"%s\",\"act\":\"%s\"",
          first ? "" : ",\n", u.GetID(), TypeName(u.GetType()), u.GetOwner(), at.x - PadTiles(), at.y,
          u.GetMaxHitpoints() - u.GetDamage(), u.GetMaxHitpoints(), CommandName(mo->command_), ActionName(int(mo->action_)));
  first = false;
  if (u.IsVehicle()) {
    const auto* v = static_cast<const Vehicle*>(mo);
    if (u.GetType() == MapID::CargoTruck)
      fprintf(f, ",\"truck\":[%d,%d]", int(mo->truckCargoType_), int(mo->truckCargoAmount_));
    else if (u.GetType() == MapID::ConVec) {
      if (mo->cargo_ > 0 && mo->cargo_ < 0x80) fprintf(f, ",\"kit\":\"%s\"", TypeName(MapID(mo->cargo_)));
      if (v->weaponOfCargo_ > 0) fprintf(f, ",\"kitWeapon\":\"%s\"", TypeName(MapID(v->weaponOfCargo_)));
    } else if (const int w = mo->weapon_; w >= int(MapID::AcidCloud) && w <= int(MapID::BFG))
      fprintf(f, ",\"weapon\":\"%s\"", TypeName(MapID(w)));
    const PathContext* pc = mo->pPathContext_;
    if (pc && !IsBadReadPtr(pc, sizeof(PathContext)) && mo->command_ != int(CommandType::Nop))
      fprintf(f, ",\"dest\":[%d,%d]", pc->dstTileX - PadTiles(), pc->dstTileY);
    if (mo->flags_ & MoFlagEMPed) fputs(",\"emp\":1", f);
    fputc('}', f);
    return;
  }
  // Structures: status flags, the dock, labs' research, factories' bays and production.
  const uint32_t fl = mo->flags_;
  fprintf(f, ",\"flags\":%u", unsigned(fl));
  if (const Location d = u.GetDockLocation(); d) fprintf(f, ",\"dock\":[%d,%d]", d.x - PadTiles(), d.y);
  if (const int w = mo->weapon_; u.GetType() == MapID::GuardPost && w >= int(MapID::AcidCloud) && w <= int(MapID::BFG))
    fprintf(f, ",\"weapon\":\"%s\"", TypeName(MapID(w)));
  if (mo->command_ == int(CommandType::Research)) {
    // ECONOMY.md section 7: tech number +0x60, team +0x64, work left in actionTimer (cost << 8 at the start).
    const int num = *reinterpret_cast<const int*>(b + 0x60), team = *reinterpret_cast<const int*>(b + 0x64);
    if (num >= 0 && num < g_research.numTechs_) {
      const TechInfo* t = g_research.ppTechInfos_[num];
      const int cost = (fl & 0x1000) ? t->edenCost : t->plymouthCost;
      fprintf(f, ",\"research\":{\"tech\":%d,\"team\":%d,\"left\":%d,\"cost\":%d}", t->techID, team, mo->actionTimer_, cost << 8);
    }
  }
  if (fl & MoFlagBldFactory) {
    const auto* fb = static_cast<const FactoryBuilding*>(mo);
    fputs(",\"bays\":[", f);
    for (int i = 0; i < 6; ++i) {
      const int c = fb->cargoBayContents_[i], w = static_cast<const Building*>(mo)->cargoBayCargoOrWeapon_[i];
      if (i) fputc(',', f);
      if (c > 0) {
        fprintf(f, "\"%s", TypeName(MapID(c)));
        if (w > 0) fprintf(f, " %s", TypeName(MapID(w)));
        fputc('"', f);
      } else fputs("null", f);
    }
    fputc(']', f);
    if (mo->command_ == int(CommandType::Produce) && fb->itemToProduce_ > 0)
      fprintf(f, ",\"producing\":\"%s\",\"timer\":%d", TypeName(MapID(fb->itemToProduce_)), mo->actionTimer_);
  }
  fputc('}', f);
}

void WriteSessionState(int tick) {
  if (!g_stateOn) return;
  FILE* f = fopen((cfg.out + "\\state.json").c_str(), "w");
  if (!f) return;
  auto& game = g_gameImpl;
  const int w = GameMap::GetWidth(), h = GameMap::GetHeight(), pad = PadTiles();
  fprintf(f, "{\"tick\":%d,\"end\":%d,\"stop\":", tick, int(game.gameTermReasons_));
  JsonStr(f, g_stopReason.c_str());
  fprintf(f, ",\"map\":{\"width\":%d,\"height\":%d,\"pad\":%d},\n\"players\":[", w, h, pad);
  for (int i = 0; i < game.numPlayers_; ++i) {
    PlayerImpl* p = game.GetPlayer(i);
    const auto& m = p->moraleState_;
    fprintf(f, "%s{\"id\":%d,\"human\":%d,\"eden\":%d,\"food\":%d,\"commonOre\":%d,\"rareOre\":%d,\"workers\":%d,"
               "\"scientists\":%d,\"kids\":%d,\"morale\":%d,\"moraleLevel\":%d,\"powerGen\":%d,\"powerUsed\":%d,"
               "\"foodNet\":%d,\"foodProd\":%d,\"foodCons\":%d,\"maxCommon\":%d,\"maxRare\":%d,\"maxFood\":%d,"
               "\"workersReq\":%d,\"scientistsReq\":%d,\"availWorkers\":%d,\"availScientists\":%d,\"sciResearching\":%d,"
               "\"sciAsWorkers\":%d,\"buildings\":%d,\"idleBuildings\":%d,\"unpowered\":%d}",
            i ? ",\n" : "", i, p->isHuman_ != 0, p->isEden_ != 0, p->foodStored_, p->commonOre_, p->rareOre_,
            p->numWorkers_, p->numScientists_, p->numKids_, m.morale, int(m.moraleLevel), p->amountPowerGenerated_,
            p->amountPowerConsumed_, p->netFoodProduction_, p->totalFoodProduction_, p->totalFoodConsumption_,
            p->maxCommonOre_, p->maxRareOre_, p->maxFood_, p->numWorkersRequired_, p->numScientistsRequired_,
            p->numAvailableWorkers_, p->numAvailableScientists_, p->numScientistsResearching_, p->numScientistsAsWorkers_,
            p->numBuildings_, p->numIdleBuildings_, p->numUnpoweredStructures_);
  }
  // The tech tree: id, name, lab (1 basic, 2 standard, 3 advanced), Eden and Plymouth cost, max scientists, the
  // topics it requires (ids) and which players have it (bitmask).
  fputs("],\n\"techs\":[", f);
  for (int t = 0; t < g_research.numTechs_; ++t) {
    const TechInfo* ti = g_research.ppTechInfos_[t];
    fprintf(f, "%s{\"id\":%d,\"name\":", t ? ",\n" : "", ti->techID);
    JsonStr(f, ti->pTechName);
    uint32_t has;
    memcpy(&has, &ti->playerHasTechMask, sizeof(has));
    fprintf(f, ",\"lab\":%d,\"eden\":%d,\"ply\":%d,\"max\":%d,\"has\":%u,\"req\":[", int(ti->lab), ti->edenCost,
            ti->plymouthCost, ti->maxScientists, has & 0x3F);
    for (int r = 0; r < ti->numRequiredTechs && ti->pRequiredTechNum; ++r) {
      const int n = ti->pRequiredTechNum[r];
      fprintf(f, "%s%d", r ? "," : "", n >= 0 && n < g_research.numTechs_ ? g_research.ppTechInfos_[n]->techID : -1);
    }
    fputs("]}", f);
  }
  // Per vehicle, structure and weapon type, the id of the research topic that makes it available (MapObjectType +0x1E4, as
  // the eco run's typeExtra, a tech number; 0 when it isn't one): what the factory and build panes offer.
  fputs("],\n\"typeTopics\":{", f);
  bool firstType = true;
  for (int i = 1; i <= int(MapID::BFG); ++i) {
    const uint8_t* t = reinterpret_cast<const uint8_t*>(MapObjectType::GetInstance(i));
    if (!t) continue;
    const int num = *reinterpret_cast<const int*>(t + 0x1E4);  // A tech number: the index in the loaded tree.
    fprintf(f, "%s\"%s\":%d", firstType ? "" : ",", TypeName(MapID(i)),
            num >= 0 && num < g_research.numTechs_ ? g_research.ppTechInfos_[num]->techID : 0);
    firstType = false;
  }
  fputs("}", f);
  if (!g_names.empty()) {  // name -> id of the units `as NAME` and `name` bound (names are [A-Za-z0-9_], so no escaping)
    fputs(",\n\"names\":{", f);
    bool firstName = true;
    for (const auto& [name, b] : g_names) {
      fprintf(f, "%s\"%s\":%d", firstName ? "" : ",", name.c_str(), b.id);
      firstName = false;
    }
    fputc('}', f);
  }
  fputs(",\n\"objectives\":[", f);
  bool first = true;
  ForEachObjective([&](int, const VictoryConditionImpl* v) {
    fprintf(f, "%s{\"met\":%d,\"text\":", first ? "" : ",", ObjectiveMet(v));
    JsonStr(f, v->pObjectiveText_);
    fputc('}', f);
    first = false;
  });
  fputs("],\n\"units\":[\n", f);
  first = true;
  ForEachPlayerUnit([&](Unit u) { WriteStateUnit(f, first, u); });
  // Gaia: beacons (ore 0 common / 1 rare; bar 0 = 3-bar, 1 = 2-bar, 2 = 1-bar; surveyed by player 0), vents,
  // fumaroles, wreckage (its tech) and disasters.
  fputs("],\n\"gaia\":[", f);
  first = true;
  const int last = HighestObjectIndex();
  for (int i = 1; i <= last; ++i) {
    const MapObject* mo = MapObject::GetInstance(i);
    if (!mo || !mo->IsLive()) continue;
    const MapID t = mo->GetTypeID();
    if (!(t == MapID::MiningBeacon || t == MapID::MagmaVent || t == MapID::Fumarole || t == MapID::Wreckage || IsDisaster(t))) continue;
    fprintf(f, "%s{\"id\":%d,\"type\":\"%s\",\"x\":%d,\"y\":%d", first ? "" : ",\n", i, TypeName(t), mo->pixelX_ / 32 - pad,
            mo->pixelY_ / 32);
    first = false;
    if (t == MapID::MiningBeacon) {
      const auto* mb = static_cast<const MapObj::MiningBeacon*>(mo);
      fprintf(f, ",\"ore\":%d,\"bar\":%d,\"variant\":%d,\"surveyed\":%d", int(mb->mineType_), int(mb->mineYield_),
              mb->mineVariant_, int(mb->playerSurveyedMask_.Get(0)));
    } else if (t == MapID::Wreckage) {
      const auto* wr = static_cast<const MapObj::Wreckage*>(mo);
      uint32_t found;
      memcpy(&found, &wr->playerDiscoveredMask_, sizeof(found));
      fprintf(f, ",\"tech\":%d,\"discovered\":%d", wr->techID_, int(found & 1));
    } else if (IsDisaster(t)) {
      fprintf(f, ",\"timer\":%d,\"act\":\"%s\",\"flags\":%u", mo->actionTimer_, ActionName(int(mo->action_)), unsigned(mo->flags_));
    }
    fputc('}', f);
  }
  // Cells, file coordinates: type | lava << 5 | microbe << 6 | wall/building << 7 | lava possible << 8 | expand << 9.
  fputs("],\n\"cells\":[", f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const TileData t = GameMap::GetImpl()->Tile(x + pad, y);
      fprintf(f, "%s%u", (x | y) ? "," : "", t.cellType | (t.lava << 5) | (t.microbe << 6) | (t.wallOrBuilding << 7) | (t.lavaPossible << 8) | (t.expand << 9));
    }
  fputs("]}\n", f);
  fclose(f);
}

// ---------------------------------------------------------------------------------------------------------------------

GameStartInfo g_gsi;
constexpr int DrawLead = 4;  // Ticks rendered ahead of a capture (and at startup) so all panes are current.
int  g_lastTick = -1, g_nextShot = DrawLead + 1, g_nextTelem = 0, g_nextTrace = 0;
bool g_started = false;
int64_t g_startTime = 0;
int64_t Now()       { LARGE_INTEGER t;  QueryPerformanceCounter(&t);    return t.QuadPart; }
int64_t Frequency() { LARGE_INTEGER f;  QueryPerformanceFrequency(&f);  return f.QuadPart; }

// Pane presents: the DIB surface's +0x28 method (0x4A9410, vtable slot 0x4D74E0) ends in GDI BitBlt or StretchDIBits.
// Identify GameFrame's detail, minimap and command surfaces; other users of the shared surface implementation don't count.
struct PresentCounts { uint32_t detail = 0, minimap = 0, command = 0; };
using PresentFn = void(__thiscall*)(void*, const RECT*);
PresentFn g_origPresent = nullptr;
FILE* g_present = nullptr;
CRITICAL_SECTION g_presentLock;
PresentCounts g_presentTickCounts, g_presentSecondCounts;
int g_presentTick = -1, g_presentSecondTick = 0;
int64_t g_presentSecondStart = 0, g_presentNextSecond = 0;

// The *Locked writers re-check g_present: the control thread's quit can close the log between a game-thread caller's
// unlocked check and its EnterCriticalSection.
void WritePresentTickLocked() {
  if (!g_present || g_presentTick < 0) return;
  fprintf(g_present, "{\"kind\":\"tick\",\"tick\":%d,\"detail\":%u,\"minimap\":%u,\"command\":%u}\n",
          g_presentTick, g_presentTickCounts.detail, g_presentTickCounts.minimap, g_presentTickCounts.command);
}

void SyncPresentTickLocked(int tick) {
  if (tick == g_presentTick) return;
  WritePresentTickLocked();
  g_presentTick = tick;
  g_presentTickCounts = {};
}

void FlushPresentSecondLocked(int64_t now, bool final) {
  if (!g_present || (!final && now < g_presentNextSecond)) return;
  const int tick = g_gameImpl.tick_;
  if (tick < g_presentSecondTick) g_presentSecondTick = 0;  // a new shell game restarted the tick count
  fprintf(g_present,
          "{\"kind\":\"second\",\"ms\":%.1f,\"ticks\":%d,\"tick\":%d,\"detail\":%u,\"minimap\":%u,\"command\":%u}\n",
          (now - g_presentSecondStart) * 1000.0 / Frequency(), tick - g_presentSecondTick, tick,
          g_presentSecondCounts.detail, g_presentSecondCounts.minimap, g_presentSecondCounts.command);
  fflush(g_present);
  g_presentSecondCounts = {};
  g_presentSecondStart = now;
  g_presentNextSecond = now + Frequency();
  g_presentSecondTick = tick;
}

void PresentTick(int tick) {
  if (!g_present) return;
  EnterCriticalSection(&g_presentLock);
  SyncPresentTickLocked(tick);
  FlushPresentSecondLocked(Now(), false);
  LeaveCriticalSection(&g_presentLock);
}

void PresentClock() {
  if (!g_present) return;
  EnterCriticalSection(&g_presentLock);
  FlushPresentSecondLocked(Now(), false);
  LeaveCriticalSection(&g_presentLock);
}

void __fastcall PresentHook(void* self, void*, const RECT* rect) {
  uint32_t PresentCounts::* field = nullptr;
  if (self == g_gameFrame.detailPane_.pGfxSurface_) field = &PresentCounts::detail;
  else if (self == g_gameFrame.miniMapPane_.pGfxSurface_) field = &PresentCounts::minimap;
  else if (self == g_gameFrame.commandPane_.pGfxSurface_) field = &PresentCounts::command;
  if (g_present && field) {
    EnterCriticalSection(&g_presentLock);
    SyncPresentTickLocked(g_gameImpl.tick_);
    ++(g_presentTickCounts.*field);
    ++(g_presentSecondCounts.*field);
    LeaveCriticalSection(&g_presentLock);
  }
  g_origPresent(self, rect);
}

// --rng-log FROM-TO: every draw from the gameplay RNG (Random::Rand 0x46F000 and Rand(n) 0x46F060 on 0x56BE20) in
// those ticks, with its caller, to rng.log: `tick caller n` (n is - for a plain Rand()). For finding which code drew
// where op2-sim's RNG first differs.
using RandFn  = uint32_t(__thiscall*)(void*);
using RandNFn = uint32_t(__thiscall*)(void*, int);
RandFn  g_rand;
RandNFn g_randN;
FILE* g_rngLog = nullptr;
int g_rngFrom = 0, g_rngTo = -1;

bool RngLogged(const void* self) {
  return g_rngLog && self == &g_gameRNG && g_gameImpl.tick_ >= g_rngFrom && g_gameImpl.tick_ <= g_rngTo;
}

uint32_t __fastcall RandHook(void* self, void*) {
  if (RngLogged(self)) { fprintf(g_rngLog, "%d %p -\n", g_gameImpl.tick_, __builtin_return_address(0));  fflush(g_rngLog); }
  return g_rand(self);
}

uint32_t __fastcall RandNHook(void* self, void*, int n) {
  if (RngLogged(self)) { fprintf(g_rngLog, "%d %p %d\n", g_gameImpl.tick_, __builtin_return_address(0), n);  fflush(g_rngLog); }
  return g_randN(self, n);
}

void InstallRngLog() {
  const std::string spec = Env("OP2H_RNG_LOG");
  if (spec.empty()) return;
  // FROM-TO, or a single tick.
  char rest = 0;
  const int n = sscanf(spec.c_str(), "%d-%d%c", &g_rngFrom, &g_rngTo, &rest);
  if (n == 1 && spec.find('-') == std::string::npos) g_rngTo = g_rngFrom;
  if ((n != 2 && !(n == 1 && g_rngTo == g_rngFrom)) || g_rngFrom < 0 || g_rngTo < g_rngFrom) {
    Log("rng: bad --rng-log range \"%s\" (want FROM-TO with FROM <= TO, or one tick); no log", spec.c_str());
    return;
  }
  g_rngLog = fopen((cfg.out + "\\rng.log").c_str(), "w");
  if (!g_rngLog) { Log("rng: cannot open log");  return; }
  static const uint8_t rand0[] = { 0x8B, 0x01, 0x83, 0xEC, 0x08 };
  static const uint8_t randN[] = { 0x8B, 0x01, 0x83, 0xEC, 0x0C };
  g_rand  = reinterpret_cast<RandFn>(HookEntry(0x46F000, rand0, 5, reinterpret_cast<void*>(&RandHook)));
  g_randN = reinterpret_cast<RandNFn>(HookEntry(0x46F060, randN, 5, reinterpret_cast<void*>(&RandNHook)));
  Log("rng: log ticks %d-%d, hooks %s", g_rngFrom, g_rngTo, g_rand && g_randN ? "installed" : "FAILED");
}

void InstallPresentHook() {
  if (!cfg.presents) return;
  g_present = fopen((cfg.out + "\\present.log").c_str(), "w");
  if (!g_present) { Log("present: cannot open log");  return; }
  InitializeCriticalSection(&g_presentLock);
  g_presentSecondStart = g_presentNextSecond = Now();
  g_presentNextSecond += Frequency();
  g_presentSecondTick = g_gameImpl.tick_;
  static const uint8_t present[] = { 0x83, 0xEC, 0x08, 0x53, 0x56, 0x57 };
  g_origPresent = reinterpret_cast<PresentFn>(HookEntry(0x4A9410, present, 6, reinterpret_cast<void*>(&PresentHook)));
  Log("present: hook %s", g_origPresent ? "installed" : "FAILED");
  if (g_origPresent) return;
  fclose(g_present);
  g_present = nullptr;
  DeleteCriticalSection(&g_presentLock);
}

void ClosePresents() {
  if (!g_present) return;
  EnterCriticalSection(&g_presentLock);
  WritePresentTickLocked();
  FlushPresentSecondLocked(Now(), true);
  fclose(g_present);
  g_present = nullptr;
  LeaveCriticalSection(&g_presentLock);
}

void TakeShot(int tick) {
  char path[MAX_PATH * 2];
  snprintf(path, sizeof(path), "%s\\shot_%07d.bmp", cfg.out.c_str(), tick);
  Screenshot(path);
}

void Quit(const char* why) {
  const double secs = (Now() - g_startTime) * 1.0 / Frequency();
  Log("quit at tick %d: %s (%.2fs in game, %.0f ticks/s)", g_gameImpl.tick_, why, secs, g_gameImpl.tick_ / (secs > 0 ? secs : 1));
  WriteTelemetry(g_gameImpl.tick_);
  WriteTrace(g_gameImpl.tick_);
  if (cfg.shotEvery > 0) TakeShot(g_gameImpl.tick_);
  WriteSessionState(g_gameImpl.tick_);
  if (g_record) {
    g_recEnd = "# end of the recording: tick " + std::to_string(g_gameImpl.tick_) + ", " + why;
    WriteRecording();
  }
  if (!g_packetDue.empty()) {
    std::string ticks;
    for (const auto& [t, p] : g_packetDue) ticks += " " + std::to_string(t);
    Log("script: WARN %u `packet` lines never ran (their cycles didn't hand the local player's slot to ProcessCommandPacket):%s",
        unsigned(g_packetDue.size()), ticks.c_str());
  }
  ClosePresents();
  if (g_events) fclose(g_events);
  if (g_telem) fclose(g_telem);
  if (g_trace) fclose(g_trace);
  if (g_audio) fclose(g_audio);
  if (g_log) fclose(g_log);
  // Skip normal teardown: it would flush window/ini state and can block on modal dialogs.
  TerminateProcess(GetCurrentProcess(), 0);
}

void ParkCursor() {
  HWND hWnd = g_gameFrame.hWnd_;
  if (!hWnd) return;
  RECT win, client;
  GetWindowRect(hWnd, &win);
  GetClientRect(hWnd, &client);
  Log("window %ldx%ld at %ld,%ld, client %ldx%ld", win.right - win.left, win.bottom - win.top, win.left, win.top,
      client.right, client.bottom);
  // The game polls the cursor through the virtual GetCursorPos, which starts over the empty part of the side pane:
  // outside the window or near an edge it would scroll the view (the game edge-scrolls on any cursor beyond the detail
  // pane), over the map it would show tooltips. The real X pointer goes to the screen's corner, outside the window, so
  // Wine sends the game no mouse messages of its own (over a pane they would show button tooltips, for example).
  g_in.pos = { client.right - 90, client.bottom / 2 };
  g_in.active = true;
  if (!cfg.desktop) SetCursorPos(GetSystemMetrics(SM_CXSCREEN) - 1, GetSystemMetrics(SM_CYSCREEN) - 1);
  // Without a window manager the window is sometimes not yet active here (an X focus race). An inactive game ignores
  // view changes until it is activated, so make it the active, focused window now.
  const bool wasActive = g_tApp.AppActive() && GetFocus() == hWnd;
  if (!wasActive && !cfg.desktop) {
    SetForegroundWindow(hWnd);
    SetActiveWindow(hWnd);
    SetFocus(hWnd);
  }
  if (cfg.desktop) Log("desktop: pointer and focus left alone; game active %d", int(g_tApp.AppActive()));
  if (g_inputUsed) {
    Log("input: frame %p %s, now active %d paused %d focus %p, class style %lx", (void*)hWnd,
        wasActive ? "was active" : "activated", g_tApp.AppActive(), g_tApp.AppPaused(), (void*)GetFocus(),
        GetClassLongA(g_gameFrame.detailPane_.hWnd_, GCL_STYLE));
    for (const IWnd* p : { (const IWnd*)&g_gameFrame.detailPane_, (const IWnd*)&g_gameFrame.miniMapPane_,
                           (const IWnd*)&g_gameFrame.commandPane_ }) {
      RECT rc;
      GetClientRect(p->hWnd_, &rc);
      const POINT o = PaneOrigin(*p);
      Log("input: %s pane %p at (%ld,%ld) size %ldx%ld", PaneName(p), (void*)p->hWnd_, o.x, o.y, rc.right, rc.bottom);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Recording a player's own game (OP2H_RECORD, `run.sh --record`, README "Recording your own play") and replaying it
// (the `packet` verb).
//
// The UI never runs an order itself. Each command handler asks for the local player's pending slot (0x490810: slot
// (((tick + interval - 1) >> log2 interval) + delay) & 15 of the 16 113-byte packets at PlayerImpl +0x4C8; it stamps
// +6 = 0x80000000 and +0xA = 0) and writes its packet there, over whatever the slot held. GameFrame::OnIdle also writes
// a ctMachineSettings packet there itself (0x49CC30, from 0x49C55C) after each cycle whose interval number is 7 mod 8.
// On ticks that are a multiple of the interval (4), GameImpl::ProcessGameCycle (0x489EA0) hands every player's slot
// (tick >> log2 interval) & 15 to PlayerImpl::ProcessCommandPacket (0x40E300) at 0x489FF5, before the units act
// (0x43AD30), and at the end of the cycle resets the local player's newest slot to ctNop (0x48A035). So the packets
// that reach 0x489FF5 are the player's orders that took effect, after any overwrite, at the tick they took effect: the
// recorder logs the local player's there.
//
// The replay can't write the slot the way the UI did: the script runs before OnIdle's ctMachineSettings write, which
// would overwrite it on some ticks. `T packet TYPE HEX` instead puts the packet into the slot as ProcessCommandPacket is
// about to read it, in the cycle that starts at tick T, so the same bytes run at the same point of the same cycle.
// ctNop and ctMachineSettings (the machine's speed, memory and window size, read only for a debug line, 0x467CB0) are
// not recorded; the replay's game sends its own.
//
// Saves and loads: TApp::SaveGame (0x4877E0, called at 0x487269; GameImpl::SaveGame's result at 0x48786B) and
// TApp::LoadGame (0x4878F0, called at 0x4865F2, 0x487394, 0x48754D, 0x488F55) are hooked. A save writes a sidecar
// folder `<save file>.rec` with the recording so far (packets.txt), the telemetry so far and meta.txt (tick, the
// session's identity); a load puts them back, so the recording is the one history that led to the loaded game. Every
// packet the save's history ran had run before the save's tick, and the slots pending at that tick are in the save
// (they run again after the load, and are recorded then).

constexpr uintptr_t CallSlotPacket       = 0x489FF5;  // GameImpl::ProcessGameCycle: PlayerImpl::ProcessCommandPacket(slot)
constexpr uintptr_t CallTAppSaveGame     = 0x487269;  // TApp::SaveGame(name, dialog) (0x4877E0)
constexpr uintptr_t CallGameImplSaveGame = 0x48786B;  // in TApp::SaveGame: GameImpl::SaveGame(stream, dialog), 0 = failed
constexpr uintptr_t CallsTAppLoadGame[]  = { 0x4865F2, 0x487394, 0x48754D, 0x488F55 };  // TApp::LoadGame(name) (0x4878F0)

using SlotPacketFn = ibool(__thiscall*)(PlayerImpl*, CommandPacket*);
using TAppSaveFn   = void(__thiscall*)(void*, const char*, void*);
using GameSaveFn   = ibool(__thiscall*)(GameImpl*, void*, void*);
using TAppLoadFn   = int(__thiscall*)(void*, const char*);
SlotPacketFn g_origSlotPacket = nullptr;
TAppSaveFn   g_origTAppSave   = nullptr;
GameSaveFn   g_origGameSave   = nullptr;
TAppLoadFn   g_origTAppLoad   = nullptr;
std::vector<std::string> g_recLines;    // its packet lines (the history that led here)
std::vector<std::string> g_recLoads;    // header lines: each load and what it rolled back
std::string g_recSetup;                 // the setup script (--script), copied to the top of the recording
std::string g_recBroken;                // why the recording no longer replays, if it doesn't
int g_saveResult = -1;

std::string HexStr(const uint8_t* p, size_t n) {
  static const char digits[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) { s += digits[p[i] >> 4];  s += digits[p[i] & 15]; }
  return s;
}

// A unit as a recording's comments name it: `id(TYPE@x,y)` in file tiles, as it is when its order runs.
std::string UnitWord(int id) {
  const Unit u(id);
  if (!Exists(u)) return std::to_string(id);
  const Location l = u.GetLocation();
  char b[96];
  snprintf(b, sizeof b, "%d(%s@%d,%d)", id, TypeName(u.GetType()), l.x - PadTiles(), l.y);
  return b;
}

// What a recorded packet does, for the comment after its line: `= verb ...` when the harness verb builds the same bytes
// (one unit, tile centres: MovePacket, WaypointPacket, TargetedPacket), else `~ verb ...`, the nearest verb or a
// description (several units, a pixel that isn't a tile's centre, a packet no verb sends). Tiles are file tiles.
std::string DescribePacket(const CommandPacket& p) {
  const uint8_t* d = p.data.buffer;
  const size_t n = std::min<size_t>(p.dataLength, CommandPacketDataSize);
  size_t i = 0;
  bool ok = true;
  auto u8  = [&]() -> int { if (i + 1 > n) { ok = false;  return 0; }  return d[i++]; };
  auto u16 = [&]() -> int { if (i + 2 > n) { ok = false;  return 0; }  const int v = d[i] | d[i + 1] << 8;  i += 2;  return v; };
  auto u32 = [&]() -> uint32_t { const uint32_t lo = u16();  return lo | uint32_t(u16()) << 16; };
  std::vector<int> ids;
  auto units = [&]() { const int k = int8_t(u8());  if (k <= 0) ok = false;  for (int j = 0; j < k && ok; ++j) ids.push_back(u16()); };
  auto unitList = [&]() { std::string s;  for (int id : ids) s += (s.empty() ? "" : " ") + UnitWord(id);  return s; };
  auto same = [&](const CommandPacket& q) {
    return q.type == p.type && q.dataLength == p.dataLength && memcmp(q.data.buffer, d, n) == 0;
  };
  const int pad = PadTiles();
  auto tile = [&](int x, int y) { return std::to_string(x - pad) + " " + std::to_string(y); };
  auto item = [](int id) { return std::string(TypeName(MapID(id))); };
  const CommandType type = p.type;
  const std::string name = CommandName(int(type));
  std::string out;
  switch (type) {
  case CommandType::Move: case CommandType::Dock: case CommandType::DockEG: case CommandType::StandGround:
  case CommandType::Patrol: {
    units();
    const int nw = u16();
    std::vector<Location> locs;
    std::vector<int> ranges;
    std::string words;
    bool centred = true;
    for (int k = 0; k < nw && ok; ++k) {
      const uint32_t w = u32();
      const int px = w & 0x7FFF, py = (w >> 15) & 0x3FFF, r = int(w >> 29);
      centred &= px % 32 == 16 && py % 32 == 16;
      locs.push_back(Location(px / 32, py / 32));
      ranges.push_back(r);
      words += " " + std::to_string(px / 32 - pad) + "," + std::to_string(py / 32) + (r ? "," + std::to_string(r) : "");
    }
    const int loop = type == CommandType::Patrol ? u16() : -1;
    if (!ok || nw < 1 || nw > 8) break;
    const bool simple = nw == 1 && ranges[0] == 0 && type != CommandType::StandGround && type != CommandType::Patrol;
    const char* verb = type == CommandType::Dock ? "dock" : type == CommandType::DockEG ? "dockeg"
                     : type == CommandType::StandGround ? "standground" : type == CommandType::Patrol ? "patrol"
                     : simple ? "move" : "movewp";
    std::string args = simple ? " " + tile(locs[0].x, locs[0].y) : (loop >= 0 ? " " + std::to_string(loop) : "") + words;
    if (ids.size() == 1) {
      const CommandPacket q = simple ? MovePacket(ids[0], locs[0], type) : WaypointPacket(ids[0], type, locs, ranges, loop);
      if (same(q)) return "= " + std::string(verb) + " " + std::to_string(ids[0]) + args + "  " + UnitWord(ids[0]);
    }
    return "~ " + std::string(verb) + " " + unitList() + args + (centred ? "" : " (not tile centres)");
  }
  case CommandType::Attack: case CommandType::Guard: {
    units();
    u16();
    const int x = u16(), y = u16();
    if (!ok) break;
    const bool onTile = y != 0xFFFF;
    const std::string verb = onTile ? "attackground" : type == CommandType::Guard ? "guard" : "attack";
    const std::string args = onTile ? tile(x, y) : UnitWord(x);
    if (ids.size() == 1 && same(TargetedPacket(ids[0], type, x, y, onTile)))
      return "= " + verb + " " + std::to_string(ids[0]) + " " + (onTile ? tile(x, y) : std::to_string(x)) + "  " +
             UnitWord(ids[0]) + (onTile ? "" : " -> " + args);
    return "~ " + verb + " " + unitList() + " -> " + args;
  }
  case CommandType::Stop: case CommandType::SelfDestruct: case CommandType::Scatter: {
    units();
    if (!ok) break;
    return "~ " + std::string(type == CommandType::Stop ? "stop" : type == CommandType::Scatter ? "scatter" : "selfdestruct") +
           " " + unitList();
  }
  case CommandType::LightToggle: {
    units();
    const int on = u16();
    if (!ok) break;
    return "~ lights " + unitList() + " " + std::to_string(on);
  }
  case CommandType::Doze: case CommandType::BuildWall: case CommandType::Build: case CommandType::RemoveWall: {
    units();
    if (type == CommandType::Build || type == CommandType::RemoveWall) { const int nw = u16();  for (int k = 0; k < nw && ok; ++k) u32(); }
    const int x1 = u16(), y1 = u16(), x2 = u16(), y2 = u16();
    const int wall = type == CommandType::BuildWall ? u16() : 0;
    if (!ok || ids.empty()) break;
    if (type == CommandType::Doze) return "~ doze " + unitList() + " " + tile(x1, y1) + " " + tile(x2, y2);
    if (type == CommandType::RemoveWall) return "~ removewall " + unitList() + " " + tile(x1, y1);
    if (type == CommandType::BuildWall)  // the packet's bottom-right is exclusive, the verb's inclusive
      return "~ buildwall " + unitList() + " " + tile(x1, y1) + " " + tile(x2 - 1, y2 - 1) + " " + item(wall);
    const Unit cv(ids[0]);
    return "~ build " + unitList() + " " + tile(x2, y2) + (Exists(cv) ? " (kit " + item(int(cv.GetMapObject()->cargo_)) + ")" : "");
  }
  case CommandType::CargoRoute: {
    units();
    const int nw = u16();
    for (int k = 0; k < nw && ok; ++k) u32();
    u16(), u16();
    const int mine = u16(), smelter = u16();
    if (!ok) break;
    return "~ route " + unitList() + " " + UnitWord(mine) + " " + UnitWord(smelter);
  }
  case CommandType::RepairObj: case CommandType::Reprogram: case CommandType::Dismantle: {
    units();
    u16();
    const int target = u16();
    if (!ok || ids.empty()) break;
    const std::string verb = type == CommandType::RepairObj ? "repairobj" : type == CommandType::Reprogram ? "reprogram" : "dismantle";
    std::string more;
    for (size_t k = 1; k < ids.size(); ++k) more += " " + UnitWord(ids[k]);
    return "~ " + verb + " " + UnitWord(ids[0]) + " " + (target ? UnitWord(target) : "none") + more;
  }
  case CommandType::Produce: {
    const int f = u16(), what = u16(), weapon = u16();
    if (!ok) break;
    return "~ produce " + UnitWord(f) + " " + item(what) + (weapon && weapon != 0xFFFF ? " " + item(weapon) : "");
  }
  case CommandType::Research: {
    const int lab = u16(), num = u16(), sci = u16();
    if (!ok) break;
    const int id = num < g_research.numTechs_ ? g_research.ppTechInfos_[num]->techID : -1;
    return "~ research " + UnitWord(lab) + " " + std::to_string(id) + " " + std::to_string(sci);
  }
  case CommandType::TrainScientists: { const int u = u16(), k = u16();  if (!ok) break;  return "~ train " + UnitWord(u) + " " + std::to_string(k); }
  case CommandType::TransferCargo: {
    const int u = u16(), bay = u16(), second = u16();
    if (!ok) break;
    return "~ transfercargo " + UnitWord(u) + " " + std::to_string(bay) + (second ? " " + std::to_string(second) : "");
  }
  case CommandType::Launch: {
    const int u = u16(), x = u16(), y = u16();
    if (!ok) break;
    return "~ launch " + UnitWord(u) + (x || y ? " " + std::to_string(x) + " " + std::to_string(y) : "");
  }
  case CommandType::LoadCargo: case CommandType::UnloadCargo: case CommandType::Idle: case CommandType::Unidle: {
    const int u = u16();
    if (!ok) break;
    const char* verb = type == CommandType::LoadCargo ? "load" : type == CommandType::UnloadCargo ? "unload"
                     : type == CommandType::Idle ? "idle" : "unidle";
    return "~ " + std::string(verb) + " " + UnitWord(u);
  }
  case CommandType::GameOpt: {
    u16();
    const int var = u16(), value = u16(), player = u16();
    if (!ok) break;
    char b[96];
    snprintf(b, sizeof b, "GameOpt %d = %d, player %d%s", var, value, player, var == 0x14 ? " (game speed x4)" : "");
    return b;
  }
  case CommandType::Chat: return "Chat \"" + std::string(reinterpret_cast<const char*>(d) + 2, strnlen(reinterpret_cast<const char*>(d) + 2, n > 2 ? n - 2 : 0)) + "\"";
  default: break;
  }
  if (!ids.empty()) return name + " " + unitList();
  return name;
}

// The recording file: header, setup script, then one line per packet. Rewritten whole on a load; appended to otherwise.
std::string UnixPath(std::string p) {
  if (p.size() > 2 && (p[0] == 'Z' || p[0] == 'z') && p[1] == ':') p = p.substr(2);
  for (auto& ch : p) if (ch == '\\') ch = '/';
  return p;
}

// Rewritten whole after every change, to a temporary file renamed over it: a crash never leaves half a recording.
void WriteRecording() {
  const std::string path = cfg.out + "\\recording.txt", tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) { Log("record: WARN cannot write %s", tmp.c_str());  return; }
  fprintf(f, "# Recorded play of %s (./run.sh --record; README.md \"Recording your own play\").\n", cfg.mission.c_str());
  fprintf(f, "# seed %d, difficulty %d, campaign %s\n", cfg.seed, cfg.difficulty,
          cfg.campaign.empty() ? "none" : UnixPath(cfg.campaign).c_str());
  fprintf(f, "# Replay: ./run.sh --mission %s%s%s --seed %d --difficulty %d --script <this file> --ticks 0 --out <dir>\n",
          cfg.mission.c_str(), cfg.campaign.empty() ? "" : " --campaign ", cfg.campaign.empty() ? "" : UnixPath(cfg.campaign).c_str(),
          cfg.seed, cfg.difficulty);
  fputs("# `T packet TYPE HEX`: the command packet the player's UI sent, run in the cycle that starts at tick T. After it,\n"
        "# `= verb ...` is the harness verb that sends the same bytes (between cycles, not inside one), `~ verb ...` the\n"
        "# nearest verb or what the packet does. Units are `id(TYPE@x,y)` when the packet ran; tiles are file tiles (the\n"
        "# verbs need `0 coords file` before them).\n", f);
  for (const auto& l : g_recLoads) fprintf(f, "# %s\n", l.c_str());
  if (!g_recBroken.empty()) fprintf(f, "# BROKEN: %s\n", g_recBroken.c_str());
  if (!g_recSetup.empty()) {
    fputs("# --- setup script (--script) ---\n", f);
    // Its `savegame` and `loadgame` lines made the session's history; the replay is that history, without them.
    std::istringstream in(g_recSetup);
    for (std::string line; std::getline(in, line);) {
      std::istringstream words(line);
      std::string tick, verb;
      words >> tick >> verb;
      fprintf(f, "%s%s\n", verb == "savegame" || verb == "loadgame" ? "# (the recorded session only) " : "", line.c_str());
    }
    fputs("# --- recorded play ---\n", f);
  }
  for (const auto& l : g_recLines) fprintf(f, "%s\n", l.c_str());
  if (!g_recEnd.empty()) fprintf(f, "%s\n", g_recEnd.c_str());
  const bool ok = !ferror(f);
  if (fclose(f) != 0 || !ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    Log("record: WARN writing %s failed (error %lu)", path.c_str(), GetLastError());
}

// The bytes of the slot after dataLength up to its last nonzero byte: whatever earlier packets left there. Handlers read
// by their own counts, not by dataLength, so the recording keeps them and the replay puts them back.
size_t TailLength(const CommandPacket& p) {
  size_t end = CommandPacketDataSize;
  while (end > p.dataLength && p.data.buffer[end - 1] == 0) --end;
  return end > p.dataLength ? end - p.dataLength : 0;
}

void RecordPacket(int tick, const CommandPacket& p) {
  const size_t n = std::min<size_t>(p.dataLength, CommandPacketDataSize), tail = TailLength(p);
  std::string line = std::to_string(tick) + " packet " + std::to_string(int(p.type)) + " " + (n ? HexStr(p.data.buffer, n) : "-");
  if (tail) line += " +" + HexStr(p.data.buffer + n, tail);
  // Quitting is the session's end, not part of the game: kept as a comment.
  if (p.type == CommandType::Quit) line = "# " + line + "  (not replayed)";
  std::string what = DescribePacket(p);
  for (auto& ch : what) if (uint8_t(ch) < 0x20) ch = ' ';  // A chat line's newline mustn't end the comment.
  g_recLines.push_back(line + "  # " + what);
  WriteRecording();
}

ibool __fastcall SlotPacketHook(PlayerImpl* self, void*, CommandPacket* p) {
  if (self == g_gameImpl.GetPlayer(g_gameImpl.localPlayer_)) {
    const int tick = g_gameImpl.tick_;
    bool injected = false;
    if (auto it = g_packetDue.find(tick); it != g_packetDue.end()) {
      const RawPacket& r = it->second;
      p->type = CommandType(r.type);
      p->dataLength = uint16(r.data.size());
      memset(p->data.buffer, 0, CommandPacketDataSize);
      if (!r.data.empty()) memcpy(p->data.buffer, r.data.data(), r.data.size());
      if (!r.tail.empty()) memcpy(p->data.buffer + r.data.size(), r.tail.data(), r.tail.size());
      g_packetDue.erase(it);
      injected = true;
    }
    if (p->type != CommandType::Nop && p->type != CommandType::MachineSettings) {
      const size_t n = std::min<size_t>(p->dataLength, CommandPacketDataSize);
      Log("packet: tick %d %s %d %s%s", tick, CommandName(int(p->type)), int(p->type), n ? HexStr(p->data.buffer, n).c_str() : "-",
          injected ? " (a `packet` line)" : "");
      // A `packet` line of the setup script (a recording played on) is already in the recording, as part of the setup.
      if (g_record && !injected) RecordPacket(tick, *p);
    }
  }
  return g_origSlotPacket(self, p);
}

bool ParseHex(const std::string& hex, std::vector<uint8_t>* out) {
  if (hex.size() % 2) return false;
  for (size_t k = 0; k < hex.size(); k += 2) {
    if (!isxdigit(uint8_t(hex[k])) || !isxdigit(uint8_t(hex[k + 1]))) return false;
    out->push_back(uint8_t(strtol(hex.substr(k, 2).c_str(), nullptr, 16)));
  }
  return true;
}

// `T packet TYPE HEX|- [+TAIL]`: the local player's packet for the cycle starting at tick T (a multiple of the
// interval), with the slot's bytes after the data (TAIL; the rest of the slot's 99 data bytes are zero).
void QueuePacket(const ScriptCmd& c, int tick) {
  auto warn = [&](const char* why) { Log("script: WARN tick %d line %d: `packet`: %s (skipped)", c.tick, c.line, why); };
  if (!g_origSlotPacket) return warn("the packet hook isn't installed (shell mode can't replay packets)");
  // In a network game the slot hook only logs: a packet put into the local slot as it runs would change this machine's
  // game alone. Orders go through the verbs, which queue them in the pending slot for every machine.
  if (g_net) return warn("a network game can't replay `packet` lines (they would run on this machine only); give orders with the verbs");
  if (c.args.size() < 2) return warn("needs a type and the data as hex (`-` for none)");
  RawPacket r;
  char* end = nullptr;
  r.type = int(strtol(c.args[0].c_str(), &end, 10));
  if (*end || r.type <= 0 || r.type >= int(CommandType::Count)) return warn("the type isn't a command type (1-53)");
  if (c.args[1] != "-" && !ParseHex(c.args[1], &r.data)) return warn("bad hex data");
  if (c.args.size() > 2 && (c.args[2][0] != '+' || !ParseHex(c.args[2].substr(1), &r.tail))) return warn("bad +tail hex");
  if (r.data.size() + r.tail.size() > CommandPacketDataSize) return warn("more than the slot's 99 data bytes");
  const int interval = g_gameImpl.commandPacketProcessingInterval_;
  if (interval <= 0 || c.tick % interval) return warn("not a command-processing tick (a multiple of 4)");
  if (tick > c.tick) return warn("its cycle has run already (the first script tick is 1)");
  if (g_packetDue.count(c.tick)) Log("script: WARN tick %d line %d: `packet`: a second packet for this cycle replaces the first", c.tick, c.line);
  g_packetDue[c.tick] = r;
}

void InstallPacketHook() {
  if (g_origSlotPacket) return;
  g_origSlotPacket = reinterpret_cast<SlotPacketFn>(RelinkCall(CallSlotPacket, reinterpret_cast<void*>(&SlotPacketHook)));
  Log("packet hook: %s", g_origSlotPacket ? "installed" : "FAILED");
}

uint64_t Fnv(const std::string& s, uint64_t h = 1469598103934665603ull) {
  for (unsigned char ch : s) { h ^= ch;  h *= 1099511628211ull; }
  return h;
}

std::string ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

// What a save's recording must match to be put back: the same start (mission, seed, difficulty, carry-over, setup).
// The setup's `savegame`/`loadgame` lines aren't part of the history (the recording keeps them as comments).
std::string RecIdentity() {
  char b[32];
  std::string setup;
  std::istringstream in(g_recSetup);
  for (std::string line; std::getline(in, line);) {
    std::istringstream words(line);
    std::string tick, verb;
    words >> tick >> verb;
    if (verb != "savegame" && verb != "loadgame") setup += line + "\n";
  }
  const std::string all = cfg.mission + "|" + std::to_string(cfg.seed) + "|" + std::to_string(cfg.difficulty) + "|" +
                          (cfg.campaign.empty() ? "" : ReadFile(cfg.campaign)) + "|" + setup;
  snprintf(b, sizeof b, "%016llx", (unsigned long long)Fnv(all));
  return b;
}

std::string SidecarDir(const char* save) {
  char full[MAX_PATH * 2] = {};
  if (!GetFullPathNameA(save, sizeof full, full, nullptr)) strncpy(full, save, sizeof full - 1);
  return std::string(full) + ".rec";
}

// The save file itself, as its recording names it: its size and an FNV hash of its bytes. A slot saved over without
// the hook (a run that isn't a recording, the shell, plain play in prefix-harness) keeps an old sidecar; this tells.
std::string SaveFingerprint(const char* save) {
  const std::string bytes = ReadFile(save);
  char b[64];
  snprintf(b, sizeof b, "%u %016llx", unsigned(bytes.size()), (unsigned long long)Fnv(bytes));
  return b;
}

// Removes a save's sidecar (its files, then the folder), so a load of that save is a recording's BROKEN, not a
// wrong history.
void DeleteSidecar(const char* save) {
  const std::string dir = SidecarDir(save);
  for (const char* name : { "meta.txt", "packets.txt", "telemetry.jsonl", "trace.jsonl" }) DeleteFileA((dir + "\\" + name).c_str());
  if (RemoveDirectoryA(dir.c_str())) Log("record: removed the sidecar %s", dir.c_str());
}

void WriteSidecar(const char* save) {
  const std::string dir = SidecarDir(save);
  DeleteSidecar(save);
  bool ok = CreateDirectoryA(dir.c_str(), nullptr) != 0;
  if (FILE* f = ok ? fopen((dir + "\\packets.txt").c_str(), "w") : nullptr) {
    for (const auto& l : g_recLines) fprintf(f, "%s\n", l.c_str());
    ok = !ferror(f);
    ok = fclose(f) == 0 && ok;
  } else ok = false;
  for (FILE* f : { g_telem, g_trace }) if (f) fflush(f);
  for (const char* name : { "telemetry.jsonl", "trace.jsonl" }) {
    const std::string mine = cfg.out + "\\" + name;
    if (ok && GetFileAttributesA(mine.c_str()) != INVALID_FILE_ATTRIBUTES) ok = CopyFileA(mine.c_str(), (dir + "\\" + name).c_str(), FALSE) != 0;
  }
  // meta.txt last, and only when everything else is there: a sidecar without it counts as none.
  if (FILE* f = ok ? fopen((dir + "\\meta.txt").c_str(), "w") : nullptr) {
    fprintf(f, "tick %d\nidentity %s\nsave %s\nmission %s\nbroken %s\n", g_gameImpl.tick_, RecIdentity().c_str(),
            SaveFingerprint(save).c_str(), cfg.mission.c_str(), g_recBroken.c_str());
    ok = !ferror(f);
    ok = fclose(f) == 0 && ok;
  } else ok = false;
  if (!ok) {
    Log("record: WARN the sidecar of %s couldn't be written (error %lu): loading this save will mark the recording BROKEN",
        save, GetLastError());
    DeleteSidecar(save);
    return;
  }
  Log("record: saved %s at tick %d with %u packets -> %s", save, g_gameImpl.tick_, unsigned(g_recLines.size()), dir.c_str());
}

// The harness's own clocks after a load took the game back to `tick`: samples continue from there, and the script
// lines after it run again (they belong to the history after the save; `savegame` and `loadgame` lines run once only).
bool g_loaded = false;  // A load happened (RunSaveLoad checks it).
void AfterLoad(int tick) {
  if (cfg.telemEvery > 0) g_nextTelem = (tick / cfg.telemEvery + 1) * cfg.telemEvery;
  if (cfg.traceEvery > 0) g_nextTrace = (tick / cfg.traceEvery + 1) * cfg.traceEvery;
  g_lastTelemTick = g_lastTraceTick = -1;
  g_lastTick = tick;
  g_scriptPos = 0;
  while (g_scriptPos < g_script.size() && g_script[g_scriptPos].tick <= tick) ++g_scriptPos;
  g_packetDue.clear();
  g_loaded = true;
}

void RestoreSidecar(const char* save, int fromTick) {
  const int tick = g_gameImpl.tick_;
  const std::string dir = SidecarDir(save);
  std::map<std::string, std::string> meta;
  {
    std::ifstream f(dir + "\\meta.txt");
    std::string line;
    while (std::getline(f, line)) {
      const size_t sp = line.find(' ');
      if (sp != std::string::npos) meta[line.substr(0, sp)] = line.substr(sp + 1);
    }
  }
  char what[640];
  const char* why = meta.empty() ? "no recording (saved outside a recording session?)"
                  : meta["identity"] != RecIdentity() ? "the recording of another start (mission, seed, campaign or setup)"
                  : meta["save"] != SaveFingerprint(save) ? "a recording of another game (the file was saved over without one)"
                  : atoi(meta["tick"].c_str()) != tick ? "a recording made at another tick"
                  : nullptr;
  if (why) {
    snprintf(what, sizeof what, "loaded %s (tick %d), which has %s: the recording does not lead to this game", save, tick, why);
    g_recBroken = what;
    Log("record: WARN %s", what);
    g_recLoads.push_back(what);
    AfterLoad(tick);
    WriteRecording();
    return;
  }
  g_recBroken = meta["broken"];
  const size_t before = g_recLines.size();
  g_recLines.clear();
  {
    std::ifstream f(dir + "\\packets.txt");
    std::string line;
    while (std::getline(f, line)) if (!line.empty()) g_recLines.push_back(line);
  }
  // The telemetry (and trace) of the save's history, continued from the save's tick.
  FILE** files[] = { &g_telem, &g_trace };
  const char* names[] = { "telemetry.jsonl", "trace.jsonl" };
  for (int k = 0; k < 2; ++k) {
    if (!*files[k]) continue;
    fclose(*files[k]);
    const std::string mine = cfg.out + "\\" + names[k];
    if (!CopyFileA((dir + "\\" + names[k]).c_str(), mine.c_str(), FALSE)) DeleteFileA(mine.c_str());
    *files[k] = fopen(mine.c_str(), "a");
  }
  AfterLoad(tick);
  snprintf(what, sizeof what, "load %s: from tick %d back to its tick %d, %u packets kept, %d of the abandoned history dropped",
           save, fromTick, tick, unsigned(g_recLines.size()), int(before) - int(g_recLines.size()));
  g_recLoads.push_back(what);
  Log("record: %s", what);
  WriteRecording();
}

ibool __fastcall GameSaveHook(GameImpl* self, void*, void* stream, void* dialog) {
  const ibool ok = g_origGameSave(self, stream, dialog);
  g_saveResult = ok;
  return ok;
}

void __fastcall TAppSaveHook(void* self, void*, const char* name, void* dialog) {
  g_saveResult = -1;
  g_origTAppSave(self, name, dialog);
  if (g_saveResult > 0) WriteSidecar(name);
  else if (name) {
    Log("record: saving %s failed (%d): no sidecar", name, g_saveResult);
    DeleteSidecar(name);  // The file may be half written: no history may claim it.
  }
}

int __fastcall TAppLoadHook(void* self, void*, const char* name) {
  const int from = g_gameImpl.tick_;
  const int ok = g_origTAppLoad(self, name);
  if (ok) RestoreSidecar(name, from);
  else Log("record: loading %s failed", name ? name : "?");
  return ok;
}

// `savegame NAME` / `loadgame NAME`: TApp::SaveGame / TApp::LoadGame on OPU\saves\NAME (or a path), as the dialogs do, through
// the recording's hooks in a recording. For testing recordings (a load from the script rolls a recording back like
// one from the dialog): each runs once, and a recording keeps them as comments.
// They run after the cycle's samples (telemetry, trace) and session state, as a save or load from the dialog comes
// after the cycle; RunSaveLoad at the end of GameCycleHook.
std::vector<std::pair<std::string, std::string>> g_saveLoadDue;  // verb, path

// A save's path: OPU\saves\NAME in the game folder, as the dialogs name their files, or a path as given.
std::string SavePathOf(const std::string& a) {
  if (a.find_first_of(":\\/") != std::string::npos) return a;
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, sizeof exe - 1);
  std::string dir = exe;
  return dir.substr(0, dir.find_last_of('\\') + 1) + "OPU\\saves\\" + a;
}

void SaveLoadVerb(const ScriptCmd& c, int tick) {
  if (c.args.empty()) { Log("script: WARN tick %d line %d: `%s` needs a file name (skipped)", tick, c.line, c.verb.c_str());  return; }
  g_saveLoadDue.emplace_back(c.verb, SavePathOf(c.args[0]));
}

// ---------------------------------------------------------------------------------------------------------------------
// Checkpoints (README "Checkpoints"): op2-inspect's game sessions resume a long game from a save instead of replaying
// its script from tick 0. A `savegame` outside a recording also writes `<save>.dll`, the mission DLL's writable
// sections (its PE headers in memory) as they are at the save. OP2H_RESUME (`run.py --resume SAVE`) loads a save at the
// first game cycle, before any script line runs (TApp::LoadGame, as the Load dialog does), then plays the script, whose
// lines are the ones still to run: all at the save's tick or later. Those at the save's tick run right after the load,
// where they would have run after the saved cycle, and that tick's samples and session events are taken again after
// them. After the load, the DLL's writable memory outside its save region (GetSaveRegions, as MissionManager keeps it)
// is compared with `<save>.dll` and what the save didn't carry is put back where it can be (CheckDllState; the
// `checkpoint: dll` line in harness.log says what), as are the hives' counts (`<save>.hive`, PutBackHiveCounts).

struct DllSection { uint32_t rva, size; };

const uint8_t* MissionBase() {
  const MissionManager* m = MissionManager::GetInstance();
  return m ? reinterpret_cast<const uint8_t*>(m->hModule_) : nullptr;
}

std::vector<DllSection> WritableSections(const uint8_t* base) {
  std::vector<DllSection> out;
  if (!base || IsBadReadPtr(base, sizeof(IMAGE_DOS_HEADER))) return out;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  if (IsBadReadPtr(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return out;
  const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
    if (s->Characteristics & IMAGE_SCN_MEM_WRITE)
      out.push_back({ uint32_t(s->VirtualAddress), uint32_t(s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData) });
  return out;
}

void WriteDllState(const std::string& save) {
  const std::string path = save + ".dll";
  DeleteFileA(path.c_str());
  const uint8_t* base = MissionBase();
  const auto sections = WritableSections(base);
  FILE* f = sections.empty() ? nullptr : fopen(path.c_str(), "wb");
  if (!f) { Log("checkpoint: WARN no mission DLL state written for %s", save.c_str());  return; }
  const SaveRegion& r = MissionManager::GetInstance()->saveRegion_;
  fprintf(f, "op2h-dll 2 %08x %08x %u %u\n", unsigned(uintptr_t(base)), unsigned(uintptr_t(r.pData)), unsigned(r.size), unsigned(sections.size()));
  for (const auto& s : sections) fprintf(f, "%08x %u\n", s.rva, s.size);
  for (const auto& s : sections) fwrite(base + s.rva, 1, s.size, f);
  // Then each dword as this process's DecodePointer reads it: the CRT keeps some pointers encoded with the process's
  // own secret, which only this process can undo.
  for (const auto& s : sections)
    for (uint32_t k = 0; k + 4 <= s.size; k += 4) {
      void* p;
      memcpy(&p, base + s.rva + k, 4);
      const uint32_t d = uint32_t(uintptr_t(DecodePointer(p)));
      fwrite(&d, 4, 1, f);
    }
  const bool ok = !ferror(f);
  if (fclose(f) != 0 || !ok) { DeleteFileA(path.c_str());  Log("checkpoint: WARN writing %s failed", path.c_str()); }
}

// The CRT's security cookie of the mission DLL (its load config's SecurityCookie), and where it is; 0 without one.
uintptr_t CookieAddress(const uint8_t* base) {
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
  if (!dir.VirtualAddress || dir.Size < offsetof(IMAGE_LOAD_CONFIG_DIRECTORY32, SecurityCookie) + 4) return 0;
  return reinterpret_cast<const IMAGE_LOAD_CONFIG_DIRECTORY32*>(base + dir.VirtualAddress)->SecurityCookie;
}

uint32_t CookieNow(const uint8_t* base) {
  const uintptr_t at = base ? CookieAddress(base) : 0;
  return at && !IsBadReadPtr(reinterpret_cast<const void*>(at), 4) ? *reinterpret_cast<const uint32_t*>(at) : 0;
}

// The DLL's writable sections as its file defines them (zero past each section's raw data): what a load that loads
// the DLL again leaves in a global nothing set since. None when the file can't be read or the DLL isn't at its
// preferred base (its pointers would be relocated, not the file's).
std::vector<std::string> FileSections(const uint8_t* base, const std::vector<DllSection>& secs) {
  std::vector<std::string> out(secs.size());
  char path[MAX_PATH] = {};
  if (!GetModuleFileNameA(reinterpret_cast<HMODULE>(const_cast<uint8_t*>(base)), path, sizeof path - 1)) return out;
  const std::string file = ReadFile(path);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  if (file.empty() || nt->OptionalHeader.ImageBase != uintptr_t(base)) return out;
  const IMAGE_SECTION_HEADER* h = IMAGE_FIRST_SECTION(nt);
  for (size_t i = 0; i < secs.size(); ++i)
    for (int k = 0; k < nt->FileHeader.NumberOfSections; ++k) {
      if (h[k].VirtualAddress != secs[i].rva) continue;
      out[i].assign(secs[i].size, '\0');
      const size_t n = std::min<size_t>({ h[k].SizeOfRawData, secs[i].size, file.size() > h[k].PointerToRawData ? file.size() - h[k].PointerToRawData : 0 });
      if (n) memcpy(&out[i][0], file.data() + h[k].PointerToRawData, n);
    }
  return out;
}

// After a resume's load: the DLL's writable dwords outside its save region against the saved game's. The load loads
// the DLL again, so its globals start over from the file's values, and a process has its own security cookie, heap,
// handles and CRT state. Classed in this order:
// - put back: the loaded value is still the file's and the saved one is a small number (below 64 KB, or a small
//   negative) or an address in a module mapped here: a global the mission set during play, which the load undid;
// - mission state not put back (`lost`): other plain numbers (below 4 MB): a value this process set itself (a handle,
//   a TLS index, a CRT counter) or one that may be a low heap address, where writing the other process's would be
//   wrong; and anything left after the next two;
// - process-only: the cookie and values XORed with it (their difference is the cookies'), pointers the CRT encodes
//   with it or with the process's own secret (EncodePointer) that both decode to addresses, and heap addresses.
// `checkpoint: dll same | process-only | restored | DIFFERS` says what; with any lost dword op2-inspect checks the
// session against a replay from tick 0 at once and then more often.
void CheckDllState(const std::string& save) {
  std::ifstream f(save + ".dll", std::ios::binary);
  std::string magic;
  unsigned version = 0, savedBase = 0, savedRegion = 0, savedSize = 0, n = 0;
  if (!(f >> magic >> version >> std::hex >> savedBase >> savedRegion >> std::dec >> savedSize >> n) || magic != "op2h-dll" || version != 2) {
    Log("checkpoint: dll unknown (no state saved beside %s)", save.c_str());
    return;
  }
  std::vector<DllSection> saved(n);
  for (auto& s : saved) f >> std::hex >> s.rva >> std::dec >> s.size;
  f.get();  // The header's last newline.
  uint8_t* base = const_cast<uint8_t*>(MissionBase());
  const auto now = WritableSections(base);
  const SaveRegion& r = MissionManager::GetInstance()->saveRegion_;
  const uintptr_t lo = uintptr_t(r.pData), hi = lo + r.size;
  bool sameLayout = now.size() == saved.size() && savedBase == unsigned(uintptr_t(base)) && savedRegion == unsigned(lo) && savedSize == unsigned(r.size);
  for (size_t i = 0; sameLayout && i < now.size(); ++i) sameLayout = now[i].rva == saved[i].rva && now[i].size == saved[i].size;
  if (!sameLayout) {
    Log("checkpoint: dll DIFFERS: the mission DLL's layout or save region isn't the saved game's (base %08x/%08x, region %08x+%u/%08x+%u)",
        savedBase, unsigned(uintptr_t(base)), savedRegion, savedSize, unsigned(lo), unsigned(r.size));
    return;
  }
  std::vector<std::string> bytes(saved.size());
  for (size_t i = 0; i < saved.size(); ++i) {
    bytes[i].resize(saved[i].size);
    f.read(&bytes[i][0], saved[i].size);
    if (size_t(f.gcount()) != saved[i].size) { Log("checkpoint: dll DIFFERS: %s.dll is short", save.c_str());  return; }
  }
  std::vector<std::vector<uint32_t>> decoded(saved.size());
  for (size_t i = 0; i < saved.size(); ++i) {
    decoded[i].resize(saved[i].size / 4);
    f.read(reinterpret_cast<char*>(decoded[i].data()), decoded[i].size() * 4);
    if (size_t(f.gcount()) != decoded[i].size() * 4) { Log("checkpoint: dll DIFFERS: %s.dll is short", save.c_str());  return; }
  }
  const uintptr_t cookieAt = CookieAddress(base);
  uint32_t cookieSaved = 0;
  const uint32_t cookieNow = CookieNow(base);
  for (size_t i = 0; cookieAt && i < saved.size(); ++i) {
    const uintptr_t from = uintptr_t(base) + saved[i].rva;
    if (cookieAt >= from && cookieAt + 4 <= from + saved[i].size) memcpy(&cookieSaved, &bytes[i][cookieAt - from], 4);
  }
  // An address: above the 4 MB a mission's numbers (ticks, ids, counts) stay under, or (`here`) one this process has
  // mapped, above the first 64 KB (Wine keeps the process parameters, e.g. the command line, low). A number: a count, id,
  // tick or flag, or a small negative. A dword that differs is a heap address when the loaded one is mapped here.
  auto address = [](uint32_t v) { return v >= 0x400000 && v < 0x80000000u; };
  auto number = [](uint32_t v) { return v < 0x400000 || v >= 0xFFC00000u; };
  auto small = [](uint32_t v) { return v < 0x10000 || v >= 0xFFFF0000u; };  // Below any address Wine hands out.
  const auto file = FileSections(base, saved);
  auto here = [&](uint32_t v) {
    MEMORY_BASIC_INFORMATION m;
    return v >= 0x10000 && v < 0x80000000u && VirtualQuery(reinterpret_cast<const void*>(uintptr_t(v)), &m, sizeof m) && m.State == MEM_COMMIT;
  };
  auto image = [&](uint32_t v) {
    MEMORY_BASIC_INFORMATION m;
    return v >= 0x10000 && v < 0x80000000u && VirtualQuery(reinterpret_cast<const void*>(uintptr_t(v)), &m, sizeof m) &&
           m.State == MEM_COMMIT && m.Type == MEM_IMAGE;
  };
  // The CRTs' pointer decodings with the cookie: the UCRT's (rotate right by cookie % 32, then XOR the cookie) and an
  // older one's (XOR, then rotate), as measured on c_euf.dll's onexit table.
  auto ror = [](uint32_t v, int k) { k &= 31;  return k ? (v >> k | v << (32 - k)) : v; };
  auto decode = [&](uint32_t v, uint32_t cookie) { return ror(v, cookie & 31) ^ cookie; };
  auto decodeOld = [&](uint32_t v, uint32_t cookie) { return ror(v ^ cookie, cookie & 31); };
  // Both decode to the same value, or to aligned addresses (this one mapped).
  auto same = [&](uint32_t da, uint32_t db) { return da == db || (da % 4 == 0 && db % 4 == 0 && address(da) && here(db)); };
  auto encoded = [&](uint32_t a, uint32_t b, uint32_t systemSaved) {
    void* p;
    memcpy(&p, &b, 4);
    return same(decode(a, cookieSaved), decode(b, cookieNow)) || same(decodeOld(a, cookieSaved), decodeOld(b, cookieNow)) ||
           same(systemSaved, uint32_t(uintptr_t(DecodePointer(p))));
  };
  size_t total = 0, cookieBytes = 0, heapBytes = 0;
  std::vector<std::pair<uintptr_t, uint32_t>> restore;  // address, the saved game's value
  std::string restored, lost;
  int shownRestored = 0, shownLost = 0;
  auto note = [](std::string& list, int& shown, uintptr_t at, uint32_t was, uint32_t is) {
    if (shown++ >= 12) return;
    char b[48];
    snprintf(b, sizeof b, "%s%08x %08x/%08x", list.empty() ? "" : ", ", unsigned(at), unsigned(was), unsigned(is));
    list += b;
  };
  size_t restoredBytes = 0, lostBytes = 0;
  for (size_t i = 0; i < saved.size(); ++i) {
    const auto& s = saved[i];
    const uint8_t* was = reinterpret_cast<const uint8_t*>(bytes[i].data());
    const uint8_t* cur = base + s.rva;
    for (uint32_t k = 0; k + 4 <= s.size; k += 4) {
      size_t differ = 0, counted = 0;
      for (uint32_t j = k; j < k + 4; ++j) {
        const uintptr_t at = uintptr_t(cur) + j;
        if (at >= lo && at < hi) continue;
        ++counted;
        differ += was[j] != cur[j];
      }
      total += counted;
      if (!differ) continue;
      uint32_t a = 0, b = 0;
      memcpy(&a, was + k, 4);
      memcpy(&b, cur + k, 4);
      const uintptr_t at = uintptr_t(cur) + k;
      uint32_t initial = 0;
      const bool known = !file[i].empty();
      if (known) memcpy(&initial, file[i].data() + k, 4);
      if (counted == 4 && known && b == initial && (small(a) || image(a))) {
        // Untouched since the load loaded the DLL again (its file's value), and as saved a small number or an address
        // in a module mapped here (the same in every process, e.g. a function pointer TethysAPI looked up on first
        // use): a global the mission set during play. Put back as the continued game has it.
        restore.emplace_back(at, a);
        restoredBytes += differ;
        note(restored, shownRestored, at, a, b);
      } else if (a >= 0x10000 && a < 0x80000000u && here(b)) {
        // Both addresses, the loaded one mapped here (low ones too: Wine keeps the process parameters, e.g. the
        // command line the CRT notes, below 4 MB).
        heapBytes += differ;
      } else if (counted == 4 && number(a) && number(b)) {
        // Plain numbers, but not one the mission set and the load undid: a value this process set itself (a handle,
        // a TLS index, a CRT counter) or one that may be a low heap address. Not put back; reported.
        lostBytes += differ;
        note(lost, shownLost, at, a, b);
      } else if ((cookieSaved != cookieNow && (a ^ b) == (cookieSaved ^ cookieNow)) || encoded(a, b, decoded[i][k / 4])) {
        cookieBytes += differ;
      } else if (address(a) && address(b)) {
        heapBytes += differ;
      } else {
        lostBytes += differ;
        note(lost, shownLost, at, a, b);
        void* p;
        memcpy(&p, &b, 4);
        Log("checkpoint: lost %08x: %08x/%08x, decoded with the cookie %08x/%08x, by DecodePointer %08x/%08x; cookies %08x/%08x", unsigned(at), a, b,
            decode(a, cookieSaved), decode(b, cookieNow), decoded[i][k / 4], unsigned(uintptr_t(DecodePointer(p))), cookieSaved, cookieNow);
      }
    }
  }
  for (const auto& [at, v] : restore) memcpy(reinterpret_cast<void*>(at), &v, 4);
  char what[200];
  snprintf(what, sizeof what, "%u writable bytes outside the save region %08x+%u; %u differ by the security cookie, %u are heap addresses",
           unsigned(total), unsigned(lo), unsigned(r.size), unsigned(cookieBytes), unsigned(heapBytes));
  if (lostBytes) Log("checkpoint: dll DIFFERS: %u bytes of mission state can't be put back (addresses the loaded DLL lacks: %s%s); %u restored (%s)",
                     unsigned(lostBytes), lost.c_str(), shownLost > 12 ? ", ..." : "", unsigned(restoredBytes), what);
  else if (restoredBytes) Log("checkpoint: dll restored: %u bytes of mission state outside the save region put back as saved (address saved/loaded: %s%s; %s)",
                              unsigned(restoredBytes), restored.c_str(), shownRestored > 12 ? ", ..." : "", what);
  else Log("checkpoint: dll %s (%s)", cookieBytes || heapBytes ? "process-only" : "same", what);
}

// The session state as the loaded game has it, without an event: the next SessionTick reports only what changes after.
void SessionRebaseline(int tick) {
  if (!g_events) return;
  g_seen.clear();
  g_seenTech.clear();
  g_seenObjective.clear();
  g_seenDisaster.clear();
  g_objectiveScanTick = -1000000;
  g_sessionStarted = false;
  g_sessionSilent = true;
  SessionTick(tick);
  g_sessionSilent = false;
}

// The tube networks' counts. A save doesn't hold the hives (HiveManager): saving rebuilds them in the game that goes on
// (new slots) and loading builds them afresh, both with the counts a hive keeps (Command Centers +0xC, an enabled one
// +0x10, DIRTs +0x14) at zero until the owner's next allocation pass recounts them (0x4300B0; every 256 ticks for a
// computer player, so up to 256 ticks: p12's Eden after any save, measured 2026-11-06). DoDamage's Damage Prevention
// reads them meanwhile (0x430050), so a structure hit then takes another damage than in the game never saved. Each
// structure's counts are noted before a checkpoint's save (`SAVE.hive`) and put back into its hive after the save and
// after the load: the game then goes on as if never saved.
struct HiveCounts { int id, cc, enabled, dirt; };

std::vector<HiveCounts> NoteHiveCounts() {
  std::vector<HiveCounts> out;
  ForEachPlayerUnit([&](Unit u) {
    if (!u.IsLive() || !u.IsBuilding()) return;
    const int* h = *reinterpret_cast<const int* const*>(reinterpret_cast<const uint8_t*>(u.GetMapObject()) + 0x38);
    if (h && !IsBadReadPtr(h, 0x1C)) out.push_back({ u.GetID(), h[3], h[4], h[5] });
  });
  return out;
}

// Puts the noted counts back into each structure's hive; returns how many hives got two members' different counts
// (the hives were rebuilt otherwise than they were: the counts of the last member noted stand).
int PutBackHiveCounts(const std::vector<HiveCounts>& counts) {
  std::map<int*, HiveCounts> set;
  int clash = 0;
  for (const auto& c : counts) {
    const Unit u(c.id);
    if (!Exists(u) || !u.IsBuilding()) continue;
    int* h = *reinterpret_cast<int* const*>(reinterpret_cast<const uint8_t*>(u.GetMapObject()) + 0x38);
    if (!h || IsBadWritePtr(h, 0x1C)) continue;
    const auto [it, fresh] = set.emplace(h, c);
    if (!fresh && (it->second.cc != c.cc || it->second.enabled != c.enabled || it->second.dirt != c.dirt)) ++clash;
    h[3] = c.cc, h[4] = c.enabled, h[5] = c.dirt;
  }
  return clash;
}

// OP2H_CHECKPOINT's save, with the mission DLL's state (`SAVE.dll`) and the hives' counts (`SAVE.hive`) beside it; the
// counts are put back after SaveGame too (the run's last records and state.json come after the save). Only checkpoint
// saves do this: a `savegame` line saves as the game does, so the original's own post-save behaviour stays measurable.
void SaveCheckpoint(int tick) {
  const std::string path = SavePathOf(cfg.checkpoint);
  DeleteFileA(path.c_str());  // So a save that fails leaves no older file behind to pass for it.
  for (const char* ext : { ".dll", ".hive" }) DeleteFileA((path + ext).c_str());
  const auto hives = NoteHiveCounts();
  g_tApp.SaveGame(path);
  if (const int clash = PutBackHiveCounts(hives)) Log("checkpoint: WARN the save rebuilt %d hives otherwise (their counts may differ)", clash);
  if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    Log("checkpoint: WARN save %s at tick %d failed", path.c_str(), tick);
    return;
  }
  WriteDllState(path);
  // Written whole or not at all: a header with the count, to a temporary file renamed into place.
  const std::string hive = path + ".hive", tmp = hive + ".tmp";
  bool ok = false;
  if (FILE* f = fopen(tmp.c_str(), "w")) {
    fprintf(f, "op2h-hive 1 %u\n", unsigned(hives.size()));
    for (const auto& c : hives) fprintf(f, "%d %d %d %d\n", c.id, c.cc, c.enabled, c.dirt);
    ok = !ferror(f);
    ok = fclose(f) == 0 && ok;
  }
  if (!ok || !MoveFileExA(tmp.c_str(), hive.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileA(tmp.c_str());
    Log("checkpoint: WARN writing %s failed: no checkpoint", hive.c_str());
    DeleteFileA(path.c_str());
    return;
  }
  Log("checkpoint: saved %s at tick %d", path.c_str(), tick);
}

// After a resume's load: the hives' counts as the saved game had them. `checkpoint: hives: ...` says it was done; any
// other `checkpoint: hives` line (no file, a short one) makes op2-inspect distrust the run.
void LoadHiveCounts(const std::string& save) {
  std::ifstream f(save + ".hive");
  std::string magic;
  unsigned version = 0, n = 0;
  if (!(f >> magic >> version >> n) || magic != "op2h-hive" || version != 1) {
    Log("checkpoint: hives unknown (no counts saved beside %s)", save.c_str());
    return;
  }
  std::vector<HiveCounts> counts;
  for (HiveCounts c; counts.size() < n && f >> c.id >> c.cc >> c.enabled >> c.dirt;) counts.push_back(c);
  if (counts.size() != n) { Log("checkpoint: hives unknown (%s.hive is short: %u of %u)", save.c_str(), unsigned(counts.size()), n);  return; }
  const int clash = PutBackHiveCounts(counts);
  Log("checkpoint: hives: the counts of %u structures' hives put back%s", unsigned(counts.size()),
      clash ? (" (WARN: " + std::to_string(clash) + " hives rebuilt otherwise than saved)").c_str() : "");
}

// OP2H_RESUME: loads the checkpoint and sets the harness's clocks to its tick. Returns that tick, or -1.
int Resume() {
  const std::string path = SavePathOf(cfg.resume);
  const int from = g_gameImpl.tick_;
  // Whether the load loads the mission DLL again (its CRT draws a new cookie) or keeps the one this start loaded.
  const uint8_t* baseBefore = MissionBase();
  const uint32_t cookieBefore = CookieNow(baseBefore);
  const int ok = GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES ? g_tApp.LoadGame(path) : 0;
  const int now = g_gameImpl.tick_;
  Log("resume: load %s at tick %d: %s, now tick %d; the mission DLL %s", path.c_str(), from, ok ? "ok" : "FAILED", now,
      MissionBase() != baseBefore || CookieNow(MissionBase()) != cookieBefore ? "was loaded again"
      : cookieBefore ? "stayed as the start loaded it" : "has no CRT cookie to tell whether it was loaded again");
  if (!ok) return -1;
  CheckDllState(path);
  LoadHiveCounts(path);
  g_lastTick = now;
  auto up = [](int t, int every) { return (t + every - 1) / every * every; };
  if (cfg.telemEvery > 0) g_nextTelem = up(now, cfg.telemEvery);
  if (cfg.traceEvery > 0) g_nextTrace = up(std::max(now, EnvInt("OP2H_TRACE_FROM", 0)), cfg.traceEvery);
  g_lastTelemTick = g_lastTraceTick = -1;
  g_packetDue.clear();
  g_scriptPos = 0;
  while (g_scriptPos < g_script.size() && g_script[g_scriptPos].tick < now) {
    Log("script: WARN tick %d line %d: `%s` is before the checkpoint's tick %d (skipped)", g_script[g_scriptPos].tick,
        g_script[g_scriptPos].line, g_script[g_scriptPos].verb.c_str(), now);
    ++g_scriptPos;
  }
  SessionRebaseline(now);
  return now;
}

void RunSaveLoad(int tick) {
  for (size_t k = 0; k < g_saveLoadDue.size(); ++k) {
    const auto [verb, path] = g_saveLoadDue[k];
    if (verb == "savegame") {
      if (g_record) TAppSaveHook(&g_tApp, nullptr, path.c_str(), nullptr);
      else {
        g_tApp.SaveGame(path);
        DeleteSidecar(path.c_str());  // Saved without a recording: an older recording's sidecar no longer matches it.
      }
      Log("script: save %s at tick %d", path.c_str(), tick);
      continue;
    }
    const int ok = g_record ? TAppLoadHook(&g_tApp, nullptr, path.c_str()) : g_tApp.LoadGame(path);
    Log("script: load %s at tick %d: %s, now tick %d", path.c_str(), tick, ok ? "ok" : "FAILED", g_gameImpl.tick_);
    if (ok && !g_record) AfterLoad(g_gameImpl.tick_);
    if (ok) break;  // Anything after a load in the same tick belonged to the abandoned game.
  }
  g_saveLoadDue.clear();
}

void InstallRecordHooks() {
  InstallPacketHook();
  g_origGameSave = reinterpret_cast<GameSaveFn>(RelinkCall(CallGameImplSaveGame, reinterpret_cast<void*>(&GameSaveHook)));
  g_origTAppSave = reinterpret_cast<TAppSaveFn>(RelinkCall(CallTAppSaveGame, reinterpret_cast<void*>(&TAppSaveHook)));
  for (uintptr_t at : CallsTAppLoadGame) {
    void* prev = RelinkCall(at, reinterpret_cast<void*>(&TAppLoadHook));
    if (!g_origTAppLoad) g_origTAppLoad = reinterpret_cast<TAppLoadFn>(prev);
    else if (prev != reinterpret_cast<void*>(g_origTAppLoad)) Log("record: WARN the load call at %08x goes to %p, not %p", unsigned(at), prev, (void*)g_origTAppLoad);
  }
  Log("record: hooks save %p/%p load %p", (void*)g_origTAppSave, (void*)g_origGameSave, (void*)g_origTAppLoad);
  if (!cfg.script.empty()) g_recSetup = ReadFile(cfg.script);
  WriteRecording();
}

// ---------------------------------------------------------------------------------------------------------------------
// Network games (shell mode with OP2H_NET=1; netgame.py plays a LAN game between two copies of the game and
// docs/multiplayer.md "Stage 2a" says what was measured). Each copy logs, in its output folder:
//   start_info.json  at the game's first cycle: the GameStartInfo the lobby agreed on (seed, map DLL, flags, slots),
//                    raw and decoded, the local player and the turn constants (CPPI, its log2, the command delay)
//   packets.jsonl    every player's command slot as ProcessGameCycle hands it to PlayerImpl::ProcessCommandPacket
//                    (0x489FF5, every 4th tick, before the units act): tick, player, type, dataLength, the slot's
//                    +6 (timeStamp) and +0xA (netID) words, the data and the slot's tail (what `packet` lines carry)
//   net.jsonl        every UDP datagram the game sends or receives (WSOCK32 sendto / recvfrom, ordinals 20 and 17,
//                    through their import slots), with the time, the tick, the peer and the bytes
// and a tick script's orders become the local player's own commands: while script lines run, a call to
// ProcessCommandPacket (0x40E300) that would execute an order on this machine alone (the verbs' packets and TethysAPI's
// Unit::Do* helpers) is written into the local player's pending slot instead, as the UI's command handlers do (0x490810:
// the slot for turn (tick + interval - 1) / interval + delay, stamped +6 = 0x80000000, +0xA = 0). The turn exchange then
// sends it and every copy runs it in the same cycle. A slot holds one command: a later order in the same turn, or the
// frame's own ctMachineSettings (after cycles whose interval number is 7 mod 8, 0x49C55C), replaces it: harness.log
// gets a WARN when an order replaces a pending command and when a queued order didn't run, and packets.jsonl shows what
// ran. Orders for another player's units are skipped with a WARN; `packet` lines are refused (they would run on one
// machine only), and the script's other verbs that change the game (spawn, setcell, poke, ...) would desync the copies:
// don't use them in a network game. start_info.json's slot `active` is playerFlags bit 0x02 (TethysAPI's `isHost`),
// set on every human slot of a network game, so it isn't the host.

std::string JsonStr(const char* s, size_t max = 400);

FILE* g_netPackets = nullptr;
FILE* g_netWire = nullptr;
CRITICAL_SECTION g_netLock;
bool g_netScript = false;  // Script lines are running (the game thread): orders go to the pending slot.
SlotPacketFn g_netProcess = nullptr;  // ProcessCommandPacket's own code (the entry hook's trampoline).
int64_t g_netT0 = 0;
int g_netDrawFrom = 0;  // OP2H_DRAW_FROM: with turbo, draw every tick from this one (the driver's last screenshot).
// The script orders waiting in the local player's ring, by slot index: the command and the tick it was queued, to warn
// when something else replaces it before it runs.
struct NetQueued { CommandType type;  int tick; };
std::map<int, NetQueued> g_netQueued;

using PendingSlotFn = CommandPacket*(__thiscall*)(PlayerImpl*);
const auto PendingSlot = reinterpret_cast<PendingSlotFn>(0x490810);

int PlayerIndex(const PlayerImpl* p) {
  for (int i = 0; i < 7; ++i) if (g_gameImpl.GetPlayer(i) == p) return i;
  return -1;
}

ibool __fastcall NetProcessHook(PlayerImpl* self, void*, CommandPacket* p) {
  if (!g_netScript) return g_netProcess(self, p);
  const int player = PlayerIndex(self);
  if (player != g_gameImpl.localPlayer_) {
    Log("net order: WARN tick %d: %s for player %d, not the local player %d (skipped)", g_gameImpl.tick_,
        CommandName(int(p->type)), player, g_gameImpl.localPlayer_);
    return 0;
  }
  CommandPacket* slot = PendingSlot(self);
  const int index = int((reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(self) - 0x4C8) / sizeof(CommandPacket));
  // An earlier script order in this slot that something else (the frame's ctMachineSettings) replaced already.
  if (auto it = g_netQueued.find(index); it != g_netQueued.end() && it->second.type != slot->type)
    Log("net order: WARN tick %d: the %s queued at tick %d won't run: the frame replaced it with %s", g_gameImpl.tick_,
        CommandName(int(it->second.type)), it->second.tick, CommandName(int(slot->type)));
  if (slot->type != CommandType::Nop)
    Log("net order: WARN tick %d: %s replaces the pending %s in the local player's slot (one command per turn)",
        g_gameImpl.tick_, CommandName(int(p->type)), CommandName(int(slot->type)));
  slot->type = p->type;
  slot->dataLength = p->dataLength;
  memcpy(slot->data.buffer, p->data.buffer, CommandPacketDataSize);
  Log("net order: tick %d %s %s -> slot %d", g_gameImpl.tick_, CommandName(int(p->type)),
      HexStr(p->data.buffer, std::min<size_t>(p->dataLength, CommandPacketDataSize)).c_str(), index);
  g_netQueued[index] = { p->type, g_gameImpl.tick_ };
  return 1;
}

ibool __fastcall NetSlotHook(PlayerImpl* self, void*, CommandPacket* p) {
  const size_t n = std::min<size_t>(p->dataLength, CommandPacketDataSize), tail = TailLength(*p);
  // A script order replaced before it ran (by the frame's ctMachineSettings, or a later order in the same turn).
  if (PlayerIndex(self) == g_gameImpl.localPlayer_) {
    const int index = int((reinterpret_cast<uint8_t*>(p) - reinterpret_cast<uint8_t*>(self) - 0x4C8) / sizeof(CommandPacket));
    if (auto it = g_netQueued.find(index); it != g_netQueued.end()) {
      if (it->second.type != p->type)
        Log("net order: WARN tick %d: the %s queued at tick %d didn't run: the slot holds %s", g_gameImpl.tick_,
            CommandName(int(it->second.type)), it->second.tick, CommandName(int(p->type)));
      g_netQueued.erase(it);
    }
  }
  if (!g_netPackets) return g_origSlotPacket(self, p);
  fprintf(g_netPackets, "{\"tick\":%d,\"player\":%d,\"type\":%d,\"name\":\"%s\",\"len\":%d,\"stamp\":%d,\"netID\":\"%08x\","
          "\"data\":\"%s\",\"tail\":\"%s\"}\n", g_gameImpl.tick_, PlayerIndex(self), int(p->type), CommandName(int(p->type)),
          int(p->dataLength), p->timeStamp, unsigned(p->netID), HexStr(p->data.buffer, n).c_str(),
          HexStr(p->data.buffer + n, tail).c_str());
  fflush(g_netPackets);
  return g_origSlotPacket(self, p);
}

using SendToFn   = int(WINAPI*)(uintptr_t, const char*, int, int, const sockaddr*, int);
using RecvFromFn = int(WINAPI*)(uintptr_t, char*, int, int, sockaddr*, int*);
SendToFn   g_sendTo   = nullptr;
RecvFromFn g_recvFrom = nullptr;

void WireEvent(const char* dir, uintptr_t s, const sockaddr* a, const char* buf, int n) {
  if (n <= 0 || !g_netWire) return;
  char peer[32] = "?";
  if (a && a->sa_family == AF_INET) {
    const auto* in = reinterpret_cast<const sockaddr_in*>(a);
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(&in->sin_addr);
    snprintf(peer, sizeof peer, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], (unsigned(in->sin_port) >> 8 | unsigned(in->sin_port) << 8) & 0xFFFF);
  }
  const std::string hex = HexStr(reinterpret_cast<const uint8_t*>(buf), size_t(n));
  EnterCriticalSection(&g_netLock);
  fprintf(g_netWire, "{\"ms\":%.1f,\"tick\":%d,\"dir\":\"%s\",\"sock\":%u,\"peer\":\"%s\",\"hex\":\"%s\"}\n",
          (Now() - g_netT0) * 1000.0 / Frequency(), g_gameImpl.tick_, dir, unsigned(s), peer, hex.c_str());
  fflush(g_netWire);
  LeaveCriticalSection(&g_netLock);
}

int WINAPI SendToHook(uintptr_t s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
  const int r = g_sendTo(s, buf, len, flags, to, tolen);
  WireEvent("send", s, to, buf, r > 0 ? len : 0);
  return r;
}

int WINAPI RecvFromHook(uintptr_t s, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
  const int r = g_recvFrom(s, buf, len, flags, from, fromlen);
  WireEvent("recv", s, from, buf, r);
  return r;
}

// The lobby's GameStartInfo as the game started with it (GameImpl +gameStartInfo_), raw and decoded.
void WriteStartInfo(const std::string& path) {
  const GameStartInfo& g = g_gameImpl.gameStartInfo_;
  const auto& f = g.startupFlags;
  FILE* out = fopen(path.c_str(), "w");
  if (!out) return;
  fprintf(out, "{\"hex\":\"%s\",\n \"script\":%s,\"missionType\":%d,\"maxPlayers\":%d,\"numPlayers\":%d,\"multiplayer\":%d,"
          "\"disasters\":%d,\"dayNight\":%d,\"moraleSteady\":%d,\"cheats\":%d,\"initialVehicles\":%d,\"version\":\"%08x\","
          "\"gameSpeed\":%d,\"seed\":%u,\"checksum\":\"%08x\",\n \"localPlayer\":%d,\"gamePlayers\":%d,\"humanPlayers\":%d,"
          "\"cppi\":%d,\"log2cppi\":%d,\"delay\":%d,\n \"slots\":[",
          HexStr(reinterpret_cast<const uint8_t*>(&g), sizeof g).c_str(), JsonStr(g.scriptName).c_str(),
          int(f.missionType) - (f.missionType & 0x10 ? 32 : 0), int(f.maxPlayers), int(g.gameFlags.numPlayers),
          int(f.isMultiplayer), int(f.disastersEnabled), int(f.dayNightEnabled), int(f.moraleSteady), int(f.cheatsEnabled),
          int(f.numInitialVehicles), unsigned(g.version), g.gameSpeed, unsigned(g.randomNumberSeed), unsigned(g.checksum),
          g_gameImpl.localPlayer_, g_gameImpl.numPlayers_, g_gameImpl.numHumanPlayers_,
          g_gameImpl.commandPacketProcessingInterval_, g_gameImpl.log2CommandPacketProcessingInterval_,
          g_gameImpl.commandPacketProcessingDelay_);
  for (int i = 0; i < 6; ++i) {
    const auto& p = g.playerFlags[i];
    char name[14] = {};
    memcpy(name, g.playerName[i].str, 13);
    fprintf(out, "%s\n  {\"slot\":%d,\"active\":%d,\"eden\":%d,\"resources\":%d,\"color\":%d,\"netID\":\"%08x\",\"name\":%s}",
            i ? "," : "", i, int(p.isHost), int(p.isEden), int(p.resources), int(p.color), unsigned(g.playerNetID[i]),
            JsonStr(name).c_str());
  }
  fprintf(out, "]}\n");
  fclose(out);
}

// OP2H_NET_SEED (netgame.py --seed): every copy replaces the lobby's seed with this one as the game starts, so a network
// game can be played again from the same start. GameImpl::PrepareGame (0x4893B0) seeds the game's RNG from the
// GameStartInfo's randomNumberSeed (+0xB2, at 0x489549) and keeps the GameStartInfo (start_info.json shows the seed it
// ran with); the lobby has agreed on the start and checked its checksum before this, and every copy writes the same
// value, so they stay in step.
using PrepareGameFn = ibool(__thiscall*)(GameImpl*, GameStartInfo*);
PrepareGameFn g_netPrepare = nullptr;
uint32 g_netSeed = 0;
bool g_netSeedSet = false;

ibool __fastcall NetPrepareHook(GameImpl* self, void*, GameStartInfo* gsi) {
  if (g_netSeedSet && gsi) {
    Log("net: seed %u -> %u (OP2H_NET_SEED)", unsigned(gsi->randomNumberSeed), unsigned(g_netSeed));
    gsi->randomNumberSeed = g_netSeed;
  }
  return g_netPrepare(self, gsi);
}

void NetInit() {
  InitializeCriticalSection(&g_netLock);
  if (const char* s = getenv("OP2H_NET_SEED"); s && *s) {
    g_netSeed = uint32(strtoul(s, nullptr, 10));
    g_netSeedSet = true;
    static const uint8_t prepare[] = { 0x81, 0xEC, 0x2C, 0x01, 0x00, 0x00 };  // sub esp,12Ch
    g_netPrepare = reinterpret_cast<PrepareGameFn>(HookEntry(0x4893B0, prepare, 6, reinterpret_cast<void*>(&NetPrepareHook)));
    Log("net: forced seed %u, PrepareGame hook %s", unsigned(g_netSeed), g_netPrepare ? "on" : "FAILED");
    // A game asked for a seed must not quietly start from the lobby's: it would be another game under the same recipe.
    if (!g_netPrepare) { if (g_log) fclose(g_log);  TerminateProcess(GetCurrentProcess(), 2); }
  }
  g_netT0 = Now();
  g_netPackets = fopen((cfg.out + "\\packets.jsonl").c_str(), "w");
  g_netWire = fopen((cfg.out + "\\net.jsonl").c_str(), "w");
  g_origSlotPacket = reinterpret_cast<SlotPacketFn>(RelinkCall(CallSlotPacket, reinterpret_cast<void*>(&NetSlotHook)));
  static const uint8_t prologue[] = { 0x81, 0xEC, 0x18, 0x01, 0x00, 0x00 };  // sub esp,118h
  g_netProcess = reinterpret_cast<SlotPacketFn>(HookEntry(0x40E300, prologue, 6, reinterpret_cast<void*>(&NetProcessHook)));
  // WSOCK32's sendto and recvfrom, imported by ordinal (20, 17): the import slots at 0x58391C and 0x58392C.
  g_sendTo   = reinterpret_cast<SendToFn>(PatchIat(0x58391C, reinterpret_cast<void*>(&SendToHook)));
  g_recvFrom = reinterpret_cast<RecvFromFn>(PatchIat(0x58392C, reinterpret_cast<void*>(&RecvFromHook)));
  // Turbo (OP2H_TURBO, netgame.py): the frame limiter lets a cycle run on every idle loop and the panes are drawn only
  // a few ticks in 256; the turn exchange (0x421110, after the limiter) still holds a cycle until every player's turn
  // is in, so the copies stay in lockstep and only run as fast as the slower one.
  cfg.turbo = EnvInt("OP2H_TURBO", 0) != 0;
  g_netDrawFrom = EnvInt("OP2H_DRAW_FROM", 0);
  memcpy(g_drawPanesCode, reinterpret_cast<void*>(OnIdleDrawPanes), 2);
  if (cfg.turbo) { const uint8_t jmp = 0xEB;  WriteCode(OnIdleFrameLimiter, &jmp, 1); }
  Log("net: turbo %d, slot hook %s, ProcessCommandPacket hook %s, sendto %p, recvfrom %p", int(cfg.turbo), g_origSlotPacket ? "on" : "FAILED",
      g_netProcess ? "on" : "FAILED", reinterpret_cast<void*>(g_sendTo), reinterpret_cast<void*>(g_recvFrom));
}

// Whether to render after this cycle: frames are drawn for a few ticks ahead of any capture so the panes are current.
bool WantDraw(int tick) {
  constexpr int Lead = DrawLead;
  if (tick <= Lead) return true;  // The panes must render at least once before drawing can be skipped safely.
  if (cfg.drawEvery > 0 && tick % cfg.drawEvery >= cfg.drawEvery - Lead) return true;
  if (cfg.shotEvery > 0 && (g_nextShot - tick <= Lead || (cfg.ticks > 0 && cfg.ticks - tick <= Lead))) return true;
  for (size_t i = g_scriptPos; i < g_script.size() && g_script[i].tick - tick <= Lead; ++i)
    if (g_script[i].verb == "shot" || IsInputVerb(g_script[i].verb) || (g_inputUsed && g_script[i].verb == "view"))
      return true;
  // Keep the panes current around inputs: UI views and controls are laid out as they are drawn.
  if (tick - g_lastInputTick <= Lead) return true;
  for (int t : g_pendingShots) if (t - tick <= Lead) return true;
  for (const auto& d : g_deferred) if (d.tick - tick <= Lead) return true;
  return false;
}

void __fastcall GameCycleHook(GameImpl* self, void*) {
  g_origGameCycle(self);
  int tick = self->tick_;
  PresentTick(tick);
  if (tick == g_lastTick) return;
  if (g_lastTick < 0) {
    // A recording is played with the real mouse and keys, unless its setup script has input verbs to run first.
    if (!g_record || g_inputUsed) ParkCursor();
    g_startTime = Now();
    Log("first game cycle: tick %d, map %dx%d, padding %d tiles", tick, GameMap::GetWidth(), GameMap::GetHeight(),
        PadTiles());
    WriteGameData((cfg.out + "\\gamedata.json").c_str());
    WriteAudioTables();
  }
  g_lastTick = tick;
  // Parallel runs share one wineserver, so they take the foreground from each other, and an inactive game ignores keys
  // and edge/arrow scrolling (TApp::OnActivateApp 0x4862F0). Keep it active: this runs before the frame is drawn.
  if (g_inputUsed && (!g_record || g_in.active) && !g_tApp.AppActive()) g_tApp.OnActivateApp(1);
  // A resume loads its checkpoint before anything of this cycle runs, then goes on as the cycle of the save's tick.
  static bool resumed = false;
  if (!cfg.resume.empty() && !resumed) {
    resumed = true;
    tick = Resume();
    if (tick < 0) { Quit("resume: the checkpoint didn't load");  return; }
  }

  // A `packet` whose cycle ran without handing the local player's slot to ProcessCommandPacket (a paused or playback
  // cycle, [GameImpl+0x9C] at 0x489F03, 0x4864A0 at 0x489FE0, the player's +0x4AC flag) never ran: say so.
  while (!g_packetDue.empty() && g_packetDue.begin()->first < tick) {
    Log("script: WARN tick %d: a `packet` for the cycle starting at tick %d never ran (dropped)", tick, g_packetDue.begin()->first);
    g_packetDue.erase(g_packetDue.begin());
  }
  for (size_t k = 0; k < g_deferred.size();) {
    if (g_deferred[k].tick <= tick) { const ScriptCmd d = g_deferred[k];  g_deferred.erase(g_deferred.begin() + k);  RunInputCmd(d, tick); }
    else ++k;
  }
  for (size_t k = 0; k < g_pendingShots.size();) {
    if (g_pendingShots[k] <= tick) { TakeShot(tick);  g_pendingShots.erase(g_pendingShots.begin() + k); }
    else ++k;
  }
  NameScan();
  while (g_scriptPos < g_script.size() && g_script[g_scriptPos].tick <= tick) {
    ScriptCmd& c = g_script[g_scriptPos++];
    if (c.verb == "savegame" || c.verb == "loadgame") {  // Once each: a load rewinds the script to the save's tick.
      if (c.ran) continue;
      c.ran = true;
    }
    RunScriptCmd(c, tick);
  }
  if (g_record && g_inputUsed) {
    // The virtual cursor and keys stand in for the real ones while the setup's input verbs (and their button releases)
    // are still to run, as in a replay; then the player's real mouse and keys take over.
    bool more = !g_deferred.empty();
    for (size_t i = g_scriptPos; i < g_script.size() && !more; ++i) more = IsInputVerb(g_script[i].verb);
    if (g_in.active != more) Log("input: tick %d: %s", tick, more ? "the setup's input verbs again (virtual cursor and keys)" : "the setup's input verbs are done; the real mouse and keys from now");
    g_in.active = more;
  }
  if (cfg.turbo) SetDrawing(WantDraw(tick));
  if (g_botTick) g_botTick(tick);
  AudioTick(tick);
  // GameImpl's speed (x 4) whenever it changes, for the `gamespeed` verb's timing in single player (harness.log).
  static int missionSpeed = -1;
  if (g_gameImpl.gameSpeed_ != missionSpeed) {
    Log("speed: tick %d GameImpl speed %d (was %d)", tick, g_gameImpl.gameSpeed_, missionSpeed);
    missionSpeed = g_gameImpl.gameSpeed_;
  }

  // Sample at tick 1 (telemetry) / the first fully drawn frame (screenshot), then at multiples of the interval.
  if (cfg.telemEvery > 0 && tick >= g_nextTelem) { WriteTelemetry(tick);  g_nextTelem = (tick / cfg.telemEvery + 1) * cfg.telemEvery; }
  if (cfg.traceEvery > 0 && tick >= g_nextTrace) { WriteTrace(tick);      g_nextTrace = (tick / cfg.traceEvery + 1) * cfg.traceEvery; }
  if (cfg.shotEvery  > 0 && tick >= g_nextShot)  { TakeShot(tick);        g_nextShot  = (tick / cfg.shotEvery  + 1) * cfg.shotEvery;  }
  SessionTick(tick);
  if (!g_saveLoadDue.empty()) {
    g_loaded = false;
    RunSaveLoad(tick);
    if (g_loaded) return;  // The loaded game's end and tick limit are checked after its next cycle.
  }
  // OP2H_CHECKPOINT: a run that stops while the game goes on (its tick limit, a stop event) saves it first.
  if (!cfg.checkpoint.empty() && self->gameTermReasons_ == GameTermReasons::Running &&
      (!g_stopReason.empty() || (cfg.ticks > 0 && tick >= cfg.ticks)))
    SaveCheckpoint(tick);
  if (!g_stopReason.empty()) Quit(("stop: " + g_stopReason).c_str());
  if (self->gameTermReasons_ != GameTermReasons::Running) Quit("mission ended");
  if (cfg.ticks > 0 && tick >= cfg.ticks) Quit("tick limit");
}

std::vector<int> g_campaignTechs;
bool g_campaignHasTechs = false;
using LoadCampaignStateFn = ibool(__thiscall*)(void*, GameStartInfo*);
LoadCampaignStateFn g_loadCampaignState = nullptr;

ibool __fastcall LoadCampaignStateHook(void* self, void*, GameStartInfo* start) {
  if (!g_campaignHasTechs) return g_loadCampaignState(self, start);
  // PrepareGame has loaded the new tree by now, unlike StartMissionHook. GetMissionResults saved the previous tree's
  // size and every index's player mask; the original restores it only into a tree exactly one topic larger (0x473830).
  auto& research = static_cast<MissionResults*>(start)->researchState;
  research.numTechs = std::max(0, g_research.numTechs_ - 1);
  for (int t = 0; t < research.numTechs; ++t) research.techNum[t] = uint8_t(t);
  for (int id : g_campaignTechs) {
    bool known = false;
    for (int t = 0; t < research.numTechs && !known; ++t) {
      if (g_research.ppTechInfos_[t]->techID == id) {
        research.playerBitVector[t].Set(0, true);
        known = true;
      }
    }
    if (!known) Log("campaign: topic %d is not in the previous mission's tech tree", id);
  }
  return g_loadCampaignState(self, start);
}

// Mission DLLs are linked to load at 0x11000000, and some lack base relocations for their own import-table reads (the
// OfficialDemos cde1, cdp1 and ml4_dm1: InitProc's `mov ecx, [0x11007234]` in cdp1 has none), so loaded anywhere else
// they crash as soon as they run. Native Windows can hand that address to the game's own allocations before OPU loads
// the mission (a 2.4 MB private block, measured on Windows 11 ARM64); Wine leaves it free. So on Windows the harness
// reserves it from InitMod, frees it when OPU loads any DLL linked at that address (LoadLibraryExW), and takes it again
// whenever that DLL is unloaded (a save's load unloads the mission and loads it again; so may a campaign's next mission).
// Not under Wine, where the reservation would move those allocations and with them heap pointers the recordings hold.
constexpr uintptr_t kMissionBase = 0x11000000;
void* g_missionBase = nullptr;
bool OnWine() {
  static const bool wine = GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != nullptr;
  return wine;
}
void HoldMissionBase() {
  void* const base = reinterpret_cast<void*>(kMissionBase);
  MEMORY_BASIC_INFORMATION m{};
  if (VirtualQuery(base, &m, sizeof(m)) && m.State != MEM_FREE) {
    Log("mission base: 0x11000000 is taken already (allocation %p); a mission DLL loaded elsewhere may crash", m.AllocationBase);
    return;
  }
  g_missionBase = VirtualAlloc(base, 0x400000, MEM_RESERVE, PAGE_NOACCESS);
}
// Whether the PE file `path` names is linked at kMissionBase (its optional header's ImageBase).
bool LinkedAtMissionBase(const wchar_t* path) {
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS32 nt{};
  DWORD n = 0;
  bool linked = ReadFile(f, &dos, sizeof(dos), &n, nullptr) && n == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
                SetFilePointer(f, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
                ReadFile(f, &nt, sizeof(nt), &n, nullptr) && n == sizeof(nt) && nt.Signature == IMAGE_NT_SIGNATURE &&
                nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && nt.OptionalHeader.ImageBase == kMissionBase;
  CloseHandle(f);
  return linked;
}
using LoadLibraryExWFn = HMODULE(WINAPI*)(const wchar_t*, HANDLE, DWORD);
LoadLibraryExWFn g_loadLibraryExW = nullptr;
HMODULE WINAPI MissionLoadHook(const wchar_t* name, HANDLE file, DWORD flags) {
  if (g_missionBase && name && LinkedAtMissionBase(name)) {
    VirtualFree(g_missionBase, 0, MEM_RELEASE);
    g_missionBase = nullptr;
  }
  return g_loadLibraryExW(name, file, flags);
}
// FreeLibrary as each patched module imported it (another mod's hook, if any, stays in the chain).
using FreeLibraryFn = BOOL(WINAPI*)(HMODULE);
FreeLibraryFn g_freeLibrary[3] = {};
template <int N>
BOOL WINAPI MissionFreeHook(HMODULE module) {
  const BOOL r = g_freeLibrary[N](module);
  if (reinterpret_cast<uintptr_t>(module) == kMissionBase) {
    MEMORY_BASIC_INFORMATION m{};
    if (VirtualQuery(module, &m, sizeof(m)) && m.State == MEM_FREE) HoldMissionBase();
  }
  return r;
}
// Replaces GameImpl::LoadDebugMap() on the no-shell startup path with starting the requested mission.
ibool __fastcall StartMissionHook(GameImpl* self, void*) {
  g_gsi = {};
  g_gsi.startupFlags.disastersEnabled = 1;
  g_gsi.startupFlags.dayNightEnabled  = 1;
  g_gsi.startupFlags.maxPlayers       = 1;
  g_gsi.startupFlags.missionType      = uint32(cfg.mission[0] == 't' || cfg.mission[0] == 'T' ? -3 : -1) & 0x1F;
  g_gsi.gameFlags.numPlayers          = 1;
  strncpy(g_gsi.scriptName, cfg.mission.c_str(), sizeof(g_gsi.scriptName) - 1);
  for (int i = 0; i < 6; ++i) {
    g_gsi.playerFlags[i].resources = cfg.difficulty;
    strncpy(g_gsi.playerName[i].str, "Harness", sizeof(g_gsi.playerName[i].str) - 1);
  }
  g_gsi.gameSpeed        = cfg.speed * 4;
  g_gsi.randomNumberSeed = cfg.seed;

  if (cfg.players > 1) {
    char path[MAX_PATH] = {};
    const char* filename = ResManager::GetInstance()->GetFilePath(cfg.mission.c_str(), path) ? path : cfg.mission.c_str();
    Log("multiplayer start: reading descriptors for %s", filename);
    AIModDesc* desc = MissionManager::GetModuleDesc(filename);
    Log("multiplayer start: DescBlock %p", (void*)desc);
    // 0x402780 takes its filename in ECX; MissionManager.h's declaration misses FASTCALL.
    using GetDescEx = ModDescEx*(__fastcall*)(const char*);
    const auto getDescEx = reinterpret_cast<GetDescEx>(0x402780);
    ModDescEx* ex = desc ? getDescEx(filename) : nullptr;
    Log("multiplayer start: DescBlockEx %p", (void*)ex);
    const int ai = ex ? ex->numMultiplayerAIs : 0;
    const int type = desc ? int(desc->descBlock.missionType) : 0;
    const int maxPlayers = desc ? int(desc->descBlock.numPlayers) : 0;
    // No DescBlockEx export means no AI slots: the game's own start (0x48958F) skips adding them on null.
    if (desc && !ex) Log("multiplayer start: no DescBlockEx, so no AI slots");
    // A harness-side sanity limit, not a rule taken from the game.
    if (desc && cfg.players > maxPlayers) {
      Log("FATAL: multiplayer start: %d human players requested but the descriptor allows at most %d; aborting", cfg.players,
          maxPlayers);
    }
    if (!desc || type < -8 || type > -4 || cfg.players > maxPlayers || cfg.players + ai > 6) {
      Log("multiplayer start: invalid descriptor or player count (desc %p, type %d, humans %d, AIs %d)", (void*)desc,
          type, cfg.players, ai);
      if (desc) MissionManager::FreeModuleDesc(desc);
      if (ex) MissionManager::FreeModuleDescEx(ex);
      if (g_log) fclose(g_log);
      TerminateProcess(GetCurrentProcess(), 2);
    }
    g_gsi.startupFlags.isMultiplayer = 1;
    g_gsi.startupFlags.moraleSteady = 1;
    g_gsi.startupFlags.disastersEnabled = type != -4 && type != -8;
    g_gsi.startupFlags.maxPlayers = desc->descBlock.numPlayers;
    g_gsi.startupFlags.missionType = uint32(type) & 0x1F;
    g_gsi.gameFlags.numPlayers = cfg.players;
    for (int i = 0; i < 6; ++i) {
      g_gsi.playerFlags[i].isHost = i == 0;
      g_gsi.playerFlags[i].isEden = i == 0 || (i & 1);
      g_gsi.playerFlags[i].resources = 0;
      g_gsi.playerFlags[i].color = (i + 1) % 6;
    }
    Log("multiplayer start: type %d max %d humans %d AIs %d, high resources, day/night on, morale steady on, "
        "disasters %d, initial vehicles %d, cheats %d, network bit %d", type, int(g_gsi.startupFlags.maxPlayers),
        cfg.players, ai, int(g_gsi.startupFlags.disastersEnabled), int(g_gsi.startupFlags.numInitialVehicles),
        int(g_gsi.startupFlags.cheatsEnabled), int(g_gsi.startupFlags.isMultiplayer));
    MissionManager::FreeModuleDesc(desc);
    if (ex) MissionManager::FreeModuleDescEx(ex);
  }

  // Campaign start: PrepareGame takes a MissionResults (GameStartInfo plus the carry-over) when isCampaign is set
  // and applies it with LoadCampaignState (0x4899C0) before the mission runs. OP2H_CAMPAIGN names a text file of
  // `key value` lines: mission (the finished campaign mission's number; default the DLL's minus one), food, common, rare, workers, scientists, kids, satellites (raw
  // SatelliteCounts word), moraleLevel, morale, event, techLevel, and `techs` followed by completed topic ids. No
  // `techs` line leaves numTechs 0, preserving older probe files.
  static MissionResults mr;
  GameStartInfo* start = &g_gsi;
  if (!cfg.campaign.empty()) {
    mr = {};
    memcpy(static_cast<GameStartInfo*>(&mr), &g_gsi, sizeof(GameStartInfo));
    g_campaignTechs.clear();
    g_campaignHasTechs = false;
    g_loadCampaignState = reinterpret_cast<LoadCampaignStateFn>(RelinkCall(0x48982F, reinterpret_cast<void*>(&LoadCampaignStateHook)));
    // MissionResults.missionType is the campaign mission just finished: the game starts the next one, picking the
    // colony's DLL by the local player's isEden flag (measured: number 2 with isEden 0 started p03).
    int number = atoi(cfg.mission.c_str() + 1) - 1;
    for (auto& f : mr.playerFlags) f.isEden = (cfg.mission[0] == 'e' || cfg.mission[0] == 'E');
    if (std::ifstream f{cfg.campaign}) {
      std::string line;
      while (std::getline(f, line)) {
        std::istringstream in{line};
        std::string k;
        if (!(in >> k)) continue;
        auto& ps = mr.playerStartInfo;
        auto& ms = mr.moraleState;
        if (k == "techs") {
          g_campaignHasTechs = true;
          int id;
          while (in >> id) g_campaignTechs.push_back(id);
          continue;
        }
        int v;
        if (!(in >> v)) continue;
        if      (k == "mission")     number = v;
        else if (k == "food")        ps.food = v;
        else if (k == "common")      ps.commonOre = v;
        else if (k == "rare")        ps.rareOre = v;
        else if (k == "workers")     ps.workers = v;
        else if (k == "scientists")  ps.scientists = v;
        else if (k == "kids")        ps.kids = v;
        else if (k == "satellites")  memcpy(&ps.satelliteCounts, &v, sizeof(v));
        else if (k == "moraleLevel") ms.moraleLevel = MoraleLevel(v);
        else if (k == "morale")      ms.morale = v;
        else if (k == "event")       ms.eventMoraleModifier = v;
        else if (k == "techLevel")   mr.researchState.techLevel = uint16(v);
        else Log("campaign: unknown key %s", k.c_str());
      }
    } else {
      Log("campaign: cannot open %s", cfg.campaign.c_str());
    }
    mr.startupFlags.isCampaign  = 1;
    mr.startupFlags.missionType = uint32(number) & 0x1F;
    mr.missionType              = TethysAPI::MissionType(number);
    mr.gameTermReason           = GameTermReasons::MissionAccomplished;
    Log("campaign start: mission %d, food %d common %d rare %d workers %d scientists %d kids %d, morale %d level %d",
        number, mr.playerStartInfo.food, mr.playerStartInfo.commonOre, mr.playerStartInfo.rareOre,
        mr.playerStartInfo.workers, mr.playerStartInfo.scientists, mr.playerStartInfo.kids, mr.moraleState.morale,
        int(mr.moraleState.moraleLevel));
    start = &mr;
  }
  // No-shell startup has no NetTransportLayer. The original playback branch keeps the GSI's player flags without it;
  // restore playback immediately (PrepareGame includes the mission's InitProc).
  const ibool oldPlayback = g_tApp.playback_;
  if (cfg.players > 1) g_tApp.playback_ = 1;
  const ibool ok = self->PrepareGame(start);
  if (cfg.players > 1) {
    g_tApp.playback_ = oldPlayback;
    Log("multiplayer start: used the original's offline playback path through PrepareGame");
  }
  Log("PrepareGame(%s) = %d, players %d", cfg.mission.c_str(), ok, self->numPlayers_);
  LogMusic("start");
  if (!ok) { Log("mission failed to start");  if (g_log) fclose(g_log);  TerminateProcess(GetCurrentProcess(), 2); }
  g_started = true;
  return ok;
}


// ---------------------------------------------------------------------------------------------------------------------
// Shell mode (OP2H_SHELL=1, `shell.py`): the game boots normally (intro video, title, menus, briefings) to
// measure the original's startup flow. The harness only watches and logs; shell.py drives the game with real X input
// and takes X screenshots of the whole (virtual) desktop. Events go to shell.jsonl, one JSON object per line, with
// `ms` since InitMod:
//   mci     every mciSendCommandA of OP2Shell.dll (movies: device type, file, window, rectangles, notifications)
//   notify  MM_MCINOTIFY as the shell receives it (1 = played to the end, 2 superseded, 4 aborted, 8 failed)
//   post    OP2Shell's PostMessage/SendMessage calls with message >= WM_USER: the shell's state machine (0x72B = play
//           the movie in +0x53C, then post +0x540; see notes/findings-shell.md)
//   modal / modal_end, modeless   IDlgWnd::DoModal/DoModeless (0x431A80/0x431BF0): dialog resource id and DLL, caller
//   activate_shell   TApp::ActivateShell (0x486DB0): back to the shell from a game, with the end reason
//   dialog  a dialog after WM_INITDIALOG, with every control (id, class, text, screen rectangle)
//   show / hide / destroy   top-level windows and dialogs
//   file    ResManager::OpenStream / GetFilePath asked by the shell (briefing RTFs, story text, music)
//   lib     LoadLibraryA from OP2Shell
//   input   key and mouse button messages as the game receives them (to confirm the driver's input landed)
//   start   TApp::StartSingleGame: the GameStartInfo the shell built
//   game    in-game milestones: the first cycle, every 500 ticks, the end reason changing
//   dump    windows and controls, on request (shell.py `dump`)
//   state   the game's tick, pause flag (TApp +0x178, 0x56E9E0) and count (GameFrame +0x1E0), the selection
//           (0x547210) with each selected unit's command, and the detail view's top-left corner (file pixels), on
//           request (shell.py `state`): running, at the end of a cycle a given number of ticks on; paused, at once
//           (when no cycle runs to write telemetry)
// shell.py's control file (ctl.txt in the output directory) is polled by a thread: `dump <tag>` logs every window of
// the game, `state <tag> [after]` the game state above, `quit` ends the process.

bool    g_shellMode = false;
FILE*   g_shell = nullptr;
int64_t g_shellT0 = 0;
CRITICAL_SECTION g_shellLock;
DWORD   g_mainThread = 0;

std::string JsonStr(const char* s, size_t max) {
  std::string out = "\"";
  for (size_t i = 0; s && s[i] && i < max; ++i) {
    const unsigned char c = s[i];
    if (c == '"' || c == '\\') { out += '\\';  out += char(c); }
    else if (c < 0x20) { char b[8];  snprintf(b, sizeof(b), "\\u%04x", c);  out += b; }
    else if (c >= 0x80) { char b[8];  snprintf(b, sizeof(b), "\\u%04x", c);  out += b; }  // Windows-1252, as code points.
    else out += char(c);
  }
  return out + "\"";
}

void ShellEvent(const char* fmt, ...) {
  if (!g_shell) return;
  va_list args, copy;
  va_start(args, fmt);
  va_copy(copy, args);
  std::string body(size_t(std::max(0, vsnprintf(nullptr, 0, fmt, copy))) + 1, '\0');
  va_end(copy);
  vsnprintf(body.data(), body.size(), fmt, args);
  va_end(args);
  body.pop_back();
  EnterCriticalSection(&g_shellLock);
  fprintf(g_shell, "{\"ms\":%.1f,%s}\n", (Now() - g_shellT0) * 1000.0 / Frequency(), body.c_str());
  fflush(g_shell);
  LeaveCriticalSection(&g_shellLock);
}

std::string ResName(const char* res) {
  if (IS_INTRESOURCE(res)) return std::to_string(unsigned(reinterpret_cast<uintptr_t>(res)));
  return JsonStr(res);
}

std::string WindowText(HWND h) {
  char buf[256] = {};
  DWORD_PTR n = 0;
  // Through a timeout: the dump thread asks for windows owned by the busy main thread.
  if (!SendMessageTimeoutA(h, WM_GETTEXT, sizeof(buf), reinterpret_cast<LPARAM>(buf), SMTO_ABORTIFHUNG, 500, &n)) buf[0] = 0;
  return buf;
}

// A combo box's list (`,"items":[...],"sel":N`) for a dump, so a driver can pick an entry by its text (shell.py `pick`:
// the lobby's scenario list). Asked through timeouts like WindowText; nothing for other classes.
std::string ComboItemsJson(HWND h, const char* cls) {
  if (_stricmp(cls, "ComboBox") != 0) return "";
  auto ask = [&](UINT msg, WPARAM w, LPARAM l) {
    DWORD_PTR r = 0;
    return SendMessageTimeoutA(h, msg, w, l, SMTO_ABORTIFHUNG, 500, &r) ? LRESULT(r) : LRESULT(CB_ERR);
  };
  const LRESULT n = ask(CB_GETCOUNT, 0, 0);
  if (n == CB_ERR) return "";
  std::string out = ",\"items\":[";
  for (LRESULT i = 0; i < std::min<LRESULT>(n, 300); ++i) {
    char item[256] = {};
    if (ask(CB_GETLBTEXTLEN, i, 0) < LRESULT(sizeof(item))) ask(CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(item));
    out += (i ? "," : "") + JsonStr(item);
  }
  return out + "],\"sel\":" + std::to_string(int(ask(CB_GETCURSEL, 0, 0)));
}

std::string WindowJson(HWND h) {
  char cls[64] = {};
  GetClassNameA(h, cls, sizeof(cls));
  RECT r = {};
  GetWindowRect(h, &r);
  const LONG style = GetWindowLongA(h, GWL_STYLE);
  char buf[1024];
  snprintf(buf, sizeof(buf), "\"hwnd\":\"%p\",\"cls\":%s,\"text\":%s,\"id\":%d,\"rect\":[%ld,%ld,%ld,%ld],\"style\":\"%08lx\","
           "\"visible\":%d,\"enabled\":%d,\"parent\":\"%p\"",
           (void*)h, JsonStr(cls).c_str(), JsonStr(WindowText(h).c_str()).c_str(), GetDlgCtrlID(h), r.left, r.top,
           r.right - r.left, r.bottom - r.top, (unsigned long)style, IsWindowVisible(h) ? 1 : 0, IsWindowEnabled(h) ? 1 : 0,
           (void*)GetParent(h));
  return std::string(buf) + ComboItemsJson(h, cls);
}

BOOL CALLBACK AddChildJson(HWND c, LPARAM p) {
  auto& s = *reinterpret_cast<std::string*>(p);
  s += (s.empty() ? "{" : ",{") + WindowJson(c) + "}";
  return TRUE;
}

std::string ChildrenJson(HWND h) {
  std::string out;
  EnumChildWindows(h, &AddChildJson, reinterpret_cast<LPARAM>(&out));
  return "[" + out + "]";
}

bool IsDialog(HWND h) { char cls[16] = {};  GetClassNameA(h, cls, sizeof(cls));  return strcmp(cls, "#32770") == 0; }
bool IsTopOrDialog(HWND h) { return !(GetWindowLongA(h, GWL_STYLE) & WS_CHILD) || IsDialog(h); }

LRESULT CALLBACK ShellCallWndRet(int code, WPARAM w, LPARAM l) {
  if (code == HC_ACTION) {
    const auto* m = reinterpret_cast<const CWPRETSTRUCT*>(l);
    switch (m->message) {
    case WM_INITDIALOG:
      ShellEvent("\"ev\":\"dialog\",%s,\"controls\":%s", WindowJson(m->hwnd).c_str(), ChildrenJson(m->hwnd).c_str());
      break;
    case WM_SHOWWINDOW:
      if (IsTopOrDialog(m->hwnd)) ShellEvent("\"ev\":\"%s\",%s", m->wParam ? "show" : "hide", WindowJson(m->hwnd).c_str());
      break;
    case WM_DESTROY:
      if (IsTopOrDialog(m->hwnd)) ShellEvent("\"ev\":\"destroy\",%s", WindowJson(m->hwnd).c_str());
      break;
    }
  }
  return CallNextHookEx(nullptr, code, w, l);
}

LRESULT CALLBACK ShellGetMsg(int code, WPARAM w, LPARAM l) {
  if (code == HC_ACTION && w == PM_REMOVE) {
    const auto* m = reinterpret_cast<const MSG*>(l);
    char cls[64] = {};
    switch (m->message) {
    case MM_MCINOTIFY:
      ShellEvent("\"ev\":\"notify\",\"flags\":%u,\"dev\":%u,\"hwnd\":\"%p\"", unsigned(m->wParam), unsigned(m->lParam), (void*)m->hwnd);
      break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_LBUTTONUP:
      GetClassNameA(m->hwnd, cls, sizeof(cls));
      ShellEvent("\"ev\":\"input\",\"msg\":\"%s\",\"w\":%u,\"x\":%d,\"y\":%d,\"hwnd\":\"%p\",\"cls\":%s,\"id\":%d",
                 m->message == WM_KEYDOWN ? "keydown" : m->message == WM_SYSKEYDOWN ? "syskeydown" :
                 m->message == WM_LBUTTONDOWN ? "ldown" : m->message == WM_LBUTTONUP ? "lup" : "rdown",
                 unsigned(m->wParam), short(LOWORD(m->lParam)), short(HIWORD(m->lParam)), (void*)m->hwnd,
                 JsonStr(cls).c_str(), GetDlgCtrlID(m->hwnd));
      break;
    }
  }
  return CallNextHookEx(nullptr, code, w, l);
}

// ---- Hooks on OP2Shell.dll's imports (patched whenever op2ext reports the shell loaded, see RunMod).

// Patches `fn` imported from `dll` in `mod`'s import table; returns the previous target, or nullptr if the import isn't
// there or already points at `hook` (the shell can be loaded again into the same place).
void* PatchImport(HMODULE mod, const char* dll, const char* fn, void* hook) {
  auto* base = reinterpret_cast<uint8_t*>(mod);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
    if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->OriginalFirstThunk);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
      const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(ibn->Name), fn) != 0) continue;
      if (reinterpret_cast<void*>(slots->u1.Function) == hook) return nullptr;
      return PatchIat(reinterpret_cast<uintptr_t>(&slots->u1.Function), hook);
    }
  }
  Log("shell: import %s!%s not found", dll, fn);
  return nullptr;
}

// ---- Mission DLLs' own modal dialogs. Some missions (Pursued, Rising From the Ashes) open a briefing box from InitProc
// with user32's DialogBoxParamA, which would block an unattended run forever (they ignore its result). OPU loads mission
// DLLs itself (its hook at 0x402B44), so instead of their imports the harness watches window creation on the game's
// thread: a dialog whose instance is a mission DLL (it exports InitProc) isn't created, and DialogBoxParamA returns -1
// at once, as if the player had closed it.

LRESULT CALLBACK MissionDialogCbt(int code, WPARAM w, LPARAM l) {
  if (code == HCBT_CREATEWND) {
    const auto* cs = reinterpret_cast<const CBT_CREATEWNDA*>(l)->lpcs;
    const bool dialog = cs && (reinterpret_cast<uintptr_t>(cs->lpszClass) == 0x8002 ||
                               (HIWORD(reinterpret_cast<uintptr_t>(cs->lpszClass)) && strcmp(cs->lpszClass, "#32770") == 0));
    HMODULE mod = cs ? reinterpret_cast<HMODULE>(cs->hInstance) : nullptr;
    if (dialog && mod && mod != GetModuleHandleA(nullptr) && GetProcAddress(mod, "InitProc")) {
      char path[MAX_PATH] = {};
      GetModuleFileNameA(mod, path, sizeof(path) - 1);
      Log("dialog: a mission dialog box (%s, \"%s\") not opened", path, cs->lpszName ? cs->lpszName : "");
      return 1;
    }
  }
  return CallNextHookEx(nullptr, code, w, l);
}

const char* MciName(UINT m) {
  switch (m) {
  case MCI_OPEN: return "open";      case MCI_CLOSE: return "close";    case MCI_PLAY: return "play";
  case MCI_STOP: return "stop";      case MCI_PAUSE: return "pause";    case MCI_SEEK: return "seek";
  case MCI_SET: return "set";        case MCI_STATUS: return "status";  case MCI_WINDOW: return "window";
  case MCI_PUT: return "put";        case MCI_WHERE: return "where";    case MCI_REALIZE: return "realize";
  case MCI_UPDATE: return "update";  case MCI_GETDEVCAPS: return "getdevcaps";  case MCI_INFO: return "info";
  case MCI_CUE: return "cue";        case MCI_RESUME: return "resume";
  default: return "?";
  }
}

using MciSendCommandFn = MCIERROR(WINAPI*)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
MciSendCommandFn g_mciSendCommand = nullptr;
MCIERROR WINAPI MciHook(MCIDEVICEID id, UINT msg, DWORD_PTR flags, DWORD_PTR parms) {
  const MCIERROR r = g_mciSendCommand(id, msg, flags, parms);
  char extra[600] = "";
  if (msg == MCI_OPEN && parms) {
    const auto* p = reinterpret_cast<const MCI_OPEN_PARMSA*>(parms);
    const std::string type = !(flags & MCI_OPEN_TYPE) ? "null"
                           : (flags & MCI_OPEN_TYPE_ID) ? std::to_string(uintptr_t(p->lpstrDeviceType)) : JsonStr(p->lpstrDeviceType);
    snprintf(extra, sizeof(extra), ",\"type\":%s,\"file\":%s,\"newdev\":%u", type.c_str(),
             (flags & MCI_OPEN_ELEMENT) ? JsonStr(p->lpstrElementName).c_str() : "null", unsigned(p->wDeviceID));
  } else if (msg == MCI_PLAY && parms) {
    const auto* p = reinterpret_cast<const MCI_PLAY_PARMS*>(parms);
    snprintf(extra, sizeof(extra), ",\"from\":%ld,\"to\":%ld,\"callback\":\"%p\"", (flags & MCI_FROM) ? long(p->dwFrom) : -1L,
             (flags & MCI_TO) ? long(p->dwTo) : -1L, (flags & MCI_NOTIFY) ? (void*)p->dwCallback : nullptr);
  } else if ((msg == MCI_PUT || msg == MCI_WHERE) && parms) {
    const auto* p = reinterpret_cast<const MCI_ANIM_RECT_PARMS*>(parms);
    snprintf(extra, sizeof(extra), ",\"rect\":[%ld,%ld,%ld,%ld]", p->rc.left, p->rc.top, p->rc.right, p->rc.bottom);
  } else if (msg == MCI_WINDOW && parms) {
    const auto* p = reinterpret_cast<const MCI_ANIM_WINDOW_PARMSA*>(parms);
    snprintf(extra, sizeof(extra), ",\"window\":\"%p\",\"show\":%u", (flags & MCI_ANIM_WINDOW_HWND) ? (void*)p->hWnd : nullptr,
             (flags & MCI_ANIM_WINDOW_STATE) ? unsigned(p->nCmdShow) : 999u);
  } else if (msg == MCI_STATUS && parms) {
    const auto* p = reinterpret_cast<const MCI_STATUS_PARMS*>(parms);
    snprintf(extra, sizeof(extra), ",\"item\":%lu,\"value\":%lu", (unsigned long)p->dwItem, (unsigned long)p->dwReturn);
  } else if (msg == MCI_SET && parms) {
    const auto* p = reinterpret_cast<const MCI_SET_PARMS*>(parms);
    snprintf(extra, sizeof(extra), ",\"timeFormat\":%lu", (flags & MCI_SET_TIME_FORMAT) ? (unsigned long)p->dwTimeFormat : 0ul);
  }
  ShellEvent("\"ev\":\"mci\",\"cmd\":\"%s\",\"msg\":\"%#x\",\"dev\":%u,\"flags\":\"%#lx\",\"ret\":%lu%s", MciName(msg), msg,
             unsigned(id), (unsigned long)flags, (unsigned long)r, extra);
  return r;
}

using PostMessageFn = BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
using SendMessageFn = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
PostMessageFn g_postMessage = nullptr;
SendMessageFn g_sendMessage = nullptr;
BOOL WINAPI PostMessageHook(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m >= WM_USER) ShellEvent("\"ev\":\"post\",\"msg\":\"%#x\",\"w\":%u,\"l\":%ld,\"hwnd\":\"%p\"", m, unsigned(w), long(l), (void*)h);
  return g_postMessage(h, m, w, l);
}
LRESULT WINAPI SendMessageHook(HWND h, UINT m, WPARAM w, LPARAM l) {
  // Below 0x7FF: the shell's own messages; dialog/control messages (BM_*, LB_*...) start at WM_USER too, so skip the
  // common control ranges.
  if (m >= 0x700 && m < 0x800)
    ShellEvent("\"ev\":\"send\",\"msg\":\"%#x\",\"w\":%u,\"l\":%ld,\"hwnd\":\"%p\"", m, unsigned(w), long(l), (void*)h);
  return g_sendMessage(h, m, w, l);
}

using DoModalFn    = int(__thiscall*)(void*, const char*, HINSTANCE);
using DoModelessFn = int(__thiscall*)(void*, const char*, HINSTANCE, HWND);
DoModalFn    g_doModal = nullptr;
DoModelessFn g_doModeless = nullptr;
// `module`: the resource DLL (op2shres.dll for the shell's dialogs, out2res.dll for the game's); `at`: the caller.
std::string ModuleName(HINSTANCE h) {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(h, path, sizeof(path));
  const char* slash = strrchr(path, '\\');
  return slash ? slash + 1 : path;
}
int __fastcall DoModalHook(void* self, void*, const char* res, HINSTANCE inst) {
  void* at = __builtin_return_address(0);
  ShellEvent("\"ev\":\"modal\",\"res\":%s,\"module\":%s,\"at\":\"%p\",\"vtbl\":\"%p\"", ResName(res).c_str(),
             JsonStr(ModuleName(inst).c_str()).c_str(), at, *reinterpret_cast<void**>(self));
  const int r = g_doModal(self, res, inst);
  ShellEvent("\"ev\":\"modal_end\",\"res\":%s,\"ret\":%d", ResName(res).c_str(), r);
  return r;
}
int __fastcall DoModelessHook(void* self, void*, const char* res, HINSTANCE inst, HWND parent) {
  void* at = __builtin_return_address(0);
  ShellEvent("\"ev\":\"modeless\",\"res\":%s,\"module\":%s,\"at\":\"%p\",\"vtbl\":\"%p\"", ResName(res).c_str(),
             JsonStr(ModuleName(inst).c_str()).c_str(), at, *reinterpret_cast<void**>(self));
  return g_doModeless(self, res, inst, parent);
}

// TApp::ActivateShell (0x486DB0): the game hands control back to the shell with the end reason and the results.
using ActivateShellFn = int(__thiscall*)(void*, int, MissionResults*);
ActivateShellFn g_activateShell = nullptr;
int g_shellGameTick = -1, g_shellEnd = 0;  // The shell's current game's tick; -1 outside a game.

// A status message in a shell-mode game (AddMessage 0x439070): GameImpl's tick, the SoundID, the place (internal pixels;
// a chat line's is (-2, -1), 0x40FE14) and the text as shown ("NAME: text" for a chat line, 0x40FDFB). op2-missionhost
// compares a network game's chat lines with these.
void ShellMessage(int x, int y, const char* text, int sound) {
  if (!g_shell || g_shellGameTick < 0) return;
  ShellEvent("\"ev\":\"game\",\"what\":\"msg\",\"tick\":%d,\"sound\":%d,\"x\":%d,\"y\":%d,\"text\":%s", g_gameImpl.tick_, sound, x, y,
             JsonStr(text ? text : "").c_str());
}
int g_shellSpeed = -1;  // GameImpl's speed last written as a `speed` event (-1: none yet in this game).

int __fastcall ActivateShellHook(void* self, void*, int reason, MissionResults* r) {
  g_shellGameTick = -1;  // The game's panes and units go now: `state` probes stop reading them.
  ShellEvent("\"ev\":\"activate_shell\",\"reason\":%d,\"at\":\"%p\",\"results\":%d,\"mission\":%d,\"resultReason\":%d", reason,
             __builtin_return_address(0), r ? 1 : 0, r ? int(r->missionType) : 0, r ? int(r->gameTermReason) : 0);
  return g_activateShell(self, reason, r);
}

using OpenStreamFn  = void*(__thiscall*)(void*, const char*);
using GetFilePathFn = int(__thiscall*)(void*, const char*, char*);
OpenStreamFn  g_openStream = nullptr;
GetFilePathFn g_getFilePath = nullptr;
void* __fastcall OpenStreamHook(void* self, void*, const char* name) {
  void* s = g_openStream(self, name);
  ShellEvent("\"ev\":\"file\",\"fn\":\"OpenStream\",\"name\":%s,\"found\":%d", JsonStr(name).c_str(), s ? 1 : 0);
  return s;
}
int __fastcall GetFilePathHook(void* self, void*, const char* name, char* path) {
  const int r = g_getFilePath(self, name, path);
  ShellEvent("\"ev\":\"file\",\"fn\":\"GetFilePath\",\"name\":%s,\"found\":%d,\"path\":%s", JsonStr(name).c_str(), r,
             JsonStr(r ? path : "").c_str());
  return r;
}

using LoadLibraryFn = HMODULE(WINAPI*)(const char*);
LoadLibraryFn g_loadLibrary = nullptr;
HMODULE WINAPI LoadLibraryHook(const char* name) {
  HMODULE h = g_loadLibrary(name);
  ShellEvent("\"ev\":\"lib\",\"name\":%s,\"ok\":%d", JsonStr(name).c_str(), h ? 1 : 0);
  return h;
}

using StartSingleGameFn = int(__thiscall*)(void*, GameStartInfo*);
StartSingleGameFn g_startSingleGame = nullptr;
int __fastcall StartSingleGameHook(void* self, void*, GameStartInfo* gsi) {
  const auto& f = gsi->startupFlags;
  const bool campaign = f.isCampaign;
  ShellEvent("\"ev\":\"start\",\"script\":%s,\"missionType\":%d,\"isCampaign\":%d,\"speed\":%d,\"seed\":%d,"
             "\"difficulty\":%d,\"eden\":%d,\"cheats\":%d,\"daynight\":%d,\"disasters\":%d,\"mission\":%d",
             JsonStr(gsi->scriptName).c_str(), int(f.missionType), campaign ? 1 : 0, gsi->gameSpeed, int(gsi->randomNumberSeed),
             int(gsi->playerFlags[0].resources), int(gsi->playerFlags[0].isEden), int(f.cheatsEnabled), int(f.dayNightEnabled),
             int(f.disastersEnabled), campaign ? int(static_cast<MissionResults*>(gsi)->missionType) : -1);
  return g_startSingleGame(self, gsi);
}

void PatchShellImports() {
  HMODULE shell = GetModuleHandleA("OP2Shell.dll");
  if (!shell) { Log("shell: OP2Shell.dll not loaded");  return; }
  ShellEvent("\"ev\":\"shell_loaded\",\"base\":\"%p\"", (void*)shell);
  auto patch = [&](auto& orig, const char* dll, const char* fn, void* hook) {
    if (void* prev = PatchImport(shell, dll, fn, hook)) orig = reinterpret_cast<std::remove_reference_t<decltype(orig)>>(prev);
  };
  patch(g_mciSendCommand, "WINMM.dll", "mciSendCommandA", reinterpret_cast<void*>(&MciHook));
  patch(g_postMessage, "USER32.dll", "PostMessageA", reinterpret_cast<void*>(&PostMessageHook));
  patch(g_sendMessage, "USER32.dll", "SendMessageA", reinterpret_cast<void*>(&SendMessageHook));
  patch(g_loadLibrary, "KERNEL32.dll", "LoadLibraryA", reinterpret_cast<void*>(&LoadLibraryHook));
  patch(g_openStream, "Outpost2.exe", "?OpenStream@ResManager@@QAEPAVStreamIO@@PBD@Z", reinterpret_cast<void*>(&OpenStreamHook));
  patch(g_getFilePath, "Outpost2.exe", "?GetFilePath@ResManager@@QAEHPBDPAD@Z", reinterpret_cast<void*>(&GetFilePathHook));
  patch(g_startSingleGame, "Outpost2.exe", "?StartSingleGame@TApp@@QAEHPAUGameStartInfo@@@Z",
        reinterpret_cast<void*>(&StartSingleGameHook));
}

// ---- In game: the cycle hook only logs (and runs a tick script, e.g. a mission's winning orders); no turbo, no quit.

void ProbeTick(int tick);

void __fastcall ShellCycleHook(GameImpl* self, void*) {
  g_origGameCycle(self);
  const int tick = self->tick_;
  PresentTick(tick);
  if (tick == g_shellGameTick) return;
  if (tick < g_shellGameTick || g_shellGameTick < 0) {
    // A new game: reset the per-game state.
    g_scriptPos = 0;
    g_names.clear(), g_goneNames.clear(), g_pendingNames.clear(), g_nameSeen.clear();
    g_shellEnd = 0;
    g_shellSpeed = -1;
    g_nextTelem = 0;
    RECT r = {};
    GetWindowRect(g_gameFrame.hWnd_, &r);
    ShellEvent("\"ev\":\"game\",\"what\":\"first_cycle\",\"tick\":%d,\"map\":[%d,%d],\"frame\":[%ld,%ld,%ld,%ld]", tick,
               GameMap::GetWidth(), GameMap::GetHeight(), r.left, r.top, r.right - r.left, r.bottom - r.top);
    LogMusic("start");
    if (g_net) WriteStartInfo(cfg.out + "\\start_info.json");
  }
  g_shellGameTick = tick;
  if (g_net && cfg.turbo) SetDrawing(tick % 256 < 4 || (g_netDrawFrom > 0 && tick >= g_netDrawFrom));
  const bool hadCommands = g_scriptPos < g_script.size();
  NameScan();
  g_netScript = g_net;
  while (g_scriptPos < g_script.size() && g_script[g_scriptPos].tick <= tick) RunScriptCmd(g_script[g_scriptPos++], tick);
  g_netScript = false;
  if (hadCommands && g_scriptPos == g_script.size()) ShellEvent("\"ev\":\"script\",\"what\":\"done\",\"tick\":%d", tick);
  AudioTick(tick);
  if (cfg.telemEvery > 0 && tick >= g_nextTelem) { WriteTelemetry(tick);  g_nextTelem = (tick / cfg.telemEvery + 1) * cfg.telemEvery; }
  ProbeTick(tick);
  if (tick % 500 == 0) ShellEvent("\"ev\":\"game\",\"what\":\"tick\",\"tick\":%d", tick);
  // GameImpl::gameSpeed_, the frame limiter's ticks per second (set at the start from GameStartInfo +0xAE, 0x4893CD;
  // by a ctGameOpt 0x14 on every machine, 0x422DBE).
  // The first cycle of each game (the new-game block above resets g_shellSpeed: a new game, or a load that goes back
  // in time) always writes one.
  if (g_gameImpl.gameSpeed_ != g_shellSpeed)
    ShellEvent("\"ev\":\"game\",\"what\":\"speed\",\"tick\":%d,\"speed\":%d", tick, g_shellSpeed = g_gameImpl.gameSpeed_);
  const int end = int(self->gameTermReasons_);
  if (end != g_shellEnd) {
    ShellEvent("\"ev\":\"game\",\"what\":\"end\",\"tick\":%d,\"reason\":%d", tick, end);
    g_shellEnd = end;
  }
}

// ---- Control file thread.

std::string g_shellCtl;
HWND g_focusWindow = nullptr;
BOOL CALLBACK AddWindowJson(HWND h, LPARAM p) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != GetCurrentProcessId()) return TRUE;
  auto& s = *reinterpret_cast<std::string*>(p);
  s += (s.empty() ? "{" : ",{") + WindowJson(h) + ",\"children\":" + ChildrenJson(h) + "}";
  return TRUE;
}

void DumpWindows(const std::string& tag) {
  std::string out;
  EnumWindows(&AddWindowJson, reinterpret_cast<LPARAM>(&out));
  ShellEvent("\"ev\":\"dump\",\"tag\":%s,\"windows\":[%s]", JsonStr(tag.c_str()).c_str(), out.c_str());
}

// `state` probes. Running, the game thread answers one at the end of the cycle `after` ticks past the first cycle that
// sees the request, so what it reads is one cycle's state and how far the game got is counted in ticks, not wall time.
// Paused (no cycle runs) or outside a game, the control thread answers at once: nothing moves while paused, and input
// doesn't reach the game then (IWnd::CallFilters 0x431740), so the read is consistent.
struct Probe { std::string tag;  int after = 0, at = -1;  bool pending = false; };
Probe g_probe;  // Guarded by g_shellLock.

void DumpState(const std::string& tag) {
  const bool inGame = g_shellGameTick >= 0;
  // The cursor's handle (GetCursorInfo, the X pointer's shape as Wine shows it): only compared within a run.
  CURSORINFO ci = { sizeof(ci) };
  GetCursorInfo(&ci);
  ShellEvent("\"ev\":\"state\",\"tag\":%s,\"tick\":%d,\"paused\":%d,\"pauseCount\":%d,\"active\":%d,\"iconic\":%d,"
             "\"cursor\":\"%p\"%s", JsonStr(tag.c_str()).c_str(), g_shellGameTick, inGame ? int(g_tApp.AppPaused()) : 0,
             inGame ? g_gameFrame.gamePausedCount_ : 0, inGame ? int(g_tApp.AppActive()) : 0,
             inGame && IsIconic(g_gameFrame.hWnd_), (void*)ci.hCursor, inGame ? InputStateJson(true).c_str() : "");
}

// The control thread's side: answer now if no cycle will, else leave it to ProbeTick.
void RequestProbe(const std::string& tag, int after) {
  EnterCriticalSection(&g_shellLock);
  g_probe = { tag, after, -1, true };
  LeaveCriticalSection(&g_shellLock);
}

void ProbeNowIfIdle() {
  EnterCriticalSection(&g_shellLock);
  const bool idle = g_probe.pending && (g_shellGameTick < 0 || g_tApp.AppPaused());
  const std::string tag = g_probe.tag;
  if (idle) g_probe.pending = false;
  LeaveCriticalSection(&g_shellLock);
  if (idle) DumpState(tag);
}

void ProbeTick(int tick) {
  EnterCriticalSection(&g_shellLock);
  bool now = false;
  if (g_probe.pending) {
    if (g_probe.at < 0) g_probe.at = tick + g_probe.after;
    now = tick >= g_probe.at;
    if (now) g_probe.pending = false;
  }
  const std::string tag = g_probe.tag;
  LeaveCriticalSection(&g_shellLock);
  if (now) DumpState(tag);
}

DWORD WINAPI ShellControlThread(void*) {
  for (;;) {
    Sleep(25);
    PresentClock();
    ProbeNowIfIdle();
    FILE* f = fopen(g_shellCtl.c_str(), "r");
    if (!f) continue;
    char line[256];
    std::vector<std::string> cmds;
    while (fgets(line, sizeof(line), f)) cmds.emplace_back(line);
    fclose(f);
    DeleteFileA(g_shellCtl.c_str());
    for (auto c : cmds) {
      while (!c.empty() && (c.back() == '\n' || c.back() == '\r')) c.pop_back();
      if (c.rfind("dump", 0) == 0) DumpWindows(c.size() > 5 ? c.substr(5) : "");
      else if (c.rfind("state", 0) == 0) {  // state <tag> [after]
        char tag[200] = "";
        int after = 0;
        sscanf(c.c_str() + 5, " %199s %d", tag, &after);
        RequestProbe(tag, after);
      }
      else if (c == "unfocus" || c == "minimize" || c == "restore") {
        const HWND h = g_gameFrame.hWnd_;
        if (c == "unfocus") {
          if (!g_focusWindow) g_focusWindow = CreateWindowExA(0, "Static", "Harness focus target", WS_OVERLAPPEDWINDOW,
                                                             8, 8, 200, 100, nullptr, nullptr, nullptr, nullptr);
          ShowWindow(g_focusWindow, SW_SHOW);
          SetForegroundWindow(g_focusWindow);
        } else {
          if (g_focusWindow) { DestroyWindow(g_focusWindow);  g_focusWindow = nullptr; }
          ShowWindow(h, c == "minimize" ? SW_MINIMIZE : SW_RESTORE);
          if (c == "restore") { SetForegroundWindow(h);  SetActiveWindow(h);  SetFocus(h); }
        }
        ShellEvent("\"ev\":\"window\",\"what\":%s,\"active\":%d,\"iconic\":%d", JsonStr(c.c_str()).c_str(),
                   int(g_tApp.AppActive()), IsIconic(h));
      }
      else if (c == "quit") {
        ShellEvent("\"ev\":\"quit\"");
        ClosePresents();
        if (g_telem) fclose(g_telem);
        if (g_audio) fclose(g_audio);
        if (g_log) fclose(g_log);
        TerminateProcess(GetCurrentProcess(), 0);
      }
    }
  }
}

void ShellInit() {
  InitializeCriticalSection(&g_shellLock);
  g_shellT0 = Now();
  g_mainThread = GetCurrentThreadId();
  cfg.out        = Env("OP2H_OUT", ".");
  cfg.script     = Env("OP2H_SCRIPT");
  cfg.telemEvery = EnvInt("OP2H_TELEM_EVERY", 0);
  cfg.presents   = EnvInt("OP2H_PRESENTS", 0) != 0;
  g_log   = fopen((cfg.out + "\\harness.log").c_str(), "w");
  g_shell = fopen((cfg.out + "\\shell.jsonl").c_str(), "w");
  g_telem = cfg.telemEvery > 0 ? fopen((cfg.out + "\\telemetry.jsonl").c_str(), "w") : nullptr;
  g_audio = EnvInt("OP2H_AUDIO", 0) ? fopen((cfg.out + "\\audio.jsonl").c_str(), "w") : nullptr;
  g_shellCtl = cfg.out + "\\ctl.txt";
  Log("harness: shell mode");
  InstallPresentHook();

  // A private copy of the settings, as in mission runs; here sound and the movies stay as configured (OP2H_INI_<key>
  // overrides a [Game] key, e.g. OP2H_INI_IntroAVI=1).
  const std::string ini = cfg.out + "\\outpost2.ini";
  if (CopyFileA(g_configFile.iniPath_, ini.c_str(), FALSE) && ini.size() < sizeof(g_configFile.iniPath_)) {
    strcpy(g_configFile.iniPath_, ini.c_str());
    for (const char* key : { "IntroAVI", "ShowMovies", "Music", "Sound", "GameSpeed", "Demo", "MusicVol", "SoundVol", "Name" }) {
      const std::string v = Env((std::string("OP2H_INI_") + key).c_str());
      if (!v.empty()) WritePrivateProfileStringA("Game", key, v.c_str(), ini.c_str());
    }
    Log("config: using %s", ini.c_str());
  }
  JumpTo(FnOP2MessageBox, reinterpret_cast<void*>(&MessageBoxHook));
  // Shell-mode games log every status message to shell.jsonl (`msg` events: a network game's chat lines, ShellMessage).
  if (g_audio) InstallAudioHooks();
  else InstallMessageHook();
  g_origGameCycle = reinterpret_cast<GameCycleFn>(RelinkCall(CallProcessGameCycle, reinterpret_cast<void*>(&ShellCycleHook)));
  // Every dialog of the shell and the game goes through IDlgWnd::DoModal/DoModeless in Outpost2.exe.
  static const uint8_t doModal[]    = { 0x83, 0xEC, 0x04, 0xA1, 0xAC, 0xE8, 0x56, 0x00 };  // sub esp,4; mov eax,[56E8ACh]
  static const uint8_t doModeless[] = { 0x83, 0xEC, 0x10, 0xA1, 0xAC, 0xE8, 0x56, 0x00 };  // sub esp,10h; mov eax,[56E8ACh]
  static const uint8_t activate[]   = { 0x83, 0xEC, 0x2C, 0x53, 0x56 };  // sub esp,2Ch; push ebx; push esi
  g_doModal       = reinterpret_cast<DoModalFn>(HookEntry(0x431A80, doModal, 8, reinterpret_cast<void*>(&DoModalHook)));
  g_doModeless    = reinterpret_cast<DoModelessFn>(HookEntry(0x431BF0, doModeless, 8, reinterpret_cast<void*>(&DoModelessHook)));
  g_activateShell = reinterpret_cast<ActivateShellFn>(HookEntry(0x486DB0, activate, 5, reinterpret_cast<void*>(&ActivateShellHook)));
  SetWindowsHookExA(WH_CALLWNDPROCRET, &ShellCallWndRet, nullptr, g_mainThread);
  SetWindowsHookExA(WH_GETMESSAGE, &ShellGetMsg, nullptr, g_mainThread);
  if (!cfg.script.empty()) LoadScript(cfg.script);
  if (EnvInt("OP2H_NET", 0)) { g_net = true;  NetInit(); }
  CreateThread(nullptr, 0, &ShellControlThread, nullptr, 0, nullptr);
  ShellEvent("\"ev\":\"init\",\"screen\":[%d,%d]", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

} // namespace

extern "C" __declspec(dllexport) void InitMod(const char* /*iniSectionName*/) {
  if (EnvInt("OP2H_SHELL", 0)) { g_shellMode = true;  ShellInit();  return; }  // Normal boot, watched (shell.py).
  cfg.mission = Env("OP2H_MISSION");
  if (cfg.mission.empty()) return;  // Not a harness run: stay completely inert.

  cfg.out        = Env("OP2H_OUT", ".");
  cfg.script     = Env("OP2H_SCRIPT");
  cfg.bot        = Env("OP2H_BOT");
  cfg.campaign   = Env("OP2H_CAMPAIGN");
  cfg.resume     = Env("OP2H_RESUME");
  cfg.checkpoint = Env("OP2H_CHECKPOINT");
  cfg.ticks     = EnvInt("OP2H_TICKS", 0);
  cfg.shotEvery  = EnvInt("OP2H_SHOT_EVERY", 0);
  cfg.telemEvery = EnvInt("OP2H_TELEM_EVERY", 100);
  cfg.speed      = EnvInt("OP2H_SPEED", 10);
  cfg.seed       = EnvInt("OP2H_SEED", 1);
  cfg.difficulty = EnvInt("OP2H_DIFFICULTY", 1);
  cfg.players    = EnvInt("OP2H_PLAYERS", 1);
  cfg.turbo      = EnvInt("OP2H_TURBO", 1) != 0;
  cfg.presents   = EnvInt("OP2H_PRESENTS", 0) != 0;
  cfg.drawEvery  = EnvInt("OP2H_DRAW_EVERY", 0);
  cfg.traceEvery = EnvInt("OP2H_TRACE_EVERY", 0);
  // --trace-from: no trace lines before this tick (full raw traces reach 500 MB), rounded up to a multiple of
  // --trace-every so the lines that remain are the ones a full trace (and op2-sim's) has.
  if (const int from = EnvInt("OP2H_TRACE_FROM", 0); from > 0 && cfg.traceEvery > 0)
    g_nextTrace = (from + cfg.traceEvery - 1) / cfg.traceEvery * cfg.traceEvery;
  g_traceRaw     = EnvInt("OP2H_TRACE_RAW", 0) != 0;
  g_traceMap     = EnvInt("OP2H_TRACE_MAP", 0) != 0;
  g_eco          = EnvInt("OP2H_ECO", 0) != 0;
  cfg.width      = EnvInt("OP2H_WIDTH", 1024);
  cfg.height     = EnvInt("OP2H_HEIGHT", 768);
  cfg.desktop    = EnvInt("OP2H_DESKTOP", 0) != 0;
  g_record       = EnvInt("OP2H_RECORD", 0) != 0;

  g_log   = fopen((cfg.out + "\\harness.log").c_str(), "w");
  if (!OnWine()) {  // the mission DLL's base address (HoldMissionBase)
    HoldMissionBase();
    HMODULE opu = GetModuleHandleA("OpuPatch.dll");
    if (void* prev = opu ? PatchImport(opu, "KERNEL32.dll", "LoadLibraryExW", reinterpret_cast<void*>(&MissionLoadHook)) : nullptr)
      g_loadLibraryExW = reinterpret_cast<LoadLibraryExWFn>(prev);
    else if (g_missionBase) {
      VirtualFree(g_missionBase, 0, MEM_RELEASE);
      g_missionBase = nullptr;
      Log("mission base: could not hook OPU's LoadLibraryExW, so 0x11000000 isn't held");
    }
    const HMODULE freers[3] = {GetModuleHandleA(nullptr), opu, GetModuleHandleA("op2ext.dll")};
    void* const freeHooks[3] = {reinterpret_cast<void*>(&MissionFreeHook<0>), reinterpret_cast<void*>(&MissionFreeHook<1>),
                                reinterpret_cast<void*>(&MissionFreeHook<2>)};
    for (int i = 0; i < 3; ++i)
      if (freers[i]) g_freeLibrary[i] = reinterpret_cast<FreeLibraryFn>(PatchImport(freers[i], "KERNEL32.dll", "FreeLibrary", freeHooks[i]));
  }
  g_telem = cfg.telemEvery > 0 ? fopen((cfg.out + "\\telemetry.jsonl").c_str(), "w") : nullptr;
  g_trace = cfg.traceEvery > 0 ? fopen((cfg.out + "\\trace.jsonl").c_str(), "w") : nullptr;
  g_audio = EnvInt("OP2H_AUDIO", 0) ? fopen((cfg.out + "\\audio.jsonl").c_str(), "w") : nullptr;
  g_stateOn = EnvInt("OP2H_STATE", 0) != 0;
  g_events  = g_stateOn ? fopen((cfg.out + "\\events.jsonl").c_str(), "w") : nullptr;
  g_stopSpec = Env("OP2H_STOP");
  g_stopFrom = EnvInt("OP2H_STOP_FROM", 0);
  Log("harness: mission=%s ticks=%d shot=%d telem=%d speed=%d turbo=%d seed=%d", cfg.mission.c_str(), cfg.ticks,
      cfg.shotEvery, cfg.telemEvery, cfg.speed, cfg.turbo, cfg.seed);

  // The game writes window/desktop sizes (and OPU its defaults) back to outpost2.ini while running. Point it at a
  // private copy so harness runs never touch the user's settings, and silence audio in that copy.
  const std::string ini = cfg.out + "\\outpost2.ini";
  if (CopyFileA(g_configFile.iniPath_, ini.c_str(), FALSE) && ini.size() < sizeof(g_configFile.iniPath_)) {
    strcpy(g_configFile.iniPath_, ini.c_str());
    // A recording keeps the player's sound and music unless asked (OP2H_SILENT).
    if (!g_record || EnvInt("OP2H_SILENT", 0))
      for (const char* key : { "Music", "Sound" }) WritePrivateProfileStringA("Game", key, "0", ini.c_str());
    // Window geometry comes from here at creation; resizing later would shift the mission's initial view.
    // The game's frame adds 24x34 px around the client area.
    const std::pair<const char*, int> window[] = {
      { "MAXIMIZED", 0 }, { "H_POS", 0 }, { "V_POS", 0 }, { "H_SIZE", cfg.width + 48 }, { "V_SIZE", cfg.height + 36 } };
    for (const auto& [key, value] : window)
      WritePrivateProfileStringA("GAME_WINDOW", key, std::to_string(value).c_str(), ini.c_str());
    // The saved geometry is only honoured if the saved desktop size matches the current one.
    WritePrivateProfileStringA("DESKTOP", "H_SIZE", std::to_string(GetSystemMetrics(SM_CXSCREEN)).c_str(), ini.c_str());
    WritePrivateProfileStringA("DESKTOP", "V_SIZE", std::to_string(GetSystemMetrics(SM_CYSCREEN)).c_str(), ini.c_str());
    Log("config: using %s", ini.c_str());
  } else {
    Log("config: could not redirect %s; the game may write window settings to it", g_configFile.iniPath_);
  }

  // InitMod runs from op2ext's TApp::Init() hook, after TApp::ParseCommandLine(): force the /noshell startup path.
  g_tApp.bShowShell_ = 0;

  RelinkCall(CallLoadDebugMap, reinterpret_cast<void*>(&StartMissionHook));
  g_origGameCycle = reinterpret_cast<GameCycleFn>(RelinkCall(CallProcessGameCycle, reinterpret_cast<void*>(&GameCycleHook)));
  if (cfg.turbo) { const uint8_t jmp = 0xEB;  WriteCode(OnIdleFrameLimiter, &jmp, 1); }
  memcpy(g_drawPanesCode, reinterpret_cast<void*>(OnIdleDrawPanes), 2);
  InstallPresentHook();
  InstallRngLog();
  // A recording is a person playing: the game's own questions (save over a slot, quit) are theirs to answer, and the
  // mouse and keys are the real ones.
  if (!g_record || EnvInt("OP2H_NOBODY", 0)) JumpTo(FnOP2MessageBox, reinterpret_cast<void*>(&MessageBoxHook));
  if (g_audio) InstallAudioHooks();
  else if (g_events) InstallMessageHook();
  SetWindowsHookExA(WH_CBT, &MissionDialogCbt, nullptr, GetCurrentThreadId());
  // In a recording the virtual cursor and keys are active only while a setup script's input verbs run (GameCycleHook);
  // otherwise they pass the real ones through.
  g_realGetCursorPos     = reinterpret_cast<GetCursorPosFn>(PatchIat(IatGetCursorPos, reinterpret_cast<void*>(&VirtualGetCursorPos)));
  g_realGetAsyncKeyState =
    reinterpret_cast<GetAsyncKeyStateFn>(PatchIat(IatGetAsyncKeyState, reinterpret_cast<void*>(&VirtualGetAsyncKeyState)));

  if (!cfg.script.empty()) LoadScript(cfg.script);
  if (g_record) InstallRecordHooks();
  else if (std::any_of(g_script.begin(), g_script.end(), [](const ScriptCmd& c) { return c.verb == "packet"; })) InstallPacketHook();
  if (!cfg.bot.empty()) {
    if (HMODULE h = LoadLibraryA(cfg.bot.c_str())) g_botTick = reinterpret_cast<BotTickFn>(GetProcAddress(h, "HarnessBotTick"));
    Log("bot: %s -> %s", cfg.bot.c_str(), g_botTick ? "ok" : "HarnessBotTick not found");
  }
}

// op2ext calls this when OP2Shell.dll gets loaded. In a mission run that only happens if the game fell back to the
// shell (e.g. the mission ended through the normal end-of-game path), so treat it as the end of the run. In shell mode
// it is when the shell's imports get hooked.
extern "C" __declspec(dllexport) void RunMod() {
  if (g_shellMode) { PatchShellImports();  return; }
  if (!cfg.mission.empty() && g_started) Quit("returned to shell");
}

extern "C" __declspec(dllexport) void DestroyMod() {}
