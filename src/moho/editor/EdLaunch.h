// EDITOR: what happens when the map given on the command line cannot be launched (moho/misc/StartupHelpers.cpp StartCommandLineSession).
//
// The engine's own answer is a modal message box ("Ack!") on the main thread: it stops the whole loop until somebody clicks it, so an editor started with a missing or broken
// map (a stale recent-file entry, a launcher with a wrong path, an unattended test) sat behind a dialog, then looked frozen (the freeze watchdog wrote a crash report after
// 12 s). In the editor the error is remembered instead, logged, and the start screen opens at once; the Lua side reads it with ed_launch_error() and shows it in its own
// UI. Should nobody read it within kBoxDelayMs, a message box is shown from its own thread (it does not stop the loop), so the error is never lost.
#pragma once

namespace moho::editor::launch
{
  constexpr int kBoxDelayMs = 2500;

  /// The launch of `mapName` failed with `reason` (the text of the Lua error). Thread: the main thread.
  void ReportFailure(const char* mapName, const char* reason);

  /// True in the editor (command line `/editor`): the caller reports with ReportFailure instead of a modal box.
  bool Active();
}
