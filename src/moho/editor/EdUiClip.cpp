// EDITOR: clipping for Maui, the control side (EdUiClip.h).
#include "moho/editor/EdUiClip.h"

#include <unordered_map>
#include <unordered_set>

#include "moho/ui/UiRuntimeTypes.h"

namespace moho::editor::uiclip
{
  namespace
  {
    struct Memo
    {
      bool clipped = false;
      Rect rect{};
    };

    std::unordered_set<const CMauiControl*>& Clippers()
    {
      static std::unordered_set<const CMauiControl*> set;
      return set;
    }

    std::unordered_map<const CMauiControl*, Memo>& Memos()
    {
      static std::unordered_map<const CMauiControl*, Memo> map;
      return map;
    }

    Rect RectOf(const CMauiControl* const control)
    {
      CMauiControl* const c = const_cast<CMauiControl*>(control);
      Rect r;
      r.left = CScriptLazyVar_float::GetValue(&c->mLeftLV);
      r.top = CScriptLazyVar_float::GetValue(&c->mTopLV);
      r.right = CScriptLazyVar_float::GetValue(&c->mRightLV);
      r.bottom = CScriptLazyVar_float::GetValue(&c->mBottomLV);
      return r;
    }

    // what cuts the drawing of `control`: its parent's clip plus the parent's own rectangle when the parent clips
    Memo Work(const CMauiControl* const control, const bool remember)
    {
      const CMauiControl* const parent = control != nullptr ? control->mParent : nullptr;
      if (parent == nullptr) {
        return {};
      }
      if (remember) {
        const auto found = Memos().find(control);
        if (found != Memos().end()) {
          return found->second;
        }
      }
      Memo m = Work(parent, remember);
      if (Clippers().count(parent) != 0u) {
        const Rect own = RectOf(parent);
        m.rect = m.clipped ? Intersect(m.rect, own) : own;
        m.clipped = true;
      }
      if (remember) {
        Memos()[control] = m;
      }
      return m;
    }
  } // namespace

  void SetClipChildren(const CMauiControl* const control, const bool on)
  {
    if (control == nullptr) {
      return;
    }
    if (on) {
      Clippers().insert(control);
    } else {
      Clippers().erase(control);
    }
    Memos().clear();
  }

  bool GetClipChildren(const CMauiControl* const control)
  {
    return Clippers().count(control) != 0u;
  }

  void Forget(const CMauiControl* const control)
  {
    if (Clippers().empty()) {
      return;
    }
    if (Clippers().erase(control) != 0u) {
      Memos().clear();
    }
  }

  bool Any()
  {
    return !Clippers().empty();
  }

  void BeginFrame()
  {
    if (!Memos().empty()) {
      Memos().clear();
    }
  }

  bool EffectiveRect(const CMauiControl* const control, Rect& out)
  {
    if (Clippers().empty()) {
      return false;
    }
    const Memo m = Work(control, true);
    if (m.clipped) {
      out = m.rect;
    }
    return m.clipped;
  }

  bool PointVisible(const CMauiControl* const control, const float x, const float y)
  {
    if (Clippers().empty()) {
      return true;
    }
    const Memo m = Work(control, false);
    return !m.clipped || Contains(m.rect, x, y);
  }
} // namespace moho::editor::uiclip
