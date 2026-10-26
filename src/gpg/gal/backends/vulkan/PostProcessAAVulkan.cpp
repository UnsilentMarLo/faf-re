// FAF addition, not in the shipped binary: DeviceVulkan::ApplyPostProcessAA (see Device.hpp). The back buffer is copied into a texture and
// one GPU program (the vertex-pulling path of GpuDrivenVulkan.cpp, the same HLSL as Direct3D 11 and 12) filters it back into the back buffer.

#include "DeviceVulkan.h"
#include "ResourcesVulkan.h"

#include "gpg/core/utils/Logging.h"
#include "gpg/gal/GpuDriven.h"
#include "gpg/gal/RenderTargetContext.hpp"
#include "gpg/gal/TextureContext.h"
#include "gpg/gal/backends/common/PostProcessAA.h"

namespace gpg::gal
{
  bool DeviceVulkan::ApplyPostProcessAA(const std::uint32_t head, const std::uint32_t mode)
  {
    if (mode != kPostProcessAAFxaa || mAaFailed || (mGpuFeatures & kGpuFeatureVertexPulling) == 0U || head >= mHeadOutputContexts.size() ||
        mHeadOutputContexts[head].surface == nullptr) {
      return false;
    }
    const std::lock_guard<std::recursive_mutex> lock(mMutex);
    const boost::shared_ptr<RenderTarget> back = mHeadOutputContexts[head].surface;
    const std::uint32_t width = back->GetContext()->width_;
    const std::uint32_t height = back->GetContext()->height_;
    // The copy is a gal texture; the back buffer is B8G8R8A8 (gal texture format 2) on this backend.
    if (static_cast<RenderTargetVulkan*>(back.get())->Current().image.format != VK_FORMAT_B8G8R8A8_UNORM || width == 0U || height == 0U) {
      mAaFailed = true;
      gpg::Warnf("Vulkan: post-process anti-aliasing needs a B8G8R8A8 back buffer; disabled");
      return false;
    }

    if (mAaProgram == nullptr) {
      // The texture sits at t4 in the GPU-driven binding model (GpuDriven.h), the sampler at s0.
      const std::string source = backend::PostProcessAASource("t4");
      GpuProgramDesc desc;
      desc.name = "PostProcessAA";
      desc.source = source.c_str();
      desc.sourceBytes = source.size();
      desc.vertexEntry = "AaVS";
      desc.pixelEntry = "FxaaPS";
      desc.blend = GpuBlend::Opaque;
      desc.sampler = GpuSampler::LinearClamp;
      mAaProgram = CreateGpuProgram(desc);
      if (mAaProgram == nullptr) {
        mAaFailed = true;
        gpg::Warnf("Vulkan: post-process anti-aliasing disabled, its program did not compile");
        return false;
      }
      gpg::Logf("Vulkan: post-process anti-aliasing mode %u ready", mode);
    }

    if (mAaCopy == nullptr || mAaCopyWidth != width || mAaCopyHeight != height) {
      mAaCopy.reset();
      TextureContext context{};
      context.source_ = 2U; // no file data: an empty texture
      context.format_ = 2U; // B8G8R8A8
      context.mipmapLevels_ = 1U;
      context.width_ = width;
      context.height_ = height;
      mAaCopy = CreateTexture(&context);
      mAaCopyWidth = width;
      mAaCopyHeight = height;
      gpg::Logf("Vulkan: post-process anti-aliasing copy %ux%u", width, height);
    }

    GetRenderTargetData(back, mAaCopy);

    // Draw into the head's back buffer, then put the engine's targets and viewport back.
    const OutputContext saved = outputContext_;
    D3DVIEWPORT9 savedViewport{};
    GetViewport(&savedViewport);
    ClearTarget(&mHeadOutputContexts[head]);
    D3DVIEWPORT9 full{};
    full.Width = width;
    full.Height = height;
    full.MaxZ = 1.0f;
    SetViewport(&full);

    GpuDrawCall call;
    call.program = mAaProgram.get();
    call.bindings.textures[0] = mAaCopy.get();
    call.vertexCountPerInstance = 3U;
    call.instanceCount = 1U;
    DrawGpuProgram(call);

    ClearTarget(&saved);
    SetViewport(&savedViewport);
    return true;
  }
}
