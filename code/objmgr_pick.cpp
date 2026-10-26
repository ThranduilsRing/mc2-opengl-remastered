// GameObjectManager: object / mover lookup by id, cell and mouse, and the terrain-object pick.
// Declared in the class headers; shared names in objmgr_internal.h.
#include "objmgr_internal.h"
#include "mc2fmt.h"
#include "mech3d.h"

std::vector<mc2::world::SpatialGrid::Candidate> GameObjectManager::nearbyMovers(const glm::vec3& position, float radius) {
	if (moverGridDirty) {
		moverGrid.clear();
		for (long i = 0; i < numMovers; ++i) {
			auto* mover = moverList[i];
			const auto p = mover->getPosition();
			moverGrid.add(p.x, p.y, i, entityId(mover->getHandle()));
		}
		moverGrid.finish();
		moverGridDirty = false;
	}
	return moverGrid.query(position.x, position.y, radius);
}

std::vector<mc2::world::SpatialGrid::Candidate> GameObjectManager::nearbyBuildings(const glm::vec3& position, float radius) {
	if (buildingGridDirty) {
		buildingGrid.clear();
		for (long i = 0; i < numBuildings; ++i) {
			auto* building = buildings[i];
			const auto p = building->getPosition();
			buildingGrid.add(p.x, p.y, i, entityId(building->getHandle()));
		}
		buildingGrid.finish();
		buildingGridDirty = false;
	}
	return buildingGrid.query(position.x, position.y, radius);
}


//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObject (glm::vec3 position) {

	float closestDistance = 10000.0;
	GameObjectPtr closestObj = nullptr;
	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++) {
		GameObjectPtr obj = objList[objIndex];
		Assert(obj != nullptr, objIndex, " GameObjectManager.findObject: NULL obj ");
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
	return(nullptr);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByPartId (long partId) {

	if (partId == 0)
		return(nullptr);

	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++) 
	{
		GameObjectPtr obj = objList[objIndex];
		if (obj && obj->getExists() && (obj->getPartId() == partId))
			return(obj);
	}
	return(nullptr);
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

	return(nullptr);
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

	const glm::vec3& position = ba->position;
	float rotation = ba->rotation;

	glm::vec3 boxCoords[8];
	Stuff::Vector4D bcsp[8];

	glm::vec3 boxStart;
	mc2ToRender(boxStart, appearType->typeUpperLeft);

	glm::vec3 boxEnd;
	mc2ToRender(boxEnd, appearType->typeLowerRight);

	glm::vec3 addCoords;

	addCoords.x = boxStart.x;
	addCoords.y = boxStart.y;
	addCoords.z = boxEnd.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[0] = position + addCoords;

	addCoords.x = boxStart.x;
	addCoords.y = boxEnd.y;
	addCoords.z = boxEnd.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[1] = position + addCoords;

	addCoords.x = boxEnd.x;
	addCoords.y = boxEnd.y;
	addCoords.z = boxEnd.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[2] = position + addCoords;

	addCoords.x = boxEnd.x;
	addCoords.y = boxStart.y;
	addCoords.z = boxEnd.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[3] = position + addCoords;

	addCoords.x = boxStart.x;
	addCoords.y = boxStart.y;
	addCoords.z = boxStart.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[4] = position + addCoords;

	addCoords.x = boxEnd.x;
	addCoords.y = boxStart.y;
	addCoords.z = boxStart.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[5] = position + addCoords;

	addCoords.x = boxEnd.x;
	addCoords.y = boxEnd.y;
	addCoords.z = boxStart.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[6] = position + addCoords;

	addCoords.x = boxStart.x;
	addCoords.y = boxEnd.y;
	addCoords.z = boxStart.z;
	if (rotation != 0.0f)
		Rotate(addCoords,-rotation);

	boxCoords[7] = position + addCoords;

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
	outMinX = static_cast<long>(minX);
	outMinY = static_cast<long>(minY);
	outMaxX = static_cast<long>(maxX);
	outMaxY = static_cast<long>(maxY);

	// CURSOR_TARGET mechanism emit: only when a corner was behind the camera (rare,
	// exactly the bug). Gate MC2_CURSOR_TARGET_TRACE + MC2_DIAG_TAGS=CURSOR_TARGET.
	// Read via get_diagnostic_events("CURSOR_TARGET"). No behavior change.
	if (behindCount > 0)
	{
		static const bool s_cursorTargetTrace = mc2::gate::get<"MC2_CURSOR_TARGET_TRACE">();
		if (s_cursorTargetTrace && mc2_diag::tagEnabled("CURSOR_TARGET"))
		{
			char _ct_buf[256];
			mc2::fmt::to_buf(_ct_buf, "{{\"site\":\"pick_rect\",\"behind_corners\":{:d},\"pos\":[{:.1f},{:.1f},{:.1f}],\"rect\":[{:d},{:d},{:d},{:d}]}}", mc2::fmt::as_i32(behindCount), position.x, position.y, position.z, mc2::fmt::as_i32(outMinX), mc2::fmt::as_i32(outMinY), mc2::fmt::as_i32(outMaxX), mc2::fmt::as_i32(outMaxY));
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
			Assert(obj != nullptr, objIndex, " GameObjectManager.findObjectByMouse: NULL obj ");
			// These classes use the stock bounds path below. Their CPU render
			// mesh may be replaced, or have no transformed faces on the GPU path.
			const long cls = obj->getObjectClass();
			if (cls == BUILDING || cls == TREEBUILDING || cls == TERRAINOBJECT
				|| cls == BRIDGE || cls == TURRET || cls == GATE) continue;
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
	
	return(nullptr);
}

//---------------------------------------------------------------------------

// A fallen mech's animated root can move the display box below ground.
// Keep the stock type box at ground level as well, with a small screen margin.
static void wreckPickRect(GameObjectPtr obj, float& left, float& top, float& right, float& bottom)
{
	auto* app = dynamic_cast<Mech3DAppearance*>(obj->getAppearance());
	if (!app || !app->getAppearanceType() || !eye) return;
	const auto* type = app->getAppearanceType();
	for (int i = 0; i < 8; ++i) {
		glm::vec3 corner = obj->getPosition() + glm::vec3(
			(i & 1) ? type->typeUpperLeft.x : type->typeLowerRight.x,
			(i & 2) ? type->typeUpperLeft.y : type->typeLowerRight.y,
			(i & 4) ? type->typeUpperLeft.z : type->typeLowerRight.z);
		Stuff::Vector4D screen;
		eye->projectForScreenXY(corner, screen);
		if (screen.w <= 1e-4f) return;
		left = std::min(left, screen.x); top = std::min(top, screen.y);
		right = std::max(right, screen.x); bottom = std::max(bottom, screen.y);
	}
	left -= 4; top -= 4; right += 4; bottom += 4;
}

GameObjectPtr GameObjectManager::findMoverByMouse (long mouseX,
												   long mouseY,
												   long commanderId,
												   bool skipDisabled) {

	GameObjectPtr* searchList = nullptr;
	long numMovers = getMaxMovers();
	if (objList)
		searchList = &objList[1];

	if (!searchList)
		return(nullptr);

	static bool wreckProbed = false;
	if (!wreckProbed && turn > 90 && mc2::gate::get<"MC2_VK_PICK_PROBE">()) {
		wreckProbed = true;
		for (long i = 0; i < numMovers; ++i) {
			auto* obj = searchList[i];
			if (!obj || !obj->getExists() || obj->getObjectClass() != BATTLEMECH
				|| !obj->isDisabled() || obj->isDestroyed() || !obj->getAppearance()) continue;
			auto* app = obj->getAppearance();
			float left = app->upperLeft.x, top = app->upperLeft.y;
			float right = app->lowerRight.x, bottom = app->lowerRight.y;
			wreckPickRect(obj, left, top, right, bottom);
			int hits = 0;
			for (int y = 0; y < 7; ++y) for (int x = 0; x < 7; ++x)
				hits += findMoverByMouse(long(left + (right-left)*(x+.5f)/7),
					long(top + (bottom-top)*(y+.5f)/7), -1, false) == obj;
			mc2::fmt::print_out("[WRECK_PICK_PROBE] id={} rect={:.0f},{:.0f},{:.0f},{:.0f} hits={}/49 stamp={} turn={} visible={}\n",
				obj->getPartId(), left, top, right, bottom, hits, obj->getWindowsVisible(), turn, app->canBeSeen());
		}
	}

	if (commanderId == -1)
		for (long objIndex = 0; objIndex < numMovers; objIndex++) 
		{
			if (searchList[objIndex] && searchList[objIndex]->getExists()) 
			{
				GameObjectPtr obj = searchList[objIndex];
				Assert(obj != nullptr, objIndex, " GameObjectManager.findObjectByMouse: NULL obj ");
				AppearancePtr objAppearance = obj->getAppearance();
				if (objAppearance && objAppearance->canBeSeen()) 
				{
					if (obj->getWindowsVisible() >= (turn - VISIBLE_THRESHOLD))
					{
						//-----------------------------------------------------
						float tlx = objAppearance->upperLeft.x;
						float tly = objAppearance->upperLeft.y;
						float brx = objAppearance->lowerRight.x;
						float bry = objAppearance->lowerRight.y;
						if (obj->getObjectClass() == BATTLEMECH && obj->isDisabled() && !obj->isDestroyed())
							wreckPickRect(obj, tlx, tly, brx, bry);
							
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
				Assert(obj != nullptr, objIndex, " GameObjectManager.findObjectByMouse: NULL obj ");
				if (obj->getCommanderId() == commanderId)
					continue;
				AppearancePtr objAppearance = obj->getAppearance();
				if (objAppearance && objAppearance->canBeSeen()) 
				{
					if (obj->getWindowsVisible() >= (turn - VISIBLE_THRESHOLD))
					{
						//-----------------------------------------------------
						float tlx = objAppearance->upperLeft.x;
						float tly = objAppearance->upperLeft.y;
						float brx = objAppearance->lowerRight.x;
						float bry = objAppearance->lowerRight.y;
						if (obj->getObjectClass() == BATTLEMECH && obj->isDisabled() && !obj->isDestroyed())
							wreckPickRect(obj, tlx, tly, brx, bry);
							
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

	return(nullptr);
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
		mc2::fmt::print_out("[PICK_RECON v1] event=shutdown calls={:d} candidates={:d} inRect={:d} rectProj={:d} total_ms={:.1f} avg_us={:.1f} max_us={:.1f} max_candidates_call={:d}\n", mc2::fmt::as_u64(s_pkCalls), mc2::fmt::as_u64(s_pkCandidates), mc2::fmt::as_u64(s_pkInRect), mc2::fmt::as_u64(s_pkRectProj), static_cast<double>(s_pkNs)/1e6, s_pkCalls?static_cast<double>(s_pkNs)/s_pkCalls/1000.0:0.0, static_cast<double>(s_pkMaxNs)/1000.0, mc2::fmt::as_u64(s_pkMaxCand));
		std::fflush(stdout);
	}
	struct PkTimer {
		std::chrono::steady_clock::time_point t0; bool on;
		PkTimer():on(s_pickRecon){ if(on){ if(!s_pkAtexit){s_pkAtexit=true;std::atexit(pkEmit);} t0=std::chrono::steady_clock::now(); ++s_pkCalls; } }
		~PkTimer(){ if(!on)return; unsigned long long ns=static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count()); s_pkNs+=ns; if(ns>s_pkMaxNs)s_pkMaxNs=ns; }
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
		return s_pmWatchID ? getByWatchID(s_pmWatchID) : nullptr;
	}
	// A camera change invalidates the result, including at an unchanged mouse
	// position. Updating its revision without scanning caches a stale hit forever.

	PkTimer _pk;  // MC2_PICK_RECON per-call wall-time
	int32_t pickCandidates = 0;
	int32_t activeBlocksVisited = 0;
	GameObjectPtr pickBest = nullptr;        // BUILDING-PICK FIX: world-space nearest-to-camera
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
				if (s_pickRecon) { s_pkCandidates += static_cast<unsigned long long>(numObjs); if (static_cast<unsigned long long>(numObjs) > s_pkMaxCand) s_pkMaxCand = static_cast<unsigned long long>(numObjs); }

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
					Assert(obj != nullptr, li, " GameObjectManager.findTerrainObjectByMouse: NULL obj ");
					AppearancePtr objAppearance = obj->getAppearance();
					// Lagged render occlusion is not gameplay pick authority. Small
					// props can have no readback sample even while their mesh is drawn.
					if (objAppearance)
					{
						// Static props no longer run the per-frame update that stamps
						// windowsVisible. Current projected stock bounds decide the hit.
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
									? static_cast<BldgAppearance*>(obj->getAppearance())
									: nullptr;
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
										glm::vec3 _cp = eye->getPosition();
										glm::vec3 _op = obj->getPosition();
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
	// Extend the existing scripted pick probe to the static-object path. It
	// exercises real picks across each visible stock silhouette's bounding box.
	static bool probed = false;
	if (!probed && turn > 20 && mc2::gate::get<"MC2_VK_PICK_PROBE">()) {
		probed = true;
		for (long h = 1; h <= getMaxObjects(); ++h) {
			auto* candidate = objList[h];
			if (!candidate || !candidate->getExists()) continue;
			const long cls = candidate->getObjectClass();
			if (cls != BUILDING && cls != TREEBUILDING && cls != TERRAINOBJECT) continue;
			auto* ba = static_cast<BldgAppearance*>(candidate->getAppearance());
			long x0, y0, x1, y1;
			if (!projectPickCandidateRect(ba, x0, y0, x1, y1) || x0 < 0 || y0 < 0
				|| x1 >= Environment.screenWidth || y1 >= Environment.screenHeight) continue;
			int hits = 0;
			for (int y = 0; y < 7; ++y) for (int x = 0; x < 7; ++x)
				hits += findTerrainObjectByMouse(x0 + (x1-x0)*(2*x+1)/14,
					y0 + (y1-y0)*(2*y+1)/14, true) == candidate;
			mc2::fmt::print_out("[STATIC_PICK_PROBE] name={} handle={} rect={},{},{},{} hits={}/49 stamp={} turn={} readback={} disabled={} damage={}\n",
				ba->appearType->name, h, x0, y0, x1, y1, hits, candidate->getWindowsVisible(), turn,
				gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(h)), candidate->isDisabled(), candidate->getDamageLevel());
		}
		// Probe calls changed the cache; restore the caller's result.
		s_pmX = mouseX; s_pmY = mouseY; s_pmWatchID = pickBest ? pickBest->getWatchID() : 0;
	}
	return(pickBest);   // BUILDING-PICK FIX: nearest world-OBB hit (NULL if none)
}

//---------------------------------------------------------------------------


namespace {
struct PickRect {
    float left = 3.4e38f, top = 3.4e38f, right = -3.4e38f, bottom = -3.4e38f;
    bool contains(long x, long y) const {
        return x >= left && x <= right && y >= top && y <= bottom;
    }
};

// Grounded stock bounds survive GPU-only geometry and fallen animation roots.
// Display bounds are retained for animated limbs; no render-frame stamp is authority.
bool gameplayPickRect(GameObjectPtr obj, PickRect& rect, bool margin = true) {
    if (!obj || !obj->getExists() || obj->inTransport() || !eye) return false;
    auto* app = obj->getAppearance();
    if (!app || !app->getAppearanceType()) return false;
    const auto* type = app->getAppearanceType();
    glm::vec3 lo, hi;
    mc2ToRender(lo, type->typeUpperLeft);
    mc2ToRender(hi, type->typeLowerRight);
    for (int i = 0; i < 8; ++i) {
        glm::vec3 offset((i & 1) ? lo.x : hi.x,
                         (i & 2) ? lo.y : hi.y,
                         (i & 4) ? lo.z : hi.z);
        Rotate(offset, -obj->getRotation());
        Stuff::Vector4D screen;
        auto corner = obj->getPosition() + offset;
        eye->projectForScreenXY(corner, screen);
        if (screen.w <= 1e-4f || !std::isfinite(screen.x) || !std::isfinite(screen.y)) return false;
        rect.left = std::min(rect.left, screen.x); rect.top = std::min(rect.top, screen.y);
        rect.right = std::max(rect.right, screen.x); rect.bottom = std::max(rect.bottom, screen.y);
    }
    if (obj->isMover() && app->canBeSeen()) {
        rect.left = std::min(rect.left, app->upperLeft.x); rect.top = std::min(rect.top, app->upperLeft.y);
        rect.right = std::max(rect.right, app->lowerRight.x); rect.bottom = std::max(rect.bottom, app->lowerRight.y);
        if (obj->getObjectClass() == BATTLEMECH && obj->isDisabled() && !obj->isDestroyed()) {
            // Preserve the salvage fix's grounded envelope exactly.
            wreckPickRect(obj, rect.left, rect.top, rect.right, rect.bottom);
            // wreckPickRect already added its four-pixel margin.
            rect.left += 4; rect.top += 4; rect.right -= 4; rect.bottom -= 4;
        }
    }
    if (margin) {
        const float pad = 4.f * std::max(1.f, std::min(Environment.screenWidth / 800.f, Environment.screenHeight / 600.f));
        rect.left -= pad; rect.top -= pad; rect.right += pad; rect.bottom += pad;
    }
    return rect.right >= 0 && rect.bottom >= 0 && rect.left < Environment.screenWidth && rect.top < Environment.screenHeight;
}

bool gameplayPickEligible(GameObjectPtr obj, bool skipDisabled) {
    if (!obj || !obj->getExists() || obj->inTransport() || obj->isDestroyed()) return false;
    const long cls = obj->getObjectClass();
    if (cls == TREE || cls == ARTILLERY || obj->getDamageLevel() == 36000000
        || (skipDisabled && obj->isDisabled())) return false;
    return !obj->isMover() || (obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition()));
}

// Probe unambiguous bounds separately: a box is not a visible-mesh oracle.
void gameplayPickBattery(GameObjectManager& manager) {
    static bool busy = false;
    static uint32_t previous = 0, tested = 0;
    static int stable = 0;
    if (busy || turn <= 90 || !eye || !mc2::gate::get<"MC2_VK_PICK_PROBE">()) return;
    const auto revision = eye->getViewProjectionRevision();
    if (revision != previous) { previous = revision; stable = 0; }
    if (++stable < 4 || tested == revision) return;
    tested = revision;
    busy = true;
    struct Candidate { GameObjectPtr obj; PickRect rect; };
    std::vector<Candidate> candidates;
    for (long h = 1; h <= manager.getMaxObjects(); ++h) {
        auto* obj = manager.get(h);
        PickRect rect;
        if (gameplayPickEligible(obj, false) && gameplayPickRect(obj, rect, false)
            && rect.left >= 0 && rect.top >= 0 && rect.right < Environment.screenWidth && rect.bottom < Environment.screenHeight)
            candidates.push_back({obj, rect});
    }
    for (const auto& c : candidates) {
        int hits = 0, misses = 0, wrong = 0, overlap = 0;
        for (int y = 0; y < 7; ++y) for (int x = 0; x < 7; ++x) {
            const long px = std::lround(c.rect.left + (c.rect.right-c.rect.left)*x/6.f);
            const long py = std::lround(c.rect.top + (c.rect.bottom-c.rect.top)*y/6.f);
            bool ambiguous = false;
            for (const auto& other : candidates)
                if (other.obj != c.obj && other.rect.contains(px, py)) { ambiguous = true; break; }
            if (ambiguous) { ++overlap; continue; }
            auto* picked = manager.findObjectByMouse(px, py);
            if (picked == c.obj) ++hits;
            else if (picked) ++wrong;
            else ++misses;
        }
        mc2::fmt::print_out("[PICK_BATTERY] revision={} class={} id={} hits={} miss={} wrong={} overlap={}\n",
            revision, int(c.obj->getObjectClass()), c.obj->getPartId(), hits, misses, wrong, overlap);
    }
    busy = false;
}
} // namespace

GameObjectPtr GameObjectManager::findObjectByMouse (long mouseX, long mouseY) {
	gameplayPickBattery(*this);

	GameObjectPtr obj = nullptr;

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
bool GameObjectManager::moverInRect(long index, glm::vec3 &dStart, glm::vec3 &dEnd)
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
