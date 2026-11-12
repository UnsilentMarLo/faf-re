// EDITOR: the "up" natives of water (docs/projects/editor/EDITOR_PORT_ANALYSIS.md, work package WP6): water bodies
// (lakes at their own levels, `ScenarioInfo.WaterBodies`, docs/WATER_BODIES.md) and shore wave generators (the wave
// section of the `.scmap`, FAForeverMapEditor's `.scmwaves`), over upstream's logic (src/editor/doc/EdWaterBodies,
// src/editor/tools/EdWaves). Bodies are plain Lua tables with the scenario's own field names; the fill is the engine's
// (`CWaterBodies::Build`, the same function `STIMap::InstallWaterBodies` calls), so the preview is what the game fills.
// `.scmwtr` is WP2's (ed_scmwtr_*).
//
// Water bodies (a body: { Elevation, FillTo, Deep, Abyss, Dry, Seed = { x, z }, Outline = { { x, z }, ... }, Bounds = {
// x0, z0, x1, z1 } }; a field left out stays out):
//   ed_waterbodies_fill(bodies[, { install = bool, cells = bool }])
//        -> { bodies = { [i] = { cells, x0, z0, x1, z1, elevation, deep, abyss, skipped, message } }, other = { lines
//        }, count, installed, generation, cells, width, height } Fills `bodies` over the open map's heights as the game
//        does (the defaults of the sea's shading included). `install` puts the result on the map (as the game's session
//        does) and rebuilds the water map, the shore and the water surfaces; the open map then draws the bodies.
//        `cells` also returns one byte per cell (0 sea, n body n).
//   ed_waterbodies_clear()            takes the bodies off the map (the sea only again)
//   ed_waterbodies_info()             -> { count, generation, installed = bool, bodies = { { elevation, deep, abyss,
//   cells, x0, z0, x1, z1 } } } ed_waterbodies_crosscheck(bodies) -> { same, vertices, cells, bodies, previewCount,
//   engineCount, log }   the same bodies through `STIMap::InstallWaterBodies` (the game's own
//        reader of the scenario, the sim fix forced on for the call) and through the fill above, compared vertex by
//        vertex and cell by cell; the map is left as it was
//   ed_waterbodies_simfix(on)         the console variable sim_FixWaterBodies (the game needs it for a map's bodies; a
//   Play of the editor sets it) ed_water_texel(x, z)              -> { packed, shore, depth, foam, flatness, width,
//   height, texelsPerCell }   the texel of the water map the renderers use (GetWaterMap: the finer one of
//        ed_water_hires when it is on) at world x, z: `shore` is the flag the water alpha test reads (ground above the
//        water there), `depth` the colour byte. For the shore checks.
// Wave generators (a wave: { texture, ramp, position = {x,y,z}, angle, velocity = {x,y,z}, lifetime = {a,b}, period =
// {a,b}, scale = {a,b}, frameCount, frameRate = {a,b}, stripCount }):
//   ed_waves_get()                    -> { wave, ... }  the open map's generators (through the wave system's own Save)
//   ed_waves_set(waves)               -> n  puts these generators on the map in place of its own (the wave system's own
//   Load) ed_waves_read(text) / ed_waves_write(waves) -> waves | text    FAForeverMapEditor's `.scmwaves`
//   ed_waves_patterns([luaText])      -> { { name, texture, ramp, speed, speedVariance, lifetime, period, scale,
//   scaleVariance, frameCount, frameRate, stripCount } } ed_waves_generate({ pattern, depth, centre, range, density,
//   spacing, seed }) -> { waves, shorePoints, picked, seed }   generators along the shore of the sea (not added to the
//   map)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "editor/doc/EdWaterBodies.h"
#include "editor/tools/EdWaves.h"
#include "gpg/core/streams/BinaryReader.h"
#include "gpg/core/streams/BinaryWriter.h"
#include "gpg/core/streams/PipeStream.h"
#include "gpg/core/utils/Logging.h"
#include "lua/LuaObject.h"
#include "moho/console/CConCommand.h"
#include "moho/editor/EdUpCommon.h"
#include "moho/editor/EditorDocument.h"
#include "moho/sim/CBackgroundTaskControl.h"
#include "moho/sim/CWldMap.h"
#include "moho/sim/CWldSession.h"
#include "moho/sim/FafGameVersion.h"
#include "moho/sim/STIMap.h"
#include "moho/sim/WaterBodies.h"
#include "moho/render/textures/CD3DDynamicTextureSheet.h"
#include "moho/terrain/CWldTerrainRes.h"
#include "moho/terrain/splat/CWldSplat.h"
#include "moho/terrain/water/WaterFactory.h"
#include "moho/terrain/water/WaveSystem.h"

namespace
{
  namespace up = moho::editor::up;
  using faf::editor::WaterBody;
  using faf::editor::WavePattern;
  using faf::editor::WaveRecord;

  constexpr float kHeightWordScale = 1.0f / 128.0f;

  [[nodiscard]] moho::CWldMap* ActiveMap()
  {
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    return session != nullptr ? session->mWldMap : nullptr;
  }

  [[nodiscard]] moho::IWldTerrainRes* ActiveTerrain()
  {
    moho::CWldMap* const map = ActiveMap();
    return map != nullptr ? map->mTerrainRes : nullptr;
  }

  [[nodiscard]] moho::STIMap* ActiveSti()
  {
    moho::IWldTerrainRes* const terrain = ActiveTerrain();
    return terrain != nullptr ? terrain->mMap : nullptr;
  }

  // ---- Lua <-> structs
  // ------------------------------------------------------------------------------------------------------------------------------

  void Pop(
    lua_State* const L
  )
  {
    lua_settop(L, -2);
  }

  /** `{ a, b, ... }` with exactly `count` numbers at 1..count (absolute index `table`). */
  [[nodiscard]] bool NumbersAt(
    lua_State* const L,
    const int table,
    const int count,
    double* const out
  )
  {
    if (lua_type(L, table) != LUA_TTABLE) {
      return false;
    }
    for (int i = 0; i < count; ++i) {
      lua_rawgeti(L, table, i + 1);
      const bool number = lua_type(L, -1) == LUA_TNUMBER;
      if (number) {
        out[i] = static_cast<double>(lua_tonumber(L, -1));
      }
      Pop(L);
      if (!number) {
        return false;
      }
    }
    lua_rawgeti(L, table, count + 1);
    const bool extra = lua_type(L, -1) != LUA_TNIL;
    Pop(L);
    return !extra;
  }

  /** One body of a Lua list: a field the engine can't use is treated as the engine treats it (a number field of another
   * type is not there; a field of the wrong shape skips the body). */
  [[nodiscard]] WaterBody BodyAt(
    lua_State* const L,
    const int body
  )
  {
    WaterBody out;
    out.hasElevation = false;
    out.hasSeed = false;
    const auto unreadable = [](const std::string& why) {
      WaterBody kept;
      kept.raw = "{ ... }";
      kept.unreadable = why;
      return kept;
    };
    if (lua_type(L, body) != LUA_TTABLE) {
      return unreadable("not a table");
    }
    const auto number = [&](const char* const name, bool& has, float& value) {
      if (up::PushField(L, body, name) == LUA_TNUMBER) {
        has = true;
        value = static_cast<float>(lua_tonumber(L, -1));
      }
      Pop(L);
    };
    number("Elevation", out.hasElevation, out.elevation);
    number("FillTo", out.hasFillTo, out.fillTo);
    number("Deep", out.hasDeep, out.deep);
    number("Abyss", out.hasAbyss, out.abyss);
    if (up::PushField(L, body, "Dry") == LUA_TBOOLEAN) {
      out.dry = lua_toboolean(L, -1) != 0;
    }
    Pop(L);
    double values[4] = {0.0, 0.0, 0.0, 0.0};
    if (const int type = up::PushField(L, body, "Seed"); type != LUA_TNIL) {
      const bool ok = NumbersAt(L, lua_gettop(L), 2, values);
      Pop(L);
      if (!ok) {
        return unreadable("Seed is not { x, z }");
      }
      out.hasSeed = true;
      out.seedX = static_cast<float>(values[0]);
      out.seedZ = static_cast<float>(values[1]);
    } else {
      Pop(L);
    }
    if (const int type = up::PushField(L, body, "Outline"); type != LUA_TNIL) {
      const int list = lua_gettop(L);
      bool ok = type == LUA_TTABLE;
      for (int i = 1; ok; ++i) {
        lua_rawgeti(L, list, i);
        const int itemType = lua_type(L, -1);
        if (itemType == LUA_TNIL) {
          Pop(L);
          break;
        }
        ok = NumbersAt(L, lua_gettop(L), 2, values);
        Pop(L);
        if (ok) {
          out.outline.emplace_back(static_cast<float>(values[0]), static_cast<float>(values[1]));
        }
      }
      Pop(L);
      if (!ok) {
        return unreadable("Outline is not a list of { x, z } points");
      }
    } else {
      Pop(L);
    }
    if (const int type = up::PushField(L, body, "Bounds"); type != LUA_TNIL) {
      const bool ok = NumbersAt(L, lua_gettop(L), 4, values);
      Pop(L);
      if (!ok) {
        return unreadable("Bounds is not { x0, z0, x1, z1 }");
      }
      out.hasBounds = true;
      out.x0 = static_cast<int>(static_cast<float>(values[0]));
      out.z0 = static_cast<int>(static_cast<float>(values[1]));
      out.x1 = static_cast<int>(static_cast<float>(values[2]));
      out.z1 = static_cast<int>(static_cast<float>(values[3]));
    } else {
      Pop(L);
    }
    return out;
  }

  [[nodiscard]] std::vector<WaterBody> BodiesAt(
    lua_State* const L,
    const int list
  )
  {
    std::vector<WaterBody> bodies;
    if (lua_type(L, list) != LUA_TTABLE) {
      return bodies;
    }
    for (int i = 1;; ++i) {
      lua_rawgeti(L, list, i);
      if (lua_type(L, -1) == LUA_TNIL) {
        Pop(L);
        break;
      }
      bodies.push_back(BodyAt(L, lua_gettop(L)));
      Pop(L);
    }
    return bodies;
  }

  void PushArray(
    lua_State* const L,
    const float* const values,
    const int count
  )
  {
    lua_newtable(L);
    for (int i = 0; i < count; ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(values[i]));
      lua_rawseti(L, -2, i + 1);
    }
  }

  void SetArray(
    lua_State* const L,
    const char* const key,
    const float* const values,
    const int count
  )
  {
    lua_pushstring(L, key);
    PushArray(L, values, count);
    lua_settable(L, -3);
  }

  [[nodiscard]] bool ReadArray(
    lua_State* const L,
    const int table,
    const char* const key,
    float* const out,
    const int count
  )
  {
    if (up::PushField(L, table, key) != LUA_TTABLE) {
      Pop(L);
      return false;
    }
    const int list = lua_gettop(L);
    for (int i = 0; i < count; ++i) {
      lua_rawgeti(L, list, i + 1);
      if (lua_type(L, -1) == LUA_TNUMBER) {
        out[i] = static_cast<float>(lua_tonumber(L, -1));
      }
      Pop(L);
    }
    Pop(L);
    return true;
  }

  // ---- water bodies
  // ---------------------------------------------------------------------------------------------------------------------------------

  /** What the fill of the open map was made for: the bodies as the editor holds them and the heights' revision. The
   * editor's refill (ed_waterbodies_info) uses it. */
  struct Installed
  {
    bool on = false;
    std::uint64_t revision = 0;
  };

  Installed& TheInstalled()
  {
    static Installed installed;
    return installed;
  }

  /** The number at the start of a "water body N ..." line, 0 when it has none. */
  [[nodiscard]] std::size_t LineBody(
    const std::string& line
  )
  {
    constexpr char kPrefix[] = "water body ";
    if (line.rfind(kPrefix, 0) != 0) {
      return 0;
    }
    return static_cast<std::size_t>(std::strtoul(line.c_str() + sizeof(kPrefix) - 1, nullptr, 10));
  }

  struct BodyReport
  {
    bool skipped = false;
    std::string message;
    bool filled = false;
    moho::SWaterBody body{};
  };

  struct FillResult
  {
    std::unique_ptr<moho::CWaterBodies> filled;
    std::vector<BodyReport> perBody;
    std::vector<std::string> other;
  };

  /** The fill of the game over the map's heights: `STIMap::InstallWaterBodies`'s defaults and skips (WaterBodyDescs),
   * then `CWaterBodies::Build`. */
  [[nodiscard]] FillResult Fill(
    const moho::STIMap& map,
    const std::vector<WaterBody>& bodies
  )
  {
    FillResult result;
    result.perBody.resize(bodies.size());
    const moho::CHeightField* const field = map.mHeightField.get();
    if (bodies.empty() || field == nullptr) {
      return result;
    }
    const bool seaShading = map.mWaterEnabled != 0u && map.mWaterElevation > map.mWaterElevationDeep &&
      map.mWaterElevation > map.mWaterElevationAbyss;
    const float deepBelow = seaShading ? map.mWaterElevation - map.mWaterElevationDeep : 2.5f;
    const float abyssBelow = seaShading ? map.mWaterElevation - map.mWaterElevationAbyss : 15.0f;
    std::vector<std::string> log;
    std::vector<std::size_t> bodyOf;
    const std::vector<moho::SWaterBodyDesc> descs =
      faf::editor::WaterBodyDescs(bodies, deepBelow, abyssBelow, log, &bodyOf);
    for (const std::string& line : log) {
      const std::size_t body = LineBody(line);
      if (body >= 1 && body <= bodies.size()) {
        BodyReport& report = result.perBody[body - 1];
        report.message += (report.message.empty() ? "" : "; ") + line.substr(line.find(' ', 11) + 1);
        report.skipped = report.skipped || line.find("skipped") != std::string::npos ||
          line.find("not previewed") != std::string::npos;
      } else {
        result.other.push_back(line);
      }
    }
    // Bodies the description step skipped have no report slot in `descs`: `skipped` is set above; the rest are the
    // filler's.
    std::vector<std::string> fillLog;
    if (!descs.empty()) {
      result.filled =
        moho::CWaterBodies::Build(field->data, field->width, field->height, kHeightWordScale, descs, &fillLog);
    }
    std::vector<bool> fillSkipped(descs.size(), false);
    for (const std::string& line : fillLog) {
      // The filler numbers its lines by the bodies it was given; the editor's by the scenario's.
      const std::size_t desc = LineBody(line);
      if (desc >= 1 && desc <= descs.size()) {
        BodyReport& report = result.perBody[bodyOf[desc - 1]];
        report.message += (report.message.empty() ? "" : "; ") + line.substr(line.find(' ', 11) + 1);
        const bool skipped = line.find("skipped") != std::string::npos;
        report.skipped = report.skipped || skipped;
        fillSkipped[desc - 1] = fillSkipped[desc - 1] || skipped;
      } else {
        result.other.push_back(line);
      }
    }
    std::uint8_t number = 0;
    for (std::size_t d = 0; d < descs.size(); ++d) {
      if (fillSkipped[d]) {
        continue;
      }
      BodyReport& report = result.perBody[bodyOf[d]];
      if (result.filled != nullptr && number < result.filled->BodyCount()) {
        report.filled = true;
        report.body = result.filled->Body(++number);
      }
    }
    return result;
  }

  /** The open map's water map, shore and surfaces follow its bodies (as the game's session does after it installs them,
   * and as a level change does in the editor). */
  void RefreshWaterDrawing(
    moho::IWldTerrainRes* const terrain
  )
  {
    if (terrain->IsInEditMode()) {
      terrain->UpdateWaterMap();
    } else {
      moho::CBackgroundTaskControl control{nullptr};
      terrain->EnterEditMode(control);
      terrain->ExitEditMode();
    }
    moho::EdRefreshWaterSurfaces(terrain);
    if (moho::IDecalManager* const decals = terrain->GetDecalManager(); decals != nullptr) {
      decals->MarkPendingChanges();
    }
  }

  void PushBodyStats(
    lua_State* const L,
    const moho::SWaterBody& body
  )
  {
    up::SetNumber(L, "elevation", static_cast<double>(body.elevation));
    up::SetNumber(L, "deep", static_cast<double>(body.deep));
    up::SetNumber(L, "abyss", static_cast<double>(body.abyss));
    up::SetNumber(L, "cells", static_cast<double>(body.cellCount));
    up::SetNumber(L, "x0", body.x0);
    up::SetNumber(L, "z0", body.z0);
    up::SetNumber(L, "x1", body.x1);
    up::SetNumber(L, "z1", body.z1);
  }

  void PushCells(
    lua_State* const L,
    const moho::CWaterBodies& bodies
  )
  {
    const std::int32_t width = bodies.CellWidth();
    const std::int32_t height = bodies.CellHeight();
    std::string cells(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), '\0');
    for (std::int32_t z = 0; z < height; ++z) {
      for (std::int32_t x = 0; x < width; ++x) {
        cells[static_cast<std::size_t>(z) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
          static_cast<char>(bodies.BodyOfCell(x, z));
      }
    }
    lua_pushstring(L, "cells");
    lua_pushlstring(L, cells.data(), cells.size());
    lua_settable(L, -3);
    up::SetNumber(L, "width", width);
    up::SetNumber(L, "height", height);
  }

  int __cdecl cfunc_WaterBodiesFill(
    lua_State* const L
  )
  {
    moho::IWldTerrainRes* const terrain = ActiveTerrain();
    moho::STIMap* const map = terrain != nullptr ? terrain->mMap : nullptr;
    if (map == nullptr || map->mHeightField.get() == nullptr) {
      return up::Fail(L, "no map is open");
    }
    const bool install = lua_type(L, 2) == LUA_TTABLE && up::FieldBool(L, 2, "install", false);
    const bool wantCells = lua_type(L, 2) == LUA_TTABLE && up::FieldBool(L, 2, "cells", false);
    const std::vector<WaterBody> bodies = BodiesAt(L, 1);
    FillResult result = Fill(*map, bodies);

    lua_newtable(L);
    lua_pushstring(L, "bodies");
    lua_newtable(L);
    for (std::size_t i = 0; i < result.perBody.size(); ++i) {
      const BodyReport& report = result.perBody[i];
      lua_newtable(L);
      if (report.filled) {
        PushBodyStats(L, report.body);
      }
      up::SetBool(L, "skipped", report.skipped);
      up::SetString(L, "message", report.message);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
    up::SetStringList(L, "other", result.other);
    up::SetNumber(L, "count", result.filled != nullptr ? static_cast<double>(result.filled->BodyCount()) : 0.0);
    if (wantCells && result.filled != nullptr) {
      PushCells(L, *result.filled);
    }
    const bool had = map->HasWaterBodies();
    if (install) {
      map->mWaterBodies = std::move(result.filled);
      TheInstalled().on = map->HasWaterBodies();
      TheInstalled().revision = moho::editor::Document().Revision();
      if (had || map->HasWaterBodies()) {
        RefreshWaterDrawing(terrain);
      }
    }
    up::SetBool(L, "installed", install);
    up::SetNumber(L, "generation", map->HasWaterBodies() ? static_cast<double>(map->mWaterBodies->Generation()) : 0.0);
    return 1;
  }

  int __cdecl cfunc_WaterBodiesClear(
    lua_State* const L
  )
  {
    moho::IWldTerrainRes* const terrain = ActiveTerrain();
    moho::STIMap* const map = terrain != nullptr ? terrain->mMap : nullptr;
    if (map == nullptr) {
      return up::Fail(L, "no map is open");
    }
    const bool had = map->HasWaterBodies();
    map->mWaterBodies.reset();
    TheInstalled() = Installed{};
    if (had) {
      RefreshWaterDrawing(terrain);
    }
    lua_pushboolean(L, had ? 1 : 0);
    return 1;
  }

  int __cdecl cfunc_WaterBodiesInfo(
    lua_State* const L
  )
  {
    const moho::STIMap* const map = ActiveSti();
    lua_newtable(L);
    const bool has = map != nullptr && map->HasWaterBodies();
    up::SetBool(L, "installed", has);
    up::SetNumber(L, "count", has ? static_cast<double>(map->mWaterBodies->BodyCount()) : 0.0);
    up::SetNumber(L, "generation", has ? static_cast<double>(map->mWaterBodies->Generation()) : 0.0);
    up::SetNumber(L, "revision", static_cast<double>(TheInstalled().revision));
    lua_pushstring(L, "bodies");
    lua_newtable(L);
    if (has) {
      for (std::size_t n = 1; n <= map->mWaterBodies->BodyCount(); ++n) {
        lua_newtable(L);
        PushBodyStats(L, map->mWaterBodies->Body(static_cast<std::uint8_t>(n)));
        lua_rawseti(L, -2, static_cast<int>(n));
      }
    }
    lua_settable(L, -3);
    return 1;
  }

  /** The sim fix `WaterBodies` forced on for a scope: `STIMap::InstallWaterBodies` asks `FAF_IsSimFixActive`, which
   * holds for a whole session. */
  struct SimFixOn
  {
    std::uint32_t saved;
    SimFixOn()
      : saved(moho::gFafSessionSimFixes)
    {
      moho::gFafSessionSimFixes |= 1u << static_cast<std::uint32_t>(moho::EFafSimFix::WaterBodies);
    }
    ~SimFixOn()
    {
      moho::gFafSessionSimFixes = saved;
    }
    SimFixOn(const SimFixOn&) = delete;
    SimFixOn& operator=(const SimFixOn&) = delete;
  };

  int __cdecl cfunc_WaterBodiesCrossCheck(
    lua_State* const L
  )
  {
    moho::STIMap* const map = ActiveSti();
    if (map == nullptr || map->mHeightField.get() == nullptr) {
      return up::Fail(L, "no map is open");
    }
    if (lua_type(L, 1) != LUA_TTABLE) {
      return up::Fail(L, "ed_waterbodies_crosscheck(bodies): bodies is a list of tables");
    }
    FillResult preview = Fill(*map, BodiesAt(L, 1));

    // The game's own reader of the scenario: a ScenarioInfo table around the same list.
    std::unique_ptr<moho::CWaterBodies> held = std::move(map->mWaterBodies);
    lua_newtable(L);
    const int info = lua_gettop(L);
    lua_pushstring(L, "WaterBodies");
    lua_pushvalue(L, 1);
    lua_settable(L, info);
    {
      const LuaPlus::LuaObject scenario(LuaPlus::LuaState::CastState(L), info);
      const SimFixOn fix;
      map->InstallWaterBodies(scenario);
    }
    std::unique_ptr<moho::CWaterBodies> engine = std::move(map->mWaterBodies);
    map->mWaterBodies = std::move(held);

    std::size_t vertices = 0;
    std::size_t cells = 0;
    std::size_t bodyDiffs = 0;
    const moho::CWaterBodies* const a = preview.filled.get();
    const moho::CWaterBodies* const b = engine.get();
    const std::size_t previewCount = a != nullptr ? a->BodyCount() : 0;
    const std::size_t engineCount = b != nullptr ? b->BodyCount() : 0;
    if ((a == nullptr) != (b == nullptr) || previewCount != engineCount) {
      ++bodyDiffs;
    }
    const moho::CHeightField* const field = map->mHeightField.get();
    if (a != nullptr && b != nullptr) {
      for (std::int32_t z = 0; z < field->height; ++z) {
        for (std::int32_t x = 0; x < field->width; ++x) {
          vertices += a->BodyOfVertex(x, z) != b->BodyOfVertex(x, z) ? 1u : 0u;
          if (x + 1 < field->width && z + 1 < field->height) {
            cells += a->BodyOfCell(x, z) != b->BodyOfCell(x, z) ? 1u : 0u;
          }
        }
      }
      for (std::size_t n = 1; n <= std::min(previewCount, engineCount); ++n) {
        const moho::SWaterBody& pa = a->Body(static_cast<std::uint8_t>(n));
        const moho::SWaterBody& pb = b->Body(static_cast<std::uint8_t>(n));
        if (
          std::memcmp(&pa, &pb, sizeof(pa)) != 0 &&
          !(pa.elevation == pb.elevation && pa.deep == pb.deep && pa.abyss == pb.abyss &&
            pa.cellCount == pb.cellCount && pa.x0 == pb.x0 && pa.z0 == pb.z0 && pa.x1 == pb.x1 && pa.z1 == pb.z1)
        ) {
          ++bodyDiffs;
        }
      }
    }
    lua_newtable(L);
    const int out = lua_gettop(L);
    up::SetBool(L, "same", vertices == 0 && cells == 0 && bodyDiffs == 0);
    up::SetNumber(L, "vertices", static_cast<double>(vertices));
    up::SetNumber(L, "cells", static_cast<double>(cells));
    up::SetNumber(L, "bodies", static_cast<double>(bodyDiffs));
    up::SetNumber(L, "previewCount", static_cast<double>(previewCount));
    up::SetNumber(L, "engineCount", static_cast<double>(engineCount));
    std::vector<std::string> log = preview.other;
    for (const BodyReport& report : preview.perBody) {
      if (!report.message.empty()) {
        log.push_back(report.message);
      }
    }
    up::SetStringList(L, "log", log);
    lua_settop(L, out);
    return 1;
  }

  int __cdecl cfunc_WaterBodiesSimFix(
    lua_State* const L
  )
  {
    const bool on = lua_toboolean(L, 1) != 0;
    moho::CON_Execute(on ? "sim_FixWaterBodies 1" : "sim_FixWaterBodies -1");
    return 0;
  }

  int __cdecl cfunc_WaterTexel(
    lua_State* const L
  )
  {
    moho::IWldTerrainRes* const terrain = ActiveTerrain();
    const moho::CHeightField* const field = terrain != nullptr ? terrain->GetHeightField() : nullptr;
    if (field == nullptr) {
      return up::Fail(L, "no map is open");
    }
    const boost::shared_ptr<moho::ID3DTextureSheet> sheet = terrain->GetWaterMap();
    const boost::shared_ptr<moho::CD3DDynamicTextureSheet> dynamic =
      boost::static_pointer_cast<moho::CD3DDynamicTextureSheet>(sheet);
    if (dynamic.get() == nullptr) {
      return up::Fail(L, "the map has no water map");
    }
    Wm3::Vector3f size{};
    (void)dynamic->GetDimensions(&size);
    const int width = static_cast<int>(size.x);
    const int height = static_cast<int>(size.y);
    const double cellsX = static_cast<double>(field->width - 1);
    const double cellsZ = static_cast<double>(field->height - 1);
    const int tx = std::clamp(static_cast<int>(std::floor(luaL_checknumber(L, 1) / cellsX * width)), 0, width - 1);
    const int tz = std::clamp(static_cast<int>(std::floor(luaL_checknumber(L, 2) / cellsZ * height)), 0, height - 1);
    std::uint32_t pitchBytes = 0;
    void* bits = nullptr;
    if (!dynamic->Lock(&pitchBytes, &bits)) {
      return up::Fail(L, "the water map can't be read");
    }
    const std::uint32_t packed = reinterpret_cast<const std::uint32_t*>(
      static_cast<const std::uint8_t*>(bits) + static_cast<std::size_t>(tz) * pitchBytes
    )[tx];
    (void)dynamic->Unlock();
    lua_newtable(L);
    up::SetNumber(L, "packed", static_cast<double>(packed));
    up::SetBool(L, "shore", (packed & 0xFFu) != 0u);
    up::SetNumber(L, "depth", static_cast<double>((packed >> 8) & 0xFFu));
    up::SetNumber(L, "flatness", static_cast<double>((packed >> 16) & 0xFFu));
    up::SetNumber(L, "foam", static_cast<double>((packed >> 24) & 0xFFu));
    up::SetNumber(L, "width", width);
    up::SetNumber(L, "height", height);
    up::SetNumber(L, "texelsPerCell", cellsX > 0.0 ? static_cast<double>(width) / cellsX : 0.0);
    return 1;
  }

  // ---- wave generators
  // ------------------------------------------------------------------------------------------------------------------------------

  void PushWave(
    lua_State* const L,
    const WaveRecord& wave
  )
  {
    lua_newtable(L);
    up::SetString(L, "texture", wave.texture);
    up::SetString(L, "ramp", wave.ramp);
    SetArray(L, "position", wave.position, 3);
    up::SetNumber(L, "angle", static_cast<double>(wave.angle));
    SetArray(L, "velocity", wave.velocity, 3);
    SetArray(L, "lifetime", wave.lifetime, 2);
    SetArray(L, "period", wave.period, 2);
    SetArray(L, "scale", wave.scale, 2);
    up::SetNumber(L, "frameCount", static_cast<double>(wave.frameCount));
    SetArray(L, "frameRate", wave.frameRate, 2);
    up::SetNumber(L, "stripCount", static_cast<double>(wave.stripCount));
  }

  void PushWaves(
    lua_State* const L,
    const std::vector<WaveRecord>& waves
  )
  {
    lua_newtable(L);
    for (std::size_t i = 0; i < waves.size(); ++i) {
      PushWave(L, waves[i]);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
  }

  [[nodiscard]] WaveRecord WaveAt(
    lua_State* const L,
    const int wave
  )
  {
    WaveRecord out;
    out.texture = up::FieldString(L, wave, "texture", out.texture);
    out.ramp = up::FieldString(L, wave, "ramp", out.ramp);
    (void)ReadArray(L, wave, "position", out.position, 3);
    out.angle = static_cast<float>(up::FieldNumber(L, wave, "angle", out.angle));
    (void)ReadArray(L, wave, "velocity", out.velocity, 3);
    (void)ReadArray(L, wave, "lifetime", out.lifetime, 2);
    (void)ReadArray(L, wave, "period", out.period, 2);
    (void)ReadArray(L, wave, "scale", out.scale, 2);
    out.frameCount = static_cast<float>(up::FieldNumber(L, wave, "frameCount", out.frameCount));
    (void)ReadArray(L, wave, "frameRate", out.frameRate, 2);
    out.stripCount = static_cast<float>(up::FieldNumber(L, wave, "stripCount", out.stripCount));
    return out;
  }

  [[nodiscard]] std::vector<WaveRecord> WavesAt(
    lua_State* const L,
    const int list
  )
  {
    std::vector<WaveRecord> waves;
    if (lua_type(L, list) != LUA_TTABLE) {
      return waves;
    }
    for (int i = 1;; ++i) {
      lua_rawgeti(L, list, i);
      if (lua_type(L, -1) != LUA_TTABLE) {
        Pop(L);
        break;
      }
      waves.push_back(WaveAt(L, lua_gettop(L)));
      Pop(L);
    }
    return waves;
  }

  [[nodiscard]] moho::WaveSystem* ActiveWaves()
  {
    moho::IWldTerrainRes* const terrain = ActiveTerrain();
    return terrain != nullptr ? terrain->GetWaveSystem() : nullptr;
  }

  /** The open map's generators, through the wave system's own Save (WaveSystem::Save 0x00889AD0) and upstream's reader
   * of that section. */
  [[nodiscard]] bool WavesNow(
    std::vector<WaveRecord>& waves,
    std::string& error
  )
  {
    moho::WaveSystem* const system = ActiveWaves();
    if (system == nullptr) {
      error = "no map is open";
      return false;
    }
    gpg::PipeStream pipe;
    {
      gpg::BinaryWriter writer(&pipe);
      system->Save(writer);
    }
    pipe.VirtFlush();
    // Drained in chunks: GetLength (0x009566C0) counts the first block only from where a read left it, so before any
    // read it comes up short on a stream of several blocks.
    std::string bytes;
    char chunk[65536];
    for (;;) {
      const std::size_t got = pipe.VirtReadNonBlocking(chunk, sizeof(chunk));
      if (got == 0) {
        break;
      }
      bytes.append(chunk, got);
    }
    if (!faf::editor::WaveRecordsFromBytes(bytes, waves)) {
      error = "the wave generators didn't read back (" + std::to_string(bytes.size()) + " bytes)";
      return false;
    }
    return true;
  }

  /** Puts these generators on the open map in place of its own (WaveSystem::Load 0x008899E0, the map's current format
   * 60: every float of a generator is read). */
  [[nodiscard]] bool WavesInto(
    const std::vector<WaveRecord>& waves,
    std::string& error
  )
  {
    moho::WaveSystem* const system = ActiveWaves();
    moho::IWldTerrainRes* const terrain = ActiveTerrain();
    const moho::CHeightField* const field = terrain != nullptr ? terrain->GetHeightField() : nullptr;
    if (system == nullptr || field == nullptr) {
      error = "no map is open";
      return false;
    }
    const std::string bytes = faf::editor::WaveRecordsToBytes(waves);
    gpg::PipeStream pipe;
    pipe.VirtWrite(bytes.data(), bytes.size());
    pipe.VirtFlush();
    gpg::BinaryReader reader(&pipe);
    system->Load(60, field->height - 1, field->width - 1, reader);
    return true;
  }

  int __cdecl cfunc_WavesGet(
    lua_State* const L
  )
  {
    std::vector<WaveRecord> waves;
    std::string error;
    if (!WavesNow(waves, error)) {
      return up::Fail(L, error);
    }
    PushWaves(L, waves);
    return 1;
  }

  int __cdecl cfunc_WavesSet(
    lua_State* const L
  )
  {
    const std::vector<WaveRecord> waves = WavesAt(L, 1);
    std::string error;
    if (!WavesInto(waves, error)) {
      return up::Fail(L, error);
    }
    lua_pushnumber(L, static_cast<lua_Number>(waves.size()));
    return 1;
  }

  int __cdecl cfunc_WavesRead(
    lua_State* const L
  )
  {
    const std::string text = up::ArgString(L, 1, "ed_waves_read");
    std::vector<WaveRecord> waves;
    std::string error;
    if (!faf::editor::WavesFromScmwaves(text, waves, error)) {
      return up::Fail(L, error);
    }
    PushWaves(L, waves);
    return 1;
  }

  int __cdecl cfunc_WavesWrite(
    lua_State* const L
  )
  {
    const std::string text = faf::editor::WavesToScmwaves(WavesAt(L, 1));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
  }

  void PushPattern(
    lua_State* const L,
    const WavePattern& pattern
  )
  {
    lua_newtable(L);
    up::SetString(L, "name", pattern.name);
    up::SetString(L, "texture", pattern.texture);
    up::SetString(L, "ramp", pattern.ramp);
    up::SetNumber(L, "speed", static_cast<double>(pattern.speed));
    up::SetNumber(L, "speedVariance", static_cast<double>(pattern.speedVariance));
    SetArray(L, "lifetime", pattern.lifetime, 2);
    SetArray(L, "period", pattern.period, 2);
    SetArray(L, "scale", pattern.scale, 2);
    up::SetNumber(L, "scaleVariance", static_cast<double>(pattern.scaleVariance));
    up::SetNumber(L, "frameCount", static_cast<double>(pattern.frameCount));
    SetArray(L, "frameRate", pattern.frameRate, 2);
    up::SetNumber(L, "stripCount", static_cast<double>(pattern.stripCount));
  }

  [[nodiscard]] WavePattern PatternAt(
    lua_State* const L,
    const int pattern
  )
  {
    WavePattern out;
    out.name = up::FieldString(L, pattern, "name", out.name);
    out.texture = up::FieldString(L, pattern, "texture", out.texture);
    out.ramp = up::FieldString(L, pattern, "ramp", out.ramp);
    out.speed = static_cast<float>(up::FieldNumber(L, pattern, "speed", out.speed));
    out.speedVariance = static_cast<float>(up::FieldNumber(L, pattern, "speedVariance", out.speedVariance));
    (void)ReadArray(L, pattern, "lifetime", out.lifetime, 2);
    (void)ReadArray(L, pattern, "period", out.period, 2);
    (void)ReadArray(L, pattern, "scale", out.scale, 2);
    out.scaleVariance = static_cast<float>(up::FieldNumber(L, pattern, "scaleVariance", out.scaleVariance));
    out.frameCount = static_cast<float>(up::FieldNumber(L, pattern, "frameCount", out.frameCount));
    (void)ReadArray(L, pattern, "frameRate", out.frameRate, 2);
    out.stripCount = static_cast<float>(up::FieldNumber(L, pattern, "stripCount", out.stripCount));
    return out;
  }

  int __cdecl cfunc_WavesPatterns(
    lua_State* const L
  )
  {
    std::vector<WavePattern> patterns;
    if (lua_type(L, 1) == LUA_TSTRING) {
      std::string error;
      if (!faf::editor::ParseWavePatterns(lua_tostring(L, 1), patterns, error)) {
        return up::Fail(L, error);
      }
    } else {
      patterns = faf::editor::BuiltInWavePatterns();
    }
    lua_newtable(L);
    for (std::size_t i = 0; i < patterns.size(); ++i) {
      PushPattern(L, patterns[i]);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
  }

  int __cdecl cfunc_WavesGenerate(
    lua_State* const L
  )
  {
    const moho::STIMap* const map = ActiveSti();
    const moho::CHeightField* const field = map != nullptr ? map->mHeightField.get() : nullptr;
    if (field == nullptr) {
      return up::Fail(L, "no map is open");
    }
    if (map->mWaterEnabled == 0u) {
      return up::Fail(L, "the map has no sea: waves are made along the shore of the sea");
    }
    WavePattern pattern;
    if (up::PushField(L, 1, "pattern") == LUA_TTABLE) {
      pattern = PatternAt(L, lua_gettop(L));
    } else {
      const std::vector<WavePattern>& built = faf::editor::BuiltInWavePatterns();
      if (!built.empty()) {
        pattern = built.front();
      }
    }
    Pop(L);
    const float depth = static_cast<float>(up::FieldNumber(L, 1, "depth", 1.5));
    const float centre = static_cast<float>(up::FieldNumber(L, 1, "centre", 180.0));
    const float range = static_cast<float>(up::FieldNumber(L, 1, "range", 180.0));
    const float density = static_cast<float>(up::FieldNumber(L, 1, "density", 0.5));
    const float spacing = static_cast<float>(up::FieldNumber(L, 1, "spacing", 12.0));
    std::uint32_t seed = static_cast<std::uint32_t>(up::FieldNumber(L, 1, "seed", 1.0));

    // Samples in the game's height units; (width + 1) x (height + 1) of a map of width x height cells.
    const std::size_t count = static_cast<std::size_t>(field->width) * static_cast<std::size_t>(field->height);
    std::vector<float> heights(count);
    for (std::size_t i = 0; i < count; ++i) {
      heights[i] = static_cast<float>(field->data[i]) * kHeightWordScale;
    }
    std::vector<faf::editor::ShorePoint> points = faf::editor::FindShorePoints(
      heights, field->width - 1, field->height - 1, map->mWaterElevation, depth, centre, range
    );
    const std::size_t found = points.size();
    points = faf::editor::PickShorePoints(std::move(points), density, spacing, seed);
    std::vector<WaveRecord> waves;
    waves.reserve(points.size());
    std::uint32_t state = seed;
    for (const faf::editor::ShorePoint& point : points) {
      waves.push_back(faf::editor::MakeShoreWave(pattern, point, map->mWaterElevation, state));
    }
    lua_newtable(L);
    lua_pushstring(L, "waves");
    PushWaves(L, waves);
    lua_settable(L, -3);
    up::SetNumber(L, "shorePoints", static_cast<double>(found));
    up::SetNumber(L, "picked", static_cast<double>(points.size()));
    up::SetNumber(L, "seed", static_cast<double>(state));
    return 1;
  }

  struct EdUpWaterBootstrap
  {
    EdUpWaterBootstrap()
    {
      moho::editor::OnSessionEnd([] {
        TheInstalled() = Installed{};
      });
    }
  };

  const EdUpWaterBootstrap gBootstrap{};

  const up::Natives gEdUpWaterNatives{{
    {"ed_waterbodies_fill",
     &cfunc_WaterBodiesFill,
     "ed_waterbodies_fill(bodies[, { install, cells }]) -> { bodies, other, count, installed, generation[, cells, "
     "width, height] } | nil, message"},
    {"ed_waterbodies_clear", &cfunc_WaterBodiesClear, "ed_waterbodies_clear() -> had bodies"},
    {"ed_waterbodies_info",
     &cfunc_WaterBodiesInfo,
     "ed_waterbodies_info() -> { installed, count, generation, revision, bodies }"},
    {"ed_waterbodies_crosscheck",
     &cfunc_WaterBodiesCrossCheck,
     "ed_waterbodies_crosscheck(bodies) -> { same, vertices, cells, bodies, previewCount, engineCount, log } | nil, "
     "message"},
    {"ed_waterbodies_simfix",
     &cfunc_WaterBodiesSimFix,
     "ed_waterbodies_simfix(on): the console variable sim_FixWaterBodies"},
    {"ed_water_texel",
     &cfunc_WaterTexel,
     "ed_water_texel(x, z) -> { packed, shore, depth, foam, flatness, width, height, texelsPerCell } | nil, message"},
    {"ed_waves_get", &cfunc_WavesGet, "ed_waves_get() -> { wave, ... } | nil, message"},
    {"ed_waves_set", &cfunc_WavesSet, "ed_waves_set(waves) -> n | nil, message"},
    {"ed_waves_read", &cfunc_WavesRead, "ed_waves_read(scmwavesText) -> waves | nil, message"},
    {"ed_waves_write", &cfunc_WavesWrite, "ed_waves_write(waves) -> scmwavesText"},
    {"ed_waves_patterns",
     &cfunc_WavesPatterns,
     "ed_waves_patterns([wavepatternsLua]) -> { pattern, ... } | nil, message"},
    {"ed_waves_generate",
     &cfunc_WavesGenerate,
     "ed_waves_generate({ pattern, depth, centre, range, density, spacing, seed }) -> { waves, shorePoints, picked, "
     "seed } | nil, message"},
  }};
} // namespace
