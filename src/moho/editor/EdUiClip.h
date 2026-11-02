// EDITOR: clipping for Maui. The engine's UI never clipped: a control past the edge of its parent was drawn over whatever lay there, so every scrolling panel moved the controls
// that were not completely inside off screen (and a control half inside vanished until it was scrolled completely in).
//
// A control (usually a Group) can be told to clip its descendants: Control:SetClipChildren(true) in Lua. Everything below it is then drawn only inside the control's rectangle
// (Left / Top / Right / Bottom), nested clipping controls intersect, and the mouse does not hit a descendant outside of it. The clipping is done on the CPU, on the quads the controls
// hand to the primitive batcher (CD3DPrimBatcher::SetClipRect): the batch is not broken, nothing depends on the graphics backend, text and bitmaps are cut with their texture
// coordinates adjusted.
//
// This header is the engine independent part (rectangle, polygon clipping): tested on its own in moho/editor/tests/EdUiClipTest.cpp.
#pragma once

#include <algorithm>
#include <cstdint>

namespace moho
{
  class CMauiControl;
}

namespace moho::editor::uiclip
{
  struct Rect
  {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
  };

  [[nodiscard]] constexpr bool Empty(const Rect& r) noexcept
  {
    return !(r.right > r.left && r.bottom > r.top);
  }

  [[nodiscard]] constexpr Rect Intersect(const Rect& a, const Rect& b) noexcept
  {
    return {a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top, a.right < b.right ? a.right : b.right, a.bottom < b.bottom ? a.bottom : b.bottom};
  }

  [[nodiscard]] constexpr bool Contains(const Rect& r, const float x, const float y) noexcept
  {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
  }

  /** The quad (given by its bounding box) lies completely inside `r`. */
  [[nodiscard]] constexpr bool Inside(const Rect& r, const float minX, const float minY, const float maxX, const float maxY) noexcept
  {
    return minX >= r.left && maxX <= r.right && minY >= r.top && maxY <= r.bottom;
  }

  /** The quad (given by its bounding box) has no point in common with `r`. */
  [[nodiscard]] constexpr bool Outside(const Rect& r, const float minX, const float minY, const float maxX, const float maxY) noexcept
  {
    return maxX <= r.left || minX >= r.right || maxY <= r.top || minY >= r.bottom;
  }

  /**
   * Sutherland-Hodgman: the polygon `in` (`count` vertices, at most 8) cut to `r`. `V` has `mX` and `mY`; `lerp(a, b, t)` makes the vertex at `t` between two of them (position
   * and every attribute). The result goes to `out` (room for count + 4 vertices) and its size is returned (0 when nothing is left, 3 or more otherwise).
   */
  template <class V, class Lerp>
  int ClipPolygon(const V* const in, const int count, const Rect& r, V* const out, Lerp lerp)
  {
    constexpr int kMax = 16;
    V bufferA[kMax];
    V bufferB[kMax];
    if (count < 3 || count > 8) {
      return 0;
    }
    for (int i = 0; i < count; ++i) {
      bufferA[i] = in[i];
    }
    V* src = bufferA;
    V* dst = bufferB;
    int n = count;

    // side: 0 left, 1 right, 2 top, 3 bottom. dist > = 0 is inside.
    for (int side = 0; side < 4 && n > 0; ++side) {
      auto dist = [&](const V& v) -> float {
        switch (side) {
        case 0: return v.mX - r.left;
        case 1: return r.right - v.mX;
        case 2: return v.mY - r.top;
        default: return r.bottom - v.mY;
        }
      };
      int m = 0;
      for (int i = 0; i < n; ++i) {
        const V& a = src[i];
        const V& b = src[(i + 1) % n];
        const float da = dist(a);
        const float db = dist(b);
        if (da >= 0.0f) {
          dst[m++] = a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
          const float t = da / (da - db);
          dst[m++] = lerp(a, b, t);
        }
      }
      std::swap(src, dst);
      n = m;
    }

    if (n < 3) {
      return 0;
    }
    float minX = src[0].mX, maxX = src[0].mX, minY = src[0].mY, maxY = src[0].mY;
    for (int i = 1; i < n; ++i) {
      minX = std::min(minX, src[i].mX);
      maxX = std::max(maxX, src[i].mX);
      minY = std::min(minY, src[i].mY);
      maxY = std::max(maxY, src[i].mY);
    }
    if (!(maxX > minX && maxY > minY)) {          // a sliver on the clip edge: no area
      return 0;
    }
    for (int i = 0; i < n; ++i) {
      out[i] = src[i];
    }
    return n;
  }

  // ---- the controls ---------------------------------------------------------------------------------------------------------------------------------

  /** Control:SetClipChildren(on). */
  void SetClipChildren(const CMauiControl* control, bool on);

  [[nodiscard]] bool GetClipChildren(const CMauiControl* control);

  /** The control is gone (~CMauiControl). */
  void Forget(const CMauiControl* control);

  /** Some control clips (the fast exit of everything below). */
  [[nodiscard]] bool Any();

  /** Starts a frame of drawing: the clip rectangles worked out for the controls of the last frame are dropped. */
  void BeginFrame();

  /**
   * The rectangle the drawing of `control` is cut to: the intersection of the rectangles of every ancestor that clips. False when no ancestor clips. Remembered for the frame (one
   * walk per parent, not per control). The control's own rectangle does not clip the control itself.
   */
  [[nodiscard]] bool EffectiveRect(const CMauiControl* control, Rect& out);

  /** (x, y) is a point of `control` the user can see: no clipping ancestor cuts it away. Not remembered (hit tests are rare). */
  [[nodiscard]] bool PointVisible(const CMauiControl* control, float x, float y);
} // namespace moho::editor::uiclip
