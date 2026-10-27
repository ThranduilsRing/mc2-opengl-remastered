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

// Immutable view of the terrain mesh, the cell grid and the legacy globals the LOS walk reads (see los_kernel.h).
// valid=false when MapData::terrainElevation would add its Phong or dirt-displacement terms (non-default options): those
// callers keep the legacy walk below.
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
		if (!s_retireCpuPhong)
			return s;
		if (gos_GetTerrainDisplaceScale() > 0.0f && tcm->cpuColorMap && tcm->cpuDispAlpha)
			return s;
	}

	PostcompVertex* blocks = md->getBlocks();
	s.vertexBase = reinterpret_cast<const unsigned char*>(&blocks[0].elevation);
	s.vertexStride = sizeof(PostcompVertex);
	s.realVerticesMapSide = Terrain::realVerticesMapSide;
	s.verticesMapSide = Terrain::verticesBlockSide * Terrain::blocksMapSide;
	s.worldUnitsPerVertex = Terrain::worldUnitsPerVertex;
	s.oneOverWorldUnitsPerVertex = Terrain::oneOverWorldUnitsPerVertex;
	s.worldUnitsMapSide = Terrain::worldUnitsMapSide;
	s.mapTopLeft3d = Terrain::mapTopLeft3d;
	const Stuff::Vector2DOf<float> tl = md->getTopLeftVertex();
	s.topLeftVertexX = tl.x;
	s.topLeftVertexY = tl.y;
	s.cells = reinterpret_cast<const uint32_t*>(GameMap->map.data());
	s.cellWidth = GameMap->width;
	s.cellHeight = GameMap->height;
	s.useRealLOS = useRealLOS;
	s.maxTeams = MAX_TEAMS;
	s.worldUnitsPerMeter = worldUnitsPerMeter;
	s.small = Stuff::SMALL;
	s.maxTreeLOSCellBlock = MaxTreeLOSCellBlock;
	s.valid = true;
	return s;
}

#ifdef LAB_ONLY
extern bool drawTerrainGrid;
extern long currentLineElement;
extern LineElement *debugLines[];
#endif

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
		if (!obj->isDisabled() && !obj->isDestroyed() && (obj->getStatus() != OBJECT_STATUS_SHUTDOWN))
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
				MoverPtr mover = obj;
				if (mover->pilot && mover->pilot->isScout())
					radius += (radius * 0.2f);
					
				radius *= mover->getLOSFactor();
			}
		
			if (dist <= (radius * 25.0f * worldUnitsPerMeter))
			{
				addRay(obj->getLOSPosition(), extRad, 0.0f);
			}
		}
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
			if (!obj->isDisabled() && !obj->isDestroyed() && (obj->getStatus() != OBJECT_STATUS_SHUTDOWN))
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
			
				if (dist <= (radius * 25.0f * worldUnitsPerMeter))
				{
					addRay(obj->getLOSPosition(), 0.0f, obj->getAppearRadius());
				}
			}
				
		}
	}

	if (pending.empty())
		return false;

	const mc2::los::Snapshot snap = captureLosSnapshot();
	if (!snap.valid)
	{
		for (const mc2::los::Query& q : pending)
			if (lineOfSight(q.position, q.target, q.teamId, q.extRad, q.startExtRad))
				return true;
		return false;
	}

	constexpr size_t kBatchMin = 8;
	if (pending.size() >= kBatchMin && mc2::jobs::enabled())
	{
		static std::vector<uint8_t> hits;
		hits.assign(pending.size(), 0);
		mc2::los::batch(snap, pending.data(), hits.data(), static_cast<int>(pending.size()));
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

constexpr float ACCURACY_ADJUST = 1.5f;
const float HALF_CELL_DIST	= (128.0f / 6.0f);
//---------------------------------------------------------------------------
bool Team::lineOfSight (float startLocal, long mCellRow, long mCellCol, float endLocal, long tCellRow, long tCellCol, long teamId, float extRad, float startExtRad)
{
	#ifdef LAB_ONLY
	__int64 x=GetCycles();
	#endif
	
	//-----------------------------------------------------
	// Once we allow teams to have alliances (for contacts,
	// etc.), simply set all nec. team bits in this mask...

	if ((teamId < 0) || (teamId >= MAX_TEAMS))	//Not on any team.  It can see everything!
		return true;


	if (useRealLOS)
	{
		//------------------------------------------------------------------------------------------
		// Within magic radius.  Check REAL LOS now.
		// Check is really simple.
		// Find deltaCellRow and deltaCellCol and iterate over them from source to dest.
		// If the magic line ever goes BELOW the terrainElevation PLUS localElevation return false.
		glm::vec3 startPos, endPos;
		startPos = glm::vec3(0.0f);
		endPos = glm::vec3(0.0f);
		
		land->getCellPos(tCellRow,tCellCol,endPos);
		land->getCellPos(mCellRow,mCellCol,startPos);
		startPos.z += startLocal;
		endPos.z += endLocal;

		glm::vec3 deltaCellVec;
		deltaCellVec.y = tCellRow - mCellRow;
		deltaCellVec.x = tCellCol - mCellCol;
		deltaCellVec.z = 0.0f;
		float startHeight = startPos.z;
		
		float length = glm::length(deltaCellVec);
		length *= ACCURACY_ADJUST;
		
		if (length > Stuff::SMALL)
		{
			float colLength = (endPos.x - startPos.x) / length;
			float rowLength = (endPos.y - startPos.y) / length;
			float heightLen = (endPos.z - startPos.z) / (length + ACCURACY_ADJUST);
			
			glm::vec3 currentPos = startPos;
			currentPos.z = land->getTerrainElevation(currentPos);
			long maxDistIter = (length - 0.5f);
			long maxTrees = 0;

			glm::vec3 dist;
			dist = endPos - currentPos;
			
			float remainingDist = glm::length(dist);
			bool checkExtent = (extRad > Stuff::SMALL);
			bool checkStart = (startExtRad > Stuff::SMALL);
			extRad += HALF_CELL_DIST;
			for (long distIter = 0;distIter < maxDistIter;distIter++)
			{
				bool outsideStartRadius = true;
				if (checkStart)
				{
					glm::vec3 distance;
					distance = currentPos - startPos;
					distance.z = 0.0f;
					float dist = glm::length(distance);
					if (dist <= startExtRad)
						outsideStartRadius = false;
				}

				startHeight += heightLen;

				int curCellRow, curCellCol;
				land->worldToCell(currentPos,curCellRow, curCellCol);

				float localElev = (worldUnitsPerMeter * 4.0f * static_cast<float>(GameMap->getLocalHeight(curCellRow,curCellCol)));
				float thisHeight = currentPos.z + localElev;

				//First, check if we are now inside the extent radius of the thing we are calcing LOS to.
				// If we are and we haven't returned false since we're here, we can see it!!!!
				if (checkExtent)
				{
					dist = endPos - currentPos;
					remainingDist = glm::length(dist);
					if (remainingDist <= extRad)
						break;
				}

				if (outsideStartRadius && (startHeight < thisHeight))
				{
					bool isTree = false;
					if (GameMap->getForest(curCellRow,curCellCol))
					{
						maxTrees++;
						isTree = true;
					}

					if (!isTree || (maxTrees >= MaxTreeLOSCellBlock))
					{
		
#ifdef LAB_ONLY
						if (drawTerrainGrid)
						{
							glm::vec3 realStart = startPos;
							Stuff::Vector4D lineStart, lineEnd;
							// [PROJECTZ:DebugOnly id=debug_los_line_c0]
							eye->projectForDebugOverlay(realStart,lineStart);
							// [PROJECTZ:DebugOnly id=debug_los_line_c1]
							eye->projectForDebugOverlay(endPos,lineEnd);

							debugLines[currentLineElement++] = new LineElement(lineStart,lineEnd,SD_RED,nullptr,-1);
						}
#endif
						return false;
					}
				}

				currentPos.x += colLength;
				currentPos.y += rowLength;
				currentPos.z = land->getTerrainElevation(currentPos);
			}
		}

#ifdef LAB_ONLY
		if (drawTerrainGrid)
		{
			glm::vec3 realStart = startPos;
			Stuff::Vector4D lineStart, lineEnd;
			// [PROJECTZ:DebugOnly id=debug_los_line_d0]
			eye->projectForDebugOverlay(realStart,lineStart);
			// [PROJECTZ:DebugOnly id=debug_los_line_d1]
			eye->projectForDebugOverlay(endPos,lineEnd);

			debugLines[currentLineElement++] = new LineElement(lineStart,lineEnd,SD_GREEN,nullptr,-1);
		}
#endif
	}


	return true;
}

//---------------------------------------------------------------------------
bool Team::lineOfSight (glm::vec3 position, glm::vec3 targetPosition, long teamId, float extRad, float startExtRad)
{
	{
		const mc2::los::Snapshot snap = captureLosSnapshot();
		if (snap.valid)
		{
			mc2::los::Query q;
			q.position = position;
			q.target = targetPosition;
			q.teamId = teamId;
			q.extRad = extRad;
			q.startExtRad = startExtRad;
			return mc2::los::query(snap, q);
		}
	}

	int posCellR, posCellC;
	int tarCellR, tarCellC;
	land->worldToCell(position, posCellR, posCellC);
	land->worldToCell(targetPosition, tarCellR, tarCellC);
	
	float elev = land->getTerrainElevation(position);
	float localStart = position.z - elev;

	elev = land->getTerrainElevation(targetPosition);
	float localEnd = targetPosition.z - elev;
	
	return(lineOfSight(localStart,posCellR, posCellC, localEnd, tarCellR, tarCellC,teamId,extRad, startExtRad));
}
