// Host test of the SMAA 1x and sharpening passes (src/gpg/gal/backends/common/PostProcessSmaa.h): the real shaders run on a software Direct3D 11
// device (WARP) over synthetic images whose answer is known. Flat areas and straight edges stay exactly as they are; the staircase of a slanted
// edge moves much closer to the exact pixel coverage of the line it was drawn from; sharpening leaves flat areas and full-contrast edges alone,
// adds contrast to soft edges, and High adds more than Low.

#define NOMINMAX
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "gpg/gal/backends/common/PostProcessSmaa.h"

using Microsoft::WRL::ComPtr;

namespace
{
  int gFailures = 0;

  void Check(const bool condition, const char* const what)
  {
    if (!condition) {
      ++gFailures;
      std::printf("FAIL: %s\n", what);
    }
  }

  constexpr int kWidth = 96;
  constexpr int kHeight = 64;

  using Image = std::vector<float>; // one luma value per pixel, replicated to grey

  struct Runner
  {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> edges, weights, blend, sharpenLow, sharpenHigh;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> opaque;

    bool Init()
    {
      D3D_FEATURE_LEVEL level{};
      if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0U, nullptr, 0U, D3D11_SDK_VERSION, &device, &level, &context))) {
        std::printf("no WARP device\n");
        return false;
      }
      const std::string source = gpg::gal::backend::PostProcessSmaaSource();
      const auto compile = [&](const char* const entry, const char* const profile, ComPtr<ID3DBlob>& code) {
        ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(source.c_str(), source.size(), "smaa", nullptr, nullptr, entry, profile, 0U, 0U, &code, &errors);
        if (FAILED(result)) {
          std::printf("compile %s failed: %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        }
        return SUCCEEDED(result);
      };
      ComPtr<ID3DBlob> blobs[6];
      if (!compile("SmaaVS", "vs_5_0", blobs[0]) || !compile("SmaaEdgePS", "ps_5_0", blobs[1]) || !compile("SmaaWeightsPS", "ps_5_0", blobs[2]) ||
          !compile("SmaaBlendPS", "ps_5_0", blobs[3]) || !compile("SharpenLowPS", "ps_5_0", blobs[4]) || !compile("SharpenHighPS", "ps_5_0", blobs[5])) {
        return false;
      }
      device->CreateVertexShader(blobs[0]->GetBufferPointer(), blobs[0]->GetBufferSize(), nullptr, &vertexShader);
      const auto pixel = [&](const int i, ComPtr<ID3D11PixelShader>& out) {
        device->CreatePixelShader(blobs[i]->GetBufferPointer(), blobs[i]->GetBufferSize(), nullptr, &out);
      };
      pixel(1, edges);
      pixel(2, weights);
      pixel(3, blend);
      pixel(4, sharpenLow);
      pixel(5, sharpenHigh);
      D3D11_SAMPLER_DESC samplerDesc{};
      samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
      device->CreateSamplerState(&samplerDesc, &sampler);
      D3D11_RASTERIZER_DESC rasterDesc{};
      rasterDesc.FillMode = D3D11_FILL_SOLID;
      rasterDesc.CullMode = D3D11_CULL_NONE;
      device->CreateRasterizerState(&rasterDesc, &raster);
      D3D11_BLEND_DESC blendDesc{};
      blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      device->CreateBlendState(&blendDesc, &opaque);
      return true;
    }

    struct Target
    {
      ComPtr<ID3D11Texture2D> texture;
      ComPtr<ID3D11RenderTargetView> rtv;
      ComPtr<ID3D11ShaderResourceView> srv;
    };

    Target MakeTarget(const std::uint8_t* const initial)
    {
      Target target;
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = kWidth;
      desc.Height = kHeight;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      const D3D11_SUBRESOURCE_DATA data{initial, kWidth * 4, 0};
      device->CreateTexture2D(&desc, initial != nullptr ? &data : nullptr, &target.texture);
      device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.rtv);
      device->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv);
      return target;
    }

    void Draw(ID3D11PixelShader* const shader, Target& output, ID3D11ShaderResourceView* const first, ID3D11ShaderResourceView* const second)
    {
      ID3D11ShaderResourceView* views[6] = {nullptr, nullptr, nullptr, nullptr, first, second};
      context->PSSetShaderResources(0, 6, views);
      ID3D11RenderTargetView* rtv = output.rtv.Get();
      context->OMSetRenderTargets(1, &rtv, nullptr);
      context->OMSetBlendState(opaque.Get(), nullptr, 0xFFFFFFFFU);
      context->RSSetState(raster.Get());
      const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
      context->RSSetViewports(1, &viewport);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context->VSSetShader(vertexShader.Get(), nullptr, 0);
      context->PSSetShader(shader, nullptr, 0);
      ID3D11SamplerState* s = sampler.Get();
      context->PSSetSamplers(0, 1, &s);
      context->Draw(3, 0);
      ID3D11ShaderResourceView* none[6] = {};
      context->PSSetShaderResources(0, 6, none);
      ID3D11RenderTargetView* noTarget = nullptr;
      context->OMSetRenderTargets(1, &noTarget, nullptr);
    }

    std::vector<std::uint8_t> Read(Target& target)
    {
      D3D11_TEXTURE2D_DESC desc{};
      target.texture->GetDesc(&desc);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ComPtr<ID3D11Texture2D> staging;
      device->CreateTexture2D(&desc, nullptr, &staging);
      context->CopyResource(staging.Get(), target.texture.Get());
      D3D11_MAPPED_SUBRESOURCE mapped{};
      std::vector<std::uint8_t> bytes(kWidth * kHeight * 4);
      if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        for (int y = 0; y < kHeight; ++y) {
          std::copy_n(static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch, kWidth * 4, bytes.begin() + y * kWidth * 4);
        }
        context->Unmap(staging.Get(), 0);
      }
      return bytes;
    }

    static std::vector<std::uint8_t> ToBytes(const Image& image)
    {
      std::vector<std::uint8_t> bytes(kWidth * kHeight * 4);
      for (int i = 0; i < kWidth * kHeight; ++i) {
        const auto v = static_cast<std::uint8_t>(std::lround(std::clamp(image[i], 0.0f, 1.0f) * 255.0f));
        bytes[i * 4 + 0] = bytes[i * 4 + 1] = bytes[i * 4 + 2] = v;
        bytes[i * 4 + 3] = 255;
      }
      return bytes;
    }

    static Image ToImage(const std::vector<std::uint8_t>& bytes)
    {
      Image image(kWidth * kHeight);
      for (int i = 0; i < kWidth * kHeight; ++i) {
        image[i] = (bytes[i * 4] + bytes[i * 4 + 1] + bytes[i * 4 + 2]) / (3.0f * 255.0f);
      }
      return image;
    }

    Image Smaa(const Image& input)
    {
      const std::vector<std::uint8_t> bytes = ToBytes(input);
      Target source = MakeTarget(bytes.data());
      Target edgeTarget = MakeTarget(nullptr);
      Target weightTarget = MakeTarget(nullptr);
      Target out = MakeTarget(bytes.data());
      Draw(edges.Get(), edgeTarget, source.srv.Get(), nullptr);
      Draw(weights.Get(), weightTarget, edgeTarget.srv.Get(), nullptr);
      Draw(blend.Get(), out, source.srv.Get(), weightTarget.srv.Get());
      return ToImage(Read(out));
    }

    Image Sharpen(const Image& input, const bool high)
    {
      const std::vector<std::uint8_t> bytes = ToBytes(input);
      Target source = MakeTarget(bytes.data());
      Target out = MakeTarget(bytes.data());
      Draw(high ? sharpenHigh.Get() : sharpenLow.Get(), out, source.srv.Get(), nullptr);
      return ToImage(Read(out));
    }
  };

  constexpr float kDark = 0.1f;
  constexpr float kLight = 0.9f;

  // A half-plane: light where y > slope * x + offset (pixel coordinates, y down). Returns the aliased image and the exact coverage image.
  void SlantedEdge(const float slope, const float offset, Image& aliased, Image& exact)
  {
    aliased.assign(kWidth * kHeight, kDark);
    exact.assign(kWidth * kHeight, kDark);
    constexpr int kSub = 16;
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        aliased[y * kWidth + x] = (y + 0.5f > slope * (x + 0.5f) + offset) ? kLight : kDark;
        int inside = 0;
        for (int sy = 0; sy < kSub; ++sy) {
          for (int sx = 0; sx < kSub; ++sx) {
            const float px = x + (sx + 0.5f) / kSub;
            const float py = y + (sy + 0.5f) / kSub;
            inside += py > slope * px + offset ? 1 : 0;
          }
        }
        exact[y * kWidth + x] = kDark + (kLight - kDark) * static_cast<float>(inside) / (kSub * kSub);
      }
    }
  }

  // Mean absolute error against the exact image over the pixels within `band` rows of the line (away from the image border).
  double BandError(const Image& image, const Image& exact, const float slope, const float offset, const float band)
  {
    double sum = 0.0;
    int count = 0;
    for (int y = 6; y < kHeight - 6; ++y) {
      for (int x = 8; x < kWidth - 8; ++x) {
        const float lineY = slope * (x + 0.5f) + offset;
        if (std::fabs((y + 0.5f) - lineY) <= band) {
          sum += std::fabs(image[y * kWidth + x] - exact[y * kWidth + x]);
          ++count;
        }
      }
    }
    return count > 0 ? sum / count : 0.0;
  }

  double MaxDifference(const Image& a, const Image& b)
  {
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
      worst = std::max(worst, static_cast<double>(std::fabs(a[i] - b[i])));
    }
    return worst;
  }

  Image Filled(const std::function<float(int, int)>& value)
  {
    Image image(kWidth * kHeight);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        image[y * kWidth + x] = value(x, y);
      }
    }
    return image;
  }
}

int main()
{
  Runner runner;
  if (!runner.Init()) {
    return 1;
  }

  // ---- SMAA: what must not change ------------------------------------------------------------------------------------------------------
  {
    const Image flat = Filled([](int, int) { return 0.4f; });
    Check(MaxDifference(runner.Smaa(flat), flat) < 0.003, "SMAA leaves a flat image alone");

    const Image vertical = Filled([](int x, int) { return x < 40 ? kDark : kLight; });
    Check(MaxDifference(runner.Smaa(vertical), vertical) < 0.003, "SMAA leaves a straight vertical edge alone");
    const Image horizontal = Filled([](int, int y) { return y < 30 ? kDark : kLight; });
    Check(MaxDifference(runner.Smaa(horizontal), horizontal) < 0.003, "SMAA leaves a straight horizontal edge alone");

    // A box much longer than the search (16 pixels each way): the middle of its sides is out of reach of the corners and stays sharp. Near a
    // corner SMAA rounds the edge, as the reference implementation does.
    const Image box = Filled([](int x, int y) { return (x >= 12 && x < 84 && y >= 4 && y < 60) ? kLight : kDark; });
    const Image boxOut = runner.Smaa(box);
    double sideDifference = 0.0;
    for (int y = 4 + 17; y < 60 - 17; ++y) {
      for (int x : {11, 12, 83, 84}) {
        sideDifference = std::max(sideDifference, static_cast<double>(std::fabs(boxOut[y * kWidth + x] - box[y * kWidth + x])));
      }
    }
    for (int x = 12 + 17; x < 84 - 17; ++x) {
      for (int y : {3, 4, 59, 60}) {
        sideDifference = std::max(sideDifference, static_cast<double>(std::fabs(boxOut[y * kWidth + x] - box[y * kWidth + x])));
      }
    }
    Check(sideDifference < 0.003, "SMAA keeps the long sides of a box sharp");
    // The corner pixel is blended with its neighbours, not left alone and not replaced.
    const float corner = boxOut[4 * kWidth + 12];
    Check(corner > kLight - 0.45f && corner < kLight - 0.01f, "SMAA rounds the corner of a box a little");
    // Along the top edge the blend dies away smoothly from the corner: no step where the search gives up 16 pixels out.
    double worstStep = 0.0;
    for (int x = 12; x < 48; ++x) {
      worstStep = std::max(worstStep, static_cast<double>(std::fabs(boxOut[4 * kWidth + x + 1] - boxOut[4 * kWidth + x])));
    }
    std::printf("box top edge: worst step between neighbouring pixels near a corner %.3f\n", worstStep);
    Check(worstStep < 0.06, "SMAA's blend along a long edge fades out without a step");
  }

  // ---- SMAA: slanted edges get closer to the exact coverage -----------------------------------------------------------------------------
  const float slopes[] = {0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, -0.25f, -1.0f};
  for (const float slope : slopes) {
    // The staircase of a slanted line depends on where the line sits in the pixel grid; average over nine positions.
    double before = 0.0;
    double after = 0.0;
    float lo = 1.0f;
    float hi = 0.0f;
    const float band = 2.5f * std::max(1.0f, std::fabs(slope));
    for (int k = 1; k <= 9; ++k) {
      const float offset = (slope >= 0.0f ? 14.0f : 40.0f) + 0.1f * static_cast<float>(k);
      Image aliased, exact;
      SlantedEdge(slope, offset, aliased, exact);
      const Image filtered = runner.Smaa(aliased);
      before += BandError(aliased, exact, slope, offset, band) / 9.0;
      after += BandError(filtered, exact, slope, offset, band) / 9.0;
      lo = std::min(lo, *std::min_element(filtered.begin(), filtered.end()));
      hi = std::max(hi, *std::max_element(filtered.begin(), filtered.end()));
    }
    std::printf("slope %+.4f: mean error in the band %.4f aliased, %.4f SMAA (%.0f %% of it)\n", slope, before, after, 100.0 * after / before);
    char what[96];
    std::snprintf(what, sizeof(what), "SMAA moves a slope %.4f edge closer to the exact coverage", slope);
    Check(after < 0.8 * before, what);
    Check(lo > kDark - 0.01f && hi < kLight + 0.01f, "SMAA output stays between the two colours");
  }

  // ---- sharpening ------------------------------------------------------------------------------------------------------------------------
  {
    const Image flat = Filled([](int, int) { return 0.4f; });
    Check(MaxDifference(runner.Sharpen(flat, true), flat) < 0.003, "sharpening leaves a flat image alone");

    const Image hard = Filled([](int x, int) { return x < 40 ? 0.0f : 1.0f; });
    Check(MaxDifference(runner.Sharpen(hard, true), hard) < 0.003, "sharpening does not touch a full-contrast edge");

    const Image soft = Filled([](int x, int) { return std::clamp(0.3f + (x - 40) * 0.08f, 0.3f, 0.7f); });
    const Image low = runner.Sharpen(soft, false);
    const Image high = runner.Sharpen(soft, true);
    // At the foot of the ramp the value dips below the original, at its head it rises above.
    const float foot = soft[30 * kWidth + 40];
    const float head = soft[30 * kWidth + 45];
    Check(low[30 * kWidth + 40] < foot - 0.003f && high[30 * kWidth + 40] < low[30 * kWidth + 40], "sharpening deepens the foot of a soft edge (High more than Low)");
    Check(low[30 * kWidth + 45] > head + 0.003f && high[30 * kWidth + 45] > low[30 * kWidth + 45], "sharpening raises the head of a soft edge (High more than Low)");
    Check(*std::min_element(high.begin(), high.end()) >= 0.0f && *std::max_element(high.begin(), high.end()) <= 1.0f, "sharpened values stay in range");
  }

  if (gFailures == 0) {
    std::printf("post-process tests passed\n");
    return 0;
  }
  std::printf("%d post-process test(s) FAILED\n", gFailures);
  return 1;
}
