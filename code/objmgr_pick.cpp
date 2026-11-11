// GameObjectManager: object / mover lookup by id, cell and mouse, and the terrain-object pick.
// Declared in the class headers; shared names in objmgr_internal.h.
#include <span>
#include "objmgr_internal.h"
#include "mc2fmt.h"
#include "mech3d.h"
#include "gameplay_pick.h"
#include "pick_occlusion.h"
#include "terrain_runtime.h"
#include "terrain_ray_pick.h"
#include "multplyr.h"

extern bool ShowMovers;

static std::vector<int> s_moverOrdinalByHandle;	// rebuilt with moverGrid, same lifetime

static void ensureMoverGrid(GameObjectManager& om, mc2::world::SpatialGrid& grid, bool& dirty, std::span<MoverPtr> movers) {
	if (!dirty) return;
	grid.clear();
	GameObjectHandle maxHandle = -1;
	for (int i = 0; i < static_cast<int>(movers.size()); ++i) {
		auto* mover = movers[i];
		const auto p = mover->getPosition();
		grid.add(p.x, p.y, i, om.entityId(mover->getHandle()));
		maxHandle = std::max(maxHandle, mover->getHandle());
	}
	grid.finish();
	s_moverOrdinalByHandle.assign(static_cast<size_t>(maxHandle + 1), -1);
	for (int i = 0; i < static_cast<int>(movers.size()); ++i)
		s_moverOrdinalByHandle[movers[i]->getHandle()] = static_cast<int>(i);
	dirty = false;
}

std::vector<mc2::world::SpatialGrid::Candidate> GameObjectManager::nearbyMovers(const glm::vec3& position, float radius) {
	ensureMoverGrid(*this, moverGrid, moverGridDirty, std::span<MoverPtr>(moverList, static_cast<size_t>(numMovers)));
	return moverGrid.query(position.x, position.y, radius);
}

void GameObjectManager::nearbyMoversInto(const glm::vec3& position, float radius, std::vector<mc2::world::SpatialGrid::Candidate>& out) {
	ensureMoverGrid(*this, moverGrid, moverGridDirty, std::span<MoverPtr>(moverList, static_cast<size_t>(numMovers)));
	moverGrid.queryInto(position.x, position.y, radius, out);
}

int GameObjectManager::moverOrdinal(GameObjectHandle handle) {
	ensureMoverGrid(*this, moverGrid, moverGridDirty, std::span<MoverPtr>(moverList, static_cast<size_t>(numMovers)));
	return (handle >= 0 && static_cast<size_t>(handle) < s_moverOrdinalByHandle.size()) ? s_moverOrdinalByHandle[handle] : -1;
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

// R3: partId -> slots in ascending slot order. Entries are hints: a hit is accepted only if the object still has
// that partId and exists, so a stale entry (partId changed or object freed) is skipped, never returned. Contract kept:
// the first existing object in slot order. GameObjectManager::setPartId, GameObject::Load and the marine hack feed
// notePartId; a missed feed is caught by the linear cross-check (debug builds, or define MC2_OBJMGR_XCHECK for a proof
// build), which reports [PARTID_XCHECK] through the diag trace and falls back to the linear answer.

void GameObjectManager::resetPartIdIndex (void) {
	partIdIndex.reset();
}

void GameObjectManager::notePartId (GameObjectPtr obj, long newPartId) {

	if (!partIdIndex || partIdIndex->dirty || newPartId == 0)
		return;
	const uint32_t slot = obj->getHandle();
	if (!slot || slot > static_cast<uint32_t>(getMaxObjects()) || objList[slot] != obj) {
		partIdIndex->dirty = true;
		return;
	}
	auto& v = partIdIndex->slots[newPartId];
	auto it = std::lower_bound(v.begin(), v.end(), slot);
	if (it == v.end() || *it != slot)
		v.insert(it, slot);
}

GameObjectPtr GameObjectManager::findByPartId (long partId) {

	if (partId == 0)
		return(nullptr);

	if (!partIdIndex)
		partIdIndex = std::make_shared<PartIdIndex>();
	if (partIdIndex->dirty) {
		partIdIndex->slots.clear();
		const long numObjects = getMaxObjects();
		for (long objIndex = 1; objIndex <= numObjects; objIndex++) {
			GameObjectPtr obj = objList[objIndex];
			if (obj && obj->getPartId() != 0)
				partIdIndex->slots[obj->getPartId()].push_back(static_cast<uint32_t>(objIndex));
		}
		partIdIndex->dirty = false;
	}

	GameObjectPtr found = nullptr;
	if (partIdIndex->slots.contains(partId)) {
		for (uint32_t slot : partIdIndex->slots.at(partId)) {
			GameObjectPtr obj = objList[slot];
			if (obj && obj->getExists() && obj->getPartId() == partId) {
				found = obj;
				break;
			}
		}
	}

#if !defined(NDEBUG) || defined(MC2_OBJMGR_XCHECK)
	{
		GameObjectPtr linear = nullptr;
		const long numObjects = getMaxObjects();
		for (long objIndex = 1; objIndex <= numObjects; objIndex++) {
			GameObjectPtr obj = objList[objIndex];
			if (obj && obj->getExists() && (obj->getPartId() == partId)) { linear = obj; break; }
		}
		if (linear != found) {
			char buf[96];
			(*std::format_to_n(buf, sizeof(buf) - 1, "{{\"partId\":{:d},\"hash\":{:d},\"linear\":{:d}}}",
				mc2::fmt::as_i32(partId), mc2::fmt::as_i32(found ? found->getHandle() : 0),
				mc2::fmt::as_i32(linear ? linear->getHandle() : 0)).out = '\0');
			mc2_diag::writeEvent("PARTID_XCHECK", 1, 0, buf);
			partIdIndex->dirty = true;
			found = linear;
		}
	}
#endif
	return(found);
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
		if (obj && obj->getExists())
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
	static const bool s_pickBehindLegacy = false;
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
			maxX = std::max(maxX, bcsp[i].x);
			minX = std::min(minX, bcsp[i].x);
			maxY = std::max(maxY, bcsp[i].y);
			minY = std::min(minY, bcsp[i].y);
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
		static const bool s_cursorTargetTrace = false;
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


static void wreckPickRect(GameObjectPtr obj, float& left, float& top, float& right, float& bottom, bool margin = true)
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
	if (margin) { left -= 4; top -= 4; right += 4; bottom += 4; }
}

namespace {
struct PickRect {
    float left = 3.4e38f, top = 3.4e38f, right = -3.4e38f, bottom = -3.4e38f;
    bool contains(long x, long y) const {
        return x >= left && x <= right && y >= top && y <= bottom;
    }
};

// Grounded stock bounds survive GPU-only geometry and fallen animation roots.
// Display bounds are retained for animated limbs; no render-frame stamp is authority.
float gameplayPickPad() {
    const float scale = Environment.screenWidth / 800.f < Environment.screenHeight / 600.f
        ? Environment.screenWidth / 800.f : Environment.screenHeight / 600.f;
    return 4.f * (scale > 1.f ? scale : 1.f);
}

// Cheap cull before the 8-corner projection: the bounding sphere of the type box against the four side planes of the view
// frustum, widened by the pick pad. A sphere wholly outside cannot have a padded rect on screen, so results are unchanged.
bool pickSphereOutsideScreen(const glm::vec3& pos, const AppearanceType* type) {
    static uint32_t s_rev = 0;
    static float s_planes[6][4];
    const auto revision = eye->getViewProjectionRevision();
    if (!revision) return false;
    if (revision != s_rev) { eye->extractFrustumPlanes(s_planes); s_rev = revision; }
    const auto maxAbs = [](float a, float b) { return std::max(std::fabs(a), std::fabs(b)); };
    const float ex = std::max({maxAbs(type->typeUpperLeft.x, type->typeLowerRight.x), maxAbs(type->typeUpperLeft.y, type->typeLowerRight.y),
                               maxAbs(type->typeUpperLeft.z, type->typeLowerRight.z)});
    const float radius = ex * 1.7320509f;   // sqrt(3) * largest half-extent bounds every corner
    const auto dot = [&](const float* pl) { return pl[0]*pos.x + pl[1]*pos.y + pl[2]*pos.z + pl[3]; };
    const auto len = [](const float* pl) { return std::sqrt(pl[0]*pl[0] + pl[1]*pl[1] + pl[2]*pl[2]); };
    // clip w = near + far plane (z and w - z); a pixel of slack at depth w is 2*w/extent in plane units.
    const float wc[4] = {s_planes[4][0]+s_planes[5][0], s_planes[4][1]+s_planes[5][1], s_planes[4][2]+s_planes[5][2], s_planes[4][3]+s_planes[5][3]};
    const float wMax = dot(wc) + radius * len(wc);
    const float pad = 2.f * gameplayPickPad() + 2.f;
    for (int i = 0; i < 4; ++i) {
        const float extent = i < 2 ? static_cast<float>(Environment.screenWidth) : static_cast<float>(Environment.screenHeight);
        const float tol = wMax > 0.f && extent > 0.f ? pad * 2.f * wMax / extent : 0.f;
        if (dot(s_planes[i]) + radius * len(s_planes[i]) + tol < 0.f) return true;
    }
    return false;
}

bool gameplayPickRect(GameObjectPtr obj, PickRect& rect, bool margin = true) {
    if (!obj || !obj->getExists() || obj->inTransport() || !eye) return false;
    auto* app = obj->getAppearance();
    if (!app || !app->getAppearanceType()) return false;
    const auto* type = app->getAppearanceType();
    if (!obj->isMover()) {
        auto* ba = dynamic_cast<BldgAppearance*>(app);
        if (!ba) return false;
        long l, t, r, b;
        auto& cache = ba->pickCache_;
        const auto revision = eye->getViewProjectionRevision();
        if (revision && cache.valid && cache.cameraRevision == revision) {
            l=cache.ulx; t=cache.uly; r=cache.lrx; b=cache.lry;
        } else {
            if (pickSphereOutsideScreen(ba->position, type)) return false;
            if (!projectPickCandidateRect(ba, l, t, r, b)) return false;
            cache.ulx=l; cache.uly=t; cache.lrx=r; cache.lry=b;
            cache.cameraRevision=revision; cache.valid=true;
        }
        rect.left=static_cast<float>(l); rect.top=static_cast<float>(t); rect.right=static_cast<float>(r); rect.bottom=static_cast<float>(b);
    } else {
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
    }
    if (obj->isMover() && app->canBeSeen()) {
        rect.left = std::min(rect.left, app->upperLeft.x); rect.top = std::min(rect.top, app->upperLeft.y);
        rect.right = std::max(rect.right, app->lowerRight.x); rect.bottom = std::max(rect.bottom, app->lowerRight.y);
        if (obj->getObjectClass() == BATTLEMECH && obj->isDisabled() && !obj->isDestroyed()) {
            // Preserve the salvage fix's grounded envelope exactly.
            wreckPickRect(obj, rect.left, rect.top, rect.right, rect.bottom, false);
        }
    }
    if (margin) {
        const float pad = gameplayPickPad();
        rect.left -= pad; rect.top -= pad; rect.right += pad; rect.bottom += pad;
    }
    return rect.right >= 0 && rect.bottom >= 0 && rect.left < Environment.screenWidth && rect.top < Environment.screenHeight;
}

bool gameplayPickEligible(GameObjectPtr obj, bool skipDisabled) {
    if (!obj || !obj->getExists() || obj->inTransport() || obj->isDestroyed()) return false;
    const long cls = obj->getObjectClass();
    if (cls == TREE || cls == ARTILLERY || obj->getDamageLevel() == 36000000
        || (skipDisabled && obj->isDisabled())) return false;
    if (!obj->isMover() && (!obj->getAppearance() || obj->isDisabled() || !obj->isSelectable()))
        return false;
    // Apply the existing UI contact restriction before ranking so an unseen
    // enemy cannot hide a visible friendly farther down the candidate stack.
    if (obj->isMover() && !ShowMovers
        && !(MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID])
        && obj->getTeamId() != Team::home->getId() && !obj->isDisabled()
        && !static_cast<Mover*>(obj)->contactView.blip) return false;
    return !obj->isMover() || (obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition()));
}

GameObjectPtr pickGameplayObjects(long x, long y, std::span<GameObjectPtr> list,
                                 bool skipDisabled, long excludeCommander = -1, bool terrainOnly = false);

// The screen rect of a projected box is its axis-aligned bound; for a thin object laid diagonally (fences, wires) that bound
// covers a lot of empty ground. A static prop is picked only when the cursor is within the pad of the projected box's own
// outline (the convex hull of its eight corners).
bool cursorOnProjectedBox(BldgAppearance* ba, long x, long y) {
    glm::vec3 lo, hi;
    mc2ToRender(lo, ba->appearType->typeUpperLeft);
    mc2ToRender(hi, ba->appearType->typeLowerRight);
    std::array<glm::vec2, 8> pts;
    for (int i = 0; i < 8; ++i) {
        glm::vec3 offset((i & 1) ? lo.x : hi.x, (i & 2) ? lo.y : hi.y, (i & 4) ? lo.z : hi.z);
        if (ba->rotation != 0.0f) Rotate(offset, -ba->rotation);
        glm::vec3 corner = ba->position + offset;
        Stuff::Vector4D screen;
        eye->projectForScreenXY(corner, screen);
        if (screen.w <= 1e-4f || !std::isfinite(screen.x) || !std::isfinite(screen.y)) return true;
        pts[i] = {screen.x, screen.y};
    }
    std::sort(pts.begin(), pts.end(), [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    const auto cross = [](const glm::vec2& o, const glm::vec2& a, const glm::vec2& b) { return (a.x-o.x)*(b.y-o.y) - (a.y-o.y)*(b.x-o.x); };
    std::array<glm::vec2, 16> hull;
    int n = 0;
    for (int i = 0; i < 8; ++i) {	// monotone chain
        while (n >= 2 && cross(hull[n-2], hull[n-1], pts[i]) <= 0.0f) --n;
        hull[n++] = pts[i];
    }
    for (int i = 6, floor = n + 1; i >= 0; --i) {
        while (n >= floor && cross(hull[n-2], hull[n-1], pts[i]) <= 0.0f) --n;
        hull[n++] = pts[i];
    }
    --n;	// the last point repeats the first
    const glm::vec2 p{static_cast<float>(x), static_cast<float>(y)};
    const float pad = gameplayPickPad();
    bool inside = n >= 3;
    for (int i = 0; i < n; ++i) {
        const glm::vec2 a = hull[i], b = hull[(i + 1) % n], e = b - a;
        if (inside && cross(a, b, p) < 0.0f) inside = false;
        const float len2 = glm::dot(e, e);
        const float t = len2 > 0.0f ? std::clamp(glm::dot(p - a, e) / len2, 0.0f, 1.0f) : 0.0f;
        if (glm::length(p - (a + e * t)) <= pad) return true;
    }
    return inside;
}

// Probe unambiguous bounds separately: a box is not a visible-mesh oracle.
void gameplayPickBattery(GameObjectManager& manager) {
    static bool busy = false;
    static uint32_t previous = 0, tested = 0;
    static int stable = 0;
    if (busy || turn <= 90 || !eye || !false) return;
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
        if (gameplayPickEligible(obj, false) && gameplayPickRect(obj, rect, false))
            candidates.push_back({obj, rect});
    }
    const auto fullyOnScreen = [](const PickRect& rect) {
        return rect.left >= 0 && rect.top >= 0 && rect.right < Environment.screenWidth && rect.bottom < Environment.screenHeight;
    };
    std::vector<GameObjectPtr> all, movers;
    for (long h=1;h<=manager.getMaxObjects();++h) all.push_back(manager.get(h));
    for (long h=0;h<manager.getNumMovers();++h) movers.push_back(manager.getMover(h));
    // Preserve the original parity census, including non-targetable props.
    for (auto* obj : all) {
        if (!obj || !obj->getExists()) continue;
        const auto cls = obj->getObjectClass();
        if (cls == BUILDING || cls == TREEBUILDING || cls == TERRAINOBJECT) {
            auto* ba = static_cast<BldgAppearance*>(obj->getAppearance());
            long x0,y0,x1,y1;
            if (projectPickCandidateRect(ba,x0,y0,x1,y1) && x0>=0 && y0>=0
                && x1<Environment.screenWidth && y1<Environment.screenHeight) {
                int hits=0;
                for (int y=0;y<7;++y) for (int x=0;x<7;++x)
                    hits += pickGameplayObjects(x0+(x1-x0)*(2*x+1)/14,y0+(y1-y0)*(2*y+1)/14,all,true,-1,true)==obj;
                mc2::fmt::print_out("[STATIC_PICK_PROBE] name={} handle={} rect={},{},{},{} hits={}/49\n",
                    ba->appearType->name,obj->getHandle(),x0,y0,x1,y1,hits);
            }
        }
        if (cls == BATTLEMECH && obj->isDisabled() && !obj->isDestroyed()) {
            auto* app=obj->getAppearance();
            float l=app->upperLeft.x,t=app->upperLeft.y,r=app->lowerRight.x,b=app->lowerRight.y;
            wreckPickRect(obj,l,t,r,b);
            int hits=0;
            for(int y=0;y<7;++y) for(int x=0;x<7;++x)
                hits += pickGameplayObjects(static_cast<long>(l+(r-l)*(x+.5f)/7),static_cast<long>(t+(b-t)*(y+.5f)/7),movers,false)==obj;
            mc2::fmt::print_out("[WRECK_PICK_PROBE] id={} rect={:.0f},{:.0f},{:.0f},{:.0f} hits={}/49\n",obj->getPartId(),l,t,r,b,hits);
        }
    }
    for (const auto& c : candidates) {
        if (!fullyOnScreen(c.rect)) continue;
        if (!c.obj->isMover() || c.obj->isDisabled() || c.obj->getTeam()!=Team::home
            || c.obj->getCommanderId()!=Commander::home->getId()) continue;
        int click=0, samples=0, box=0;
        for (int y=0;y<7;++y) for (int x=0;x<7;++x) {
            const long px=std::lround(c.rect.left+(c.rect.right-c.rect.left)*x/6.f);
            const long py=std::lround(c.rect.top+(c.rect.bottom-c.rect.top)*y/6.f);
            bool overlap=false;
            for (const auto& other:candidates)
                if (other.obj!=c.obj && other.obj->getCommanderId()==Commander::home->getId()
                    && other.obj->isMover() && other.rect.contains(px,py)) overlap=true;
            if (!overlap) { ++samples; click += pickGameplaySelection(px,py)==c.obj; }
        }
        for (long i=0;i<manager.getNumMovers();++i) if (manager.getMover(i)==c.obj) {
            for (const auto& point : {glm::vec3(c.rect.left,(c.rect.top+c.rect.bottom)/2,0),
                    glm::vec3(c.rect.right,(c.rect.top+c.rect.bottom)/2,0),
                    glm::vec3((c.rect.left+c.rect.right)/2,c.rect.top,0),
                    glm::vec3((c.rect.left+c.rect.right)/2,c.rect.bottom,0)}) {
                auto start=point-glm::vec3(1,1,0), end=point+glm::vec3(1,1,0);
                box += manager.moverInRect(i,start,end);
            }
            break;
        }
        mc2::fmt::print_out("[SELECTION_PICK_PROBE] id={} click={}/{} box={}/4\n",c.obj->getPartId(),click,samples,box);
    }
    for (const auto& c : candidates) {
        if (!fullyOnScreen(c.rect)) continue;
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
            else if (picked) {
                if (wrong < 3) mc2::fmt::print_out("[PICK_BATTERY_WRONG] expected={} actual={} x={} y={}\n",c.obj->getPartId(),picked->getPartId(),px,py);
                ++wrong;
            }
            else ++misses;
        }
        mc2::fmt::print_out("[PICK_BATTERY] revision={} class={} id={} hits={} miss={} wrong={} overlap={}\n",
            revision, static_cast<int>(c.obj->getObjectClass()), c.obj->getPartId(), hits, misses, wrong, overlap);
    }
    busy = false;
}

struct PickCandidate { GameObjectPtr obj; double depth; int priority; bool eligible; };

double pickTerrainDepth(const double (&origin)[3], const double (&direction)[3]) {
    double depth = std::numeric_limits<double>::infinity();
    if (land && Terrain::mapData) {
        const double half = Terrain::worldUnitsMapSide * 0.5 - 1.0;
        const double lower[2] = {-half, -half}, upper[2] = {half, half};
        mc2::firstTerrainHit(origin, direction, lower, upper, Terrain::worldUnitsPerVertex / 8.0,
            [](double px, double py) {
                return TerrainRuntime::sampleVisualHeight(glm::vec3(static_cast<float>(px), static_cast<float>(py), 0));
            }, depth);
    }
    return depth;
}

bool objectPickDepth(GameObjectPtr obj, const double (&origin)[3], const double (&direction)[3], double& depth) {
    const auto* type = obj->getAppearance()->getAppearanceType();
    glm::vec3 lo, hi;
    mc2ToRender(lo, type->typeUpperLeft);
    mc2ToRender(hi, type->typeLowerRight);
    auto position = obj->getPosition();
    auto rotation = obj->getRotation();
    if (auto* ba = dynamic_cast<BldgAppearance*>(obj->getAppearance())) {
        // Static rectangles use the appearance transform, including visual ground placement.
        position = ba->position;
        rotation = ba->rotation;
    }
    glm::vec3 localOrigin(static_cast<float>(origin[0]-position.x),
                          static_cast<float>(origin[1]-position.y), static_cast<float>(origin[2]-position.z));
    glm::vec3 localDirection(static_cast<float>(direction[0]), static_cast<float>(direction[1]), static_cast<float>(direction[2]));
    // Projection rotates local corners by -rotation; undo it for the ray.
    Rotate(localOrigin, rotation);
    Rotate(localDirection, rotation);
    const double ro[3] = {localOrigin.x, localOrigin.y, localOrigin.z};
    const double rd[3] = {localDirection.x, localDirection.y, localDirection.z};
    const double lower[3] = {lo.x, lo.y, lo.z}, upper[3] = {hi.x, hi.y, hi.z};
    return mc2::pickBoxHit(ro, rd, lower, upper, depth);
}

int pickPriority(GameObjectPtr obj) {
    if (obj->isDisabled()) return 3;
    if (obj->isEnemy(Team::home)) {
        if (obj->isMover()) return 0;
        const auto cls = obj->getObjectClass();
        if (cls == BUILDING || cls == TREEBUILDING || cls == TURRET || cls == GATE) return 1;
    }
    if (obj->isMover() || obj->isFriendly(Team::home)) return 2;
    return 4;
}

// All gameplay point picks reject terrain first, then prefer visible movers.
GameObjectPtr pickGameplayObjects(long x, long y, std::span<GameObjectPtr> list,
                                 bool skipDisabled, long excludeCommander, bool terrainOnly) {
    if (!eye || list.empty() || x < 0 || y < 0 || x >= Environment.screenWidth || y >= Environment.screenHeight) return GameObjectPtr{};
    static thread_local std::vector<PickCandidate> stack;
    stack.clear();
    double origin[3], direction[3];
    if (!mc2::pickScreenRay(*eye, x, y, origin, direction)) return GameObjectPtr{};
    const double terrainT = pickTerrainDepth(origin, direction);
    // Include non-targetable foreground objects and other commanders in the
    // occlusion stack. Filtering them first would pick straight through them.
    for (long h = 1; h <= ObjectManager->getMaxObjects(); ++h) {
        auto* obj = ObjectManager->get(h);
        if (!obj || !obj->getExists() || obj->inTransport() || !obj->getAppearance()
            || !obj->getAppearance()->canBeSeen()) continue;
        PickRect rect;
        if (!gameplayPickRect(obj, rect) || !rect.contains(x, y)) continue;
        if (!obj->isMover())
            if (auto* ba = dynamic_cast<BldgAppearance*>(obj->getAppearance()); ba && ba->appearType && !cursorOnProjectedBox(ba, x, y)) continue;
        const float pad = gameplayPickPad();
        const bool coreHit = x >= rect.left+pad && x <= rect.right-pad
            && y >= rect.top+pad && y <= rect.bottom-pad;
        double surfaceT = terrainT;
        const auto hit = mc2::pickDepthWithForgiveness(obj->isMover(),
            [&](double& t) { return objectPickDepth(obj, origin, direction, t); },
            [&](double& t) {
            // Keep the stock padded mover rectangle, including animated limbs
            // and salvage. Check terrain along a ray that actually hits the
            // mover, rather than the empty ground beside the clicked mech.
            const auto* type = obj->getAppearance()->getAppearanceType();
            glm::vec3 lo, hi;
            mc2ToRender(lo, type->typeUpperLeft);
            mc2ToRender(hi, type->typeLowerRight);
            auto offset = (lo + hi) * 0.5f;
            Rotate(offset, -obj->getRotation());
            auto center = obj->getPosition() + offset;
            Stuff::Vector4D screen;
            eye->projectForScreenXY(center, screen);
            double moverOrigin[3], moverDirection[3];
            if (screen.w <= 1e-4f || !std::isfinite(screen.x) || !std::isfinite(screen.y)
                || !mc2::pickScreenRay(*eye, std::lround(screen.x), std::lround(screen.y), moverOrigin, moverDirection)
                || !objectPickDepth(obj, moverOrigin, moverDirection, t)) return false;
            surfaceT = pickTerrainDepth(moverOrigin, moverDirection);
            return true;
        });
        if (!hit) continue;
        const double depth = *hit;
        if (!mc2::pickBeforeSurface(depth, surfaceT)) {
            if (mc2_diag::tagEnabled("CURSOR_TARGET")) {
                char payload[256];
                mc2::fmt::to_buf(payload,
                    "{{\"site\":\"pick_occlusion\",\"mouse\":[{},{}],\"part_id\":{},\"name_id\":{},\"object_t\":{:.3f},\"terrain_t\":{:.3f}}}",
                    mc2::fmt::as_i32(x), mc2::fmt::as_i32(y), obj->getPartId(),
                    obj->getAppearance()->getObjectNameId(), depth, surfaceT);
                mc2_diag::writeEvent("CURSOR_TARGET", 1, 0, payload);
            }
            continue;
        }
        const bool eligible = gameplayPickEligible(obj, skipDisabled)
            && (excludeCommander == -1 || obj->getCommanderId() != excludeCommander)
            && (!terrainOnly || !obj->isMover()) && std::find(list.begin(), list.end(), obj) != list.end();
        stack.push_back({obj, depth, pickPriority(obj) + (coreHit ? 0 : 5), eligible});
    }
    if (stack.empty()) return GameObjectPtr{};
    std::stable_sort(stack.begin(), stack.end(), [](const auto& a, const auto& b) {
        const bool mover = a.obj->isMover() && a.eligible;
        const bool otherMover = b.obj->isMover() && b.eligible;
        if (mc2::pickCandidateBefore(mover, a.depth, a.priority, otherMover, b.depth, b.priority)) return true;
        if (mc2::pickCandidateBefore(otherMover, b.depth, b.priority, mover, a.depth, a.priority)) return false;
        return a.obj->getHandle() < b.obj->getHandle();
    });
    const auto& front = stack.front();
    return front.eligible ? front.obj : nullptr;
}
} // namespace


GameObjectPtr GameObjectManager::findMoverByMouse(long x, long y, long commanderId, bool skipDisabled) {
    return pickGameplayObjects(x, y, objList ? std::span(&objList[1], getMaxMovers()) : std::span<GameObjectPtr>{}, skipDisabled, commanderId);
}

GameObjectPtr GameObjectManager::findTerrainObjectByMouse(long x, long y, bool skipDisabled) {
    return pickGameplayObjects(x, y, objList ? std::span(&objList[1], getMaxObjects()) : std::span<GameObjectPtr>{}, skipDisabled, -1, true);
}

GameObjectPtr GameObjectManager::findObjectByMouse(long x, long y) {
    gameplayPickBattery(*this);
    // The hover pick runs every frame: one cached answer per (mouse, view revision, turn).
    static struct { long x = -1, y = -1; uint32_t revision = 0; long turn = -1; GameObjectPtr result{}; } s_last;
    const uint32_t revision = eye ? eye->getViewProjectionRevision() : 0;
    if (revision && s_last.revision == revision && s_last.x == x && s_last.y == y && s_last.turn == ::turn) return s_last.result;
    auto* result = pickGameplayObjects(x, y, objList ? std::span(&objList[1], getMaxObjects()) : std::span<GameObjectPtr>{}, false);
    s_last = {x, y, revision, ::turn, result};
    return result;
}

bool GameObjectManager::moverInRect(long index, glm::vec3& start, glm::vec3& end) {
    if (index < 0 || index >= getNumMovers()) return false;
    auto* obj = getMover(index);
    if (!gameplayPickEligible(obj, false) || obj->getTeam() != Team::home) return false;
    PickRect rect;
    if (!gameplayPickRect(obj, rect)) return false;
    const auto [x0, x1] = std::minmax(start.x, end.x);
    const auto [y0, y1] = std::minmax(start.y, end.y);
    return x0 <= rect.right && x1 >= rect.left && y0 <= rect.bottom && y1 >= rect.top;
}

// Click selection uses the same service with an own-unit candidate list.
GameObjectPtr pickGameplaySelection(long x, long y) {
    std::vector<GameObjectPtr> own;
    for (long i=0;i<Team::home->getRosterSize();++i) {
        auto* mover=Team::home->getMover(i);
        if (mover && mover->getCommanderId()==Commander::home->getId()) own.push_back(mover);
    }
    return pickGameplayObjects(x,y,own,true);
}

// UI-command readback (pickgrid): pick every cell of a cols x rows screen grid and describe the winner, so a pick can be
// proven from logs without a human cursor.
std::string mc2PickGridReport(int cols, int rows) {
    std::string out;
    char line[512];
    for (int r = 0; r < rows; ++r) for (int c = 0; c < cols; ++c) {
        const long x = Environment.screenWidth * (2 * c + 1) / (2 * cols);
        const long y = Environment.screenHeight * (2 * r + 1) / (2 * rows);
        auto* obj = ObjectManager->findObjectByMouse(x, y);
        if (!obj) { mc2::fmt::to_buf(line, "  ({},{}) -> none\n", mc2::fmt::as_long(x), mc2::fmt::as_long(y)); out += line; continue; }
        PickRect rect; const bool ok = gameplayPickRect(obj, rect);
        Stuff::Vector4D sp; glm::vec3 pos = obj->getPosition(); eye->projectForScreenXY(pos, sp);
        auto* app = obj->getAppearance();
        mc2::fmt::to_buf(line, "  ({},{}) -> id={} cls={} type={} pos=({:.0f},{:.0f}) screen=({:.0f},{:.0f} w={:.2f}) rect={}({:.0f},{:.0f},{:.0f},{:.0f}) winVis={} turn={} nameId={}\n",
            mc2::fmt::as_long(x), mc2::fmt::as_long(y), static_cast<int>(obj->getPartId()), static_cast<int>(obj->getObjectClass()), mc2::fmt::safe((app && app->getAppearanceType()) ? static_cast<const char*>(app->getAppearanceType()->name) : "?"),
            pos.x, pos.y, sp.x, sp.y, sp.w, ok ? "" : "BAD", rect.left, rect.top, rect.right, rect.bottom,
            static_cast<int>(obj->getWindowsVisible()), static_cast<int>(::turn), app ? static_cast<long>(app->getObjectNameId()) : -1L);
        out += line;
    }
    return out;
}
