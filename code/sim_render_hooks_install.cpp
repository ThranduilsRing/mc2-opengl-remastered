// Render/GameOS side of the sim hook table (code/sim_render_hooks.h). Not part of mc2_sim: it names gos_* and gpu_cull::.
// Installed by static initialisation, before main, so every exe (game, headless runner) has the table filled.
#include "sim_render_hooks.h"
#include "gameos.hpp"
#include "../GameOS/gameos/gos_terrain_lod_chunk.h"
#include "../GameOS/gameos/gpu_cull_substrate.h"
#include "../GameOS/gameos/gos_smoke.h"
#include "../GameOS/gameos/utils/mc2log.h"
#include <cstring>

extern HGOSFONT3D gosFontHandle;   // code/controlgui.cpp
extern float gosFontScale;         // mclib/floathelp.h

namespace {

bool hookKeyPressed(int key) { return gos_GetKeyStatus(static_cast<gosEnum_KeyIndex>(key)) == KEY_PRESSED; }

void hookSetWorldToClip(const float* m16)
{
	glm::mat4 m;
	std::memcpy(&m, m16, sizeof(m));
	gos_SetWorldToClipGL(m);
}

void hookMeasureDebugText(mc2::czstring text, uint32_t* width, uint32_t* height)
{
	DWORD w = 0, h = 0;
	gos_TextSetAttributes(gosFontHandle, 0, gosFontScale, false, true, false, false);
	gos_TextStringLength(&w, &h, text);
	*width = static_cast<uint32_t>(w);
	*height = static_cast<uint32_t>(h);
}

const bool s_installed = []() {
	auto& h = mc2::sim::g_renderHooks;
	h.keyPressed = hookKeyPressed;
	h.getTerrainShadowSoftness = &gos_GetTerrainShadowSoftness;
	h.setTerrainShadowSoftness = &gos_SetTerrainShadowSoftness;
	h.setWorldToClip = hookSetWorldToClip;
	h.substrateFrameBegin = &gpu_cull::substrate_frameBegin;
	h.visualDampWanted = &gos_TerrainLodChunk_VisualDampWanted;
	h.updateVisualDampMovers = &gos_TerrainLodChunk_UpdateVisualDampMovers;
	h.measureDebugText = hookMeasureDebugText;
	h.smokeFixedTimestepEnabled = &SmokeMode::fixedTimestepEnabled;
	h.smokeFixedTimestepOnSimFrame = &SmokeMode::fixedTimestepOnSimFrame;
	h.smokeOnSimTick = &SmokeMode::onSimTick;
	// The sim worker's kick/join/extract hooks: installed here, in the same one static initialiser as the render table,
	// so the order is fixed (render table, then overlap table) and happens once per process.
	mc2::overlap::installHooks(mc2::sim::overlapHookTable());
	MC2_EVENT(Sim, "SIM_HOOKS", 1, "event=installed render_table=1 overlap_table=1");
	return true;
}();

}  // namespace
