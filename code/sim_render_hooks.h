// Sim-to-render hook table (sim kernel slice 2). Owned by mc2_sim; filled by the render/GameOS side at startup
// (code/sim_render_hooks_install.cpp). A null member is a no-op (headless runner, unit tests). The sim calls these
// where it used to call gos_* / gpu_cull:: directly, at the same points and in the same order.
#pragma once
#include <cstdint>
#include "../mclib/mc2_czstring.h"
#include "../mclib/sim_overlap.h"

namespace mc2::sim {

struct RenderHooks {
	void (*advanceTimeOfDay)() = nullptr; // one sim tick; live light publication runs on Main
	bool (*liveLighting)() = nullptr;    // live member writes cannot overlap rendering yet
	// Input read in the update path (dev hotkeys). `key` is a gosEnum_KeyIndex; true when its status is KEY_PRESSED.
	bool  (*keyPressed)(int key) = nullptr;
	float (*getTerrainShadowSoftness)() = nullptr;
	void  (*setTerrainShadowSoftness)(float v) = nullptr;
	// Publish the live camera world-to-clip matrix (16 floats, glm::mat4 column-major layout).
	void  (*setWorldToClip)(const float* m16) = nullptr;
	// GPU cull substrate per-frame begin.
	void  (*substrateFrameBegin)() = nullptr;
	// Terrain visual-damp mover stamps.
	bool  (*visualDampWanted)() = nullptr;
	void  (*updateVisualDampMovers)(const float* cellXY, int count, float radiusCells, float innerCells) = nullptr;
	// Debug-overlay text measure with the shared HUD font (sets the font attributes, then measures).
	void  (*measureDebugText)(mc2::czstring text, uint32_t* width, uint32_t* height) = nullptr;
	// Smoke/headless tick driver (SmokeMode). Null fixedTimestepEnabled means "not enabled".
	bool  (*smokeFixedTimestepEnabled)() = nullptr;
	void  (*smokeFixedTimestepOnSimFrame)(double scenarioTimeSeconds) = nullptr;
	void  (*smokeOnSimTick)(double scenarioTimeSeconds) = nullptr;
};

inline RenderHooks g_renderHooks{};

// The sim worker's kick/join/extract hooks (defined in mission_update.cpp, installed by sim_render_hooks_install.cpp).
mc2::overlap::Hooks overlapHookTable();

}  // namespace mc2::sim
