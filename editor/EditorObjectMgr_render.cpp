// EditorObjectMgr: per-frame update and render, building appearance lookup/priming and static-prop registration.
// Declared in the class headers; shared names in EditorObjectMgr_internal.h.
#include "EditorObjectMgr_internal.h"
#include "Gates/gate.h"
#include "../GameOS/gameos/static_caster_set.h"   // the shadow bake's caster set


// --------------------------------------------------------------------------
// GV-slide diagnostics + fix knobs (editor-only).
//
// Placed ground-vehicles (VEHICLE_APPR_TYPE) ride GpuStaticPropBatcher, which
// draws them every frame with the LIVE terrain MVP while reading each GV's
// last-baked shapeToWorld (gvactor.cpp TransformMultiShape, run only from
// GVAppearance::update()->updateGeometry()).  The editor gated that refresh
// behind recalcBounds()==inView, so a cluster of GVs swinging to the frustum
// edge on camera rotate skipped update() and rendered with a stale,
// terrain-conformed transform -> the group slid off the ground.  Buildings
// don't (static, no per-frame terrain conform); mechs use a different batcher.
//
// MC2_EDITOR_MOVER_REFRESH (default-ON): refresh placed-mover transforms every
//   frame regardless of inView, matching what BldgAppearance already does.
//   Set =0 to restore the legacy inView-gated behavior (bisect / A-B).
static bool editorMoverRefreshOn()
{
	return true;
}

void EditorObjectMgr::update()
{
	static long homeRelations[9] = {0, 0, 2, 1, 1, 1, 1, 1, 1};
	if (renderObjects)
	{
		for ( auto iter = buildings.begin();
			iter != buildings.end(); iter++ )
			{
				currentFloatHelp = 0;
				ObjectAppearance* pBAppr = (*iter) ? (*iter)->appearance() : nullptr;
				if (!pBAppr) continue;
				if (renderTrees || (!renderTrees && (pBAppr->getAppearanceClass() != TREE_APPR_TYPE)))
				{
					if ( pBAppr->recalcBounds() )
					{
						glm::vec3 pos = pBAppr->position;
						// Keep the building level with the terrain WITHOUT snapping it to the
						// cell grid. The legacy path called moveBuilding(worldToCell(pos)) here,
						// which re-derived x,y from the cell CENTRE every frame -- making free
						// (sub-cell) placement impossible (a dragged building snapped straight
						// back to tile intersections). We only need to refresh z to the terrain
						// height at the building's (possibly sub-cell) x,y.
						pos.z = land->getTerrainElevation(pos);
						pBAppr->position = pos;
						gosASSERT((8 > pBAppr->teamId) && (-1 <= pBAppr->teamId));
						pBAppr->setObjectParameters(pos, pBAppr->rotation, pBAppr->selected, pBAppr->teamId, homeRelations[pBAppr->teamId+1]);
						pBAppr->update();
						pBAppr->setVisibility(true,true);
					}
				}
			}
	}

		const bool moverRefresh = editorMoverRefreshOn();
		for ( auto mIter = units.begin();
		mIter != units.end(); mIter++ )
		{
			EditorObject* pObj = (*mIter);
			currentFloatHelp = 0;
			ObjectAppearance* pUAppr = pObj ? pObj->appearance() : nullptr;
			if (!pUAppr) continue;
			const bool uInView = pUAppr->recalcBounds();
			// GV-slide fix: refresh the mover transform every frame (not only when
			// inView), so static-prop-batched GVs never render with a shapeToWorld
			// stale from a frame they swung off the rotating frustum.  Visibility is
			// still only asserted when actually in view.
			if ( uInView || moverRefresh )
			{
				// Vehicles and mech forced to ground in Object classes in game now.
				// NOT in the appearance anymore since they can fly when they die!
				// Must force here as well.
				glm::vec3 pos = pUAppr->position;
				pos.z = land->getTerrainElevation(pos);
				if (getIsHoverCraft(pObj->id)) {
					float waterElevation = land->getWaterElevation();
					if (waterElevation > pos.z) {
						pos.z = waterElevation;
					}
				}
				gosASSERT((8 > pUAppr->teamId) && (-1 <= pUAppr->teamId));
				pUAppr->setObjectParameters(pos, pUAppr->rotation, pUAppr->selected, pUAppr->teamId, homeRelations[pUAppr->teamId+1]);
				if (pObj->getSpecialType() == HELICOPTER)
					pUAppr->setMoverParameters(0.0f,0.0f,0.0f,true);
				pUAppr->update();
				if ( uInView )
					pUAppr->setVisibility(true,true);
			}
		}
}

void EditorObjectMgr::render()
{
	// S2.5 (2026-05-25): actor walk only. Frame-level orchestration
	// (renderLists, MVP, batcher flush, terrain dispatch) lives in the
	// canonical game render-world chain wired by S2 -- DO NOT re-add here.
	// Each appearance()->render() enqueues into the static-prop / mech
	// batcher; the flush happens later via mc2::VkSceneRenderer::renderLists(mc2::renderSubmissions()).
	// See docs/superpowers/plans/2026-05-25-editor-rebuild.md S2 followup.

	// the editor registers static props (registerStatic
	// arms needsFullBakeNextFrame) but, unlike the game's Mission::init -> objmgr
	// prewarmStaticPropLightBakes, NEVER drains the H4 latch. A static editor scene
	// never re-bakes in-view, so a prop's per-instance lightDataIndex is captured by
	// markVisible() while still stale (latch armed / slot not yet baked) and, being
	// static, is never re-captured -> STABLE wrong light on the same few props:
	// RED when the stale slot aliases a neighbour's live row (e.g. an LRM unit's
	// pulsing highlight), BLACK when it lands on a zero/empty slot. Drain the latch
	// here -- once the camera has lights and BEFORE the buildings/trees render loop
	// below captures slots -- by baking each latched prop's correct light slot
	// (prewarmStaticLightBake sets cachedGpuLightIndex + clears the latch,
	// bdactor.cpp:4131-4135). Retries each frame until lights are up and all latched
	// props are baked; then one-shot done.
	{
		static bool s_prewarmDone = false;
		if (!s_prewarmDone && eye && eye->getNumLights() > 0)
		{
			int attempted = 0, baked = 0, remaining = 0;
			for (auto it = buildings.begin(); it != buildings.end(); it++)
			{
				EditorObject* o = *it;
				if (!o || !o->appearance()) continue;
				ObjectAppearance* a = o->appearance();
				if (!a->isStaticRegistered() || !a->needsPrewarmBake()) continue;
				++attempted;
				if (a->prewarmStaticLightBake(eye)) ++baked;
			}
			for (auto it = buildings.begin(); it != buildings.end(); it++)
			{
				EditorObject* o = *it;
				if (o && o->appearance() && o->appearance()->needsPrewarmBake()) ++remaining;
			}
			if (remaining == 0) s_prewarmDone = true;
			fprintf(stderr, "[EDITOR_STATIC_PREWARM] attempted=%d baked=%d remaining=%d done=%d\n",
			        attempted, baked, remaining, s_prewarmDone ? 1 : 0);
			fflush(stderr);
		}
	}

	static long s_emrFrame = 0;
	++s_emrFrame;
	const bool s_emrLog = (s_emrFrame == 1 || s_emrFrame == 5 || s_emrFrame == 30 || s_emrFrame == 120);
	if (s_emrLog)
		EditorObjMgrTrace("EditorObjectMgr::render frame=%ld renderObjects=%d renderTrees=%d buildings=%ld units=%ld links=%ld dropZones=%ld",
			s_emrFrame, renderObjects ? 1 : 0, renderTrees ? 1 : 0,
			static_cast<long>(buildings.size()), static_cast<long>(units.size()),
			static_cast<long>(links.size()), static_cast<long>(dropZones.size()));

	long bTreeFiltered = 0;
	long bRecalcPassed = 0;
	long bRendered = 0;
	if (renderObjects)
	{
		if (s_emrLog)
			EditorObjMgrTrace("EditorObjectMgr::render frame=%ld buildings-loop enter count=%ld",
				s_emrFrame, static_cast<long>(buildings.size()));
		for ( auto iter = buildings.begin();
			iter != buildings.end(); iter++ )
		{
			currentFloatHelp = 0;
			const bool passTreeFilter =
				renderTrees || (!renderTrees && ((*iter)->appearance()->getAppearanceClass() != TREE_APPR_TYPE));
			if (passTreeFilter)
			{
				++bTreeFiltered;
				const bool rb = (*iter)->appearance()->recalcBounds();
				if ( rb )
				{
					++bRecalcPassed;
					if ( (*iter)->getDamage() )
						(*iter)->appearance()->drawBars();
					// BldgAppearance::render() submits into the static-prop batcher's
					// per-frame buckets via submitMultiShape() (bdactor.cpp).
					// Flush is issued later from mc2::VkSceneRenderer::renderLists(mc2::renderSubmissions()).
					(*iter)->appearance()->render();
					++bRendered;
					if ( (*iter)->isSelected() )
						(*iter)->appearance()->drawSelectBrackets( 0xffffff00 );  // selection highlight (yellow)
					else if ( (*iter)->getColor() & 0xff000000 )
						(*iter)->appearance()->drawSelectBrackets( (*iter)->getColor() );
				}
			}
		}
		if (s_emrLog)
			EditorObjMgrTrace("EditorObjectMgr::render frame=%ld buildings-loop exit treeFiltered=%ld recalcPassed=%ld rendered=%ld",
				s_emrFrame, bTreeFiltered, bRecalcPassed, bRendered);
	}
	else if (s_emrLog)
	{
		EditorObjMgrTrace("EditorObjectMgr::render frame=%ld buildings-loop SKIPPED (renderObjects=false)", s_emrFrame);
	}

	long uRecalcPassed = 0;
	long uRendered = 0;
	if (s_emrLog)
		EditorObjMgrTrace("EditorObjectMgr::render frame=%ld units-loop enter count=%ld",
			s_emrFrame, static_cast<long>(units.size()));
	for ( auto mIter = units.begin();
		mIter != units.end(); mIter++ )
	{
		EditorObject* pObj = (*mIter);
		currentFloatHelp = 0;
		// Capture recalcBounds() ONCE (it has LOD-swap / inView side effects;
		// calling it twice would re-swap and re-invalidate the baked transform).
		const bool rInView = pObj->appearance()->recalcBounds();
		if ( rInView )
		{
			++uRecalcPassed;
			// Mech3DAppearance::render() submits into the mech batcher via
			// submitActor() (mech3d.cpp). Flush is via renderLists().
			pObj->appearance()->render();
			++uRendered;
			if ( (*mIter)->getDamage() )
				pObj->appearance()->drawBars();
			if ( (*mIter)->isSelected() )
				pObj->appearance()->drawSelectBrackets( 0xffffff00 );  // selection highlight (yellow)
			else if ( (*mIter)->getColor() & 0xff000000 )
				pObj->appearance()->drawSelectBrackets( pObj->getColor() );
		}
	}
	if (s_emrLog)
		EditorObjMgrTrace("EditorObjectMgr::render frame=%ld units-loop exit recalcPassed=%ld rendered=%ld",
			s_emrFrame, uRecalcPassed, uRendered);

	// Building links -- 2D overlay, NOT batcher scope.
	for ( auto lIter = links.begin();
		lIter != links.end(); lIter++ )
	{
		(*lIter)->render();
	}

	// Drop zones -- 2D overlay, NOT batcher scope.
	for ( auto dIter = dropZones.begin();
		dIter != dropZones.end(); dIter++ )
	{
		glm::vec3 pos = (*dIter)->getPosition();
		Stuff::Vector4D screen;

		// Modern zoom-correct projection — legacy projectZ X-collapses on zoom.
		if ( !EditorObjectMgr_ProjectScreenXY_GL( pos, screen ) )
			continue;  // behind near plane -> skip marker

		GUI_RECT Rect;
		Rect.top = screen.y - 3;
		Rect.left = screen.x - 3;
		Rect.bottom = screen.y + 3;
		Rect.right = screen.x + 3;
		drawRect( Rect, (*dIter)->isVTol() ? 0xff0000ff : 0xffffff00 );
	}
}

ObjectAppearance* EditorObjectMgr::getAppearance( EditorObjectMgr::Building* pBuilding )
{
	ObjectAppearance* appearance = nullptr;

	if ( !appearanceTypeList )
	{
		appearanceTypeList = new AppearanceTypeList;
		gosASSERT(appearanceTypeList != nullptr);
	}

	//MUST ALWAYS CALL GET, EVEN IF WE HAVE AN APPEARANCE TYPE OR REFERENCE COUNT DOES NOT INCREASE!
	pBuilding->appearanceType = appearanceTypeList->getAppearance( pBuilding->type << 24, pBuilding->fileName );
	
	if ( pBuilding->type == BLDG_TYPE )
		appearance = new BldgAppearance;
	
	else if ( pBuilding->type == MECH_TYPE )
		appearance = new Mech3DAppearance;
		
	else if ( pBuilding->type == GV_TYPE )
		appearance = new GVAppearance;
	
	else
		appearance = new TreeAppearance;


	appearance->init( pBuilding->appearanceType );
	appearance->setObjectNameId( pBuilding->nameID );

	return appearance;

}

ObjectAppearance* EditorObjectMgr::getAppearance( unsigned long group, unsigned long indexWithinGroup )
{
	gosASSERT( group >= 0 && group < static_cast<int>(groups.size()) );

	Group* pGroup = &groups[group];

	gosASSERT( static_cast<int>(pGroup->buildings.size()) > indexWithinGroup );
	Building* pBuilding = &pGroup->buildings[indexWithinGroup];

	return getAppearance( pBuilding );

}

// Pre-loads every building AppearanceType from the object catalogue and
// registers its TG_TypeMultiShape(s) with GpuStaticPropBatcher so that all
// object types are available before finalizeGeometry() is called.  Without
// this, any object type not already present on the loaded map hits the
// late-register path (registerType after s_geometryFinalized == true) and
// renders solid black when first placed.
//
// Bridges and other rarely-used building types are the most common victims
// because they are absent from most stock starter maps.
void EditorObjectMgr::primeAllBuildingAppearanceTypes()
{
	EditorObjMgrTrace("EditorObjectMgr::primeAllBuildingAppearanceTypes enter groups=%ld",
		(long)static_cast<int>(groups.size()));

	if (!appearanceTypeList)
	{
		appearanceTypeList = new AppearanceTypeList;
		gosASSERT(appearanceTypeList != nullptr);
	}

	long nPrimed = 0;
	for (auto groupIter = groups.begin(); groupIter != groups.end(); groupIter++)
	{
		Group& grp = *groupIter;
		for (auto bldgIter = grp.buildings.begin(); bldgIter != grp.buildings.end(); bldgIter++)
		{
			Building& bldg = *bldgIter;

			if (bldg.type != BLDG_TYPE && bldg.type != TREED_TYPE)
				continue;

			// Each cached appearance still needs its reference count incremented.
			bldg.appearanceType = appearanceTypeList->getAppearance(
				bldg.type << 24, bldg.fileName);

			if (!bldg.appearanceType)
				continue;

			if (bldg.type == BLDG_TYPE)
			{
				BldgAppearanceType* bat = static_cast<BldgAppearanceType*>(bldg.appearanceType);
				for (int lod = 0; lod < MAX_LODS; ++lod)
				{
					EditorBridge::registerStaticPropShape(bat->bldgShape[lod].get());
					// Render overrides and their textures must be resident before finalization.
					TG_TypeMultiShape* bRenderShape = bat->getBldgRenderShape(lod);
					EditorBridge::registerStaticPropShape(bRenderShape);
					if (bRenderShape)
						Bldg_ForceRenderShapeTexturesResident(bRenderShape);
				}
				EditorBridge::registerStaticPropShape(bat->bldgDmgShape.get());
				++nPrimed;
			}
			else
			{
				TreeAppearanceType* tat = static_cast<TreeAppearanceType*>(bldg.appearanceType);
				const int lodCount = (tat->treeRenderShapeLodCount > 0)
				                       ? (int)tat->treeRenderShapeLodCount : MAX_LODS;
				for (int lod = 0; lod < lodCount; ++lod)
				{
					// Overrides have more LOD slots than the stock shape array.
					if (lod < MAX_LODS)
						EditorBridge::registerStaticPropShape(tat->treeShape[lod].get());
					else
						EditorObjMgrTrace("primeAllBuildingAppearanceTypes: skip stock shape name=%s lod=%d stockLods=%d",
							bldg.fileName, lod, MAX_LODS);
					TG_TypeMultiShape* tRenderShape = tat->getTreeRenderShape(lod);
					EditorBridge::registerStaticPropShape(tRenderShape);
					if (tRenderShape)
						Bldg_ForceRenderShapeTexturesResident(tRenderShape);
				}
				EditorBridge::registerStaticPropShape(tat->treeDmgShape.get());
				++nPrimed;
			}
		}
	}

	EditorObjMgrTrace("EditorObjectMgr::primeAllBuildingAppearanceTypes exit nPrimed=%ld", nPrimed);
}

EditorObject* EditorObjectMgr::findObjectByStaticRecipeIndex(int32_t recipeIndex)
{
	if (recipeIndex < 0)
		return nullptr;

	for (auto iter = buildings.begin(); iter != buildings.end(); iter++)
	{
		EditorObject* obj = *iter;
		if (!obj || !obj->appearance())
			continue;
		if (obj->appearance()->getStaticRecipeIndex() == recipeIndex)
			return obj;
	}
	for (auto mIter = units.begin(); mIter != units.end(); mIter++)
	{
		Unit* unit = *mIter;
		if (!unit || !unit->appearance())
			continue;
		if (unit->appearance()->getStaticRecipeIndex() == recipeIndex)
			return unit;
	}
	return nullptr;
}

void EditorObjectMgr::registerStaticPropsForMissionLoad()
{
	if (!EditorBridge::staticPropMissionLoadRegEnabled())
		return;
	static const long homeRelations[9] = {0, 0, 2, 1, 1, 1, 1, 1, 1};
	int total = 0, registered = 0;
	for (auto iter = buildings.begin(); iter != buildings.end(); iter++)
	{
		EditorObject* obj = *iter;
		if (!obj || !obj->appearance())
			continue;
		++total;
		ObjectAppearance* app = obj->appearance();
		int teamIdx = (app->teamId + 1 >= 0 && app->teamId + 1 < 9) ? app->teamId + 1 : 0;
		app->setObjectParameters(app->position, app->rotation, app->selected,
		                         app->teamId, homeRelations[teamIdx]);
		app->registerStatic();
		if (app->isStaticRegistered())
			++registered;
	}
	// The shadow bake's caster set: every registered building / tree / prop, as GameObjectManager::buildStaticShadowCasterSet does in the game.
	{
		mc2shadow::StaticCasterSet& set = mc2shadow::staticCasterSet();
		set.beginRebuild();
		uint32_t handle = 1;
		for (auto iter = buildings.begin(); iter != buildings.end(); iter++)
		{
			EditorObject* obj = *iter;
			if (!obj || !obj->appearance())
				continue;
			const Appearance* app = obj->appearance();
			const int32_t recipe = app->getShadowCasterRecipeIndex();
			const int special=getSpecialType(obj->getID());
			if (recipe < 0 && special!=EDITOR_TURRET)
				continue;
			mc2shadow::StaticCasterEntry e;
			e.owner = app;
			e.gameHandle = handle++;
			e.recipeIndex = recipe;
			e.kind=special==EDITOR_TURRET?mc2shadow::CasterKind::Turret:
				special==EDITOR_GATE?mc2shadow::CasterKind::Gate:mc2shadow::CasterKind::TerrainObject;
			set.add(e);
		}
		set.endRebuild();
		// Only addBuilding admits a new scene owner; brush preview recipes do not bake.
		set.setAdmitNewOwners(false);
	}
	MC2_DIAGFE(stderr, "[STATIC_PROP_REG v1] event=editor_load enumerated=%d registered=%d skipped=%d\n",
	        total, registered, total - registered);
	fflush(stderr);
}
