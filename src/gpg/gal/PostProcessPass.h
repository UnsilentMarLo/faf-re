#pragma once

#include <cstdint>

#include "boost/shared_ptr.h"
#include "gpg/gal/GpuDriven.h"
#include "gpg/gal/RenderTarget.hpp"

namespace gpg::gal
{
  class Device;

  /**
   * FAF addition, not in the shipped binary: the SMAA 1x anti-aliasing and the contrast adaptive sharpening of the finished 3D world on any
   * backend that has the GPU-driven programs (`kGpuFeatureVertexPulling`: Direct3D 11, 12 and Vulkan; docs/RENDERER_BACKENDS.md, "Post-process
   * anti-aliasing" and "Sharpening").
   *
   * Like `AmbientOcclusionPass` it is written against the public `Device` calls only (`CreateRenderTarget`, `StretchRect`, `ClearTarget`,
   * `SetViewport`, `CreateGpuProgram`, `DrawGpuProgram`), so one implementation serves all three. The head's back buffer cannot be read while it
   * is drawn into, so each pass starts with a copy of it (`StretchRect` into a target of the same size and format), then draws from the copy
   * back into the back buffer. SMAA draws the edges and the blend weights into two more targets first. The engine's bound targets and
   * viewport are put back.
   */
  class PostProcessPass
  {
  public:
    [[nodiscard]] static PostProcessPass& Instance();

    /**
     * SMAA 1x on head `head`'s back buffer. False (having drawn nothing) when the backend has no GPU programs, a target or a program could not
     * be made; the first refusal is logged once.
     */
    bool ApplySmaa(Device& device, std::uint32_t head);

    /** Sharpening (`PostProcessSharpenLevel` 1 or 2) of head `head`'s back buffer; false as above. */
    bool ApplySharpen(Device& device, std::uint32_t head, std::uint32_t level);

    /** Frees the targets and programs (a device reset or exit). */
    void Release();

    [[nodiscard]] std::uint32_t SmaaFrames() const noexcept { return mSmaaFrames; }
    [[nodiscard]] std::uint32_t SharpenFrames() const noexcept { return mSharpenFrames; }

  private:
    /** The programs (made once) and the targets for a `width` x `height` back buffer of gal format `format`. */
    bool EnsureResources(Device& device, std::uint32_t width, std::uint32_t height, std::uint32_t format, bool smaa);
    bool Prepare(Device& device, std::uint32_t head, bool smaa, boost::shared_ptr<RenderTarget>& back, const char*& refusal);
    void DrawPass(
      Device& device, GpuProgram* program, const boost::shared_ptr<RenderTarget>& output, std::uint32_t width, std::uint32_t height,
      RenderTarget* first, RenderTarget* second
    );

    Device* mDevice = nullptr;
    boost::shared_ptr<GpuProgram> mEdges;
    boost::shared_ptr<GpuProgram> mWeights;
    boost::shared_ptr<GpuProgram> mBlend;
    boost::shared_ptr<GpuProgram> mSharpenLow;
    boost::shared_ptr<GpuProgram> mSharpenHigh;
    boost::shared_ptr<RenderTarget> mColour;
    boost::shared_ptr<RenderTarget> mEdgeTarget;
    boost::shared_ptr<RenderTarget> mWeightTarget;
    std::uint32_t mWidth = 0U;
    std::uint32_t mHeight = 0U;
    std::uint32_t mFormat = 0U;
    std::uint32_t mSmaaFrames = 0U;
    std::uint32_t mSharpenFrames = 0U;
    bool mFailed = false;
    bool mLoggedRefusal = false;
  };
}
