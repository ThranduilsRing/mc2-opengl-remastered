// Shared surface of the GraphicsOptionsWindow translation units (GraphicsOptionsWindow*.cpp).
//
// GraphicsOptionsWindow.cpp keeps the open flag and draw(); each tuning section lives in one of the section files.
#ifndef GRAPHICSOPTIONSWINDOW_INTERNAL_H
#define GRAPHICSOPTIONSWINDOW_INTERNAL_H

#include "GraphicsOptionsWindow.h"
#include "imgui.h"
#include "../mclib/particles/batcher.h"  // Batcher::is_enabled() (MC2_GPU_PARTICLES reader)

#include "../GameOS/gameos/gos_postprocess.h"
#include "../GameOS/gameos/view_registry.h"
#include "../RenderCore/RenderResourceRegistry.h"
#include "gameos.hpp"
#include "../GameOS/gameos/gos_static_prop_killswitch.h"
#include "../GameOS/gameos/gos_mech_killswitch.h"
#include "../GameOS/gameos/ibl_sh_runtime.h"     // g_iblShStrength, g_pbrV1Strength, g_pbrV1RoughnessOverride*
#include "../mclib/projectz_overlay.h"
#include "draw_packet_emitter.h"       // DrawPacketsDebugSnapshot g_dpSnapshot
#include "StaticPropTypeDesc.h"        // RenderCore::StaticPropTypeDesc
// Forward-declare the two batcher accessors we need rather than pulling in
// gos_static_prop_batcher.h, which transitively requires Stuff.hpp / tgl.h.
bool     batcher_getStaticPropTypeDesc(uint32_t typeId, RenderCore::StaticPropTypeDesc* out);
uint32_t batcher_getStaticPropTypeDescCount();
// Terrain PBR parameter accessors (defined in gameos_graphics.cpp).
void  gos_SetTerrainMatNormalBoost(float rock, float grass, float dirt, float concrete);
void  gos_GetTerrainMatNormalBoost(float* rock, float* grass, float* dirt, float* concrete);
void  gos_SetTerrainMatTiling(float rock, float grass, float dirt, float concrete, float snow);
void  gos_GetTerrainMatTiling(float* rock, float* grass, float* dirt, float* concrete, float* snow);
void  gos_SetTerrainTintStrengthScale(float s);
float gos_GetTerrainTintStrengthScale();
void  gos_SetTerrainTintRock(float r, float g, float b);
void  gos_GetTerrainTintRock(float* r, float* g, float* b);
void  gos_SetTerrainTintGrass(float r, float g, float b);
void  gos_GetTerrainTintGrass(float* r, float* g, float* b);
void  gos_SetTerrainTintDirt(float r, float g, float b);
void  gos_GetTerrainTintDirt(float* r, float* g, float* b);
void  gos_SetTerrainNormalsFromHeightStrength(float s);
float gos_GetTerrainNormalsFromHeightStrength();
void  gos_SetTerrainCliffShadowFloor(float f);
float gos_GetTerrainCliffShadowFloor();
// MDI water FS accessors (defined in
// gameos_graphics.cpp). Debug-mode selector + runtime material tunables
// (defaults match the former gos_terrain_water_mdi.frag consts exactly).
int   gos_GetWaterFsDebugMode();
void  gos_SetWaterFsDebugMode(int m);
float gos_GetWaterAbsorptionDensity();
void  gos_SetWaterAbsorptionDensity(float v);
float gos_GetWaterMaxAlpha();
void  gos_SetWaterMaxAlpha(float v);
float gos_GetWaterRippleGain();
void  gos_SetWaterRippleGain(float v);
float gos_GetWaterGlintGain();
void  gos_SetWaterGlintGain(float v);
void  gos_GetWaterDeepColor(float* rgb);
void  gos_SetWaterDeepColor(float r, float g, float b);
void  gos_GetWaterShallowColor(float* rgb);
void  gos_SetWaterShallowColor(float r, float g, float b);
float gos_GetWaterSkyTintStrength();
void  gos_SetWaterSkyTintStrength(float v);
void  gos_GetWaterSkyTintColor(float* rgb);
void  gos_SetWaterSkyTintColor(float r, float g, float b);
// gated camera-dependent SH-L2 sky reflection.
float gos_GetWaterReflStrength();
int   gos_GetWaterReflectionQuality();
void  gos_SetWaterReflectionQuality(int quality);
void  gos_SetWaterReflStrength(float v);
int   gos_GetWaterReflectionGate();
float gos_GetWaterRtStrength();
void  gos_SetWaterRtStrength(float v);
int   gos_GetWaterReflectionRtGate();
// colormap RGB classifier thresholds (gameos_graphics.cpp).
void gos_SetTerrainClassGrass(float gMinusRLo, float gMinusRHi, float gBrightLo, float gBrightHi);
void gos_GetTerrainClassGrass(float* gMinusRLo, float* gMinusRHi, float* gBrightLo, float* gBrightHi);
void gos_SetTerrainClassDirt(float rMinusGLo, float rMinusGHi, float rBrightLo, float rBrightHi);
void gos_GetTerrainClassDirt(float* rMinusGLo, float* rMinusGHi, float* rBrightLo, float* rBrightHi);
// mech ambient + specular + PBR roughness (gos_mech_batcher.cpp).
extern "C" int   batcher_getMechSpecularEnabled(void);
extern "C" void  batcher_setMechSpecularEnabled(int on);
extern "C" float batcher_getMechSpecularStrength(void);
extern "C" void  batcher_setMechSpecularStrength(float s);
extern "C" float batcher_getMechMetalRoughness(void);
extern "C" void  batcher_setMechMetalRoughness(float r);
// imported-mech albedo knobs (imported only).
extern "C" float batcher_getImportedMechGamma(void);
extern "C" void  batcher_setImportedMechGamma(float g);
extern "C" float batcher_getImportedMechAlbedoScale(void);
extern "C" void  batcher_setImportedMechAlbedoScale(float s);
extern "C" float batcher_getImportedMechAoStrength(void);
extern "C" void  batcher_setImportedMechAoStrength(float s);
extern "C" float batcher_getImportedMechNormalStrength(void);
extern "C" void  batcher_setImportedMechNormalStrength(float s);
extern "C" float batcher_getMechGlassRoughness(void);
extern "C" void  batcher_setMechGlassRoughness(float r);
// GPU particle debug-mode + intensity scales (defined in
// GameOS/gameos/gos_particle_bridge.cpp). All scales default 1.0 = no-op.
extern "C" int   gos_vfx_getDebugMode(void);
extern "C" void  gos_vfx_setDebugMode(int mode);
extern "C" float gos_vfx_getBrightness(void);
extern "C" float gos_vfx_getAdditiveBrightness(void);
extern "C" float gos_vfx_getAlphaScale(void);
extern "C" void  gos_vfx_setBrightness(float v);
extern "C" void  gos_vfx_setAdditiveBrightness(float v);
extern "C" void  gos_vfx_setAlphaScale(float v);
extern "C" int   gos_vfx_getSoftEnabled(void);
extern "C" void  gos_vfx_setSoftEnabled(int e);
extern "C" float gos_vfx_getSoftDistance(void);
extern "C" void  gos_vfx_setSoftDistance(float v);
extern "C" int   gos_vfx_getLitEnabled(void);
extern "C" void  gos_vfx_setLitEnabled(int e);
extern "C" float gos_vfx_getLitStrength(void);
extern "C" void  gos_vfx_setLitStrength(float v);
// profile status accessors (defined in visual_tuning_profile.cpp)
void        visualTuning_applyProfile(const char* missionName);
bool        visualTuning_saveCurrentToMission();
const char* visualTuning_getProfilePath();
const char* visualTuning_getActiveMission();
bool        visualTuning_hasProfileFile();
int         visualTuning_getAppliedKeyCount();

#include <cstdio>
#include <cmath>
#include <cstdlib>   // getenv


namespace GraphicsOptionsWindow {
void drawTerrainSection();
void drawTerrainTuningSection();
void drawStaticPropTuningSection();
void drawWaterSection();
void drawDrawPacketsSection();
void drawRenderPassesSection();
void drawGBufferPreview();
void drawEnvGatesSection();
void drawMechSection();
void drawVfxTuningSection();
} // namespace GraphicsOptionsWindow

#endif // GRAPHICSOPTIONSWINDOW_INTERNAL_H
