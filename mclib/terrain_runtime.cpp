//******************************************************************************
// terrain_runtime.cpp — TERRAIN-RUNTIME-API-1
//
// Pure pass-through implementation over the legacy `Terrain* land` global.
// See terrain_runtime.h and docs/recon/terrain-runtime-api-recon-1.md.
//******************************************************************************
#include <algorithm>
#include <vector>
#include "terrain.h"
#include "terrain_runtime.h"
#include "Gates/gate.h"

#include <cstdlib>
#include <cmath>
#include <cstring>

// terrain.h pulls in Stuff::Vector3D (used throughout its method signatures).

// Defined in mclib/terrain.cpp; the single live Terrain instance.
extern Terrain* land;

namespace TerrainRuntime
{

float sampleGameplayHeight(const glm::vec3& worldPos)
{
	// Authoritative gameplay surface. Identical to every grounding/pathing call.
	return land ? land->getTerrainElevation(worldPos) : 0.0f;
}

float groundElevation(const glm::vec3& worldPos)
{
	if (!land)
		return 0.0f;
	// Gate exists for A/B + as the future gameplay/visual split flip-point.
	// Both branches are byte-identical today.
	static const bool s_gate = mc2::gate::get<"MC2_TERRAIN_RUNTIME_GROUNDING">();   // default OFF
	return s_gate ? sampleGameplayHeight(worldPos)
	              : land->getTerrainElevation(worldPos);
}

// UNIT-VISUAL-SNAP-1: CPU copy of the ACCEPTED 4x visual bake (empty = none). Render-only: the sim
// never reads this. Set by Terrain::lodVisualHeight when the bake passes the signature/mean guards.
static std::vector<float> s_cpuVisualHeight;
static int s_cpuVisualV = 0;

void setCpuVisualHeight(const float* vh, int V)
{
	if (vh && V > 1) { s_cpuVisualHeight.assign(vh, vh + static_cast<size_t>(V) * static_cast<size_t>(V)); s_cpuVisualV = V; }
	else { s_cpuVisualHeight.clear(); s_cpuVisualV = 0; }
}

float visualHeightDelta(const glm::vec3& worldPos)
{
	if (!land || s_cpuVisualV < 2) return 0.0f;
	const float inv = 4.0f / Terrain::worldUnitsPerVertex;
	float fx = (worldPos.x - Terrain::mapTopLeft3d.x) * inv;
	float fy = (Terrain::mapTopLeft3d.y - worldPos.y) * inv;
	const float hi = static_cast<float>(s_cpuVisualV - 1);
	if (fx < 0.0f || fy < 0.0f || fx > hi || fy > hi) return 0.0f;
	int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
	if (x0 >= s_cpuVisualV - 1) x0 = s_cpuVisualV - 2;
	if (y0 >= s_cpuVisualV - 1) y0 = s_cpuVisualV - 2;
	const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
	const float* p = s_cpuVisualHeight.data() + static_cast<size_t>(y0) * s_cpuVisualV + x0;
	const float v = (p[0] * (1 - tx) + p[1] * tx) * (1 - ty) + (p[s_cpuVisualV] * (1 - tx) + p[s_cpuVisualV + 1] * tx) * ty;
	return v - land->getTerrainElevation(worldPos);
}

float sampleVisualHeight(const glm::vec3& worldPos)
{
	// Render surface. SAME buffer as gameplay today; a future visual-height
	// slice (resample/displacement) will diverge this without touching the
	// gameplay accessor above. Kept distinct on purpose.
	return land ? land->getTerrainElevation(worldPos) : 0.0f;
}

float decalElevation(const glm::vec3& worldPos)
{
	if (!land)
		return 0.0f;
	// Gate = the future flip-point where decals follow the VISUAL surface while
	// units stay on gameplay height. Both branches byte-identical today.
	static const bool s_gate = mc2::gate::get<"MC2_TERRAIN_RUNTIME_DECALS">();   // default OFF
	return s_gate ? sampleVisualHeight(worldPos)
	              : land->getTerrainElevation(worldPos);
}

float sampleWaterLevel(const glm::vec3& /*worldPos*/)
{
	// Flat plane today (waterDepth). Position reserved for a future
	// per-cell / sloped-water model.
	return land ? land->getWaterElevation() : 0.0f;
}

int sampleMaterialId(const glm::vec3& worldPos)
{
	return land ? static_cast<int>(land->getTerrainType(worldPos)) : 0;
}

long sampleWaterClass(const glm::vec3& worldPos)
{
	// Pure forward — single legacy impl, byte-identical relocation onto the spine.
	return land ? land->getWater(worldPos) : 0;
}

float sampleFeatureMask(const glm::vec3& worldPos, FeatureMask kind)
{
	if (!land)
		return 0.0f;

	switch (kind)
	{
	case FM_Cliff:
	{
		// Live approximation: normalized terrain slope. getTerrainAngle returns
		// the surface angle; normalize against a near-vertical reference so the
		// mask saturates on steep faces. Consumers may threshold as needed.
		const float angle = land->getTerrainAngle(worldPos);
		float m = angle / 60.0f;
		m = std::clamp(m, 0.0f, 1.0f);
		return m;
	}
	case FM_Shoreline:
	{
		// Live approximation: proximity of the surface to the water plane,
		// mirroring the intent of the legacy shore test (terrain.cpp shore band).
		const float gh    = land->getTerrainElevation(worldPos);
		const float water = land->getWaterElevation();
		const float band  = 30.0f; // world units; ~quarter tile spacing
		float d = fabsf(gh - water);
		float m = 1.0f - (d / band);
		m = std::clamp(m, 0.0f, 1.0f);
		return m;
	}
	case FM_Cement:        // colormap-baked artifact today — no runtime feature data
	case FM_ForestSuppress:// future veg-suppress mask
	default:
		return 0.0f;
	}
}

bool parityGateEnabled()
{
	return mc2::gate::get<"MC2_TERRAIN_RUNTIME_PARITY">();
}

} // namespace TerrainRuntime
