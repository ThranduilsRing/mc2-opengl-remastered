// Pure line-of-sight kernel (gamelogic rewrite T5, H0+L0+J0).
//
// Inputs are an immutable Snapshot (views of the terrain height mesh and the mission cell grid, plus the scalar
// constants the legacy walk read from globals) and a Query; the output is a bit. No globals, no allocation, no engine
// headers: the same code runs on the main thread, on mc2::jobs Sim workers, and in the unit tier.
// Bit contract: query() reproduces Team::lineOfSight's legacy cell walk (code/team_los.cpp) exactly, float op for float op.
#pragma once

#include "math/glm_config.h"
#include <cstddef>
#include <array>
#include <cstdint>
#include <mdspan>
#include <span>

namespace mc2::los {

// Packed MapCell bits the walk reads (mirrors mclib/move.h; team_los.cpp static_asserts the match).
inline constexpr uint32_t kCellHeightMask  = 0x003C0000u;
inline constexpr int      kCellHeightShift = 18;
inline constexpr uint32_t kCellForestMask  = 0x10000000u;

struct Snapshot {
	bool valid = false;  // false: no terrain / mission map is loaded
	// Non-null when MapData::terrainElevation adds terms the pure path does not carry (CPU Phong, dirt displacement):
	// elevation then comes from this hook. It reads engine globals, so such a snapshot is main-thread only.
	float (*elevationOverride)(const glm::vec3&) = nullptr;

	// Terrain height mesh: elevation of the vertex at column x, row y is vertices[y, x].
	using VertexGrid = std::mdspan<const float, std::dextents<long, 2>, std::layout_stride>;
	VertexGrid vertices;
	long   verticesMapSide = 0;           // verticesBlockSide * blocksMapSide
	float  worldUnitsPerVertex = 0.0f;
	float  oneOverWorldUnitsPerVertex = 0.0f;
	float  worldUnitsMapSide = 0.0f;
	glm::vec3 mapTopLeft3d{0.0f};
	float  topLeftVertexX = 0.0f;
	float  topLeftVertexY = 0.0f;

	// Mission cell grid: packed MapCell word at cells[row, col].
	using CellGrid = std::mdspan<const uint32_t, std::dextents<int, 2>>;
	CellGrid cells;

	// Legacy globals, captured by value.
	bool  useRealLOS = false;
	long  maxTeams = 0;
	float worldUnitsPerMeter = 0.0f;
	float smallEps = 0.0f;                   // Stuff::SMALL
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

// The cell walk between two cells with the endpoints' heights above ground (Team::lineOfSight's legacy entry).
bool walk(const Snapshot& s, float startLocal, long mCellRow, long mCellCol, float endLocal, long tCellRow, long tCellCol,
          long teamId, float extRad, float startExtRad);

// One query. Requires s.valid.
bool query(const Snapshot& s, const Query& q);

// q.size() queries on mc2::jobs Sim workers (inline when the pool is off or the batch is small). out[i] = query(s, q[i]).
// out is pre-sized by the caller; the caller applies results in its own (original) order.
void batch(const Snapshot& s, std::span<const Query> q, std::span<uint8_t> out);

// Test the nearest origin first, then batch the remainder. Reorders caller scratch only.
bool any(const Snapshot& s, std::span<Query> q);

}  // namespace mc2::los
