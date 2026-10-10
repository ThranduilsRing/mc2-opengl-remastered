// Shared surface of the EditorInspector translation units: the include bundle, the C-linkage hooks and the panel
// state the Object Inspector sections read.
#ifndef GUIRUNTIME_EDITORINSPECTOR_INTERNAL_H
#define GUIRUNTIME_EDITORINSPECTOR_INTERNAL_H

#include "EditorInspector.h"
#include "imgui.h"
#include <cstdio>    // snprintf, fopen, fprintf, fclose
#include <cstdlib>

// HZB camera-view dump: thin C helper defined in code/gamecam.cpp so we don't
// have to pull the full camera.h -> terrain.h chain into GuiRuntime.
extern "C" int mc2_hzb_dump_camera_view(void);
// Bookmark-schema camera save (BOOKMARK-SAVE-BUTTON-1): same thin-C pattern.
extern "C" void mc2_save_camera_bookmark(char* outPath, int outPathLen);
#include "../GameOS/gameos/debug_renderer.h"  // IMG-INSPECT-3
#include "../GameOS/gameos/gos_frame_pass_stats.h"  // FRAME-INSPECTOR-1
#include "../GameOS/gameos/gos_render_pass_timer.h"  // FRAME-INSPECTOR-1 (ms col)
#include "../GameOS/gameos/ibl_sh_runtime.h"   // V-IBL-STATIC-1: g_iblShStrength
#include "draw_packet_emitter.h"              // g_dpSelectedRecipeIndex
#include "../RenderCore/RendererFeatureRegistry.h"
#include "../RenderCore/RenderPassContract.h"  // RENDERPASS-CONTRACT-2.5 (descriptive table)
#include "RenderDebugView.h"                   // DEBUG-VIEW-REGISTRY-1
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

// Terrain tunable C-API used to be forward-declared here for an inline
// Terrain Pass tunables panel; those controls were moved to
// GuiRuntime/GraphicsOptionsWindow.cpp (the existing terrain tuning home),
// so the forward decls and the inline panel are gone. EditorInspector's
// Terrain Pass section is now read-only diagnostic only.

// V-MATERIAL-STATIC-0: forward-declared inventory contract — duplicating the
// struct keeps gui_runtime independent of gos_static_prop_batcher.h, which
// transitively pulls Stuff/Stuff.hpp (not visible in this TU). The single
// source of truth is GameOS/gameos/gos_static_prop_batcher.h; layout must
// match exactly. Sized-fields verified against that header.
struct StaticPropMaterialInventoryEntry {
    uint32_t materialIdx;
    uint32_t albedoTexLayer;
    uint32_t alphaGroup;
    uint32_t flags;
    uint32_t nodeIdx;
    uint32_t textureWidth;
    uint32_t textureHeight;
    uint32_t usageCount;
    char     textureName[64];
    bool     placeholder;
    float    metallicFactor;   // V-MATERIAL-PBR-1
    float    roughnessFactor;  // V-MATERIAL-PBR-1
};
uint32_t batcher_getStaticPropMaterialInventoryCount();
bool     batcher_getStaticPropMaterialInventoryEntry(
             uint32_t idx, StaticPropMaterialInventoryEntry* out);
// WATER-DEBUG-VIEWS-1: MDI water FS debug mode accessor (defined at global
// scope in GameOS/gameos/gameos_graphics.cpp). Declared here at file scope so
// the call inside namespace EditorInspector resolves to the global symbol
// (a block-scope extern would bind to EditorInspector::, causing LNK2019).
int gos_GetWaterFsDebugMode();

// DEBUG-VIEW-REGISTRY-1: runtime debug-mode getter/setter + view<->shaderMode helpers
// (gos_static_prop_batcher.cpp).
void batcher_setDebugMaterialMode(int shaderMode);
int  batcher_getDebugMaterialMode();
int             StaticPropViewToShaderMode(RenderDebugView view);
RenderDebugView StaticPropShaderModeToView(int shaderMode);

// MECH-SPINE-1: read-only accessors for mech pass-level state. Defined in
// gos_mech_batcher.cpp; declared here so the inspector can reference them
// without including engine-private mech batcher headers.
extern "C" uint32_t gos_getMechProgramId();
extern "C" uint32_t gos_getMechShadowProgramId();
extern "C" const char* gos_getMechTextureNameByNodeIdx(uint32_t nodeIdx);

// MECH-DEBUG-VIEWS-1: debug-mode getter/setter + view<->shaderMode helpers
// (gos_mech_batcher.cpp). Same pattern as StaticProp decls above.
// Not declared extern "C": pure C++ functions over RenderDebugView enum.
int             MechViewToFragDebugMode(RenderDebugView view);
RenderDebugView MechFragDebugModeToView(int shaderMode);
extern "C" void batcher_setMechDebugMode(int shaderMode);
extern "C" int  batcher_getMechDebugMode();

// MECH-NORMALS-FIX-1: live normal recompute API (gos_mech_batcher.cpp).
// Declared extern "C" to avoid including gos_mech_batcher.h which
// transitively pulls Stuff/Stuff.hpp (not visible in GuiRuntime TU).
extern "C" void  batcher_setMechNormalsMode(int mode);
extern "C" int   batcher_getMechNormalsMode();
extern "C" void  batcher_setMechNormalsSmoothDeg(float deg);
extern "C" float batcher_getMechNormalsSmoothDeg();
extern "C" void  batcher_rebuildMechNormals();
// MECH-AMBIENT-1: gated hemisphere ambient fill (per-flush uniform, no rebuild).
extern "C" void  batcher_setMechAmbientEnabled(int on);
extern "C" int   batcher_getMechAmbientEnabled();
extern "C" void  batcher_setMechAmbientStrength(float s);
extern "C" float batcher_getMechAmbientStrength();
// MECH-SPECULAR-V1: gated Blinn specular sheen (per-flush uniforms, no rebuild).
// Effective only when MC2_MECH_VIEWUNIFORMS=1 (shader has camera position data).
extern "C" void  batcher_setMechSpecularEnabled(int on);
extern "C" int   batcher_getMechSpecularEnabled();
extern "C" void  batcher_setMechSpecularStrength(float s);
extern "C" float batcher_getMechSpecularStrength();
extern "C" void  batcher_setMechMetalRoughness(float r);
extern "C" float batcher_getMechMetalRoughness();
extern "C" void  batcher_setMechGlassRoughness(float r);
extern "C" float batcher_getMechGlassRoughness();
extern "C" void  batcher_setMechGlassLumaThresh(float t);
extern "C" float batcher_getMechGlassLumaThresh();
extern "C" void  batcher_setMechGlassMaxChanThresh(float t);
extern "C" float batcher_getMechGlassMaxChanThresh();
extern "C" void  batcher_setMechSpecDebugMask(int on);
extern "C" int   batcher_getMechSpecDebugMask();

namespace EditorInspector {
extern InspectorSelection     s_selection;
extern StaticPropInspectorData s_staticPropData;
extern MechInspectorData       s_mechData;
extern TerrainInspectorData    s_terrainData;
extern TerrainPassSnapshot     s_terrainPass;
extern ShadowPassSnapshot      s_shadowPass;
extern VfxPassSnapshot         s_vfxPass;
extern bool s_open;
extern bool s_featuresOpen;
const char* pickKindName(InspectorPickKind k);
}  // namespace EditorInspector

#endif  // GUIRUNTIME_EDITORINSPECTOR_INTERNAL_H
