// G7b K3: storage for the per-frame render clock and render list (see render_view.h). It lives in mclib next to
// tick_commit.cpp, which clears the list at commit, so every target that links mclib resolves it.
#include "render_view.h"
#include "../RenderCore/FrameArena.h"
#include "../mclib/sim_overlap.h"
#include <cstdlib>
#include <mutex>
#include <utility>
#include "../mclib/battle_bench_sink.h"

namespace mc2::render_view {

namespace {
RenderClock g_clock;
RenderList g_list;
HudSnapshot g_hud;
std::mutex g_poolMutex;
std::vector<std::unique_ptr<FrameDraws>> g_pool;   // recycled frames: vectors keep their capacity
thread_local const FrameDraws* g_drawFrame = nullptr;
thread_local RenderCore::FrameArena* g_drawArena = nullptr;
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
    {   // [PERF_COUNTERS v1] element storage copied by the extract, and an allocation count (one per non-empty vector + the shared block)
        namespace bb = mc2::battlebench;
        const auto& h = g_hud;
        bb::add(bb::ExtractBytes, (g_list.terrain.size() + g_list.movers.size() + g_list.other.size()) * sizeof(RenderView)
            + g_list.fx.size() * sizeof(FxPacket) + g_list.effects.size() * sizeof(gosFX::Effect*)
            + h.movers.size() * sizeof(HudMover) + h.roster.size() * sizeof(size_t) + (h.sensors.size() + h.coverage.size()) * sizeof(HudSensor)
            + h.objectives.size() * sizeof(HudObjective) + h.blips.size() * sizeof(HudBlip) + h.vtols.size() * sizeof(HudVtol));
        bb::add(bb::HeapAllocs, 1 + (g_list.terrain.empty() ? 0 : 1) + (g_list.movers.empty() ? 0 : 1) + (g_list.other.empty() ? 0 : 1)
            + (g_list.fx.empty() ? 0 : 1) + (g_list.effects.empty() ? 0 : 1)
            + (h.movers.empty() ? 0 : 1) + (h.roster.empty() ? 0 : 1) + (h.sensors.empty() ? 0 : 1) + (h.coverage.empty() ? 0 : 1)
            + (h.objectives.empty() ? 0 : 1) + (h.blips.empty() ? 0 : 1) + (h.vtols.empty() ? 0 : 1));
    }
    // Hand the staged streams to the frame by swapping with a recycled FrameDraws (no per-frame element copy, no
    // allocation once the pool is warm); staging takes the recycled, emptied vectors and their capacity. The clock is a
    // small POD and stays readable outside a draw scope (Mission::update), so it is copied.
    FrameDraws* frame;
    {
        std::lock_guard<std::mutex> lock(g_poolMutex);
        if (g_pool.empty()) frame = std::make_unique<FrameDraws>().release();
        else { frame = g_pool.back().release(); g_pool.pop_back(); }
    }
    frame->sequence = sequence;
    frame->clock = g_clock;
    frame->list.terrain.swap(g_list.terrain);
    frame->list.movers.swap(g_list.movers);
    frame->list.other.swap(g_list.other);
    frame->list.fx.swap(g_list.fx);
    frame->list.effects.swap(g_list.effects);
    std::swap(frame->hud, g_hud);
    return std::shared_ptr<const FrameDraws>(frame, [](const FrameDraws* done) {
        auto* f = const_cast<FrameDraws*>(done);   // sole owner here; empty the streams (marker holders release on this thread) and recycle
        f->list.clear();
        f->hud.clear();
        std::lock_guard<std::mutex> lock(g_poolMutex);
        g_pool.emplace_back(f);
    });
}

RenderCore::FrameArena* drawArena() { return g_drawArena; }

DrawFrameScope::DrawFrameScope(const FrameDraws& frame, RenderCore::FrameArena* arena)
    : previous_(g_drawFrame), previousArena_(g_drawArena) {
    g_drawFrame = &frame;
    g_drawArena = arena;
}
DrawFrameScope::~DrawFrameScope() {
    g_drawFrame = previous_;
    g_drawArena = previousArena_;
}

}  // namespace mc2::render_view
