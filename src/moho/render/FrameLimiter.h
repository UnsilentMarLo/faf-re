#pragma once

// FAF addition, not in the shipped binary: the frame rate limit (docs/RENDERER_BACKENDS.md, "Frame rate limit"). The renderer calls
// FrameLimiterWait once per frame, right after Present; it sleeps until the frame is due. Works the same on every render API, render only.
namespace moho
{
  // ren_FpsLimit: the level of the Options > Video entry (index into the table in FrameLimiter.cpp; 0 = off, the default).
  extern int ren_FpsLimit;
  // ren_MaxFps: an exact limit in frames per second; when above 0 it wins over ren_FpsLimit. /maxfps N on the command line sets it.
  extern float ren_MaxFps;

  // The limit in effect, frames per second; 0 = none.
  [[nodiscard]] float FrameLimitFps();

  // Waits until the next frame is due (the frame times are kept on a fixed grid, so a slow frame does not shift the later ones; a frame that
  // is more than one interval late restarts the grid instead of racing to catch up). Returns at once when there is no limit.
  void FrameLimiterWait();
}
