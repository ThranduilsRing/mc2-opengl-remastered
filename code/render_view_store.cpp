// G7b K3: storage for the per-frame render clock and render list (see render_view.h). It lives in mclib next to
// tick_commit.cpp, which clears the list at commit, so every target that links mclib resolves it.
#include "render_view.h"
#include "../RenderCore/FrameArena.h"
#include "../mclib/sim_overlap.h"
#include <cstdlib>

namespace mc2::render_view {

namespace {
RenderClock g_clock;
RenderList g_list;
HudSnapshot g_hud;
thread_local const FrameDraws* g_drawFrame = nullptr;
}  // namespace

RenderClock& stagingClock() {
    mc2::overlap::assertMainThread("stagingRenderClock");
    return g_clock;
}
RenderList& stagingList() {
    mc2::overlap::assertMainThread("stagingRenderList");
    return g_list;
}
HudSnapshot& stagingHud() {
    mc2::overlap::assertMainThread("stagingHud");
    return g_hud;
}
const RenderClock& clock() { return g_drawFrame ? g_drawFrame->clock : g_clock; }
const RenderList& list() { return g_drawFrame ? g_drawFrame->list : g_list; }
const HudSnapshot& hud() { return g_drawFrame ? g_drawFrame->hud : g_hud; }

std::shared_ptr<const FrameDraws> freezeDrawFrame(uint64_t sequence) {
    mc2::overlap::assertMainThread("freezeDrawFrame");
    if (!sequence) std::abort();
    return std::make_shared<const FrameDraws>(FrameDraws{sequence, g_clock, g_list, g_hud});
}

DrawFrameScope::DrawFrameScope(const FrameDraws& frame) : previous_(g_drawFrame) {
    g_drawFrame = &frame;
}
DrawFrameScope::~DrawFrameScope() { g_drawFrame = previous_; }

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
