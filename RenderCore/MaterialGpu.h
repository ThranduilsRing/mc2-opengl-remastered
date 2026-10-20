// RenderCore/MaterialGpu.h
//
// GPU-visible material record.
//
// *** RUNTIME STATUS: LIVE ***
//   Static props: table upload default-ON, albedo sampling default-ON (as of 2026-05-26, v5/v7)
//   Mechs:        per-instance materialIdx substrate live (Mech-1, 2026-05-26), shader sampling
//                 pending texture model decision (see docs/superpowers/specs/2026-05-26-mech-material-gpu-mech2-decision.md)
//   Kill switches: MC2_MATERIAL_GPU=0 (disable table upload/bind/compare)
//                  MC2_MATERIAL_GPU_SAMPLE=0 (disable static-prop shader sampling)
//
// GLSL mirror: shaders/include/material_gpu.hglsl
// Manifest:    tools/material_cook/material_manifest.schema.json
// Mirror gate: scripts/check-material-gpu-mirror.sh
//
// std430 contract:
//   All members are uint32_t or float (base align = 4).
//   Struct size = 32 bytes → stride = 32 bytes in a std430 array.
//   Indexed by MaterialHandle::index() (20-bit; max 1M entries).
//
// Texture field schema — per-consumer semantics
//
//   albedoTex
//     static props : GL_TEXTURE_2D_ARRAY layer index (shader-actionable;
//                   shader samples via texture(u_texArr, vec3(uv, layer)))
//     mechs        : mc2::textures::ready() texHandle/slot — compare-only;
//                   NOT shader-actionable without a dedicated texture-model arc.
//                   CPU resolves: get_gosTextureHandle(slot) → gos handle → GL tex.
//     VFX / future : undefined until each consumer defines its own texture identity.
//
//   normalTex / metallicRoughnessTex / emissiveTex
//     Not yet wired for any consumer. kMaterialTexAbsent in all live entries.
//     Future semantics will match albedoTex once a unified texture model exists.
//
//   kMaterialTexAbsent (0xFFFFFFFF) : slot absent; shader uses default.
//
// MATERIAL-M0 — texture-identity semantic, PINNED (contract only; not yet wired).
//
//   The texture fields (albedoTex etc.) carry a uint32 whose *meaning* differs
//   per consumer lane today (the "split-brain" the recon flags). M0 does NOT
//   rewire the consumers; it makes the contract EXPLICIT so the divergence
//   cannot be misread. The enum below names every meaning a texture field can
//   hold and pins who produces and consumes each. A MaterialTextureSemantic
//   field is NOT yet stored in the record — adding it is a later milestone
//   (M2/M4, gated on the mech texture-model decision). Until then each
//   consumer table is *homogeneous* in semantic and that semantic is declared
//   here, not inferred at the sample site.
//
//   See: docs/material-m0-contract.md,
//        docs/superpowers/specs/2026-05-26-mech-material-gpu-mech2-decision.md
//
// CHANGING THIS STRUCT REQUIRES CHANGING THE GLSL MIRROR IN LOCKSTEP.
// Invariant enforced by: scripts/check-material-gpu-mirror.sh
// Reference doc: memory/cpp_glsl_ubo_struct_lockstep.md

#pragma once

#include <cstdint>
#include <cstddef>

namespace RenderCore {

// MATERIAL-M0 — texture-identity semantic, PINNED.
//
// Names the meaning a texture field (albedoTex/normalTex/...) holds for a given
// consumer lane. Each lane's MaterialGpu table is homogeneous in one semantic.
// This enum is the DECLARED contract; it is not (yet) a stored field of the
// record — wiring a per-record semantic is a later milestone (M2/M4). M0 only
// pins the vocabulary so a reader knows what a given table's texture uint means.
//
// Producer/consumer per value:
//   TextureArrayLayer   produced by gos_static_prop_batcher.cpp (array-layer
//                       dedup at finalizeGeometry); consumed shader-side by
//                       static_prop.frag (texture(u_texArr, vec3(uv, layer))).
//                       *** The only shader-actionable semantic today. ***
//   TextureManagerSlot  produced by gos_mech_batcher.cpp (mc2::textures::ready()
//                       slot/handle); COMPARE-ONLY — the mech shader never
//                       samples MaterialGpu. CPU resolves slot -> gos handle
//                       -> GL tex. NOT shader-actionable without a texture arc.
//   RawGlId             asset-viewer only (tools/asset_viewer): a raw GL texture
//                       id bound directly (MaterialSlotTextures). Never reaches
//                       the runtime SSBO; viewer-local identity.
//   DescriptorIndex     future: index into an indirection-table SSBO. Target
//                       semantic once mechs gain a real shader-actionable model.
//   BindlessHandle      future: ARB_bindless_texture (needs AMD driver audit).
enum class MaterialTextureSemantic : uint32_t {
    TextureArrayLayer  = 0,  // static-prop current model (shader-actionable)
    TextureManagerSlot = 1,  // mech current model (compare-only)
    RawGlId            = 2,  // asset-viewer only (raw GL id, viewer-local)
    DescriptorIndex    = 3,  // future: indirection-table SSBO index
    BindlessHandle     = 4,  // future: ARB_bindless_texture
};

// Material flags (bit positions in MaterialGpu::flags).
namespace MaterialFlags {
    // Bit 0 — alpha-test: discard fragments with alpha < 0.5.
    // Mirrors STATIC_PROP_FLAG_ALPHA_TEST in gos_static_prop_batcher.h.
    constexpr uint32_t kAlphaTest          = 1u << 0;
    // Bit 1 — has normal map: normalTex is a valid tangent-space normal map.
    constexpr uint32_t kNormalMap          = 1u << 1;
    // Bit 2 — has metallic-roughness map: metallicRoughnessTex is valid
    //         (R=AO, G=roughness, B=metalness packed).
    constexpr uint32_t kMetallicRoughness  = 1u << 2;
    // Bit 3 — has emissive map: emissiveTex is valid.
    constexpr uint32_t kEmissive           = 1u << 3;
    // Bit 4 — double-sided: back faces are not discarded.
    constexpr uint32_t kDoubleSided        = 1u << 4;
    // Bit 5 — is window: hot-color magic applies; sun/ambient lighting
    //         skipped. Mirrors kFlagIsWindow in static_prop.vert bit 1.
    constexpr uint32_t kWindow             = 1u << 5;
    // Bits 6-31 reserved; must be 0 in cooked manifests.
} // namespace MaterialFlags

// Sentinel: texture slot is absent (no map provided for this material).
constexpr uint32_t kMaterialTexAbsent = 0xFFFFFFFFu;

// Field order matches shaders/include/material_gpu.hglsl exactly.
// Mirror gate: scripts/check-material-gpu-mirror.sh verifies field
// names and order match between the two files at CI time.
struct alignas(4) MaterialGpu {
    // --- Texture indices (4 × uint32 = 16 bytes) ---
    // MATERIAL-M0 — canonical texture-identity meaning per consumer lane (PINNED):
    //   static props (shader-actionable): texture uint == GL_TEXTURE_2D_ARRAY
    //                 layer index  -> MaterialTextureSemantic::TextureArrayLayer.
    //   mechs (compare-only):             texture uint == mc2::textures::ready() slot
    //                 -> MaterialTextureSemantic::TextureManagerSlot. The mech
    //                 shader NEVER samples this record; CPU resolves the slot.
    //   asset viewer (out-of-band):       not in this SSBO; the viewer binds raw
    //                 GL ids (MaterialSlotTextures) -> MaterialTextureSemantic::RawGlId.
    // Each lane's table is HOMOGENEOUS in one semantic. M0 pins/documents only —
    // it does NOT rewire consumers or store the semantic in the record (later
    // milestone). Do NOT add a mech shader consumer without first defining a
    // shader-actionable texture identity for mechs. See decision doc above and
    // docs/material-m0-contract.md.
    //
    // NOT part of this record: TERRAIN splat material. Terrain is a per-pixel-
    // classified fixed-layer palette (TerrainLayerGpu-style, 5 semantic layers
    // selected by a colormap classifier), a DISTINCT ABI that shares field
    // vocabulary (roughness/normal/tint) but has no per-draw materialIdx. See
    // docs/material-m0-contract.md and mclib/terrain.h.
    uint32_t albedoTex;             //  0  — diffuse/albedo; KIND-SEMANTIC (see above)
    uint32_t normalTex;             //  4  — tangent-space normal map
    uint32_t metallicRoughnessTex;  //  8  — R=AO  G=roughness  B=metalness
    uint32_t emissiveTex;           // 12  — emissive color map

    // --- Control word (1 × uint32 = 4 bytes) ---
    uint32_t flags;                 // 16  — see MaterialFlags namespace

    // --- PBR scalar parameters (3 × float = 12 bytes) ---
    // Multipliers: applied to map sample when map present, used as flat
    // value when the map slot is kMaterialTexAbsent.
    float    baseColorFactor;       // 20  — [0, 1] albedo tint / brightness
    float    metallicFactor;        // 24  — [0, 1] metalness override/mult
    float    roughnessFactor;       // 28  — [0, 1] roughness override/mult
};
static_assert(sizeof(MaterialGpu) == 32,
              "MaterialGpu must be 32 bytes for clean std430 stride");
static_assert(offsetof(MaterialGpu, albedoTex)            ==  0, "albedoTex offset");
static_assert(offsetof(MaterialGpu, normalTex)            ==  4, "normalTex offset");
static_assert(offsetof(MaterialGpu, metallicRoughnessTex) ==  8, "metallicRoughnessTex offset");
static_assert(offsetof(MaterialGpu, emissiveTex)          == 12, "emissiveTex offset");
static_assert(offsetof(MaterialGpu, flags)                == 16, "flags offset");
static_assert(offsetof(MaterialGpu, baseColorFactor)      == 20, "baseColorFactor offset");
static_assert(offsetof(MaterialGpu, metallicFactor)       == 24, "metallicFactor offset");
static_assert(offsetof(MaterialGpu, roughnessFactor)      == 28, "roughnessFactor offset");

} // namespace RenderCore
