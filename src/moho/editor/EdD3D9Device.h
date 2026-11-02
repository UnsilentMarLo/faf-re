#pragma once

#include <d3d9.h>

#include "gpg/gal/backends/d3d9/DeviceD3D9.hpp"

namespace moho::editor
{
  /**
   * The raw Direct3D 9 device behind the game's device, or nullptr when the game runs on another renderer (Direct3D 11 / 12, Vulkan: a graphics_api option or /d3d12 on the command line).
   * The editor's natives (overlay, mesh drawing, pop-outs, FXAA, the GPU compute adapter match) only work on Direct3D 9; they used to cast whatever device the game had and read a
   * foreign object's memory (an access violation in the GPU compute adapter match on Direct3D 12).
   */
  inline IDirect3DDevice9* D3D9RawDevice(gpg::gal::Device* const gal)
  {
    gpg::gal::DeviceD3D9* const d3d9 = dynamic_cast<gpg::gal::DeviceD3D9*>(gal);
    return d3d9 != nullptr ? d3d9->mDevice : nullptr;
  }
}
