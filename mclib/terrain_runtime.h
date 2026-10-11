//******************************************************************************
// terrain_runtime.h — TERRAIN-RUNTIME-API-1
//
// Compatibility seam for the Terrain 2.0 arc. PURE PASS-THROUGH over the legacy
// `Terrain* land` global today: every accessor forwards to an existing Terrain
// method and returns the identical value. No behavior change; nothing in the
// engine calls these yet (foundation slice).
//
// Recon / design: docs/recon/terrain-runtime-api-recon-1.md (+ -datamodel,
// -render-consumers, -pak-format, -landmines sub-docs).
//
// THE DIVERGENCE TRAP (why two heights):
//   gameplay height (pathing/LOS/grounding) and visual height (render surface)
//   are the SAME buffer today, but a future slice may diverge them. Each is a
//   distinct entry point so every call site is forced to pick. NEVER collapse
//   to a single height() accessor — silent default-equality is the bug we are
//   building this API to prevent.
//******************************************************************************
#ifndef TERRAIN_RUNTIME_H
#define TERRAIN_RUNTIME_H

namespace Stuff { class Vector3D; }

namespace TerrainRuntime
{
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
	float sampleGameplayHeight(const Stuff::Vector3D& worldPos);

	// Grounding chokepoint: the single seam every per-frame object/unit Z-onto-
	// terrain call routes through (TERRAIN-RUNTIME-CONSUMER-GROUNDING-1). Gate
	// MC2_TERRAIN_RUNTIME_GROUNDING (default-OFF). Byte-identical to legacy today
	// (== sampleGameplayHeight == land->getTerrainElevation); returns 0 if no
	// terrain. This is the one place a future gameplay/visual split decides how
	// grounding samples height. Use this, not raw land->getTerrainElevation, at
	// grounding sites.
	float groundElevation(const Stuff::Vector3D& worldPos);

	// Render/visual surface height. == gameplay height today; may diverge later.
	float sampleVisualHeight(const Stuff::Vector3D& worldPos);

	// Decal chokepoint: terrain decals (craters, impact rings) sit ON the rendered
	// surface, so they belong on VISUAL height — unlike grounding, which stays on
	// gameplay height (TERRAIN-RUNTIME-CONSUMER-DECALS-1). Gate
	// MC2_TERRAIN_RUNTIME_DECALS (default-OFF). Byte-identical today
	// (sampleVisualHeight == gameplay); the future split is exactly where decals
	// (visual) and units (gameplay) must diverge to avoid z-fight.
	float decalElevation(const Stuff::Vector3D& worldPos);

	// Flat water plane level. == land->getWaterElevation() (position ignored today).
	float sampleWaterLevel(const Stuff::Vector3D& worldPos);

	// Material / terrain class at world pos. == land->getTerrainType(pos).
	int sampleMaterialId(const Stuff::Vector3D& worldPos);

	// Gameplay water classification (deep/shallow/dry) at a position — the
	// chokepoint every gameplay water-threshold query routes through
	// (TERRAIN-RUNTIME-CONSUMER-WATER-1). == land->getWater(pos) today (single
	// impl, so no gate). The future gameplay/visual split keeps water-class on
	// gameplay height here, independent of the visual waterline mesh.
	long sampleWaterClass(const Stuff::Vector3D& worldPos);

	// Derived feature query in [0,1]. Live for FM_Cliff / FM_Shoreline; the
	// remaining masks return 0 until their data layer exists.
	float sampleFeatureMask(const Stuff::Vector3D& worldPos, FeatureMask kind);

	// Gated parity self-test driver (MC2_TERRAIN_RUNTIME_PARITY=1). The actual
	// grid walk lives in Terrain::update (in-scope to the inline cell helpers);
	// this returns true when the gate is set so callers can decide to run it.
	bool parityGateEnabled();
}

#endif // TERRAIN_RUNTIME_H
