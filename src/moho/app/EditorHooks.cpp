#include "moho/app/EditorHooks.h"

namespace moho
{
  // All null: the game has no editor. moho/editor/EdEngineHooks.cpp fills it when the editor starts.
  EditorHooks gEditorHooks{};

  // The editor's switches of the engine's passes: off here, so a build without the editor draws and caches exactly as
  // the engine does without them. The editor turns them on (EdEngineHooks.cpp) and registers the console variables.
  bool ren_ShadowCache = false;
  bool ren_ShadowCacheTrees = false;
  int ren_ShadowCacheMaxAge = 300;
  bool ren_WaterSkipDisabled = false;
  bool ui_CullOffscreen = false;
  bool ui_SkipTransparent = false;
} // namespace moho
