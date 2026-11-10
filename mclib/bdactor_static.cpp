// BldgAppearance / TreeAppearance: BldgAppearance static registry: eligibility, registerStatic, touch*, rebake gates, invalidate, destroy.
// Declared in the class headers; shared names in bdactor_internal.h.
#include <atomic>
#include <utility>
extern uint32_t mc2LightSlotBakedHighWater();
#include "bdactor_internal.h"
#include "gos_static_prop_registry.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"
#include "battle_bench_sink.h"
#include "building_static_eligibility.h"
#include "../GameOS/gameos/static_caster_set.h"   // shadow-bake caster events

// BLDG-TYPE-ANIM-GATE-FIX-1 kill-switch.
// Unset/1 = new: idle animatable-type buildings eligible for static path.
// 0       = legacy: bldgTypeHasAnimations disqualifies entire type.
static bool s_bldgTypeAnimStaticEligible = mc2::gate::get<"MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE">();

// Stage 3.D: BldgAppearance static-registry path. Mirror of TreeAppearance's
// IsStaticNow / touch / invalidateStaticRegistration / isStaticEligible.
// Registry-replay eligibility for buildings is stricter than for trees because
// buildings have many dynamic states (animation, spin, flash, destruct FX).
// memory/bldg_animation_lod_swap_unsafe.md WAS the load-bearing reason: animated
// types share LOD-0 node-index state across instances, so replaying a recipe
// for an animated building drives the wrong node when LOD swaps.
// ENV-DROP-CLOSE-1 (2026-07-30) -- THAT MECHANISM IS DEAD TODAY. Building LOD is
// pinned to 0: every currentLOD write in this file assigns 0 (  
//    ), no path assigns nonzero, and the LOD-select
// body was deleted from recalcBounds (see the LATENT HAZARD note). There
// is no LOD swap for the shared shapeId cache to be wrong about. The cache defect
// itself is unfixed (msl.cpp shapeIdsSet is still per-type), so the hazard is
// LATENT, not gone -- it re-arms the moment the TEMP LOD-0 pin is lifted.
// Do NOT cite it to explain a present-day building/turret symptom; several lanes
// were sent chasing this dead path on 2026-07-30.

namespace {
	// Type-level "any animation defined for this building type". Even if the
	// current instance isn't actively animating, an animated TYPE is excluded
	// from the static path because LOD swap could surface the animation later
	// and break the cached recipe. (Unreachable today -- LOD-0 pin, see above --
	// and additionally the exclusion is default-OFF: the caller applies it only
	// when MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE=0.)
	bool bldgTypeHasAnimations(const BldgAppearanceType* t) {
		if (!t) return false;
		for (const auto & bdAnimDataElem : t->bdAnimData) {
			if (bdAnimDataElem != nullptr) return true;
		}
		return false;
	}

	bool bldgGestureHasAnimation(const BldgAppearanceType* t, long gestureId) {
		if (!t) return false;
		if (gestureId < 0 || gestureId >= MAX_BD_ANIMATIONS) return false;
		return t->bdAnimData[gestureId] != nullptr;
	}
} // anon namespace

bool BldgAppearance::isStaticEligible() const
{
	// Type-level disqualifiers.
	if (!appearType)        return false;
	if (mc2WorldMarkerAppearance(appearType->name)) return false;
	if (appearType->spinMe) return false;
	// BLDG-TYPE-ANIM-GATE-FIX-1 (nifty): legacy type-level animation-capacity
	// check. Kill-switch MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE=0 restores old
	// behaviour. When enabled (default), the bdAnimationState guard below is
	// sufficient.
	if (!s_bldgTypeAnimStaticEligible && bldgTypeHasAnimations(appearType))
		return false;
	// Instance-level disqualifiers.
	if (drawFlash)          return false;
	if (destructFX)         return false;
	if (mc2::building_static::activityNeedsUpdate(isActivitying, static_cast<bool>(activity), static_cast<bool>(activity1)))
		return false;
	// MODEL-OVERRIDE admission fix (SEAMPROBE-1, override branch): a renderOnly
	// override prop substitutes a static .glb render shape that carries NO MC2
	// bdAnimData; setAnimation() on it is a no-op. Stock buildings get a default
	// idle gesture (bdAnimationState=0) set during init/update even when the
	// TYPE has no animations — which spuriously trips the guard below for an
	// override prop, leaving its static recipe permanently un-admitted
	// (markVisible never fires -> no substrate record -> never drawn). When this
	// is an override-backed type with no real animations, the cached recipe is a
	// faithful representation regardless of bdAnimationState, so skip the guard.
	// Strictly scoped to bldgRenderShape!=nullptr: stock path byte-identical.
	// Also require no rotational node: turrets/radars rotate via rotationalNodeId
	// (code-driven), not bdAnimData, so bldgTypeHasAnimations() misses them.
	// A renderOnly static .glb cannot represent a rotating part -> never admit.
	const bool hasRotationalNode =
		appearType->rotationalNodeId[0] != '\0'
		&& S_stricmp(appearType->rotationalNodeId, "NONE") != 0;
	if (hasRotationalNode) return false;
	const bool overrideStatic =
		appearType->bldgRenderShape && !bldgTypeHasAnimations(appearType) && !hasRotationalNode;
	const bool activeGestureHasAnimation =
		unitAnim().bdAnimationState != -1 && bldgGestureHasAnimation(appearType, unitAnim().bdAnimationState);
	if (activeGestureHasAnimation && !overrideStatic) return false;  // currently animating
	return true;
}

// Task 5 (Track B): mission-load bulk static-prop registration.
// Called from GameObjectManager::registerStaticPropsForMissionLoad() after
// primeTerrainObjectsForMissionLoad() has set position/rotation on every
// actor. Populates shapeToWorld matrices via TransformMultiShape_PositionsOnly,
// builds a recipe batch per leaf via buildRecipeFromShape, and registers with
// GpuStaticPropRegistry. HC-1: writes directly to typed staticReg member.
void BldgAppearance::registerStatic() {
	// [SEAMPROBE] stage 2: registerStatic entry + which early-return fires.
	// Gate on the override target (hangar) to avoid log spam.
	const bool seamProbe = (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">())
		&& appearType && appearType->bldgRenderShape;
	if (seamProbe)
		mc2::fmt::print_err("[SEAMPROBE] bldg registerStatic ENTER name={} registered={:d} hasShape={:d} eligible={:d} renderShape={:016X}\n",
			mc2::fmt::safe(appearType->name), mc2::fmt::as_i32(static_cast<int>(staticReg.registered)), mc2::fmt::as_i32(static_cast<int>((bldgShape!=nullptr))),
			mc2::fmt::as_i32(static_cast<int>(isStaticEligible())),
			mc2::fmt::as_ptr(static_cast<void*>(appearType->bldgRenderShape.get()))), fflush(stderr);
	if (staticReg.registered) {
		return;
	}
	if (!bldgShape) {
		return;
	}
	if (!isStaticEligible()) {
		if (seamProbe) mc2::fmt::print_err("[SEAMPROBE] bldg registerStatic ABORT name={} reason=!isStaticEligible\n", mc2::fmt::safe(appearType->name)), fflush(stderr);
		return;
	}

	// MODEL-OVERRIDE: register the RENDER shape geometry into s_typeIndex HERE,
	// in the mission-load pre-pass (runs before StaticProp::finalizeGeometry).
	// An override type-shape otherwise first registers late in per-instance
	// init() -> buildRecipeFromShape() below misses s_typeIndex -> aborts to the
	// CPU first-render fallback, which is catastrophic with ~1k tree instances
	// (per-instance pool copies overflow). Idempotent for stock (already
	// registered). getBldgRenderShape == stock when no override is present.
	const bool _regIsOverride = (appearType->bldgRenderShape != nullptr);
	appearType->registerRenderShapes();

	// G5 RENDER-PATH OBSERVABILITY (descriptive; env-gated; ZERO behavior change):
	// one line per registered static-prop type recording which render path it takes.
	// path = override_multidraw (cooked/override geometry, layer-0 route) vs legacy_static.
	// Full MeshCapability bits come once the engine reads the cooked manifest.json (later
	// integration); for now we log what the seam knows. Schema-grep: \[RENDER_PATH v[0-9]+\].
	if (mc2::gate::get<"MC2_RENDER_PATH">()) {
		mc2::fmt::print_err("[RENDER_PATH v1] key=staticprop:{} isOverride={:d} path={} pools={}\n", mc2::fmt::safe(appearType->name), mc2::fmt::as_i32(_regIsOverride ? 1 : 0), mc2::fmt::safe(_regIsOverride ? "override_multidraw" : "legacy_static"), mc2::fmt::safe("skipped"));
		fflush(stderr);
	}

	if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">() && appearType->bldgRenderShape) {
		TG_TypeMultiShape* rs = appearType->bldgRenderShape.get();
		mc2::fmt::print_err("[MODOVERRIDE_TRACE] bldg registerStatic name={} renderShape={:016X} numShapes={:d} leaf0={:016X} leaf1={:016X} finalized_guess(typeNodes_above)\n", mc2::fmt::safe(appearType->name), mc2::fmt::as_ptr(static_cast<void*>(rs)), mc2::fmt::as_i32(rs->GetNumShapes()), mc2::fmt::as_ptr(rs->GetNumShapes() > 0 ? static_cast<void*>(rs->GetTypeNode(0)) : nullptr), mc2::fmt::as_ptr(rs->GetNumShapes() > 1 ? static_cast<void*>(rs->GetTypeNode(1)) : nullptr));
		fflush(stderr);
	}

	// Compute transform — same coordinate convention as BldgAppearance::update().
	// Mission-load registration precedes the first gameplay grounding update.
	float yaw = rotation * DEGREES_TO_RADS;
	glm::quat rot;
	rot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
	glm::vec3 xlatPosition;
	xlatPosition.x = -position.x;
	xlatPosition.y = TerrainRuntime::sampleVisualHeight(position);
	xlatPosition.z = position.y;
	bldgShape->TransformMultiShape_BuildRecipe(&xlatPosition, &rot);

	// TERRAIN-DECAL-SLICE-0A — CLIFF_WALL face frame (mesh-decal system).
	// When this appearance is the
	// cliff-wall decal type ("MarbleCliff"), build an EXPLICIT face-frame mat4
	// for the placed instance INSTEAD of the yaw-only recipe transform. This is
	// the CLIFF_WALL conform mode from docs/superpowers/specs/
	// 2026-07-03-terrain-mesh-decal-system-design.md: up = world vertical,
	// facing = outward (downslope) horizontal cliff normal, tangent = contour.
	// A small outward offset lifts the wall off the terrain face to avoid
	// z-fighting. Row-vector convention: world = shapeRow * M, so each 3x3 ROW
	// (X_Axis/Y_Axis/Z_Axis) holds the world direction of the mesh's local axis
	// (GetLocalForward reads (FORWARD_AXIS, *)). The generated wall GLB authors
	// width->local X, height->local Y, relief/face->local Z.
	//   Shape-world axes (from xlatPosition remap): Xsw = -worldX, Ysw = up,
	//   Zsw = worldY. Terrain normal (nx,ny,nz){world, nz up} maps to shape-world
	//   (-nx, nz, ny); its horizontal projection (-nx,0,ny) is the outward facing.
	// TERRAIN-DECAL-SLICE-0B/0C — placement knobs live in CliffDecalTuning (mutable,
	// seeded from the MC2_TERRAIN_DECAL_* env defaults). The face-frame math is factored into CliffDecalTuning::buildCliffWall
	// Matrix(), shared by this registration path and the live ImGui tuning panel.
	bool useCliffWallFrame = false;
	glm::mat4 cliffWallXform;
	glm::vec3 decalNAcc;  // captured for the live-update panel
	if (appearType && appearType->name &&
	    _stricmp(appearType->name, "MarbleCliff") == 0) {
		// Sample the terrain normal at the placement site (average the 4
		// enclosing corners via small offsets for stability — the raw normal is
		// per-triangle/faceted). worldUnitsPerVertex ~ heightfield cell size.
		glm::vec3 nAcc; nAcc = glm::vec3(0.0f);
		const float d = 32.0f; // ~half a heightfield cell for a 4-corner average
		const float dx[4] = { -d,  d, -d,  d };
		const float dy[4] = { -d, -d,  d,  d };
		Terrain* landPtr = land; // file-scope terrain pointer
		for (int c = 0; c < 4; ++c) {
			glm::vec3 sp = position;
			sp.x += dx[c]; sp.y += dy[c];
			glm::vec3 nc = landPtr ? landPtr->getTerrainNormal(sp)
			                             : glm::vec3(0.0f, 0.0f, 1.0f);
			nAcc.x += nc.x; nAcc.y += nc.y; nAcc.z += nc.z;
		}
		decalNAcc = nAcc;
		// Build the initial CLIFF_WALL face frame from the current knobs.
		CliffDecalTuning::buildCliffWallMatrix(
		    xlatPosition, nAcc, CliffDecalTuning::knobs(), cliffWallXform);
		useCliffWallFrame = true;
	}

	// Build per-leaf recipe batch.
	// Use public GetNumShapes()/GetShapeRec() — numTG_Shapes/listOfShapes are protected.
	std::vector<GpuStaticPropInstance> batch;
	const uint32_t tintByte = mc2::prop_light::packWord(mc2PropSlotI(bldgShape->getCachedGpuLightIndex(), land && eye ? land->getTerrainLight(position) : 1.0f), 0);
	const int numShapes = static_cast<int>(bldgShape->GetNumShapes());
	batch.reserve(numShapes);
	// 2026-05-10 diag: count outcomes for buildings to see why so few reach count>1.
	int diag_total = 0, diag_skip_processMe = 0, diag_skip_helper = 0,
	    diag_skip_unreg = 0, diag_added = 0;
	for (int i = 0; i < numShapes; ++i) {
		++diag_total;
		const TG_ShapeRec* rec = bldgShape->GetShapeRec(i);
		if (!rec || !rec->processMe || !rec->node) { ++diag_skip_processMe; continue; }
		TG_Shape* child = rec->node.get();
		// 2026-05-10 fix: skip non-SHAPE_NODE children (helpers, spotlight
		// emitters, animation roots) — mirrors GpuStaticPropBatcher::submitMultiShape
		// at gos_static_prop_batcher.cpp. Without this, the loop hits a
		// helper, buildRecipeFromShape's static_cast<TG_TypeShape*>(myType)
		// produces a pointer that isn't in s_typeIndex, returns false, and
		// the entire building registration aborts on its FIRST helper. This
		// is why mc2_10 buildings (warehouses, S_admin, control) previously
		// failed registerStatic and never reached the substrate.
		if (!child->IsShapeNode()) { ++diag_skip_helper; continue; }
		// CONE-LEAF-NAME-DIAG (batch-8C, MC2_CONE_LEAF_DUMP, default OFF): dump every
		// static-prop recipe leaf's real node name + flags. cone_glow's diag emits a
		// node INDEX only, so before this there was no
		// build-free way to learn a static-prop leaf's NAME. That gap blocked the
		// batch-7/8 "which leaf draws the 212019 searchlight wedge" question for two
		// sessions. Answer (this dump, mc2_10): the wedge is the standalone `Spotlight`
		// prop, whose leaves are 'L_SpotLight'/'L_SpotRotar'/'L_SpotBattery' — all
		// isSpot=0 because GetIsSpotlight() keys on the EXACT "SpotLight_" 10-char prefix
		// and "L_SpotLight" does not match it. Building-attached ghost cones
		// ('Spotlight_Building01', 'SpotLight_Building', 'Spotlight_SensorCon',
		// 'Spotlight_TurretCon') DO get isSpot=1 and ARE skipped below — the intended
		// GHOST-CONE-RECIPE-SKIP-1 target. See .claude/DEFERRED-BATCH-8C-STATUS.md.
		// GHOST-CONE-RECIPE-SKIP-1 (batch-7 items 4+5): SpotLight_ cone leaves
		// must NOT enter the persistent-bucket recipe. submitMultiShape has
		// skipped them unconditionally since T3.1 (gos_static_prop_batcher.cpp
		// ~5006: cones are replaced by real TG_Lights + the additive cone
		// glow), but THIS registerStatic recipe path still baked the authored
		// cone MESH into the cached record — every static building with a
		// searchlight drew its cone as an OPAQUE LIT tan wedge (the user's
		// "ghost light" floating wedge + the small wedges at every searchlight
		// housing; proven by A/B: wedge gone with
		// MC2_STATIC_PROP_PERSISTENT_BUCKETS=0, unchanged
		// by MC2_LIGHT_CONE_GLOW=0). The bit-2 instanceFlag it used to set was
		// already dead (shader read deleted in T3.2).
		// Killswitch MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP=0 restores the bake.
		// CONE-LEAF-NAME INVARIANT (batch-8C -- DO NOT WIDEN THIS PREDICATE):
		// GetIsSpotlight() is TRUE only for leaves whose node name carries the exact
		// 10-char "SpotLight_" prefix (tgl.cpp, case-insensitive). The mc2_10
		// saved_212019 "cream wedge" belongs to the standalone `Spotlight` prop (leaf
		// 'L_SpotLight', a TEXTURED 16-vert lamp head, bitmap L_Spotlight.tga), which is
		// NOT "SpotLight_"-prefixed, so it is legit geometry this skip must leave alone.
		// Do NOT broaden the test to "SpotLight"/"L_Spot"/a cone-shape heuristic to chase
		// that wedge: it would EAT the real searchlight-lamp mesh (and other L_Spot* /
		// LitWin_Spot* leaves). The wedge's wrong cream look is the SAME unlit-static-prop
		// root as the batch-8 Item A/B far-backdrop artifact (unlit geometry showing raw
		// art colour); fix it by LIGHTING that pass, not by skipping the prop. Evidence +
		// the MC2_CONE_LEAF_DUMP census: .claude/DEFERRED-BATCH-8C-STATUS.md.
		{
			static const bool s_recipeSpotSkip = mc2::gate::get<"MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP">();
			if (s_recipeSpotSkip && child->GetIsSpotlight()) { ++diag_skip_helper; continue; }
		}
		uint32_t flags = 0;
		if (child->GetLightsOut())   flags |= (1u << 0);
		if (child->GetIsWindow())    flags |= (1u << 1);
		if (child->GetIsAlphaTest())   flags |= (1u << 2);   // FOLIAGE-ALPHAFLAG-1: kFlagIsAlphaTest, as submitMultiShape sets it
		// rec->shapeToWorld is LinearMatrix4D; convert to Matrix4D for buildRecipeFromShape().
		glm::mat4 xform = glm::mat4(rec->shapeToWorld);
		// TERRAIN-DECAL-SLICE-0A: replace the yaw-only leaf transform with the
		// explicit CLIFF_WALL face frame when the decal gate armed this type.
		// Uniform scale only (Slice 0) — the wall GLB carries its own world-scale
		// geometry, so the face frame is a pure rotation+translation (no scale).
		if (useCliffWallFrame) {
			xform = cliffWallXform;
			// TERRAIN-DECAL-FILL-1: tag this decal leaf so static_prop.frag applies
			// the shadow-side ambient/fill floor (u_terrainDecalFill) ONLY to the
			// cliff decal — faces pointing away from the sun otherwise render as a
			// black void (static-prop lighting is max(N·L,0) with no ambient floor).
			// Bit 3 mirrors kFlagDecalFill in static_prop.frag; no other static prop
			// ever sets it, so all other props stay byte-identical.
			flags |= (1u << 3);
		}
		// LIGHT-BILLBOARD-GPU-1 (master-FAIL #8): bit-4 tags beacon/runway glow
		// cards so static_prop.vert rotates the quad to face the camera (bit-5
		// = debug exaggeration). This recipe path is the live one for
		// persistent statics, so it MUST emit the same bit as
		// GpuStaticPropBatcher::submitMultiShape — a split here would fix only
		// half the airbase light population. Gate 0 => no bit => byte-identical.
		if (child->GetIsLightBillboard()) {
			const int lbMode = gos_lightBillboardFacingMode();
			if (lbMode != 0) {
				flags |= (1u << 4);
				if (lbMode == 2) flags |= (1u << 5);
			}
		}
		GpuStaticPropInstance inst;
		if (!GpuStaticPropBatcher::instance().buildRecipeFromShape(
				child, xform,
				static_cast<uint32_t>(child->GetARGBHighlight()),
				static_cast<uint32_t>(child->GetFogRGB()),
				flags, &inst)) {
			++diag_skip_unreg;
			if (seamProbe)
				mc2::fmt::print_err("[SEAMPROBE] bldg buildRecipe MISS name={} leaf={:d} child={:016X} node={} (override type not in s_typeIndex) -> ABORT\n",
					mc2::fmt::safe(appearType->name), mc2::fmt::as_i32(i), mc2::fmt::as_ptr(static_cast<void*>(child)), mc2::fmt::safe(child->getNodeName())), fflush(stderr);
			return;  // unregistered type — abort; first-render fallback covers it
		}
		inst.actorTint = tintByte;
		if (seamProbe)
			mc2::fmt::print_err("[SEAMPROBE] bldg buildRecipe HIT name={} leaf={:d} child={:016X} node={} typeID={:d}\n",
				mc2::fmt::safe(appearType->name), mc2::fmt::as_i32(i), mc2::fmt::as_ptr(static_cast<void*>(child)), mc2::fmt::safe(child->getNodeName()), mc2::fmt::as_u32(inst.typeID)), fflush(stderr);
		// EDITOR-STATIC-RECIPE-FROM-TYPE-1: the recipe is built from the INSTANCE
		// bldgShape (a CreateFrom copy, bdactor.cpp). For a building placed at
		// runtime in the editor, that copy's leaf child->myType registers AFTER the
		// one-shot finalizeGeometry() latch -> it gets a typeID in s_typeIndex but
		// its geometry is NOT in the immutable VBO (batcher_getTypeDrawInfo
		// indexCount==0) -> the draw replays an empty type -> invisible body (shadow
		// still draws from the correct worldCenter record). Re-point the recipe at the
		// canonical TYPE render-shape leaf (getBldgRenderShape, primed by
		// primeAllBuildingAppearanceTypes BEFORE finalize, so it carries real VBO
		// geometry); the instance still supplies transform/light/flags. Editor-only:
		// in-game the instance leaves register pre-finalize, so this is a no-op. Guard:
		// only re-point when the canonical leaf resolves to a typeID that actually has
		// finalized geometry, else leave inst.typeID untouched.
		{
			extern bool InEditor;   // mech3d.cpp
			if (InEditor && appearType) {
				TG_TypeMultiShape* typeRS = appearType->getBldgRenderShape(0);
				if (typeRS && i < typeRS->GetNumShapes()) {
					TG_TypeNodePtr typeLeaf = typeRS->GetTypeNode(i);
					uint32_t canonId = 0, idxc = 0, fi = 0, ic = 0; int32_t bv = 0;
					const bool gotId = typeLeaf && batcher_typeIdForTypeShape(static_cast<const void*>(typeLeaf), &canonId);
					const bool gotGeo = gotId && batcher_getTypeDrawInfo(canonId, &idxc, &fi, &bv, &ic);
					if (gotGeo && idxc > 0) {
						inst.typeID = canonId;
					}
				}
			}
		}
		batch.push_back(inst);
		++diag_added;
	}
	// 2026-05-10 diag: env-gated per-building outcome. MC2_BLDG_REG_TRACE=1.
	{
		static const bool s_trace = false;
		static int s_loggedCount = 0;
		if (s_trace && s_loggedCount < 80) {
			++s_loggedCount;
			mc2::fmt::print_err("[BLDG_REG_DIAG v1] sample=first80 appearType={} numShapes={:d} total={:d} processMe_skip={:d} helper_skip={:d} unreg_skip={:d} added={:d}\n", mc2::fmt::safe((appearType ? appearType->name : "<null>")), mc2::fmt::as_i32(numShapes), mc2::fmt::as_i32(diag_total), mc2::fmt::as_i32(diag_skip_processMe), mc2::fmt::as_i32(diag_skip_helper), mc2::fmt::as_i32(diag_skip_unreg), mc2::fmt::as_i32(diag_added));
			fflush(stderr);
		}
		(void)diag_total; (void)diag_skip_processMe; (void)diag_skip_helper;
		(void)diag_skip_unreg; (void)diag_added;
	}
	if (batch.empty()) {
		return;
	}

	int32_t regIdx = -1;
	(void)GameAdapters::StaticProp::syncStaticProp(
		bldgShape, batch.data(), batch.size(), &regIdx);
	if (seamProbe)
		mc2::fmt::print_err("[SEAMPROBE] bldg syncStaticProp name={} batchSize={:d} regIdx={:d} firstTypeID={:d}\n",
			mc2::fmt::safe(appearType->name), mc2::fmt::as_size(batch.size()), mc2::fmt::as_i32(static_cast<int>(regIdx)),
			mc2::fmt::as_u32(batch.empty()?0xFFFFFFFFu:batch[0].typeID)), fflush(stderr);
	if (regIdx >= 0) {
		staticReg.registered  = true;
		staticReg.shape       = bldgShape;
		staticReg.recipeIndex = regIdx;
		mc2shadow::staticCasterSet().ownerRegistered(static_cast<const Appearance*>(this), regIdx);
		// TERRAIN-DECAL-SLICE-0C: capture this decal's live-update context so the
		// "Cliff Decal" ImGui panel can recompute + re-upload its transform when a
		// slider moves. Only for the CLIFF_WALL decal (useCliffWallFrame); other
		// buildings never register a live decal target.
		if (useCliffWallFrame) {
			CliffDecalTuning::captureDecalContext(regIdx, xlatPosition, decalNAcc);
		}
		// SHADOW-STATIC-BUILDINGS-2: tag this recipe Building so the world-fixed
		// static shadow map replays it (visibility-independent; not per-frame buckets).
		GpuStaticPropRegistry::setRecipePopulation(regIdx, GpuStaticPropPopulation::Building);
		// H4 fix (2026-05-06): registerStatic only ran TransformMultiShape_BuildRecipe
		// (positions only); leaf TG_Shape::lightData_ is still default/zero. Without
		// this flag, IsStaticNow() returns true on the very next frame, UPDATE_SKIP
		// fires, touch() re-submits the empty lightData_ via addLightDataStructure
		// → all-zero lighting slot → black actor. Setting needsFullBakeNextFrame
		// uses the existing late-reg recovery mechanism: IsStaticNow() returns
		// false until the next update() runs a full TransformMultiShape and clears
		// the flag (bdactor.cpp). One-time cost of one extra update() per
		// mission-load-registered actor; recovers the UPDATE_SKIP perf win
		// every frame thereafter. Spec:
		// docs/superpowers/specs/2026-05-06-update-skip-touch-residual-debug-strategy.md
		needsFullBakeNextFrame = true;
		// [G1-STATIC-EAGER-LIGHT v1] Eager fallback: write a non-zero ambient slot so
		// lightData_[recipeIndex] is never pure-zero if this prop is never on screen.
		// CANNOT call CacheGpuLightData here -- worldLights[] are not populated until
		// the first update() (SetLightList requires a live eye/camera). Instead we seed
		// a minimal ambient record (numLights_=1, dim neutral color) so GPU-Scene cull
		// can upload this slot without blacking the prop. The first real update() calls
		// mc2CacheOrBakeStaticGpuLight -> MISS path -> CacheGpuLightData + mc2WriteStaticLightSlot
		// which overwrites this fallback with the terrain-correct light. Gate default-OFF;
		// unset = byte-identical to existing behavior.
		{
			extern void mc2SetBakedStaticLight(int32_t, const TG_HWLightsData&);
			extern void mc2WriteStaticLightSlot(int32_t, const TG_HWLightsData&);
			static const bool s_eagerLightBake = mc2::gate::get<"MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE">();
			if (s_eagerLightBake) {
				TG_HWLightsData fallback;
				// Minimal ambient: one dim neutral-white light. Overwritten on first
				// in-view update() by the terrain-correct bake (MISS path). Non-zero
				// numLights_ is the sentinel the zero-probe checks.
				fallback.numLights_ = 1;
				fallback.lightColor[0][0] = 0.3f;
				fallback.lightColor[0][1] = 0.3f;
				fallback.lightColor[0][2] = 0.3f;
				fallback.lightColor[0][3] = 1.0f;
				mc2SetBakedStaticLight(regIdx, fallback);
				mc2WriteStaticLightSlot(regIdx, fallback);
			}
	}
}
}

bool BldgAppearance::isStaticRegistered() const { return staticReg.registered; }

int32_t BldgAppearance::getStaticRecipeIndex() const {
    return staticReg.registered ? staticReg.recipeIndex : -1;
}

// STATIC-REG-PREWARM-QUEUE-1: mission-load off-screen light bake.
// Called after finalizeGeometry() + eye init, before the first update() frame.
// Purpose: drain the H4 latch (needsFullBakeNextFrame=true) that registerStatic()
// arms, using the permanent SSBO path (mc2CacheOrBakeStaticGpuLight → MISS →
// mc2WriteStaticLightSlot). Off-screen props that never reach render() will
// have a valid cached light slot and will pass stableLightSkipEligible from
// frame 1 onward, eliminating the rej_no_static_reg bucket on mc2_24.
// The approach mirrors the existing gpuEligible non-full-bake branch in update():
//   SetLightList → TransformMultiShape_HierarchyOnly → mc2CacheOrBakeStaticGpuLight
// We re-use the static mc2CacheOrBakeStaticGpuLight() declared above in this TU.
bool BldgAppearance::prewarmStaticLightBake(Camera* cam)
{
	if (mc2::overlap::onSimWorker()) return false;
	mc2::overlap::assertMainThread("BldgAppearance::prewarmStaticLightBake");
	// Gate: only bake objects that are registered and have the latch set.
	if (!staticReg.registered || staticReg.recipeIndex < 0)
		return false;
	if (!needsFullBakeNextFrame)
		return false;
	if (!bldgShape)
		return false;
	if (!cam || cam->getNumLights() == 0)
		return false;

	// Populate shapeToWorld hierarchy (no full pool bake — mirrors gpuEligible branch).

	// Build xlatPosition/rot from the appearance world position/rotation.
	// Same coordinate transform as BldgAppearance::update (line ~3281):
	glm::vec3 xlatPosition;
	mc2ToRender(xlatPosition, position);
	xlatPosition.y += TerrainRuntime::sampleVisualHeight(position) - TerrainRuntime::sampleGameplayHeight(position);

	float yaw = rotation * DEGREES_TO_RADS;
	glm::quat rot;
	rot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));

	// Hierarchy-only walk: fills shapeToWorld matrices without touching TGL pools.
	// Safe to call from mission load: no GL calls, no frame-pool alloc.
	bldgShape->TransformMultiShape_HierarchyOnly(&xlatPosition, &rot);

	// Bake light into permanent SSBO slot (MISS path: CacheGpuLightData + mc2WriteStaticLightSlot).
	mc2CacheOrBakeStaticGpuLight(bldgShape, staticReg.registered, staticReg.recipeIndex, cam->lightContext());
	staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();
	if (staticReg.lightDataIndex != 0xFFFFFFFFu && staticReg.registered)
		GpuStaticPropRegistry::staticPropSetActorTint(staticReg.recipeIndex, mc2::prop_light::packWord(mc2PropSlotI(staticReg.lightDataIndex, land && eye ? land->getTerrainLight(position) : 1.0f), 0));

	if (staticReg.lightDataIndex != 0xFFFFFFFFu) {
		staticReg.hasValidStaticLight = true;
		needsFullBakeNextFrame = false;
		return true;  // baked successfully
	}
	return false;  // gpu objects not enabled or CacheGpuLightData early-returned
}

void BldgAppearance::recordStaticUpdateReason() const
{
	// Count full presentation updates, not the many eligibility probes in render/registration.
	// Eligibility precedes registration here so an unregistered animated actor is attributed
	// to animation rather than hiding every disqualifier in the unregistered bucket.
	using namespace mc2::battlebench;
	Field reason = BldgUpdateOther;
	if (!appearType) reason = BldgUpdateType;
	else if (mc2WorldMarkerAppearance(appearType->name)) reason = BldgUpdateMarker;
	else if (appearType->spinMe) reason = BldgUpdateSpin;
	else if (!s_bldgTypeAnimStaticEligible && bldgTypeHasAnimations(appearType)) reason = BldgUpdateLegacyAnim;
	else if (drawFlash) reason = BldgUpdateFlash;
	else if (destructFX) reason = BldgUpdateDestruct;
	else if (mc2::building_static::activityNeedsUpdate(isActivitying, static_cast<bool>(activity), static_cast<bool>(activity1))) reason = BldgUpdateActivity;
	else if (appearType->rotationalNodeId[0] != '\0' && S_stricmp(appearType->rotationalNodeId, "NONE") != 0) reason = BldgUpdateRotation;
	else if (bldgGestureHasAnimation(appearType, unitAnim().bdAnimationState)) reason = BldgUpdateAnimation;
	else if (!staticReg.registered) reason = BldgUpdateUnregistered;
	else if (currentLOD != desiredRenderLOD()) reason = BldgUpdateLod;
	else if (staticReg.shape != bldgShape) reason = BldgUpdateShape;
	else if (needsFullBakeNextFrame) reason = BldgUpdateBake;
	add(reason);
}

bool BldgAppearance::IsStaticNow() const
{
	return staticReg.registered
		&& currentLOD == desiredRenderLOD()
		&& staticReg.shape == bldgShape
		&& !needsFullBakeNextFrame
		&& isStaticEligible();
}

void BldgAppearance::touch()
{
	// Keep visual emissions in terrain-object order, including the split path
	// whose serial light-cache commit may already have run before this visit.
	updateFrameEffects();
	// FRAME-JOBS-2F: if touchSerialCommit() already ran this frame (split path active),
	// skip redundant Path B work. When MC2_FRAME_JOBS_TOUCH=0, stamp is never set
	// (0xFFFFFFFFu initial value cannot match any real frame counter).
	if (mc2FrameJobsTouchDiagEnabled()) {
		touchEntryDiagRollFrame(g_mc2FrameCounter.load(std::memory_order_relaxed));
		if (touchSerialCommitFrame == g_mc2FrameCounter.load(std::memory_order_relaxed)) {
			++s_touchEntryDiag.legacy_skipped;
			return;
		}
		++s_touchEntryDiag.legacy_ran_nosplit;
	} else {
		if (touchSerialCommitFrame == g_mc2FrameCounter.load(std::memory_order_relaxed))
			return;
	}

	// BLDG-TYPE-ANIM-GATE-FIX-1: count touch() calls for newly-eligible pop:
	// type has animation data, instance is idle (bdAnimationState==-1), and
	// currently registered (staticReg.registered). Nonzero delta proves fix works.
	// Const read: the non-const accessor would clear animSettled_ every frame.
	if (bldgTypeHasAnimations(appearType) && std::as_const(*this).unitAnim().bdAnimationState == -1 && staticReg.registered)
		s_animTypeIdleNowStatic.fetch_add(1, std::memory_order_relaxed);
	// MC2_STATIC_UPDATE_SKIP defaults TRUE (terrobj.cpp); touch() is the
	// DEFAULT path. update() runs only when the env var is explicitly cleared.
	if (bldgShape) {
		const bool stableLightSkipArmed = mc2StableLightSkipEnabled();
		const uint64_t currentLightEnvGen = mc2StaticLightEnvironmentGeneration(eye);
		const bool stableLightSkipEligible =
			staticReg.registered &&
			staticReg.recipeIndex >= 0 &&
			staticReg.hasValidStaticLight &&
			staticReg.lightDataIndex != 0xFFFFFFFFu &&
			staticReg.lightDataIndex < mc2LightSlotBakedHighWater() &&  // dynamic slots (>= S) die at the next resetLightData
			staticReg.lastLightEnvGen == currentLightEnvGen &&
			!needsFullBakeNextFrame;
		recordStableLightSkipDiag(
			stableLightSkipArmed,
			stableLightSkipEligible,
			!staticReg.registered || staticReg.recipeIndex < 0,
			!staticReg.hasValidStaticLight,
			staticReg.hasValidStaticLight && (staticReg.lastLightEnvGen != currentLightEnvGen),
			needsFullBakeNextFrame,
			staticReg.lightDataIndex == 0xFFFFFFFFu);

		// STABLE-LIGHT-SKIP-BROADEN-1: broaden the LBSS resubmit-skip that
		// touchSerialCommit() takes (bdactor.cpp ~4490) to THIS legacy path. In
		// stock config touch() is the live path (FRAME-JOBS touch split default-OFF),
		// so touchSerialCommit()'s skip never fires and every stable static building
		// pays a redundant ResubmitCachedGpuLightData() per frame. When
		// stableLightSkipEligible the cached lightDataIndex from a prior frame is
		// still valid on the GPU (baked static prefix [0..s_staticLightHighWater)
		// persists frame-to-frame under MC2_LIGHTBAKE — same invariant proof as the
		// serial-commit skip below), so that resubmit is pure redundant recompute.
		// SAFER-THAN-SERIAL-VARIANT: we still call bldgShape->Touch() (one field
		// write advancing lastTurnTransformed) before returning — the non-split
		// path bundles Touch() with the resubmit, whereas the split path runs Touch()
		// in touchWorkerPrepass(). Skipping only the expensive resubmit keeps the
		// legacy CPU-render staleness guard (tgl.cpp) byte-identical. staticReg
		// fields are already correct from the last full update()/resubmit. Gate:
		// mc2StableLightSkipTouchEnabled() (default ON => touch skip preserves registry liveness).
		if (mc2StableLightSkipTouchEnabled() && mc2LightbridgeStableSkipEnabled()
		    && stableLightSkipArmed && stableLightSkipEligible) {
			lbssRecordBldg(/*eligible=*/true, /*skipTaken=*/true,
			               /*blk_idx=*/false, /*blk_gen=*/false, /*blk_noelig=*/false);
			bldgShape->Touch();
			// The skip bypasses the emit/resubmit that stamped cachedFrame_; the static flush drops any
			// range whose cachedFrame_ != currentFrame (and render() already suppressed the legacy draw).
			bldgShape->setCachedFrame(g_mc2FrameCounter.load(std::memory_order_relaxed));
			return;
		}

		// [LIGHTBRIDGE v1] C6 retirement: repoint to the primed 38d8720 slot
		// (zero FNV/memcmp; cachedFrame_ stamped). MISS keeps the legacy
		// resubmit (NOT CacheGpuLightData -- terrain-color-staleness,
		// msl.cpp). MC2_LIGHTBAKE=0 -> legacy path bit-for-bit.
		extern bool mc2LightBakeEnabled();
		extern bool mc2IsBakedStaticLightPresent(int32_t);
		// THREAD-SAFETY CLASSIFICATION: EmitBakedGpuLightData — WORKER_SAFE.
		// (1) Writes only per-instance members cachedGpuLightIndex_ and cachedFrame_
		//     on the TG_MultiShape pointed to by bldgShape (unique per BldgAppearance
		//     instance, never aliased across actors). (2) Makes NO GL calls
		//     (glBufferSubData, glMapBuffer, etc.). (3) The 'baked' struct is
		//     explicitly discarded ((void)baked in msl.cpp) — the slot index IS
		//     recipeIndex, which is per-recipe-constant. Two calls on different shape
		//     instances cannot conflict. ResubmitCachedGpuLightData (the else branch)
		//     also writes only per-instance members on the same hot path; no shared
		//     pool write occurs unless the repoint fast-path misses, in which case it
		//     calls addLightDataStructure (now mutex-protected by s_lightDataMapMu).
		// [LIGHTBRIDGE-BAKED-PROBE-1] probe only — EmitBakedGpuLightData discards baked
		if (mc2LightBakeEnabled()
		    && staticReg.registered && staticReg.recipeIndex >= 0
		    && mc2IsBakedStaticLightPresent(staticReg.recipeIndex)) {
			TG_HWLightsData baked{};
			bldgShape->EmitBakedGpuLightData(staticReg.recipeIndex, baked);
		} else {
			bldgShape->ResubmitCachedGpuLightData();
		}
		// 2026-05-11 per-instance capture: snapshot the just-resubmitted slot
		// for THIS actor before sibling instances of the same multi-type
		// overwrite multi->cachedGpuLightIndex_ in the same update phase.
		staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();
		staticReg.hasValidStaticLight = (staticReg.lightDataIndex != 0xFFFFFFFFu);
		staticReg.lastLightEnvGen = currentLightEnvGen;
		bldgShape->Touch();
	}
}

// FRAME-JOBS-2D: lock-free per-instance prep; runs on worker threads.
// bldgShape->Touch() is lock-free (verified: sets lastTurnTransformed only, tgl.cpp).
// s_animTypeIdleNowStatic.fetch_add is atomic — worker-safe.
void BldgAppearance::touchWorkerPrepass()
{
	if (bldgTypeHasAnimations(appearType) && std::as_const(*this).unitAnim().bdAnimationState == -1 && staticReg.registered)
		s_animTypeIdleNowStatic.fetch_add(1, std::memory_order_relaxed);
	if (bldgShape)
		bldgShape->Touch();
}

// FRAME-JOBS-2D: light-data resubmit; runs serially on main thread after worker join.
void BldgAppearance::touchSerialCommit()
{
	// FRAME-JOBS-2F: stamp this frame unconditionally so touch() (Path B, terrain object
	// loop) returns immediately. Set BEFORE any early returns including stable-skip:
	// if stable-skip fires and we return early, the stamp still prevents Path B from
	// re-doing work that stable-skip correctly decided to omit.
	touchSerialCommitFrame = g_mc2FrameCounter.load(std::memory_order_relaxed);
	if (mc2FrameJobsTouchDiagEnabled()) {
		touchEntryDiagRollFrame(g_mc2FrameCounter.load(std::memory_order_relaxed));
		++s_touchEntryDiag.serial_commit_hits;
	}

	if (!bldgShape) {
		return;
	}
	const bool stableLightSkipArmed = mc2StableLightSkipEnabled();
	const uint64_t currentLightEnvGen = mc2StaticLightEnvironmentGeneration(eye);
	const bool stableLightSkipEligible =
		staticReg.registered &&
		staticReg.recipeIndex >= 0 &&
		staticReg.hasValidStaticLight &&
		staticReg.lightDataIndex != 0xFFFFFFFFu &&
		staticReg.lightDataIndex < mc2LightSlotBakedHighWater() &&  // dynamic slots (>= S) die at the next resetLightData
		staticReg.lastLightEnvGen == currentLightEnvGen &&
		!needsFullBakeNextFrame;
	recordStableLightSkipDiag(
		stableLightSkipArmed,
		stableLightSkipEligible,
		!staticReg.registered || staticReg.recipeIndex < 0,
		!staticReg.hasValidStaticLight,
		staticReg.hasValidStaticLight && (staticReg.lastLightEnvGen != currentLightEnvGen),
		needsFullBakeNextFrame,
		staticReg.lightDataIndex == 0xFFFFFFFFu);

	// [LIGHTBRIDGE-STABLE-SKIP-WIRE-1]: skip Section C (EmitBakedGpuLightData +
	// getCachedGpuLightIndex + staticReg writes) when MC2_LIGHTBRIDGE_STABLE_SKIP=1
	// and this object's baked light slot is known valid for this frame.
	// INVARIANT proof (verified 2026-06-20):
	//   Q1: resetLightData() rebases lightDataStructuresCount to s_staticLightHighWater
	//       (not 0) when MC2_LIGHTBAKE is on. lightData_[0..s_staticLightHighWater) is
	//       NEVER memset'd or cleared — it persists intact frame-to-frame.
	//   Q2: EmitBakedGpuLightData(recipeIndex, ...) sets cachedGpuLightIndex_ =
	//       static_cast<uint32_t>(recipeIndex) — i.e. the permanent static prefix slot.
	//   Q3: staticReg.lightDataIndex = getCachedGpuLightIndex() = recipeIndex, which
	//       is an index into the static prefix [0..s_staticLightHighWater).
	//   Q4: renderLists() uploads lightData_[0..max(lightDataStructuresCount,64)] which
	//       always covers [0..s_staticLightHighWater) since count >= S after rebase.
	//       With MC2_STATIC_LIGHT_UPLOAD_SPLIT the prefix is uploaded once-per-dirty;
	//       suffix is uploaded every frame. Either path keeps [0..S) in GPU memory.
	// Therefore: staticReg.lightDataIndex from a prior frame remains valid on the GPU
	// for stable objects — we can skip resubmit entirely when stableLightSkipEligible.
	// DO NOT remove the else-branch fallback — it handles any frame where light state
	// changed, needsFullBakeNextFrame, or slot was invalidated.
	if (mc2LightbridgeStableSkipEnabled() && stableLightSkipArmed) {
		// FRAME-JOBS-2F Fix-A: stableLightSkipEligible already requires
		// (lightDataIndex != 0xFFFFFFFF) and (lastLightEnvGen == currentLightEnvGen),
		// so blk_idx and blk_gen were always false — removed (always-zero, misleading).
		// skipTaken == stableLightSkipEligible by definition now.
		const bool skipTaken  = stableLightSkipEligible;
		const bool blk_noelig = !stableLightSkipEligible;
		lbssRecordBldg(stableLightSkipEligible, skipTaken, /*blk_idx=*/false, /*blk_gen=*/false, blk_noelig);
		if (skipTaken) {
			bldgShape->setCachedFrame(g_mc2FrameCounter.load(std::memory_order_relaxed));   // see the touch-skip above
			return;
		}
	}

	extern bool mc2LightBakeEnabled();
	extern bool mc2IsBakedStaticLightPresent(int32_t);
	// [LIGHTBRIDGE-BAKED-PROBE-1] probe only — EmitBakedGpuLightData discards baked
	if (mc2LightBakeEnabled()
	    && staticReg.registered && staticReg.recipeIndex >= 0
	    && mc2IsBakedStaticLightPresent(staticReg.recipeIndex)) {
		TG_HWLightsData baked{};
		bldgShape->EmitBakedGpuLightData(staticReg.recipeIndex, baked);
	} else {
		bldgShape->ResubmitCachedGpuLightData();
	}
	staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();
	staticReg.hasValidStaticLight = (staticReg.lightDataIndex != 0xFFFFFFFFu);
	staticReg.lastLightEnvGen = currentLightEnvGen;
	// NOTE: bldgShape->Touch() is in touchWorkerPrepass() (verified lock-free)
}

// Recipe invalidation queues the old leaves for incremental shadow updates.
void BldgAppearance::invalidateStaticRegistration()
{
	markTerrainPresentationDirty();
	if (staticReg.registered && staticReg.recipeIndex >= 0) {
		mc2shadow::staticCasterSet().ownerInvalidated(static_cast<const Appearance*>(this), staticReg.recipeIndex);
		GameAdapters::StaticProp::destroyStaticPropByIndex(staticReg.recipeIndex);
	}
	staticReg = {};
}

void BldgAppearance::destroy (void)
{
	simulationShape_.reset();
	simulationType_ = nullptr;
	simPose->ready = false;
	// Stage 3.D: NULL the registry's RecipeRange::multi pointer before
	// bldgShape is freed below. Mirrors TreeAppearance::destroy ordering.
	invalidateStaticRegistration();

	// STATIC-BLDG-SHADOW-REBAKE-1: cheap insurance -- see the helper's
	// comment. In practice combat-destroyed buildings are already re-armed
	// earlier via setObjStatus(OBJECT_STATUS_DESTROYED); this covers the
	// mission-end/pool-recycle path.

	if ( bldgShape )
	{
		bldgShapeOwner_.reset();
		bldgShape = nullptr;
	}

	if (destructFX)
	{
		destructFX->Kill();
		destructFX.reset();
	}
	
	//Turn the lights off!
	//Need to kill the light source here too!
	if (pointLight)
	{
		if (eye)
			eye->removeWorldLight(lightId,pointLight);

		free(pointLight);
		pointLight = nullptr;
	}

	// (E) T1.5: paired cleanup for SpotLight_-child illumination from T1.4.
	// IMPORTANT (C-r2 M5): bldgShape was deleted above; do NOT call
	// getNodeIdPosition or any bldgShape method here. Use CACHED state
	// (spotlightLights_/spotlightSlotIds_/spotlightNodeIds_) only. Unlike
	// `pointLight` (alloc/free per night/day boundary), spotlightLights_
	// stay allocated for the building's lifetime — this destroy hook is the
	// only cleanup site.
	clearSpotlights();

	if (activity)
	{
		activity->Kill();
		activity.reset();
	}

	if (activity1)
	{
		activity1->Kill();
		activity1.reset();
	}

	releaseAppearanceType(registryGeneration, appearType); // skipped when the registry that issued appearType is gone
}

void BldgAppearance::clearSpotlights()
{
	for (size_t k = 0; k < spotlightLights_.size(); ++k)
	{
		if (eye)
			eye->removeWorldLight(spotlightSlotIds_[k], spotlightLights_[k]);
		free(spotlightLights_[k]);
	}
	spotlightLights_.clear();
	spotlightSlotIds_.clear();
	spotlightNodeIds_.clear();
	spotlightsRegistered_ = false;
}

void BldgAppearance::releaseBldgShape()
{
    clearSpotlights();
    bldgShape->ClearAnimation();
    bldgShapeOwner_.reset();
    bldgShape = nullptr;
}
