// TEST CODE: a GPU compute Executor and Backend that run the kernels on the CPU through gpu/hlsl_cpp_shim.h (docs/projects/editor/GPU_COMPUTE.md, "Testing a kernel").
// Include AFTER hlsl_cpp_shim.h and after every standard / engine header (the shim defines global min / max / clamp templates).
//
//   EmuExecutor ex;                      a device without a GPU: buffers are byte vectors, Dispatch runs the registered emulation of the kernel (emu::Kernels(), filled by
//                                        EMU_KERNEL_BEGIN / #include "<kernel>.hlsl" / EMU_KERNEL_END(name) in the test), Sync costs nothing.
//   ex.failAtDispatch = n                the n-th Dispatch (0 based) fails with an error; ex.lostAtDispatch = n: it fails and the executor reports DeviceLost().
//   EmuBackend backend;                  the Backend interface over an EmuExecutor: SetBackendForTest(&backend) lets the field engine's interceptor run kernels on it.
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "moho/editor/EdGpu.h"

namespace moho::editor::gpu
{
  class EmuResource final : public Resource
  {
  public:
    std::vector<unsigned char> bytes;
  };

  class EmuExecutor final : public Executor
  {
  public:
    int failAtDispatch = -1;
    int lostAtDispatch = -1;
    bool texturesSupported = true;
    std::uint64_t maxBytes = 1ull << 30;
    int dispatches = 0;
    int syncs = 0;
    int uploads = 0;
    int downloads = 0;
    bool lost = false;

    [[nodiscard]] const char* Name() const override { return "CPU emulation"; }
    [[nodiscard]] std::uint64_t MaxBytes() const override { return maxBytes; }
    [[nodiscard]] bool SupportsTextures() const override { return texturesSupported; }
    [[nodiscard]] bool DeviceLost() const override { return lost; }

    std::unique_ptr<Resource> CreateBuffer(const u32 count, const u32 stride, std::string& err) override
    {
      if (lost) { err = "device lost"; return nullptr; }
      if (count == 0 || stride == 0 || stride % 4 != 0 || static_cast<std::uint64_t>(count) * stride > maxBytes) { err = "buffer size out of range"; return nullptr; }
      auto r = std::make_unique<EmuResource>();
      r->kind = Resource::Kind::Buffer;
      r->count = count;
      r->stride = stride;
      r->bytes.assign(static_cast<std::size_t>(count) * stride, 0xFF);   // undefined on a GPU too: a NaN pattern makes a read of an unwritten sample visible
      return r;
    }

    std::unique_ptr<Resource> CreateTexture(const u32 w, const u32 h, std::string& err) override
    {
      if (lost) { err = "device lost"; return nullptr; }
      if (!texturesSupported) { err = "textures are not supported by this device"; return nullptr; }
      if (w == 0 || h == 0 || w > 16384 || h > 16384) { err = "texture size out of range"; return nullptr; }
      auto r = std::make_unique<EmuResource>();
      r->kind = Resource::Kind::Texture2D;
      r->w = w;
      r->h = h;
      r->bytes.assign(static_cast<std::size_t>(w) * h * 4u, 0xFF);
      return r;
    }

    bool Upload(Resource& r, const void* data, const std::size_t n, std::string& err) override
    {
      if (lost) { err = "device lost"; return false; }
      auto& e = static_cast<EmuResource&>(r);
      if (data == nullptr || n == 0 || n > e.bytes.size()) { err = "upload size out of range"; return false; }
      if (r.kind == Resource::Kind::Texture2D && n != e.bytes.size()) { err = "a texture is uploaded whole"; return false; }
      std::memcpy(e.bytes.data(), data, n);
      ++uploads;
      return true;
    }

    bool Download(Resource& r, void* data, const std::size_t n, std::string& err) override
    {
      if (lost) { err = "device lost"; return false; }
      auto& e = static_cast<EmuResource&>(r);
      if (data == nullptr || n == 0 || n > e.bytes.size()) { err = "download size out of range"; return false; }
      std::memcpy(data, e.bytes.data(), n);
      ++downloads;
      return true;
    }

    bool Dispatch(const KernelInfo& k, const Groups g, const void* cb, const u32 cbBytes, const Bindings& b, std::string& err) override
    {
      if (lost) { err = "device lost"; return false; }
      const int index = dispatches++;
      if (index == failAtDispatch) { err = "injected failure"; return false; }
      if (index == lostAtDispatch) { lost = true; err = "injected device loss"; return false; }
      if (g.x == 0 || g.y == 0 || g.z == 0 || g.x > kMaxGroups || g.y > kMaxGroups || g.z > kMaxGroups) { err = "dispatch group count out of range"; return false; }
      if ((k.hasCb != 0) != (cb != nullptr) || (k.hasCb != 0 && (cbBytes == 0 || cbBytes % 16 != 0 || cbBytes > 4096))) { err = std::string("kernel ") + k.name + ": constant buffer missing, unexpected or not a multiple of 16 bytes"; return false; }
      const auto it = emu::Kernels().find(k.name);
      if (it == emu::Kernels().end()) { err = std::string("kernel ") + k.name + " has no emulation registered"; return false; }
      emu::KernelEmu& ke = *it->second;
      if (static_cast<u32>(ke.tx) != k.tx || static_cast<u32>(ke.ty) != k.ty || static_cast<u32>(ke.tz) != k.tz) { err = std::string("kernel ") + k.name + ": thread group differs from the generated table"; return false; }
      emu::ResView srv[kMaxSrv] = {};
      emu::ResView uav[kMaxUav] = {};
      for (u32 s = 0; s < kMaxSrv; ++s) {
        if (b.srv[s] == nullptr) {
          if ((k.srvMask >> s) & 1u) { err = std::string("kernel ") + k.name + ": SRV slot " + std::to_string(s) + " is not bound"; return false; }
          continue;
        }
        const bool wantTex = ((k.srvTexMask >> s) & 1u) != 0;
        if ((b.srv[s]->kind == Resource::Kind::Texture2D) != wantTex) { err = std::string("kernel ") + k.name + ": SRV slot " + std::to_string(s) + " has the wrong resource kind"; return false; }
        srv[s] = View(*b.srv[s]);
      }
      for (u32 s = 0; s < kMaxUav; ++s) {
        if (b.uav[s] == nullptr) {
          if ((k.uavMask >> s) & 1u) { err = std::string("kernel ") + k.name + ": UAV slot " + std::to_string(s) + " is not bound"; return false; }
          continue;
        }
        const bool wantTex = ((k.uavTexMask >> s) & 1u) != 0;
        if ((b.uav[s]->kind == Resource::Kind::Texture2D) != wantTex) { err = std::string("kernel ") + k.name + ": UAV slot " + std::to_string(s) + " has the wrong resource kind"; return false; }
        for (u32 t = 0; t < kMaxSrv; ++t) {
          if (b.srv[t] == b.uav[s]) { err = std::string("kernel ") + k.name + ": a resource is bound for reading and writing in one dispatch"; return false; }
        }
        uav[s] = View(*b.uav[s]);
      }
      return emu::Run(ke, g.x, g.y, g.z, cb, cbBytes, srv, uav, err);
    }

    bool Sync(double* gpuMs, std::string& err) override
    {
      if (lost) { err = "device lost"; return false; }
      ++syncs;
      if (gpuMs != nullptr) *gpuMs = 0.0;
      return true;
    }

  private:
    static emu::ResView View(const Resource& r)
    {
      emu::ResView v;
      v.data = const_cast<unsigned char*>(static_cast<const EmuResource&>(r).bytes.data());
      v.count = r.count;
      v.stride = r.stride;
      v.w = r.w;
      v.h = r.h;
      v.texture = r.kind == Resource::Kind::Texture2D;
      return v;
    }
  };

  class EmuBackend final : public Backend
  {
  public:
    EmuExecutor exec;
    bool failed = false;
    std::string why;
    int markFailedCalls = 0;
    bool available = true;

    BackendInfo Info(bool) override
    {
      BackendInfo i;
      i.compiledIn = true;
      i.tried = true;
      i.available = available && !failed;
      i.failed = failed;
      i.adapter = "CPU emulation";
      i.lastError = why;
      return i;
    }
    bool MayWork() override { return available && !failed; }
    Executor* Acquire(const bool wait, bool& busy, std::string& err) override
    {
      busy = false;
      if (wait) {
        use_.lock();
      } else if (!use_.try_lock()) {
        busy = true;
        err = "the GPU is busy with another operation";
        return nullptr;
      }
      if (failed || !available) {
        err = why.empty() ? std::string("unavailable") : why;
        use_.unlock();
        return nullptr;
      }
      return &exec;
    }
    void Release() override { use_.unlock(); }
    void MarkFailed(const std::string& text) override
    {
      failed = true;
      why = text;
      ++markFailedCalls;
    }
    void Reset() override
    {
      failed = false;
      why.clear();
      exec.lost = false;
    }

  private:
    std::mutex use_;
  };
} // namespace moho::editor::gpu
