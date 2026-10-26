// BldgAppearance / TreeAppearance: BldgAppearance terrain/move-map/line-of-sight marking and cell coverage.
// Declared in the class headers; shared names in bdactor_internal.h.
#include "bdactor_internal.h"
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include "mc2fmt.h"
#include "building_gameplay_observation.h"

// Exercise production marking on isolated maps; never modify the live placement or map.
static void dumpGameplay(BldgAppearance& a, glm::vec3 pos, std::span<const short> cells)
{
    if (!mc2::gate::get<"MC2_DEBUG_STATE_DUMP">()) return;
    std::ostringstream out;
    out << a.appearType->name << "|" << pos.x << "," << pos.y << "," << pos.z
        << "|" << a.rotation << "|" << a.status << "|" << a.getRadius() << "," << a.getTopZ();
    std::set<std::pair<int, int>> covered;
    for (long i = 0; i < static_cast<long>(cells.size()); i += 2) covered.emplace(cells[i], cells[i + 1]);
    out << "|covered=";
    for (auto [r, c] : covered) out << r << "," << c << ";";
    const auto prefix = out.str();
    const auto* art = a.appearType->propMaterialSource;
    mc2::fmt::print_err("[BLDG_ART] {}|applied={}\n", prefix,
                        art && a.appearType->bldgRenderShape ? art->appearanceName : "-");
    std::vector<MapCell> live;
    live.swap(GameMap->map);
    struct RestoreMap {
        std::vector<MapCell>& live;
        ~RestoreMap() { live.swap(GameMap->map); }
    } restore{live};
    BldgAppearance probe;
    probe.bldgShape = nullptr;
    probe.appearType = a.appearType;
    // The probe borrows the type without acquiring a registry reference.
    struct ClearType { BldgAppearance& probe; ~ClearType() { probe.appearType = nullptr; } } clearType{probe};
    probe.position = pos;
    probe.rotation = a.rotation;
    const int side = Terrain::realVerticesMapSide * MAPCELL_DIM;
    std::vector<MissionMapCellInfo> terrain(side * side);
    for (int type : {SPECIAL_NONE, SPECIAL_GATE, SPECIAL_WALL}) {
        out.str("");
        out << prefix << "|kind=" << type;
        for (bool destroyed : {false, true}) {
            out << "|state=";
            if (destroyed && !a.appearType->bldgDmgShape) { out << "absent"; continue; }
            probe.status = destroyed ? OBJECT_STATUS_DESTROYED : OBJECT_STATUS_NORMAL;
            if (probe.bldgShape) probe.releaseBldgShape();
            auto* source = destroyed ? a.appearType->bldgDmgShape :
                a.appearType->getBldgCollisionShape(0);
            probe.bldgShape = source->CreateFrom();
            std::fill(terrain.begin(), terrain.end(), MissionMapCellInfo{});
            for (auto& cell : terrain) cell.passable = true;
            GameMap->map.assign(live.size(), MapCell{});
            probe.markTerrain(terrain.data(), type, 1);
            probe.markLOS();
            writeBuildingGameplayState(out, terrain, GameMap->map, side);
        }
        mc2::fmt::print_err("[BLDG_GAMEPLAY] {}\n", out.str());
    }
    // Targeted runtime regression: all requests share one simulation turn.
    // Compare the reused hierarchy with a fresh stock instance, including frozen
    // animation endpoints, and record the actual cells changed by marking.
    if (S_stricmp(a.appearType->name, "generic_rpbuilding") == 0 ||
        (a.appearType->bdAnimData[1] && a.appearType->bdAnimData[3])) {
        probe.status = OBJECT_STATUS_NORMAL;
        auto blocked = [&] {
            std::ostringstream cells;
            for (int r = 0; r < side; ++r)
                for (int c = 0; c < side; ++c)
                    if (!GameMap->getCell(r, c)->getPassable()) cells << r << "," << c << ";";
            return cells.str();
        };
        for (int gesture : {0, 1, 2, 3, 0}) {
            const long frames = a.appearType->getNumFrames(gesture);
            for (float frame : {0.0f, static_cast<float>(std::max(0L, frames - 1))}) {
                probe.unitAnim().bdAnimationState = gesture;
                probe.unitAnim().currentFrame = frame;
                probe.unitAnim().bdFrameRate = 0.0f;
                GameMap->map.assign(live.size(), MapCell{});
                for (auto& cell : GameMap->map) cell.setPassable(true);
                probe.markMoveMap(false, nullptr);
                const auto before = blocked();
                const auto nodes = probe.simPose->shapeNodes;
                probe.markMoveMap(true, nullptr);
                const auto after = blocked();
                probe.simulationShape_.reset();
                probe.simPose->ready = false;
                probe.updateSimPose();
                const bool matches = nodes == probe.simPose->shapeNodes;
                mc2::fmt::print_err("[BLDG_MOVE] {}|{}|gesture={}|frame={}|cached_matches_fresh={}|blocked={}|released={}\n",
                    a.appearType->name, prefix, gesture, frame, matches, before, after);
            }
        }
    }
    if (probe.bldgShape) probe.releaseBldgShape();
}

inline constexpr float HEIGHT_THRESHOLD = 10.0f;

long BldgAppearance::calcCellsCovered (glm::vec3& pos, short* cellList) {

	gosASSERT((Terrain::realVerticesMapSide * MAPCELL_DIM) == GameMap->width);
	long numCoords = 0;
	long maxCoords = cellList[0];

	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		releaseBldgShape();
	
		bldgShape = appearType->getBldgRenderShape(currentLOD)->CreateFrom();   // render shape stays the override
		if (unitAnim().bdAnimationState != -1)
			appearType->setAnimation(bldgShape,unitAnim().bdAnimationState);
	}

	// Cell coverage comes from the stock collision shape: a model override owns the per-instance
	// RENDER shape (bldgShape) and must never become the passability source.
	std::unique_ptr<TG_MultiShape> stockCover;          // owns the stock copy, if one is made
	struct ClearOnExit { TG_MultiShape* s = nullptr; ~ClearOnExit() { if (s) s->ClearAnimation(); } } clearOnExit;
	TG_MultiShapePtr coverShape = bldgShape;
	if (appearType->bldgRenderShape || appearType->bldgDmgRenderShape)
	{
		TG_TypeMultiShape* source = appearType->getBldgCollisionShape(0);
		if (status == OBJECT_STATUS_DESTROYED && appearType->bldgDmgShape)
			source = appearType->bldgDmgShape;
		stockCover.reset(source->CreateFrom());
		coverShape = clearOnExit.s = stockCover.get();
		if (unitAnim().bdAnimationState != -1)
			appearType->setAnimation(coverShape,unitAnim().bdAnimationState);
	}

	for (int i=0;i<coverShape->GetNumShapes();i++)
	{
		//Check if the artists meant for this piece to NOT block passability!!
		if (S_strnicmp(coverShape->GetNodeId(i),"_PAB",4) != 0)
		{
			for (int j=0;j<coverShape->GetNumVerticesInShape(i);j++)
			{
				glm::vec3 vertexPos, worldPos;
				vertexPos = coverShape->GetShapeVertexInEditor(i,j,-rotation);
				worldPos = pos + vertexPos;

				bool recordCell = false;
				if (appearType->isForestClump)
					recordCell = (vertexPos.z <= 1.0f);
				else
					recordCell = (vertexPos.z >= 1.0f);
				if (recordCell) 
				{
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR)
						|| (0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
						if (numCoords > (maxCoords - 2))
							Fatal(numCoords, "BldgAppearance.markMoveMap: too many coords for cellList ");
							
						cellList[numCoords++] = static_cast<short>(cellR);
						cellList[numCoords++] = static_cast<short>(cellC);
				}
			}
		}
	}
	
	dumpGameplay(*this, pos, std::span<const short>(cellList, numCoords));
	return(numCoords);
}

void BldgAppearance::markTerrain (_ScenarioMapCellInfo* pInfo, int type, int counter)
{
	if (appearType->spinMe)			//We are a marker
		return;						//Do not mark impassable
		
	// A temporary hierarchy shares the original type's mesh buffers. Never replace the
	// render instance while computing editor terrain, LOS or permanent gate rubble.
	const bool overridden = appearType->bldgRenderShape || appearType->bldgDmgRenderShape;
	std::unique_ptr<TG_MultiShape> terrainShape;
	TG_MultiShapePtr terrain = overridden ? nullptr : bldgShape;
	auto useShape = [&](TG_TypeMultiShape* source) {
		if (overridden) {
			if (terrain) terrain->ClearAnimation();
			terrainShape.reset(source->CreateFrom());
			terrain = terrainShape.get();
		} else {
			releaseBldgShape();
			terrain = bldgShape = source->CreateFrom();
		}
		if (unitAnim().bdAnimationState != -1)
			appearType->setAnimation(terrain, unitAnim().bdAnimationState);
	};
	if (overridden)
		useShape(status == OBJECT_STATUS_DESTROYED && appearType->bldgDmgShape ?
			appearType->bldgDmgShape : appearType->getBldgCollisionShape(0));
	else if (currentLOD) {
		currentLOD = 0;
		useShape(appearType->getBldgCollisionShape(0));
	}

	int cellR, cellC;
	land->worldToCell(position, cellR, cellC);
	if (appearType->isForestClump)
	{
		for (int i=0;i<terrain->GetNumShapes();i++)
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(terrain->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<terrain->GetNumVerticesInShape(i);j++)
				{
					glm::vec3 vertexPos, worldPos;
					vertexPos = terrain->GetShapeVertexInEditor(i,j,-rotation);
					worldPos = position + vertexPos;
		
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					
					_ScenarioMapCellInfo*pTmp = &(pInfo[cellR * Terrain::realVerticesMapSide * MAPCELL_DIM + cellC]);
	
					if (vertexPos.z <= 1.0f)
					{
						pTmp->passable = true;
						pTmp->gate = false;
						pTmp->forest = true;
					}
					
					float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
					if (cellLocalHeight > 15.0f)
						cellLocalHeight = 15.0f;
						
					//ONLY mark LOS on cells that are impassable with forests.  Maybe everything?
					if (pTmp->passable && (pTmp->lineOfSight < cellLocalHeight))
						pTmp->lineOfSight = cellLocalHeight+0.5f;
				}
			}
		}
	}
	else
	{
		if ((type == SPECIAL_GATE) || (type == SPECIAL_WALL))
		{
			if (appearType->bldgShape[0])
			{
				useShape(appearType->bldgShape[0]);
			}
		}

		if (type == SPECIAL_LAND_BRIDGE)
		{
			if (appearType->bldgDmgShape)
			{
				useShape(appearType->bldgDmgShape);
			}
		}

		for (int i=0;i<terrain->GetNumShapes();i++)
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(terrain->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<terrain->GetNumVerticesInShape(i);j++)
				{
					glm::vec3 vertexPos, worldPos;
					vertexPos = terrain->GetShapeVertexInEditor(i,j,-rotation);
					worldPos = position + vertexPos;
		
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					_ScenarioMapCellInfo*pTmp = &(pInfo[cellR * Terrain::realVerticesMapSide * MAPCELL_DIM + cellC]);
	
					if (vertexPos.z >= 1.0f)
					{
						pTmp->passable = false;
						
						if (((type == SPECIAL_GATE) || (type == SPECIAL_WALL)))
						{
							pTmp->passable = true;
							pTmp->specialID = counter;
							pTmp->specialType = type;
							if (type == SPECIAL_GATE)
								pTmp->gate = true;
						}
						else if (type == SPECIAL_LAND_BRIDGE)
						{
							pTmp->passable = true;
							pTmp->specialID = counter;
							pTmp->specialType = type;
						}
						else if (type == 18)
						{
							pTmp->specialID = 0;
							pTmp->specialType = SPECIAL_NONE;
							pTmp->passable = true;
						}
						else
						{
							pTmp->specialID = 0;
							pTmp->specialType = SPECIAL_NONE;
						}
							
						if (type != 18)
						{
							float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
							if (cellLocalHeight > 15.0f)
								cellLocalHeight = 15.0f;
								
							if (pTmp->lineOfSight < cellLocalHeight)
								pTmp->lineOfSight = cellLocalHeight+0.5f;
						}
					}
				}
			}
		}
		
 		//Switch to destroyed state to mark impassable.  The destroyed impassability will NEVER change!!
		// When a gate opens or a wall or gate is destroyed, we only want to mark stuff that is going 
		// away passable and long range capable.
		if ((type == SPECIAL_GATE) || (type == SPECIAL_WALL))
		{
			if (appearType->bldgDmgShape)
			{
				useShape(appearType->bldgDmgShape);
			}
		}

		if (type == SPECIAL_LAND_BRIDGE)
		{
			if (appearType->bldgShape[0])
			{
				useShape(appearType->bldgShape[0]);
			}
		}

		for (int i=0;i<terrain->GetNumShapes();i++)
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(terrain->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<terrain->GetNumVerticesInShape(i);j++)
				{
					glm::vec3 vertexPos, worldPos;
					vertexPos = terrain->GetShapeVertexInEditor(i,j,-rotation);
					worldPos = position + vertexPos;
		
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					_ScenarioMapCellInfo*pTmp = &(pInfo[cellR * Terrain::realVerticesMapSide * MAPCELL_DIM + cellC]);
	
					if (vertexPos.z >= 1.0f)
					{
						if (type == 18)
						{
							pTmp->passable = true;
							pTmp->specialID = 0;
							pTmp->specialType = SPECIAL_NONE;
						}
						else
						{
							pTmp->passable = false;
							pTmp->gate = false;			//Perfectly OK to mark these again,  They are no longer special!!
							pTmp->specialID = 0;
							pTmp->specialType = SPECIAL_NONE;
						}
					}
				}
			}
		}
			
	}
	if (overridden) terrain->ClearAnimation();
	else if (!appearType->isForestClump && status != OBJECT_STATUS_DESTROYED && appearType->bldgShape[0])
		useShape(appearType->bldgShape[0]);
	else if (!appearType->isForestClump && status == OBJECT_STATUS_DESTROYED && appearType->bldgDmgShape)
		useShape(appearType->bldgDmgShape);
}

long BldgAppearance::markMoveMap (bool passable, long* lineOfSightRect, bool useHeight, short* cellList)
{
	int minRow = 9999;
	int maxRow = 0;
	int minCol = 9999;
	int maxCol = 0;

	// Passability is saved gameplay state. Use the posed stock hierarchy, never
	// render transforms or an untransformed clone selected by a visual override.
	updateSimPose();
	TG_MultiShapePtr tempBldgShape = simulationShape_.get();
	if (!tempBldgShape) {
		if (lineOfSightRect) {
			lineOfSightRect[0] = lineOfSightRect[1] = 9999;
			lineOfSightRect[2] = lineOfSightRect[3] = 0;
		}
		return 0;
	}

	int numCoords = 0;
	if (cellList) {
		gosASSERT(!useHeight);
		// Store the max number of coords allowed in the first cell. Can overwrite it now...
		int maxCoords = cellList[0];
		for (int i = 0; i < tempBldgShape->GetNumShapes(); i++) 
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(tempBldgShape->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<tempBldgShape->GetNumVerticesInShape(i);j++) 
				{
					glm::vec3 vertexPos, worldPos;
					worldPos = tempBldgShape->GetShapeVertexInWorld(i,j,0.0f);
					vertexPos = worldPos - position;
	
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					
					if (cellR < minRow)
						minRow = cellR;
					if (cellR > maxRow)
						maxRow = cellR;
					if (cellC < minCol)
						minCol = cellC;
					if (cellC > maxCol)
						maxCol = cellC;
						
					if (numCoords > (maxCoords - 2))
						Fatal(numCoords, "BldgAppearance.markMoveMap: too many coords for cellList ");
					cellList[numCoords++] = static_cast<short>(cellR);
					cellList[numCoords++] = static_cast<short>(cellC);
				}
			}
		}
	}
	else 
	{
		for (int i=0;i<tempBldgShape->GetNumShapes();i++)
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(tempBldgShape->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<tempBldgShape->GetNumVerticesInShape(i);j++)
				{
					glm::vec3 vertexPos, worldPos;
					worldPos = tempBldgShape->GetShapeVertexInWorld(i,j,0.0f);
					vertexPos = worldPos - position;
	
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					
					if (cellR < minRow)
						minRow = cellR;
					if (cellR > maxRow)
						maxRow = cellR;
					if (cellC < minCol)
						minCol = cellC;
					if (cellC > maxCol)
						maxCol = cellC;
						
					MapCellPtr curCell = GameMap->getCell(cellR, cellC);
					if (appearType->isForestClump) {
						if (vertexPos.z <= 1.0f)
							curCell->setPassable(passable);
						}
					else {
						if (vertexPos.z >= 1.0f)
							curCell->setPassable(passable);
					}
				}
			}
		}
	}
	
	if (lineOfSightRect) {
		lineOfSightRect[0] = minRow;
		lineOfSightRect[1] = minCol;
		lineOfSightRect[2] = maxRow;
		lineOfSightRect[3] = maxCol;
	}

	return(numCoords/2);
}

void BldgAppearance::markLOS (bool clearIt)
{
	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	// Line of sight comes from the stock shape, never a render override (see markMoveMap).
	TG_MultiShapePtr tempBldgShape = bldgShape;
	const bool renderOverridden = appearType->bldgRenderShape || appearType->bldgDmgRenderShape;
	if (currentLOD || renderOverridden)
	{
		TG_TypeMultiShape* source = appearType->bldgShape[0];
		if (renderOverridden && status == OBJECT_STATUS_DESTROYED && appearType->bldgDmgShape)
			source = appearType->bldgDmgShape;
		tempBldgShape = source->CreateFrom();
		if (unitAnim().bdAnimationState != -1)
			appearType->setAnimation(tempBldgShape,unitAnim().bdAnimationState);
	}

	for (int i=0;i<tempBldgShape->GetNumShapes();i++)
	{
		//Check if the artists meant for this piece to NOT block LOS!!
		// Probably should check for light cones,too!
		
		if ((S_strnicmp(tempBldgShape->GetNodeId(i),"LOS_",4) != 0) &&
			(S_strnicmp(tempBldgShape->GetNodeId(i),"SpotLight_",10) != 0))
		{
			for (int j=0;j<tempBldgShape->GetNumVerticesInShape(i);j++)
			{
				glm::vec3 vertexPos, worldPos;
				vertexPos = tempBldgShape->GetShapeVertexInEditor(i,j,-rotation);
				worldPos = position + vertexPos;
	
				int cellR, cellC;
				land->worldToCell(worldPos,cellR,cellC);
				if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
					(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
				{
					continue;
				}
				
				MapCellPtr curCell = GameMap->getCell(cellR, cellC);

				if (!clearIt)
				{
					float currentCellHeight = curCell->getLocalHeight();
					
					float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
					if (cellLocalHeight > 15.0f)
						cellLocalHeight = 15.0f;

					if (cellLocalHeight > currentCellHeight)
						curCell->setLocalHeight(cellLocalHeight+0.5f);
				}
				else	//We want to clear all LOS height INFO.  We're about to change shape!!
				{
					curCell->setLocalHeight(0.0f);
				}
			}
		}
	}

	if (tempBldgShape != bldgShape)
	{
		tempBldgShape->ClearAnimation();
		delete tempBldgShape;
		tempBldgShape = nullptr;
	}
}

void BldgAppearance::calcAdjCell (long& row, long& col)
{
	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		releaseBldgShape();
	
		bldgShape = appearType->bldgShape[currentLOD]->CreateFrom();
		if (unitAnim().bdAnimationState != -1)
			appearType->setAnimation(bldgShape,unitAnim().bdAnimationState);
	}

	int numVert = 0;
	for (int i=0;i<bldgShape->GetNumShapes();i++)
	{
		for (int j=0;j<bldgShape->GetNumVerticesInShape(i);j++)
		{
			glm::vec3 vertexPos, worldPos;
			vertexPos = bldgShape->GetShapeVertexInWorld(i,j,-rotation);
			worldPos = position + vertexPos;

			{
				numVert++;
				int cellR, cellC;
				land->worldToCell(worldPos,cellR,cellC);
				
			}
		}
	}
}
