//******************************************************************************
// terrain_runtime.h — TERRAIN-RUNTIME-API-1
//
// Terrain query seam: gameplay reads the legacy sim surface; rendering reads
// the accepted 4x bake, with a sim fallback for maps/mods without one.
//
// Recon / design: docs/recon/terrain-runtime-api-recon-1.md (+ -datamodel,
// -render-consumers, -pak-format, -landmines sub-docs).
//
// THE DIVERGENCE TRAP (why two heights):
//   gameplay height (pathing/LOS/grounding) and visual height (render surface)
//   are distinct entry points so every call site must pick its surface. NEVER
//   collapse them to a single height() accessor: visual placement must not
//   change gameplay grounding, movement, passability or LOS.
//******************************************************************************
#ifndef TERRAIN_RUNTIME_H
#define TERRAIN_RUNTIME_H

#include "math/glm_config.h"

namespace TerrainRuntime
{
	// Bumped by every terrain vertex height write; per-object ground-height caches compare against it.
	inline uint32_t g_heightEpoch = 0;

	enum class FeatureMask
	{
		FM_Cliff = 0,      // slope-derived (live approximation)
		FM_Shoreline,      // elevation-vs-water band (live approximation)
		FM_Cement,         // STUB(0) — colormap-baked today, not a runtime feature (landmine #2)
		FM_ForestSuppress, // STUB(0) — future vegetation-suppress mask
		FM_Count
	};
	using enum FeatureMask;

	// Gameplay-authoritative surface height. == land->getTerrainElevation(pos).
	float sampleGameplayHeight(const glm::vec3& worldPos);

	// Grounding chokepoint: the single seam every per-frame object/unit Z-onto-
	// terrain call routes through (TERRAIN-RUNTIME-CONSUMER-GROUNDING-1). Gate
	// MC2_TERRAIN_RUNTIME_GROUNDING (default-OFF). Byte-identical to legacy today
	// (== sampleGameplayHeight == land->getTerrainElevation); returns 0 if no
	// terrain. This is the one place a future gameplay/visual split decides how
	// grounding samples height. Use this, not raw land->getTerrainElevation, at
	// grounding sites.
	float groundElevation(const glm::vec3& worldPos);

	// UNIT-VISUAL-SNAP-1 (render only): visual-bake height minus gameplay height at worldPos; 0 when no
	// bake is loaded or outside the map. Mech/vehicle MODEL draw heights add this; sim never reads it.
	float visualHeightDelta(const glm::vec3& worldPos);
	void setCpuVisualHeight(const float* vh, int V);   // nullptr clears (mission start / bake rejected)

	// Bilinear accepted 4x bake, falling back to gameplay outside it or without a bake.
	float sampleVisualHeight(const glm::vec3& worldPos);
	// Filtered TL-BR visual ground shared with terrain LOD0 and vegetation placement.
	float sampleVisibleGroundHeight(const glm::vec3& worldPos);
	bool visibleGroundSupports(const glm::vec3& worldPos, float radius);

	// Footprints/contact blobs follow the same visual surface as the terrain.
	float decalElevation(const glm::vec3& worldPos);
	// Minimum eye Z covering the eye and near-plane footprint, including ridges
	// between corners. No bake preserves the legacy eye-only sim/water clamp.
	float cameraClearanceHeight(const glm::vec3& eye, const glm::vec3 (&nearOffsets)[4], float minimum);

	// Flat water plane level. == land->getWaterElevation() (position ignored today).
	float sampleWaterLevel(const glm::vec3& worldPos);

	// Material / terrain class at world pos. == land->getTerrainType(pos).
	int sampleMaterialId(const glm::vec3& worldPos);

	// Gameplay water classification (deep/shallow/dry) at a position — the
	// chokepoint every gameplay water-threshold query routes through
	// (TERRAIN-RUNTIME-CONSUMER-WATER-1). == land->getWater(pos) today (single
	// impl, so no gate). The future gameplay/visual split keeps water-class on
	// gameplay height here, independent of the visual waterline mesh.
	long sampleWaterClass(const glm::vec3& worldPos);

	// Derived feature query in [0,1]. Live for FM_Cliff / FM_Shoreline; the
	// remaining masks return 0 until their data layer exists.
	float sampleFeatureMask(const glm::vec3& worldPos, FeatureMask kind);

	// Gated parity self-test driver (MC2_TERRAIN_RUNTIME_PARITY=1). The actual
	// grid walk lives in Terrain::update (in-scope to the inline cell helpers);
	// this returns true when the gate is set so callers can decide to run it.
	bool parityGateEnabled();
}

#endif // TERRAIN_RUNTIME_H
