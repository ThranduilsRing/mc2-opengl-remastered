#include "unit_animation_tick.h"
// BldgAppearance / TreeAppearance: BldgAppearance::update and the activity/flash helpers.
// Declared in the class headers; shared names in bdactor_internal.h.
#include "bdactor_internal.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"
#include "anim_freeze.h"
#include "../GameOS/gameos/visual_tuning_profile.h"

// LOCAL-LIGHTS-1: authored lamps per SpotLight_ leaf (the stock leaves carry up to 3).
static constexpr int kMaxBeamsPerNode = 8;
// T1.15 SpotLight_ illumination diagnostic — registration probe (bldg class).
// Env-gated per Debug Instrumentation Rule. First-hit is always-on (one stderr
// line per BldgAppearance instance that walks the lazy-init block). Periodic
// summary emits every 600 update() calls when env=1. Demote-not-delete.
static const bool s_spotDiagBldgEnabled = mc2::gate::get<"MC2_SPOT_DIAG">();
static unsigned long s_spotDiagBldgRegistered = 0;   // total lights added
static unsigned long s_spotDiagBldgOverflows  = 0;   // pool-overflow count
static unsigned long s_spotDiagBldgActors     = 0;   // actor first-hits
static unsigned long s_spotDiagBldgCalls      = 0;   // update() call counter

long BldgAppearance::desiredRenderLOD() const
{
	if (!eye || !appearType || !appearType->bldgRenderShape || status != OBJECT_STATUS_NORMAL) return 0;
	const auto origin = eye->getCameraOrigin();
	const float distance = glm::length(position - renderToMc2(origin));
	long lod = 0;
	for (long i = 1; i < MAX_LODS; ++i)
		if (appearType->bldgRenderLods[i] && appearType->lodDistance[i] > 0 && distance >= appearType->lodDistance[i]) lod = i;
	return lod;
}

void BldgAppearance::updateRenderLOD()
{
	const long lod = desiredRenderLOD();
	if (lod == currentLOD || !appearType || !appearType->bldgRenderShape || status != OBJECT_STATUS_NORMAL) return;
	// All LOD geometry and textures were registered before mission finalization.
	invalidateStaticRegistration();
	releaseBldgShape();
	currentLOD = lod;
	bldgShape = appearType->getBldgRenderShape(lod)->CreateFrom();
	rotationalNodeId = -1;
	beenInView = false;
	needsFullBakeNextFrame = true;
}

long BldgAppearance::update (bool animate)
{
	if (simPresentationPass) presentEffects();

	::mc2_object_recon::Scope _recon_bldg_(
		&::mc2_object_recon::g_per_frame.bldg_update_ns,
		&::mc2_object_recon::g_per_frame.bldg_update_calls);
	glm::vec3 xlatPosition;
	glm::quat rot;


    if (!simPresentationPass || !simulationOwned()) {
	if (simulationOwned()) {
		if (unitAnim().lastAnimAdvanceFrame == static_cast<uint32_t>(turn)) { updateSimPose(); return TRUE; }
		unitAnim().lastAnimAdvanceFrame = static_cast<uint32_t>(turn);
	}
	// Recycle the weapon Nodes
	if (!unitAnim().nodeRecycle.empty())
	{
		for (long i=0;i<appearType->numWeaponNodes;i++)
		{
			if (unitAnim().nodeRecycle[i] > 0.0f)
			{
				unitAnim().nodeRecycle[i] -= frameLength;
				if (unitAnim().nodeRecycle[i] < 0.0f)
					unitAnim().nodeRecycle[i] = 0.0f;
			}
		}
	}
	if (animate) mc2::sim::advanceLinearAnimation(unitAnim(), unitAnim().bdFrameRate, appearType->getNumFrames(unitAnim().bdAnimationState), frameLength);

        if (simulationOwned()) { updateSimPose(); return TRUE; }
    }
	updateRenderLOD();
	updateSearchlight();
   	if (appearType->terrainLightRGB != 0xffffffff && (eye->nightFactor > 0.0f) && !forceLightsOut)
   	{
   		if (!pointLight)
   		{
			pointLight = static_cast<TG_LightPtr>(malloc(sizeof(TG_Light)));
   			pointLight->init(TG_LIGHT_TERRAIN);
			pointLight->stockTerrainLamp = appearType->stockTerrainLamp;
   			lightId = eye->addWorldLight(pointLight);
   	
   			pointLight->SetaRGB(appearType->terrainLightRGB);
   			pointLight->SetIntensity(appearType->terrainLightIntensity);
			// Retune only verified stock definitions; mods retain authored lamp values.
			if (appearType->stockTerrainLamp)
   				pointLight->SetFalloffDistances(0.0f, appearType->terrainLightOuterRadius * 0.35f);   // LIGHTS-R3: tighter pools
			else
				pointLight->SetFalloffDistances(appearType->terrainLightInnerRadius, appearType->terrainLightOuterRadius);
   		}
		
		if (pointLight)
		{
			glm::vec3 ourPosition;
			mc2ToRender(ourPosition, position);
	
			pointLight->direction = ourPosition;
	
			glm::mat4 lightToWorldMatrix;
			mc2::math::buildTranslation(lightToWorldMatrix, ourPosition);
			mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(0.0f,0.0f,0.0f));
			pointLight->SetLightToWorld(&lightToWorldMatrix);
			pointLight->SetPosition(&position);
			// LOCAL-LIGHTS-5: a building with a self-lit spinning leaf (the power
			// generator's Coil) throws its light from that leaf, swept around with it.
			if (const int litLeaf = mc2_cone_glow::SelfLitLeaf(bldgShape); litLeaf >= 0)
			{
				glm::vec3 c, axis;
				if (mc2_cone_glow::SelfLitLeafPose(bldgShape, litLeaf, c, axis))
				{
					glm::vec3 orbit = c + glm::vec3(axis.x, axis.y, 0.0f) * 30.0f;
					orbit.z = position.z;
					pointLight->SetPosition(&orbit);
				}
			}
			// LIGHTS-R3: occupancy spill, not a stage light -- several of these overlap in a
			// base centre (owner: "too bright in the middle compared to the ambient").
			const float buildingLightIntensity = appearType->stockTerrainLamp
				? std::min(appearType->terrainLightIntensity * 1.5f, 0.15f) : appearType->terrainLightIntensity;
			pointLight->SetIntensity(buildingLightIntensity * eye->getNightFactor() *
                visualTuning_environment().buildingSpillIntensityScale);
		}
   	}
	else
	{
		//Turn the lights off!
		//Need to kill the light source here too!
		if (pointLight)
		{
			eye->removeWorldLight(lightId,pointLight);
			free(pointLight);
			pointLight = nullptr;
		}
	}

	// (E) T3.1: SpotLight_-child illumination. Distinct from the per-building
	// terrain pointLight above (R7). World position is valid here in update()
	// (not in init() per C-r1 C1). Lazy first-night register; subsequent frames
	// do per-frame SetPosition + active toggle ONLY (R3: no per-frame add/remove).
	// Gate retired in T3.1 (Stage 3 substitutive completion); behavior is now
	// unconditional. See docs/superpowers/plans/2026-05-20-spotlight-real-illumination-plan.md
	if (bldgShape)
	{
		if (!spotlightsRegistered_ && eye && eye->isNight)
		{
			// Public accessors (msl.h GetNumShapes, msl.h GetShapeRec)
			// because BldgAppearance is NOT in the TG_MultiShape friend list at
			// msl.h (C-r4 C1 fix).
			// [SPOT_DIAG v1] T1.15 per-actor registration counters.
			int diagChildrenWalked = 0;
			int diagSpotlightsFound = 0;
			int diagRegistered = 0;
			int diagOverflow = 0;
			for (int i = 0; i < bldgShape->GetNumShapes(); ++i)
			{
				const TG_ShapeRec* recp = bldgShape->GetShapeRec(i);
				if (!recp) continue;
				TG_Shape* child = recp->node.get();
				// Mirror canonical batcher guards at
				// gos_static_prop_batcher.cpp (C-r3 M1).
				if (!child || !recp->processMe) continue;
				++diagChildrenWalked;
				const bool fieldLamp = S_stricmp(appearType->elevationNodeId, "NONE") != 0 &&
					S_stricmp(child->getNodeName(), appearType->elevationNodeId) == 0;
				if (!child->GetIsSpotlight() && !fieldLamp) continue;
				++diagSpotlightsFound;

				// Resolve node-NAME id (NOT listOfShapes index) per the anubis
				// pattern at mech3d.cpp (C-r3 C1).
				const char* nodeName = child->getNodeName();  // tgl.h
				if (!nodeName) continue;
				long nodeId = bldgShape->GetNodeNameId(nodeName);
				if (nodeId == -1) continue;

				// LOCAL-LIGHTS-1: one light per authored beam of the leaf (a leaf may
				// carry several lamps); the node id repeats once per beam, in beam order.
				const int nBeams = fieldLamp ? 1 : std::max(1,
					mc2_cone_glow::GetNodeBeams(bldgShape, nodeId, nullptr, kMaxBeamsPerNode));
				for (int beam = 0; beam < nBeams; ++beam)
				{
					TG_LightPtr light = static_cast<TG_LightPtr>(malloc(sizeof(TG_Light)));
					light->init(TG_LIGHT_POINT);            // v1 — POINT (OQ2)
					light->SetaRGB(0xffe8c870);             // OQ3 warm hardcoded
					light->SetIntensity(0.5f);              // OQ4 initial
					light->SetFalloffDistances(20.0f, 80.0f); // OQ4

					long slotId = eye->addWorldLight(light);  // camera.h
					if (slotId < 0) { free(light); break; }  // pool overflow

					spotlightLights_.push_back(light);
					spotlightSlotIds_.push_back(static_cast<DWORD>(slotId));
					spotlightNodeIds_.push_back(static_cast<int>(nodeId));
					// T1.16 — tag this slot as (E)-owned, source=Bldg, so the
					// Camera::updateLights per-slot probe can recognize it.
					mc2_spotlight_diag::tag_slot(eye, slotId, mc2_spotlight_diag::Bldg, this);
				}
				++diagRegistered;
			}
			// Pool overflow detection: any spotlight found but not registered
			// where node-id resolution succeeded is treated as overflow signal.
			// Simpler: overflow = spotlights_found - registered (under-counts
			// nodeName misses; honest enough for the H1 disambiguation).
			diagOverflow = diagSpotlightsFound - diagRegistered;
			if (diagOverflow < 0) diagOverflow = 0;
			s_spotDiagBldgRegistered += static_cast<unsigned long>(diagRegistered);
			s_spotDiagBldgOverflows  += static_cast<unsigned long>(diagOverflow);
			++s_spotDiagBldgActors;
			// First-hit per actor — always-on (one stderr line per actor that
			// walks lazy-init, regardless of env). Cap noise by emitting only
			// for the first 8 actors to avoid log flood; cap is generous enough
			// to confirm registration is firing across distinct buildings.
			if (s_spotDiagBldgActors <= 8) {
				MC2_DIAGFE(stderr,
					"[SPOT_DIAG v1] event=first_register sample=first8_actors class=bldg actor_id=%p "
					"children_walked=%d spotlights_found=%d registered=%d overflow=%d\n",
					static_cast<void*>(this), diagChildrenWalked, diagSpotlightsFound,
					diagRegistered, diagOverflow);
				std::fflush(stderr);
			}
			spotlightsRegistered_ = true;
		}
		// [SPOT_DIAG v1] T1.15 per-summary registration aggregate (bldg).
		// Increments every BldgAppearance::update() call; emit every 600 calls
		// when env=1. `calls=N` rather than `frames=N` because update() runs per
		// actor per frame (many actors -> many calls per frame).
		if (s_spotDiagBldgEnabled) {
			++s_spotDiagBldgCalls;
			if ((s_spotDiagBldgCalls % 600) == 0) {
				mc2::fmt::print_err("[SPOT_DIAG v1] event=registration_summary class=bldg calls={:d} actors_first_hit={:d} lights_registered={:d} overflows={:d}\n", mc2::fmt::as_u32(s_spotDiagBldgCalls), mc2::fmt::as_u32(s_spotDiagBldgActors), mc2::fmt::as_u32(s_spotDiagBldgRegistered), mc2::fmt::as_u32(s_spotDiagBldgOverflows));
				std::fflush(stderr);
			}
		}

		// Per-frame in-place update. Runs UNCONDITIONALLY once registered
		// (NOT inside the isNight gate above) — C-r2 C2: day->night->day
		// transitions toggle active via the gate below; lights stay allocated.
		mc2_cone_glow::NodeBeam beams[kMaxBeamsPerNode];
		int nBeams = 0, beamIdx = 0;
		long beamNode = -1;
		for (size_t k = 0; k < spotlightLights_.size(); ++k)
		{
			const bool fieldLamp = S_stricmp(appearType->elevationNodeId, "NONE") != 0 &&
				spotlightNodeIds_[k] == bldgShape->GetNodeNameId(appearType->elevationNodeId);
			glm::vec3 childPos =
				getNodeIdPosition(spotlightNodeIds_[k]);
			// LOCAL-LIGHTS-1: k-th light of a node = its k-th authored beam.
			if (spotlightNodeIds_[k] != beamNode)
			{
				beamNode = spotlightNodeIds_[k];
				beamIdx = 0;
				// Animated nodes and shape transforms can change while the building is stationary.
				nBeams = mc2_cone_glow::GetNodeBeams(bldgShape, beamNode, beams, kMaxBeamsPerNode);
				if (fieldLamp) {
					const auto lamp = mc2_cone_glow::GetSearchlightBeam(bldgShape, beamNode);
					nBeams = lamp ? 1 : -1;
					if (lamp) beams[0] = *lamp;
				}
			}
			else
				++beamIdx;
			// LIGHT-CONE-MODELS-1 (A/C): pool at the authored cone's FAR END
			// (node transform × cone geometry axis), grounded on terrain —
			// tower searchlights illuminate the compound they're aimed INTO,
			// not the tower base. No-cone nodes keep the node-origin pool.
			// MC2_LIGHT_CONE_NODES=0 restores node-origin pools.
			// CONE-SPILL-COUPLING-1: true beam/terrain INTERSECTION, not the
			// old far-end-then-drop-z approximation. See mc2ConeGroundHit.
			// A beam that never meets ground within its own length lights
			// nothing, so its pool is suppressed entirely (coneMissedGround).
			bool coneMissedGround = false;
			// LOCAL-LIGHTS-1: a beam that is not aimed at the sky becomes a real spot
			// light AT THE LAMP, aimed down the authored cone with the cone's own
			// half-angle; the per-fragment pool evaluates the cone on the ground,
			// props, mechs and vegetation (light_pool_attenuation.hglsl). Sky
			// beams keep the CONE-SPILL-POLICY-1 suppression below.
			// The cone leaf of a static building is only transformed while the building
			// is drawn on the CPU path; until its first transform (nBeams < 0) a coned
			// lamp keeps its last pose, and one never posed stays dark.
			const bool keepPose = nBeams < 0 && spotlightLights_[k]->spotCosOuter > 0.0f;
			if (nBeams < 0 && !keepPose) coneMissedGround = true;
			if (keepPose) childPos = spotlightLights_[k]->position;
			else spotlightLights_[k]->spotCosOuter = 0.0f;
			if (mc2_cone_glow::nodes_enabled() && nBeams >= 0)
			{
				glm::vec3 aimW;
				float coneLen = 0.0f;
				bool haveAim = false;
				if (beamIdx < nBeams && (fieldLamp || beams[beamIdx].aim.z <= 0.15f))
				{
					// The lamp sits at the beam apex (mc2_cone_glow::ConfigureBeamLight).
					const mc2_cone_glow::NodeBeam& b = beams[beamIdx];
					mc2_cone_glow::ConfigureBeamLight(spotlightLights_[k], b);
					childPos = b.apex;
					static const bool s_beamDiag = mc2::gate::get<"MC2_LIGHT_CONE_DIAG">();
					static unsigned long s_b = 0;
					if (s_beamDiag && (s_b++ % 600) < 8)
						mc2::fmt::print_err("[LOCAL_LIGHT_BEAM v1] node={:d} beam={:d}/{:d} apex=({:.1f},{:.1f},{:.1f}) aim=({:.3f},{:.3f},{:.3f}) len={:.1f} cosHalf={:.3f}\n", mc2::fmt::as_i32(beamNode), mc2::fmt::as_i32(beamIdx), mc2::fmt::as_i32(nBeams), b.apex.x, b.apex.y, b.apex.z, b.aim.x, b.aim.y, b.aim.z, b.length, b.cosHalf);
				}
				else
				{
					// No tagged beam (or a sky beam): the CONE-SPILL-POLICY-1 ground pool.
					haveAim = mc2_cone_glow::GetNodeAim(bldgShape, spotlightNodeIds_[k], aimW, coneLen);
					spotlightLights_[k]->SetFalloffDistances(20.0f, 80.0f);
				}
				// CONE-SPILL-COUPLING-1 reach probe: does the BUILDING path
				// actually get usable cone geometry? (GetNodeAim returns false
				// for a lamp node with no cone mesh, in which case the pool
				// keeps the node origin and no coupling is possible at all.)
				{
					static const bool s_reach = mc2::gate::get<"MC2_LIGHT_CONE_DIAG">();
					static unsigned long s_r = 0;
					if (s_reach && (s_r++ % 600) < 6)
					{
						mc2::fmt::print_err("[CONE_SPILL_REACH v1] bldg lights={:d} k={:d} nodeId={:d} haveAim={:d} coneLen={:.1f}\n", mc2::fmt::as_u32(static_cast<unsigned>(spotlightLights_.size())), mc2::fmt::as_u32(static_cast<unsigned>(k)), mc2::fmt::as_i32(static_cast<long>(spotlightNodeIds_[k])), mc2::fmt::as_i32(haveAim ? 1 : 0), coneLen);
						std::fflush(stderr);
					}
				}
				if (haveAim && spotlightLights_[k]->spotCosOuter <= 0.0f)
				{
					// The authored beam length IS the physical reach — the old
					// hard 200.0f clamp truncated long beams so their pool fell
					// short of the visible cone tip. The march is a fixed step
					// COUNT, so dropping the clamp costs no extra samples.
					const float spill = coneLen * mc2_cone_glow::spill_scale();
					glm::vec3 hit;
					const bool got = mc2ConeGroundHit(childPos, aimW, spill, hit);

					// CONE-SPILL-COUPLING-1 evidence (reuses the already
					// smoke-allowlisted MC2_LIGHT_CONE_DIAG; adds no new gate).
					// Prints the OLD algorithm's answer (far-end + 200u clamp,
					// then snap z) beside the NEW intersection, in the SAME
					// frame, so the two are directly comparable without an A/B
					// rebuild. horizErr is how far the old pool sat from where
					// the beam actually lands — the user-visible decoupling.
					static const bool s_coneDiag = mc2::gate::get<"MC2_LIGHT_CONE_DIAG">();
					if (s_coneDiag)
					{
						static unsigned long s_n = 0;
						if ((s_n++ % 600) < 12)
						{
							float oldSpill = spill;
							if (oldSpill > 200.0f) oldSpill = 200.0f;
							glm::vec3 oldP;
							oldP.x = childPos.x + aimW.x * oldSpill;
							oldP.y = childPos.y + aimW.y * oldSpill;
							oldP.z = childPos.z + aimW.z * oldSpill;
							if (land) oldP.z = land->getTerrainElevation(oldP);
							const float dx = got ? (hit.x - oldP.x) : 0.0f;
							const float dy = got ? (hit.y - oldP.y) : 0.0f;
							mc2::fmt::print_err("[CONE_SPILL v1] apex=({:.1f},{:.1f},{:.1f}) aim=({:.3f},{:.3f},{:.3f}) coneLen={:.1f} spill={:.1f} clamped={:d} hit={:d} old=({:.1f},{:.1f},{:.1f}) new=({:.1f},{:.1f},{:.1f}) horizErr={:.1f} coupling={:d} poolKept={:d}\n", childPos.x, childPos.y, childPos.z, aimW.x, aimW.y, aimW.z, coneLen, spill, mc2::fmt::as_i32((spill > 200.0f) ? 1 : 0), mc2::fmt::as_i32(got ? 1 : 0), oldP.x, oldP.y, oldP.z, got ? hit.x : 0.0f, got ? hit.y : 0.0f, got ? hit.z : 0.0f, std::sqrt(dx * dx + dy * dy), mc2::fmt::as_i32(mc2_cone_glow::spill_coupling_enabled() ? 1 : 0), mc2::fmt::as_i32((!mc2_cone_glow::spill_coupling_enabled() || got) ? 1 : 0));
							std::fflush(stderr);
						}
					}

					// CONE-SPILL-GRADE-1: MC2_CONE_SPILL_COUPLING=0 restores the
					// exact pre-c1f98b45 placement (far-end + snap z, 200.0f
					// clamp, no miss rejection) so this ungated default-behaviour
					// change can be A/B graded on ONE attributable exe.
					if (!mc2_cone_glow::spill_coupling_enabled())
					{
						// CONE-SPILL-POLICY-1: the DEFAULT path is no longer
						// unconditional. Policy 1 (default):
						//  (a) upward beam (aim.z above a small threshold;
						//      MC2 world is z-up -- the mc2_23 beacons aim
						//      exactly (0,0,1)) -> the beam lights the SKY,
						//      so NO ground pool. The legacy snap-z planted
						//      a pool at the beacon's own base.
						//  (b) otherwise: true beam/terrain intersection over
						//      an EXTENDED reach k*coneLen (authored coneLen
						//      is a visual extent, not a physical reach --
						//      the near-horizontal mc2_10 tower beams drop
						//      only ~15u over their authored length, so
						//      requiring a hit within coneLen deleted 7/9
						//      pools, see CONE-SPILL-GRADE-1). Hit -> pool AT
						//      the intersection; miss within k*coneLen -> no
						//      pool. Constants validated offline by
						//      scripts/predict_cone_pools.py (mc2_10 6/6
						//      non-up fixtures KEPT, mc2_23 beacons 0 pools).
						// MC2_CONE_POOL_POLICY=0 = legacy unconditional
						// far-end + snap-z (exact pre-slice behaviour).
						if (mc2_cone_glow::pool_policy() == 0)
						{
							float oldSpill = spill;
							if (oldSpill > 200.0f) oldSpill = 200.0f;
							childPos.x += aimW.x * oldSpill;
							childPos.y += aimW.y * oldSpill;
							childPos.z += aimW.z * oldSpill;
							if (land)
								childPos.z = land->getTerrainElevation(childPos);
						}
						else
						{
							const float kUpThresh = 0.15f;
							if (aimW.z > kUpThresh)
								coneMissedGround = true;   // sky beam: no pool
							else
							{
								const float reach =
									spill * mc2_cone_glow::pool_reach_scale();
								glm::vec3 poolHit;
								if (mc2ConeGroundHit(childPos, aimW, reach, poolHit))
									childPos = poolHit;
								else
									coneMissedGround = true;
							}
						}
					}
					else if (got)
						childPos = hit;
					else
						coneMissedGround = true;
				}
			}
			spotlightLights_[k]->SetPosition(&childPos);
			// Rule-2 correctness fix: lightToWorld is consumed at
			// msl.cpp (s_lightToShape = lightToWorld * worldToShape).
			// Without setting it, lightToShape collapses to worldToShape
			// alone and the light's effective world position falls back to
			// origin regardless of the `position` field for some consumers.
			// Anubis sets this same matrix; existing per-building pointLight
			// at bdactor.cpp also sets it. Translation-only suffices
			// for POINT lights (matches the anubis/terrain-light pattern).
			glm::mat4 lightToWorldMatrix;
			glm::vec3 childPosP;
			if (mc2_cone_glow::spot_direction_fix_enabled())
			{
				// SPOTLIGHT-DIRECTION-1. TRAP: despite the name, TG_Light::
				// direction is a POSITION for POINT/SPOT lights -- tgl.h
				// says "This is the light spot center for POINT and SPOT", and
				// the consumer in mclib/msl.cpp reads it as
				//     lightPos = light->direction; shapePosition -= lightPos;
				// i.e. it SUBTRACTS it from a world position to derive the
				// per-vertex light vector. Writing a normalized aim vector here
				// would be a silent wrong fix (it nearly happened twice).
				// Both fields below use the same MC2 -> Stuff position swizzle
				// (-x, z, y) as the sibling terrain-light registration earlier
				// in this file; the historical code left `direction` at the
				// init() default (0,0,0) -- the world origin -- and built
				// lightToWorld from the RAW, unswizzled childPos.
				mc2ToRender(childPosP, childPos);
				spotlightLights_[k]->direction = childPosP;
				// MC2_SPOTLIGHT_DIRECTION_FIX=2 falsification arm — see
				// cone_glow.cpp spot_direction_exaggerate(). Absurd spot centre;
				// a byte-identical frame under this proves the consumer's output
				// is discarded downstream. Diagnostic only.
				if (mc2_cone_glow::spot_direction_exaggerate())
				{
					glm::vec3 wild;
					wild.x = 100000.0f; wild.y = 100000.0f; wild.z = 100000.0f;
					spotlightLights_[k]->direction = wild;
				}
			}
			else
			{
				childPosP.x = childPos.x; childPosP.y = childPos.y; childPosP.z = childPos.z;
			}
			mc2::math::buildTranslation(lightToWorldMatrix, childPosP);
			mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(0.0f, 0.0f, 0.0f));
			spotlightLights_[k]->SetLightToWorld(&lightToWorldMatrix);
			// eye->isNight is a bare field at camera.h (C-r3 C2). visible
			// matches anubis at mech3d.cpp (C-r1 C5). forceLightsOut
			// matches the existing per-building pointLight gate.
			// Owner intent: is this lamp switched on at all? Kept SEPARATE from
			// the pool predicate below — CONE-SPILL-COUPLING-1 suppresses the
			// ground pool of a beam that lands nowhere, but the beam VOLUME is
			// still real and must keep rendering. Collapsing the two would make
			// tower searchlights vanish when aimed at a wall.
			const bool ownerLit = (eye && eye->isNight && visible && !forceLightsOut);
			// LOCAL-LIGHTS-1: a coned lamp concentrates its light in the beam.
			const float lampIntensity = spotlightLights_[k]->spotCosOuter > 0.0f ? 0.6f : 0.5f;
			spotlightLights_[k]->SetIntensity(lampIntensity * visualTuning_environment().buildingSpillIntensityScale);
			spotlightLights_[k]->active = ownerLit && !coneMissedGround;
			// LIGHT-CULL-RADIUS-1: owner intent for the camera composition.
			// Building light positions are static, so persistence while the
			// prop's visibility-gated update is culled is CORRECT — the
			// camera-side volume test alone decides screen relevance (fixes
			// the mc2_10 airbase all-pools-dark angle, 204907).
			spotlightLights_[k]->ownerEnabled =
				spotlightLights_[k]->active ? 1 : 0;
			// LIGHT-CONE-MODELS-1 (B): visible glowing beam volume, tied to
			// the SAME owner predicate — lights-out/destroyed buildings show
			// no cone. Screen-clip rejection lives in QueueConeGlow.
			// MC2_LIGHT_CONE_GLOW=0 kills it.
			// Gated on ownerLit, NOT on ->active: a beam whose pool was
			// suppressed for missing the ground is still a visible volume.
			if (ownerLit && beamIdx == 0 && fieldLamp)
				mc2_cone_glow::QueueSearchlightGlow(bldgShape, beamNode,
					spotlightLights_[k]->GetaRGB(), eye->getNightFactor());
			else if (ownerLit && beamIdx == 0)   // the glow draws the whole leaf once
				mc2_cone_glow::QueueConeGlow(bldgShape, spotlightNodeIds_[k],
					spotlightLights_[k]->GetaRGB(), eye->getNightFactor());
		}

		// LIGHT-REALISM-1 (beacon emission): red/green/blue beacon + runway
		// light cards read as bright UNLIT sprites on dark ground because
		// nothing spills from them. Queue a small soft colored ground disc per
		// beacon card. Deliberately outside the spotlightLights_ loop — a
		// building can carry beacons WITHOUT carrying any SpotLight_ node
		// (runway light strips are exactly that case).
		// Owner predicate mirrors the spotlight one above (visible, not
		// lights-out); night gating + the MC2_LIGHT_REALISM gate (default ON
		// since CONE-SPILL-POLICY-1; =0 reverts)
		// live inside QueueBeaconGlow. No TG_Light slots are consumed — see
		// the cone_glow.h contract note.
		if (eye && eye->isNight && visible && !forceLightsOut)
			mc2_cone_glow::QueueBeaconGlow(bldgShape, eye->getNightFactor());
	}

	if (forceLightsOut)
		bldgShape->SetLightsOut(true);
		
	//Update flashing regardless of view!!!
	if (duration > 0.0f)
	{
		duration -= frameLength;
		currentFlash -= frameLength;
		if (currentFlash < 0.0f)
		{
			drawFlash ^= true;
			currentFlash = flashDuration;
		}
	}
	else
	{
		drawFlash = false;
	}

	// Under the GPU static-prop path, compute xlatPosition/rot + fog/light
	// for every building so the later TransformMultiShape (also gated on inView)
	// has valid inputs.
	if (inView)
	{
		if (appearType->spinMe)
			rotation += SPINRATE * frameLength;

 		if (rotation > 180)
			rotation -= 360;

		if (rotation < -180)
			rotation += 360;

		// Does math necessary to draw Tree
		float yaw = rotation * DEGREES_TO_RADS;
		rot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
	
		if (appearType->spinMe && land)
		{
			//Make sure we are above the water level
			if (position.z < Terrain::waterElevation)
				position.z = Terrain::waterElevation;
		}

		mc2ToRender(xlatPosition, position);

		if (!fogLightSet)
		{
			unsigned char lightr,lightg,lightb;
			float lightIntensity = 1.0f;
			if (land)
				lightIntensity = land->getTerrainLight(position);

			lightr = eye->getLightRed(lightIntensity);
			lightg = eye->getLightGreen(lightIntensity);
			lightb = eye->getLightBlue(lightIntensity);

			lightRGB = (lightr<<16) + (lightg<<8) + lightb;

			fogRGB = 0xff<<24;
			float fogStart = eye->fogStart;
			float fogFull = eye->fogFull;

			if (xlatPosition.y < fogStart)
			{
				float fogFactor = fogStart - xlatPosition.y;
				if (fogFactor < 0.0)
					fogRGB = 0xff<<24;
				else
				{
					fogFactor /= (fogStart - fogFull);
					if (fogFactor <= 1.0)
					{
						fogFactor *= fogFactor;
						fogFactor = 1.0 - fogFactor;
						fogFactor *= 256.0;
					}
					else
					{
						fogFactor = 256.0;
					}

					unsigned char fogResult = float2long(fogFactor);
					fogRGB = fogResult << 24;
				}
			}
			else
			{
				fogRGB = 0xff<<24;
			}

			fogLightSet = true;

		}

		eye->setLightColor(0,lightRGB);
		eye->setLightIntensity(0,1.0);

		if (useFog)
			bldgShape->SetFogRGB(fogRGB);
		else
			bldgShape->SetFogRGB(0xffffffff);
	
		if (S_stricmp(appearType->elevationNodeId, "NONE") == 0) {
		glm::quat turretRot;
		turretRot = mc2::math::quatFromEuler(mc2::math::Euler((unitAnim().pitch * DEGREES_TO_RADS),(unitAnim().turretRotation * DEGREES_TO_RADS),0.0f));
		if (rotationalNodeId == -1)
	   		rotationalNodeId = bldgShape->SetNodeRotation(appearType->rotationalNodeId,&turretRot);
   
	   	bldgShape->SetNodeRotation(rotationalNodeId,&turretRot);
		}
	}

	float oldFrame = unitAnim().currentFrame;
	if (unitAnim().bdAnimationState >= 0) appearType->setAnimation(bldgShape, unitAnim().bdAnimationState);
	bldgShape->SetFrameNum(unitAnim().currentFrame);

	// Under the GPU static-prop path we need listOfColors / shapeToWorld
	// fresh every frame regardless of inView so the batcher can safely
	// memcpy from shape->listOfColors during submit().
	if (inView)
	{
		bldgShape->SetUseShadow(false);


		bldgShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
		// Slice 2 (object-offload) — Stage 2.B: eligibility hoist.
		// Branch lives INSIDE the existing inView cull
		// gate to preserve slice 1's R1 invariant (no cull bypass).
		// Run positions-only when:
		//   - this actor did not hit a late-registration recovery last frame
		//     (needsFullBakeNextFrame is a NEW bool from Stage 2.A; set by
		//     BldgAppearance::render when submitMultiShape returns false with
		//     wasLastFailureLateRegistration() true), AND
		//   - the multi-shape's leaves are all registered with the slice 1
		//     batcher (isMultiShapeEligibleForGpuObjects mirrors slice 1's
		//     render-time per-child gates EXCEPT late-reg, which is handled
		//     via the recovery flag above).
		// Otherwise full bake. The full bake clears the recovery flag —
		// re-establishing valid .argb before render reads it.
		// PERF DIAGNOSTIC 2026-05-06: Tracy zones to attribute the 9.52 µs/call
		// observed in TerrainObject::update appearanceUpdate. Six theories under
		// investigation; these zones discriminate between them. Demote to silent
		// (or remove) once the regression is identified.
		bool gpuEligible;
		{
			gpuEligible = !needsFullBakeNextFrame &&
			              GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(bldgShape);
		}

		if (gpuEligible)
		{
			// Stage 2.D.2 fix: cache GPU light data NOW, while worldLights[0]->aRGB
			// is the per-actor terrain-scaled value set at line 2144 above.
			// By the time submitMultiShape() runs (during renderLists()), later
			// actors have overwritten worldLights[0]->aRGB for their positions.
			{
				mc2CacheOrBakeStaticGpuLight(bldgShape, staticReg.registered, staticReg.recipeIndex);
				// 2026-05-11 per-instance capture: snapshot the multi's just-written
				// cache slot for THIS actor before sibling actors of the same
				// multi-type overwrite it. Ferried to RecipeRange via markVisible().
				staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();
			}
			{
				// GPU-INSTANCE-SKIP-POOLS-1 (2026-06-03): see TreeAppearance::
				// update for the full rationale. gpuEligible guarantees every
				// leaf is registered; run the ZERO-POOL hierarchy walk so no TGL
				// frame pool is allocated per visible instance. submit reads
				// rec.shapeToWorld (populated by the walk) + the debug-only
				// zero-padded Colors SSBO.
				bldgShape->TransformMultiShape_HierarchyOnly (&xlatPosition,&rot);
			}
		}
		else
		{
			// GPU-INSTANCE-SKIP-POOLS-1 (2026-06-03): see TreeAppearance::update
			// for the full rationale + A/B evidence. For a registered type the
			// full-bake pool content is vestigial (GPU draws from the per-type
			// VBO + rec.shapeToWorld; light cache below is pool-independent), so
			// run the zero-pool hierarchy walk. mc2CacheOrBakeStaticGpuLight still
			// runs to seed the light index.
			if (GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(bldgShape))
			{
				bldgShape->TransformMultiShape_HierarchyOnly (&xlatPosition,&rot);
			}
			else
			{
				bldgShape->TransformMultiShape (&xlatPosition,&rot);
			}
			// 2026-05-10: also seed cachedGpuLightIndex_ in the full-bake
			// branch. Without this, the first-frame transition out of the
			// H4 latch (set by registerStatic) leaves the index
			// at UINT32_MAX, and the static-path render gate
			// (`getCachedGpuLightIndex() == UINT32_MAX → invalidate`)
			// invalidates the registration on the very next render —
			// markVisible() never fires, registry::flush() short-circuits,
			// substrate gets no static-prop records, and the cull writes
			// 0 to all bucketCountData. The fix mirrors the gpuEligible
			// branch's CacheGpuLightData call so any path
			// through update() seeds the light index. Cheap: same call
			// already runs unconditionally in submitMultiShape; here we
			// just hoist its effect to be visible to render() this frame.
			mc2CacheOrBakeStaticGpuLight(bldgShape, staticReg.registered, staticReg.recipeIndex);
			// 2026-05-11 per-instance capture: see gpuEligible branch above.
			staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();
			needsFullBakeNextFrame = false;
		}

		if ((turn > 3) && useShadows)
			beenInView = true;
			
		// Update GOSFX
		if (destructFX && destructFX->IsExecuted())
		{
			glm::mat4 	shapeOrigin;
			glm::mat4 	localToWorld;
			glm::vec3			tPosition;
				
			//Stuff::Vector3D offsetPosition;
			//offsetPosition.x = Terrain::worldUnitsPerVertex / 3.0f;
			//offsetPosition.y = -(Terrain::worldUnitsPerVertex / 3.0f);
			//offsetPosition.z = 0.0f;

			//OppRotate(offsetPosition,rotation);

			glm::vec3 actualPosition = position;
			//actualPosition.Add(position,offsetPosition);

			mc2ToRender(tPosition, actualPosition);

			float yaw = (180.0f + rotation) * DEGREES_TO_RADS;
			glm::quat rot;
			rot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
 			mc2::math::buildRotation(shapeOrigin, rot);
			mc2::math::buildTranslation(shapeOrigin, tPosition);

	 		mc2::math::Obb boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = destructFX->Execute(&info);
			if (!result)
			{
				destructFX->Kill();
				destructFX.reset();
			}
		}
		
		if (isActivitying)
		{
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			glm::mat4	localResult;
					
			if (activityNodeId == -1)
				activityNodeId = bldgShape->GetNodeNameId("activity_node");
			glm::vec3 dustPos = getNodeIdPosition(activityNodeId);

			if (rotationalNodeId == -1)
			{
				if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
	   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
				else
					rotationalNodeId = -2;
			}

			if (rotationalNodeId >= 0)
				dustPos = getNodeIdPosition(rotationalNodeId);
 			
			glm::vec3 wakePos;
			mc2ToRender(wakePos, dustPos);
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, wakePos);
					
			/*
			glm::quat effectRot;
			effectRot = mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			*/
			
			mc2::math::Obb boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
			
			// sebi: make as all other do with Execute(), otherwise ther is constanrt assert in Execute() function (at line Verify(IsExecuted()) )
			if (activity && activity->IsExecuted())
			{
				bool result = activity->Execute(&info);
				if (!result)
				{
					activity->Kill();		//Effect is over.  Otherwise, wait until hit!
					activity.reset();
				}

			}
			
			if (activity1)
			{
				if (activityNodeId == -1)
					activityNodeId = bldgShape->GetNodeNameId("activity_node");
				glm::vec3 dustPos = getNodeIdPosition(activityNodeId);
	
				if (rotationalNodeId == -1)
				{
					if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
		   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
					else
						rotationalNodeId = -2;
				}
	
				if (rotationalNodeId >= 0)
					dustPos = getNodeIdPosition(rotationalNodeId);
				
				glm::vec3 wakePos;
				mc2ToRender(wakePos, dustPos);
				
				mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
				mc2::math::buildTranslation(shapeOrigin, wakePos);
						
				/*
				glm::quat effectRot;
				effectRot = mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f);
				localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
				localResult.Multiply(localToWorld,shapeOrigin);
				*/
				
				mc2::math::Obb boundingBox;
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
				
				//sebi:
				if (activity1 && activity1->IsExecuted())
				{
					bool result = activity1->Execute(&info);
					if (!result)
					{
						activity1->Kill();		//Effect is over.  Otherwise, wait until hit!
						activity1.reset();
					}
				}

			}
		}
	}
	
	return TRUE;
}

void BldgAppearance::startActivity (long effectId, bool loop)
{
	if (!simPresentationPass) { unitAnim().activityEffect = effectId; unitAnim().activityLoop = loop; return; }

	//Check if we are already playing one.  If not, be active!
	
	//First, check if its even loaded.
	// can easily preload this.  Should we?  NO.  We don't know what will be passed in.
	if (!activity && useNonWeaponEffects)
	{
   		if (strcmp(weaponEffects->GetEffectName(effectId),"NONE") != 0)
   		{
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;
			if (!loop)
				flags = gosFX::Effect::ExecuteFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(effectId));
			
			if (gosEffectSpec)
			{
				activity = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(activity != nullptr);
				
				glm::vec3 testPos = getNodeNamePosition("activity_node1");
				if (!mc2::math::closeEnough(testPos, position))
				{
					activity1 = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
					gosASSERT(activity1 != nullptr);
				}

  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if (!isActivitying && activity)		//Start the effect if we are not running it yet!!
	{
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
		
		if (activityNodeId == -1)
   			activityNodeId = bldgShape->GetNodeNameId("activity_node");
   		glm::vec3 nodePos = getNodeIdPosition(activityNodeId);

   		if (rotationalNodeId == -1)
   		{
   			if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
      				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
   			else
   				rotationalNodeId = -2;
   		}

   		if (rotationalNodeId >= 0)
   			nodePos = getNodeIdPosition(rotationalNodeId);

 		glm::vec3 wakePos;
		mc2ToRender(wakePos, nodePos);	//Wake is at Water Level!
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, wakePos);
				
		/*
		glm::quat effectRot;
		effectRot = mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		*/
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);

		activity->Start(&info);
		
		if (activity1)
		{
			if (activityNode1Id == -1)
				activityNode1Id = bldgShape->GetNodeNameId("activity_node1");
			glm::vec3 nodePos = getNodeIdPosition(activityNode1Id);

			if (rotationalNodeId == -1)
			{
				if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
	   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
				else
					rotationalNodeId = -2;
			}

			if (rotationalNodeId >= 0)
				nodePos = getNodeIdPosition(rotationalNodeId);
			
 			glm::vec3 wakePos;
			mc2ToRender(wakePos, nodePos);	//Wake is at Water Level!
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, wakePos);
					
			/*
			glm::quat effectRot;
			effectRot = mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			*/
				
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
	
			activity1->Start(&info);
		}
		
		isActivitying = true;
	}
}

void BldgAppearance::stopActivity (void)
{
	if (!simPresentationPass) { unitAnim().activityEffect = -2; return; }

	if (isActivitying)		//Stop the effect if we are running it!!
	{
		if(activity) //sebi
			activity->Kill();
		if (activity1)
			activity1->Kill();
	}
	
	isActivitying = false;
}

void BldgAppearance::flashBuilding (float dur, float fDuration, DWORD color)
{
	if (simulationOwned() && !simPresentationPass)
	{
		recordSimFlash(dur, fDuration, color);
		return;
	}
	duration = dur;
	flashDuration = fDuration;
	flashColor = color;
	drawFlash = true;
	currentFlash = flashDuration;
}
