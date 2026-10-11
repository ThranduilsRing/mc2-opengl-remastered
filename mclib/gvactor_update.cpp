// GVAppearance: updateGeometry (reads the shadow-skip gate and the spot-light diagnostic counters), update, destroy, water wake and activity effects.
// Declared in the class headers; shared names in gvactor_internal.h.
#include "gvactor_internal.h"
#include "Gates/gate.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"


// Slice GV-shadow-skip (2026-05-09): skip gvShadowShape->TransformMultiShape
// when modern engine has terrain tessellation active. Mirrors mech D-shadow-skip
// (mech3d.cpp). Recon: every byte produced by that call has zero
// consumer in modern + tessellation path:
//   - GVAppearance::renderShadows early-returns on tessellation (gvactor.cpp),
//     making gvShadowShape->RenderShadows(true) at gvactor.cpp unreachable.
//   - GpuStaticPropBatcher::registerMultiShape(appearType->gvShadowShape) at
//     gvactor.cpp is TYPE-level template registration only; per-actor
//     instance state mutated by TransformMultiShape is NOT fed into the batcher
//     (verified at gos_static_prop_batcher.cpp — registers leaf
//     TG_TypeShape templates and primes texture handles, no per-frame state).
// Default-on per the post-mech-flip convention. Opt out: MC2_GPU_GV_SHADOW_SKIP=0.
// Tessellation gate is belt-and-suspenders: if a user disables tessellation,
// the legacy RenderShadows path becomes reachable and would need
// TransformMultiShape outputs.
static const bool g_useGpuGVShadowSkip = mc2::gate::get<"MC2_GPU_GV_SHADOW_SKIP">();
// T1.15 SpotLight_ illumination diagnostic — registration probe (gv class).
// First-hit always-on; per-summary every 600 updateGeometry calls when env=1.
static const bool s_spotDiagGvEnabled = mc2::gate::get<"MC2_SPOT_DIAG">();
static unsigned long s_spotDiagGvRegistered = 0;
static unsigned long s_spotDiagGvOverflows  = 0;
static unsigned long s_spotDiagGvActors     = 0;
static unsigned long s_spotDiagGvCalls      = 0;

void GVAppearance::updateGeometry (void)
{
		if (rotation > 180)
			rotation -= 360;
	
		if (rotation < -180)
			rotation += 360;
	
		if (turretRotation > 180)
			turretRotation -= 360;
	
		if (turretRotation < -180)
			turretRotation += 360;
	
		float pitchAngle = pitch;
		float rollAngle = roll;

		if ((status == OBJECT_STATUS_NORMAL || status == OBJECT_STATUS_SHUTDOWN) && land)  //Are we still oK?
		{
			int cellR, cellC;
			land->worldToCell(position,cellR, cellC);
			if (GameMap && !GameMap->getDeepWater(cellR, cellC) && !GameMap->getShallowWater(cellR, cellC))
			{
				Stuff::Vector3D worldK;
				land->getTerrainAngle(position,&worldK);

				Rotate(worldK,rotation);

				Stuff::Vector3D pitchK;
				pitchK = worldK;
				pitchK.x = 0.0f;
				pitchK.Normalize(pitchK);

				Stuff::Vector3D rollK;
				rollK = worldK;
				rollK.y = 0.0f;
				rollK.Normalize(rollK);

				Stuff::Vector3D up;
				up.x = up.y = 0.0f;
				up.z = 1.0f;

				pitchAngle = up * pitchK;
				pitchAngle = acos(pitchAngle) * RADS_TO_DEGREES;
				if (pitchK.y < 0.0f)
					pitchAngle = -pitchAngle;

				rollAngle = up * rollK;
				rollAngle = acos(rollAngle) * RADS_TO_DEGREES;
				if (rollK.x < 0.0f)
					rollAngle = -rollAngle;
			}
		}
		
		Stuff::UnitQuaternion rot, pitchRoll;
		float yaw = rotation * DEGREES_TO_RADS;
		pitchAngle *= DEGREES_TO_RADS;
		rollAngle *= DEGREES_TO_RADS;
	
		Stuff::UnitQuaternion totalRotation;
		rot = Stuff::EulerAngles(0.0f, yaw, 0.0f);
	
		pitchRoll = Stuff::EulerAngles(pitchAngle, 0.0f, rollAngle);
		totalRotation.Multiply(pitchRoll,rot);
	
		unsigned char lightr,lightg,lightb;
		float lightIntensity = 1.0f;
		if (land)
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
	
		Stuff::Point3D xlatPosition;
		mc2ToRender(xlatPosition, position);
	
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
	
				unsigned char fogResult = fogFactor;
				fogRGB = fogResult << 24;
			}
		}
		else
		{
			fogRGB = 0xff<<24;
		}
	
		if (useFog)
			gvShape->SetFogRGB(fogRGB);
		else
			gvShape->SetFogRGB(0xffffffff);
	
		Stuff::UnitQuaternion turretRot;
		turretRot = Stuff::EulerAngles(0.0f,(turretRotation * DEGREES_TO_RADS),0.0f);
		if (rotationalNodeIndex == -1)
		   	rotationalNodeIndex = gvShape->SetNodeRotation(appearType->rotationalNodeId,&turretRot);

		gvShape->SetNodeRotation(rotationalNodeIndex,&turretRot);
				 
		if (gvShadowShape)
			gvShape->SetUseShadow(false);
			
		if (gvShadowShape && useShadows)
		{
			gvShadowShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
			// Slice GV-shadow-skip: when modern engine has tessellation active,
			// downstream gvShadowShape->RenderShadows(true) (gvactor.cpp) is
			// unreachable so the transform's outputs go unused. See block comment
			// at top of file for full consumer enumeration.
			// ISSUE-34: modern-shadow-path predicate (was tess-keyed), matching
			// the renderShadows early-return above.
			const bool skipShadowXform = g_useGpuGVShadowSkip && gos_IsModernShadowPathActive();
			if (!skipShadowXform)
				gvShadowShape->TransformMultiShape (&xlatPosition,&rot);
		}
		
		gvShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
		gvShape->TransformMultiShape (&xlatPosition,&totalRotation);

		// (E) T1.11: generalised SpotLight_-child illumination on ground
		// vehicles. Mirrors the Mech3DAppearance T1.6/T1.7 block in
		// mech3d.cpp ~:3392-3452 and the BldgAppearance T1.4/T1.5 block in
		// bdactor.cpp ~:1986-2055. Lazy first-night register, per-frame
		// in-place update afterwards (no per-frame pool churn). Public
		// accessors (GetNumShapes/GetShapeRec/GetIsSpotlight) because
		// GVAppearance is NOT in TG_MultiShape's friend list (msl.h
		// ~:251-256).
		// (E) T3.1: gate retired; behavior is now unconditional.
		if (gvShape)
		{
			if (!spotlightsRegistered_ && eye->isNight)
			{
				// [SPOT_DIAG v1] T1.15 per-actor registration counters.
				int diagChildrenWalked = 0;
				int diagSpotlightsFound = 0;
				int diagRegistered = 0;
				int diagOverflow = 0;
				for (int i = 0; i < gvShape->GetNumShapes(); ++i)
				{
					const TG_ShapeRec* recp = gvShape->GetShapeRec(i);
					if (!recp) continue;
					TG_Shape* c = recp->node;
					if (!c || !recp->processMe) continue;
					++diagChildrenWalked;
					if (!c->GetIsSpotlight()) continue;
					++diagSpotlightsFound;

					const char* nodeName = c->getNodeName();
					if (!nodeName) continue;
					long nodeId = gvShape->GetNodeNameId(nodeName);
					if (nodeId == -1) continue;

					TG_LightPtr light = (TG_LightPtr)malloc(sizeof(TG_Light));
					light->init(TG_LIGHT_POINT);              // v1 (OQ2/M6) — POINT
					// MOVER-LIGHT-HALOGEN-1: warm halogen, not legacy yellow.
					light->SetaRGB(mc2_spotlight_diag::mover_light_argb());
					light->SetIntensity(0.15f);                // anubis default
					light->SetFalloffDistances(50.0f, 250.0f); // anubis default

					long slotId = eye->addWorldLight(light);
					if (slotId < 0) { free(light); continue; }
					spotlightNodeIds_.push_back(nodeId);
					spotlightLights_.push_back(light);
					spotlightSlotIds_.push_back(static_cast<DWORD>(slotId));
					// T1.16 — tag this slot as (E)-owned, source=Gv.
					mc2_spotlight_diag::tag_slot(slotId, mc2_spotlight_diag::Gv, this);
					++diagRegistered;

					// First-hit trace (gv-side) — one stderr line per
					// successful GV spotlight registration, matching the
					// MC2_SPOTLIGHT_REAL_TRACE schema. Kept post-T3.1 per the
					// Debug Instrumentation Rule (demote, don't delete).
					MC2_DIAGFE(stderr,
						"[SPOTLIGHT_REAL_TRACE v1] event=gv_first_hit"
						" actorHandle=%ld node=%s slot=%ld\n",
						actorHandle_, nodeName, slotId);
				}
				diagOverflow = diagSpotlightsFound - diagRegistered;
				if (diagOverflow < 0) diagOverflow = 0;
				s_spotDiagGvRegistered += (unsigned long)diagRegistered;
				s_spotDiagGvOverflows  += (unsigned long)diagOverflow;
				++s_spotDiagGvActors;
				if (s_spotDiagGvActors <= 8) {
					MC2_DIAGFE(stderr,
						"[SPOT_DIAG v1] event=first_register class=gv actor_id=%ld "
						"children_walked=%d spotlights_found=%d registered=%d overflow=%d\n",
						actorHandle_, diagChildrenWalked, diagSpotlightsFound,
						diagRegistered, diagOverflow);
					std::fflush(stderr);
				}
				spotlightsRegistered_ = true;  // register-once flag
			}
			// [SPOT_DIAG v1] T1.15 per-summary registration aggregate (gv).
			if (s_spotDiagGvEnabled) {
				++s_spotDiagGvCalls;
				if ((s_spotDiagGvCalls % 600) == 0) {
					mc2::fmt::print_err("[SPOT_DIAG v1] event=registration_summary class=gv calls={:d} actors_first_hit={:d} lights_registered={:d} overflows={:d}\n", mc2::fmt::as_u32(s_spotDiagGvCalls), mc2::fmt::as_u32(s_spotDiagGvActors), mc2::fmt::as_u32(s_spotDiagGvRegistered), mc2::fmt::as_u32(s_spotDiagGvOverflows));
					std::fflush(stderr);
				}
			}

			// Per-frame in-place update. UNCONDITIONAL once registered
			// (lights stay allocated across day/night; toggle active).
			// active gate matches the mech/building pattern verbatim:
			// visible && (sensorLevel > 4) && !InEditor, plus isNight.
			for (size_t k = 0; k < spotlightLights_.size(); ++k)
			{
				Stuff::Vector3D childPos =
					getNodeIdPosition(spotlightNodeIds_[k]);
				// LIGHT-CONE-MODELS-1 (A/C): place the pool at the authored
				// cone's FAR END — aim comes from the node's live transform
				// composed with the cone type geometry's own axis, then the
				// spill anchor is grounded on terrain (headlights light the
				// road where the beam actually lands). Falls back to the
				// MOVER-LIGHT-FORWARD-1 facing heuristic when the model has
				// no usable cone. MC2_LIGHT_CONE_NODES=0 restores heuristic.
				bool coneAimed = false;
				if (mc2_cone_glow::nodes_enabled())
				{
					Stuff::Vector3D aimW;
					float coneLen = 0.0f;
					if (mc2_cone_glow::GetNodeAim(gvShape, spotlightNodeIds_[k], aimW, coneLen))
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
				// vehicle along its facing (headlight semantics; see
				// mech3d.cpp counterpart). MC2_LIGHT_POOL_MOVER_FWD=0
				// restores centered pools.
				if (!coneAimed)
				{
					const float fwdWu = mc2_spotlight_diag::mover_light_fwd_offset();
					if (fwdWu > 0.0f)
					{
						Stuff::Vector3D fwdv;
						fwdv.x = 0.0f; fwdv.y = -1.0f; fwdv.z = 0.0f;
						Rotate(fwdv, -rotation);
						childPos.x += fwdv.x * fwdWu;
						childPos.y += fwdv.y * fwdWu;
					}
				}
				spotlightLights_[k]->SetPosition(&childPos);
				// lightToWorld is consumed at msl.cpp; without it the
				// precomputed s_lightToShape collapses to worldToShape
				// alone and the light's effective world position is lost.
				// Translation-only matrix matches the canonical pattern at
				// mech3d.cpp and bdactor.cpp.
				Stuff::LinearMatrix4D lightToWorldMatrix;
				Stuff::Point3D childPosP;
				childPosP.x = childPos.x; childPosP.y = childPos.y; childPosP.z = childPos.z;
				lightToWorldMatrix.BuildTranslation(childPosP);
				lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(0.0f, 0.0f, 0.0f));
				spotlightLights_[k]->SetLightToWorld(&lightToWorldMatrix);
				// T1.13: dropped (sensorLevel > 4) gate. The original gate
				// was copied verbatim from the anubis searchlight pattern at
				// mech3d.cpp where it correctly restricts to sensor-locked
				// enemy mechs. For GVs the gate is *impossible*: every
				// code/gvehicl.cpp setSensorLevel(N) caller passes 0..4, so
				// (sensorLevel > 4) is never true and all 39 GV spotlight
				// registrations were stuck active=false. Generalized SpotLight_
				// illumination should fire for any visible GV at night.
				// LIGHT-POOL-VIS-GATE-1: destroyed/disabled movers emit nothing
				// (user report: wrecks kept glowing).
				// GHOST-LIGHT-VISIBILITY-1: `visible` is NOT a fog-of-war
				// predicate (code/gvehicl.cpp sets visibility true
				// on every contact branch, sensor-blip ones included), so a
				// vehicle the player holds only as a blip was emitting a
				// searchlight cone that gave away its position. Gate on the
				// predicate that decides whether the MESH draws.
				// Killswitch MC2_LIGHT_MOVER_VIS_GATE=0.
				// VISIBILITY-SEMANTICS-1 job (c) EMITS LIGHT — identical rule
				// to the mech path. Own/allied vehicles always emit (they are
				// never alpha-faded); a foreign vehicle emits only while its
				// mesh is drawn. MC2_VIS_SEMANTICS=0 restores the pre-lane
				// predicate.
				spotlightLights_[k]->active =
					(eye->isNight && !InEditor
					 && mc2_vis::emitsLight(buildVisState()));
				// LIGHT-CULL-RADIUS-1: owner intent for the camera composition.
				spotlightLights_[k]->ownerEnabled =
					spotlightLights_[k]->active ? 1 : 0;
				// LIGHT-CONE-MODELS-1 (B): visible glowing beam volume, tied
				// to the SAME owner predicate — shutdown/dead/FoW-hidden
				// vehicles show no cone. MC2_LIGHT_CONE_GLOW=0 kills it.
				if (spotlightLights_[k]->active)
					mc2_cone_glow::QueueConeGlow(gvShape, spotlightNodeIds_[k],
						spotlightLights_[k]->GetaRGB(), eye->getNightFactor(),
						/*ownerIsMover=*/true);
			}
		}
	
	if (destructFX && destructFX->IsExecuted())
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D 	localToWorld;
		Stuff::Point3D			tPosition;
			
		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(tPosition);

		mc2ToRender(tPosition, position);
		
		Stuff::OBB boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);

		bool result = destructFX->Execute(&info);
		if (!result)
		{
			destructFX->Kill();
			delete destructFX;
			destructFX = nullptr;
		}
	}
	
	if (waterWake && isWaking)
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		Stuff::Point3D wakePos;
		wakePos.x = -position.x;
		wakePos.y = Terrain::waterElevation;
		wakePos.z = position.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(90.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		Stuff::OBB boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
		
		waterWake->Execute(&info);
	}
	else if (dustCloud)
	{
		if (movedThisFrame && !isInfantry)
		{
			dustCloud->SetLoopOn();
			dustCloud->SetExecuteOn();
		}
		else
		{
			dustCloud->SetLoopOff();
			dustCloud->SetExecuteOn();
		}
		
		if (!dustCloudStart)
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
					
			if (dustNodeIndex == -1)
				dustNodeIndex = gvShape->GetNodeNameId("dust_body");

			Stuff::Vector3D dustPos = getNodeIdPosition(dustNodeIndex);

			Stuff::Point3D wakePos;
			mc2ToRender(wakePos, dustPos);
 			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(wakePos);
					
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
				
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
	
			dustCloud->Start(&info);
			dustCloudStart = true;
		}
	
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		if (dustNodeIndex == -1)
			dustNodeIndex = gvShape->GetNodeNameId("dust_body");

		Stuff::Vector3D dustPos = getNodeIdPosition(dustNodeIndex);

		Stuff::Point3D wakePos;
		mc2ToRender(wakePos, dustPos);
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		Stuff::OBB boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
		
		dustCloud->Execute(&info);
	}
	
	if (activity && isActivitying)
	{
 		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		if (activityNodeIndex == -1)
			activityNodeIndex = gvShape->GetNodeNameId("activity_node");

		Stuff::Vector3D dustPos = getNodeIdPosition(activityNodeIndex);

		Stuff::Point3D wakePos;
		mc2ToRender(wakePos, dustPos);
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
				
		
 		Stuff::OBB boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
		
		activity->Execute(&info);
	}
	
	// FREEZE-MOVERS-1: the mech path got a spin freeze in
	// VK-MECH-CAPTURE-DETERMINISM-1 (mech3d.cpp) and the VEHICLE path did not,
	// so a frozen pose containing a vehicle still drifted a rotating sensor
	// marker run-to-run. Guarded on freeze_movers ONLY (not on the capture
	// predicate) so existing visual-capture goldens render exactly as before.
	if (mc2FreezeMoversActive() == 0)
	sensorSpin += SPIN_RATE * frameLength;
	if (sensorSpin > 180)
		sensorSpin -= 360;

	if (sensorSpin < -180)
		sensorSpin += 360;

	totalRotation = Stuff::EulerAngles(0.0f,sensorSpin * DEGREES_TO_RADS,0.0f);
	sensorTriangleShape->SetFogRGB(0xffffffff);
	sensorTriangleShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
	sensorTriangleShape->TransformMultiShape (&xlatPosition,&totalRotation);
		
	sensorCircleShape->SetFogRGB(0xffffffff);
	sensorCircleShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
	sensorCircleShape->TransformMultiShape (&xlatPosition,&totalRotation);
}

long GVAppearance::update (bool animate) 
{
	if (nodeRecycle)
	{
		for (long i=0;i<appearType->numWeaponNodes;i++)
		{
			if (nodeRecycle[i] > 0.0f)
			{
				nodeRecycle[i] -= frameLength;
				if (nodeRecycle[i] < 0.0f)
					nodeRecycle[i] = 0.0f;
			}
		}
	}

	//Update flashing regardless of view!!!
	// FREEZE-MOVERS-1: same wall-clock blink as the mech path; see mech3d.cpp.
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

	//Always override with our local instance.
	gvShape->SetTextureHandle(0,localTextureHandle);
 
	// VEHICLE-NIGHT-LIT-2 (GV-LIGHTSOUT-CLEAR): the legacy code set lightsOut
	// for DESTROYED/DISABLED but never cleared it, unlike the mech path
	// (mech3d.cpp Mech3DAppearance::update, which sets BOTH arms). Once a
	// vehicle latched lightsOut it could never be lit again — including after
	// an actor slot is recycled for a live vehicle. MC2_GV_LIGHTSOUT_CLEAR=1
	// adds the missing else-branch so the flag tracks status like the mech
	// path does. Default-OFF -> legacy latch preserved -> byte-identical.
	{
		static int s_gvLightsOutClear = -1;
		if (s_gvLightsOutClear < 0)
		{
			s_gvLightsOutClear = mc2::gate::get<"MC2_GV_LIGHTSOUT_CLEAR">();
		}
		if ((status == OBJECT_STATUS_DESTROYED) || (status == OBJECT_STATUS_DISABLED))
		{
			gvShape->SetLightsOut(true);
		}
		else if (s_gvLightsOutClear)
		{
			gvShape->SetLightsOut(false);
		}
	}

	// FREEZE-MOVERS-1: the VEHICLE track/limb animation had NO freeze guard at
	// all -- not the mech path's ANIM-CADENCE-FIX, not a capture check. Since
	// Mover::getLOSPosition (code/mover.cpp) calls appearance->update()
	// with the default animate=true, and LOS resolution still runs while movers
	// are frozen, a frozen vehicle's animation could keep ticking off the
	// wall-clock frameLength. Same mech-vs-vehicle asymmetry as sensorSpin.
 	if (animate && gvFrameRate != 0.0f && (mc2FreezeMoversActive() == 0))
	{
		//--------------------------------------------------------
		// Make sure animation runs no faster than bdFrameRate fps.
		float frameInc = gvFrameRate * frameLength;
		
		if (frameInc != 0.0f)
		{
			if (!setFirstFrame)		//DO NOT ANIMATE ON FIRST FRAME!  Wait a bit!
			{
				if (isReversed)
					currentFrame -= frameInc;
				else
					currentFrame += frameInc;
			}
			else
			{
				setFirstFrame = false;
			}
	
			if (currentFrame >= appearType->getNumFrames(gvAnimationState))
			{
				if (isLooping)
					currentFrame -= appearType->getNumFrames(gvAnimationState);
				else
					currentFrame = appearType->getNumFrames(gvAnimationState) - 1;
					
				canTransition = true;		//Whenever we have completed one cycle or at last frame, OK to move on!
			}
			
	
			if (currentFrame < 0)
			{
				if (isLooping)
					currentFrame += appearType->getNumFrames(gvAnimationState); 
				else
					currentFrame = 0.0f; 
					
				canTransition = true;		//Whenever we have completed one cycle or at last frame, OK to move on!
			}
		}
		
		gvShape->SetFrameNum(currentFrame);
	}

	// LIGHT-CULL-RADIUS-1 freshness (batch-6 item 5 ext a/c): light owner
	// intent + coarse position refreshed EVERY frame even when
	// updateGeometry is frustum-culled below (see mech3d.cpp counterpart
	// for the ghost-light/shutdown-staleness mechanism writeup).
	if (!spotlightLights_.empty())
	{
		// GHOST-LIGHT-VISIBILITY-1: keep the off-screen freshness refresh in
		// lockstep with the updateGeometry predicate — a sensor-blip-only
		// enemy must not keep ownerEnabled latched while culled.
		// VISIBILITY-SEMANTICS-1: job (c), in lockstep with updateGeometry.
		const mc2_vis::MoverVis vs_ = buildVisState();
		const bool ownerOn =
			(eye && eye->isNight && !InEditor && mc2_vis::emitsLight(vs_));
		mc2_vis::trace("vehicle", (const void*)this, vs_);
		if (ownerOn)
		{
			Stuff::Vector3D lpos = position;
			const float fwdWu = mc2_spotlight_diag::mover_light_fwd_offset();
			if (fwdWu > 0.0f)
			{
				Stuff::Vector3D fwdv;
				fwdv.x = 0.0f; fwdv.y = -1.0f; fwdv.z = 0.0f;
				Rotate(fwdv, -rotation);
				lpos.x += fwdv.x * fwdWu;
				lpos.y += fwdv.y * fwdWu;
			}
			Stuff::LinearMatrix4D l2w;
			Stuff::Point3D lposP;
			lposP.x = lpos.x; lposP.y = lpos.y; lposP.z = lpos.z;
			l2w.BuildTranslation(lposP);
			l2w.BuildRotation(Stuff::EulerAngles(0.0f, 0.0f, 0.0f));
			for (size_t k = 0; k < spotlightLights_.size(); ++k)
			{
				spotlightLights_[k]->ownerEnabled = 1;
				spotlightLights_[k]->SetPosition(&lpos);
				spotlightLights_[k]->SetLightToWorld(&l2w);
			}
		}
		else
		{
			for (size_t k = 0; k < spotlightLights_.size(); ++k)
				spotlightLights_[k]->ownerEnabled = 0;
		}
	}

	// Mirror the mech fix: under the GPU static-prop killswitch always
	// run updateGeometry so gvShape->TransformMultiShape runs every
	// frame, even when the broken cull thinks the vehicle is off-
	// screen. Without this, TG_Shape::Render silently returns on
	// stale listOfVertices and the vehicle geometry disappears while
	// its bar/UI keeps drawing.
	// C3: route inView input to GPU-lagged visibility when killswitch is enabled.
	// PRESERVE cascade gate structure: turn<3 condition is unchanged.
	{
		const bool gpuVis = s_gpuCullLifecycle
			? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
			: inView;
		if ((turn < 3) || gpuVis || g_useGpuStaticProps)
			updateGeometry();
	}
		
	return TRUE;
}

void GVAppearance::destroy (void)
{
	// (E) T1.12: paired cleanup for SpotLight_-child illumination from
	// T1.11. Walks CACHED state only (spotlightLights_/spotlightSlotIds_);
	// does NOT call getNodeIdPosition or any gvShape method — same
	// destroy-ordering discipline as mech3d.cpp T1.8 and bdactor.cpp T1.5.
	// Unlike Mech3DAppearance there is no pre-existing pointLight pair to
	// worry about leaking; GVAppearance had no legacy spotlight wiring
	// before T1.10.
	for (size_t k = 0; k < spotlightLights_.size(); ++k)
	{
		if (eye)
			eye->removeWorldLight(spotlightSlotIds_[k], spotlightLights_[k]);
		// T1.16 — pair untag with removeWorldLight.
		mc2_spotlight_diag::untag_slot(static_cast<long>(spotlightSlotIds_[k]));
		free(spotlightLights_[k]);
	}
	spotlightLights_.clear();
	spotlightSlotIds_.clear();
	spotlightNodeIds_.clear();
	spotlightsRegistered_ = false;

	if (gvShape)
	{
		delete gvShape;
		gvShape = nullptr;
	}
	
	if (gvShadowShape)
	{
		delete gvShadowShape;
		gvShadowShape = nullptr;
	}
	
	if (sensorCircleShape)
	{
		delete sensorCircleShape;
		sensorCircleShape = nullptr;	
	}
	   
	if (sensorTriangleShape)
	{
		delete sensorTriangleShape;
		sensorTriangleShape = nullptr;	
	}

	if (destructFX)
	{
		destructFX->Kill();
		delete destructFX;
		destructFX = nullptr;
	}

	if (waterWake)
	{
		waterWake->Kill();
		delete waterWake;
		waterWake = nullptr;
	}

	if (dustCloud)
	{
		dustCloud->Kill();
		delete dustCloud;
		dustCloud = nullptr;
	}

	if (activity)
	{
		activity->Kill();
		delete activity;
		activity = nullptr;
	}

	appearanceTypeList->removeAppearance(appearType);
}

void GVAppearance::startWaterWake (void)
{
	
	if (!waterWake && useNonWeaponEffects)
	{
   		if (strcmp(weaponEffects->GetEffectName(VEHICLE_WATER_WAKE),"NONE") != 0)
   		{
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(VEHICLE_WATER_WAKE));
			
			if (gosEffectSpec)
			{
				waterWake = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(waterWake != nullptr);
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if (waterWake && !isWaking)		//Start the effect if we are not running it yet!!
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		Stuff::Point3D wakePos;
		wakePos.x = -position.x;
		wakePos.y = Terrain::waterElevation;	//Wake is at Water Level!
		wakePos.z = position.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(90.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);

		waterWake->Start(&info);

		isWaking = true;
	}
}

void GVAppearance::stopWaterWake (void)
{
	if (waterWake && isWaking)		//Stop the effect if we are running it!!
	{
		waterWake->Kill();
	}
	
	isWaking = false;
}

void GVAppearance::startActivity (long effectId, bool loop)
{
	
	//First, check if its even loaded.
	// can easily preload this.  Should we?  NO.  We don't know what will be passed in.
	if (!activity && useNonWeaponEffects)
	{
   		if (strcmp(weaponEffects->GetEffectName(effectId),"NONE") != 0)
   		{
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;
			if (!loop)
				flags = gosFX::Effect::ExecuteFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(effectId));
			
			if (gosEffectSpec)
			{
				activity = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(activity != nullptr);
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if (!isActivitying && activity)		//Start the effect if we are not running it yet!!
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
		
		if (activityNodeIndex == -1)
			activityNodeIndex = gvShape->GetNodeNameId("activity_node");

		Stuff::Vector3D nodePos = getNodeIdPosition(activityNodeIndex);

		Stuff::Point3D wakePos;
		mc2ToRender(wakePos, nodePos);	//Wake is at Water Level!
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
				
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);

		activity->Start(&info);
		
		isActivitying = true;
	}
}

void GVAppearance::stopActivity (void)
{
	if (activity && isActivitying)		//Stop the effect if we are running it!!
	{
		activity->Kill();
	}
	
	isActivitying = false;
}

bool GVAppearance::PerPolySelect (long mouseX, long mouseY)
{
	return gvShape->PerPolySelect(mouseX,mouseY);
}

void GVAppearance::flashBuilding (float dur, float fDuration, DWORD color)
{
	duration = dur;
	flashDuration = fDuration;
	flashColor = color;
	drawFlash = true;
	currentFlash = flashDuration;
}
