#pragma once

namespace moho
{
  class Sim;

  /**
   * Engine addition: called on the sim's thread at the start of each beat (`Sim::AdvanceBeat`), before anything of the
   * beat runs. The map editor sets it to put height edits made in its view into a running solo play (WPE.40); null in the
   * game, where it is never set (no effect on the sim or its checksums).
   */
  extern void (*gSimBeforeBeat)(Sim& sim);

  /**
   * Engine addition: the map editor's gate on the sim's forced Lua collection (every 70 ticks). `gSimGcDue(heapKb, seconds)` says whether this one runs
   * (the editor skips it while the heap has not grown); `gSimGcDone(heapKb)` is told after it ran. Both null in the game: the collection runs every time.
   */
  extern bool (*gSimGcDue)(int heapKb, float seconds);
  extern void (*gSimGcDone)(int heapKb);
} // namespace moho
