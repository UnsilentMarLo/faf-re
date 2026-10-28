#include "moho/render/lights/LightBinding.h"

#include <algorithm>
#include <cstring>
#include <exception>

#include "boost/shared_ptr.h"
#include "gpg/core/utils/Logging.h"
#include "gpg/gal/D3D9Utils.h"
#include "gpg/gal/Device.hpp"
#include "gpg/gal/EffectVariable.hpp"
#include "gpg/gal/Texture.hpp"
#include "gpg/gal/TextureContext.h"
#include "moho/render/d3d/ShaderVar.h"

namespace moho::lights
{
  namespace
  {
    // The parameters of one effect. Never destroyed: the effect unlinks its shader-vars when it goes, and that may happen after static
    // destruction would have run (the same reason the bone palette's vars are leaked).
    struct EffectVars
    {
      ShaderVar data{};
      ShaderVar grid{};
      ShaderVar params{};
      ShaderVar gridSize{};
      ShaderVar dataSize{};
      ShaderVar camera{};

      explicit EffectVars(const char* effectFile)
      {
        RegisterShaderVar("dynLightData", &data, effectFile);
        RegisterShaderVar("dynLightGrid", &grid, effectFile);
        RegisterShaderVar("dynLightParams", &params, effectFile);
        RegisterShaderVar("dynLightGridSize", &gridSize, effectFile);
        RegisterShaderVar("dynLightDataSize", &dataSize, effectFile);
        RegisterShaderVar("dynLightCamera", &camera, effectFile);
      }
    };

    struct State
    {
      EffectVars mesh{"mesh"};
      EffectVars terrain{"terrain"};

      boost::shared_ptr<gpg::gal::Texture> dataTexture;
      boost::shared_ptr<gpg::gal::Texture> gridTexture;
      std::uint32_t dataRows = 0;
      std::uint32_t gridWidth = 0;
      std::uint32_t gridHeight = 0;

      bool wasEmpty = true;      // the last upload carried no light
      bool everUploaded = false;
      bool texturesFresh = false; // textures were (re)created: bind them again
      BindingStats stats;
    };

    State& GetState()
    {
      static State* const state = new State();
      return *state;
    }

    boost::shared_ptr<gpg::gal::Texture> CreateFloat4Texture(const std::uint32_t width, const std::uint32_t height)
    {
      gpg::gal::TextureContext context{};
      context.source_ = 2u; // no file data: an empty texture
      context.usage_ = 2u;  // dynamic
      context.format_ = gpg::gal::kTextureFormatFloat4;
      context.mipmapLevels_ = 1u;
      context.width_ = width;
      context.height_ = height;
      return gpg::gal::Device::GetInstance()->CreateTexture(&context);
    }

    // Copies `rows` rows of `width` RGBA32F texels into a locked dynamic texture (its pitch may be wider than the image).
    bool Upload(gpg::gal::Texture& texture, const std::vector<float>& image, const std::uint32_t width, const std::uint32_t rows)
    {
      const RECT wholeLevel{};
      const gpg::gal::TextureLockRect lock = texture.Lock(0, wholeLevel, static_cast<int>(gpg::gal::MohoD3DLockFlags::Discard));
      if (lock.bits == nullptr) {
        return false;
      }
      const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u * sizeof(float);
      auto* const destination = static_cast<std::uint8_t*>(lock.bits);
      const auto* const source = reinterpret_cast<const std::uint8_t*>(image.data());
      for (std::uint32_t row = 0; row < rows; ++row) {
        std::memcpy(destination + static_cast<std::size_t>(row) * static_cast<std::size_t>(lock.pitch), source + row * rowBytes, rowBytes);
      }
      (void)texture.Unlock(lock);
      return true;
    }

    bool SetVec(ShaderVar& var, const float (&v)[4])
    {
      if (!var.Exists()) {
        return false;
      }
      var.mEffectVariable->SetVector(v);
      return true;
    }

    bool SetTex(ShaderVar& var, const boost::shared_ptr<gpg::gal::Texture>& texture)
    {
      if (!var.Exists()) {
        return false;
      }
      var.mEffectVariable->SetTexture(texture);
      return true;
    }

    std::uint32_t Bind(EffectVars& vars, const LightGridImage& image, const boost::shared_ptr<gpg::gal::Texture>& dataTexture,
                       const boost::shared_ptr<gpg::gal::Texture>& gridTexture, const bool bindTextures)
    {
      const GridLayout& l = image.layout;
      // the texture always has the rows of the largest light count: that is the height the shader divides by, not the rows in use
      const float rows = static_cast<float>(image.DataRows(static_cast<std::uint32_t>(kMaxGridLights)));
      const float params[4] = {l.originX, l.originZ, 1.0f / l.cellSize, static_cast<float>(image.lightCount)};
      const float gridSize[4] = {
        static_cast<float>(l.cellsX), static_cast<float>(l.cellsZ), 1.0f / static_cast<float>(kGridTexelWidth), 1.0f / static_cast<float>(kGridRows)
      };
      const float dataSize[4] = {1.0f / static_cast<float>(kDataWidth), 1.0f / rows, static_cast<float>(kDataWidth), rows};

      std::uint32_t existing = 0;
      if (bindTextures) {
        existing += SetTex(vars.data, dataTexture) ? 1u : 0u;
        existing += SetTex(vars.grid, gridTexture) ? 1u : 0u;
      }
      existing += SetVec(vars.params, params) ? 1u : 0u;
      existing += SetVec(vars.gridSize, gridSize) ? 1u : 0u;
      existing += SetVec(vars.dataSize, dataSize) ? 1u : 0u;
      return existing;
    }
  } // namespace

  void UpdateBinding(const LightGridImage& image)
  {
    State& s = GetState();
    const bool empty = image.lightCount == 0;
    if (empty && s.wasEmpty && s.everUploaded) {
      return; // nothing changed since the last (empty) upload
    }

    try {
      const std::uint32_t rows = image.DataRows(static_cast<std::uint32_t>(kMaxGridLights));
      // The grid texture has a fixed size (a table texel per cell, then the lists); an upload copies the rows in use.
      const std::uint32_t gridWidth = kGridTexelWidth;
      const std::uint32_t gridHeight = kGridRows;

      if (!s.dataTexture || s.dataRows != rows) {
        s.dataTexture = CreateFloat4Texture(kDataWidth, rows);
        s.dataRows = rows;
        s.texturesFresh = true;
        ++s.stats.creates;
        gpg::Logf("DynLights: light data texture %ux%u", kDataWidth, rows);
      }
      if (!s.gridTexture || s.gridWidth != gridWidth || s.gridHeight != gridHeight) {
        s.gridTexture = CreateFloat4Texture(gridWidth, gridHeight);
        s.gridWidth = gridWidth;
        s.gridHeight = gridHeight;
        s.texturesFresh = true;
        ++s.stats.creates;
        gpg::Logf("DynLights: light grid texture %ux%u (cell %.1f)", gridWidth, gridHeight, image.layout.cellSize);
      }
      if (!s.dataTexture || !s.gridTexture) {
        ++s.stats.failures;
        return;
      }

      // The data texture is a fixed size (room for kMaxGridLights); the image carries only the rows it needs, the rest stays as it was
      // (never read: the shader indexes below lightCount).
      const bool dataOk = Upload(*s.dataTexture, image.data, kDataWidth, image.DataRows());
      const bool gridOk = Upload(*s.gridTexture, image.grid, gridWidth, std::min(image.GridRows(), gridHeight));
      if (!dataOk || !gridOk) {
        ++s.stats.failures;
        return;
      }
      ++s.stats.uploads;
      s.wasEmpty = empty;
      s.everUploaded = true;

      // Textures are bound when they are new or when an effect (re)appeared; a ShaderVar whose effect is not loaded yet reports
      // Exists() == false, so keep trying to bind until one effect has them.
      // Every frame: an effect that was rebuilt (a device reset, a reload) has lost what was set on its variables.
      const bool bindTextures = true;
      const std::uint32_t existing = Bind(s.mesh, image, s.dataTexture, s.gridTexture, bindTextures) +
                                     Bind(s.terrain, image, s.dataTexture, s.gridTexture, bindTextures);
      s.stats.boundVariables = existing;
      if (existing > 0) {
        s.texturesFresh = false;
      }
    } catch (const std::exception& e) {
      ++s.stats.failures;
      gpg::Logf("DynLights: light texture upload failed: %s", e.what());
    }
  }

  void SetCameraPosition(const float x, const float y, const float z)
  {
    const float position[4] = {x, y, z, 0.0f};
    State& s = GetState();
    (void)SetVec(s.mesh.camera, position); // only the mesh effect declares it
  }

  void ReleaseBinding()
  {
    State& s = GetState();
    // The effects hold references of their own: unbind first.
    const boost::shared_ptr<gpg::gal::Texture> none;
    for (EffectVars* vars : {&s.mesh, &s.terrain}) {
      if (vars->data.mEffectVariable && vars->data.Exists()) {
        vars->data.mEffectVariable->SetTexture(none);
      }
      if (vars->grid.mEffectVariable && vars->grid.Exists()) {
        vars->grid.mEffectVariable->SetTexture(none);
      }
    }
    s.dataTexture.reset();
    s.gridTexture.reset();
    s.dataRows = s.gridWidth = s.gridHeight = 0;
    s.wasEmpty = true;
    s.everUploaded = false;
    s.texturesFresh = true;
    s.stats.boundVariables = 0;
  }

  const BindingStats& GetBindingStats()
  {
    return GetState().stats;
  }
} // namespace moho::lights
