// EditorObjectMgr: object picking by position, screen position and location.
// Declared in the class headers; shared names in EditorObjectMgr_internal.h.
#include "EditorObjectMgr_internal.h"
#include "Gates/gate.h"


static bool EditorObjectMgr_PointInsideAppearance(ObjectAppearance* pAppearance, int screenX, int screenY, bool allowPerPoly)
{
	if (!pAppearance)
		return false;

	int left = pAppearance->upperLeft.x;
	int top = pAppearance->upperLeft.y;
	int right = pAppearance->lowerRight.x;
	int bottom = pAppearance->lowerRight.y;

	if (right < left)
	{
		int tmp = left;
		left = right;
		right = tmp;
	}

	if (bottom < top)
	{
		int tmp = top;
		top = bottom;
		bottom = tmp;
	}

	if (right == left && bottom == top)
		return false;

	if (screenX < left || screenX > right || screenY < top || screenY > bottom)
		return false;

	if (allowPerPoly && pAppearance->canBeSeen())
		return pAppearance->PerPolySelect(screenX, screenY) ? true : false;

	return true;
}

static void EditorObjectMgr_ConsiderScreenCenter(EditorObject* pObject, int screenX, int screenY, float& bestDistanceSq, EditorObject*& pBestObject)
{
	if (!pObject || !pObject->appearance())
		return;

	ObjectAppearance* pApp = pObject->appearance();

	// Modern zoom-correct projection (projectModernClipGL + viewport remap) — the
	// legacy projectZ diverges/rejects when zoomed in (X-collapse). If the center
	// projects behind the near plane, this object is not pickable -> skip.
	Stuff::Vector4D center;
	if (!EditorObjectMgr_ProjectScreenXY_GL(pApp->position, center))
		return;

	// Size-aware pick radius: project a world point one extent-radius from the
	// object center and measure the screen-space gap, so large objects (walls,
	// big buildings) are pickable across their whole footprint rather than only
	// within a fixed pixel radius of the projected center. The legacy fixed 32px
	// fallback made click-selection feel unreliable -- it only hit near the
	// center and ignored the (often stale/3D) screen bounds.
	float worldRadius = pApp->getRadius();
	if (worldRadius < 1.0f)
		worldRadius = 1.0f;
	Stuff::Vector3D edgeWorld = pApp->position;
	edgeWorld.x += worldRadius;
	Stuff::Vector4D edge;
	// If the edge point rejects, fall back to the floored pixel radius below by
	// reusing center.x/y (zero screen gap -> screenRadius clamps to the floor).
	if (!EditorObjectMgr_ProjectScreenXY_GL(edgeWorld, edge))
		edge = center;
	float rdx = edge.x - center.x;
	float rdy = edge.y - center.y;
	float screenRadius = sqrtf(rdx * rdx + rdy * rdy);
	// Floor so small or zoomed-out objects stay clickable.
	if (screenRadius < 14.0f)
		screenRadius = 14.0f;

	float dx = center.x - (float)screenX;
	float dy = center.y - (float)screenY;
	float distanceSq = dx * dx + dy * dy;

	if (distanceSq <= screenRadius * screenRadius && distanceSq < bestDistanceSq)
	{
		bestDistanceSq = distanceSq;
		pBestObject = pObject;
	}
}

EditorObject* EditorObjectMgr::getObjectAtPosition( const Stuff::Vector3D& position )
{
	// World-space match: nearest object whose footprint (extent radius, with a
	// terrain-cell tolerance floor) contains the given world point. The old path
	// projected each object to screen and tested stale screen bounds + PerPolySelect,
	// which mis-targeted in the GL port -- damage/erase/link hit the wrong object or
	// nothing. This mirrors getObjectAtScreenPosition's matching so every brush that
	// uses getObjectAtPosition picks the same object the Select tool does.
	const float pickFloor = 3.0f * ( land ? land->worldUnitsPerVertex : 64.0f );
	float bestDistSq = 1.0e30f;
	EditorObject* pBest = NULL;

	for ( BUILDING_LIST::EIterator iter = buildings.Begin(); !iter.IsDone(); iter++ )
	{
		EditorObject* o = (*iter);
		if ( !o || !o->appearance() ) continue;
		ObjectAppearance* a = o->appearance();
		float dx = a->position.x - position.x;
		float dy = a->position.y - position.y;
		float d = dx * dx + dy * dy;
		float r = a->getRadius(); if ( r < pickFloor ) r = pickFloor;
		if ( d <= r * r && d < bestDistSq ) { bestDistSq = d; pBest = o; }
	}

	for ( UNIT_LIST::EIterator mIter = units.Begin(); !mIter.IsDone(); mIter++ )
	{
		EditorObject* o = (*mIter);
		if ( !o || !o->appearance() ) continue;
		ObjectAppearance* a = o->appearance();
		float dx = a->position.x - position.x;
		float dy = a->position.y - position.y;
		float d = dx * dx + dy * dy;
		float r = a->getRadius(); if ( r < pickFloor ) r = pickFloor;
		if ( d <= r * r && d < bestDistSq ) { bestDistSq = d; pBest = o; }
	}

	for ( DROP_LIST::EIterator dIter = dropZones.Begin(); !dIter.IsDone(); dIter++ )
	{
		EditorObject* o = (*dIter);
		if ( !o || !o->appearance() ) continue;
		float dx = o->getPosition().x - position.x;
		float dy = o->getPosition().y - position.y;
		float d = dx * dx + dy * dy;
		if ( d <= pickFloor * pickFloor && d < bestDistSq ) { bestDistSq = d; pBest = o; }
	}

	return pBest;
}

// Screen-space object pick helper: project the object's ACTUAL world position
// (real elevation) to screen pixels with the GL-correct forward transform
// (Camera::projectForScreenXY -- the same one FoliageRender renders with) and
// match by pixel distance to the click. This replaces the old inverseProject
// (screen->world unproject) pick, which diverged from the status-bar/placement
// transform (screenToGroundPlaneApprox) and, under the default-on LOD-chunk
// terrain path, returned a far/garbage world point on a heightfield-raycast miss
// -- so a click on empty foliage latched onto the nearest mech across the map.
//
// Forward projection is elevation-EXACT (uses position.z, no z=0 parallax) and
// needs no unproject, no t-range guard, and no nearest-vertex fallback. A true
// miss leaves pBest NULL. The per-object pixel tolerance is derived from the
// object's own world radius projected to screen (so big buildings stay clickable
// near and small props don't over-grab far), floored by a base click slop.
// GL-correct forward projection for editor screen-space picking. Projects a
// world point to screen pixels through worldToClipGL (projectModernClipGL),
// which keeps the SIGNED clip.w. The legacy projectForScreenXY default (Legacy
// mode) routes through projectZ, which does screen.w = fabs(rhw) (camera.h:508)
// and so DISCARDS the w-sign: a point BEHIND the camera came back with positive
// w, sailed through the `sp.w <= 1e-4` near-plane guard, and its mirrored
// screen-XY matched the click -> "selecting things behind the camera". Using the
// signed-w GL transform makes the near-plane reject correct. No frustum bool
// cull (a2cebf34): off-screen-but-in-front points get a real screen.xy and are
// filtered by the caller's pixel-distance test. Same transform the scene renders
// with, so the pick lines up with what is on screen.
bool EditorObjectMgr_ProjectScreenXY_GL(const Stuff::Vector3D& wp, Stuff::Vector4D& sp)
{
	if (!eye)
		return false;
	ModernClipResult r = eye->projectModernClipGL(wp);
	if (r.clip.w <= 1e-4f)            // at/behind the near plane (SIGNED w)
		return false;
	float vmx = 0.f, vmy = 0.f, vax = 0.f, vay = 0.f;
	gos_GetViewport(&vmx, &vmy, &vax, &vay);
	const float ndcX = r.clip.x / r.clip.w;
	const float ndcY = r.clip.y / r.clip.w;
	sp.x = vax + (ndcX * 0.5f + 0.5f) * vmx;
	sp.y = vay + (1.0f - (ndcY * 0.5f + 0.5f)) * vmy;  // GL origin bottom-left -> screen-Y flip
	sp.z = r.clip.z / r.clip.w;
	sp.w = r.clip.w;
	return true;
}

static void EditorObjectMgr_ConsiderScreenPick(EditorObject* pObject,
	float clickX, float clickY, float pixelFloor, float& bestPixSq, EditorObject*& pBest)
{
	if (!pObject || !pObject->appearance())
		return;
	ObjectAppearance* pApp = pObject->appearance();

	// --- Object's world height ---------------------------------------------
	// The old pick centered on the bare GROUND point (position) with tolerance =
	// projected horizontal getRadius(). But tall objects (mechs/vehicles) render
	// UPWARD from the ground point, and at the editor camera tilt "world up"
	// projects up-and-left on screen -- so the hit circle landed at the model's
	// base (down-right of the visible center), forcing the user to click the
	// wheels/feet. The selection brackets (recalcBounds) already cover the model;
	// this makes the pick hit-region do the same.
	//
	// Height source (both accessors are base-class virtuals on Appearance, so no
	// new API is added): BldgAppearance overrides getTopZ() -> highZ (a true world
	// height above the ground point; codebase already uses position.z + getTopZ()
	// for hotspots, e.g. bldng.h:237). Mech3D/GV appearances DO NOT override
	// getTopZ() (inherits 0.0f) but DO override getRadius() -> OBBRadius (a
	// bounding-sphere radius). So: use getTopZ() when it is meaningful (buildings),
	// else fall back to the sphere radius as a height proxy (mechs/vehicles).
	float worldR = pApp->getRadius();
	float worldH = pApp->getTopZ();
	if (worldH <= 1.0f)                    // no real height (mech/vehicle) -> use sphere radius
		worldH = (worldR > 0.0f) ? worldR : 0.0f;

	// Three world points up the object's vertical axis: ground, bounding-center
	// (ground + half height), and top (ground + full height). We center the pick
	// on the bounding-center so the hit-region sits on the visible model, not its
	// feet; ground+top give us the on-screen vertical span.
	Stuff::Vector3D wGround = pApp->position;   // real position, real elevation
	Stuff::Vector3D wCenter = wGround;  wCenter.z += 0.5f * worldH;
	Stuff::Vector3D wTop    = wGround;  wTop.z    += worldH;

	Stuff::Vector4D sCenter;
	if (!EditorObjectMgr_ProjectScreenXY_GL(wCenter, sCenter))  // behind near plane (signed w) / no camera
		return;
	if (sCenter.w <= 1e-4f)                // at/behind the near plane (defensive; helper checks)
		return;
	if (!(sCenter.x == sCenter.x) || !(sCenter.y == sCenter.y))  // NaN guard
		return;

	// Vertical screen reach: project ground and top, take the larger screen-space
	// distance from the bounding-center to either endpoint. This is the half-span
	// of the model's projected vertical extent. Endpoints that fail to project
	// (behind near plane / NaN) are skipped -- the center is always valid here.
	float tol = pixelFloor;
	{
		Stuff::Vector4D sGround, sTop;
		if (EditorObjectMgr_ProjectScreenXY_GL(wGround, sGround) && sGround.w > 1e-4f
		    && (sGround.x == sGround.x) && (sGround.y == sGround.y))
		{
			float gx = sGround.x - sCenter.x;
			float gy = sGround.y - sCenter.y;
			float gReach = sqrtf(gx * gx + gy * gy);
			if (gReach > tol)
				tol = gReach;
		}
		if (EditorObjectMgr_ProjectScreenXY_GL(wTop, sTop) && sTop.w > 1e-4f
		    && (sTop.x == sTop.x) && (sTop.y == sTop.y))
		{
			float tx = sTop.x - sCenter.x;
			float ty = sTop.y - sCenter.y;
			float tReach = sqrtf(tx * tx + ty * ty);
			if (tReach > tol)
				tol = tReach;
		}
	}

	// Horizontal screen radius: project a point one world-radius away in X from the
	// bounding-center and measure the pixel delta. Keeps wide/short objects
	// clickable across their breadth. Combined with the vertical reach above so the
	// final hit-circle covers the full on-screen extent.
	if (worldR > 1.0f)
	{
		Stuff::Vector3D wr = wCenter;
		wr.x += worldR;
		Stuff::Vector4D sr;
		if (EditorObjectMgr_ProjectScreenXY_GL(wr, sr) && sr.w > 1e-4f
		    && (sr.x == sr.x) && (sr.y == sr.y))
		{
			float rpx = sr.x - sCenter.x;
			float rpy = sr.y - sCenter.y;
			float screenR = sqrtf(rpx * rpx + rpy * rpy);
			if (screenR > tol)
				tol = screenR;
		}
	}

	// Hit test + nearest-object tie-break are both measured to the bounding-CENTER
	// projection (the region center), so the closest visible object still wins.
	float dx = sCenter.x - clickX;
	float dy = sCenter.y - clickY;
	float pixSq = dx * dx + dy * dy;
	if (pixSq <= tol * tol && pixSq < bestPixSq)
	{
		bestPixSq = pixSq;
		pBest = pObject;
	}
}

EditorObject* EditorObjectMgr::getObjectAtScreenPosition( int screenX, int screenY )
{
	// Match objects in SCREEN space by forward-projecting each object's real
	// world position (see EditorObjectMgr_ConsiderScreenPick). This is what the
	// user actually sees on screen, so the pick lines up with the cursor; it does
	// not depend on the fragile screen->world unproject.
	if (!eye)
		return NULL;

	const float clickX = (float)screenX;
	const float clickY = (float)screenY;

	// Base click slop in pixels (objects with a larger projected radius widen it).
	const float pixelFloor = 12.0f;

	float bestPixSq = 1.0e30f;
	EditorObject* pBest = NULL;

	for( BUILDING_LIST::EIterator iter = buildings.Begin(); !iter.IsDone(); iter++ )
		EditorObjectMgr_ConsiderScreenPick(*iter, clickX, clickY, pixelFloor, bestPixSq, pBest);

	for( UNIT_LIST::EIterator mIter = units.Begin(); !mIter.IsDone(); mIter++ )
		EditorObjectMgr_ConsiderScreenPick(*mIter, clickX, clickY, pixelFloor, bestPixSq, pBest);

	for ( DROP_LIST::EIterator dIter = dropZones.Begin(); !dIter.IsDone(); dIter++ )
		EditorObjectMgr_ConsiderScreenPick(*dIter, clickX, clickY, pixelFloor, bestPixSq, pBest);

	// MC2_EDITOR_PICK_TRACE diagnostic (default-off). Independent measurement of the
	// rotate-pick failure: forward-project EVERY object and report the one whose
	// projGL screen pos is NEAREST the cursor. After a camera rotate, if the user
	// clicked a VISIBLE object but its projGL screen pos is FAR from the cursor, the
	// forward projection is wrong at this yaw (not a stale matrix). If it is NEAR but
	// hit==NO, the failure is the pick tolerance / selection gate, not projection.
	{
		static const bool s_pickTrace = mc2::gate::get<"MC2_EDITOR_PICK_TRACE">();
		if ( s_pickTrace )
		{
			float nbest = 1.0e30f, nSx = -1.f, nSy = -1.f;
			Stuff::Vector3D nWorld; nWorld.x = nWorld.y = nWorld.z = 0.f;
			const char* nKind = "none";
			for ( BUILDING_LIST::EIterator i = buildings.Begin(); !i.IsDone(); i++ )
			{
				if ( !(*i) || !(*i)->appearance() ) continue;
				Stuff::Vector3D wp = (*i)->appearance()->position; Stuff::Vector4D sp;
				if ( !EditorObjectMgr_ProjectScreenXY_GL( wp, sp ) ) continue;
				float dx = sp.x - clickX, dy = sp.y - clickY, d2 = dx*dx + dy*dy;
				if ( d2 < nbest ) { nbest = d2; nWorld = wp; nSx = sp.x; nSy = sp.y; nKind = "bldg"; }
			}
			for ( UNIT_LIST::EIterator i = units.Begin(); !i.IsDone(); i++ )
			{
				if ( !(*i) || !(*i)->appearance() ) continue;
				Stuff::Vector3D wp = (*i)->appearance()->position; Stuff::Vector4D sp;
				if ( !EditorObjectMgr_ProjectScreenXY_GL( wp, sp ) ) continue;
				float dx = sp.x - clickX, dy = sp.y - clickY, d2 = dx*dx + dy*dy;
				if ( d2 < nbest ) { nbest = d2; nWorld = wp; nSx = sp.x; nSy = sp.y; nKind = "unit"; }
			}
			fprintf( stderr,
				"[PickTrace] cursor=(%d,%d) hit=%s nearest=%s world=(%.1f,%.1f,%.1f) projScreen=(%.1f,%.1f) pixDist=%.1f\n",
				screenX, screenY, pBest ? "YES" : "NO", nKind,
				nWorld.x, nWorld.y, nWorld.z, nSx, nSy, sqrtf( nbest ) );
			fflush( stderr );
		}
	}

	return pBest;
}

static bool areCloseEnough(float f1, float f2)
{
	bool retval = false;
	float diff = fabs(f1 - f2);
	float bigger = fabs(f1);
	if (fabs(f2) > bigger) {
		bigger = fabs(f2);
	}
	float proportion_diff = 0.0;
	if (0.0 != bigger) {
		proportion_diff = diff / bigger;
	}
	if ((pow(10.0, -FLT_DIG) * 10.0/*arbitrary*/) > proportion_diff) {
		retval = true;
	}
	return retval;
}

EditorObject* EditorObjectMgr::getObjectAtLocation( float x, float y )
{
	for( BUILDING_LIST::EIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
	{
		if (areCloseEnough((*iter)->getPosition().x, x) && areCloseEnough((*iter)->getPosition().y, y))
		{
			return (*iter);
		}
	}

	for( UNIT_LIST::EIterator mIter = units.Begin(); !mIter.IsDone(); mIter++ )
	{
		if (areCloseEnough((*mIter)->getPosition().x, x) && areCloseEnough((*mIter)->getPosition().y, y))
		{
			return (*mIter);
		}
	}

	for ( DROP_LIST::EIterator dIter = dropZones.Begin(); !dIter.IsDone(); dIter++ )
	{
		// need to change this when we get drop zone art
		if (areCloseEnough((*dIter)->getPosition().x, x) && areCloseEnough((*dIter)->getPosition().y, y))
		{
			return (*dIter);
		}
	}

	return NULL;
}
