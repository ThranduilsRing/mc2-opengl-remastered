// Team: teamLineOfSight and the Team::lineOfSight overloads (the cell-walk and the world-position entry).
// Declared in the class headers; shared names in team_internal.h.
#include <algorithm>
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "team_internal.h"
#include "los_kernel.h"
#include "sensor_trace.h"
#include "job_system.h"
#include "Gates/render_options.h"
#include <gameos.hpp>
#include "terrtxm2.h"
#include <vector>
#include "gos_profiler.h"

static_assert(mc2::los::kCellHeightMask == static_cast<uint32_t>(MAPCELL_HEIGHT_MASK) &&
              mc2::los::kCellHeightShift == MAPCELL_HEIGHT_SHIFT &&
              mc2::los::kCellForestMask == static_cast<uint32_t>(MAPCELL_FOREST_MASK),
              "los_kernel.h cell bits must match MapCell");

static float legacyTerrainElevation(const glm::vec3& position)
{
	return land->getTerrainElevation(position);
}

using ElevationOverride = float (*)(const glm::vec3&);

// Everything the legacy override decision reads, so a mid-turn option/colormap change invalidates the snapshot.
struct LosElevationState
{
	ElevationOverride fn = nullptr;
	const TerrainColorMap* textureSet = nullptr;
	const void* colorMap = nullptr;
	const void* dispAlpha = nullptr;
	bool displacePositive = false;

	bool operator==(const LosElevationState& o) const
	{
		return fn == o.fn && textureSet == o.textureSet && colorMap == o.colorMap &&
		       dispAlpha == o.dispAlpha && displacePositive == o.displacePositive;
	}
};

static LosElevationState losElevationState()
{
	LosElevationState state;
	static const bool s_retireCpuPhong = mc2::options::current().cpuPhongRetire;
	state.displacePositive = gos_GetTerrainDisplaceScale() > 0.0f;
	if (!Terrain::terrainTextures2)
		return state;
	TerrainColorMap* tcm = Terrain::terrainTextures2;
	state.textureSet = tcm;
	state.colorMap = tcm->cpuColorMap;
	state.dispAlpha = tcm->cpuDispAlpha;
	if (!s_retireCpuPhong || (state.displacePositive && tcm->cpuColorMap && tcm->cpuDispAlpha))
		state.fn = &legacyTerrainElevation;
	return state;
}

// The by-value inputs captureLosSnapshot copies. The vertex and cell grids are views, so edits to them need no new snapshot.
struct LosSnapshotKey
{
	long turn;
	const MapData* mapData;
	const void* landTerrain;
	const void* vertices;
	const void* cells;
	long cellsHeight;
	long cellsWidth;
	long realVerticesMapSide;
	long verticesMapSide;
	float worldUnitsPerVertex;
	float oneOverWorldUnitsPerVertex;
	float worldUnitsMapSide;
	glm::vec3 mapTopLeft3d;
	float topLeftVertexX;
	float topLeftVertexY;
	LosElevationState elevation;
	bool useRealLOS;
	float worldUnitsPerMeter;
	unsigned long maxTreeLOSCellBlock;

	bool operator==(const LosSnapshotKey& o) const
	{
		return turn == o.turn && mapData == o.mapData && landTerrain == o.landTerrain && vertices == o.vertices && cells == o.cells &&
		       cellsHeight == o.cellsHeight && cellsWidth == o.cellsWidth &&
		       realVerticesMapSide == o.realVerticesMapSide && verticesMapSide == o.verticesMapSide &&
		       worldUnitsPerVertex == o.worldUnitsPerVertex &&
		       oneOverWorldUnitsPerVertex == o.oneOverWorldUnitsPerVertex &&
		       worldUnitsMapSide == o.worldUnitsMapSide &&
		       mapTopLeft3d.x == o.mapTopLeft3d.x && mapTopLeft3d.y == o.mapTopLeft3d.y &&
		       mapTopLeft3d.z == o.mapTopLeft3d.z &&
		       topLeftVertexX == o.topLeftVertexX && topLeftVertexY == o.topLeftVertexY &&
		       elevation == o.elevation && useRealLOS == o.useRealLOS &&
		       worldUnitsPerMeter == o.worldUnitsPerMeter && maxTreeLOSCellBlock == o.maxTreeLOSCellBlock;
	}
};

static LosSnapshotKey losSnapshotKey()
{
	MapData* md = Terrain::mapData;
	LosSnapshotKey key{};
	key.turn = turn;
	key.mapData = md;
	key.landTerrain = land;
	key.vertices = md ? md->getBlocks() : nullptr;
	if (GameMap)
	{
		key.cellsHeight = GameMap->height;
		key.cellsWidth = GameMap->width;
		key.cells = GameMap->map.empty() ? nullptr : GameMap->map.data();
	}
	key.realVerticesMapSide = Terrain::realVerticesMapSide;
	key.verticesMapSide = Terrain::verticesBlockSide * Terrain::blocksMapSide;
	key.worldUnitsPerVertex = Terrain::worldUnitsPerVertex;
	key.oneOverWorldUnitsPerVertex = Terrain::oneOverWorldUnitsPerVertex;
	key.worldUnitsMapSide = Terrain::worldUnitsMapSide;
	key.mapTopLeft3d = Terrain::mapTopLeft3d;
	if (md)
	{
		const Stuff::Vector2DOf<float> tl = md->getTopLeftVertex();
		key.topLeftVertexX = tl.x;
		key.topLeftVertexY = tl.y;
	}
	key.elevation = losElevationState();
	key.useRealLOS = useRealLOS;
	key.worldUnitsPerMeter = worldUnitsPerMeter;
	key.maxTreeLOSCellBlock = MaxTreeLOSCellBlock;
	return key;
}

// Immutable view of the terrain mesh, the cell grid and the legacy globals the LOS walk reads (see los_kernel.h).
// Non-default terrain options (CPU Phong, dirt displacement) route elevation through the legacy MapData call.
static mc2::los::Snapshot captureLosSnapshot()
{
	mc2::los::Snapshot s;
	MapData* md = Terrain::mapData;
	if (!md || !md->getBlocks() || !GameMap || GameMap->map.empty())
		return s;

	s.elevationOverride = losElevationState().fn;

	PostcompVertex* blocks = md->getBlocks();
	static_assert(sizeof(PostcompVertex) % sizeof(float) == 0 && sizeof(MapCell) == sizeof(uint32_t));
	const long side = Terrain::realVerticesMapSide;
	const long vertexStride = static_cast<long>(sizeof(PostcompVertex) / sizeof(float));
	s.vertices = mc2::los::Snapshot::VertexGrid(&blocks[0].elevation,
		std::layout_stride::mapping<std::dextents<long, 2>>(std::dextents<long, 2>(side, side),
			std::array<long, 2>{ side * vertexStride, vertexStride }));
	s.verticesMapSide = Terrain::verticesBlockSide * Terrain::blocksMapSide;
	s.worldUnitsPerVertex = Terrain::worldUnitsPerVertex;
	s.oneOverWorldUnitsPerVertex = Terrain::oneOverWorldUnitsPerVertex;
	s.worldUnitsMapSide = Terrain::worldUnitsMapSide;
	s.mapTopLeft3d = Terrain::mapTopLeft3d;
	const Stuff::Vector2DOf<float> tl = md->getTopLeftVertex();
	s.topLeftVertexX = tl.x;
	s.topLeftVertexY = tl.y;
	s.cells = mc2::los::Snapshot::CellGrid(&GameMap->map[0].data, GameMap->height, GameMap->width);
	s.useRealLOS = useRealLOS;
	s.maxTeams = MAX_TEAMS;
	s.worldUnitsPerMeter = worldUnitsPerMeter;
	s.smallEps = Stuff::SMALL;
	s.maxTreeLOSCellBlock = MaxTreeLOSCellBlock;
	s.valid = true;
	return s;
}

static const mc2::los::Snapshot& losSnapshot()
{
	thread_local mc2::los::Snapshot cached;
	thread_local LosSnapshotKey cachedKey{};
	thread_local bool hasCached = false;
	LosSnapshotKey key = losSnapshotKey();
	if (!hasCached || !(key == cachedKey))
	{
		cached = captureLosSnapshot();
		cachedKey = key;
		hasCached = true;
	}
	return cached;
}


// Is tPos inside this member's visual range (altitude table, scout bonus, LOS factor)?
static bool inVisualRange(GameObjectPtr obj, const glm::vec3& tPos)
{
	glm::vec3 distance;
	distance = tPos - obj->getPosition();
	float dist = glm::length(distance);

	//Figure out altitude above minimum terrain altitude and look up in table.
	float baseElevation = MapData::waterDepth;
	if (MapData::waterDepth < Terrain::userMin)
		baseElevation = Terrain::userMin;

	float altitude = obj->getPosition().z - baseElevation;
	float altitudeIntegerRange = (Terrain::userMax - baseElevation) * 0.00390625f;
	long altLevel = 0;
	if (altitudeIntegerRange > Stuff::SMALL)
		altLevel = altitude / altitudeIntegerRange;

	altLevel = std::max<long>(altLevel, 0);

	altLevel = std::min<long>(altLevel, 255);

	float radius = visualRangeTable[altLevel];

	//Scouting specialty skill.
	if (obj->isMover())
	{
		MoverPtr mover = static_cast<MoverPtr>(obj);
		if (mover->pilot && mover->pilot->isScout())
			radius += (radius * 0.2f);

		radius *= mover->getLOSFactor();
	}

	return dist <= (radius * 25.0f * worldUnitsPerMeter);
}

//---------------------------------------------------------------------------
bool Team::teamLineOfSight (glm::vec3 tPos, float extRad)
{
	ZoneScopedN("GameLogic.LOS.Team");
	// The range gates run here in roster order; each in-range member contributes one ray. The rays are pure reads, so
	// they run as one batch (jobs when the batch is big enough) and the answer is "any hit" - the same bit the legacy
	// first-hit early return produced.
	thread_local std::vector<mc2::los::Query> pending;
	pending.clear();
	auto addRay = [&](glm::vec3 from, float ext, float startExt) {
		mc2::los::Query q;
		q.position = from;
		q.target = tPos;
		q.teamId = id;
		q.extRad = ext;
		q.startExtRad = startExt;
		pending.push_back(q);
	};

	//-----------------------------------------------------------
	// For each member of the team, check LOS to point provided.
	for (long i = 0; i < rosterSize; i++)
	{
		MoverPtr obj = static_cast<MoverPtr>(ObjectManager->getByWatchID(roster[i]));
		if (!obj->isDisabled() && !obj->isDestroyed() && (obj->getStatus() != OBJECT_STATUS_SHUTDOWN) && inVisualRange(obj, tPos))
			addRay(obj->getLOSPosition(), extRad, 0.0f);
	}

	//-------------------------------------------------------------------------
	// Check the lookout towers now.  You can find them in special Buildings!!
	for (long spBuilding = 0; spBuilding < ObjectManager->numSpecialBuildings; spBuilding++)
	{
		if (ObjectManager->specialBuildings[spBuilding] &&
			ObjectManager->specialBuildings[spBuilding]->getExists() &&
			ObjectManager->specialBuildings[spBuilding]->isLookoutTower() &&
			(ObjectManager->specialBuildings[spBuilding]->getTeamId() ==id))
		{
			GameObjectPtr obj = ObjectManager->specialBuildings[spBuilding];
			if (!obj->isDisabled() && !obj->isDestroyed() && (obj->getStatus() != OBJECT_STATUS_SHUTDOWN) && inVisualRange(obj, tPos))
				addRay(obj->getLOSPosition(), 0.0f, obj->getAppearRadius());
		}
	}

	if (pending.empty())
		return false;

	const mc2::los::Snapshot& snap = losSnapshot();
	if (!snap.valid)
		return false;

	constexpr size_t kBatchMin = 8;
	if (pending.size() >= kBatchMin && mc2::jobs::enabled())
	{
		thread_local std::vector<uint8_t> hits;
		hits.assign(pending.size(), 0);
		mc2::los::batch(snap, pending, hits);
		for (uint8_t h : hits)
			if (h)
				return true;
		return false;
	}
	for (const mc2::los::Query& q : pending)
		if (mc2::los::query(snap, q))
			return true;
	return false;
}

//---------------------------------------------------------------------------
bool Team::lineOfSight (float startLocal, long mCellRow, long mCellCol, float endLocal, long tCellRow, long tCellCol, long teamId, float extRad, float startExtRad)
{
	ZoneScopedN("GameLogic.LOS.Walk");
	if ((teamId < 0) || (teamId >= MAX_TEAMS))
		return true;
	const mc2::los::Snapshot& snap = losSnapshot();
	if (!snap.valid)
		return false;
	return mc2::los::walk(snap, startLocal, mCellRow, mCellCol, endLocal, tCellRow, tCellCol, teamId, extRad, startExtRad);
}

//---------------------------------------------------------------------------
bool Team::lineOfSight (glm::vec3 position, glm::vec3 targetPosition, long teamId, float extRad, float startExtRad)
{
	ZoneScopedN("GameLogic.LOS.Query");
	if ((teamId < 0) || (teamId >= MAX_TEAMS))
		return true;
	const mc2::los::Snapshot& snap = losSnapshot();
	if (!snap.valid)
		return false;
	mc2::los::Query q;
	q.position = position;
	q.target = targetPosition;
	q.teamId = teamId;
	q.extRad = extRad;
	q.startExtRad = startExtRad;
	return mc2::los::query(snap, q);
}

void Team::lineOfSightBatch(std::span<const mc2::los::Query> queries, std::span<uint8_t> hits)
{
	ZoneScopedN("GameLogic.LOS.SensorBatch");
	const mc2::los::Snapshot snap = captureLosSnapshot();
	if (!snap.valid) {
		std::fill(hits.begin(), hits.end(), uint8_t{0});
		return;
	}
	if (mc2_sensor_trace::g_counters.active && snap.elevationOverride)
		++mc2_sensor_trace::g_counters.legacyBatches;
	// Single-block batches stay inline; larger batches exceed the dispatch floor.
	if (mc2_sensor_trace::g_counters.active && !snap.elevationOverride &&
		mc2::jobs::enabled() && queries.size() > 64) {
		++mc2_sensor_trace::g_counters.jobDispatches;
	}
	mc2::los::batch(snap, queries, hits);
}
