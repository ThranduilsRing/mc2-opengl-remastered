// Mech3DAppearance: setObjStatus, updateGeometry and the arm recalculations.
// Declared in the class headers; shared names in mech3d_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "texture_assets.h"
#include "mech3d_internal.h"
#include "footprints.h"
#include "../RenderCore/axis_convert.h"
#include <format>
#include "mover_lamps.h"

// T1.15 SpotLight_ illumination diagnostic — registration probe (mech class).
// First-hit always-on; per-summary every 600 updateGeometry calls when env=1.
static const bool s_spotDiagMechEnabled = mc2::gate::get<"MC2_SPOT_DIAG">();
static unsigned long s_spotDiagMechRegistered = 0;
static unsigned long s_spotDiagMechOverflows  = 0;
static unsigned long s_spotDiagMechActors     = 0;
static unsigned long s_spotDiagMechCalls      = 0;


#define SPIN_RATE		90.0f

constexpr int FOOTPRINT_SLOP = 2;

void Mech3DAppearance::setObjStatus (long oStatus)
{
	if ( (status != oStatus) && (!InEditor))
	{
		if (oStatus == OBJECT_STATUS_DESTROYED)
		{
			if (unitAnim().currentGestureId == 23)
			{
				if (mechType->mechForwardDmgShape)
				{
					mechShape->ClearAnimation();
					mc2mechanim::UnregisterImportedActor(mechShape);	// IMPORTED-ACTOR-STABLE-KEY-1
					delete mechShape;
					mechShape = nullptr;

					mechShape = mechType->mechForwardDmgShape->CreateFrom();
				}
			}
			else if (unitAnim().currentGestureId == 24)
			{
				if (mechType->mechBackwardDmgShape)
				{
					mechShape->ClearAnimation();
					mc2mechanim::UnregisterImportedActor(mechShape);	// IMPORTED-ACTOR-STABLE-KEY-1
					delete mechShape;
					mechShape = nullptr;

					mechShape = mechType->mechBackwardDmgShape->CreateFrom();
				}
			}

			currentLOD = 0;

			for (long i=0;i<mechShape->GetNumTextures();i++)
			{
				char txmName[1024];
				mechShape->GetTextureName(i,txmName,256);
	
				char texturePath[1024];
				auto texturePathEnd = std::format_to_n(texturePath, sizeof(texturePath) - 1, "{}{}" PATH_SEPARATOR, static_cast<const char*>(tglPath), ObjectTextureSize).out;
				*texturePathEnd = '\0';
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (fileExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = 0;
						
						if (!i)
						{
							localTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink,true);
							mechShape->SetTextureHandle(i,localTextureHandle);
							mechShape->SetTextureAlpha(i,true);
						}
						else
						{
							gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
							mechShape->SetTextureHandle(i,gosTextureHandle);
							mechShape->SetTextureAlpha(i,true);
						}
						
					}
					else
					{
						DWORD gosTextureHandle = 0;
						
						if (!i)
						{
							localTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink,true);
							mechShape->SetTextureHandle(i,localTextureHandle);
							mechShape->SetTextureAlpha(i,false);
						}
						else
						{
							gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
							mechShape->SetTextureHandle(i,gosTextureHandle);
							mechShape->SetTextureAlpha(i,false);
						}
					}
				}
				else
				{
					mechShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}

		if (status == OBJECT_STATUS_DESTROYED && oStatus == OBJECT_STATUS_NORMAL)
		{
			STOP(("Destroyed Mech just came back to life!!"));
		}
	}
	
	status = oStatus;
}

void Mech3DAppearance::updateGeometry (void)
{
    mc2mechanim::CopyImportedActorPose(simulationShape_.get(), mechShape);
	ZoneScopedN("GameLogic.Mech3D.UpdateGeometry");
	// F3 CPU projection cost-baseline: sidecar bucket (NEVER in
	// projection_total). One scope per visible mech per frame.
	::mc2_cpu_proj_cost::SidecarScope _f3_skin_scope(
	    ::mc2_cpu_proj_cost::SIDECAR_SKINNING_CHAIN);

	// BT2018-SKEL-ENGINE-1B-RUNTIME / 1C: advance + re-pose imported skinned mechs
	// once per frame BEFORE TransformMultiShape re-reads the shared type geometry
	// below. Idempotent on g_mc2FrameCounter (the second combat update() is a no-op
	// here); near-free when no imported animated mech is registered. 1C selects the
	// clip from this mech's movement (gesture + position/leg-heading deltas).

	//Always override with our local instance.
	mechShape->SetTextureHandle(0,localTextureHandle);
	
	if (rightArm)
		rightArm->SetTextureHandle(0,localTextureHandle);

	if (leftArm)
		leftArm->SetTextureHandle(0,localTextureHandle);

	// Slice D-shadow-state-strip: when GPU mech path + state-strip
	// killswitch + tessellation are all active, skip ALL per-frame state
	// setters on mechShadowShape. Recon (D-shadow-skip §Q1-Q4 + D-shadow-
	// state-strip §Q1-Q4) proved no consumer of these setters' effects
	// exists in this configuration: instance-state setters feed only
	// TransformMultiShape (already retired by D-shadow-skip), and the
	// global-static side effect (SetLightList writing s_listOfLights) is
	// overwritten by mechShape's identical call at mech3d.cpp before
	// any consumer reads it.
	// MECH-KILLSWITCH-SHADOW-PAIR-RETIRE-1: former MC2_GPU_MECH_SHADOW_STATE_STRIP
	// (default-ON, strict no-op on tessellated GPU-mech path) retired to constant.
	// ISSUE-34: modern-shadow-path predicate (was tess-keyed), matching the
	// renderShadows early-return so the stripped state has no consumer.
	const bool stripShadowState = gos_IsModernShadowPathActive();

	if ((status == OBJECT_STATUS_DESTROYED) ||
		(status == OBJECT_STATUS_DISABLED) || 
		(status == OBJECT_STATUS_SHUTDOWN))
	{
		mechShape->SetLightsOut(true);
	}
	else
	{
		mechShape->SetLightsOut(false);
	}
	
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);

	//Update flashing regardless of view!!!
	// FREEZE-MOVERS-1: drawFlash toggles off a wall-clock timer, so a damaged
	// mech blinks between arms of an A/B and the diff is the blink, not the fix.
	if (duration > 0.0f && (mc2FreezeMoversActive() == 0))
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

	{
		glm::quat qRotation;
		float yaw = rotation * DEGREES_TO_RADS;
		qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
	
		unsigned char lightr,lightg,lightb;
		float lightIntensity = 1.0f;
		
		if ( land )
			lightIntensity = land->getTerrainLight(position);
							
		lightr = eye->getLightRed(lightIntensity);
		lightg = eye->getLightGreen(lightIntensity);
		lightb = eye->getLightBlue(lightIntensity);
	
		DWORD lightRGB = (lightr<<16) + (lightg<<8) + lightb;
		
		eye->setLightColor(0,lightRGB);
		eye->setLightIntensity(0,1.0);
	
		DWORD fogRGB = 0xff<<24;
		float fogStart = eye->fogStart;
		float fogFull = eye->fogFull;
	
		if (useFog && (xlatPosition.y < fogStart))
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
	
				unsigned char fogResult = fogFactor;
				fogRGB = fogResult << 24;
			}
		}
		else
		{
			fogRGB = 0xff<<24;
		}
	
		if (useFog)
			mechShape->SetFogRGB(fogRGB);
		else
			mechShape->SetFogRGB(0xffffffff);

		if (!pointLight && eye->isNight)
		{
			if (lightCircleNodeIndex == -1)
				lightCircleNodeIndex = mechShape->GetNodeNameId("SLCircle_anubis");

			glm::vec3 lightPos = getNodeIdPosition(lightCircleNodeIndex);
			if (!mc2::math::closeEnough(lightPos, position))
			{
				pointLight = static_cast<TG_LightPtr>(malloc(sizeof(TG_Light)));
				pointLight->init(TG_LIGHT_SPOT);
				lightId = eye->addWorldLight(pointLight);
				// NIGHT-LIGHT-POOLS-2a: tag the anubis light as mover-owned so
				// the light-pool producer can classify it (tags are recorded
				// unconditionally; MC2_SPOT_DIAG gates only the printing).
				if (lightId >= 0)
					mc2_spotlight_diag::tag_slot(eye, lightId, mc2_spotlight_diag::Mech, this);

				// MOVER-LIGHT-HALOGEN-1: warm halogen instead of legacy pure
				// yellow (MC2_LIGHT_POOL_MOVER_COLOR override).
				pointLight->SetaRGB(mc2_spotlight_diag::mover_light_argb());
				pointLight->SetIntensity(0.15f);
				pointLight->SetFalloffDistances(50.0f, 250.0f);
			}
		}
		
		if (pointLight)
		{
			// LIGHT-POOL-VIS-GATE-1: destroyed/disabled/shutdown mechs emit
			// nothing (user report: wrecks kept glowing).
			// VISIBILITY-SEMANTICS-1 job (c): was `sensorLevel > 4`, which is
			// false for every home mech after render() resets it to 0 and for
			// every vehicle in the game. Own/allied always emit.
			if (!InEditor && mc2_vis::emitsLight(buildVisState()))
			{
				if (lightCircleNodeIndex == -1)
					lightCircleNodeIndex = mechShape->GetNodeNameId("SLCircle_anubis");

				glm::vec3 lightPos = getNodeIdPosition(lightCircleNodeIndex);

				glm::vec3 ourPosition;
				mc2ToRender(ourPosition, lightPos);
		
				pointLight->direction = ourPosition;
		
				mc2ToRender(pointLight->spotDir, position);

				pointLight->maxSpotLength = 50.0f;

				glm::mat4 lightToWorldMatrix;
				mc2::math::buildTranslation(lightToWorldMatrix, ourPosition);
				mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(0.0f,0.0f,0.0f));
				pointLight->SetLightToWorld(&lightToWorldMatrix);
				pointLight->SetPosition(&lightPos);
				pointLight->active = true;
				pointLight->ownerEnabled = 1;	//LIGHT-CULL-RADIUS-1
			}
			else
			{
				pointLight->active = false;
				pointLight->ownerEnabled = 0;	//LIGHT-CULL-RADIUS-1
			}
		}

		// (E) T1.6 / T1.7: generalised SpotLight_-child illumination. Parallel
		// path to the SLCircle_anubis pointLight block above (R7: distinct
		// node-name prefixes, no double-registration). Lazy first-night
		// register; per-frame in-place update afterwards (T1.7: no per-frame
		// pool churn). Public accessors (msl.h GetNumShapes, msl.h
		// GetShapeRec, tgl.h GetIsSpotlight) because Mech3DAppearance is
		// NOT in the TG_MultiShape friend list at msl.h.
		// (E) T3.1: gate retired; behavior is now unconditional. Coexists with
		// the SLCircle_anubis pointLight block above (R7: distinct node-name
		// prefix, no double-registration).
		if (mechShape)
		{
			if (!spotlightsRegistered_ && eye->isNight)
			{
				// [SPOT_DIAG v1] T1.15 per-actor registration counters.
				int diagChildrenWalked = 0;
				int diagSpotlightsFound = 0;
				int diagRegistered = 0;
				int diagOverflow = 0;
				for (int i = 0; i < mechShape->GetNumShapes(); ++i)
				{
					const TG_ShapeRec* recp = mechShape->GetShapeRec(i);
					if (!recp) continue;
					TG_Shape* c = recp->node.get();
					if (!c || !recp->processMe) continue;
					++diagChildrenWalked;
					if (!c->GetIsSpotlight()) continue;
					++diagSpotlightsFound;

					const char* nodeName = c->getNodeName();
					if (!nodeName) continue;
					long nodeId = mechShape->GetNodeNameId(nodeName);
					if (nodeId == -1) continue;

					if (!mc2_mover_lamps::registerLamps(mechShape, nodeId, spotlightNodeIds_, spotlightLights_, spotlightSlotIds_,
					                                    mc2_spotlight_diag::Mech, this))
						continue;
					++diagRegistered;
				}
				diagOverflow = diagSpotlightsFound - diagRegistered;
				if (diagOverflow < 0) diagOverflow = 0;
				s_spotDiagMechRegistered += static_cast<unsigned long>(diagRegistered);
				s_spotDiagMechOverflows  += static_cast<unsigned long>(diagOverflow);
				++s_spotDiagMechActors;
				if (s_spotDiagMechActors <= 8) {
					MC2_DIAGFE(stderr,
						"[SPOT_DIAG v1] event=first_register class=mech actor_id=%ld "
						"children_walked=%d spotlights_found=%d registered=%d overflow=%d\n",
						actorHandle_, diagChildrenWalked, diagSpotlightsFound,
						diagRegistered, diagOverflow);
					std::fflush(stderr);
				}
				spotlightsRegistered_ = true;  // register-once flag (M3)
			}
			// [SPOT_DIAG v1] T1.15 per-summary registration aggregate (mech).
			if (s_spotDiagMechEnabled) {
				++s_spotDiagMechCalls;
				if ((s_spotDiagMechCalls % 600) == 0) {
					std::fprintf(stderr,
						"[SPOT_DIAG v1] event=registration_summary class=mech "
						"calls=%lu actors_first_hit=%lu lights_registered=%lu overflows=%lu\n",
						s_spotDiagMechCalls, s_spotDiagMechActors,
						s_spotDiagMechRegistered, s_spotDiagMechOverflows);
					std::fflush(stderr);
				}
			}

			// T1.7: per-frame in-place update. UNCONDITIONAL once registered
			// (C-r2 C2: lights stay allocated across day/night; toggle active).
			// active gate matches anubis verbatim (C-r1 C5): visible && (sensorLevel
			// > 4) && !InEditor, plus isNight (bare field, no parens, per C-r3 C2).
			mc2_mover_lamps::LampCursor lampCursor;
			for (size_t k = 0; k < spotlightLights_.size(); ++k)
			{
				glm::vec3 childPos =
					getNodeIdPosition(spotlightNodeIds_[k]);
				bool coneAimed = lampCursor.next(mechShape, spotlightNodeIds_[k], spotlightLights_[k], childPos);
				// LIGHT-CONE-MODELS-1 (A/C): pool at the authored cone's FAR
				// END (node transform × cone geometry axis), grounded on
				// terrain — the beam lights where it actually lands. Falls
				// back to the facing heuristic below when the model has no
				// usable cone. MC2_LIGHT_CONE_NODES=0 restores heuristic.
				if (!coneAimed && mc2_cone_glow::nodes_enabled())
				{
					glm::vec3 aimW;
					float coneLen = 0.0f;
					if (mc2_cone_glow::GetNodeAim(mechShape, spotlightNodeIds_[k], aimW, coneLen))
					{
						float spill = coneLen * mc2_cone_glow::spill_scale();
						if (spill > 200.0f) spill = 200.0f;
						childPos.x += aimW.x * spill;
						childPos.y += aimW.y * spill;
						childPos.z += aimW.z * spill;
						if (land)
							childPos.z = land->getTerrainElevation(childPos);
						coneAimed = true;
					}
				}
				// MOVER-LIGHT-FORWARD-1: bias the pool center AHEAD of the
				// mech along its facing (headlights light the road ahead,
				// not an omni puddle centered underfoot). Facing convention
				// matches Mover::getRotationVector ((0,-1,0) rotated by
				// -rotation). MC2_LIGHT_POOL_MOVER_FWD=0 restores centered.
				if (!coneAimed)
				{
					const float fwdWu = mc2_spotlight_diag::mover_light_fwd_offset();
					if (fwdWu > 0.0f)
					{
						glm::vec3 fwdv;
						fwdv.x = 0.0f; fwdv.y = -1.0f; fwdv.z = 0.0f;
						Rotate(fwdv, -rotation);
						childPos.x += fwdv.x * fwdWu;
						childPos.y += fwdv.y * fwdWu;
					}
				}
				spotlightLights_[k]->SetPosition(&childPos);
				// Rule-2 correctness fix: lightToWorld is consumed at
				// msl.cpp. Without it, the precomputed s_lightToShape
				// collapses to worldToShape alone and the light's effective
				// world position is lost. Matches the existing anubis
				// pattern at mech3d.cpp (translation-only).
				glm::mat4 lightToWorldMatrix;
				glm::vec3 childPosP;
				childPosP.x = childPos.x; childPosP.y = childPos.y; childPosP.z = childPos.z;
				mc2::math::buildTranslation(lightToWorldMatrix, childPosP);
				mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(0.0f, 0.0f, 0.0f));
				spotlightLights_[k]->SetLightToWorld(&lightToWorldMatrix);
				// T1.13: dropped (sensorLevel > 4) gate. The original gate
				// was inherited verbatim from the anubis-specific pointLight
				// branch above where "show enemy searchlight only when
				// player has sensor lock" is the correct semantic. For the
				// generalized SpotLight_ vector path this is overly
				// restrictive: player-owned mechs (sensorLevel=5) work, but
				// any enemy mech at sensorLevel<5 is stuck active=false. The
				// generalized path should illuminate for any visible mech at
				// night regardless of sensor state.
				// LIGHT-POOL-VIS-GATE-1 note (2026-07-21): `visible` IS the
				// mesh-drawn fog-of-war predicate (the appearance renders the
				// full mesh only under `if (visible)`; sensor blips draw via a
				// separate sensorLevel 1..4 path) — this active flag is
				// CORRECT. The observed "pools at unseen enemy blips" leak was
				// the light-pool pack loop ignoring TG_Light::active entirely
				// (fixed in gos_terrain_lighting.cpp PackAndDispatch).
				// LIGHT-POOL-VIS-GATE-1: destroyed/disabled/shutdown movers
				// emit nothing (user report: wrecks kept glowing).
				// GHOST-LIGHT-VISIBILITY-1: the T1.13 note above is WRONG —
				// `visible` is NOT the mesh-drawn fog-of-war predicate.
				// code/mech.cpp calls setVisibility(true,true) on
				// EVERY contact branch including CONTACT_SENSOR_QUALITY_1..4,
				// so a mech the player only holds as a sensor blip had
				// visible==true and lit up the terrain / drew a searchlight
				// cone — advertising a hidden enemy's exact position. Gate on
				// the predicate that actually decides whether the MESH draws.
				// Killswitch MC2_LIGHT_MOVER_VIS_GATE=0.
				// VISIBILITY-SEMANTICS-1: job (c) EMITS LIGHT. Own/allied mechs
				// always emit; a foreign mech emits only while its mesh is
				// actually drawn (meshAlpha != 0), which is the real
				// fog-of-war mechanism. `visible` is deliberately NOT read —
				// it is true for every live mech every frame (mech.cpp).
				// Gate MC2_VIS_SEMANTICS=0 restores the pre-lane predicate.
				spotlightLights_[k]->active =
					(eye->isNight && !InEditor
					 && mc2_vis::emitsLight(buildVisState()));
				// LIGHT-CULL-RADIUS-1: owner intent for the camera composition.
				spotlightLights_[k]->ownerEnabled =
					spotlightLights_[k]->active ? 1 : 0;
				// LIGHT-CONE-MODELS-1 (B): visible glowing beam volume, tied
				// to the SAME owner predicate — shutdown/dead/FoW-hidden
				// mechs show no cone. MC2_LIGHT_CONE_GLOW=0 kills it.
				if (spotlightLights_[k]->active && lampCursor.lampIdx == 0)   // the glow draws the whole leaf once
					mc2_cone_glow::QueueConeGlow(mechShape, spotlightNodeIds_[k],
						spotlightLights_[k]->GetaRGB(), eye->getNightFactor(),
						/*ownerIsMover=*/true);
			}
		}

		// D-gpu-pose-instrument: AnimPose stage — animation pose state setters
		// (setAnimation, SetFrameNum, SetNodeRotation × N) on body + shadow
		// shapes, plus the shadow-shape transform conditional. Per-stage
		// attribution for the trimodal Mech3D.UpdateGeometry histogram.
		{ ZoneScopedN("Mech3D.UpdateGeometry.AnimPose");
		if (status != OBJECT_STATUS_DESTROYED)
		{
			mechType->setAnimation(mechShape,unitAnim().currentGestureId);
			mechShape->SetFrameNum(unitAnim().currentFrame);

			if (mechShadowShape && !stripShadowState)
			{
				mechType->setAnimation(mechShadowShape,unitAnim().currentGestureId);
				mechShadowShape->SetFrameNum(unitAnim().currentFrame);
			}
		}
	
		glm::quat torsoRot;
		torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().torsoRotation * DEGREES_TO_RADS),0.0f));
		if (rotationalNodeIndex == -1)
	   		rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

		mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
 	
		if (mechShadowShape)
			mechShape->SetUseShadow(false);
			
		if (mechShadowShape && useShadows && !stripShadowState)
		{
			if (rotationalNodeIndex == -1)
	   			rotationalNodeIndex = mechShadowShape->SetNodeRotation("joint_torso",&torsoRot);

			mechShadowShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);

 			mechShadowShape->SetNodeRotation("joint_torso",&torsoRot);
			mechShadowShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
			// Slice D-shadow-skip: when GPU mech path is on AND skip
			// killswitch is on AND tessellation is active (modern shadow
			// path engaged), omit the call entirely. Mech3DAppearance::
			// renderShadows early-returns on tessellation (mech3d.cpp),
			// so no consumer of TransformMultiShape's outputs exists in
			// this configuration. Modern dynamic shadows use g_shadowShapes[]
			// (txmmgr.cpp-1620), a separate data path that does
			// NOT consume mechShadowShape state. See D-shadow-skip spec
			// §Recon for full grep-verified consumer enumeration including
			// opposite-direction grep on addShadowShape.
			// Tessellation gate is belt-and-suspenders: if a user disables
			// tessellation, the legacy RenderShadows path becomes reachable
			// and would need TransformMultiShape outputs.
			// Slice C3-shadow: on the GPU mech path use _PositionsOnly to skip
			// the per-vertex CPU lighting kernel only. RenderShadows (tgl.cpp)
			// hardcodes gVertex.argb to 0x3f000000 and reads
			// listOfShadowTVertices populated by MultiTransformShadows
			// (which still dispatches at msl.cpp in that branch).
			// MECH-KILLSWITCH-SHADOW-PAIR-RETIRE-1: former MC2_GPU_MECH_SHADOW_SKIP retired.
			// ISSUE-34: modern-shadow-path predicate (was tess-keyed), matching
			// the renderShadows early-return that makes the consumer unreachable.
			if (!gos_IsModernShadowPathActive()) {
				mechShadowShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
			}
		}
		} // end AnimPose zone

		// D-gpu-pose-instrument: BodyXform stage — body's SetLightList +
		// TransformMultiShape* dispatch. The dominant per-mech work; will be
		// further broken down by zones inside TransformMultiShape itself.
		{ ZoneScopedN("Mech3D.UpdateGeometry.BodyXform");
		mechShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
		// Slice C3-revised: when the GPU mech path is on, use _PositionsOnly
		// to skip the per-vertex CPU lighting kernel. Output of that kernel
		// (listOfVertices[j].argb)
		// is only consumed by mechShape->Render(true) which Slice A
		// bypasses; GPU shader does its own lighting via calc_light().
		// BODY ONLY — arms (4459, 4543) and shadow (3377) explicitly
		// stay full TransformMultiShape; their Render(true) callers
		// still depend on the lighting bake.
		// Slice D-leaf-skip-v2: when the GPU mech path is on AND the modern
		// shadow path is active, use HierarchyOnly to additionally skip
		// per-leaf dispatch + MultiTransformShadows.
		// Recon proved zero practical consumer in this configuration
		// (mechShape->Render(true) bypassed by Slice A; RenderShadows
		// unreachable on tessellation; findMoverByMouse rect-only — see
		// killswitch comment in gos_mech_killswitch.h).
		// ISSUE-34: leaf-skip keyed on the modern shadow path (was tess) —
		// its unreachability proof (RenderShadows early-return, Slice A
		// Render(true) bypass, rect-only selection) holds on that predicate.
		if (gos_IsModernShadowPathActive()) {
			mechShape->TransformMultiShape_HierarchyOnly(&xlatPosition, &qRotation);
		} else {
			mechShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
		}
		} // end BodyXform zone
	}

	if (visible && (sensorLevel > 4) && !InEditor && useNonWeaponEffects)
	{
		ZoneScopedN("Mech3D.UpdateGeometry.Effects");
		// Having already transformed the mech, the foot poofs go here.
		glm::vec3 rFootPos, lFootPos;
		long footId1 = mechType->getTotalNodes() - 2;
		long footId0 = mechType->getTotalNodes() - 1;
		
		rFootPos = getNodePosition(footId1);
		lFootPos = getNodePosition(footId0);
	
		if (!rightFootDone0 &&
			(unitAnim().currentFrame >= (mechType->gestures[unitAnim().currentGestureId].rightFootDownFrame0-FOOTPRINT_SLOP)) &&
			(unitAnim().currentFrame <= (mechType->gestures[unitAnim().currentGestureId].rightFootDownFrame0+FOOTPRINT_SLOP)))
		{
			rightFootDone0 = true;
	
			if (rightDustPoofEffect[currentRightPoof] && (unitAnim().currentGestureId != 13))
			{
				glm::vec3			actualPosition;
				glm::mat4 	shapeOrigin;
				glm::mat4	localToWorld;
				
				mc2ToRender(rFootPosition[currentRightPoof], rFootPos);
				
				mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
				mc2::math::buildTranslation(shapeOrigin, rFootPosition[currentRightPoof]);
				
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
				rightDustPoofEffect[currentRightPoof]->Start(&info);
				rightFootPoofDraw[currentRightPoof] = true;
				
				currentRightPoof++;
				if (currentRightPoof >= MAX_DUST_POOFS)
					currentRightPoof = 0;
			}
			
			if (!isWaking)
				Footprints::add(mechType->rightFootprintType,rFootPos,rotation);
		}
		
		if (!rightFootDone1 && 
			(unitAnim().currentFrame >= (mechType->gestures[unitAnim().currentGestureId].rightFootDownFrame1-FOOTPRINT_SLOP)) &&
			(unitAnim().currentFrame <= (mechType->gestures[unitAnim().currentGestureId].rightFootDownFrame1+FOOTPRINT_SLOP)))
		{
			rightFootDone1 = true;
	
			if (rightDustPoofEffect[currentRightPoof] && (unitAnim().currentGestureId != 13))
			{
				glm::vec3			actualPosition;
				glm::mat4 	shapeOrigin;
				glm::mat4	localToWorld;
				
				mc2ToRender(rFootPosition[currentRightPoof], rFootPos);
				
				mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
				mc2::math::buildTranslation(shapeOrigin, rFootPosition[currentRightPoof]);
				
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
				rightDustPoofEffect[currentRightPoof]->Start(&info);
				rightFootPoofDraw[currentRightPoof] = true;
				
				currentRightPoof++;
				if (currentRightPoof >= MAX_DUST_POOFS)
					currentRightPoof = 0;
			}
			
			if (!isWaking)
				Footprints::add(mechType->rightFootprintType,rFootPos,rotation);
		}
		
		if (rightFootDone0 &&
			(unitAnim().currentFrame < (mechType->gestures[unitAnim().currentGestureId].rightFootDownFrame0-FOOTPRINT_SLOP)) ||
		INT32	(unitAnim().currentFrame > (mechType->gestures[unitAnim().currentGestureId].rightFootDownFrame0+FOOTPRINT_SLOP)))
		{
			rightFootDone0 = false;
		}
		
		if (rightFootDone1 &&
			(unitAnim().currentFrame < (mechType->gestures[unitAnim().currentGestureId].rightFootDownFrame1-FOOTPRINT_SLOP)) ||
			(unitAnim().currentFrame > (mechType->gestures[unitAnim().currentGestureId].rightFootDownFrame1+FOOTPRINT_SLOP)))
		{
			rightFootDone1 = false;
		}
		
		if (!leftFootDone0 && 
			(unitAnim().currentFrame >= (mechType->gestures[unitAnim().currentGestureId].leftFootDownFrame0-FOOTPRINT_SLOP)) &&
			(unitAnim().currentFrame <= (mechType->gestures[unitAnim().currentGestureId].leftFootDownFrame0+FOOTPRINT_SLOP)))
		{
			leftFootDone0 = true;
	
			if (leftDustPoofEffect[currentLeftPoof] && (unitAnim().currentGestureId != 13))
			{
				glm::vec3			actualPosition;
				glm::mat4 	shapeOrigin;
				glm::mat4	localToWorld;
				
				mc2ToRender(lFootPosition[currentLeftPoof], lFootPos);
				
				mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
				mc2::math::buildTranslation(shapeOrigin, lFootPosition[currentLeftPoof]);
				
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
				leftDustPoofEffect[currentLeftPoof]->Start(&info);
				leftFootPoofDraw[currentLeftPoof] = true;
				
				currentLeftPoof++;
				if (currentLeftPoof >= MAX_DUST_POOFS)
					currentLeftPoof = 0;
			}
			
			if (!isWaking)
				Footprints::add(mechType->leftFootprintType,lFootPos,rotation);
		}
		
		if (!leftFootDone1 && 
			(unitAnim().currentFrame >= (mechType->gestures[unitAnim().currentGestureId].leftFootDownFrame1-FOOTPRINT_SLOP)) &&
			(unitAnim().currentFrame <= (mechType->gestures[unitAnim().currentGestureId].leftFootDownFrame1+FOOTPRINT_SLOP)))
		{
			leftFootDone1 = true;
	
			if (leftDustPoofEffect[currentLeftPoof] && (unitAnim().currentGestureId != 13))
			{
				glm::vec3			actualPosition;
				glm::mat4 	shapeOrigin;
				glm::mat4	localToWorld;
				
				mc2ToRender(lFootPosition[currentLeftPoof], lFootPos);
				
				mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
				mc2::math::buildTranslation(shapeOrigin, lFootPosition[currentLeftPoof]);
				
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
				leftDustPoofEffect[currentLeftPoof]->Start(&info);
				leftFootPoofDraw[currentLeftPoof] = true;
				
				currentLeftPoof++;
				if (currentLeftPoof >= MAX_DUST_POOFS)
					currentLeftPoof = 0;
			}
			
			if (!isWaking)
				Footprints::add(mechType->leftFootprintType,lFootPos,rotation);
		}
		
		if (leftFootDone0 &&
			(unitAnim().currentFrame < (mechType->gestures[unitAnim().currentGestureId].leftFootDownFrame0-FOOTPRINT_SLOP)) ||
			(unitAnim().currentFrame > (mechType->gestures[unitAnim().currentGestureId].leftFootDownFrame0+FOOTPRINT_SLOP)))
		{
			leftFootDone0 = false;
		}
		
		if (leftFootDone1 &&
			(unitAnim().currentFrame < (mechType->gestures[unitAnim().currentGestureId].leftFootDownFrame1-FOOTPRINT_SLOP)) ||
			(unitAnim().currentFrame > (mechType->gestures[unitAnim().currentGestureId].leftFootDownFrame1+FOOTPRINT_SLOP)))
		{
			leftFootDone1 = false;
		}
	}
	
	// D-gpu-pose-instrument: Sensors stage — sensorTriangleShape +
	// sensorSquareShape transforms. Per user: ALL mechs have sensors;
	// need attribution.
	// MECH-KILLSWITCH-SENSORSKIP-RETIRE-1: on the GPU mech path, skip the entire
	// sensor block when sensorLevel ∈ {0, 5}. Sensor SHAPES only render when
	// sensorLevel ∈ [1,4] (mech3d.cpp); for player mechs (sensorLevel=5)
	// and undetected enemies (sensorLevel=0) the transform work has no consumer.
	// Skip gate is the exact INVERSE of the Render gate — strict no-op. sensorSpin
	// drift while skipped is imperceptible (a continuously-rotating marker that pops
	// in at any angle is indistinguishable). Formerly gated by the default-ON
	// MC2_GPU_MECH_SENSOR_SKIP killswitch (now retired to this constant); still
	// requires g_useGpuMechs (the CPU path transforms sensors regardless).
	{ ZoneScopedN("Mech3D.UpdateGeometry.Sensors");
	const bool skipSensors = (sensorLevel == 0 || sensorLevel >= 5);
	if (!skipSensors) {
	glm::quat totalRotation;
	// VK-MECH-CAPTURE-DETERMINISM-1: the sensor marker spins off the wall-clock
	// frameLength every render frame; during a frozen capture sweep it drifted
	// run-to-run (a rotating triangle/square near detected enemy mechs). Freeze
	// its advance while the capture sim is frozen. Capture-mode-only, byte-safe.
	if (mc2CaptureGaitFreezeActive() == 0 && mc2FreezeMoversActive() == 0)
	sensorSpin += SPIN_RATE * frameLength;
	if (sensorSpin > 180)
		sensorSpin -= 360;

	if (sensorSpin < -180)
		sensorSpin += 360;

	totalRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,sensorSpin * DEGREES_TO_RADS,0.0f));

	float baseRootNodeDifference = baseRootNodeHeight;
	if (isHelicopter)
		baseRootNodeDifference -= HELICOPTER_FACTOR;

	xlatPosition.y += baseRootNodeDifference;
	sensorTriangleShape->SetFogRGB(0xffffffff);
	sensorTriangleShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
	sensorTriangleShape->TransformMultiShape(&xlatPosition,&totalRotation);

	sensorSquareShape->SetFogRGB(0xffffffff);
	sensorSquareShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
	sensorSquareShape->TransformMultiShape(&xlatPosition,&totalRotation);
	} // end !skipSensors
	} // end Sensors zone
	
	if (!jumpFXSetup && (unitAnim().currentGestureId == GestureJump) && unitAnim().inJump && jumpJetEffect)
	{
		long jumpNodeId = mechType->numSmokeNodes + mechType->numWeaponNodes;
		glm::vec3 jumpNodePos = getNodePosition(jumpNodeId);
		
		glm::vec3			actualPosition;
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
		
		mc2ToRender(actualPosition, jumpNodePos);
		
		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, actualPosition);
		
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(JUMP_PITCH * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
					
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
		jumpJetEffect->Start(&info);
		
		jumpFXSetup = true;
	}
	
	for (long i=0;i<MAX_DUST_POOFS;i++)
	{
		if (rightFootPoofDraw[i] && rightDustPoofEffect[i] && rightDustPoofEffect[i]->IsExecuted())
		{
			glm::mat4 	shapeOrigin;
			glm::mat4 	localToWorld;
				
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, rFootPosition[i]);
	
			mc2::math::Obb boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = rightDustPoofEffect[i]->Execute(&info);
			if (!result)
			{
				rightFootPoofDraw[i] = false;
			}
		}
		
		if (leftFootPoofDraw[i] && leftDustPoofEffect[i] && leftDustPoofEffect[i]->IsExecuted())
		{
			glm::mat4 	shapeOrigin;
			glm::mat4 	localToWorld;
				
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, lFootPosition[i]);
	
			mc2::math::Obb boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = leftDustPoofEffect[i]->Execute(&info);
			if (!result)
			{
				leftFootPoofDraw[i] = false;
			}
		}
	}
	
	// The tick no longer touches the jet effect: stop it here once the presented jump has landed.
	if (jumpJetEffect && jumpJetEffect->IsExecuted() && unitAnim().inJump && !unitAnim().jumpAirborne &&
		(unitAnim().currentGestureId == GestureJump) && (unitAnim().currentFrame >= mechType->getJumpLandFrame()))
		jumpJetEffect->Stop();

	if ((unitAnim().currentGestureId == GestureJump) && unitAnim().inJump && jumpJetEffect && jumpJetEffect->IsExecuted())
	{
		long jumpNodeId = mechType->numSmokeNodes + mechType->numWeaponNodes;
		glm::vec3 jumpNodePos = getNodePosition(jumpNodeId);
		
		glm::vec3			actualPosition;
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
		
		mc2ToRender(actualPosition, jumpNodePos);
		
		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, actualPosition);
		
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(JUMP_PITCH * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
					
 		mc2::math::Obb boundingBox;
   		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
		jumpJetEffect->Execute(&info);
	}
	
	if (!unitAnim().inJump && jumpFXSetup)
	{
		jumpJetEffect->Stop();
		jumpFXSetup = false;
	}
	
	if (leftShoulderBoom)
	{
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
				
		if (leftArmNodeIndex == -1)
			leftArmNodeIndex = mechShape->GetNodeNameId("joint_luarm");

		glm::vec3 leftNodePos = getNodeIdPosition(leftArmNodeIndex);
		mc2ToRender(leftShoulderPos, leftNodePos);
		
		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, leftShoulderPos);
				
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
		
 		mc2::math::Obb boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
		bool result = leftShoulderBoom->Execute(&info);
		if (!result)
		{
			leftShoulderBoom->Kill();
			leftShoulderBoom.reset();
		}
	}
	
	if (rightShoulderBoom)
	{
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
				
		if (rightArmNodeIndex == -1)
			rightArmNodeIndex = mechShape->GetNodeNameId("joint_ruarm");

		glm::vec3 rightNodePos = getNodeIdPosition(rightArmNodeIndex);
		mc2ToRender(rightShoulderPos, rightNodePos);
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, rightShoulderPos);
				
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
		
 		mc2::math::Obb boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
		bool result = rightShoulderBoom->Execute(&info);
		if (!result)
		{
			rightShoulderBoom->Kill();
			rightShoulderBoom.reset();
		}
	}
	
	if ((criticalSmoke && (isSmoking > 0)) || (smokeEffect && (isSmoking == 0)))
	{
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
				
		glm::vec3 smokeNodePos = getNodePosition(0);
		glm::vec3 smokePos;
		mc2ToRender(smokePos, smokeNodePos);
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, smokePos);
				
		mc2::math::Obb boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
		
		if (isSmoking > 0)
		{
			criticalSmoke->Execute(&info);
		}
		else
		{
			smokeEffect->Execute(&info);
		}
	}
	
	if (waterWake && isWaking)
	{
		if (movedThisFrame)
		{
			waterWake->SetLoopOn();
			waterWake->SetExecuteOn();
		}
		else
		{
			waterWake->SetLoopOff();
			waterWake->SetExecuteOn();
		}
		
 		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
				
		glm::vec3 wakePos;
		wakePos.x = -position.x;
		wakePos.y = Terrain::waterElevation;
		wakePos.z = position.y;
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, wakePos);
				
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(90.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f));
		if (!unitAnim().inReverse)
			localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		else
			localToWorld = mc2::math::multiply(gosFX::Effect_Into_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
		
 		mc2::math::Obb boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
		
		waterWake->Execute(&info);
	}
	
	if (helicopterDustCloud && isDusting)
	{
 		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
				
		glm::vec3 wakePos;
		mc2ToRender(wakePos, position);
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, wakePos);
				
		
 		mc2::math::Obb boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
		
		bool result = helicopterDustCloud->Execute(&info);
		if (!result)
		{
			helicopterDustCloud->Kill();
			isDusting = false;
		}
	}
	 
}	


void Mech3DAppearance::updateSimPose (bool advanceClock)
{
	ZoneScopedN("GameLogic.Mech3D.UpdateSimPose");
	// Built entirely from the sim copies (simulationShape_, simPosition_/simRotation_/simStatus_, the sim node indices): the
	// draw-side mechShape, position, rotation, status and node indices are never touched here, so presentation and render can
	// use them while a tick runs. The locals shadow the draw members of the same names so the body below is unchanged.
	if (!mechType || !mechType->mechShape[0])
		return;

    if (!simulationShape_) simulationShape_.reset(mechType->mechShape[0]->CreateFrom());
    TG_MultiShapePtr const mechShape = simulationShape_.get();
    const glm::vec3 position = simPosition_;
    const float rotation = simRotation_;
    const long status = simStatus_;
    const long currentLOD = 0;
    long& rotationalNodeIndex = simRotationalNodeIndex_;
    rotationalNodeIndex = simHitLeftNodeIndex_ = simHitRightNodeIndex_ = -1;
    buildingSimPose_ = true;
    simPose->ready = false;
    if (unitAnim().leftArmOff) mechShape->StopUsing("joint_luarm");
    if (unitAnim().rightArmOff) mechShape->StopUsing("joint_ruarm");

	// The same pose updateGeometry sets before its body transform; the hierarchy-only transform needs no camera and fills
	// the per-node shapeToWorld that getWeaponNodePosition / getNodeIdPosition read.
	if (status != OBJECT_STATUS_DESTROYED)
	{
		mechType->setAnimation(mechShape,unitAnim().currentGestureId);
		mechShape->SetFrameNum(unitAnim().currentFrame);
	}

	glm::quat torsoRot;
	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().torsoRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
		rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);
	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);

	glm::quat qRotation;
	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, rotation * DEGREES_TO_RADS, 0.0f));
	glm::vec3 xlatPosition;
	mc2ToRender(xlatPosition, position);
	mechShape->TransformMultiShape_BuildRecipe(&xlatPosition, &qRotation);
	{
		mc2mechanim::MechMotion _mm;
		_mm.gestureId = static_cast<int>(unitAnim().currentGestureId);
		_mm.px = position.x; _mm.py = position.y; _mm.pz = position.z;
		_mm.legHeadingDeg = rotation;
		_mm.destroyed = (status == OBJECT_STATUS_DESTROYED);
		// PER-ACTOR: actorKey = this instance shape; typeKey = the chassis type shape
		// (== the batcher's rec.importedGpuType / desc.mechShape keying).
		const void* _typeKey = (mechType ? static_cast<const void*>(mechType->mechShape[currentLOD]) : nullptr);
		mc2mechanim::TickImportedMechs(frameLength, static_cast<unsigned>(turn), _mm,
		                               static_cast<const void*>(mechShape), _typeKey, advanceClock, &unitAnim().imported);
		// ANIM-FROZEN-WARNING-FIX-1: imported-mech GPU animation IS implemented and
		// working at shipped defaults (per-actor model-delta palette; proven by
		// IMPORTED-GPU-ANIM-READPATH-RECON-1). It is armed by MC2_MECH_IMPORT_GPU.
		// The pose is only FROZEN when the GPU mech path is on but that import-GPU
		// gate is OFF (the stock per-node branch then draws the immutable rest VBO).
		// So warn ONLY in that genuinely-frozen case (was firing unconditionally and
		// LYING whenever the import-GPU path was armed). Both workarounds given.
		if (mc2mechanim::AnyImportedAnim() &&
		    !mc2mechanim::ImportedGpuEnabled()) {
			static bool s_warnedImportGpu = false;
			if (!s_warnedImportGpu) {
				s_warnedImportGpu = true;
				fprintf(stderr, "[MECH_IMPORT] animated imported mech with the import-GPU "
				                "palette OFF shows a FROZEN pose; set MC2_MECH_IMPORT_GPU=1\n");
			}
		}
	}


    simPose->root = position;
    simPose->heading = rotation;
    simPose->nodes.resize(mechType->getTotalNodes());
    for (long i = 0; i < mechType->getTotalNodes(); ++i) simPose->nodes[i] = (i >= mechType->numSmokeNodes && i < mechType->numSmokeNodes + mechType->numWeaponNodes) ? getWeaponNodePosition(i) : getNodePosition(i);
    simPose->hitLeft = getHitNodeLeft();
    simPose->hitRight = getHitNodeRight();
    simPose->cockpit = getNodeNamePosition("cockpit");
    simPose->shapeNodes.resize(mechShape->GetNumShapes());
    for (long i = 0; i < mechShape->GetNumShapes(); ++i) {
        auto node = mechShape->GetTransformedNodePosition(&xlatPosition, &qRotation, i);
        simPose->shapeNodes[i] = (node.x == 0 && node.y == 0 && node.z == 0) ? position : node;
    }
    simPose->ready = true;
    buildingSimPose_ = false;
}

bool Mech3DAppearance::setJumpParameters (glm::vec3 &end)
{
	if (!unitAnim().inJump)
	{
		unitAnim().jumpSetup = true;
		unitAnim().jumpDestination = end;

		return true;
	}

	return false;
}

bool Mech3DAppearance::leftArmRecalc (void)
{
	if (!leftArm)
		return false;

	leftArmInView = false;
	float eyeDistance = 0.0f;
	if (eye->usePerspective)
	{
		glm::vec3 Distance;
		glm::vec3 objPosition;
		glm::vec3 eyePosition(eye->getCameraOrigin());
		mc2ToRender(objPosition, leftArmPos);

		Distance = objPosition - eyePosition;
		eyeDistance = glm::length(Distance);
		if (eyeDistance > Camera::MaxClipDistance)
		{
			leftArmHazeFactor = 1.0f;
			leftArmInView = false;
		}
		else if (eyeDistance > Camera::MinHazeDistance)
		{
			leftArmHazeFactor = (eyeDistance - Camera::MinHazeDistance) * Camera::DistanceFactor;
			leftArmInView = true;
		}
		else
		{
			leftArmHazeFactor = 0.0f;
			leftArmInView = true;
		}

		const bool armParentVisL = s_gpuCullLifecycle
			? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
			: inView;
		if (armParentVisL)
		{
			Distance = glm::normalize(Distance);

			float cosine = glm::dot(Distance, eye->getLookVector());
			if (cosine > eye->cosHalfFOV)
				leftArmInView = true;
			else
				leftArmInView = false;
		}
	}

	return leftArmInView;
}

bool Mech3DAppearance::rightArmRecalc (void)
{
	if (!rightArm)
		return false;

	rightArmInView = false;
	float eyeDistance = 0.0f;
	if (eye->usePerspective)
	{
		glm::vec3 Distance;
		glm::vec3 objPosition;
		glm::vec3 eyePosition(eye->getCameraOrigin());
		mc2ToRender(objPosition, rightArmPos);

		Distance = objPosition - eyePosition;
		eyeDistance = glm::length(Distance);
		if (eyeDistance > Camera::MaxClipDistance)
		{
			rightArmHazeFactor = 1.0f;
			rightArmInView = false;
		}
		else if (eyeDistance > Camera::MinHazeDistance)
		{
			rightArmHazeFactor = (eyeDistance - Camera::MinHazeDistance) * Camera::DistanceFactor;
			rightArmInView = true;
		}
		else
		{
			rightArmHazeFactor = 0.0f;
			rightArmInView = true;
		}

		const bool armParentVisR = s_gpuCullLifecycle
			? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
			: inView;
		if (armParentVisR)
		{
			Distance = glm::normalize(Distance);

			float cosine = glm::dot(Distance, eye->getLookVector());
			if (cosine > eye->cosHalfFOV)
				rightArmInView = true;
			else
				rightArmInView = false;
		}
	}

	return rightArmInView;
}
