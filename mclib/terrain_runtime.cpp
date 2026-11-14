//******************************************************************************
// terrain_runtime.cpp — TERRAIN-RUNTIME-API-1
//
// Gameplay queries preserve the sim surface; visual queries use the accepted bake.
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
	static const bool s_gate = false;   // default OFF
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

float sampleVisualHeight(const glm::vec3& worldPos)
{
	if (!land || s_cpuVisualV < 2) return sampleGameplayHeight(worldPos);
	const float inv = 4.0f / Terrain::worldUnitsPerVertex;
	float fx = (worldPos.x - Terrain::mapTopLeft3d.x) * inv;
	float fy = (Terrain::mapTopLeft3d.y - worldPos.y) * inv;
	const float hi = static_cast<float>(s_cpuVisualV - 1);
	if (fx < 0.0f || fy < 0.0f || fx > hi || fy > hi) return sampleGameplayHeight(worldPos);
	int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
	if (x0 >= s_cpuVisualV - 1) x0 = s_cpuVisualV - 2;
	if (y0 >= s_cpuVisualV - 1) y0 = s_cpuVisualV - 2;
	const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
	const float* p = s_cpuVisualHeight.data() + static_cast<size_t>(y0) * s_cpuVisualV + x0;
	const float v = (p[0] * (1 - tx) + p[1] * tx) * (1 - ty) + (p[s_cpuVisualV] * (1 - tx) + p[s_cpuVisualV + 1] * tx) * ty;
	return v;
}

float visualHeightDelta(const glm::vec3& worldPos)
{
	if (!land || s_cpuVisualV < 2) return 0.0f;
	return sampleVisualHeight(worldPos) - sampleGameplayHeight(worldPos);
}

float decalElevation(const glm::vec3& worldPos)
{
	return sampleVisualHeight(worldPos);
}

float markerElevation(const glm::vec3& worldPos)
{
	return std::max(sampleVisualHeight(worldPos), land ? Terrain::waterElevation : 0.0f);
}

float cameraClearanceHeight(const glm::vec3& eye, const glm::vec3 (&nearOffsets)[4], float minimum)
{
	float height = std::max(sampleVisualHeight(eye), land ? Terrain::waterElevation : 0.0f);
	if (!land || s_cpuVisualV < 2) return height + minimum;
	glm::vec3 lo = eye, hi = eye;
	float bottom = 0.0f;
	for (const auto& offset : nearOffsets) {
		const auto corner = eye + offset;
		lo = glm::min(lo, corner);
		hi = glm::max(hi, corner);
		bottom = std::min(bottom, offset.z);
		height = std::max(height, sampleVisualHeight(corner));
	}
	// Bilinear patches cannot exceed their vertices. Include every vertex of
	// every cell touched by the footprint's bounding rectangle, not just its
	// corners: a narrow ridge can pass through the middle of the near plane.
	const float inv = 4.0f / Terrain::worldUnitsPerVertex;
	const int last = s_cpuVisualV - 1;
	const int x0 = std::clamp(static_cast<int>(std::floor((lo.x - Terrain::mapTopLeft3d.x) * inv)), 0, last);
	const int x1 = std::clamp(static_cast<int>(std::ceil((hi.x - Terrain::mapTopLeft3d.x) * inv)), 0, last);
	const int y0 = std::clamp(static_cast<int>(std::floor((Terrain::mapTopLeft3d.y - hi.y) * inv)), 0, last);
	const int y1 = std::clamp(static_cast<int>(std::ceil((Terrain::mapTopLeft3d.y - lo.y) * inv)), 0, last);
	for (int y = y0; y <= y1; ++y)
		for (int x = x0; x <= x1; ++x) {
			const glm::vec3 p(Terrain::mapTopLeft3d.x + x / inv, Terrain::mapTopLeft3d.y - y / inv, 0.0f);
			height = std::max(height, sampleVisualHeight(p));
		}
	return height + minimum - bottom;
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
	return false;
}

} // namespace TerrainRuntime
