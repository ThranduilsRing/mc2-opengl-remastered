// render_contract.cpp — Render Contract Registry table (phase 1).
//
// CONSERVATIVE entries only. Every row whose current contract is not fully
// audited is marked TODO_RENDER_CONTRACT in the comment column. The values
// returned for those rows are not authoritative; callers must not rely on
// them for behavioral decisions during phase 1.
//
// Authoritative entries (TerrainBase, TerrainOverlay, TerrainDecal, Grass,
// StaticProp, PostProcess) are derived from the GBuffer1 producer/reader
// inventory in the design spec, section 1.1 / section 5. Other rows are
// placeholders pending commit-7 callsite-marker confirmation.
//
// See: docs/superpowers/specs/2026-04-26-render-contract-registry-design.md

#include "render_contract.h"

namespace render_contract {

namespace {

using BM = PassStateContract::BlendMode;

// TODO_RENDER_CONTRACT sentinel — used for rows pending audit.
constexpr ShadowContract kTodoShadow{
    /*castsStaticShadow*/  false,
    /*castsDynamicShadow*/ false,
    /*skipsPostScreenShadow*/ false,
};
constexpr PassStateContract kTodoState{
    /*requiresDepthTest*/  false,
    /*requiresDepthWrite*/ false,
    /*blend*/              BM::Opaque,
    /*requiresMRT*/        false,
    /*expectedFBO*/        "TODO_RENDER_CONTRACT",
    /*restoresStateOnExit*/false,
};

// ----- Authoritative entries (from spec §5 routing table) -------------------

constexpr ShadowContract kTerrainBaseShadow{
    /*castsStaticShadow*/    true,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,   // gos_terrain.frag writes alpha=1.0
};
constexpr PassStateContract kTerrainBaseState{
    /*requiresDepthTest*/  true,
    /*requiresDepthWrite*/ true,
    /*blend*/              BM::Opaque,
    /*requiresMRT*/        true,
    /*expectedFBO*/        "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/false,
};

constexpr ShadowContract kTerrainOverlayShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,   // terrain_overlay.frag writes alpha=1.0
};
constexpr PassStateContract kTerrainOverlayState{
    /*requiresDepthTest*/  true,
    /*requiresDepthWrite*/ false,
    /*blend*/              BM::AlphaBlend,
    /*requiresMRT*/        true,
    /*expectedFBO*/        "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/false,
};

constexpr ShadowContract kTerrainDecalShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,   // decal.frag writes alpha=1.0
};
constexpr PassStateContract kTerrainDecalState{
    /*requiresDepthTest*/  true,
    /*requiresDepthWrite*/ false,
    /*blend*/              BM::AlphaBlend,
    /*requiresMRT*/        true,
    /*expectedFBO*/        "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/false,
};

constexpr ShadowContract kGrassShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,   // gos_grass.frag writes alpha=1.0
};
constexpr PassStateContract kGrassState{
    /*requiresDepthTest*/  true,
    /*requiresDepthWrite*/ true,
    /*blend*/              BM::AlphaTest,
    /*requiresMRT*/        true,
    /*expectedFBO*/        "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/false,
};

constexpr ShadowContract kStaticPropShadow{
    /*castsStaticShadow*/    true,
    /*castsDynamicShadow*/   true,
    /*skipsPostScreenShadow*/false,  // static_prop.frag writes alpha=0.0
};
constexpr PassStateContract kStaticPropState{
    /*requiresDepthTest*/  true,
    /*requiresDepthWrite*/ true,
    /*blend*/              BM::Opaque,
    /*requiresMRT*/        true,
    /*expectedFBO*/        "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/false,
};

constexpr ShadowContract kPostProcessShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,  // not applicable; pass operates on G-buffer
};
constexpr PassStateContract kPostProcessState{
    /*requiresDepthTest*/  false,
    /*requiresDepthWrite*/ false,
    /*blend*/              BM::Opaque,
    /*requiresMRT*/        false,    // single-attachment fullscreen pass
    /*expectedFBO*/        "post FBO (single attachment)",
    /*restoresStateOnExit*/false,
};

// ----- Lookup helper --------------------------------------------------------

const ShadowContract& lookupShadow(PassIdentity id) {
    switch (id) {
        case PassIdentity::TerrainBase:    return kTerrainBaseShadow;
        case PassIdentity::TerrainOverlay: return kTerrainOverlayShadow;
        case PassIdentity::TerrainDecal:   return kTerrainDecalShadow;
        case PassIdentity::Grass:          return kGrassShadow;
        case PassIdentity::StaticProp:     return kStaticPropShadow;
        case PassIdentity::PostProcess:    return kPostProcessShadow;
        // TODO_RENDER_CONTRACT — pending commit-7 audit:
        case PassIdentity::Unknown:
        case PassIdentity::Water:
        case PassIdentity::OpaqueObject:
        case PassIdentity::AlphaObject:
        case PassIdentity::ParticleEffect:
        case PassIdentity::UI:
        case PassIdentity::DebugOverlay:
        case PassIdentity::ShadowCaster:
            return kTodoShadow;
    }
    return kTodoShadow;
}

const PassStateContract& lookupState(PassIdentity id) {
    switch (id) {
        case PassIdentity::TerrainBase:    return kTerrainBaseState;
        case PassIdentity::TerrainOverlay: return kTerrainOverlayState;
        case PassIdentity::TerrainDecal:   return kTerrainDecalState;
        case PassIdentity::Grass:          return kGrassState;
        case PassIdentity::StaticProp:     return kStaticPropState;
        case PassIdentity::PostProcess:    return kPostProcessState;
        case PassIdentity::Unknown:
        case PassIdentity::Water:
        case PassIdentity::OpaqueObject:
        case PassIdentity::AlphaObject:
        case PassIdentity::ParticleEffect:
        case PassIdentity::UI:
        case PassIdentity::DebugOverlay:
        case PassIdentity::ShadowCaster:
            return kTodoState;
    }
    return kTodoState;
}

} // namespace

const ShadowContract&    shadowContractFor(PassIdentity id) { return lookupShadow(id); }
const PassStateContract& stateContractFor(PassIdentity id)  { return lookupState(id); }

const char* passIdentityName(PassIdentity id) {
    switch (id) {
        case PassIdentity::Unknown:        return "Unknown";
        case PassIdentity::TerrainBase:    return "TerrainBase";
        case PassIdentity::TerrainOverlay: return "TerrainOverlay";
        case PassIdentity::TerrainDecal:   return "TerrainDecal";
        case PassIdentity::Grass:          return "Grass";
        case PassIdentity::Water:          return "Water";
        case PassIdentity::OpaqueObject:   return "OpaqueObject";
        case PassIdentity::AlphaObject:    return "AlphaObject";
        case PassIdentity::StaticProp:     return "StaticProp";
        case PassIdentity::ParticleEffect: return "ParticleEffect";
        case PassIdentity::UI:             return "UI";
        case PassIdentity::DebugOverlay:   return "DebugOverlay";
        case PassIdentity::ShadowCaster:   return "ShadowCaster";
        case PassIdentity::PostProcess:    return "PostProcess";
    }
    return "Unknown";
}

} // namespace render_contract
