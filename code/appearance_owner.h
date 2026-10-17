// B7o.3: the owner/view protocol behind GameObject::adoptAppearance / releaseAppearance, header-only so the unit tests exercise
// the production code (GameObject itself cannot be linked into mc2_tests).
//   owner : std::unique_ptr<T>, the only thing that frees
//   view  : the raw pointer the 587 readers use, written ONLY through these helpers
#pragma once

#include <memory>

namespace mc2 {

// Take ownership of `p` (may be null). A previous appearance is freed by the reset; the view follows.
template <class T>
inline void adoptOwnedView (std::unique_ptr<T>& owner, T*& view, T* p) noexcept
{
	owner.reset(p);
	view = p;
}

// Free the appearance now (at the caller's existing position, so per-class release order is unchanged) and null the view.
template <class T>
inline void releaseOwnedView (std::unique_ptr<T>& owner, T*& view) noexcept
{
	owner.reset();
	view = nullptr;
}

// True when the view no longer matches the owner (someone wrote the raw pointer). Logged by the caller, never asserted (G12).
template <class T>
inline bool ownedViewDiverged (const std::unique_ptr<T>& owner, const T* view) noexcept
{
	return view != owner.get();
}

} // namespace mc2
