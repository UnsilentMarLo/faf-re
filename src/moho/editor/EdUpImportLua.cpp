// EDITOR: the FAFME importers / exporters as Lua natives (docs/projects/editor/EDITOR_PORT_ANALYSIS.md, work package WP2): FAForeverMapEditor's preset files over
// upstream's logic (faf_editor_logic: EdMarkerPreset, EdUnitPreset, EdPropPaint, EdLayerPreset, EdLighting, EdSkybox, EdWaterPreset, EdJson). Text in, text out and plain
// Lua tables; no engine object is touched and no file is read here (Lua reads and writes the file: ed_read_file / ed_write_file), so every native is also a pure function.
//
// Failures are `nil, message`; a wrong argument type is a Lua error (EdUpCommon.h). Vectors are arrays {x, y, z}; the environment values use the names of environment.lua
// (light_mult, sun_x, ..., fog_end; dome_ox, ..., cirrus4_dz; water_on, w_color_r, ...), so `Environment.Capture` / `Environment.Apply` parts go in and out unchanged.
//
//   .fafmapmarkers   ed_fafmapmarkers_write(markers, { names, width, height }) -> text            markers = list of { name, type, position, orientation, adjacentTo }
//                    ed_fafmapmarkers_read(text, { existing, reserved })          -> { markers, skipped, links }   markers = list of { name, type, position, orientation, links }
//   .scunits         ed_scunits_write(units)                                      -> text            units = list of { bp, x, y, z, heading | orientation, orders, platoon }
//                    ed_scunits_read(text, { width, height, fit })                -> { units, centre, moved }
//   .proppaintset    ed_proppaintset_write(entries) -> text                       entries = list of { blueprint, rotationMin, rotationMax, scaleMin, scaleMax, chance }
//                    ed_proppaintset_read(text) -> entries
//   .scmsl           ed_scmsl_write(layer) -> text      ed_scmsl_read(text) -> layer          layer = { albedo, normals, albedoSize, normalsSize }
//   .scmst           ed_scmst_write({ shader, layers }) -> text                   ed_scmst_read(text) -> { shader, layers }   (ten layers, Stratum0 .. Stratum9)
//   .scmlighting     ed_scmlighting_write(num) -> text                            ed_scmlighting_read(text) -> num
//   .scmskybox       ed_scmskybox_write({ num, texts, billboards }) -> text       ed_scmskybox_read(text) -> { num, texts, billboards }   billboards = flat list, 10 numbers each
//   .scmwtr          ed_scmwtr_write({ num, texts }) -> text                      ed_scmwtr_read(text) -> { num, texts }

#include <string>
#include <vector>

#include "editor/doc/EdJson.h"
#include "editor/doc/EdLayerPreset.h"
#include "editor/doc/EdLighting.h"
#include "editor/doc/EdMarkerPreset.h"
#include "editor/doc/EdPropPaint.h"
#include "editor/doc/EdSaveFile.h"
#include "editor/doc/EdSkybox.h"
#include "editor/doc/EdUnitPreset.h"
#include "editor/doc/EdUnits.h"
#include "editor/doc/EdWater.h"
#include "editor/doc/EdWaterPreset.h"
#include "editor/tools/EdMarkers.h"
#include "moho/editor/EdUpCommon.h"

namespace
{
  namespace up = moho::editor::up;
  namespace fe = faf::editor;

  // ---- table helpers -------------------------------------------------------------------------------------------------------------------------

  /** The array t[key] as up to `n` numbers into `out` (missing entries keep their value). */
  void FieldArray(lua_State* const L, const int index, const char* const key, float* const out, const int n)
  {
    if (up::PushField(L, index, key) == LUA_TTABLE) {
      const int table = lua_gettop(L);
      for (int i = 0; i < n; ++i) {
        lua_rawgeti(L, table, i + 1);
        if (lua_type(L, -1) == LUA_TNUMBER) {
          out[i] = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_settop(L, -2);
      }
    }
    lua_settop(L, -2);
  }

  /** Adds t[key] = { v[0], ... v[n-1] } to the table on top of the stack. */
  void SetArray(lua_State* const L, const char* const key, const float* const v, const int n)
  {
    lua_pushstring(L, key);
    lua_newtable(L);
    for (int i = 0; i < n; ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(v[i]));
      lua_rawseti(L, -2, i + 1);
    }
    lua_settable(L, -3);
  }

  void SetArrayD(lua_State* const L, const char* const key, const double* const v, const int n)
  {
    lua_pushstring(L, key);
    lua_newtable(L);
    for (int i = 0; i < n; ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(v[i]));
      lua_rawseti(L, -2, i + 1);
    }
    lua_settable(L, -3);
  }

  /** Calls `each(entryIndex)` for t[1], t[2], ... while they are tables (the entry is on the stack at that index). */
  template <class Each>
  void ForEachEntry(lua_State* const L, const int list, Each&& each)
  {
    if (lua_type(L, list) != LUA_TTABLE) {
      return;
    }
    for (int i = 1;; ++i) {
      lua_rawgeti(L, list, i);
      if (lua_type(L, -1) != LUA_TTABLE) {
        lua_settop(L, -2);
        return;
      }
      each(lua_gettop(L));
      lua_settop(L, -2);
    }
  }

  /** t[key] as a sub-table argument index on top of the stack: pushes it (nil if absent); the caller pops. */
  int PushSub(lua_State* const L, const int index, const char* const key)
  {
    return up::PushField(L, index, key);
  }

  int PushText(lua_State* const L, const std::string& text)
  {
    lua_pushlstring(L, text.data(), text.size());
    return 1;
  }

  // ---- .fafmapmarkers ------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_FafmapmarkersWrite(lua_State* const L)
  {
    const int list = 1;
    const int opts = 2;
    std::vector<fe::SaveEntry> markers;
    ForEachEntry(L, list, [&](const int entry) {
      fe::SaveEntry marker;
      marker.name = up::FieldString(L, entry, "name");
      float position[3] = {0.0f, 0.0f, 0.0f};
      float orientation[3] = {0.0f, 0.0f, 0.0f};
      FieldArray(L, entry, "position", position, 3);
      FieldArray(L, entry, "orientation", orientation, 3);
      marker.properties.emplace_back("type", fe::StringValue(up::FieldString(L, entry, "type", "Blank Marker")));
      marker.properties.emplace_back("orientation", fe::Vector3Value(orientation[0], orientation[1], orientation[2]));
      marker.properties.emplace_back("position", fe::Vector3Value(position[0], position[1], position[2]));
      const std::string links = up::FieldString(L, entry, "adjacentTo");
      if (!links.empty()) {
        marker.properties.emplace_back("adjacentTo", fe::StringValue(links));
      }
      markers.push_back(std::move(marker));
    });
    const std::vector<std::string> names = up::FieldStringList(L, opts, "names");
    const int width = up::FieldInt(L, opts, "width", 0);
    const int height = up::FieldInt(L, opts, "height", 0);
    return PushText(L, fe::MarkersToFafmapmarkers(markers, names, width, height));
  }

  int __cdecl cfunc_FafmapmarkersRead(lua_State* const L)
  {
    const std::string text = up::ArgString(L, 1, "ed_fafmapmarkers_read");
    const int opts = 2;
    std::vector<fe::SaveEntry> markers;
    for (const std::string& name : up::FieldStringList(L, opts, "existing")) {
      markers.push_back({name, {}});
    }
    const std::size_t before = markers.size();
    const std::vector<std::string> reserved = up::FieldStringList(L, opts, "reserved");
    fe::MarkerImport result;
    std::string error;
    if (!fe::MarkersFromFafmapmarkers(text, markers, result, error, reserved)) {
      return up::Fail(L, error);
    }
    lua_newtable(L);
    lua_pushstring(L, "markers");
    lua_newtable(L);
    for (std::size_t i = before; i < markers.size(); ++i) {
      const fe::SaveEntry& marker = markers[i];
      lua_newtable(L);
      up::SetString(L, "name", marker.name);
      up::SetString(L, "type", fe::MarkerTypeOf(marker));
      float x = 0.0f, y = 0.0f, z = 0.0f;
      (void)fe::MarkerPosition(marker, x, y, z);
      const float position[3] = {x, y, z};
      SetArray(L, "position", position, 3);
      const std::vector<double> orientation = fe::ValueNumbers(marker.Get("orientation"));
      double o[3] = {0.0, 0.0, 0.0};
      for (std::size_t k = 0; k < 3 && k < orientation.size(); ++k) {
        o[k] = orientation[k];
      }
      SetArrayD(L, "orientation", o, 3);
      up::SetStringList(L, "links", fe::MarkerLinks(marker));
      lua_rawseti(L, -2, static_cast<int>(i - before) + 1);
    }
    lua_settable(L, -3);
    up::SetNumber(L, "skipped", result.skipped);
    up::SetNumber(L, "links", result.links);
    return 1;
  }

  // ---- .scunits ------------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_ScunitsWrite(lua_State* const L)
  {
    std::vector<fe::SaveUnit> units;
    ForEachEntry(L, 1, [&](const int entry) {
      fe::SaveUnit unit;
      unit.type = up::FieldString(L, entry, "bp");
      unit.orders = up::FieldString(L, entry, "orders");
      unit.platoon = up::FieldString(L, entry, "platoon");
      unit.position[0] = static_cast<float>(up::FieldNumber(L, entry, "x"));
      unit.position[1] = static_cast<float>(up::FieldNumber(L, entry, "y"));
      unit.position[2] = static_cast<float>(up::FieldNumber(L, entry, "z"));
      unit.orientation[1] = static_cast<float>(up::FieldNumber(L, entry, "heading"));
      FieldArray(L, entry, "orientation", unit.orientation, 3);
      if (!unit.type.empty()) {
        units.push_back(std::move(unit));
      }
    });
    return PushText(L, fe::UnitsToScunits(units));
  }

  int __cdecl cfunc_ScunitsRead(lua_State* const L)
  {
    const std::string text = up::ArgString(L, 1, "ed_scunits_read");
    std::vector<fe::SaveUnit> units;
    float centre[3] = {0.0f, 0.0f, 0.0f};
    std::string error;
    if (!fe::UnitsFromScunits(text, units, centre, error)) {
      return up::Fail(L, error);
    }
    bool moved = false;
    const float width = static_cast<float>(up::FieldNumber(L, 2, "width", 0.0));
    const float height = static_cast<float>(up::FieldNumber(L, 2, "height", 0.0));
    if (up::FieldBool(L, 2, "fit", true) && width > 0.0f && height > 0.0f) {
      moved = fe::FitUnitsOnMap(units, centre, width, height);
    }
    lua_newtable(L);
    lua_pushstring(L, "units");
    lua_newtable(L);
    for (std::size_t i = 0; i < units.size(); ++i) {
      const fe::SaveUnit& unit = units[i];
      lua_newtable(L);
      up::SetString(L, "bp", unit.type);
      up::SetString(L, "orders", unit.orders);
      up::SetString(L, "platoon", unit.platoon);
      up::SetNumber(L, "x", unit.position[0]);
      up::SetNumber(L, "y", unit.position[1]);
      up::SetNumber(L, "z", unit.position[2]);
      up::SetNumber(L, "heading", unit.orientation[1]);
      SetArray(L, "orientation", unit.orientation, 3);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
    SetArray(L, "centre", centre, 3);
    up::SetBool(L, "moved", moved);
    return 1;
  }

  // ---- .proppaintset -------------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_PropPaintWrite(lua_State* const L)
  {
    std::vector<fe::PropPaintEntry> entries;
    ForEachEntry(L, 1, [&](const int entry) {
      fe::PropPaintEntry e;
      e.blueprint = up::FieldString(L, entry, "blueprint");
      e.rotationMin = up::FieldInt(L, entry, "rotationMin", e.rotationMin);
      e.rotationMax = up::FieldInt(L, entry, "rotationMax", e.rotationMax);
      e.scaleMin = static_cast<float>(up::FieldNumber(L, entry, "scaleMin", e.scaleMin));
      e.scaleMax = static_cast<float>(up::FieldNumber(L, entry, "scaleMax", e.scaleMax));
      e.chance = up::FieldInt(L, entry, "chance", e.chance);
      entries.push_back(std::move(e));
    });
    return PushText(L, fe::PaintSetToProppaintset(entries));
  }

  int __cdecl cfunc_PropPaintRead(lua_State* const L)
  {
    const std::string text = up::ArgString(L, 1, "ed_proppaintset_read");
    std::vector<fe::PropPaintEntry> entries;
    std::string error;
    if (!fe::PaintSetFromProppaintset(text, entries, error)) {
      return up::Fail(L, error);
    }
    lua_newtable(L);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      lua_newtable(L);
      up::SetString(L, "blueprint", entries[i].blueprint);
      up::SetNumber(L, "rotationMin", entries[i].rotationMin);
      up::SetNumber(L, "rotationMax", entries[i].rotationMax);
      up::SetNumber(L, "scaleMin", entries[i].scaleMin);
      up::SetNumber(L, "scaleMax", entries[i].scaleMax);
      up::SetNumber(L, "chance", entries[i].chance);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
  }

  // ---- .scmsl, .scmst ------------------------------------------------------------------------------------------------------------------------

  fe::LayerSettings ReadLayer(lua_State* const L, const int index)
  {
    fe::LayerSettings layer;
    layer.albedo = up::FieldString(L, index, "albedo");
    layer.normals = up::FieldString(L, index, "normals");
    layer.albedoSize = static_cast<float>(up::FieldNumber(L, index, "albedoSize", layer.albedoSize));
    layer.normalsSize = static_cast<float>(up::FieldNumber(L, index, "normalsSize", layer.normalsSize));
    return layer;
  }

  void PushLayer(lua_State* const L, const fe::LayerSettings& layer)
  {
    lua_newtable(L);
    up::SetString(L, "albedo", layer.albedo);
    up::SetString(L, "normals", layer.normals);
    up::SetNumber(L, "albedoSize", layer.albedoSize);
    up::SetNumber(L, "normalsSize", layer.normalsSize);
  }

  int __cdecl cfunc_ScmslWrite(lua_State* const L)
  {
    return PushText(L, fe::LayerToScmsl(ReadLayer(L, 1)));
  }

  int __cdecl cfunc_ScmslRead(lua_State* const L)
  {
    fe::LayerSettings layer;
    std::string error;
    if (!fe::LayerFromScmsl(up::ArgString(L, 1, "ed_scmsl_read"), layer, error)) {
      return up::Fail(L, error);
    }
    PushLayer(L, layer);
    return 1;
  }

  int __cdecl cfunc_ScmstWrite(lua_State* const L)
  {
    std::vector<fe::LayerSettings> layers;
    if (PushSub(L, 1, "layers") == LUA_TTABLE) {
      ForEachEntry(L, lua_gettop(L), [&](const int entry) { layers.push_back(ReadLayer(L, entry)); });
    }
    lua_settop(L, -2);
    return PushText(L, fe::LayersToScmst(up::FieldString(L, 1, "shader"), layers));
  }

  int __cdecl cfunc_ScmstRead(lua_State* const L)
  {
    std::string shader;
    std::vector<fe::LayerSettings> layers;
    std::string error;
    if (!fe::LayersFromScmst(up::ArgString(L, 1, "ed_scmst_read"), shader, layers, error)) {
      return up::Fail(L, error);
    }
    lua_newtable(L);
    up::SetString(L, "shader", shader);
    lua_pushstring(L, "layers");
    lua_newtable(L);
    for (std::size_t i = 0; i < layers.size(); ++i) {
      PushLayer(L, layers[i]);
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_settable(L, -3);
    return 1;
  }

  // ---- the environment values by name (environment.lua's) ------------------------------------------------------------------------------------

  struct Lane
  {
    std::string name;
    float* value;
  };

  /** `num` (the table at `index`, may be nil) into the lanes named in it; returns how many were set. */
  int ReadNumbers(lua_State* const L, const int index, const std::vector<Lane>& lanes)
  {
    int set = 0;
    for (const Lane& lane : lanes) {
      if (up::PushField(L, index, lane.name.c_str()) == LUA_TNUMBER) {
        *lane.value = static_cast<float>(lua_tonumber(L, -1));
        ++set;
      }
      lua_settop(L, -2);
    }
    return set;
  }

  void WriteNumbers(lua_State* const L, const std::vector<Lane>& lanes)
  {
    for (const Lane& lane : lanes) {
      up::SetNumber(L, lane.name.c_str(), *lane.value);
    }
  }

  void Three(std::vector<Lane>& lanes, const char* const a, const char* const b, const char* const c, float* const v)
  {
    lanes.push_back({a, v});
    lanes.push_back({b, v + 1});
    lanes.push_back({c, v + 2});
  }

  // ---- .scmlighting --------------------------------------------------------------------------------------------------------------------------

  std::vector<Lane> LightingLanes(fe::LightingState& s)
  {
    std::vector<Lane> lanes;
    lanes.push_back({"light_mult", &s.multiplier});
    Three(lanes, "sun_x", "sun_y", "sun_z", s.sunDirection);
    Three(lanes, "ambient_r", "ambient_g", "ambient_b", s.sunAmbience);
    Three(lanes, "sun_r", "sun_g", "sun_b", s.sunColor);
    Three(lanes, "fill_r", "fill_g", "fill_b", s.shadowFill);
    Three(lanes, "spec_r", "spec_g", "spec_b", s.specular);
    lanes.push_back({"spec_a", &s.specular[3]});
    lanes.push_back({"bloom", &s.bloom});
    Three(lanes, "fog_r", "fog_g", "fog_b", s.fogColor);
    lanes.push_back({"fog_start", &s.fogStart});
    lanes.push_back({"fog_end", &s.fogEnd});
    return lanes;
  }

  int __cdecl cfunc_ScmlightingWrite(lua_State* const L)
  {
    fe::LightingState state;
    (void)ReadNumbers(L, 1, LightingLanes(state));
    return PushText(L, fe::LightingToScmlighting(state));
  }

  int __cdecl cfunc_ScmlightingRead(lua_State* const L)
  {
    fe::LightingState state;
    std::string error;
    if (!fe::LightingFromScmlighting(up::ArgString(L, 1, "ed_scmlighting_read"), state, error)) {
      return up::Fail(L, error);
    }
    lua_newtable(L);
    WriteNumbers(L, LightingLanes(state));
    return 1;
  }

  // ---- .scmskybox ----------------------------------------------------------------------------------------------------------------------------

  std::vector<Lane> SkyLanes(fe::SkyboxState& s)
  {
    std::vector<Lane> lanes;
    Three(lanes, "dome_ox", "dome_oy", "dome_oz", s.position);
    lanes.push_back({"dome_height", &s.horizonHeight});
    lanes.push_back({"dome_radius", &s.scale});
    lanes.push_back({"dome_start", &s.subtractHeight});
    lanes.push_back({"horizon_size", &s.zenithHeight});
    Three(lanes, "horizon_r", "horizon_g", "horizon_b", s.horizonColor);
    Three(lanes, "sky_r", "sky_g", "sky_b", s.zenithColor);
    lanes.push_back({"horizon_blend", &s.decalGlow});
    lanes.push_back({"cirrus_mult", &s.cirrusMultiplier});
    Three(lanes, "cirrus_r", "cirrus_g", "cirrus_b", s.cirrusColor);
    static const char* const part[5] = {"_fx", "_fz", "_speed", "_dx", "_dz"};
    for (int i = 0; i < 4; ++i) {
      float* const layer = reinterpret_cast<float*>(&s.cirrus[i]);
      for (int k = 0; k < 5; ++k) {
        lanes.push_back({"cirrus" + std::to_string(i + 1) + part[k], layer + k});
      }
    }
    return lanes;
  }

  int __cdecl cfunc_ScmskyboxWrite(lua_State* const L)
  {
    fe::SkyboxState sky;
    // Values the table does not have keep the file format's defaults (SkyboxState's).
    if (PushSub(L, 1, "num") == LUA_TTABLE) {
      const int num = lua_gettop(L);
      (void)ReadNumbers(L, num, SkyLanes(sky));
      // The dome's tessellation is a whole number (SkyboxState), not a float lane.
      sky.subdivisionsAxis = up::FieldInt(L, num, "dome_w", sky.subdivisionsAxis);
      sky.subdivisionsHeight = up::FieldInt(L, num, "dome_h", sky.subdivisionsHeight);
    }
    lua_settop(L, -2);
    if (PushSub(L, 1, "texts") == LUA_TTABLE) {
      const int texts = lua_gettop(L);
      sky.albedo = up::FieldString(L, texts, "sky_billboards");
      sky.glow = up::FieldString(L, texts, "sky_glow");
      sky.cirrusTexture = up::FieldString(L, texts, "sky_cirrus");
    }
    lua_settop(L, -2);
    const std::vector<double> flat = up::FieldNumberList(L, 1, "billboards");
    for (std::size_t at = 0; at + 10 <= flat.size(); at += 10) {
      fe::SkyboxPlanet planet;
      float* const lane = reinterpret_cast<float*>(&planet);
      for (int k = 0; k < 10; ++k) {
        lane[k] = static_cast<float>(flat[at + static_cast<std::size_t>(k)]);
      }
      sky.planets.push_back(planet);
    }
    return PushText(L, fe::SkyboxToScmskybox(sky));
  }

  int __cdecl cfunc_ScmskyboxRead(lua_State* const L)
  {
    fe::SkyboxState sky;
    std::string error;
    if (!fe::SkyboxFromScmskybox(up::ArgString(L, 1, "ed_scmskybox_read"), sky, error)) {
      return up::Fail(L, error);
    }
    lua_newtable(L);
    lua_pushstring(L, "num");
    lua_newtable(L);
    WriteNumbers(L, SkyLanes(sky));
    up::SetNumber(L, "dome_w", sky.subdivisionsAxis);
    up::SetNumber(L, "dome_h", sky.subdivisionsHeight);
    lua_settable(L, -3);
    lua_pushstring(L, "texts");
    lua_newtable(L);
    up::SetString(L, "sky_billboards", sky.albedo);
    up::SetString(L, "sky_glow", sky.glow);
    up::SetString(L, "sky_cirrus", sky.cirrusTexture);
    lua_settable(L, -3);
    std::vector<double> flat;
    for (const fe::SkyboxPlanet& planet : sky.planets) {
      const float* const lane = reinterpret_cast<const float*>(&planet);
      for (int k = 0; k < 10; ++k) {
        flat.push_back(static_cast<double>(lane[k]));
      }
    }
    up::SetNumberList(L, "billboards", flat);
    return 1;
  }

  // ---- .scmwtr -------------------------------------------------------------------------------------------------------------------------------

  std::vector<Lane> WaterLanes(fe::WaterState& w)
  {
    std::vector<Lane> lanes;
    fe::WaterNumbers& s = w.shader;
    lanes.push_back({"water_elev", &w.elevation});
    lanes.push_back({"water_deep", &w.deep});
    lanes.push_back({"water_abyss", &w.abyss});
    Three(lanes, "w_color_r", "w_color_g", "w_color_b", s.mWaterColor);
    lanes.push_back({"w_lerp_0", &s.mWaterLerp[0]});
    lanes.push_back({"w_lerp_1", &s.mWaterLerp[1]});
    lanes.push_back({"w_refraction", &s.mRefractionScale});
    lanes.push_back({"w_fresnel_bias", &s.mFresnelBias});
    lanes.push_back({"w_fresnel_power", &s.mFresnelPower});
    lanes.push_back({"w_unit_refl", &s.mUnitReflectionAmount});
    lanes.push_back({"w_sky_refl", &s.mSkyReflectionAmount});
    for (int i = 0; i < 4; ++i) {
      lanes.push_back({"w_repeat_" + std::to_string(i), &s.mNormalRepeatRate[i]});
    }
    float* const moves[4] = {s.mNormal1Movement, s.mNormal2Movement, s.mNormal3Movement, s.mNormal4Movement};
    for (int i = 0; i < 4; ++i) {
      lanes.push_back({"w_move" + std::to_string(i + 1) + "_x", moves[i]});
      lanes.push_back({"w_move" + std::to_string(i + 1) + "_z", moves[i] + 1});
    }
    lanes.push_back({"w_shininess", &s.mSunShininess});
    lanes.push_back({"w_strength", &s.mSunStrength});
    Three(lanes, "w_sundir_x", "w_sundir_y", "w_sundir_z", s.mSunDirection);
    Three(lanes, "w_suncol_r", "w_suncol_g", "w_suncol_b", s.mSunColor);
    lanes.push_back({"w_sun_refl", &s.mSunReflectionAmount});
    lanes.push_back({"w_glow", &s.mSunGlow});
    return lanes;
  }

  constexpr const char* kWaterTexts[6] = {"water_cubemap", "water_ramp", "water_wave1", "water_wave2", "water_wave3", "water_wave4"};

  int __cdecl cfunc_ScmwtrWrite(lua_State* const L)
  {
    fe::WaterState water;
    if (PushSub(L, 1, "num") == LUA_TTABLE) {
      const int num = lua_gettop(L);
      (void)ReadNumbers(L, num, WaterLanes(water));
      water.enabled = up::FieldNumber(L, num, "water_on", 0.0) != 0.0;
    }
    lua_settop(L, -2);
    if (PushSub(L, 1, "texts") == LUA_TTABLE) {
      const int texts = lua_gettop(L);
      for (int i = 0; i < 6; ++i) {
        water.textures[i] = up::FieldString(L, texts, kWaterTexts[i]);
      }
    }
    lua_settop(L, -2);
    return PushText(L, fe::WaterToScmwtr(water));
  }

  int __cdecl cfunc_ScmwtrRead(lua_State* const L)
  {
    fe::WaterState water;
    std::string error;
    if (!fe::WaterFromScmwtr(up::ArgString(L, 1, "ed_scmwtr_read"), water, error)) {
      return up::Fail(L, error);
    }
    lua_newtable(L);
    lua_pushstring(L, "num");
    lua_newtable(L);
    up::SetNumber(L, "water_on", water.enabled ? 1.0 : 0.0);
    WriteNumbers(L, WaterLanes(water));
    lua_settable(L, -3);
    lua_pushstring(L, "texts");
    lua_newtable(L);
    for (int i = 0; i < 6; ++i) {
      up::SetString(L, kWaterTexts[i], water.textures[i]);
    }
    lua_settable(L, -3);
    return 1;
  }

  const up::Natives gEdUpImportNatives{{
    {"ed_fafmapmarkers_write", &cfunc_FafmapmarkersWrite, "ed_fafmapmarkers_write(markers, { names, width, height }) -> text"},
    {"ed_fafmapmarkers_read", &cfunc_FafmapmarkersRead, "ed_fafmapmarkers_read(text, { existing, reserved }) -> { markers, skipped, links } | nil, message"},
    {"ed_scunits_write", &cfunc_ScunitsWrite, "ed_scunits_write(units) -> text"},
    {"ed_scunits_read", &cfunc_ScunitsRead, "ed_scunits_read(text, { width, height, fit }) -> { units, centre, moved } | nil, message"},
    {"ed_proppaintset_write", &cfunc_PropPaintWrite, "ed_proppaintset_write(entries) -> text"},
    {"ed_proppaintset_read", &cfunc_PropPaintRead, "ed_proppaintset_read(text) -> entries | nil, message"},
    {"ed_scmsl_write", &cfunc_ScmslWrite, "ed_scmsl_write(layer) -> text"},
    {"ed_scmsl_read", &cfunc_ScmslRead, "ed_scmsl_read(text) -> layer | nil, message"},
    {"ed_scmst_write", &cfunc_ScmstWrite, "ed_scmst_write({ shader, layers }) -> text"},
    {"ed_scmst_read", &cfunc_ScmstRead, "ed_scmst_read(text) -> { shader, layers } | nil, message"},
    {"ed_scmlighting_write", &cfunc_ScmlightingWrite, "ed_scmlighting_write(num) -> text"},
    {"ed_scmlighting_read", &cfunc_ScmlightingRead, "ed_scmlighting_read(text) -> num | nil, message"},
    {"ed_scmskybox_write", &cfunc_ScmskyboxWrite, "ed_scmskybox_write({ num, texts, billboards }) -> text"},
    {"ed_scmskybox_read", &cfunc_ScmskyboxRead, "ed_scmskybox_read(text) -> { num, texts, billboards } | nil, message"},
    {"ed_scmwtr_write", &cfunc_ScmwtrWrite, "ed_scmwtr_write({ num, texts }) -> text"},
    {"ed_scmwtr_read", &cfunc_ScmwtrRead, "ed_scmwtr_read(text) -> { num, texts } | nil, message"},
  }};
} // namespace
