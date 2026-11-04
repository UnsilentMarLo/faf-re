
#pragma once

#include <d3d11.h>

#include "gpg/gal/backends/d3d11/DeviceD3D11.h"

namespace moho::editor
{
  /**
   * The Direct3D 11 backend behind the game's device. The editor runs on Direct3D 11 only (CScApp.cpp, RendererAttemptOrder); nullptr only before the device exists or when another
   * backend was forced into a test build. The editor's natives (mesh drawing, pop-outs, FXAA timing, the GPU compute adapter match) draw and query through its raw device and
   * immediate context. The engine's own state cache is `Pipeline()`: a native that changes pipeline state outside it restores what it changed (see EdMeshDraw.cpp).
   */
  inline gpg::gal::DeviceD3D11* D3D11Gal(gpg::gal::Device* const gal)
  {
    return dynamic_cast<gpg::gal::DeviceD3D11*>(gal);
  }

  inline ID3D11Device* D3D11RawDevice(gpg::gal::Device* const gal)
  {
    gpg::gal::DeviceD3D11* const d3d11 = D3D11Gal(gal);
    return d3d11 != nullptr ? d3d11->NativeDevice() : nullptr;
  }

  inline ID3D11DeviceContext* D3D11RawContext(gpg::gal::Device* const gal)
  {
    gpg::gal::DeviceD3D11* const d3d11 = D3D11Gal(gal);
    return d3d11 != nullptr ? d3d11->Immediate() : nullptr;
  }
}
