// EDITOR: the render-side console variables and the ed_render_stats() native (see EdRenderStats.h).
#include "moho/editor/EdRenderStats.h"

#include "lua/LuaPrimitives.h"
#include "lua/LuaRuntimeTypes.h"
#include "moho/console/CConCommand.h"
#include "moho/editor/EdShadowCache.h"
#include "moho/lua/CScrLuaBinder.h"
#include "moho/lua/CScrLuaInitForm.h"

namespace moho
{
  bool ren_ShadowCache = true;
  bool ren_ShadowCacheTrees = false;
  int ren_ShadowCacheMaxAge = 300;
  bool ren_WaterSkipDisabled = true;
  bool ui_CullOffscreen = true;
  bool ui_SkipTransparent = true;
  bool ren_AtlasFastLookup = true;
} // namespace moho

namespace
{
  moho::TConVar<bool> gTConVar_ren_ShadowCache(
    "ren_ShadowCache",
    "Skip the shadow pass while the light camera, the shadow settings and the scene are the same as when the shadow map was drawn (1 = on, 0 = draw it every frame)",
    &moho::ren_ShadowCache
  );
  moho::TConVar<bool> gTConVar_ren_ShadowCacheTrees(
    "ren_ShadowCacheTrees",
    "1 = a shadow map drawn with wind swaying trees may be reused too (the sway moves less than a texel); 0 = such a map is drawn every frame",
    &moho::ren_ShadowCacheTrees
  );
  moho::TConVar<int> gTConVar_ren_ShadowCacheMaxAge(
    "ren_ShadowCacheMaxAge",
    "A reused shadow map is drawn again after this many frames, whatever the checks say (0 = never)",
    &moho::ren_ShadowCacheMaxAge
  );
  moho::TConVar<bool> gTConVar_ren_WaterSkipDisabled(
    "ren_WaterSkipDisabled",
    "1 = a map with its water switched off skips the reflection pass, the water mask, the refraction copy and the water surface",
    &moho::ren_WaterSkipDisabled
  );
  moho::TConVar<bool> gTConVar_ui_CullOffscreen(
    "ui_CullOffscreen",
    "1 = Maui bitmaps, borders and texts that lie completely outside the window are not built and queued",
    &moho::ui_CullOffscreen
  );
  moho::TConVar<bool> gTConVar_ui_SkipTransparent(
    "ui_SkipTransparent",
    "1 = Maui bitmaps and borders with alpha 0 are not drawn",
    &moho::ui_SkipTransparent
  );
  moho::TConVar<bool> gTConVar_ren_AtlasFastLookup(
    "ren_AtlasFastLookup",
    "1 = the texture atlas of the prim batcher finds a texture it holds by its address before it walks its ordered set",
    &moho::ren_AtlasFastLookup
  );

  void Put(lua_State* const L, const char* const key, const double value)
  {
    lua_pushstring(L, key);
    lua_pushnumber(L, static_cast<lua_Number>(value));
    lua_settable(L, -3);
  }

  // ed_render_stats() -> table of the last complete frame (counts are per frame, ms are CPU time on the render thread):
  //   drawCalls, primitives, vertices                  every draw the graphics backend got (whatever issued it)
  //   batcherFlushes, batcherVertices, batcherIndices  the prim batcher (UI, text, overlay): one flush is one draw call; vertices / 4 = quads
  //   overlayMs, overlayFlushes, overlayVertices       the editor's native overlay (slope view, grids, lines, icons): its share of the above
  //   uiMs                                             Maui: CUIManager::RenderFrames + DrawUI
  //   uiBitmapsDrawn / Culled / Transparent, uiBordersDrawn / Culled, uiTextsDrawn / Culled
  //   atlasLookups, atlasFastHits, atlasAdds, atlasResets
  //   shadowMs, shadowSceneMs, shadowRendered, shadowReused     the shadow pass of the frame (1 / 0)
  //   waterSkipped                                     1 when the water passes were left out
  //   renderMs                                         WRenViewport::Render, all heads
  // and since the start: frames, shadowHits, shadowMisses, waterSkippedFrames
  int __cdecl cfunc_EdRenderStats(lua_State* const L)
  {
    namespace rs = moho::editor::renderstats;
    const rs::Frame& f = rs::gLast;
    lua_newtable(L);
    Put(L, "frames", static_cast<double>(rs::gFrames));
    Put(L, "drawCalls", f.drawCalls);
    Put(L, "primitives", f.primitives);
    Put(L, "vertices", f.vertices);
    Put(L, "batcherFlushes", f.batcherFlushes);
    Put(L, "batcherVertices", f.batcherVertices);
    Put(L, "batcherIndices", f.batcherIndices);
    Put(L, "overlayMs", f.overlayMs);
    Put(L, "overlayFlushes", f.overlayFlushes);
    Put(L, "overlayVertices", f.overlayVertices);
    Put(L, "uiMs", f.uiMs);
    Put(L, "uiBitmapsDrawn", f.uiBitmapsDrawn);
    Put(L, "uiBitmapsCulled", f.uiBitmapsCulled);
    Put(L, "uiBitmapsTransparent", f.uiBitmapsTransparent);
    Put(L, "uiBordersDrawn", f.uiBordersDrawn);
    Put(L, "uiBordersCulled", f.uiBordersCulled);
    Put(L, "uiTextsDrawn", f.uiTextsDrawn);
    Put(L, "uiTextsCulled", f.uiTextsCulled);
    Put(L, "atlasLookups", f.atlasLookups);
    Put(L, "atlasFastHits", f.atlasFastHits);
    Put(L, "atlasAdds", f.atlasAdds);
    Put(L, "atlasResets", f.atlasResets);
    Put(L, "shadowMs", f.shadowMs);
    Put(L, "shadowSceneMs", f.shadowSceneMs);
    Put(L, "shadowRendered", f.shadowRendered);
    Put(L, "shadowReused", f.shadowReused);
    Put(L, "waterSkipped", f.waterSkipped);
    Put(L, "renderMs", f.renderMs);
    Put(L, "shadowHits", static_cast<double>(rs::gShadowHits));
    Put(L, "shadowMisses", static_cast<double>(rs::gShadowMisses));
    Put(L, "waterSkippedFrames", static_cast<double>(rs::gWaterSkippedFrames));
    return 1;
  }

  [[nodiscard]] moho::CScrLuaInitFormSet& UserLuaInitSet()
  {
    if (moho::CScrLuaInitFormSet* const set = moho::SCR_FindLuaInitFormSet("User"); set != nullptr) {
      return *set;
    }
    static moho::CScrLuaInitFormSet fallbackSet("User");
    return fallbackSet;
  }

  struct EdRenderStatsLuaBootstrap
  {
    EdRenderStatsLuaBootstrap()
    {
      (void)new moho::CScrLuaBinder(UserLuaInitSet(), "ed_render_stats", &cfunc_EdRenderStats, nullptr, "<global>",
                                    "ed_render_stats() -> table: draw calls, batcher flushes, UI controls drawn / culled, atlas lookups, shadow cache, water skip, CPU ms of the last frame");
    }
  };

  const EdRenderStatsLuaBootstrap gEdRenderStatsLuaBootstrap{};
} // namespace
