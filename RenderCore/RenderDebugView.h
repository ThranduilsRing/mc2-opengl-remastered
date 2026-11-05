// RenderCore/RenderDebugView.h
// DEBUG-VIEW-REGISTRY-1: canonical render debug view vocabulary.
// Each lane supports a subset defined by its kDebugViewMask_* constant.
#pragma once
#include <cstdint>

enum class RenderDebugView : int {
    Final        = 0,   // default: no override
    Albedo       = 1,
    Normal       = 2,
    Roughness    = 3,
    Metallic     = 4,
    LightingOnly = 5,
    IblOnly      = 6,
    SpecularOnly = 7,
    MaterialIdx  = 8,   // static_prop lane only
    TexArrayLayer= 9,   // static_prop lane only
    _Count       = 10,
};

// Per-lane support masks.  Bit N = RenderDebugView(N) is supported.
// StaticPropOpaque: modes that map to a real shader branch (no unsupported stubs).
constexpr uint32_t kDebugViewMask_StaticPropOpaque =
    (1u << static_cast<int>(RenderDebugView::Final))        |
    (1u << static_cast<int>(RenderDebugView::Albedo))       |
    (1u << static_cast<int>(RenderDebugView::Normal))       |
    (1u << static_cast<int>(RenderDebugView::Roughness))    |
    (1u << static_cast<int>(RenderDebugView::Metallic))     |
    (1u << static_cast<int>(RenderDebugView::MaterialIdx))  |
    (1u << static_cast<int>(RenderDebugView::TexArrayLayer));

// Mech lane: modes that map to real mech.frag branches (u_debugMode).
// mech.frag has no roughness/metallic/IBL/specular/materialIdx data.
// MECH-DEBUG-VIEWS-1: enum -> mech.frag u_debugMode mapping (see
// MechViewToFragDebugMode): Final->0, Albedo->2, Normal->4, LightingOnly->3.
constexpr uint32_t kDebugViewMask_Mech =
    (1u << static_cast<int>(RenderDebugView::Final))        |
    (1u << static_cast<int>(RenderDebugView::Albedo))       |
    (1u << static_cast<int>(RenderDebugView::Normal))       |
    (1u << static_cast<int>(RenderDebugView::LightingOnly));

// VFX lane: particle_billboard.frag u_debugMode. Only Final/Albedo map to the
// canonical RenderDebugView vocabulary (Final->mode 0, Albedo->mode 1). The
// shader also implements VFX-LOCAL modes with no canonical enum slot — Alpha(2),
// ParticleKind(3), Overdraw(4) — reachable via MC2_VFX_DEBUG_MODE; promote to
// the enum only if they become cross-lane. Particles have no normal/roughness/
// metallic/IBL/specular/materialIdx data, so those modes are intentionally absent.
constexpr uint32_t kDebugViewMask_Vfx =
    (1u << static_cast<int>(RenderDebugView::Final))        |
    (1u << static_cast<int>(RenderDebugView::Albedo));

// Placeholders -- zero until the lane implements debug views.
constexpr uint32_t kDebugViewMask_Terrain  = 0u;
constexpr uint32_t kDebugViewMask_Shadow   = 0u;

inline bool RenderDebugViewSupported(RenderDebugView v, uint32_t mask) {
    int i = static_cast<int>(v);
    if (i < 0 || i >= static_cast<int>(RenderDebugView::_Count)) return false;
    return (mask >> i) & 1u;
}

// Declared here, defined in RenderDebugView.cpp.
const char* RenderDebugViewName(RenderDebugView v);
const char* RenderDebugViewDescription(RenderDebugView v);
