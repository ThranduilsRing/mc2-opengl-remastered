// GameObjectManager: object / mover lookup by id, cell and mouse, and the terrain-object pick.
// Declared in the class headers; shared names in objmgr_internal.h.
#include "objmgr_internal.h"


//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObject (Stuff::Vector3D position) {

	float closestDistance = 10000.0;
	GameObjectPtr closestObj = NULL;
	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++) {
		GameObjectPtr obj = objList[objIndex];
		Assert(obj != NULL, objIndex, " GameObjectManager.findObject: NULL obj ");
		if (obj->getExists() && !obj->inTransport()) {
			float distanceFromObject = obj->distanceFrom(position);
			if (distanceFromObject < closestDistance)
				if (distanceFromObject < obj->getExtentRadius()) {
					closestObj = obj;
					closestDistance = distanceFromObject;
				}
		}
	}

	return(closestObj);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObjectByTypeHandle (long typeHandle) {

	//-------------------------------------------------
	// This function was called findObjectId() in MC...

	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++) {
		GameObjectPtr obj = objList[objIndex];
		if (obj->getExists() && (obj->getTypeHandle() == typeHandle))
			return(obj);
	}
	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByPartId (long partId) {

	if (partId == 0)
		return(NULL);

	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++) 
	{
		GameObjectPtr obj = objList[objIndex];
		if (obj && obj->getExists() && (obj->getPartId() == partId))
			return(obj);
	}
	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByBlockVertex (long blockNum, long vertex) {

	//----------------------------------------
	// SLOW compared to using object handle...
	long partId = calcPartId(TERRAINOBJECT, blockNum, vertex);
	return(findByPartId(partId));
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByCellPosition (long row, long col) 
{
	//-------------------------------------------------------------------------------------
	// Must implement for Linkage code.  10/20/99 -fs
	// PLEASE DO NOT CALL EVERY FRAME. THIS ONE WILL BE SLOWER THEN CHRISTMAS!!!!!!!!!!!!
	// Store the pointer if at all possible.  YOU HAVE BEEN WARNED!
	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++)
	{
		GameObjectPtr obj = objList[objIndex];
		if (obj->getExists())
		{
			int cellR, cellC;
			objList[objIndex]->getCellPosition(cellR,cellC);

			if ((cellR == row) && (cellC == col))
				return objList[objIndex];
		}
	}

	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByUnitInfo (long commander, long group, long mate) {

	//----------------------------------------
	// SLOW compared to using object handle...
	long partId = calcPartId(MOVER, commander, group, mate);
	return(findByPartId(partId));
}

//---------------------------------------------------------------------------

// Lazy pick-time screen projection. Replaces the per-frame recalcBounds
// projection body (deleted 2026-05-18, Task 2/3, commits 4294937/d5c5546/
// 69e7968/d511ad9) for the ONLY surviving consumer: mouse-pick (per-click
// over already-cull-narrowed active blocks, not per-frame). Populates a
// local screen rect; PerPolySelect (geometry-space, mclib/bdactor.cpp,
// survives) does the precise hit-test unchanged. Typed BldgAppearance* --
// Appearance* has no position/getPosition; position/rotation are public on
// ObjectAppearance, typeUpperLeft/typeLowerRight on appearType.
//
// Byte-equivalence note: the deleted recalcBounds block's FINAL
// upperLeft/lowerRight (the values pick consumed) were ENTIRELY the
// 8-corner projected min/max -- the screenPos-seeded values it computed
// first were unconditionally overwritten by the corner min/max loop
// (pre-delete mclib/bdactor.cpp lines 1396-1399, rev 4294937^). This
// helper therefore seeds the rect from corner[0] (matching the pre-delete
// `if(!i){maxX=minX=bcsp[i].x;...}` init) and does NOT seed from a
// projection of `position`, so the resulting rect is byte-identical to
// the old screen rect. The 8-corner boxCoords construction below is
// lifted VERBATIM from `git show 4294937^:mclib/bdactor.cpp` lines
// 1288-1399 (the deleted `if (inView)` projection block); do not
// "improve" the box geometry -- pick must remain byte-equivalent or
// building selection silently shifts/misses.
static bool projectPickCandidateRect(BldgAppearance* ba,
                                     long& outMinX, long& outMinY,
                                     long& outMaxX, long& outMaxY)
{
	if (!ba || !eye)
		return false;

	// appearType is the public BldgAppearanceType* member (mclib/bdactor.h
	// :192); typeUpperLeft/typeLowerRight live on the AppearanceType base
	// (mclib/apprtype.h:52-53).
	BldgAppearanceType* appearType = ba->appearType;
	if (!appearType)
		return false;

	const Stuff::Vector3D& position = ba->position;
	float rotation = ba->rotation;

	Stuff::Vector3D boxCoords[8];
	Stuff::Vector4D bcsp[8];

	Stuff::Vector3D boxStart;
	mc2ToRender(boxStart, appearType->typeUpperLeft);

	Stuff::Vector3D boxEnd;
	mc2ToRender(boxEnd, appearType->typeLowerRight);

	Stuff::Vector3D addCoords;

	addCoords.x = boxStart.x;
	addCoords.y = boxStart.y;
	addCoords.z = boxEnd.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[0].Add(position,addCoords);

	addCoords.x = boxStart.x;
	addCoords.y = boxEnd.y;
	addCoords.z = boxEnd.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[1].Add(position,addCoords);

	addCoords.x = boxEnd.x;
	addCoords.y = boxEnd.y;
	addCoords.z = boxEnd.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[2].Add(position,addCoords);

	addCoords.x = boxEnd.x;
	addCoords.y = boxStart.y;
	addCoords.z = boxEnd.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[3].Add(position,addCoords);

	addCoords.x = boxStart.x;
	addCoords.y = boxStart.y;
	addCoords.z = boxStart.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[4].Add(position,addCoords);

	addCoords.x = boxEnd.x;
	addCoords.y = boxStart.y;
	addCoords.z = boxStart.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[5].Add(position,addCoords);

	addCoords.x = boxEnd.x;
	addCoords.y = boxEnd.y;
	addCoords.z = boxStart.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[6].Add(position,addCoords);

	addCoords.x = boxStart.x;
	addCoords.y = boxEnd.y;
	addCoords.z = boxStart.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[7].Add(position,addCoords);

	float maxX = 0.0f, maxY = 0.0f;
	float minX = 0.0f, minY = 0.0f;

	// INPUT-CURSOR-OFFSCREEN-TARGET-RECON-1 / -FIX-1: a near-plane-clipped OBB corner
	// projects to (0,0) with w<=1e-4 (camera.h:860-862). Including it in the screen-rect
	// min/max stretches the rect to the screen origin, so an object whose lower corners
	// sit below terrain (and thus behind the near plane at low camera angles) yields a
	// huge bogus rect that swallows the cursor -> wrong/offscreen object wins the pick.
	// PROVEN in TangoMaster m1: behind_corners=4, pos.z=48.7 (terrain ~325), rect
	// [-3369,-3759,1675,809] contained a cursor 436px away.
	// FIX: build the rect from in-front corners only (skip w<=1e-4). If ALL corners are
	// behind the camera the object has no valid screen rect this frame -> not pickable.
	// Killswitch MC2_PICK_BEHIND_LEGACY=1 restores the byte-identical legacy behavior.
	static const bool s_pickBehindLegacy = mc2::gate::get<"MC2_PICK_BEHIND_LEGACY">();
	int behindCount = 0;

	for (long i=0;i<8;i++)
	{
		eye->projectForScreenXY(boxCoords[i],bcsp[i]);
		if (bcsp[i].w <= 1e-4f)
			++behindCount;
		if (!i)
		{
			maxX = minX = bcsp[i].x;
			maxY = minY = bcsp[i].y;
		}

		if (i)
		{
			if (bcsp[i].x > maxX)
				maxX = bcsp[i].x;

			if (bcsp[i].x < minX)
				minX = bcsp[i].x;

			if (bcsp[i].y > maxY)
				maxY = bcsp[i].y;

			if (bcsp[i].y < minY)
				minY = bcsp[i].y;
		}
	}

	// Pre-delete recalcBounds wrote these as upperLeft.x=minX, upperLeft.y
	// =minY, lowerRight.x=maxX, lowerRight.y=maxY (rev 4294937^ lines
	// 1396-1399); the pick test then compared (float)upperLeft/lowerRight.
	// The float->long narrowing here is the only representational change;
	// (long)(float screen coord) truncates toward zero, and the pick
	// comparison below is integer mouseX/mouseY against the rect, so the
	// hit set is byte-equivalent for all on-screen rects.
	outMinX = (long)minX;
	outMinY = (long)minY;
	outMaxX = (long)maxX;
	outMaxY = (long)maxY;

	// CURSOR_TARGET mechanism emit: only when a corner was behind the camera (rare,
	// exactly the bug). Gate MC2_CURSOR_TARGET_TRACE + MC2_DIAG_TAGS=CURSOR_TARGET.
	// Read via get_diagnostic_events("CURSOR_TARGET"). No behavior change.
	if (behindCount > 0)
	{
		static const bool s_cursorTargetTrace = mc2::gate::get<"MC2_CURSOR_TARGET_TRACE">();
		if (s_cursorTargetTrace && mc2_diag::tagEnabled("CURSOR_TARGET"))
		{
			char _ct_buf[256];
			snprintf(_ct_buf, sizeof(_ct_buf),
			         "{\"site\":\"pick_rect\",\"behind_corners\":%d,\"pos\":[%.1f,%.1f,%.1f],\"rect\":[%ld,%ld,%ld,%ld]}",
			         behindCount, position.x, position.y, position.z,
			         outMinX, outMinY, outMaxX, outMaxY);
			mc2_diag::writeEvent("CURSOR_TARGET", 1, 0, _ct_buf);
		}
	}

	// INPUT-CURSOR-OFFSCREEN-TARGET-FIX-2: any OBB corner behind the near plane means
	// the camera straddles / sits inside the object's bounding box (e.g. a prop ~280
	// units below terrain, viewed from above). Near-plane corners — both the ones
	// behind (project to (0,0)) AND the in-front ones with tiny w (perspective divide
	// blows them to extreme screen coords) — make the screen rect span the whole view,
	// so the object wins the pick at every pixel ("cannot capture" wall). No reliable
	// 2D rect exists for such an object; reject it. A building selected normally (camera
	// outside it) has all 8 corners in front -> behindCount==0 -> unaffected.
	// Killswitch MC2_PICK_BEHIND_LEGACY=1 restores the legacy (broken) behavior.
	if (!s_pickBehindLegacy && behindCount > 0)
		return false;

	return true;
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObjectByMouse (long mouseX,
													long mouseY,
													GameObjectPtr* searchList,
													long listSize,
													bool skipDisabled) {

	if (!searchList)
		Fatal(0, " GameObjectManager.findObjectByMouse: NULL searchList ");

	for (long objIndex = 0; objIndex < listSize; objIndex++) 
	{
		if (searchList[objIndex] && searchList[objIndex]->getExists()) 
		{
			GameObjectPtr obj = searchList[objIndex];
			Assert(obj != NULL, objIndex, " GameObjectManager.findObjectByMouse: NULL obj ");
			AppearancePtr objAppearance = obj->getAppearance();
			if (objAppearance && objAppearance->canBeSeen()) 
			{
				if (obj->getWindowsVisible() == (turn - VISIBLE_THRESHOLD))
				{
					//-----------------------------------------------------
					float tlx = objAppearance->upperLeft.x;
					float tly = objAppearance->upperLeft.y;
					float brx = objAppearance->lowerRight.x;
					float bry = objAppearance->lowerRight.y;
						
					if ((mouseX >= tlx) && 
						(mouseX <= brx) &&
						(mouseY >= tly) &&
						(mouseY <= bry)) 
					{
						//---------------------------
						// We're on it, so save it...
						if (!obj->isMover() || (obj->isMover() && obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition())))
						{
							if (skipDisabled) 
							{
								if (!obj->isDisabled() && 
									(obj->getObjectClass() != TREE) && 
									(obj->getDamageLevel() != 36000000) && 				//We are a rock clump
									objAppearance->PerPolySelect(mouseX, mouseY))
									return(obj);
							}
							else
							{
								//Do not target trees or artillery strikes!!
								if ((obj->getObjectClass() != TREE) && 
									(obj->getObjectClass() != ARTILLERY) &&                                  									(obj->getDamageLevel() != 36000000) && 				//We are a rock clump
									(obj->getDamageLevel() != 36000000) && 				//We are a rock clump
									objAppearance->PerPolySelect(mouseX, mouseY))
									return(obj);
							}
						}
					}
				}
			}
		}
	}
	
	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findMoverByMouse (long mouseX,
												   long mouseY,
												   long commanderId,
												   bool skipDisabled) {

	GameObjectPtr* searchList = NULL;
	long numMovers = getMaxMovers();
	if (objList)
		searchList = &objList[1];

	if (!searchList)
		return(NULL);

	if (commanderId == -1)
		for (long objIndex = 0; objIndex < numMovers; objIndex++) 
		{
			if (searchList[objIndex] && searchList[objIndex]->getExists()) 
			{
				GameObjectPtr obj = searchList[objIndex];
				Assert(obj != NULL, objIndex, " GameObjectManager.findObjectByMouse: NULL obj ");
				AppearancePtr objAppearance = obj->getAppearance();
				if (objAppearance && objAppearance->canBeSeen()) 
				{
					if (obj->getWindowsVisible() == (turn - VISIBLE_THRESHOLD)) 
					{
						//-----------------------------------------------------
						float tlx = objAppearance->upperLeft.x;
						float tly = objAppearance->upperLeft.y;
						float brx = objAppearance->lowerRight.x;
						float bry = objAppearance->lowerRight.y;
							
						if ((mouseX >= tlx) && 
							(mouseX <= brx) &&
							(mouseY >= tly) &&
							(mouseY <= bry)) 
						{
							//---------------------------
							// We're on it, so save it...
							// Movers are NOT per poly!!
							if (!obj->isMover() || (obj->isMover() && obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition())))
							{
								if (skipDisabled) 
								{
									if (!obj->isDisabled())
										return(obj);
								}
								else
									return(obj);
							}
						}
					}
				}
			}
		}
	else
		for (long objIndex = 0; objIndex < numMovers; objIndex++) 
		{
			if (searchList[objIndex] && searchList[objIndex]->getExists()) 
			{
				GameObjectPtr obj = searchList[objIndex];
				Assert(obj != NULL, objIndex, " GameObjectManager.findObjectByMouse: NULL obj ");
				if (obj->getCommanderId() == commanderId)
					continue;
				AppearancePtr objAppearance = obj->getAppearance();
				if (objAppearance && objAppearance->canBeSeen()) 
				{
					if (obj->getWindowsVisible() == (turn - VISIBLE_THRESHOLD)) 
					{
						//-----------------------------------------------------
						float tlx = objAppearance->upperLeft.x;
						float tly = objAppearance->upperLeft.y;
						float brx = objAppearance->lowerRight.x;
						float bry = objAppearance->lowerRight.y;
							
						if ((mouseX >= tlx) && 
							(mouseX <= brx) &&
							(mouseY >= tly) &&
							(mouseY <= bry)) 
						{
							if (!obj->isMover() || (obj->isMover() && obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition())))
							{
								//---------------------------
								// We're on it, so save it...
								if (skipDisabled) 
								{
									if (!obj->isDisabled())
										return(obj);
								}
								else
									return(obj);
							}
						}
					}
				}
			}
		}

	return(NULL);
}

//---------------------------------------------------------------------------

// ---- MC2_PICK_RECON: recon-only split of findTerrainObjectByMouse (the dense-
// urban FindTerrainObj 3.6ms hotspot). Default OFF, NO behavior change. Counts
// candidates (objects in active blocks), inRect-passers (-> PerPolySelect volume),
// rectProj calls, and per-call wall-time. Run interactively (mouse over a dense
// cluster) to size the fix: candidate-volume vs per-PerPolySelect cost vs the
// per-frame block scan. (NEXT-SESSION measurement scaffolding; see memory.)
#include <chrono>
namespace {
	static const bool s_pickRecon = mc2::gate::get<"MC2_PICK_RECON">();
	static unsigned long long s_pkCalls=0, s_pkCandidates=0, s_pkInRect=0, s_pkRectProj=0;
	static unsigned long long s_pkNs=0, s_pkMaxNs=0, s_pkMaxCand=0;
	static bool s_pkAtexit=false;
	static void pkEmit() {
		if (!s_pickRecon) return;
		std::printf("[PICK_RECON v1] event=shutdown calls=%llu candidates=%llu inRect=%llu "
			"rectProj=%llu total_ms=%.1f avg_us=%.1f max_us=%.1f max_candidates_call=%llu\n",
			s_pkCalls, s_pkCandidates, s_pkInRect, s_pkRectProj, (double)s_pkNs/1e6,
			s_pkCalls?(double)s_pkNs/s_pkCalls/1000.0:0.0, (double)s_pkMaxNs/1000.0, s_pkMaxCand);
		std::fflush(stdout);
	}
	struct PkTimer {
		std::chrono::steady_clock::time_point t0; bool on;
		PkTimer():on(s_pickRecon){ if(on){ if(!s_pkAtexit){s_pkAtexit=true;std::atexit(pkEmit);} t0=std::chrono::steady_clock::now(); ++s_pkCalls; } }
		~PkTimer(){ if(!on)return; unsigned long long ns=(unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count(); s_pkNs+=ns; if(ns>s_pkMaxNs)s_pkMaxNs=ns; }
	};
}

GameObjectPtr GameObjectManager::findTerrainObjectByMouse (long mouseX,
														   long mouseY,
														   bool skipDisabled)
{
	// v0+: top-level cache on (mouseX, mouseY, cameraRevision).
	// Returns immediately when cursor and camera are both unchanged.
	// cameraRevision driven by Camera::worldToClip memcmp — covers every
	// mutation path (position/rotation/FOV/viewport/cinematic/script).
	// Updated at the single return site below.
	static long         s_pmX = -999999, s_pmY = -999999;
	static uint32_t     s_pmCamRev = 0;
	static unsigned long s_pmWatchID = 0;   // UAF FIX: cache by watch-ID, not raw pointer
	uint32_t camRev = eye ? eye->getViewProjectionRevision() : 0;
	// PICK-CACHE-UAF FIX: the picked object can be destroyed between frames (e.g. a building
	// blown up, or any mover killed). Caching the raw GameObjectPtr and later calling a
	// virtual on it (getExists()) dispatches through a freed vtable -> EXEC violation
	// (objmgr.cpp:3781, crash seen in findTerrainObjectByMouse). Cache the watch-ID instead
	// and re-resolve through getByWatchID(), which returns NULL if the object is gone.
	if (mouseX == s_pmX && mouseY == s_pmY &&
		camRev != 0 && camRev == s_pmCamRev)
	{
		return s_pmWatchID ? getByWatchID(s_pmWatchID) : NULL;
	}
	// Camera moved this frame — return stale (watch-ID-validated) result without scanning.
	// Hover highlight is cosmetic; player is panning, not selecting.
	// camRev is updated so that once camera settles the next frame triggers a fresh scan.
	if (camRev != 0 && camRev != s_pmCamRev) {
		s_pmCamRev = camRev;
		return s_pmWatchID ? getByWatchID(s_pmWatchID) : NULL;
	}

	PkTimer _pk;  // MC2_PICK_RECON per-call wall-time
	int32_t pickCandidates = 0;
	int32_t activeBlocksVisited = 0;
	GameObjectPtr pickBest = NULL;        // BUILDING-PICK FIX: world-space nearest-to-camera
	float         pickBestDistSq = 3.4e38f;
	for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++)
	{
		// Cull-cascade consumer (mouse pick). Post-8c source of truth for
		// objBlockInfo[].active / objVertexActive[] is the cull-merged Step 6
		// slim reduction loop in mclib/terrain.cpp (8c-part-1 merges the
		// per-vertex cull writes into that loop) — NOT the deleted VPL body,
		// NOT a "Step 5 / 5B slim pass" (that producer never existed; v3.2
		// deleted it). See VPL-retirement plan v3.5 note (CRIT-0).
		if (Terrain::objBlockInfo[terrainBlock].active)
		{
			activeBlocksVisited++;
			long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
			long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
			pickCandidates += numObjs;
				if (s_pickRecon) { s_pkCandidates += (unsigned long long)numObjs; if ((unsigned long long)numObjs > s_pkMaxCand) s_pkMaxCand = (unsigned long long)numObjs; }

			// CRIT-1 re-home (Task 4): the per-object test below is
			// DUPLICATED from the shared 5-param findObjectByMouse loop
			// body (canBeSeen guard -> windowsVisible equality -> coarse
			// rect -> PerPolySelect) as a TERRAIN-STATIC-ONLY inline
			// test. Terrain statics are NO LONGER routed through the
			// shared 5-param overload; that overload is left byte-
			// UNCHANGED for its other (mover) call site
			// (objmgr.cpp findObjectByMouse(...,&objList[1],
			// getMaxObjects(),false)). Replacing the guard inside the
			// shared overload would gate movers on a readback slot they
			// have no entry for -> silent mover-pick regression. The
			// lists walked here are terrain-statics-only by construction
			// (countTerrainObjects fills objBlockInfo[] only for
			// TERRAINOBJECT/TREE/TURRET/GATE/BUILDING/TREEBUILDING/
			// BRIDGE). Trees stay pick-excluded via the
			// getObjectClass()!=TREE skip (preserved verbatim below).
			GameObjectPtr* searchList = &objList[objIndex];
			for (long li = 0; li < numObjs; li++)
			{
				if (searchList[li] && searchList[li]->getExists())
				{
					GameObjectPtr obj = searchList[li];
					Assert(obj != NULL, li, " GameObjectManager.findTerrainObjectByMouse: NULL obj ");
					AppearancePtr objAppearance = obj->getAppearance();
					// CRIT-1: canBeSeen() guard repointed to the GPU cull
					// readback-visible set (fail-open when readback is
					// disabled, so a stock install with GPU cull off keeps
					// the legacy "always considered" behavior). Same idiom
					// as objmgr.cpp:1930 / mech.cpp / gvehicl.cpp.
					bool readbackVisible = gpu_cull::readback_isEnabled()
						? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(obj->getHandle()))
						: true;
					if (objAppearance && readbackVisible)
					{
						// windowsVisible equality UNCHANGED. The stamp
						// (code/terrobj.cpp TerrainObject::update(),
						// `if (inView) windowsVisible = turn;`) survives
						// via the coarse-angular recalcBounds return path:
						// the projection-body delete made `inView` a
						// strict SUPERSET (coarse-only), so the stamp
						// still fires for every pick-eligible object and
						// this equality still holds. Newly-admitted
						// objects are filtered by the lazy rect +
						// geometry-space PerPolySelect below. Do NOT
						// "fix" this equality -- it is correct by the
						// coarse-superset argument (review-verified fact).
						if (obj->getWindowsVisible() == (turn - VISIBLE_THRESHOLD))
						{
							//-----------------------------------------------------
							// CRIT-1: lazy per-candidate screen rect.
							// Class-guarded cast to BldgAppearance*.
							//
							// CORRECTION (Task 4 spec-review): an earlier
							// version of this comment claimed only
							// BUILDING/TREEBUILDING use BldgAppearance and
							// that TERRAINOBJECT/BRIDGE/TURRET/GATE "do
							// not". That was FALSE. TERRAINOBJECT and
							// BRIDGE (terrobj.cpp `appearance = new
							// BldgAppearance`), TURRET (turret.cpp same),
							// GATE (gate.cpp same) and ARTILLERY
							// (artlry.cpp same) ALL instantiate
							// BldgAppearance too. (TREE uses
							// TreeAppearance; movers use Mech3D/GV.)
							//
							// The rect pre-filter is INTENTIONALLY applied
							// ONLY to BUILDING/TREEBUILDING here. For the
							// other BldgAppearance classes the lazy rect
							// is deliberately DROPPED: skipping it is a
							// correctness-safe OVER-inclusion (a candidate
							// is never wrongly rejected by a missing
							// rect), and PerPolySelect (geometry-space,
							// run below) remains the authoritative pick
							// test for every class. This matches the pre-
							// delete behavior for a missing/zero rect (the
							// deleted projection block only produced a rect
							// for BldgAppearance, and only the
							// building-class path consumed it).
							long ulx, uly, lrx, lry;
							bool haveRect = false;
							long oc = obj->getObjectClass();
							BldgAppearance* ba =
								((oc == BUILDING) || (oc == TREEBUILDING) || (oc == TERRAINOBJECT) || (oc == BRIDGE) || (oc == TURRET) || (oc == GATE))
									? (BldgAppearance*)obj->getAppearance()
									: NULL;
							if (ba) {
								// v2: per-instance screen-rect cache keyed on cameraRevision.
								// Safe for static terrain objects (never move post-spawn).
								// camRev==0 means Camera not yet initialised: always project.
								auto& pc = ba->pickCache_;
								if (camRev != 0 && pc.valid && pc.cameraRevision == camRev) {
									// cache hit — reuse stored rect
									ulx = pc.ulx; uly = pc.uly;
									lrx = pc.lrx; lry = pc.lry;
									haveRect = true;
								} else {
									if (s_pickRecon) ++s_pkRectProj;
									haveRect = projectPickCandidateRect(ba, ulx, uly, lrx, lry);
									if (haveRect && camRev != 0) {
										pc.ulx = ulx; pc.uly = uly;
										pc.lrx = lrx; pc.lry = lry;
										pc.cameraRevision = camRev;
										pc.valid = true;
									}
								}
							}

							bool inRect = true;
							if (haveRect)
							{
								inRect = (mouseX >= ulx) &&
								         (mouseX <= lrx) &&
								         (mouseY >= uly) &&
								         (mouseY <= lry);
							}

							// BUILDING-PICK FIX (world-space): the candidate's projected world OBB
							// (projectPickCandidateRect, O(8) corners, correct eye projection) must
							// contain the cursor. Replaces per-face screen PerPolySelect, which needs
							// the CPU listOfVisibleFaces the GPU static-prop path no longer bakes
							// (numVisibleFaces==0 -> buildings were untargetable). No per-face work.
							if (haveRect && inRect)
							{
								if (s_pickRecon) ++s_pkInRect;
								if (!obj->isMover() || (obj->isMover() && obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition())))
								{
									bool okClass;
									if (skipDisabled)
										okClass = (!obj->isDisabled() && (oc != TREE) && (obj->getDamageLevel() != 36000000));
									else
										okClass = ((oc != TREE) && (oc != ARTILLERY) && (obj->getDamageLevel() != 36000000));
									if (okClass)
									{
										// Disambiguate overlapping OBBs by nearest-to-camera (front building wins).
										Stuff::Vector3D _cp = eye->getPosition();
										Stuff::Vector3D _op = obj->getPosition();
										float _dx=_op.x-_cp.x, _dy=_op.y-_cp.y, _dz=_op.z-_cp.z;
										float _dd=_dx*_dx+_dy*_dy+_dz*_dz;
										if (_dd < pickBestDistSq) { pickBestDistSq = _dd; pickBest = obj; }
									}
								}
							}
						}
					}
				}
			}
		}
	}

	TracyPlot("MIF.TerrainPick.activeBlocks", int64_t(activeBlocksVisited));
	TracyPlot("MIF.TerrainPick.candidates", int64_t(pickCandidates));
	// v0+: update top-level cache before returning.
	s_pmX = mouseX; s_pmY = mouseY; s_pmCamRev = camRev;
	s_pmWatchID = pickBest ? pickBest->getWatchID() : 0;   // UAF FIX: cache watch-ID, not raw ptr
	return(pickBest);   // BUILDING-PICK FIX: nearest world-OBB hit (NULL if none)
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObjectByMouse (long mouseX, long mouseY) {

	GameObjectPtr obj = NULL;

	long homeCommanderId = Commander::home->getId();
	{
		ZoneScopedN("MIF.FindMover");
		obj = findMoverByMouse(mouseX, mouseY, homeCommanderId, true);
		if (obj)
			return(obj);
		obj = findMoverByMouse(mouseX, mouseY, homeCommanderId, false);
		if (obj)
			return(obj);
		obj = findMoverByMouse(mouseX, mouseY, -1, true);
		if (obj)
			return(obj);
		obj = findMoverByMouse(mouseX, mouseY, -1, false);
		if (obj)
			return(obj);
	}

	{
		ZoneScopedN("MIF.FindObjList");
		obj = findObjectByMouse(mouseX, mouseY, &objList[1], getMaxObjects(),false);
		if (obj)
			return(obj);
	}

	{
		ZoneScopedN("MIF.FindTerrainObj");
		return(findTerrainObjectByMouse(mouseX, mouseY,true));
	}
}

//---------------------------------------------------------------------------
bool GameObjectManager::moverInRect(long index, Stuff::Vector3D &dStart, Stuff::Vector3D &dEnd)
{
	//------------------------------------------------------------------------------
	// This function checks the mover passed in to see if it
	// is within the magic rectangle.  It assumes we are looking for our alignment.
	// This is because we are drag selecting in the GUI!
	if ((index < 0) || (index >= getMaxMovers()))
		return(false);
		
	MoverPtr checkMover = getMover(index);
	// ISSUE-50: require isOnGUI(). A link-up reinforcement mech spawns
	// SHUTDOWN + off-GUI (mission.cpp) and must stay untouchable until the
	// link-up check in BattleMech::update adds it to the interface. Without
	// this filter, "select all visible" / drag-select (both route through
	// here) could select the hidden mech; any order then auto-powered it,
	// which permanently killed the old status==SHUTDOWN link-up gate — the
	// mech stayed invisible, off the pilots tab and off the tacmap forever.
	if (checkMover && checkMover->getExists() && checkMover->isOnGUI() &&
		(checkMover->getTeam() == Team::home))
	{
		AppearancePtr objAppearance = checkMover->getAppearance();
		if (objAppearance /*&& objAppearance->canBeSeen() */) 
		{
//			if (checkMover->getWindowsVisible() > (turn - VISIBLE_THRESHOLD)) 
			{
				//-----------------------------------------------------
				// Note that this effectively tests inTransport, since
				// windowsVisible only gets set when not inTransport...
				//objAppearance->recalcBounds();	//Shouldn't need to do this.  They should already be correct!
				long left 	= fmin(dStart.x, dEnd.x);
				long right 	= fmax(dStart.x, dEnd.x);
				long top 	= fmin(dStart.y, dEnd.y);
				long bottom = fmax(dStart.y, dEnd.y);
				
				if ((left <= objAppearance->getScreenPos().x) && 
					(right >= objAppearance->getScreenPos().x) &&
					(top <= objAppearance->getScreenPos().y) &&
					(bottom >= objAppearance->getScreenPos().y))
				{
					return(true);
				}
			}
		}
	}
	
	return(false);
}	
