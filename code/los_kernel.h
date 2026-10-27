// Pure line-of-sight kernel (gamelogic rewrite T5, H0+L0+J0).
//
// Inputs are an immutable Snapshot (views of the terrain height mesh and the mission cell grid, plus the scalar
// constants the legacy walk read from globals) and a Query; the output is a bit. No globals, no allocation, no engine
// headers: the same code runs on the main thread, on mc2::jobs Sim workers, and in the unit tier.
// Bit contract: query() reproduces Team::lineOfSight's legacy cell walk (code/team_los.cpp) exactly, float op for float op.
#pragma once

#include "math/glm_config.h"
#include <cstddef>
#include <cstdint>

namespace mc2::los {

// Packed MapCell bits the walk reads (mirrors mclib/move.h; team_los.cpp static_asserts the match).
inline constexpr uint32_t kCellHeightMask  = 0x003C0000u;
inline constexpr int      kCellHeightShift = 18;
inline constexpr uint32_t kCellForestMask  = 0x10000000u;

struct Snapshot {
	bool valid = false;  // false: this terrain configuration needs the legacy elevation path (Phong / dirt displacement)

	// Terrain height mesh: elevation of vertex i is the float at vertexBase + i * vertexStride.
	const unsigned char* vertexBase = nullptr;
	size_t               vertexStride = 0;
	long   realVerticesMapSide = 0;
	long   verticesMapSide = 0;           // verticesBlockSide * blocksMapSide
	float  worldUnitsPerVertex = 0.0f;
	float  oneOverWorldUnitsPerVertex = 0.0f;
	float  worldUnitsMapSide = 0.0f;
	glm::vec3 mapTopLeft3d{0.0f};
	float  topLeftVertexX = 0.0f;
	float  topLeftVertexY = 0.0f;

	// Mission cell grid (row-major, height x width packed MapCell words).
	const uint32_t* cells = nullptr;
	int cellWidth = 0;
	int cellHeight = 0;

	// Legacy globals, captured by value.
	bool  useRealLOS = false;
	long  maxTeams = 0;
	float worldUnitsPerMeter = 0.0f;
	float small = 0.0f;                   // Stuff::SMALL
	unsigned long maxTreeLOSCellBlock = 0;
};

struct Query {
	glm::vec3 position{0.0f};
	glm::vec3 target{0.0f};
	long  teamId = 0;
	float extRad = 0.0f;
	float startExtRad = 0.0f;
};

float terrainElevation(const Snapshot& s, const glm::vec3& position);

// One query. Requires s.valid.
bool query(const Snapshot& s, const Query& q);

// n queries on mc2::jobs Sim workers (inline when the pool is off or the batch is small). out[i] = query(s, q[i]).
// out is pre-sized by the caller; the caller applies results in its own (original) order.
void batch(const Snapshot& s, const Query* q, uint8_t* out, int n);

}  // namespace mc2::los
