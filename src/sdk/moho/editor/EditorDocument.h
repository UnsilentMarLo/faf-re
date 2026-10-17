// Engine side of the editor's MapDocument (M1): binds the document to the live session and owns the commit path.
//
//   document (truth, UI thread)  --sink-->  view height field + normal map + decal-manager flag   (UI thread, at once)
//                                    \-->  queue --ApplyPendingHeightEdits--> sim height field + path caches
//                                                  (sim thread, at the start of every beat)
//
// Before this, the UI thread wrote the sim's height field while the sim thread read it. Now only the sim thread
// writes it, between beats.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "moho/editor/MapDocument.h"

namespace moho
{
  class Sim;
}

namespace moho::editor
{
  /** The one document of the editor process. Valid after a successful AttachDocument. */
  MapDocument& Document();

  /**
   * Binds the document to the active session's terrain (reads its height field). Does nothing when the document is
   * already bound to this session's terrain, unless `force` (a new session always starts a new document).
   * Returns false when there is no session or terrain.
   */
  bool AttachDocument(bool force);

  /** Sim thread, between beats: applies the queued height edits to the sim's field and dirties its path caches. */
  std::size_t ApplyPendingHeightEdits(Sim* sim);

  /** Checksum of the sim's own height field over a rectangle, comparable with MapDocument::Checksum. */
  std::uint64_t SimFieldChecksum(Sim* sim, const CellRect& rect);

  [[nodiscard]] std::string HexChecksum(std::uint64_t value);
} // namespace moho::editor
