// EDITOR: tests that tell a Maui control cannot put a single pixel on the screen, so its quads need not be built and queued (engine independent, tested on its own:
// moho/editor/tests/EdRenderOptTest.cpp).
//
// Maui does not clip and the editor parks controls it does not show far outside the window (the holder of a tab behind another tab at -20000, the controls of the other kind of a panel
// at x = -4000, see dock.lua HideHolder / panels_view.lua). Every frame the engine walked those controls like all others: asked their layout, looked their texture up in the atlas
// (and put it into it, so parked pictures filled the atlas), built the vertices and uploaded them, for a result the GPU clipped away. These tests keep that work from being done.
// Output is identical: a quad that lies completely outside the viewport covers no pixel, a vertex colour with alpha 0 gives alpha 0 in every UI technique (alpha = tex.a * colour.a *
// AlphaMultiplier, test AlphaRef 0 / blend SrcAlpha), and a glyph run that starts to the right of or below the viewport has no glyph inside it.
#pragma once

#include <cstdint>

namespace moho::editor::uicull
{
  /** The control's rectangle (left, top, right, bottom) is completely outside [0, viewW] x [0, viewH]. A viewport of unknown size (<= 0) is never outside. */
  [[nodiscard]] constexpr bool RectOutside(const float left, const float top, const float right, const float bottom, const float viewW, const float viewH) noexcept
  {
    if (!(viewW > 0.0f) || !(viewH > 0.0f)) {
      return false;
    }
    // NaN coordinates (a layout that is not evaluated yet) fail every comparison here: such a rectangle is not culled
    return right < 0.0f || bottom < 0.0f || left > viewW || top > viewH;
  }

  /** The quad of a bitmap / border is its rectangle, exactly: it is outside when the rectangle is (a rectangle with no area draws nothing either). */
  [[nodiscard]] constexpr bool QuadOutside(const float left, const float top, const float right, const float bottom, const float viewW, const float viewH) noexcept
  {
    return RectOutside(left, top, right, bottom, viewW, viewH);
  }

  /**
   * A text run, as CMauiText::DoRender lays it out: glyphs go from `originX` to the right (the x axis is +1) and stand around `baselineY`; a glyph reaches at most `pad` pixels above
   * or below the baseline and `pad` to the left of the origin. The run is outside the viewport when it starts to the right of it, when its baseline is more than `pad` above or below
   * it, or - only when `runWidth` is known (>= 0; the width of the string, the caller asks the font only when the run starts left of the viewport) - when it ends left of it.
   */
  [[nodiscard]] constexpr bool TextOutside(const float originX, const float baselineY, const float runWidth, const float viewW, const float viewH, const float pad) noexcept
  {
    if (!(viewW > 0.0f) || !(viewH > 0.0f)) {
      return false;
    }
    if (originX > viewW + pad || baselineY < -pad || baselineY > viewH + pad) {
      return true;
    }
    return runWidth >= 0.0f && (originX + runWidth) < -pad;
  }

  /** The alpha byte of a vertex colour is 0: nothing of the quad is visible. */
  [[nodiscard]] constexpr bool ColorTransparent(const std::uint32_t argb) noexcept
  {
    return (argb >> 24u) == 0u;
  }
} // namespace moho::editor::uicull
