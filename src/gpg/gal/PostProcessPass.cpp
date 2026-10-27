#include "gpg/gal/PostProcessPass.h"

#include <string>

#include "gpg/core/utils/Logging.h"
#include "gpg/gal/Device.hpp"
#include "gpg/gal/OutputContext.hpp"
#include "gpg/gal/RenderTargetContext.hpp"
#include "gpg/gal/backends/common/PostProcessSmaa.h"

namespace gpg::gal
{
  namespace
  {
    // gal render target format 2: four 8-bit unorm channels (B8G8R8A8 or R8G8B8A8; the channels are addressed as r, g, b, a either way).
    constexpr std::uint32_t kEightBitFormat = 2U;
  } // namespace

  PostProcessPass& PostProcessPass::Instance()
  {
    static PostProcessPass instance;
    return instance;
  }

  void PostProcessPass::Release()
  {
    mEdges.reset();
    mWeights.reset();
    mBlend.reset();
    mSharpenLow.reset();
    mSharpenHigh.reset();
    mColour.reset();
    mEdgeTarget.reset();
    mWeightTarget.reset();
    mWidth = 0U;
    mHeight = 0U;
    mFormat = 0U;
    mDevice = nullptr;
    mFailed = false;
    mLoggedRefusal = false;
  }

  bool PostProcessPass::EnsureResources(
    Device& device, const std::uint32_t width, const std::uint32_t height, const std::uint32_t format, const bool smaa
  )
  {
    if (mDevice != &device) {
      Release();
      mDevice = &device;
    }
    if (mBlend == nullptr) {
      const std::string source = backend::PostProcessSmaaSource();
      const auto make = [&](const char* const name, const char* const pixelEntry, const bool intoBackBuffer) {
        GpuProgramDesc desc;
        desc.name = name;
        desc.source = source.c_str();
        desc.sourceBytes = source.size();
        desc.vertexEntry = "SmaaVS";
        desc.pixelEntry = pixelEntry;
        desc.blend = GpuBlend::Opaque;
        // The back buffer keeps its alpha; the working targets use all four channels.
        desc.colorWriteMask = intoBackBuffer ? 0x07U : 0x0FU;
        desc.sampler = GpuSampler::LinearClamp;
        return device.CreateGpuProgram(desc);
      };
      mEdges = make("SmaaEdges", "SmaaEdgePS", false);
      mWeights = make("SmaaWeights", "SmaaWeightsPS", false);
      mBlend = make("SmaaBlend", "SmaaBlendPS", true);
      mSharpenLow = make("SharpenLow", "SharpenLowPS", true);
      mSharpenHigh = make("SharpenHigh", "SharpenHighPS", true);
      if (mEdges == nullptr || mWeights == nullptr || mBlend == nullptr || mSharpenLow == nullptr || mSharpenHigh == nullptr) {
        gpg::Warnf("Post-process SMAA and sharpening disabled: their GPU programs did not compile");
        mFailed = true;
        return false;
      }
    }

    const bool sizeChanged = mWidth != width || mHeight != height || mFormat != format;
    if (mColour == nullptr || sizeChanged) {
      mColour.reset();
      mEdgeTarget.reset();
      mWeightTarget.reset();
      // Creating a render target resets the device's viewport on some backends; the caller restores its own.
      const RenderTargetContext colourContext(width, height, format);
      mColour = device.CreateRenderTarget(&colourContext);
      if (mColour == nullptr) {
        gpg::Warnf("Post-process SMAA and sharpening disabled: no %ux%u render target", width, height);
        mFailed = true;
        return false;
      }
      mWidth = width;
      mHeight = height;
      mFormat = format;
      gpg::Logf("Post-process passes: %ux%u working target (gal format %u)", width, height, format);
    }
    if (smaa && (mEdgeTarget == nullptr || mWeightTarget == nullptr)) {
      const RenderTargetContext context(width, height, kEightBitFormat);
      mEdgeTarget = device.CreateRenderTarget(&context);
      mWeightTarget = device.CreateRenderTarget(&context);
      if (mEdgeTarget == nullptr || mWeightTarget == nullptr) {
        gpg::Warnf("Post-process SMAA disabled: no %ux%u edge and weight targets", width, height);
        mEdgeTarget.reset();
        mWeightTarget.reset();
        mFailed = true;
        return false;
      }
      gpg::Logf("SMAA: %ux%u edge and blend weight targets", width, height);
    }
    return true;
  }

  bool PostProcessPass::Prepare(
    Device& device, const std::uint32_t head, const bool smaa, boost::shared_ptr<RenderTarget>& back, const char*& refusal
  )
  {
    if ((device.GetGpuDrivenFeatures() & kGpuFeatureVertexPulling) == 0U) {
      refusal = "this render API has no GPU programs";
      return false;
    }
    const OutputContext* const headContext = device.GetHeadOutputContext(head);
    if (headContext == nullptr || headContext->surface == nullptr) {
      refusal = "the head has no back buffer";
      return false;
    }
    back = headContext->surface;
    const RenderTargetContext* const backContext = back->GetContext();
    if (backContext->width_ == 0U || backContext->height_ == 0U) {
      refusal = "the back buffer is empty";
      return false;
    }
    D3DVIEWPORT9 savedViewport{};
    device.GetViewport(&savedViewport);
    const bool ready = EnsureResources(device, backContext->width_, backContext->height_, backContext->format_, smaa);
    device.SetViewport(&savedViewport);
    if (!ready) {
      refusal = "a program or a target could not be made";
      return false;
    }
    return true;
  }

  void PostProcessPass::DrawPass(
    Device& device,
    GpuProgram* const program,
    const boost::shared_ptr<RenderTarget>& output,
    const std::uint32_t width,
    const std::uint32_t height,
    RenderTarget* const first,
    RenderTarget* const second
  )
  {
    const boost::shared_ptr<DepthStencilTarget> noDepth;
    const OutputContext target(output, noDepth);
    device.ClearTarget(&target);
    D3DVIEWPORT9 viewport{};
    viewport.Width = width;
    viewport.Height = height;
    viewport.MaxZ = 1.0f;
    device.SetViewport(&viewport);
    GpuDrawCall call;
    call.program = program;
    call.bindings.renderTextures[0] = first;
    call.bindings.renderTextures[1] = second;
    call.vertexCountPerInstance = 3U;
    call.instanceCount = 1U;
    device.DrawGpuProgram(call);
  }

  bool PostProcessPass::ApplySmaa(Device& device, const std::uint32_t head)
  {
    if (mFailed) {
      return false;
    }
    boost::shared_ptr<RenderTarget> back;
    const char* refusal = "";
    if (!Prepare(device, head, true, back, refusal)) {
      if (!mLoggedRefusal) {
        mLoggedRefusal = true;
        gpg::Warnf("SMAA is not available: %s; nothing is filtered", refusal);
      }
      return false;
    }
    const std::uint32_t width = mWidth;
    const std::uint32_t height = mHeight;

    OutputContext saved;
    device.GetContext(&saved);
    D3DVIEWPORT9 savedViewport{};
    device.GetViewport(&savedViewport);

    device.StretchRect(back, mColour, nullptr, nullptr);
    DrawPass(device, mEdges.get(), mEdgeTarget, width, height, mColour.get(), nullptr);
    DrawPass(device, mWeights.get(), mWeightTarget, width, height, mEdgeTarget.get(), nullptr);
    DrawPass(device, mBlend.get(), back, width, height, mColour.get(), mWeightTarget.get());

    device.ClearTarget(&saved);
    device.SetViewport(&savedViewport);
    ++mSmaaFrames;
    return true;
  }

  bool PostProcessPass::ApplySharpen(Device& device, const std::uint32_t head, const std::uint32_t level)
  {
    if (mFailed || level == kPostProcessSharpenOff) {
      return false;
    }
    boost::shared_ptr<RenderTarget> back;
    const char* refusal = "";
    if (!Prepare(device, head, false, back, refusal)) {
      if (!mLoggedRefusal) {
        mLoggedRefusal = true;
        gpg::Warnf("Sharpening is not available: %s; nothing is drawn", refusal);
      }
      return false;
    }
    const std::uint32_t width = mWidth;
    const std::uint32_t height = mHeight;

    OutputContext saved;
    device.GetContext(&saved);
    D3DVIEWPORT9 savedViewport{};
    device.GetViewport(&savedViewport);

    device.StretchRect(back, mColour, nullptr, nullptr);
    DrawPass(device, level >= kPostProcessSharpenHigh ? mSharpenHigh.get() : mSharpenLow.get(), back, width, height, mColour.get(), nullptr);

    device.ClearTarget(&saved);
    device.SetViewport(&savedViewport);
    ++mSharpenFrames;
    return true;
  }
}
