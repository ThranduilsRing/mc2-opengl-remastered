// G7b K3: storage for the per-frame render clock and render list (see render_view.h). It lives in mclib next to
// tick_commit.cpp, which clears the list at commit, so every target that links mclib resolves it.
#include "render_view.h"

namespace mc2::render_view {

namespace {
RenderClock g_clock;
RenderList g_list;
}  // namespace

RenderClock& clock() { return g_clock; }
RenderList& list() { return g_list; }

}  // namespace mc2::render_view
