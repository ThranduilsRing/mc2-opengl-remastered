// render_contract.h — Render Contract Registry (phase 1, inert).
//
// Names the implicit contracts that govern how each render path interacts
// with the G-buffer, shadow pipeline, depth/blend state, and post-process
// passes. Phase 1 is documentation + types only. No callsite changes; no
// behavior change. See:
//
//   docs/superpowers/specs/2026-04-26-render-contract-registry-design.md
//
// Two load-bearing audit findings drove this header:
//
//   1. GBuffer1.alpha is a POST-PROCESS SHADOW MASK, not a "terrain flag."
//      Terrain, grass, terrain decals, and terrain overlays all opt in by
//      writing 1.0; shadow_screen.frag's "isTerrain" local is misnamed.
//
//   2. The contract is already silently ambiguous: gos_terrain.frag writes
//      a continuous materialAlpha into the same channel for water/shoreline
//      pixels, and several MRT-bound shaders do not declare GBuffer1 at all.
//
// This header gives the contract names. It does not change pixels.

#ifndef MC2_RENDER_CONTRACT_H
#define MC2_RENDER_CONTRACT_H

#include <cstdint>

namespace render_contract {

// The kind of draw call. Queryable from material setup, shadow paths, FBO
// state machine. Phase 1 default for an unclassified callsite is Unknown;
// callsites are tagged via the // [RENDER_CONTRACT:Pass=...] marker as
// commit 7 of the impl plan walks the codebase.
enum class PassIdentity : std::uint8_t {
    Unknown = 0,
    TerrainBase,        // tessellated heightfield (gos_terrain.frag)
    TerrainOverlay,     // perimeter cement / transitions (terrain_overlay.frag)
    TerrainDecal,       // craters, footprints, scorch marks (decal.frag)
    Grass,              // GPU grass (gos_grass.frag) — terrain-derived
    Water,              // water surface + detail (intentional projected path)
    OpaqueObject,       // mechs, vehicles, buildings — opaque pass
    AlphaObject,        // same shapes when alpha-tested or alpha-blended
    StaticProp,         // GPU static-prop renderer (static_prop.frag)
    ParticleEffect,     // weapon bolts, weather, clouds, explosions
    UI,                 // HUD, text, menu (screen-space)
    DebugOverlay,       // F1 / F2 / RAlt+P / etc. — diagnostic only
    ShadowCaster,       // depth-only pass for static or dynamic shadow map
    PostProcess,        // shadow_screen, ssao, bloom, godray, shoreline
};

// What each G-buffer attachment slot carries. The alpha of slot 1 is
// renamed honestly: it is a post-process shadow mask, not a terrain flag.
// Legacy code/comments may still call it "terrain flag"; the registry
// does not preserve that misleading name as the primary API.
enum class GBufferSlot : std::uint8_t {
    Color0_Albedo = 0,
    Normal1_PostShadowMask = 1,   // RGB = normal*0.5+0.5; a > 0.5 → skip post-shadow
};

// A pass's shadow-pipeline relationship. Computed once per material setup
// (phase 2+); for phase 1, mirrors the existing conjunction at each
// callsite without changing it. The single "skipsPostScreenShadow" field
// is the semantic; the storage mechanism (GBuffer1.alpha > 0.5) is
// documented in render_contract.hglsl and may evolve under follow-up F1.
struct ShadowContract {
    bool castsStaticShadow;       // included in static terrain shadow atlas
    bool castsDynamicShadow;      // included in dynamic local shadow map
    bool skipsPostScreenShadow;   // shadow_screen.frag will not darken this pixel
};

// A pass's GL-state contract. Documents what the pass requires on entry
// and guarantees on exit. Phase 1: descriptive only, NOT enforced.
// Phase 2 (separate spec): debug-build assertions.
struct PassStateContract {
    enum class BlendMode : std::uint8_t { Opaque, AlphaBlend, AlphaTest, Additive };

    bool        requiresDepthTest;
    bool        requiresDepthWrite;
    BlendMode   blend;
    bool        requiresMRT;          // expects glDrawBuffers(2, {COLOR0, COLOR1})
    const char* expectedFBO;          // documentary string
    bool        restoresStateOnExit;  // overlay must restore; main passes need not
};

// Registry accessors. Defined in render_contract.cpp.
//
// Phase-1 entries are CONSERVATIVE. Rows whose current contract is not
// fully audited at commit-1 time are returned as TODO_RENDER_CONTRACT
// sentinel values; callers must not rely on the table values for
// behavioral decisions during phase 1. Authoritative entries are filled
// in as commits 7 (markers) and 9 (closing report) confirm them.
const ShadowContract&    shadowContractFor(PassIdentity);
const PassStateContract& stateContractFor(PassIdentity);
const char*              passIdentityName(PassIdentity);

} // namespace render_contract

#endif // MC2_RENDER_CONTRACT_H
