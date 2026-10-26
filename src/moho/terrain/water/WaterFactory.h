#pragma once

#include "moho/terrain/water/WaterSurface.h"

namespace moho
{
  /**
   * Address: 0x00811120 (FUN_00811120, func_CreateWaterFidelity)
   *
   * What it does:
   * Allocates one low/high-fidelity water surface by `graphics_Fidelity`,
   * logs the selected path, and initializes water render sheets.
   */
  [[nodiscard]] WaterSurface* CreateWaterFidelity(IWldTerrainRes* terrainResource);

  /**
   * EDITOR: rebuilds the water surface quad of every fidelity that exists, from the terrain's current water switch and elevation.
   */
  void EdRefreshWaterSurfaces(IWldTerrainRes* terrainResource);
} // namespace moho
