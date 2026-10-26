#pragma once

#include <cstdint>
#include <string>

#include "moho/math/Vector3f.h"
#include "moho/render/d3d/CD3DFontTypes.h"

namespace moho
{
  class CD3DPrimBatcher;

  /**
   * Editor addition (docs/projects/editor_research/SPIKE_CLEAN_TEXT.md): a DirectWrite text backend behind `CD3DFont`.
   * A font whose face name starts with `ed:` (for example `ed:Segoe UI Variable Text`) is shaped and rasterised by
   * `edtext.dll` (editor/edtext) instead of GDI: sub-pixel glyph positions (4 phases), kerning, full Unicode with font
   * fallback. Retail UI fonts are untouched. If the DLL cannot be loaded, `Attach` returns null and the font stays on
   * the GDI path with the family name stripped of the prefix.
   */
  namespace edtext
  {
    [[nodiscard]] bool IsEdFace(const char* face);
    /** Face name without the `ed:` prefix and without a trailing `;g` (gray) flag. */
    [[nodiscard]] std::string FamilyOf(const char* face);
    /** `ed:Family` draws LCD (ClearType-style, two-pass technique `TTextLcd`); `ed:Family;g` draws gray coverage with the normal technique. */
    [[nodiscard]] bool WantsLcd(const char* face);

    /** Creates the per-font backend state; returns null if `edtext.dll` is unavailable. Metrics are unrounded pixels. */
    [[nodiscard]] void* Attach(const char* face, float pixelSize, float& ascent, float& descent, float& lineGap);
    void Destroy(void* state);

    [[nodiscard]] Vector3f Render(
      void* state,
      const char* text,
      CD3DPrimBatcher* primBatcher,
      const Vector3f& origin,
      const Vector3f& xAxis,
      const Vector3f& yAxis,
      std::uint32_t color,
      float maxAdvance
    );
    [[nodiscard]] float GetAdvance(void* state, const char* text);
    [[nodiscard]] std::int32_t GetNearestCharacterIndex(void* state, const char* text, float targetAdvance);
    [[nodiscard]] const CD3DFontCharInfo& GetCharInfo(void* state, wchar_t chText);
  } // namespace edtext
} // namespace moho
