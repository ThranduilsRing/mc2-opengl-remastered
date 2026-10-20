// vulkan_legacy_shape_island.h -- VK-PARITY-F1: the VK owner of the LEGACY MLR
// solid-object vertex loop in mc2::textures::ready()::renderLists (ground vehicles and
// every other TG_Shape drawn through the CPU-transformed gos_VERTEX path).
// =============================================================================
// WHY THIS EXISTS. Under the VK umbrella (MC2_RENDER_BACKEND=vulkan +
// MC2_VK_SHARED_GBUFFER=1) the native VK composite is the SOLE present producer
// and every scene layer must be VK-rendered into the shared GBuffer (see
// vk_shared_gbuffer_gate.h: "no per-layer GL fallback"). Ground vehicles do NOT go
// through the GPU static-prop / GPU mech batchers: TG_Shape::Render CPU-transforms
// and CPU-lights their triangles into gos_VERTEX arrays (mc2::textures::ready()
// masterVertexNodes, one array per texture) which renderLists flushes with
// gos_RenderIndexedArray(gos_VERTEX*) -> gosRenderer::drawIndexedTris -> the basic
// gos_tex_vertex material. That draw had NO VK owner, so vehicles were rendered
// into the discarded GL backbuffer and vanished under VK (measured: GL vs VK
// mc2_10 capture with spawned vehicles, 2026-10-08).
//
// WHAT IT DOES. renderLists, instead of issuing the GL draw, submits one record
// per non-terrain vertex node of each legacy loop (solid, alpha, blob-shadow, additive
// effects/spotlight-cone; pointer to the node's gos_VERTEX array, count, texture node,
// pass). run() copies the vertices into a per-parity host-visible VK vertex
// buffer and co-writes them IN-PLACE into the shared scene GBuffer (LOAD MRT0 color +
// MRT1 normal + shared D32S8 depth, GEQUAL reverse-Z, no composite) with a VK port of
// shaders/gos_tex_vertex.{vert,frag} (pre-transformed screen vertices, vertex color x
// texture, vertex-fog mix). Same pre-transformed-vertex math the GL path uses.
//
// GATING: no new env var. The island is armed exactly when the VK mech island is
// (MC2_VK_MECH_TIER1 + MC2_VK_SHARED_SCENE_DEPTH, both umbrella members): it is the
// "other half" of object content. Gate OFF => enabled()==0 => renderLists issues the
// GL draw as before (byte-identical).
// =============================================================================

#ifndef MC2_VULKAN_LEGACY_SHAPE_ISLAND_H
#define MC2_VULKAN_LEGACY_SHAPE_ISLAND_H

#ifdef MC2_VULKAN_ISLAND

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// One legacy solid vertex node (POD; mclib/txmmgr.cpp fills it). The vertex array
// (gos_VERTEX, 32B each, triangle list with identity indices) must stay valid until
// run() returns -- renderLists keeps it alive (it only rewinds the node cursor).
// Which renderLists loop the node came from (selects the blend / depth state):
//   SOLID  : opaque, depth write, GEQUAL            (the vehicle / object solid loop)
//   ALPHA  : AlphaInvAlpha, GEQUAL, no depth write  (non-terrain alpha / alpha-test nodes)
//   SHADOW : AlphaInvAlpha, GREATER, no depth write (legacy CPU blob shadows)
//   ADD    : OneOne additive, GEQUAL, no write, fog off (MC2_ISEFFECTS / MC2_ISSPOTLGT cones)
enum Mc2LegacyMlrPass { MC2_LEGACY_PASS_SOLID = 0, MC2_LEGACY_PASS_ALPHA = 1,
                        MC2_LEGACY_PASS_SHADOW = 2, MC2_LEGACY_PASS_ADD = 3, MC2_LEGACY_PASS_COUNT = 4 };

struct Mc2LegacyMlrDraw {
    const void* vertices;      // gos_VERTEX[vertexCount]
    uint32_t    vertexCount;   // multiple of 3
    uint32_t    texNode;       // mc2::textures::ready() node index (stable cache key)
    uint32_t    gosTexHandle;  // LIVE gos texture handle this frame (0 / 0xffffffff = untextured)
    uint32_t    pass;          // Mc2LegacyMlrPass
    uint32_t    alphaTest;     // 1 = discard texel alpha < 0.5 (MC2_ALPHATEST nodes)
};

// Per-frame inputs that live in mclib / the GL renderer.
struct Mc2LegacyMlrFrame {
    float fogColor[4];         // gosRenderer fog_color_ (all zero = no fog)
};

// Native menu previews capture the existing CPU MLR vertices, preserving paint and camera transforms.
int mc2_vk_preview_active();
void mc2_vk_preview_begin(int width, int height);
void mc2_vk_preview_capture(const Mc2LegacyMlrDraw* draw);
void mc2_vk_preview_end();
uint64_t mc2_vk_preview_texture();

// True when the island owns the legacy solid loop this session (renderLists then
// skips the GL draw and calls submit()/run()).
int mc2_vk_legacy_shape_island_enabled();

// Queue one draw for this frame's run().
void mc2_vk_legacy_shape_submit(const Mc2LegacyMlrDraw* d);

// Per-frame entry. Draws and clears the queued list. Returns 1 if the VK pass
// co-wrote this frame, 0 otherwise (list is cleared either way).
int mc2_vk_legacy_shape_island_run(int w, int h, const Mc2LegacyMlrFrame* frame);

// Called on mission unload (MC_TextureManager::flush): texture node indices are
// reused by the next mission, so the copy-once texture cache must be dropped.
void mc2_vk_legacy_shape_island_reset();

// Teardown (called from gameosmain.cpp before the shared device is destroyed).
void mc2_vk_legacy_shape_island_destroy();

#ifdef __cplusplus
}
#endif

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VULKAN_LEGACY_SHAPE_ISLAND_H
