// Editor: object rotate/frame/transform helpers, the editor smoke fixtures and the foliage/forest commands.
// Declared in the class headers; shared names in EditorInterface_internal.h.
#include "EditorInterface_internal.h"


void EditorInterface::rotateSelectedObjects( int direction )
{
	ModifyBuildingAction *pAction = new ModifyBuildingAction;
	EditorObjectMgr::EDITOR_OBJECT_LIST selectedObjects;
	selectedObjects = EditorObjectMgr::instance()->getSelectedObjectList();
	auto iter = selectedObjects.begin();
	while (iter != selectedObjects.end())
	{
		// appearance() can be NULL; skip.
		if ( !(*iter) || !(*iter)->appearance() )
		{
			iter++;
			continue;
		}
		pAction->addBuildingInfo(*(*iter));
		int id = (*iter)->getID();
		int fitID = EditorObjectMgr::instance()->getFitID(id);
		if ((EditorObjectMgr::WALL == (*iter)->getSpecialType()) || (33/*repair bay*/ == fitID))
		{
			(*iter)->appearance()->rotation += direction * 90;
		}
		else
		{
			(*iter)->appearance()->rotation += direction * 45;
		}
		(*iter)->appearance()->invalidateStaticRegistration();
		(*iter)->appearance()->update();
		(*iter)->appearance()->registerStatic();
		iter++;
	}

	undoMgr.AddAction(pAction);
	pAction = nullptr;
	// A rotation is an edit: without this, close/open never prompts and the yaw is lost.
	if ( EditorData::instance )
		EditorData::instance->MissionNeedsSaving( true );
}

// Frame the camera on the current selection. Recenters the camera's ground
// anchor on the centroid of the selected objects' XY positions. Read-only:
// touches no object, pushes no undo action, does not mark the mission dirty.
// No-op when nothing is selected. setPosition() clamps the target to the map
// and derives the camera Z from terrain elevation (swoopy z-glide) — the same
// path every other editor camera move uses. (setGoalPosition() alone is inert
// here: updateGoalPosition() resets it to the current pos unless goalPosTime is
// primed, which the editor never does.)
void EditorInterface::frameSelectedObjects()
{
	if ( !eye )
		return;

	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if ( !mgr )
		return;

	EditorObjectMgr::EDITOR_OBJECT_LIST selectedObjects = mgr->getSelectedObjectList();
	glm::vec3 centroid;
	centroid.x = centroid.y = centroid.z = 0.0f;
	int count = 0;
	for ( auto iter = selectedObjects.begin();
	      iter != selectedObjects.end(); iter++ )
	{
		if ( !(*iter) )
			continue;
		const glm::vec3& p = (*iter)->getPosition();
		centroid.x += p.x;
		centroid.y += p.y;
		centroid.z += p.z;
		count++;
	}

	if ( count == 0 )
		return;  // no selection -> no-op

	float inv = 1.0f / (float)count;
	centroid.x *= inv;
	centroid.y *= inv;
	centroid.z *= inv;

	eye->setPosition( centroid, true );  // xy clamped to map, z terrain-locked
}

// Continuous-angle rotation of the current selection, used by the mouse wheel.
// Mirrors rotateSelectedObjects() but applies an arbitrary degree delta instead
// of the discrete 45/90 step, so the wheel can spin objects smoothly.
void EditorInterface::rotateSelectedObjectsDegrees( float deg )
{
	ModifyBuildingAction *pAction = new ModifyBuildingAction;
	EditorObjectMgr::EDITOR_OBJECT_LIST selectedObjects;
	selectedObjects = EditorObjectMgr::instance()->getSelectedObjectList();
	auto iter = selectedObjects.begin();
	while (iter != selectedObjects.end())
	{
		// appearance() can be NULL; skip.
		if ( !(*iter) || !(*iter)->appearance() )
		{
			iter++;
			continue;
		}
		pAction->addBuildingInfo(*(*iter));
		(*iter)->appearance()->rotation += deg;
		// Recompute transform + re-bake the static GPU recipe so static props
		// visibly rotate (mechs no-op the invalidate; see drag-move note).
		(*iter)->appearance()->invalidateStaticRegistration();
		(*iter)->appearance()->update();
		(*iter)->appearance()->registerStatic();
		iter++;
	}

	undoMgr.AddAction(pAction);
	pAction = nullptr;
	// A rotation is an edit: without this, close/open never prompts and the yaw is lost.
	if ( EditorData::instance )
		EditorData::instance->MissionNeedsSaving( true );
}

// Editable Inspector v1 transform. Reuses ModifyBuildingAction exactly like the
// drag-move (handleLeftButtonDown/Move) and rotateSelectedObjects paths: capture
// the old appearance snapshot, mutate the live appearance, re-bake the static GPU
// recipe, then note the new position so the action's location-based undo can find
// the object at its new pose. No new transform/undo system.
bool EditorInterface::applyObjectTransform( EditorObject* obj, float worldX, float worldY, float yawDegrees )
{
	if ( !obj || !obj->appearance() || !land )
		return false;

	// Forest-member trees are placed/owned by the forest; moving one individually
	// would desync forest bookkeeping. Out of scope for v1.
	if ( obj->getForestID() != -1 )
		return false;

	ModifyBuildingAction* pAction = new ModifyBuildingAction;
	pAction->addBuildingInfo( *obj );   // captures OLD snapshot + OLD position

	glm::vec3 newPos;
	newPos.x = worldX;
	newPos.y = worldY;
	newPos.z = obj->appearance()->position.z;

	// Cell/link bookkeeping (best-effort, mirrors drag-move which ignores the
	// return), then override with the precise free XY and terrain-locked Z.
	int row = 0, col = 0;
	land->worldToCell( newPos, row, col );
	EditorObjectMgr::instance()->moveBuilding( obj, row, col );

	ObjectAppearance* pApp = obj->appearance();
	newPos.z = land->getTerrainElevation( newPos );
	pApp->position = newPos;
	pApp->rotation = yawDegrees;
	pApp->invalidateStaticRegistration();
	pApp->update();
	pApp->registerStatic();

	// buildingIDs must hold the object's CURRENT position so the action's
	// getObjectAtLocation-based undo/redo can locate it (it was captured at the
	// OLD position by addBuildingInfo).
	pAction->updateNotedObjectPositions();

	undoMgr.AddAction( pAction );

	if ( EditorData::instance )
		EditorData::instance->MissionNeedsSaving( true );

	return true;
}

bool EditorInterface::openObjectContextMenu( int screenX, int screenY )
{
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	EditorObject* obj = mgr ? mgr->getObjectAtScreenPosition( screenX, screenY ) : nullptr;
	if ( !obj || !obj->appearance() || obj->getForestID() != -1 || !mgr->isAlignable( obj->getID() ) )
		return false;
	m_contextMenuObjectID = obj->getID();
	m_contextMenuObjectPos = obj->getPosition();
	m_contextMenuPending = true;
	return true;
}

// The menu keeps an ID and a position, not a pointer: undo/redo can recreate the object.
EditorObject* EditorInterface::contextMenuObject()
{
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if ( !mgr || m_contextMenuObjectID < 0 )
		return nullptr;
	EditorObject* obj = mgr->getObjectAtLocation( m_contextMenuObjectPos.x, m_contextMenuObjectPos.y );
	return obj && obj->getID() == m_contextMenuObjectID ? obj : nullptr;
}

bool EditorInterface::applyObjectAlignment( EditorObject* obj, int newTeam )
{
	if ( !obj || !obj->appearance() )
		return false;

	// Forest-member trees are owned by the forest; skip (mirrors applyObjectTransform).
	if ( obj->getForestID() != -1 )
		return false;

	// Valid team range is [-1,7] (EditorObject::setAlignment / its gosASSERT).
	if ( newTeam < -1 || newTeam > 7 )
		return false;

	// No-op if unchanged -- never push an empty undo entry.
	if ( obj->getAlignment() == newTeam )
		return false;

	ModifyBuildingAction* pAction = new ModifyBuildingAction;
	pAction->addBuildingInfo( *obj );   // snapshot captures the OLD teamId

	obj->setAlignment( newTeam );

	// Buildings are STATIC props whose team colour is baked into the GPU recipe,
	// so re-bake them. UNITS are dynamic movers -- the working UnitSettingsDlg
	// path just calls setAlignment with NO static re-bake; doing the static
	// invalidate/registerStatic on a unit corrupts it (it vanishes when
	// deselected). So skip the re-bake for units.
	if ( !dynamic_cast<Unit*>( obj ) )
	{
		ObjectAppearance* pApp = obj->appearance();
		pApp->invalidateStaticRegistration();
		pApp->update();
		pApp->registerStatic();
	}

	// Position is unchanged, but keep the action's location bookkeeping consistent
	// (undo locates the object by position).
	pAction->updateNotedObjectPositions();

	undoMgr.AddAction( pAction );

	if ( EditorData::instance )
		EditorData::instance->MissionNeedsSaving( true );

	return true;
}

int EditorInterface::runInspectorEditSmoke()
{
	if ( !land || !EditorObjectMgr::instance() )
		return -1;

	// Place a throwaway drop zone near map center (no catalog group/index needed).
	const int mid = ( land->realVerticesMapSide - 1 ) / 2;
	glm::vec3 p;
	land->cellToWorld( mid, mid, p );
	p.z = land->getTerrainElevation( p );

	EditorObject* obj = EditorObjectMgr::instance()->addDropZone( p, 0, false );
	if ( !obj || !obj->appearance() )
		return -1;

	// Exercise the real selection path too.
	EditorObjectMgr::instance()->unselectAll();
	EditorObjectMgr::instance()->select( *obj, true );

	const float ox = obj->getPosition().x;
	const float oy = obj->getPosition().y;

	bool applied = applyObjectTransform( obj, ox + 64.0f, oy + 64.0f, 90.0f );
	const float nx = obj->getPosition().x;
	const float ny = obj->getPosition().y;
	const bool moved = applied && ( fabsf( nx - ox ) > 1.0f || fabsf( ny - oy ) > 1.0f );

	int result = moved ? 1 : 0;

	// Undo through the SAME manager the transform pushed to (member undoMgr).
	if ( undoMgr.HaveUndo() )
	{
		undoMgr.Undo();
		const float ux = obj->getPosition().x;
		const float uy = obj->getPosition().y;
		if ( fabsf( ux - ox ) < 1.0f && fabsf( uy - oy ) < 1.0f )
			result |= 2;
	}

	const int oTeam = obj->getAlignment();
	const int nTeam = ( oTeam == 1 ) ? 2 : 1;   // a different valid team
	if ( applyObjectAlignment( obj, nTeam ) && obj->getAlignment() == nTeam )
	{
		result |= 4;
		if ( undoMgr.HaveUndo() )
		{
			undoMgr.Undo();
			if ( obj->getAlignment() == oTeam )
				result |= 8;
		}
	}

	return result;
}

// -smoke-place-oob: reproduce (and now guard) the off-map placement crash.
// Activates a real BuildingBrush and drives its update() at off-map screen
// points (top edge = horizon -> projects off any finite map; corners + extreme
// out-of-viewport coords cover the OOB cell range). Before the BuildingBrush
// worldToCell clamp, the first off-map point read the heightmap out of bounds
// in MapData::terrainElevation (0xC0000005). Returns 1 if all updates survived,
// -1 if setup failed (no terrain / catalog / camera).
int EditorInterface::runPlaceOobSmoke()
{
	if ( !land )
		return -1;

	// terrainElevation() guards only the UPPER cell bound; a position WEST/NORTH
	// of the map yields a negative mesh offset and reads blocks[<0] out of bounds
	// (0xC0000005). The interactive crash was exactly this. Drive far-off-map
	// world positions through the production guard BuildingBrush::snapToTerrainCell
	// (the same call BuildingBrush::update makes). Without the clamp these crash;
	// with it they snap to an in-range edge cell.
	const float wupv = Terrain::worldUnitsPerVertex;
	const float farW = land->mapTopLeft3d.x - 100000.0f * wupv; // far west  -> cc << 0
	const float farN = land->mapTopLeft3d.y + 100000.0f * wupv; // far north -> cr << 0
	const float farE = land->mapTopLeft3d.x + 100000.0f * wupv; // far east  -> cc >> max
	const float farS = land->mapTopLeft3d.y - 100000.0f * wupv; // far south -> cr >> max

	const float px[] = { farW, land->mapTopLeft3d.x, farW, farE, farW };
	const float py[] = { land->mapTopLeft3d.y, farN, farN, farS, farS };

	// Postcondition check (deterministic — the raw OOB read only *faults* on an
	// unmapped page, which is heap-dependent and unreliable to trigger headless).
	// After snapToTerrainCell, the result MUST land on a valid in-grid cell. Run
	// it back through worldToCell and assert the indices are in range. Without the
	// clamp the snapped position is still off-map -> out-of-range -> returns 0.
	const int maxCell = (int)( ( Terrain::realVerticesMapSide - 1 ) * 3 ) - 1;
	if ( maxCell < 0 )
		return -1;

	for ( int i = 0; i < (int)( sizeof( px ) / sizeof( px[0] ) ); ++i )
	{
		glm::vec3 p;
		p.x = px[i];
		p.y = py[i];
		p.z = 0.0f;
		BuildingBrush::snapToTerrainCell( land, p );  // production guard under test

		int cr = 0, cc = 0;
		land->worldToCell( p, cr, cc );
		if ( cr < 0 || cc < 0 || cr > maxCell || cc > maxCell )
			return 0;  // guard FAILED: snapped result is still off the cell grid
	}

	return 1; // every off-map input snapped back onto the valid cell grid
}

//-------------------------------------------------------------------------------------------------
// PCG foliage commands. Generate runs the Python generator's foliage pass
// on the current generated-map recipe and loads the resulting sidecar; the rest
// are pure overlay-state toggles. None touch terrain/save/load.
//-------------------------------------------------------------------------------------------------
void EditorInterface::OnFoliageGenerate()
{
	extern bool g_cliSuppressModals;   // smoke/headless: skip the modal feedback below
	// The Map Generator dialog writes genmap_recipe.json; the -gen-map / Generate Mission path leaves the
	// resolved recipe as genmap.recipe.json. Either one drives the foliage pass.
	const char* recipe = "terrain_gen_out\\genmap_recipe.json";
	if ( GetFileAttributes( recipe ) == INVALID_FILE_ATTRIBUTES )
		recipe = "terrain_gen_out\\genmap.recipe.json";
	if ( GetFileAttributes( recipe ) == INVALID_FILE_ATTRIBUTES )
	{
		if (!g_cliSuppressModals) AfxMessageBox( "No generated-map recipe found.\nUse the Map Generator first, then Generate Foliage." );
		return;
	}
	char cmd[1024];
	sprintf( cmd,
		"py -3 tools\\terrain_gen\\terrain_gen.py \"%s\" --out terrain_gen_out "
		"--generate-foliage --foliage-rules tools\\terrain_gen\\recipes\\foliage_rules_example.json",
		recipe );
	int rc = EditorTaskRunner::RunBlockingHidden( cmd, "terrain_gen_out\\terrain_gen.log" );
	if ( rc != 0 )
	{
		if (!g_cliSuppressModals) AfxMessageBox( "Foliage generation failed (output: terrain_gen_out\\terrain_gen.log).\nNeeds Python 3 + tools\\terrain_gen on the editor's working dir." );
		return;
	}
	if ( FoliageRender::Load( "terrain_gen_out\\genmap.foliage.json" ) )
	{
		char msg[128];
		sprintf( msg, "Foliage generated: %d instances.", FoliageRender::Count() );
		if (!g_cliSuppressModals) AfxMessageBox( msg );
	}
	else
	{
		if (!g_cliSuppressModals) AfxMessageBox( "Foliage generated but no instances were placed (check rules / map)." );
	}
}

void EditorInterface::OnFoliageRegenSel()
{
	// v1: selected-superchunk regeneration is not yet wired; regenerate the full
	// foliage set deterministically (same per-superchunk seeds -> stable result).
	OnFoliageGenerate();
}

void EditorInterface::OnFoliageClear()
{
	FoliageRender::Clear();
}

void EditorInterface::OnFoliageToggle()
{
	FoliageRender::Toggle();
}

void EditorInterface::OnForestTool()
{
	if (EditorInterface::instance()->ObjectSelectOnlyMode())
	{
		return;
	}

	ForestDlg dlg;

	long count = 0;
	float xAvg = 0;
	float yAvg = 0;
	

	for ( int j = 0; j < land->realVerticesMapSide; ++j )
	{
		for ( int i = 0; i < land->realVerticesMapSide; ++i )
		{
			if ( land->isVertexSelected( j, i ) )
			{
				glm::vec3 pos;
				land->tileCellToWorld( j, i, 0, 0, pos );

				xAvg += pos.x;
				yAvg += pos.y;
				count++;
			}
		}
	}

	// The forest tool scatters trees over the SELECTED terrain vertices. With no
	// selection there is nothing to plant (and the centre below divides by zero) --
	// tell the user instead of silently doing nothing.
	if ( count == 0 )
	{
		MessageBox(
			_T("Select a terrain area first (drag-select with the Area Select tool), then choose Other > Forests."),
			_T("Forest"), MB_OK | MB_ICONINFORMATION );
		return;
	}

	float centerX = xAvg/count;
	float centerY = yAvg/count;

	float dist = 0;
	
	for ( int j = 0; j < land->realVerticesMapSide; ++j )
	{
		for ( int i = 0; i < land->realVerticesMapSide; ++i )
		{
			if ( land->isVertexSelected( j, i ) )
			{
				glm::vec3 pos;
				land->tileCellToWorld( j, i, 0, 0, pos );

				float deltaX = pos.x - centerX;
				float deltaY = pos.y - centerY;

				float tmpDist = deltaX * deltaX + deltaY * deltaY;
				if ( tmpDist > dist )
					dist = tmpDist;
			}
		}
	}

	dlg.m_xLoc = centerX;
	dlg.m_yLoc = centerY;
	dist += 128 * 128; // always make as big as one tile
	dlg.m_radius = (float)sqrt( dist );

	if ( IDOK == dlg.DoModal() )
	{
		long forestID = EditorObjectMgr::instance()->createForest( dlg.forest );
		// Register an undo action. createForest assigns the real ID; fetch the
		// live Forest back so ForestAction snapshots it with that ID.
		const long kMaxForests = 256;
		Forest* forestPtrs[kMaxForests];
		long count = kMaxForests;
		EditorObjectMgr::instance()->getForests( forestPtrs, count );
		for ( long i = 0; i < count; ++i )
		{
			if ( forestPtrs[i] && forestPtrs[i]->getID() == forestID )
			{
				ActionUndoMgr::instance->AddAction( new ForestAction( *forestPtrs[i] ) );
				break;
			}
		}
	}

}

void EditorInterface::OnOtherEditforests() 
{
	if (EditorInterface::instance()->ObjectSelectOnlyMode())
	{
		return;
	}

	EditForestDlg dlg;
	dlg.DoModal();
}
