#pragma once
// VULKAN-CONTRACT-COVERAGE-1 — GL-free, OFFLINE per-resource Vulkan descriptor-class
// contract table + a coverage/kind-consistency validator.
//
// Converts the descriptor-inventory recon (VULKAN-DESCRIPTOR-INVENTORY-1.md, backed by
// VULKAN-DESCRIPTOR-LAYOUT-VALIDATOR-SPEC-1.md slice 1) into ENFORCEMENT. It records, for
// each LIVE-default-path RenderResourceId, the Vulkan descriptor CLASS it will hold after
// the RHI port (UniformBuffer / StorageBuffer / CombinedImageSampler / StorageImage /
// InputAttachment) plus a coarse change-frequency. A constexpr validator then proves:
//
//   I1 (coverage)          — every row declares a real descriptor class (!= None).
//   I2 (kind-consistency)  — the declared class AGREES with the registry's
//                            RenderResourceKind: a Buffer-kind resource must be a
//                            UBO/SSBO; a Texture*-kind resource must be a
//                            CombinedImageSampler / StorageImage / InputAttachment.
//
// ★ PROOF-ONLY. This header is SCHEMA + VALIDATION, NOT authoritative and NOT wired into
//   the renderer. It has ZERO runtime callers; it is included only by the offline doctest
//   suite (tests/unit/test_vulkan_contract.cpp). Byte-identical by construction — exactly
//   the discipline of scheduler_legal_reorder.h slice 1.
//
// Firewall: header-only, no GL includes, no game-side includes. Includes ONLY the
// registry enum header (RenderResourceKind / RenderResourceId) — a static, GL-free
// classifier. It deliberately does NOT read RenderResourceDesc (runtime-populated) nor
// bloat RenderPassContract with per-resource descriptor data (wrong granularity).

#include <cstdint>
#include "RenderResourceRegistry.h"    // RenderResourceId, RenderResourceKind

namespace RenderCore { namespace vulkan {

// ---------------------------------------------------------------------------
// Vulkan descriptor class an owned resource will hold post-port.
// (VertexInput is intentionally EXCLUDED here — VBO/IBO are bound via
// vkCmdBindVertexBuffers, not a descriptor set, and none of the enum-backed
// live resources below are pure vertex input.)
// ---------------------------------------------------------------------------
enum class VkDescriptorClass : uint8_t {
    None = 0,
    UniformBuffer,          // std140 UBO
    StorageBuffer,          // std430 SSBO
    CombinedImageSampler,   // sampled image (glActiveTexture + glUniform1i today)
    StorageImage,           // compute-written image (glBindImageTexture; none live today)
    InputAttachment,        // subpass-fused attachment read (none live today)
};

inline const char* vkDescriptorClassName(VkDescriptorClass c) {
    switch (c) {
        case VkDescriptorClass::None:                 return "None";
        case VkDescriptorClass::UniformBuffer:        return "UniformBuffer";
        case VkDescriptorClass::StorageBuffer:        return "StorageBuffer";
        case VkDescriptorClass::CombinedImageSampler: return "CombinedImageSampler";
        case VkDescriptorClass::StorageImage:         return "StorageImage";
        case VkDescriptorClass::InputAttachment:      return "InputAttachment";
    }
    return "?";
}

// Coarse change frequency (the Vulkan set-grouping axis). PerFrame = updated/written or
// rebound every frame; Persistent = mission/process-lifetime (rebound rarely). Cheap
// hint recorded from the inventory's "Freq" column; NOT asserted beyond being non-None.
enum class VkResourceFreq : uint8_t {
    None = 0,
    PerFrame,
    Persistent,
};

inline const char* vkResourceFreqName(VkResourceFreq f) {
    switch (f) {
        case VkResourceFreq::None:       return "None";
        case VkResourceFreq::PerFrame:   return "PerFrame";
        case VkResourceFreq::Persistent: return "Persistent";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// A single per-resource Vulkan-descriptor contract row. HAND-DECLARED, reviewed at
// change time (mirrors kExternalResources / staticResourceLifetime discipline).
// ---------------------------------------------------------------------------
struct VkResourceContract {
    RenderResourceId  id;
    RenderResourceKind kind;   // MIRRORED from the registry for the I2 cross-check.
    VkDescriptorClass cls;
    VkResourceFreq    freq;
    const char*       note;
};

// ---------------------------------------------------------------------------
// kVkResourceContracts[] — the LIVE-default-path resources the inventory enumerates that
// are also backed by a RenderResourceId enum value. Populated straight from
// VULKAN-DESCRIPTOR-INVENTORY-1.md Parts 1-2 (the owned buffers + the FBO-backed sampled
// images). Default-OFF substrate (cluster/lightgrid/compute-blur), WIP bakes with no stock
// data (TerrainVisualHeightSsbo, MechProfileMaterialGpuBuffer), temporal reflection RTs,
// and pure vertex/index buffers are SKIPPED for v1 (spec §7).
//
// Descriptor-class totals below: 1 UniformBuffer, 11 StorageBuffer, 4 CombinedImageSampler.
// (Inventory Part 1 names 2 core UBOs; only ViewUniformsUbo is enum-backed — CullUBO has
//  no RenderResourceId, so it is out of scope for this id-keyed table.)
// ---------------------------------------------------------------------------
static constexpr VkResourceContract kVkResourceContracts[] = {
    // --- Uniform buffers (Inventory Part 1: 2 core UBOs; CullUBO is not enum-backed) ---
    { RenderResourceId::ViewUniformsUbo, RenderResourceKind::Buffer,
      VkDescriptorClass::UniformBuffer, VkResourceFreq::PerFrame,
      "view-uniforms UBO 3, 3-slot ring -> UNIFORM_BUFFER_DYNAMIC (offset=slot*144)" },

    // --- Storage buffers (Inventory Part 1: owned SSBOs, live default path) ---
    { RenderResourceId::LightDataSsbo, RenderResourceKind::Buffer,
      VkDescriptorClass::StorageBuffer, VkResourceFreq::PerFrame,
      "SSBO 20; read by all lit shaders; global set-0 candidate (multi-binder w/ terrain SurfaceVB)" },
    { RenderResourceId::StaticPropMaterialGpuBuffer, RenderResourceKind::Buffer,
      VkDescriptorClass::StorageBuffer, VkResourceFreq::Persistent,
      "SSBO 5; static_prop.frag material table (Mission lifetime)" },
    { RenderResourceId::TerrainHeightSsbo, RenderResourceKind::Buffer,
      VkDescriptorClass::StorageBuffer, VkResourceFreq::Persistent,
      "SSBO 23; LOD-chunk height (Mission); AMD-TES transport" },
    { RenderResourceId::TerrainTypeSsbo, RenderResourceKind::Buffer,
      VkDescriptorClass::StorageBuffer, VkResourceFreq::Persistent,
      "SSBO 24; LOD-chunk per-vertex type (Mission)" },
    { RenderResourceId::TerrainCementSsbo, RenderResourceKind::Buffer,
      VkDescriptorClass::StorageBuffer, VkResourceFreq::Persistent,
      "SSBO 25; LOD-chunk packed cement word/tile (Mission)" },
    { RenderResourceId::TerrainRecipeBuffer, RenderResourceKind::Buffer,
      VkDescriptorClass::StorageBuffer, VkResourceFreq::Persistent,
      "SSBO 0/1; terrain-indirect recipe (Persistent, mission+dirty)" },
    { RenderResourceId::TerrainThinBuffer, RenderResourceKind::Buffer,
      VkDescriptorClass::StorageBuffer, VkResourceFreq::PerFrame,
      "SSBO 2 ranged; solid.comp -> gos_terrain.frag; ring-3 fenced -> STORAGE_BUFFER_DYNAMIC" },
    { RenderResourceId::TerrainLightVertexInputSsbo, RenderResourceKind::Buffer,
      VkDescriptorClass::StorageBuffer, VkResourceFreq::Persistent,
      "TL-compute SSBO 0; MC2_TERRAIN_LIGHTING_GPU default-ON (Mission)" },
    { RenderResourceId::TerrainLightInputSsbo, RenderResourceKind::Buffer,
      VkDescriptorClass::StorageBuffer, VkResourceFreq::Persistent,
      "TL-compute SSBO 1; per-light input (Mission)" },
    { RenderResourceId::TerrainLightComputeOutputSsbo, RenderResourceKind::Buffer,
      VkDescriptorClass::StorageBuffer, VkResourceFreq::PerFrame,
      "TL-compute SSBO 2; compute(w) -> Terrain/water(r); compute->VS/FS barrier" },
    { RenderResourceId::DynamicPropShadowSsbo, RenderResourceKind::Buffer,
      VkDescriptorClass::StorageBuffer, VkResourceFreq::PerFrame,
      "SSBO 0; dynamic prop shadow casters (MC2_SHADOW_DYNAMIC_PROP_CASTERS default-ON, per-frame reupload)" },

    // --- Combined image samplers (Inventory Part 2: FBO-backed images sampled downstream) ---
    { RenderResourceId::MainColor, RenderResourceKind::Texture2D,
      VkDescriptorClass::CombinedImageSampler, VkResourceFreq::PerFrame,
      "scene HDR color (sceneFBO_ attach0); sampled by PostProcess" },
    { RenderResourceId::MainDepth, RenderResourceKind::Texture2D,
      VkDescriptorClass::CombinedImageSampler, VkResourceFreq::PerFrame,
      "scene depth; sampled by VFX/Water/Veg/PostProcess" },
    { RenderResourceId::MainNormal, RenderResourceKind::Texture2D,
      VkDescriptorClass::CombinedImageSampler, VkResourceFreq::PerFrame,
      "GBuffer1 normal (attach1); sampled by SSAO/Shoreline/ScreenShadow/BoxDecals" },
    { RenderResourceId::ShadowDynamicMap, RenderResourceKind::Texture2D,
      VkDescriptorClass::CombinedImageSampler, VkResourceFreq::PerFrame,
      "dynamic shadow depth; sampled by StaticProp/Terrain/Mech/PostProcess (compare sampler)" },

    // --- POSTPROCESS-VK-IMAGE-OWNERSHIP-1: Layer-4 subgraph owned intermediate images ---
    // The subgraph owns a COLOR and a DEPTH intermediate. Only the DEPTH image is a
    // descriptor-bound resource:
    //   PostprocessSubgraphDepth — copied-in from GL sceneDepth, then SAMPLED (read-only) by
    //     both fog passes -> CombinedImageSampler. PerFrame (rebuilt every frame from GL depth).
    // PostprocessSubgraphColor is DELIBERATELY EXCLUDED from this descriptor table: it is a
    //   pure COLOR ATTACHMENT that is also a transfer src/dst (copy-in / blend / copy-out). It
    //   is bound as a render-target attachment (VkRenderingAttachmentInfo / framebuffer), NOT via
    //   a descriptor set, and it is never sampled. Forcing it into a descriptor class (Combined-
    //   ImageSampler / StorageImage / InputAttachment) would be WRONG — there is no descriptor
    //   binding for it. Its ownership is instead modeled by the enum id + the layout-transition
    //   chain (kVkImageTransitionChains in vulkan_layout_chain.h), which is the correct home for
    //   attachment/transfer-only images. (Mirrors how pure VBO/IBO vertex input is excluded.)
    { RenderResourceId::PostprocessSubgraphDepth, RenderResourceKind::Texture2D,
      VkDescriptorClass::CombinedImageSampler, VkResourceFreq::PerFrame,
      "subgraph owned depth (D32_SFLOAT); copy-in from GL sceneDepth, sampled by both fog passes" },
};

static constexpr int kVkResourceContractCount =
    sizeof(kVkResourceContracts) / sizeof(kVkResourceContracts[0]);

// ---------------------------------------------------------------------------
// Kind-consistency predicate (I2). A descriptor class is consistent with a resource
// kind iff:
//   Buffer      -> UniformBuffer | StorageBuffer
//   Texture*    -> CombinedImageSampler | StorageImage | InputAttachment
// None on either side is inconsistent (I1 also catches a None class).
// ---------------------------------------------------------------------------
constexpr bool vkClassIsBufferClass(VkDescriptorClass c) {
    return c == VkDescriptorClass::UniformBuffer
        || c == VkDescriptorClass::StorageBuffer;
}
constexpr bool vkClassIsImageClass(VkDescriptorClass c) {
    return c == VkDescriptorClass::CombinedImageSampler
        || c == VkDescriptorClass::StorageImage
        || c == VkDescriptorClass::InputAttachment;
}

constexpr bool vkKindConsistent(RenderResourceKind kind, VkDescriptorClass cls) {
    switch (kind) {
        case RenderResourceKind::Buffer:
            return vkClassIsBufferClass(cls);
        case RenderResourceKind::Texture2D:
        case RenderResourceKind::Texture2DArray:
        case RenderResourceKind::TextureCube:
            return vkClassIsImageClass(cls);
        case RenderResourceKind::Unknown:
        default:
            return false;   // an Unknown-kind row is a declaration bug.
    }
}

// ---------------------------------------------------------------------------
// Coverage + consistency over the whole table (I1 + I2). Returns true iff EVERY row has
// a declared class (!= None), a declared frequency (!= None), a known kind (!= Unknown),
// a real id (!= Unknown), and a class consistent with its kind. On failure *offending (if
// non-null) receives the id of the first bad row. constexpr so a static_assert can prove
// the shipped table at compile time.
// ---------------------------------------------------------------------------
constexpr bool vkContractRowOk(const VkResourceContract& r) {
    return r.id   != RenderResourceId::Unknown
        // ENUM-ID-GUARD-EXPAND-1: Count is the past-the-end sentinel, never a real
        // resource; a row keyed on it would index out of bounds in dense-index consumers.
        && r.id   != RenderResourceId::Count
        && static_cast<int>(r.id) < static_cast<int>(RenderResourceId::Count)
        && r.kind != RenderResourceKind::Unknown
        && r.cls  != VkDescriptorClass::None
        && r.freq != VkResourceFreq::None
        && vkKindConsistent(r.kind, r.cls);
}

constexpr bool vkContractsCoverageOk(RenderResourceId* offending = nullptr) {
    for (int i = 0; i < kVkResourceContractCount; ++i) {
        if (!vkContractRowOk(kVkResourceContracts[i])) {
            if (offending) *offending = kVkResourceContracts[i].id;
            return false;
        }
    }
    return true;
}

// No duplicate ids in the table (each resource declared exactly once).
constexpr bool vkContractsNoDuplicateIds() {
    for (int i = 0; i < kVkResourceContractCount; ++i)
        for (int j = i + 1; j < kVkResourceContractCount; ++j)
            if (kVkResourceContracts[i].id == kVkResourceContracts[j].id)
                return false;
    return true;
}

// Count rows of a given descriptor class (for the inventory-totals assertion).
constexpr int vkCountByClass(VkDescriptorClass c) {
    int n = 0;
    for (int i = 0; i < kVkResourceContractCount; ++i)
        if (kVkResourceContracts[i].cls == c) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Compile-time guards (fire at build time, not just test run) — mirrors
// RenderPassContract.h's static_assert(kRenderPassContractCount == ...).
// ---------------------------------------------------------------------------
static_assert(kVkResourceContractCount == 17,
              "kVkResourceContracts row count changed; update the slice-1 total or re-review "
              "(POSTPROCESS-VK-IMAGE-OWNERSHIP-1 added PostprocessSubgraphDepth; the sibling "
              "PostprocessSubgraphColor is intentionally NOT a descriptor row — see note above)");
static_assert(vkContractsCoverageOk(),
              "I1/I2: a vulkan_contract row has no class, no freq, or a class inconsistent with its kind");
static_assert(vkContractsNoDuplicateIds(),
              "vulkan_contract: duplicate RenderResourceId in the table");
static_assert(vkCountByClass(VkDescriptorClass::UniformBuffer) == 1,
              "expected 1 UniformBuffer row (ViewUniformsUbo; CullUBO not enum-backed)");
static_assert(vkCountByClass(VkDescriptorClass::StorageBuffer) == 11,
              "expected 11 StorageBuffer rows (inventory Part-1 live owned SSBOs, enum-backed)");
static_assert(vkCountByClass(VkDescriptorClass::CombinedImageSampler) == 5,
              "expected 5 CombinedImageSampler rows (inventory Part-2 sampled FBO images + "
              "the subgraph-owned PostprocessSubgraphDepth, enum-backed)");

}} // namespace RenderCore::vulkan
