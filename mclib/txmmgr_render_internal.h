// Shared surface of the txmmgr_render*.cpp pieces: the renderLists phase helpers, the
// per-frame state they share and the RENDERLISTS cost spans. Declared names are defined
// in txmmgr_render.cpp.
#ifndef TXMMGR_RENDER_INTERNAL_H
#define TXMMGR_RENDER_INTERNAL_H

#include "txmmgr_internal.h"

// Maximum vertices sent down per draw call by the legacy masterVertexNodes loops.
inline constexpr int MAX_SENDDOWN = 10002;

// Number of shadow-shape slots reserved this frame (see addShadowShape).
extern std::atomic<int> g_numShadowShapes;

mat4 gos2my(Stuff::Matrix4D& m);
mat4 gos2my(Stuff::LinearMatrix4D& m);

// [RENDERLISTS_COST v1] coarse per-phase CPU cost split of renderLists().
namespace rlcost {
extern const bool s_enabled;
enum Phase {
    kPreamble = 0, kLightUpload, kSceneData, kObj3d, kStateRestore,
    kStaticShadowBuild, kSpRegistryFlush, kDynShadow, kTerrainSolid,
    kGpuSpPrep, kSpBatcherFlush, kMechFlush, kOverlays, kWaterLoops,
    kNoUnderlayer, kShadowBlobs, kAlphaLoops, kVfxHud, kTotal, kPhaseCount
};
extern const char* kName[kPhaseCount];
extern unsigned long long s_ns[kPhaseCount];
extern int s_frames;

inline unsigned long long nowNs() {
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Lexical phase span. Zero-cost (two branch-predicted tests) when disabled.
struct Span {
    int p; unsigned long long t0; GosGlLostDrawsScope lost_;
    explicit Span(int phase) : p(phase), t0(s_enabled ? nowNs() : 0ULL), lost_(kName[phase]) {}
    ~Span() { if (s_enabled) s_ns[p] += nowNs() - t0; }
};

// Whole-function span + window emit. Construct FIRST in renderLists() so the
// destructor runs LAST: accumulates total, then emits every 60 frames.
struct TotalSpan {
    unsigned long long t0; long nodes_; long hwNodes_;
    TotalSpan(long nodes, long hwNodes)
        : t0(s_enabled ? nowNs() : 0ULL), nodes_(nodes), hwNodes_(hwNodes) {}
    ~TotalSpan();
};
}

// Per-frame state the renderLists phases share.
struct MC_TextureManager::RenderListsFrame
{
#ifdef MC2_VULKAN_ISLAND
	bool vkLegacyOn = false;   // VK legacy-shape island armed for this frame
#endif
};

#endif  // TXMMGR_RENDER_INTERNAL_H
