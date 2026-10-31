// G7b K3: storage for the per-frame render clock and render list (see render_view.h). It lives in mclib next to
// tick_commit.cpp, which clears the list at commit, so every target that links mclib resolves it.
#include "render_view.h"
#include "../RenderCore/FrameArena.h"
#include <cstdlib>

namespace mc2::render_view {

namespace {
RenderClock g_clock;
RenderList g_list;
HudSnapshot g_hud;
}  // namespace

RenderClock& clock() { return g_clock; }
RenderList& list() { return g_list; }
HudSnapshot& hud() { return g_hud; }

void RenderList::freezeBoltDraws(RenderCore::FrameArena& arena) {
    uint32_t count = 0;
    for (const auto& view : other) if (view.has(BoltGeometry)) ++count;
    if (!count) return;
    auto draws = arena.allocArray<BoltDraw>(count, "BoltDraw");
    if (draws.empty()) std::abort();
    uint32_t i = 0;
    for (auto& view : other) {
        if (!view.has(BoltGeometry)) continue;
        draws.data[i] = view.bolt;
        view.frozenBolt = &draws.data[i++];
    }
}

}  // namespace mc2::render_view
