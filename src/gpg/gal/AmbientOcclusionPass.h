#pragma once

#include <cstdint>

#include "boost/shared_ptr.h"
#include "gpg/gal/DepthStencilTarget.hpp"
#include "gpg/gal/GpuDriven.h"
#include "gpg/gal/RenderTarget.hpp"
#include "gpg/gal/backends/common/AmbientOcclusion.h"

namespace gpg::gal
{
  class Device;

  /**
   * FAF addition, not in the shipped binary: the screen-space ambient occlusion pass on any backend that has the GPU-driven programs
   * (`kGpuFeatureVertexPulling`: Direct3D 11, 12 and Vulkan; docs/RENDERER_BACKENDS.md, "Ambient occlusion").
   *
   * It is written against the public `Device` calls only (render targets, `SetViewport`, `ClearTarget`, `CreateGpuProgram`,
   * `DrawGpuProgram`), so one implementation serves all three; the backends contribute the head's sampled depth buffer and the binding of
   * a depth or colour target as `t4` / `t5` (`GpuBindings::depthTextures` / `renderTextures`).
   *
   * Three draws: the horizon search into a target of its own (half the size at Low and Medium), a horizontal blur into a second target, and a
   * vertical blur at back buffer resolution that is blended onto the head's back buffer as black with alpha = 1 - occlusion. The engine's
   * bound targets and viewport are put back.
   */
  class AmbientOcclusionPass
  {
  public:
    [[nodiscard]] static AmbientOcclusionPass& Instance();

    /**
     * Draws the occlusion of the depth buffer into head `head`'s back buffer. False (having drawn nothing) when the backend has no GPU
     * programs, the head's depth buffer cannot be sampled, or a program did not compile; the first refusal is logged once.
     */
    bool Apply(Device& device, std::uint32_t head, const AmbientOcclusionParams& params);

    /**
     * Copies head `head`'s depth buffer as it is now; the next `Apply` reads the copy instead of the live buffer. The engine calls it before
     * the mesh stage that draws the shield bubbles: their ShieldFill pass writes the bubble's far side into the depth buffer, so the live
     * buffer under a bubble holds the bubble and not the ground. False (nothing copied, `Apply` uses the live buffer) when the backend
     * cannot copy depth or the head's depth buffer is not sampled.
     */
    bool CaptureSceneDepth(Device& device, std::uint32_t head);

    /** Frees the targets and programs (a device reset or exit). */
    void Release();

    /** Number of passes drawn since start (diagnostics and tests). */
    [[nodiscard]] std::uint32_t FramesDrawn() const noexcept { return mFrames; }

  private:
    bool EnsureResources(Device& device, std::uint32_t width, std::uint32_t height, bool halfRes);

    Device* mDevice = nullptr;
    boost::shared_ptr<GpuProgram> mCompute;
    boost::shared_ptr<GpuProgram> mBlur;
    boost::shared_ptr<GpuProgram> mComposite;
    boost::shared_ptr<RenderTarget> mTargetA;
    boost::shared_ptr<RenderTarget> mTargetB;
    boost::shared_ptr<DepthStencilTarget> mSceneDepth; // the depth copy of CaptureSceneDepth
    std::uint32_t mSceneDepthWidth = 0U;
    std::uint32_t mSceneDepthHeight = 0U;
    std::uint32_t mSceneDepthFormat = 0U;
    bool mSceneDepthFresh = false; // taken this frame, not yet used by Apply
    bool mCopyUnsupported = false; // the backend refused a depth copy: Apply keeps using the live buffer
    std::uint32_t mTargetWidth = 0U;
    std::uint32_t mTargetHeight = 0U;
    std::uint32_t mFrames = 0U;
    bool mFailed = false;
    bool mLoggedRefusal = false;
  };
}
