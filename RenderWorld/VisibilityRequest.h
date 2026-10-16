// RenderWorld/VisibilityRequest.h
//
// Seq C: VisibilityRequest v0 + V1A.
//
// v0 contract: reporting-only, lifecycle counts only.
//   - wraps existing visibility/cull facts; no new culling decisions
//   - output: counts by kind only (handles deferred to v1+)
//   - terrain=deferred, vfx=prohibited, overlay=undefined
//   - no draw-submission change, no behavior change
//
// V1A extension: per-frame visible/submitted counts via flush-entry latches.
//   - static_props_visible: GpuStaticPropRegistry::getLastFlushLiveCount()
//   - mechs_visible:        batcher_getLastFlushSubmitCount()
//   - gpu_visible:          deferred (GPU readback, V1B)
//   - CPU validity flags are always true; GPU results may be unavailable
//
// Forbidden (V1A scope, same as v0):
//   - replace terrain / mech / static-prop culling
//   - feed draw submission
//   - become authoritative visibility

#pragma once
#include <cstdint>

namespace RenderWorld {

// Bit field for the kinds to include in a VisibilityResult.
// Matches RenderObjectKind ordinals (0=StaticProp, 1=Mech).
struct VisibilityKindMask {
    static constexpr uint32_t StaticProp = 1u << 0;
    static constexpr uint32_t Mech       = 1u << 1;
    static constexpr uint32_t All        = StaticProp | Mech;
};

// Reserved layer bits. v0: all bits ignored.
struct VisibilityLayerMask {
    static constexpr uint32_t Default = 0u;
};

// Input to queryVisibility().
// viewId / kindMask / layerMask are stored but unused in v0.
struct VisibilityRequest {
    uint32_t viewId    = 0;                        // opaque camera token
    uint32_t kindMask  = VisibilityKindMask::All;  // which kinds to count
    uint32_t layerMask = VisibilityLayerMask::Default;
};

// Output of queryVisibility().
//
// V0 fields: lifecycle counts (registered-and-alive, not per-frame visible).
// V1A fields: per-frame visible/submitted counts from flush-entry latches.
//
// Valid flags:
//   static_props_visible_valid / mechs_visible_valid are always true;
//     value is 0 before first flush (mission not loaded), which is genuine.
//   gpu_visible_valid — false until GPU compute readback is wired (V1B).
struct VisibilityResult {
    // --- V0 fields (lifecycle) ---
    uint64_t static_props = 0;  // GpuStaticPropRegistry active (alive) count
    uint64_t mechs        = 0;  // RenderWorld mech-alive counter

    // --- V1A fields (per-frame) ---
    // static_props_visible: count of recipe ranges admitted by markVisible()
    //   this frame, latched at GpuStaticPropRegistry::flush() entry.
    //   Domain: CPU-side frustum+cull gate; not GPU compute readback.
    uint64_t static_props_visible       = 0;
    // mechs_visible: count of mech actor submits to GpuMechBatcher this
    //   frame, latched at GpuMechBatcher::flush() entry.
    //   Domain: GpuMechBatcher submits ONLY. Excludes MLR fallback draws.
    uint64_t mechs_visible              = 0;
    // static_props_gpu_visible: GPU compute readback visible count (deferred).
    //   Always 0 until V1B wires readback_getLastGoodVisibleCount().
    uint64_t static_props_gpu_visible   = 0;

    // terrain  -> deferred (CPU terrain picking remains canonical)
    // vfx      -> prohibited (no stable object identity)
    // overlay  -> undefined

    bool static_props_visible_valid = true;  // always true; 0 = genuine pre-mission
    bool mechs_visible_valid        = true;  // always true; 0 = genuine pre-mission
    bool gpu_visible_valid          = false; // false until V1B; MUST NOT interpret 0 as "zero visible"
};

} // namespace RenderWorld
