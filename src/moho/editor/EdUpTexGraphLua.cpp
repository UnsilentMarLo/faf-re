// EDITOR: the "up" natives of the material editor (docs/projects/editor/WP10_MATERIAL.md, work package WP10 of EDITOR_PORT_ANALYSIS.md): upstream's colour texture
// graph (src/editor/graph/EdTextureGraph, EdNodeGraph in faf_editor_logic; the DDS encoder EdTextureBake.cpp compiled into the executable) bound to Lua over
// EdUpCommon.h. The logic is upstream's; this unit is the table <-> struct glue, the engine's texture loader, the map's heights and masks as the terrain nodes'
// source, and the polling jobs for a bake. Nothing of upstream's files is edited.
//
// A graph is a handle (an integer) of a document that lives here: the NodeGraph, its evaluator (cached per node by revision, so a slider re-runs only what is downstream
// of it) and the size it is evaluated at. Lua keeps no copy of the graph's data but the snapshot `ed_tex_graph` returns (and the node positions, which are the graph's own).
// Pin and parameter numbers are 0-based as upstream's, node ids are the graph's own (never reused within a graph).
//
//   ed_tex_types()                         -> { { name, title, category, help, output, inputs = { { name, type, initial = {r,g,b,a} } }, outputs = { ... },
//                                                  params = { { name, kind, min, max, choices = {..}, initial = {a,b,c,d}, initialText, help } } }, ... }
//                                             type: 'grey' | 'colour'; kind: 'float' | 'int' | 'bool' | 'colour' | 'choice' | 'text'
//   ed_tex_new([how])                      -> id    how 'start' (default: a noise through a gradient map into Albedo, its height into Normal), 'empty' (the Output node only)
//   ed_tex_free(id)
//   ed_tex_graph(id)                       -> { changes, nodes = { { id, type, x, y, revision, params = { { v = {a,b,c,d}, text }, ... } }, ... },
//                                               links = { { fromNode, fromPin, toNode, toPin }, ... }, output = nodeId, usesTerrain = bool, size }
//   ed_tex_add(id, type, x, y)             -> node id | nil, message
//   ed_tex_remove(id, node)                -> bool   (not the output node)
//   ed_tex_move(id, node, x, y)            -> bool
//   ed_tex_set_param(id, node, param, { v = {a,b,c,d} | n, text = s }) -> bool        param: a 0-based number or the parameter's name
//   ed_tex_link(id, fromNode, fromPin, toNode, toPin) -> 'ok' | 'bad pin' | 'same node' | 'cycle' | 'wrong type'     (replaces the input's link)
//   ed_tex_unlink(id, toNode, toPin)       -> bool
//   ed_tex_save(id)                        -> text          the graph file (JSON)
//   ed_tex_load(id, text)                  -> true | nil, message
//   ed_tex_size(id[, size])                -> size          the size the preview evaluates at (8 .. 4096)
//   ed_tex_terrain(id, on)                 -> true | nil, message     on: capture the open map's heights and stratum masks for the Map nodes now (off: drop them)
//   ed_tex_eval(id[, node])                -> computed nodes, milliseconds     brings the node (or every node) up to date
//   ed_tex_info(id, node)                  -> { warning, millis, outputs = { { size, channels }, ... } }
//   ed_tex_image(id, node, pin, path, size[, channel]) -> true | nil, message    writes the node's output as a size x size uncompressed DDS (channel 'alpha' shows the alpha as grey)
//   ed_tex_result(id, 'Albedo' | 'Normal' | 'Mask', path, size) -> true | nil, message   the output pin's image at the graph's size, as a DDS (as ed_tex_image)
//   ed_tex_lit(id, path, size, mode[, yaw, lightYaw]) -> true | nil, message    a lit preview of the Albedo and Normal outputs: mode 0 a sphere, 1 a 2 x 2 plane
//   ed_tex_bake_start(id, { size, albedo = path, normal = path, textures = bool }) -> job     the graph at `size`, Albedo as a BC3 file (`albedo`), Normal as a BC1 file (`normal`)
//   ed_tex_bake_poll(job)                  -> { done, progress, error, albedoBytes, normalBytes, hasMask, millis }
//   ed_tex_bake_cancel(job) / ed_tex_bake_free(job)
//   ed_tex_bake_mask(job, stratum)         -> true | nil, message     the finished bake's Mask output into stratum 1..8 of the map, resampled to the map's mask grid (the Lua side
//                                             keeps the undo: it reads the mask before)
//   ed_tex_dds_info(path)                  -> { width, height, mips, format } | nil, message       what a written file is (the tests read it back through the engine's own decoder)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "editor/doc/EdMapFile.h"
#include "editor/graph/EdNodeGraph.h"
#include "editor/graph/EdTextureBake.h"
#include "editor/graph/EdTextureGraph.h"
#include "gpg/gal/backends/common/ImageCodec.h"
#include "moho/editor/EdStrata.h"
#include "moho/editor/EdUpCommon.h"
#include "moho/sim/CWldMap.h"
#include "moho/sim/CWldSession.h"
#include "moho/terrain/CWldTerrainRes.h"

namespace
{
  namespace up = moho::editor::up;
  using namespace faf::editor;

  constexpr float kPi = 3.14159265358979323846f;

  // ---- the documents --------------------------------------------------------------------------------------------------------------------------

  struct TexDoc
  {
    std::unique_ptr<NodeGraph> graph;
    std::unique_ptr<TextureGraphEvaluator> eval;
    int size = 256;
    std::shared_ptr<const TerrainSource> terrain;
  };

  /** A texture file of the game's files as an RGBA image (at most 1024 across: a graph's input needn't be finer; after upstream's material editor). */
  bool LoadTexture(const std::string& path, TexImage& out)
  {
    gpg::gal::imaging::ImageCodec* const codec = gpg::gal::imaging::ImageCodec::Instance();
    std::string bytes;
    if (codec == nullptr || !ReadGameFile(path, bytes)) {
      return false;
    }
    gpg::gal::imaging::DecodeOptions options;
    options.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    options.mipLevels = 1;
    gpg::gal::imaging::Image image;
    if (FAILED(codec->Decode(bytes.data(), bytes.size(), options, image)) || image.subresources.empty() || image.format != DXGI_FORMAT_B8G8R8A8_UNORM) {
      return false;
    }
    const auto& level = image.subresources.front();
    const int w = static_cast<int>(level.width);
    const int h = static_cast<int>(level.height);
    if (w <= 0 || h <= 0) {
      return false;
    }
    const int size = std::min(w, 1024);
    out.Reset(size, 4);
    for (int y = 0; y < size; ++y) {
      const int sy = std::min(h - 1, y * h / size);
      const std::uint8_t* const row = image.bytes.data() + level.offset + static_cast<std::size_t>(sy) * level.rowPitch;
      for (int x = 0; x < size; ++x) {
        const std::uint8_t* const p = row + static_cast<std::size_t>(x * w / size) * 4U;
        float* const d = out.Pixel(x, y);
        d[0] = static_cast<float>(p[2]) / 255.0f;
        d[1] = static_cast<float>(p[1]) / 255.0f;
        d[2] = static_cast<float>(p[0]) / 255.0f;
        d[3] = static_cast<float>(p[3]) / 255.0f;
      }
    }
    return true;
  }

  std::map<int, TexDoc>& Docs()
  {
    // Never destroyed (the evaluators hold images; a static destructor at exit would run after the engine is gone).
    static auto* docs = new std::map<int, TexDoc>();
    return *docs;
  }

  int gNextDoc = 1;

  TexDoc* FindDoc(lua_State* const L, const int index)
  {
    const auto found = Docs().find(static_cast<int>(luaL_checknumber(L, index)));
    return found == Docs().end() ? nullptr : &found->second;
  }

  double OptNumber(lua_State* const L, const int index, const double fallback)
  {
    return lua_type(L, index) == LUA_TNUMBER ? static_cast<double>(lua_tonumber(L, index)) : fallback;
  }

  const char* PinName(const PinType t)
  {
    return t == PinType::Color ? "colour" : "grey";
  }

  const char* KindName(const ParamKind k)
  {
    switch (k) {
    case ParamKind::Float: return "float";
    case ParamKind::Int: return "int";
    case ParamKind::Bool: return "bool";
    case ParamKind::Color: return "colour";
    case ParamKind::Choice: return "choice";
    case ParamKind::Text: return "text";
    }
    return "float";
  }

  void PushFour(lua_State* const L, const std::array<float, 4>& v)
  {
    lua_newtable(L);
    for (int i = 0; i < 4; ++i) {
      lua_pushnumber(L, static_cast<lua_Number>(v[static_cast<std::size_t>(i)]));
      lua_rawseti(L, -2, i + 1);
    }
  }

  void SetField(lua_State* const L, const char* const key) // pops the value on top into t[key] (the table below it)
  {
    lua_pushstring(L, key);
    lua_insert(L, -2);
    lua_settable(L, -3);
  }

  void PushPins(lua_State* const L, const std::vector<PinDef>& pins)
  {
    lua_newtable(L);
    for (std::size_t i = 0; i < pins.size(); ++i) {
      lua_newtable(L);
      up::SetString(L, "name", pins[i].name);
      up::SetString(L, "type", PinName(pins[i].type));
      PushFour(L, pins[i].initial);
      SetField(L, "initial");
      lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
  }

  // ---- reading a parameter value from a table --------------------------------------------------------------------------------------------------

  bool ReadValue(lua_State* const L, const int index, ParamValue& value)
  {
    if (lua_type(L, index) == LUA_TNUMBER) {
      value.value = {static_cast<float>(lua_tonumber(L, index)), 0.0f, 0.0f, 0.0f};
      return true;
    }
    if (lua_type(L, index) != LUA_TTABLE) {
      return false;
    }
    value.text = up::FieldString(L, index, "text");
    if (up::PushField(L, index, "v") == LUA_TNUMBER) {
      value.value = {static_cast<float>(lua_tonumber(L, -1)), 0.0f, 0.0f, 0.0f};
    } else if (lua_type(L, -1) == LUA_TTABLE) {
      const int list = lua_gettop(L);
      for (int i = 0; i < 4; ++i) {
        lua_rawgeti(L, list, i + 1);
        if (lua_type(L, -1) == LUA_TNUMBER) {
          value.value[static_cast<std::size_t>(i)] = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_settop(L, -2);
      }
    }
    lua_settop(L, -2);
    return true;
  }

  // ---- natives: the palette and the document ----------------------------------------------------------------------------------------------------

  int __cdecl cfunc_Types(lua_State* const L)
  {
    lua_newtable(L);
    int n = 0;
    for (const auto& type : TextureNodeTypes().All()) {
      lua_newtable(L);
      up::SetString(L, "name", type->name);
      up::SetString(L, "title", type->title);
      up::SetString(L, "category", type->category);
      up::SetString(L, "help", type->help);
      up::SetBool(L, "output", type->isOutput);
      PushPins(L, type->inputs);
      SetField(L, "inputs");
      PushPins(L, type->outputs);
      SetField(L, "outputs");
      lua_newtable(L);
      for (std::size_t i = 0; i < type->params.size(); ++i) {
        const ParamDef& p = type->params[i];
        lua_newtable(L);
        up::SetString(L, "name", p.name);
        up::SetString(L, "kind", KindName(p.kind));
        up::SetNumber(L, "min", p.min);
        up::SetNumber(L, "max", p.max);
        up::SetString(L, "initialText", p.initialText);
        up::SetString(L, "help", p.help);
        up::SetStringList(L, "choices", p.choices);
        PushFour(L, p.initial);
        SetField(L, "initial");
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
      }
      SetField(L, "params");
      lua_rawseti(L, -2, ++n);
    }
    return 1;
  }

  int __cdecl cfunc_New(lua_State* const L)
  {
    const std::string how = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : "start";
    TexDoc doc;
    doc.graph = std::make_unique<NodeGraph>(TextureNodeTypes());
    if (how == "empty") {
      doc.graph->AddNode("Output", 520.0f, 0.0f);
    } else {
      StartTextureGraph(*doc.graph);
    }
    doc.eval = std::make_unique<TextureGraphEvaluator>(*doc.graph);
    doc.eval->SetSize(doc.size);
    doc.eval->SetTextureLoader(LoadTexture);
    const int id = gNextDoc++;
    Docs().emplace(id, std::move(doc));
    lua_pushnumber(L, id);
    return 1;
  }

  int __cdecl cfunc_Free(lua_State* const L)
  {
    Docs().erase(static_cast<int>(luaL_checknumber(L, 1)));
    return 0;
  }

  bool UsesTerrain(const NodeGraph& graph)
  {
    for (const GraphNode& node : graph.Nodes()) {
      if (node.type->category == "Map") {
        return true;
      }
    }
    return false;
  }

  int __cdecl cfunc_Graph(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    if (doc == nullptr) {
      return up::Fail(L, "no such graph");
    }
    const NodeGraph& graph = *doc->graph;
    lua_newtable(L);
    up::SetNumber(L, "changes", static_cast<double>(graph.Changes()));
    up::SetNumber(L, "output", graph.OutputNode());
    up::SetNumber(L, "size", doc->size);
    up::SetBool(L, "usesTerrain", UsesTerrain(graph));
    lua_newtable(L);
    int n = 0;
    for (const GraphNode& node : graph.Nodes()) {
      lua_newtable(L);
      up::SetNumber(L, "id", node.id);
      up::SetString(L, "type", node.type->name);
      up::SetNumber(L, "x", node.x);
      up::SetNumber(L, "y", node.y);
      up::SetNumber(L, "revision", static_cast<double>(node.revision));
      lua_newtable(L);
      for (std::size_t i = 0; i < node.params.size(); ++i) {
        lua_newtable(L);
        PushFour(L, node.params[i].value);
        SetField(L, "v");
        up::SetString(L, "text", node.params[i].text);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
      }
      SetField(L, "params");
      lua_rawseti(L, -2, ++n);
    }
    SetField(L, "nodes");
    lua_newtable(L);
    n = 0;
    for (const GraphLink& link : graph.Links()) {
      lua_newtable(L);
      up::SetNumber(L, "fromNode", link.fromNode);
      up::SetNumber(L, "fromPin", link.fromPin);
      up::SetNumber(L, "toNode", link.toNode);
      up::SetNumber(L, "toPin", link.toPin);
      lua_rawseti(L, -2, ++n);
    }
    SetField(L, "links");
    return 1;
  }

  int __cdecl cfunc_Add(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    const std::string type = up::ArgString(L, 2, "ed_tex_add");
    if (doc == nullptr) {
      return up::Fail(L, "no such graph");
    }
    const NodeType* const def = TextureNodeTypes().Find(type);
    if (def == nullptr) {
      return up::Fail(L, "unknown node type " + type);
    }
    if (def->isOutput && doc->graph->OutputNode() != 0) {
      return up::Fail(L, "the graph has its output already");
    }
    const int id = doc->graph->AddNode(type, static_cast<float>(OptNumber(L, 3, 0.0)), static_cast<float>(OptNumber(L, 4, 0.0)));
    if (id == 0) {
      return up::Fail(L, "could not add " + type);
    }
    lua_pushnumber(L, id);
    return 1;
  }

  int __cdecl cfunc_Remove(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    lua_pushboolean(L, doc != nullptr && doc->graph->RemoveNode(static_cast<int>(luaL_checknumber(L, 2))) ? 1 : 0);
    return 1;
  }

  int __cdecl cfunc_Move(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    const int node = static_cast<int>(luaL_checknumber(L, 2));
    if (doc == nullptr || doc->graph->Find(node) == nullptr) {
      lua_pushboolean(L, 0);
      return 1;
    }
    doc->graph->MoveNode(node, static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)));
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_SetParam(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    const GraphNode* const node = doc != nullptr ? doc->graph->Find(static_cast<int>(luaL_checknumber(L, 2))) : nullptr;
    if (node == nullptr) {
      lua_pushboolean(L, 0);
      return 1;
    }
    int param = -1;
    if (lua_type(L, 3) == LUA_TNUMBER) {
      param = static_cast<int>(lua_tonumber(L, 3));
    } else if (lua_type(L, 3) == LUA_TSTRING) {
      for (std::size_t i = 0; i < node->type->params.size(); ++i) {
        if (node->type->params[i].name == lua_tostring(L, 3)) {
          param = static_cast<int>(i);
        }
      }
    }
    ParamValue value;
    if (param < 0 || static_cast<std::size_t>(param) >= node->params.size() || !ReadValue(L, 4, value)) {
      lua_pushboolean(L, 0);
      return 1;
    }
    // A table without `v` or `text` keeps what the node has.
    const ParamValue& old = node->params[static_cast<std::size_t>(param)];
    if (lua_type(L, 4) == LUA_TTABLE) {
      if (up::PushField(L, 4, "v") == LUA_TNIL) {
        value.value = old.value;
      }
      lua_settop(L, -2);
      if (up::PushField(L, 4, "text") == LUA_TNIL) {
        value.text = old.text;
      }
      lua_settop(L, -2);
    }
    doc->graph->SetParam(node->id, static_cast<std::size_t>(param), value);
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_Link(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    if (doc == nullptr) {
      return up::Fail(L, "no such graph");
    }
    const LinkResult result = doc->graph->Link(static_cast<int>(luaL_checknumber(L, 2)), static_cast<int>(luaL_checknumber(L, 3)), static_cast<int>(luaL_checknumber(L, 4)),
                                               static_cast<int>(luaL_checknumber(L, 5)));
    switch (result) {
    case LinkResult::Ok: lua_pushstring(L, "ok"); break;
    case LinkResult::BadPin: lua_pushstring(L, "bad pin"); break;
    case LinkResult::SameNode: lua_pushstring(L, "same node"); break;
    case LinkResult::Cycle: lua_pushstring(L, "cycle"); break;
    case LinkResult::WrongType: lua_pushstring(L, "wrong type"); break;
    }
    return 1;
  }

  int __cdecl cfunc_Unlink(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    lua_pushboolean(L, doc != nullptr && doc->graph->Unlink(static_cast<int>(luaL_checknumber(L, 2)), static_cast<int>(luaL_checknumber(L, 3))) ? 1 : 0);
    return 1;
  }

  int __cdecl cfunc_Save(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    if (doc == nullptr) {
      return up::Fail(L, "no such graph");
    }
    const std::string text = doc->graph->Save();
    lua_pushlstring(L, text.data(), text.size());
    return 1;
  }

  int __cdecl cfunc_Load(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    const std::string text = up::ArgString(L, 2, "ed_tex_load");
    if (doc == nullptr) {
      return up::Fail(L, "no such graph");
    }
    std::string error;
    if (!doc->graph->Load(text, error)) {
      return up::Fail(L, error.empty() ? "the graph file could not be read" : error);
    }
    doc->eval->Invalidate();
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_Size(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    if (doc == nullptr) {
      return up::Fail(L, "no such graph");
    }
    if (lua_type(L, 2) == LUA_TNUMBER) {
      doc->eval->SetSize(static_cast<int>(lua_tonumber(L, 2)));
      doc->size = doc->eval->Size();
    }
    lua_pushnumber(L, doc->size);
    return 1;
  }

  // ---- the map's heights and masks ----------------------------------------------------------------------------------------------------------------

  std::shared_ptr<TerrainSource> CaptureTerrain(std::string& why)
  {
    moho::CWldSession* const session = moho::WLD_GetActiveSession();
    if (session == nullptr || session->mWldMap == nullptr || session->mWldMap->mTerrainRes == nullptr) {
      why = "no map is open";
      return nullptr;
    }
    moho::CHeightField* const field = session->mWldMap->mTerrainRes->GetHeightField();
    if (field == nullptr || field->Width() < 2 || field->Height() < 2) {
      why = "the map has no height field";
      return nullptr;
    }
    auto terrain = std::make_shared<TerrainSource>();
    terrain->width = field->Width();
    terrain->height = field->Height();
    terrain->units = 1.0f; // a sample per game unit
    terrain->heights.resize(static_cast<std::size_t>(terrain->width) * static_cast<std::size_t>(terrain->height));
    for (int z = 0; z < terrain->height; ++z) {
      for (int x = 0; x < terrain->width; ++x) {
        terrain->heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(terrain->width) + static_cast<std::size_t>(x)] = static_cast<float>(field->GetHeightAt(x, z)) / 128.0f;
      }
    }
    // The eight stratum masks, one texel's eight weights in one word, byte k for engine stratum k (stratum k + 1 of the Map Mask node).
    std::vector<std::uint8_t> lane;
    int w = 0;
    int h = 0;
    for (int s = 0; s < 8; ++s) {
      if (!moho::editor::StratumMaskRead(s, lane, w, h) || w <= 0 || h <= 0 || lane.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {
        break;
      }
      if (s == 0) {
        terrain->maskWidth = w;
        terrain->maskHeight = h;
        terrain->masks.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
      }
      for (std::size_t i = 0; i < terrain->masks.size(); ++i) {
        terrain->masks[i] |= static_cast<std::uint64_t>(lane[i]) << (8 * s);
      }
    }
    return terrain;
  }

  int __cdecl cfunc_Terrain(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    if (doc == nullptr) {
      return up::Fail(L, "no such graph");
    }
    if (lua_toboolean(L, 2) == 0) {
      doc->terrain.reset();
      doc->eval->SetTerrain(nullptr);
      lua_pushboolean(L, 1);
      return 1;
    }
    std::string why;
    std::shared_ptr<TerrainSource> terrain = CaptureTerrain(why);
    if (terrain == nullptr) {
      return up::Fail(L, why);
    }
    doc->terrain = std::move(terrain);
    doc->eval->SetTerrain(doc->terrain);
    lua_pushboolean(L, 1);
    return 1;
  }

  // ---- evaluation and pictures --------------------------------------------------------------------------------------------------------------------

  int __cdecl cfunc_Eval(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    if (doc == nullptr) {
      return up::Fail(L, "no such graph");
    }
    const auto start = std::chrono::steady_clock::now();
    const int computed = lua_type(L, 2) == LUA_TNUMBER ? doc->eval->Evaluate(static_cast<int>(lua_tonumber(L, 2))) : doc->eval->EvaluateAll();
    lua_pushnumber(L, computed);
    lua_pushnumber(L, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return 2;
  }

  int __cdecl cfunc_Info(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    const int node = static_cast<int>(luaL_checknumber(L, 2));
    const GraphNode* const n = doc != nullptr ? doc->graph->Find(node) : nullptr;
    if (n == nullptr) {
      return up::Fail(L, "no such node");
    }
    lua_newtable(L);
    up::SetString(L, "warning", doc->eval->Warning(node));
    up::SetNumber(L, "millis", doc->eval->Millis(node));
    lua_newtable(L);
    for (std::size_t pin = 0; pin < n->type->outputs.size(); ++pin) {
      lua_newtable(L);
      if (const TexImagePtr image = doc->eval->Output(node, static_cast<int>(pin))) {
        up::SetNumber(L, "size", image->size);
        up::SetNumber(L, "channels", image->channels);
      }
      lua_rawseti(L, -2, static_cast<int>(pin) + 1);
    }
    SetField(L, "outputs");
    return 1;
  }

  /** `image` at `size` x size as an opaque (or alpha-as-grey) RGBA image, for a picture on screen. */
  TexImage ForScreen(const TexImage& image, const int size, const bool alphaAsGrey)
  {
    TexImage out;
    out.Reset(size, 4);
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        float c[4];
        image.Sample((static_cast<float>(x) + 0.5f) / static_cast<float>(size), (static_cast<float>(y) + 0.5f) / static_cast<float>(size), c);
        float* p = out.Pixel(x, y);
        if (alphaAsGrey) {
          p[0] = p[1] = p[2] = c[3];
        } else {
          p[0] = c[0];
          p[1] = c[1];
          p[2] = c[2];
        }
        p[3] = 1.0f;
      }
    }
    return out;
  }

  bool WriteFile(const std::string& path, const std::vector<std::uint8_t>& bytes)
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
  }

  int WriteScreenImage(lua_State* const L, const TexImage& image, const std::string& path, const int size, const bool alphaAsGrey)
  {
    const TexImage shown = ForScreen(image, std::clamp(size, 1, 2048), alphaAsGrey);
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!EncodeDds(shown, DdsKind::Bgra, bytes, error)) {
      return up::Fail(L, error);
    }
    if (!WriteFile(path, bytes)) {
      return up::Fail(L, "cannot write " + path);
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_Image(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    const int node = static_cast<int>(luaL_checknumber(L, 2));
    const int pin = static_cast<int>(luaL_checknumber(L, 3));
    const std::string path = up::ArgString(L, 4, "ed_tex_image");
    const int size = static_cast<int>(luaL_checknumber(L, 5));
    const bool alpha = lua_type(L, 6) == LUA_TSTRING && std::string(lua_tostring(L, 6)) == "alpha";
    if (doc == nullptr) {
      return up::Fail(L, "no such graph");
    }
    const TexImagePtr image = doc->eval->Output(node, pin);
    if (image == nullptr) {
      return up::Fail(L, "the node has no output yet");
    }
    return WriteScreenImage(L, *image, path, size, alpha);
  }

  int __cdecl cfunc_Result(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    const std::string pin = up::ArgString(L, 2, "ed_tex_result");
    const std::string path = up::ArgString(L, 3, "ed_tex_result");
    const int size = static_cast<int>(luaL_checknumber(L, 4));
    if (doc == nullptr) {
      return up::Fail(L, "no such graph");
    }
    const TexImagePtr image = doc->eval->Result(pin);
    if (image == nullptr) {
      return up::Fail(L, "nothing is linked to the output's " + pin);
    }
    return WriteScreenImage(L, *image, path, size, false);
  }

  std::uint32_t Bgra(const float r, const float g, const float b)
  {
    const auto byte = [](const float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return 0xFF000000U | (byte(r) << 16) | (byte(g) << 8) | byte(b);
  }

  /** A lit preview of a stratum's albedo and normal maps (after upstream's RenderMaterialPreview, EdMaterialEditor.cpp): `mode` 0 a sphere, 1 a 2 x 2 plane. */
  std::vector<std::uint32_t> RenderLit(const TexImage* albedo, const TexImage* normal, const int mode, const int size, const float yaw, const float lightYaw)
  {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size);
    const float lx = std::sin(lightYaw) * 0.7f;
    const float lz = std::cos(lightYaw) * 0.7f;
    const float ly = 0.6f;
    const float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    const float light[3] = {lx / ll, ly / ll, lz / ll};
    const auto shade = [&](const float u, const float v, const float (&n)[3], const float (&t)[3], const float (&b)[3], const float facing) {
      float base[4] = {0.5f, 0.5f, 0.5f, 1.0f};
      if (albedo != nullptr) {
        albedo->Sample(u, v, base);
      }
      float tn[4] = {0.5f, 0.5f, 1.0f, 1.0f};
      if (normal != nullptr) {
        normal->Sample(u, v, tn);
      }
      const float nx = tn[0] * 2.0f - 1.0f;
      const float ny = tn[1] * 2.0f - 1.0f;
      const float nz = tn[2] * 2.0f - 1.0f;
      float w[3];
      for (int i = 0; i < 3; ++i) {
        w[i] = t[i] * nx + b[i] * ny + n[i] * nz;
      }
      const float wl = std::max(std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]), 1e-5f);
      const float diffuse = std::max(0.0f, (w[0] * light[0] + w[1] * light[1] + w[2] * light[2]) / wl);
      const float lit = (0.4f + 0.8f * diffuse) * facing;
      return Bgra(base[0] * lit, base[1] * lit, base[2] * lit);
    };
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        std::uint32_t& out = pixels[static_cast<std::size_t>(y) * size + x];
        const float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        out = Bgra(0.10f + 0.06f * (1.0f - fy), 0.10f + 0.06f * (1.0f - fy), 0.12f + 0.07f * (1.0f - fy));
        if (mode == 1) {
          const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f;
          const float v = fy * 2.0f;
          const float n[3] = {0.0f, 1.0f, 0.0f};
          const float t[3] = {1.0f, 0.0f, 0.0f};
          const float b[3] = {0.0f, 0.0f, 1.0f};
          out = shade(u, v, n, t, b, 1.0f);
          continue;
        }
        const float sx = ((static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f) / 0.9f;
        const float sy = -(fy * 2.0f - 1.0f) / 0.9f;
        const float r2 = sx * sx + sy * sy;
        if (r2 > 1.0f) {
          continue;
        }
        const float sz = std::sqrt(1.0f - r2);
        const float n[3] = {sx, sy, sz};
        const float phi = std::atan2(sx, sz) + yaw;
        const float u = phi / (2.0f * kPi) * 2.0f;
        const float v = std::acos(std::clamp(sy, -1.0f, 1.0f)) / kPi;
        const float t[3] = {std::cos(phi - yaw), 0.0f, -std::sin(phi - yaw)};
        const float b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
        out = shade(u, v, n, t, b, 0.85f + 0.15f * sz);
      }
    }
    return pixels;
  }

  int __cdecl cfunc_Lit(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    const std::string path = up::ArgString(L, 2, "ed_tex_lit");
    const int size = std::clamp(static_cast<int>(luaL_checknumber(L, 3)), 16, 1024);
    const int mode = static_cast<int>(luaL_checknumber(L, 4));
    const float yaw = static_cast<float>(OptNumber(L, 5, 0.6));
    const float lightYaw = static_cast<float>(OptNumber(L, 6, -0.7));
    if (doc == nullptr) {
      return up::Fail(L, "no such graph");
    }
    const TexImagePtr albedo = doc->eval->Result("Albedo");
    const TexImagePtr normal = doc->eval->Result("Normal");
    const std::vector<std::uint32_t> pixels = RenderLit(albedo.get(), normal.get(), mode, size, yaw, lightYaw);
    TexImage image;
    image.Reset(size, 4);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      float* p = image.data.data() + i * 4;
      p[0] = static_cast<float>((pixels[i] >> 16) & 0xFFU) / 255.0f;
      p[1] = static_cast<float>((pixels[i] >> 8) & 0xFFU) / 255.0f;
      p[2] = static_cast<float>(pixels[i] & 0xFFU) / 255.0f;
      p[3] = 1.0f;
    }
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!EncodeDds(image, DdsKind::Bgra, bytes, error)) {
      return up::Fail(L, error);
    }
    if (!WriteFile(path, bytes)) {
      return up::Fail(L, "cannot write " + path);
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  // ---- a bake as a job ----------------------------------------------------------------------------------------------------------------------------

  struct BakeResult
  {
    bool ok = false;
    std::string error;
    std::size_t albedoBytes = 0;
    std::size_t normalBytes = 0;
    TexImagePtr mask; ///< the Mask output at the bake size (null when unlinked)
    double millis = 0.0;
  };

  up::JobSet<BakeResult>& Bakes()
  {
    static auto* bakes = new up::JobSet<BakeResult>();
    return *bakes;
  }

  /** The texture paths a graph names (its Text parameters): they are read on the Lua thread before the job starts, so the worker never touches the game's files. */
  std::set<std::string> TexturePaths(const NodeGraph& graph)
  {
    std::set<std::string> paths;
    for (const GraphNode& node : graph.Nodes()) {
      for (std::size_t i = 0; i < node.params.size() && i < node.type->params.size(); ++i) {
        if (node.type->params[i].kind == ParamKind::Text && !node.params[i].text.empty()) {
          paths.insert(node.params[i].text);
        }
      }
    }
    return paths;
  }

  int __cdecl cfunc_BakeStart(lua_State* const L)
  {
    TexDoc* const doc = FindDoc(L, 1);
    if (doc == nullptr) {
      return up::Fail(L, "no such graph");
    }
    const int size = std::clamp(up::FieldInt(L, 2, "size", 1024), 8, 4096);
    const std::string albedoPath = up::FieldString(L, 2, "albedo");
    const std::string normalPath = up::FieldString(L, 2, "normal");
    // The worker's own copy of the graph, the textures it names (read here) and the map data (shared, never changed).
    auto graph = std::make_shared<NodeGraph>(*doc->graph);
    auto textures = std::make_shared<std::map<std::string, TexImage>>();
    for (const std::string& path : TexturePaths(*graph)) {
      TexImage image;
      if (LoadTexture(path, image)) {
        textures->emplace(path, std::move(image));
      }
    }
    std::shared_ptr<const TerrainSource> terrain = doc->terrain;
    const int job = Bakes().Start([graph, textures, terrain, size, albedoPath, normalPath](up::JobContext& context) {
      BakeResult out;
      const auto start = std::chrono::steady_clock::now();
      TextureGraphEvaluator eval(*graph);
      eval.SetSize(size);
      eval.SetTextureLoader([textures](const std::string& path, TexImage& image) {
        const auto found = textures->find(path);
        if (found == textures->end()) {
          return false;
        }
        image = found->second;
        return true;
      });
      eval.SetTerrain(terrain);
      context.Report(0.1f);
      const TexImagePtr albedo = eval.Result("Albedo");
      if (albedo == nullptr) {
        out.error = "link something to the output's Albedo first";
        return out;
      }
      if (context.Cancelled()) {
        out.error = "cancelled";
        return out;
      }
      const TexImagePtr normal = eval.Result("Normal");
      out.mask = eval.Result("Mask");
      context.Report(0.5f);
      std::vector<std::uint8_t> bytes;
      std::string error;
      if (!albedoPath.empty()) {
        if (!EncodeDds(*albedo, DdsKind::Bc3, bytes, error)) {
          out.error = "albedo: " + error;
          return out;
        }
        if (!WriteFile(albedoPath, bytes)) {
          out.error = "cannot write " + albedoPath;
          return out;
        }
        out.albedoBytes = bytes.size();
      }
      context.Report(0.8f);
      if (!normalPath.empty() && normal != nullptr) {
        if (!EncodeDds(*normal, DdsKind::Bc1, bytes, error)) {
          out.error = "normal: " + error;
          return out;
        }
        if (!WriteFile(normalPath, bytes)) {
          out.error = "cannot write " + normalPath;
          return out;
        }
        out.normalBytes = bytes.size();
      }
      out.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      out.ok = true;
      return out;
    });
    lua_pushnumber(L, job);
    return 1;
  }

  int __cdecl cfunc_BakePoll(lua_State* const L)
  {
    const int job = static_cast<int>(luaL_checknumber(L, 1));
    const up::JobStatus status = Bakes().Poll(job);
    if (!status.known) {
      return up::Fail(L, "no such job");
    }
    up::PushJobStatus(L, status);
    if (status.done) {
      if (const std::optional<BakeResult> result = Bakes().Get(job)) {
        up::SetBool(L, "ok", result->ok);
        up::SetString(L, "error", result->error);
        up::SetNumber(L, "albedoBytes", static_cast<double>(result->albedoBytes));
        up::SetNumber(L, "normalBytes", static_cast<double>(result->normalBytes));
        up::SetBool(L, "hasMask", result->mask != nullptr);
        up::SetNumber(L, "millis", result->millis);
      }
    }
    return 1;
  }

  int __cdecl cfunc_BakeCancel(lua_State* const L)
  {
    Bakes().Cancel(static_cast<int>(luaL_checknumber(L, 1)));
    return 0;
  }

  int __cdecl cfunc_BakeFree(lua_State* const L)
  {
    Bakes().Free(static_cast<int>(luaL_checknumber(L, 1)));
    return 0;
  }

  int __cdecl cfunc_BakeMask(lua_State* const L)
  {
    const int job = static_cast<int>(luaL_checknumber(L, 1));
    const int stratum = static_cast<int>(luaL_checknumber(L, 2));
    if (stratum < 1 || stratum > 8) {
      return up::Fail(L, "the stratum is 1 to 8");
    }
    const std::optional<BakeResult> result = Bakes().Get(job);
    if (!result || result->mask == nullptr) {
      return up::Fail(L, "the bake made no mask (link something to the output's Mask)");
    }
    std::vector<std::uint8_t> current;
    int w = 0;
    int h = 0;
    if (!moho::editor::StratumMaskRead(stratum - 1, current, w, h) || w <= 0 || h <= 0) {
      return up::Fail(L, "the map has no stratum masks");
    }
    const TexImage& mask = *result->mask;
    std::vector<std::uint8_t> values(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        float c[4];
        mask.Sample((static_cast<float>(x) + 0.5f) / static_cast<float>(w), (static_cast<float>(y) + 0.5f) / static_cast<float>(h), c);
        values[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
            static_cast<std::uint8_t>(std::lround(std::clamp(c[0], 0.0f, 1.0f) * 255.0f));
      }
    }
    if (!moho::editor::StratumMaskWrite(stratum - 1, values.data(), w, h)) {
      return up::Fail(L, "the mask could not be written");
    }
    lua_pushboolean(L, 1);
    return 1;
  }

  int __cdecl cfunc_DdsInfo(lua_State* const L)
  {
    const std::string path = up::ArgString(L, 1, "ed_tex_dds_info");
    gpg::gal::imaging::ImageCodec* const codec = gpg::gal::imaging::ImageCodec::Instance();
    std::string bytes;
    if (codec == nullptr) {
      return up::Fail(L, "no image codec");
    }
    {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return up::Fail(L, "cannot read " + path);
      }
      bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    gpg::gal::imaging::DecodeOptions options;
    options.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    options.mipLevels = 0;
    gpg::gal::imaging::Image image;
    if (FAILED(codec->Decode(bytes.data(), bytes.size(), options, image)) || image.subresources.empty()) {
      return up::Fail(L, "the engine's decoder cannot read " + path);
    }
    lua_newtable(L);
    up::SetNumber(L, "width", image.subresources.front().width);
    up::SetNumber(L, "height", image.subresources.front().height);
    up::SetNumber(L, "mips", static_cast<double>(image.subresources.size()));
    up::SetNumber(L, "bytes", static_cast<double>(bytes.size()));
    // A pixel of the first level (B8G8R8A8) for the tests: the centre.
    const auto& level = image.subresources.front();
    const std::uint8_t* const p = image.bytes.data() + level.offset + (level.height / 2) * level.rowPitch + (level.width / 2) * 4U;
    up::SetNumber(L, "centreR", p[2]);
    up::SetNumber(L, "centreG", p[1]);
    up::SetNumber(L, "centreB", p[0]);
    up::SetNumber(L, "centreA", p[3]);
    return 1;
  }

  const up::Natives gEdUpTexGraphNatives{{
      {"ed_tex_types", &cfunc_Types, "ed_tex_types() -> node types"},
      {"ed_tex_new", &cfunc_New, "ed_tex_new([how]) -> graph id"},
      {"ed_tex_free", &cfunc_Free, "ed_tex_free(id)"},
      {"ed_tex_graph", &cfunc_Graph, "ed_tex_graph(id) -> snapshot"},
      {"ed_tex_add", &cfunc_Add, "ed_tex_add(id, type, x, y) -> node id"},
      {"ed_tex_remove", &cfunc_Remove, "ed_tex_remove(id, node) -> bool"},
      {"ed_tex_move", &cfunc_Move, "ed_tex_move(id, node, x, y) -> bool"},
      {"ed_tex_set_param", &cfunc_SetParam, "ed_tex_set_param(id, node, param, value) -> bool"},
      {"ed_tex_link", &cfunc_Link, "ed_tex_link(id, fromNode, fromPin, toNode, toPin) -> result"},
      {"ed_tex_unlink", &cfunc_Unlink, "ed_tex_unlink(id, toNode, toPin) -> bool"},
      {"ed_tex_save", &cfunc_Save, "ed_tex_save(id) -> text"},
      {"ed_tex_load", &cfunc_Load, "ed_tex_load(id, text) -> true | nil, message"},
      {"ed_tex_size", &cfunc_Size, "ed_tex_size(id[, size]) -> size"},
      {"ed_tex_terrain", &cfunc_Terrain, "ed_tex_terrain(id, on) -> true | nil, message"},
      {"ed_tex_eval", &cfunc_Eval, "ed_tex_eval(id[, node]) -> computed, milliseconds"},
      {"ed_tex_info", &cfunc_Info, "ed_tex_info(id, node) -> info"},
      {"ed_tex_image", &cfunc_Image, "ed_tex_image(id, node, pin, path, size[, channel]) -> true | nil, message"},
      {"ed_tex_result", &cfunc_Result, "ed_tex_result(id, pin, path, size) -> true | nil, message"},
      {"ed_tex_lit", &cfunc_Lit, "ed_tex_lit(id, path, size, mode[, yaw, lightYaw]) -> true | nil, message"},
      {"ed_tex_bake_start", &cfunc_BakeStart, "ed_tex_bake_start(id, opts) -> job"},
      {"ed_tex_bake_poll", &cfunc_BakePoll, "ed_tex_bake_poll(job) -> status"},
      {"ed_tex_bake_cancel", &cfunc_BakeCancel, "ed_tex_bake_cancel(job)"},
      {"ed_tex_bake_free", &cfunc_BakeFree, "ed_tex_bake_free(job)"},
      {"ed_tex_bake_mask", &cfunc_BakeMask, "ed_tex_bake_mask(job, stratum) -> true | nil, message"},
      {"ed_tex_dds_info", &cfunc_DdsInfo, "ed_tex_dds_info(path) -> info"},
  }};
} // namespace
