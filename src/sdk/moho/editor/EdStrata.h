// EDITOR: access to the terrain strata paint masks for other editor natives (the field engine, EdField.cpp). Implemented in EdStrata.cpp on top of its
// MaskShadow, so the engine textures, the shadow and the readers of ed_stratum_read stay in step.
#pragma once

#include <cstdint>
#include <vector>

namespace moho::editor
{
  /** The mask bytes of engine stratum 0..7 (row major, w x h as ed_stratum_info says). false when there is no mask. */
  bool StratumMaskRead(int stratum, std::vector<std::uint8_t>& out, int& w, int& h);

  /** Replaces the whole mask of engine stratum 0..7 and uploads it; w x h must be the mask size. */
  bool StratumMaskWrite(int stratum, const std::uint8_t* data, int w, int h);
} // namespace moho::editor
