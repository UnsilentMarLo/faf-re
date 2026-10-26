// EDITOR: device changes asked for from UI code (anti-aliasing, vertical sync) are queued and applied by the main loop between two frames (CD3DDevice.cpp).
#pragma once

namespace gpg::gal
{
  class DeviceContext;
}

namespace moho
{
  /** The context the next queued change starts from: the pending one, else a copy of the device's active context. nullptr without a device. */
  gpg::gal::DeviceContext* ED_PendingDeviceContext();

  /** Top of every frame (CScApp::Main), outside any window message dispatch: resets the device with the queued context, if there is one. */
  void ED_ApplyPendingDeviceContext();
} // namespace moho
