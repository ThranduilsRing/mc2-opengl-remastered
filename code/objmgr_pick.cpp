// GameObjectManager: object / mover lookup by id, cell and mouse, and the terrain-object pick.
// Declared in the class headers; shared names in objmgr_internal.h.
#include "objmgr_internal.h"
#include "mc2fmt.h"
#include "mech3d.h"
#include "gameplay_pick.h"
#include "multplyr.h"

extern bool ShowMovers;

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
            if (!projectPickCandidateRect(ba, l, t, r, b)) return false;
            cache.ulx=l; cache.uly=t; cache.lrx=r; cache.lry=b;
            cache.cameraRevision=revision; cache.valid=true;
        }
        rect.left=float(l); rect.top=float(t); rect.right=float(r); rect.bottom=float(b);
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

float gameplayPickPad() {
    const float scale = Environment.screenWidth / 800.f < Environment.screenHeight / 600.f
        ? Environment.screenWidth / 800.f : Environment.screenHeight / 600.f;
    return 4.f * (scale > 1.f ? scale : 1.f);
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
        && static_cast<Mover*>(obj)->conStat < CONTACT_SENSOR_QUALITY_1) return false;
    return !obj->isMover() || (obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition()));
}

GameObjectPtr pickGameplayObjects(long x, long y, GameObjectPtr* list, long count,
                                 bool skipDisabled, long excludeCommander = -1, bool terrainOnly = false);

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
                    hits += pickGameplayObjects(x0+(x1-x0)*(2*x+1)/14,y0+(y1-y0)*(2*y+1)/14,all.data(),long(all.size()),true,-1,true)==obj;
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
                hits += pickGameplayObjects(long(l+(r-l)*(x+.5f)/7),long(t+(b-t)*(y+.5f)/7),movers.data(),long(movers.size()),false)==obj;
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
            revision, int(c.obj->getObjectClass()), c.obj->getPartId(), hits, misses, wrong, overlap);
    }
    busy = false;
}

struct PickCandidate { GameObjectPtr obj; float depth; int priority; };

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

// All gameplay point picks share filtering, bounds and a depth-ordered stack.
// Policy ranks live enemies, friendlies, wrecks, then neutral props.
GameObjectPtr pickGameplayObjects(long x, long y, GameObjectPtr* list, long count,
                                 bool skipDisabled, long excludeCommander, bool terrainOnly) {
    if (!eye || !list || x < 0 || y < 0 || x >= Environment.screenWidth || y >= Environment.screenHeight) return nullptr;
    std::vector<PickCandidate> stack;
    for (long i = 0; i < count; ++i) {
        auto* obj = list[i];
        if (!gameplayPickEligible(obj, skipDisabled)
            || (excludeCommander != -1 && obj->getCommanderId() == excludeCommander)
            || (terrainOnly && obj->isMover())) continue;
        PickRect rect;
        if (!gameplayPickRect(obj, rect) || !rect.contains(x, y)) continue;
        const float pad = gameplayPickPad();
        const bool coreHit = x >= rect.left+pad && x <= rect.right-pad
            && y >= rect.top+pad && y <= rect.bottom-pad;
        const auto delta = obj->getPosition() - eye->getCameraOrigin();
        // Forgiveness must not steal a direct hit on another object's bounds.
        stack.push_back({obj, glm::dot(delta, delta), pickPriority(obj) + (coreHit ? 0 : 5)});
    }
    std::stable_sort(stack.begin(), stack.end(), [](const auto& a, const auto& b) {
        if (a.depth != b.depth) return a.depth < b.depth;
        return a.obj->getHandle() < b.obj->getHandle();
    });
    GameObjectPtr best = nullptr;
    int priority = 10;
    for (const auto& candidate : stack) {
        if (candidate.priority < priority) { best=candidate.obj; priority=candidate.priority; }
    }
    return best;
}
} // namespace


GameObjectPtr GameObjectManager::findObjectByMouse(long x, long y, GameObjectPtr* list, long size, bool skipDisabled) {
    return pickGameplayObjects(x, y, list, size, skipDisabled);
}

GameObjectPtr GameObjectManager::findMoverByMouse(long x, long y, long commanderId, bool skipDisabled) {
    return pickGameplayObjects(x, y, objList ? &objList[1] : nullptr, getMaxMovers(), skipDisabled, commanderId);
}

GameObjectPtr GameObjectManager::findTerrainObjectByMouse(long x, long y, bool skipDisabled) {
    return pickGameplayObjects(x, y, objList ? &objList[1] : nullptr, getMaxObjects(), skipDisabled, -1, true);
}

GameObjectPtr GameObjectManager::findObjectByMouse(long x, long y) {
    gameplayPickBattery(*this);
    return pickGameplayObjects(x, y, objList ? &objList[1] : nullptr, getMaxObjects(), false);
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
    return pickGameplayObjects(x,y,own.data(),long(own.size()),true);
}
