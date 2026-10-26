#pragma once

// FAF addition, not in the shipped binary: the Direct3D 11 counterparts of
// EffectD3D10, EffectTechniqueD3D10 and EffectVariableD3D10, on Microsoft's
// Effects 11 (FX11) runtime.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <d3dx11effect.h>
#include <wrl/client.h>

#include "boost/shared_ptr.h"
#include "gpg/gal/Effect.hpp"
#include "gpg/gal/EffectContext.hpp"
#include "gpg/gal/EffectTechnique.hpp"
#include "gpg/gal/EffectVariable.hpp"
#include "legacy/containers/String.h"
#include "legacy/containers/Vector.h"

namespace gpg::gal
{
  /**
   * The sampler variables of one effect, for forced anisotropic filtering (ForcedAnisotropy.h). Effects 11 lets a sampler variable take a
   * state object of the caller's (`SetSampler`) and give it back (`UndoSetSampler`), so the effect's own sampler states are swapped for
   * anisotropic copies and back without touching the source. A sampler is forced only while the texture variable it reads holds a world
   * texture (a 2D picture with a mip chain, not a render target): `EffectVariableD3D11::SetTexture / SetRenderTarget` report that per
   * texture variable, and which sampler reads which texture variable comes from the parsed effect source (`samplerTextures`).
   */
  struct EffectSamplersD3D11
  {
    struct Entry
    {
      std::string name;
      std::string texture; // the texture parameter this sampler reads ("" = unknown: never forced)
      ID3DX11EffectSamplerVariable* variable = nullptr; // owned by the effect
      std::vector<D3D11_SAMPLER_DESC> authored;         // the effect's own state per array element
      bool world = false;                               // the texture variable holds a world texture now
    };
    std::vector<Entry> samplers;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    unsigned epoch = 0; // ForcedAnisotropyEpoch() the samplers were last set for

    /** `samplerTextures`: (sampler name, texture parameter name) pairs from the effect source. */
    void Collect(ID3DX11Effect* effect, const std::vector<std::pair<std::string, std::string>>& samplerTextures);
    /** The samplers that read the texture variable `texture`. */
    [[nodiscard]] std::vector<std::size_t> ReadersOf(const std::string& texture) const;
    /** The texture variable of these samplers now holds (or no longer holds) a world texture. */
    void NoteBinding(const std::vector<std::size_t>& readers, bool world);
    /** Re-applies every sampler when the forced level changed since the last call. Cheap otherwise. */
    void Sync();

  private:
    void Apply(Entry& entry);
  };

  class EffectD3D11 final : public Effect
  {
  public:
    EffectD3D11(
      const EffectContext& context, ID3DX11Effect* effect, const std::vector<std::pair<std::string, std::string>>& samplerTextures = {}
    );
    ~EffectD3D11() override;

    EffectContext* GetContext() override;
    void GetTechniques(msvc8::vector<boost::shared_ptr<EffectTechnique>>& outTechniques) override;
    boost::shared_ptr<EffectVariable> GetVariable(const char* name) override;
    boost::shared_ptr<EffectTechnique> GetTechnique(const char* name) override;
    /** Nothing to do: Direct3D 11 resources survive a mode change. */
    void OnReset() override;
    void OnLost() override;

  private:
    EffectContext mContext;
    Microsoft::WRL::ComPtr<ID3DX11Effect> mEffect;
    std::shared_ptr<EffectSamplersD3D11> mSamplers;
  };

  class EffectTechniqueD3D11 final : public EffectTechnique
  {
  public:
    EffectTechniqueD3D11(
      const char* name, ID3DX11Effect* effect, ID3DX11EffectTechnique* technique, std::shared_ptr<EffectSamplersD3D11> samplers = nullptr
    );
    ~EffectTechniqueD3D11() override;

    msvc8::string* GetName() override;
    int BeginTechnique() override;
    void EndTechnique() override;
    void BeginPass(int pass) override;
    void EndPass() override;
    bool GetAnnotationBool(bool* outValue, const msvc8::string& annotationName) override;
    bool GetAnnotationInt(int* outValue, const msvc8::string& annotationName) override;
    bool GetAnnotationFloat(float* outValue, const msvc8::string& annotationName) override;
    bool GetAnnotationString(msvc8::string* outValue, const msvc8::string& annotationName) override;

  private:
    msvc8::string mName;
    Microsoft::WRL::ComPtr<ID3DX11Effect> mEffect; // keeps the technique alive
    ID3DX11EffectTechnique* mTechnique = nullptr;  // owned by the effect
    ID3DX11EffectVectorVariable* mViewportSize = nullptr; // faf_ViewportSize, when the effect has it
    std::shared_ptr<EffectSamplersD3D11> mSamplers;
    bool mBegun = false;
  };

  class EffectVariableD3D11 final : public EffectVariable
  {
  public:
    EffectVariableD3D11(
      const char* name, ID3DX11Effect* effect, ID3DX11EffectVariable* variable, std::shared_ptr<EffectSamplersD3D11> samplers = nullptr
    );
    ~EffectVariableD3D11() override;

    msvc8::string* GetName() override;
    /** No cube targets on this backend yet, as on D3D10. */
    void SetCubeRenderTarget(boost::shared_ptr<CubeRenderTarget> cubeTarget) override;
    void SetRenderTarget(boost::shared_ptr<RenderTarget> renderTarget) override;
    void SetTexture(boost::shared_ptr<Texture> texture) override;
    void SetMatrix4x4(const Matrix* matrix) override;
    void SetFloatArray(std::uint32_t count, const float* values) override;
    void SetVector(const float* vector4) override;
    void SetValue(const void* data, std::uint32_t byteCount) override;
    void SetFloat(float value) override;
    void SetInt(int value) override;
    void SetBool(bool value) override;
    void SetMatrixArray(std::uint32_t count, const Matrix* matrices) override;
    void SetVectorArray(std::uint32_t count, const float* vectors4) override;
    bool GetAnnotationBool(bool* outValue, const msvc8::string& annotationName) override;
    bool GetAnnotationInt(int* outValue, const msvc8::string& annotationName) override;
    bool GetAnnotationFloat(float* outValue, const msvc8::string& annotationName) override;
    bool GetAnnotationString(msvc8::string* outValue, const msvc8::string& annotationName) override;

  private:
    /**
     * Writes `count` packed floats the way D3D9's ID3DXEffect::SetFloatArray
     * lays them into registers: by the variable's declared type, so a
     * `float arr[N]` or `float3 arr[N]` lands one element per 16-byte
     * constant-buffer row instead of packed (what SetRawValue would do).
     */
    void SetPackedFloats(const float* values, std::uint32_t count, int line);

    msvc8::string mName;
    Microsoft::WRL::ComPtr<ID3DX11Effect> mEffect;
    ID3DX11EffectVariable* mVariable = nullptr; // owned by the effect
    D3DX11_EFFECT_TYPE_DESC mType{};
    std::shared_ptr<EffectSamplersD3D11> mSamplers;
    std::vector<std::size_t> mSamplerReaders; // the samplers of the effect that read this texture variable
  };
} // namespace gpg::gal
