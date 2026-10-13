// Team: teamLineOfSight and the Team::lineOfSight overloads (the cell-walk and the world-position entry).
// Declared in the class headers; shared names in team_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "team_internal.h"

#ifdef LAB_ONLY
extern bool drawTerrainGrid;
extern long currentLineElement;
extern LineElement *debugLines[];
#endif

//---------------------------------------------------------------------------
bool Team::teamLineOfSight (Stuff::Vector3D tPos, float extRad)
{
	//-----------------------------------------------------------
	// For each member of the team, check LOS to point provided.
	for (long i = 0; i < rosterSize; i++) 
	{
		MoverPtr obj = (MoverPtr)ObjectManager->getByWatchID(roster[i]);
		if (!obj->isDisabled() && !obj->isDestroyed() && (obj->getStatus() != OBJECT_STATUS_SHUTDOWN))
		{
			Stuff::Vector3D distance;
			distance.Subtract(tPos,obj->getPosition());
			float dist = distance.GetApproximateLength();
		
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
				if (lineOfSight(obj->getLOSPosition(),tPos,id,extRad,0.0f,false))
					return true;
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
				Stuff::Vector3D distance;
				distance.Subtract(tPos,obj->getPosition());
				float dist = distance.GetApproximateLength();
			
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
					MoverPtr mover = (MoverPtr)obj;
					if (mover->pilot && mover->pilot->isScout())
						radius += (radius * 0.2f);
						
					radius *= mover->getLOSFactor();
				}
			
				if (dist <= (radius * 25.0f * worldUnitsPerMeter))
				{
					if (lineOfSight(obj->getLOSPosition(),tPos,id,0.0f,obj->getAppearRadius(),false))
						return true;
				}
			}
				
		}
	}

	return false;
}

constexpr float ACCURACY_ADJUST = 1.5f;
const float HALF_CELL_DIST	= (128.0f / 6.0f);
//---------------------------------------------------------------------------
bool Team::lineOfSight (float startLocal, long mCellRow, long mCellCol, float endLocal, long tCellRow, long tCellCol, long teamId, float extRad, float startExtRad, bool checkVisibleBits)
{
	#ifdef LAB_ONLY
	__int64 x=GetCycles();
	#endif
	
	//-----------------------------------------------------
	// Once we allow teams to have alliances (for contacts,
	// etc.), simply set all nec. team bits in this mask...

	//TILE HACK...
	long tileRow = tCellRow / 3;
	long tileCol = tCellCol / 3;

	if ((teamId < 0) || (teamId >= MAX_TEAMS))	//Not on any team.  It can see everything!
		return true;


	if (useRealLOS)
	{
		//------------------------------------------------------------------------------------------
		// Within magic radius.  Check REAL LOS now.
		// Check is really simple.
		// Find deltaCellRow and deltaCellCol and iterate over them from source to dest.
		// If the magic line ever goes BELOW the terrainElevation PLUS localElevation return false.
		Stuff::Vector3D startPos, endPos;
		startPos.Zero();
		endPos.Zero();
		
		land->getCellPos(tCellRow,tCellCol,endPos);
		land->getCellPos(mCellRow,mCellCol,startPos);
		startPos.z += startLocal;
		endPos.z += endLocal;

		Stuff::Vector3D deltaCellVec;
		deltaCellVec.y = tCellRow - mCellRow;
		deltaCellVec.x = tCellCol - mCellCol;
		deltaCellVec.z = 0.0f;
		float startHeight = startPos.z;
		
		float length = deltaCellVec.GetApproximateLength();
		length *= ACCURACY_ADJUST;
		
		if (length > Stuff::SMALL)
		{
			float colLength = (endPos.x - startPos.x) / length;
			float rowLength = (endPos.y - startPos.y) / length;
			float heightLen = (endPos.z - startPos.z) / (length + ACCURACY_ADJUST);
			
			Stuff::Vector3D currentPos = startPos;
			currentPos.z = land->getTerrainElevation(currentPos);
			long maxDistIter = (length - 0.5f);
			long maxTrees = 0;

			Stuff::Vector3D dist;
			dist.Subtract(endPos,currentPos);
			
			float remainingDist = dist.GetApproximateLength();
			bool checkExtent = (extRad > Stuff::SMALL);
			bool checkStart = (startExtRad > Stuff::SMALL);
			extRad += HALF_CELL_DIST;
			for (long distIter = 0;distIter < maxDistIter;distIter++)
			{
				bool outsideStartRadius = true;
				if (checkStart)
				{
					Stuff::Vector3D distance;
					distance.Subtract(currentPos,startPos);
					distance.z = 0.0f;
					float dist = distance.GetApproximateLength();
					if (dist <= startExtRad)
						outsideStartRadius = false;
				}

				startHeight += heightLen;

				int curCellRow, curCellCol;
				land->worldToCell(currentPos,curCellRow, curCellCol);

				float localElev = (worldUnitsPerMeter * 4.0f * (float)GameMap->getLocalHeight(curCellRow,curCellCol)); 
				float thisHeight = currentPos.z + localElev;

				//First, check if we are now inside the extent radius of the thing we are calcing LOS to.
				// If we are and we haven't returned false since we're here, we can see it!!!!
				if (checkExtent)
				{
					dist.Subtract(endPos,currentPos);
					remainingDist = dist.GetApproximateLength();
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
							Stuff::Vector3D realStart = startPos;
							Stuff::Vector4D lineStart, lineEnd;
							// [PROJECTZ:DebugOnly id=debug_los_line_c0]
							eye->projectForDebugOverlay(realStart,lineStart);
							// [PROJECTZ:DebugOnly id=debug_los_line_c1]
							eye->projectForDebugOverlay(endPos,lineEnd);

							debugLines[currentLineElement++] = new LineElement(lineStart,lineEnd,SD_RED,NULL,-1);
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
			Stuff::Vector3D realStart = startPos;
			Stuff::Vector4D lineStart, lineEnd;
			// [PROJECTZ:DebugOnly id=debug_los_line_d0]
			eye->projectForDebugOverlay(realStart,lineStart);
			// [PROJECTZ:DebugOnly id=debug_los_line_d1]
			eye->projectForDebugOverlay(endPos,lineEnd);

			debugLines[currentLineElement++] = new LineElement(lineStart,lineEnd,SD_GREEN,NULL,-1);
		}
#endif
	}


	return true;
}

//---------------------------------------------------------------------------
bool Team::lineOfSight (Stuff::Vector3D position, Stuff::Vector3D targetPosition, long teamId, float extRad, float startExtRad, bool checkVisibleBits) 
{
	int posCellR, posCellC;
	int tarCellR, tarCellC;
	land->worldToCell(position, posCellR, posCellC);
	land->worldToCell(targetPosition, tarCellR, tarCellC);
	
	float elev = land->getTerrainElevation(position);
	float localStart = position.z - elev;

	elev = land->getTerrainElevation(targetPosition);
	float localEnd = targetPosition.z - elev;
	
	return(lineOfSight(localStart,posCellR, posCellC, localEnd, tarCellR, tarCellC,teamId,extRad, startExtRad, checkVisibleBits));
}
