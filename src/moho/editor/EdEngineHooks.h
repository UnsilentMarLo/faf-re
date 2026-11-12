#pragma once

namespace moho::editor
{
  /// Fills the engine's editor hook table (moho/app/EditorHooks.h) and turns the editor's render switches on. Called at
  /// the start of the process (PrepareEditorStart), before any of the engine's call sites can run. Without this the
  /// engine behaves as the game does.
  void InstallEngineHooks();
} // namespace moho::editor
