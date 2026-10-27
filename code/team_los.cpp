// Team: teamLineOfSight and the Team::lineOfSight overloads (the cell-walk and the world-position entry).
// Declared in the class headers; shared names in team_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "team_internal.h"
#include "los_kernel.h"
#include "job_system.h"
#include "Gates/render_options.h"
#include <gameos.hpp>
#include "terrtxm2.h"
#include <vector>

static_assert(mc2::los::kCellHeightMask == static_cast<uint32_t>(MAPCELL_HEIGHT_MASK) &&
              mc2::los::kCellHeightShift == MAPCELL_HEIGHT_SHIFT &&
              mc2::los::kCellForestMask == static_cast<uint32_t>(MAPCELL_FOREST_MASK),
              "los_kernel.h cell bits must match MapCell");

static float legacyTerrainElevation(const glm::vec3& position)
{
	return land->getTerrainElevation(position);
}

// Immutable view of the terrain mesh, the cell grid and the legacy globals the LOS walk reads (see los_kernel.h).
// Non-default terrain options (CPU Phong, dirt displacement) route elevation through the legacy MapData call.
static mc2::los::Snapshot captureLosSnapshot()
{
	mc2::los::Snapshot s;
	MapData* md = Terrain::mapData;
	if (!md || !md->getBlocks() || !GameMap || GameMap->map.empty())
		return s;

	static const bool s_retireCpuPhong = mc2::options::current().cpuPhongRetire;
	if (Terrain::terrainTextures2)
	{
		TerrainColorMap* tcm = Terrain::terrainTextures2;
		if (!s_retireCpuPhong || (gos_GetTerrainDisplaceScale() > 0.0f && tcm->cpuColorMap && tcm->cpuDispAlpha))
			s.elevationOverride = &legacyTerrainElevation;
	}

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

#ifdef LAB_ONLY
extern bool drawTerrainGrid;
extern long currentLineElement;
extern LineElement *debugLines[];
#endif

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

	if (altLevel < 0)
		altLevel = 0;

	if (altLevel > 255)
		altLevel = 255;

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
	// The range gates run here in roster order; each in-range member contributes one ray. The rays are pure reads, so
	// they run as one batch (jobs when the batch is big enough) and the answer is "any hit" - the same bit the legacy
	// first-hit early return produced.
	static std::vector<mc2::los::Query> pending;
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

	const mc2::los::Snapshot snap = captureLosSnapshot();
	if (!snap.valid)
		return false;

	constexpr size_t kBatchMin = 8;
	if (pending.size() >= kBatchMin && mc2::jobs::enabled())
	{
		static std::vector<uint8_t> hits;
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
	const mc2::los::Snapshot snap = captureLosSnapshot();
	if (!snap.valid)
		return false;
	return mc2::los::walk(snap, startLocal, mCellRow, mCellCol, endLocal, tCellRow, tCellCol, teamId, extRad, startExtRad);
}

//---------------------------------------------------------------------------
bool Team::lineOfSight (glm::vec3 position, glm::vec3 targetPosition, long teamId, float extRad, float startExtRad)
{
	const mc2::los::Snapshot snap = captureLosSnapshot();
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
