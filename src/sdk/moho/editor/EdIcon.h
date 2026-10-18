#pragma once

// EDITOR: unit icon generator (docs/projects/MOD_UNIT_EDITOR_DESIGN.md, stage S5). The render pass itself lives in WRenViewport::D3DWindowOnDeviceRender
// (moho/app/WxRuntimeTypes.cpp); this header is the hand-over between the Lua natives (EdIcon.cpp) and that pass.

#include <cstdint>
#include <string>

namespace moho::edicon
{
  struct Request
  {
    float x = 0.0f;           // world position the camera looks at
    float y = 0.0f;
    float z = 0.0f;
    float heading = 0.0f;     // radians, the editor camera's convention (SnapTo)
    float pitch = 0.0f;
    float zoom = 10.0f;       // visible vertical extent in world units at the target (what CameraImpl calls zoom)
    float fovDeg = 30.0f;
    std::uint32_t keepMask = 0;   // debugging: bit i leaves render flag i of the pass enabled (see WxRuntimeTypes.cpp)
    int size = 256;           // the render is size x size pixels in the top left of the window, then reduced to the icon
  };

  /// True while the icon pass renders: WRenViewport::Render then skips the water and clears to gClearArgb.
  extern bool gActive;
  extern std::uint32_t gClearArgb;

  /// Takes the pending request (one per frame), false when there is none.
  [[nodiscard]] bool TakeRequest(Request& out);
  /// Reports the end of a pass: the two captures (on black and on white) are at TempPath(0) and TempPath(1) when ok.
  void Complete(bool ok, const char* message);
  [[nodiscard]] std::string TempPath(int pass);

  /// Reads a file of the game data (plain file or an entry inside a .scd / .nx2 / .zip, as the VFS resolves it).
  [[nodiscard]] bool ReadAllBytes(const std::string& path, std::string& out);
} // namespace moho::edicon
