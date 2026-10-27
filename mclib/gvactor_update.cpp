#include "unit_animation_tick.h"
// GVAppearance: updateGeometry (reads the shadow-skip gate and the spot-light diagnostic counters), update, destroy, water wake and activity effects.
// Declared in the class headers; shared names in gvactor_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "gvactor_internal.h"
#include "Gates/gate.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"
#include "mover_lamps.h"


// T1.15 SpotLight_ illumination diagnostic — registration probe (gv class).
// First-hit always-on; per-summary every 600 updateGeometry calls when env=1.
static const bool s_spotDiagGvEnabled = mc2::gate::get<"MC2_SPOT_DIAG">();
static unsigned long s_spotDiagGvRegistered = 0;
static unsigned long s_spotDiagGvOverflows  = 0;
static unsigned long s_spotDiagGvActors     = 0;
static unsigned long s_spotDiagGvCalls      = 0;

glm::quat GVAppearance::poseRotation() const
{
		float pitchAngle = unitAnim().pitch;
		float rollAngle = unitAnim().roll;

		if ((status == OBJECT_STATUS_NORMAL || status == OBJECT_STATUS_SHUTDOWN) && land)  //Are we still oK?
		{
			int cellR, cellC;
			land->worldToCell(position,cellR, cellC);
			if (GameMap && !GameMap->getDeepWater(cellR, cellC) && !GameMap->getShallowWater(cellR, cellC))
			{
				glm::vec3 worldK;
				land->getTerrainAngle(position,&worldK);

				Rotate(worldK,rotation);

				glm::vec3 pitchK;
				pitchK = worldK;
				pitchK.x = 0.0f;
				pitchK = glm::normalize(pitchK);

				glm::vec3 rollK;
				rollK = worldK;
				rollK.y = 0.0f;
				rollK = glm::normalize(rollK);

				glm::vec3 up;
				up.x = up.y = 0.0f;
				up.z = 1.0f;

				pitchAngle = glm::dot(up, pitchK);
				pitchAngle = acos(pitchAngle) * RADS_TO_DEGREES;
				if (pitchK.y < 0.0f)
					pitchAngle = -pitchAngle;

				rollAngle = glm::dot(up, rollK);
				rollAngle = acos(rollAngle) * RADS_TO_DEGREES;
				if (rollK.x < 0.0f)
					rollAngle = -rollAngle;
			}
		}
		
		glm::quat rot, pitchRoll;
		float yaw = rotation * DEGREES_TO_RADS;
		pitchAngle *= DEGREES_TO_RADS;
		rollAngle *= DEGREES_TO_RADS;
	
		glm::quat totalRotation;
		rot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
	
		pitchRoll = mc2::math::quatFromEuler(mc2::math::Euler(pitchAngle, 0.0f, rollAngle));
		totalRotation = mc2::math::composeQuat(pitchRoll, rot);
	
    return totalRotation;
}

void GVAppearance::updateGeometry (void)
{
		if (unitAnim().gvAnimationState >= 0) appearType->setAnimation(gvShape, unitAnim().gvAnimationState);
		gvShape->SetFrameNum(unitAnim().currentFrame);
		if (rotation > 180)
			rotation -= 360;
	
		if (rotation < -180)
			rotation += 360;
	
		if (unitAnim().turretRotation > 180)
			unitAnim().turretRotation -= 360;
	
		if (unitAnim().turretRotation < -180)
			unitAnim().turretRotation += 360;
	
		auto totalRotation = poseRotation();

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
	
		glm::vec3 xlatPosition;
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
	
		glm::quat turretRot;
		turretRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().turretRotation * DEGREES_TO_RADS),0.0f));
		if (rotationalNodeIndex == -1)
		   	rotationalNodeIndex = gvShape->SetNodeRotation(appearType->rotationalNodeId,&turretRot);

		gvShape->SetNodeRotation(rotationalNodeIndex,&turretRot);
				 
		if (gvShadowShape)
			gvShape->SetUseShadow(false);
			
		// Pose on the GPU: update walks the node hierarchy only (shapeToWorld per node); the vertex transform,
		// lighting and fog run in the vertex shader. A shape that cannot go to a batcher (fading alpha, a type
		// not registered yet) is drawn by the CPU fallback (TG_MultiShape::Render) from the same matrices.
		poseXlat_ = xlatPosition;
		poseLightRGB_ = lightRGB;
		poseFog_ = useFog ? 1.0f - static_cast<float>(fogRGB >> 24) / 255.0f : 0.0f;
		gvShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
		gpuPose_ = g_mechPreviewRenderDepth == 0 && gvShape->GetAlphaValue() == 0xff
			&& GpuMechBatcher::instance().isVehicleTypeRegistered(gvShape->GetMultiType());
		// The light row carries this actor's own terrain light colour; gather it now, while light 0 holds it.
		if (gpuPose_ && !renderShape)
			gvShape->CacheGpuLightData();
		gvShape->TransformMultiShape_HierarchyOnly (&xlatPosition,&totalRotation);
		if (!gpuPose_ && !renderShape)
			gvShape->ReserveCpuDraw();	// fading or unregistered: the CPU fallback draws it
		updateRenderShape();
		if (renderShape) renderShape->SetFogRGB(useFog ? fogRGB : 0xffffffff);

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
					TG_Shape* c = recp->node.get();
					if (!c || !recp->processMe) continue;
					++diagChildrenWalked;
					if (!c->GetIsSpotlight()) continue;
					++diagSpotlightsFound;

					const char* nodeName = c->getNodeName();
					if (!nodeName) continue;
					long nodeId = gvShape->GetNodeNameId(nodeName);
					if (nodeId == -1) continue;

					if (!mc2_mover_lamps::registerLamps(gvShape, nodeId, spotlightNodeIds_, spotlightLights_, spotlightSlotIds_,
					                                    mc2_spotlight_diag::Gv, this))
						continue;
					++diagRegistered;

					// First-hit trace (gv-side) — one stderr line per
					// successful GV spotlight registration, matching the
					// MC2_SPOTLIGHT_REAL_TRACE schema. Kept post-T3.1 per the
					// Debug Instrumentation Rule (demote, don't delete).
					MC2_DIAGFE(stderr,
						"[SPOTLIGHT_REAL_TRACE v1] event=gv_first_hit"
						" actorHandle=%ld node=%s slot=%ld\n",
						actorHandle_, nodeName, static_cast<long>(spotlightSlotIds_.back()));
				}
				diagOverflow = diagSpotlightsFound - diagRegistered;
				if (diagOverflow < 0) diagOverflow = 0;
				s_spotDiagGvRegistered += static_cast<unsigned long>(diagRegistered);
				s_spotDiagGvOverflows  += static_cast<unsigned long>(diagOverflow);
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
			mc2_mover_lamps::LampCursor lampCursor;
			for (size_t k = 0; k < spotlightLights_.size(); ++k)
			{
				glm::vec3 childPos =
					getNodeIdPosition(spotlightNodeIds_[k]);
				bool coneAimed = lampCursor.next(gvShape, spotlightNodeIds_[k], spotlightLights_[k], childPos);
				// LIGHT-CONE-MODELS-1 (A/C): place the pool at the authored
				// cone's FAR END — aim comes from the node's live transform
				// composed with the cone type geometry's own axis, then the
				// spill anchor is grounded on terrain (headlights light the
				// road where the beam actually lands). Falls back to the
				// MOVER-LIGHT-FORWARD-1 facing heuristic when the model has
				// no usable cone. MC2_LIGHT_CONE_NODES=0 restores heuristic.
				if (!coneAimed && mc2_cone_glow::nodes_enabled())
				{
					glm::vec3 aimW;
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
						glm::vec3 fwdv;
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
				glm::mat4 lightToWorldMatrix;
				glm::vec3 childPosP;
				childPosP.x = childPos.x; childPosP.y = childPos.y; childPosP.z = childPos.z;
				mc2::math::buildTranslation(lightToWorldMatrix, childPosP);
				mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(0.0f, 0.0f, 0.0f));
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
				if (spotlightLights_[k]->active && lampCursor.lampIdx == 0)   // the glow draws the whole leaf once
					mc2_cone_glow::QueueConeGlow(gvShape, spotlightNodeIds_[k],
						spotlightLights_[k]->GetaRGB(), eye->getNightFactor(),
						/*ownerIsMover=*/true);
			}
		}
	
	if (destructFX && destructFX->IsExecuted())
	{
		glm::mat4 	shapeOrigin;
		glm::mat4 	localToWorld;
		glm::vec3			tPosition;
			
		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, tPosition);

		mc2ToRender(tPosition, position);
		
		mc2::math::Obb boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);

		bool result = destructFX->Execute(&info);
		if (!result)
		{
			destructFX->Kill();
			destructFX.reset();
		}
	}
	
	if (waterWake && isWaking)
	{
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
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
		
 		mc2::math::Obb boundingBox;
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
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			glm::mat4	localResult;
					
			if (dustNodeIndex == -1)
				dustNodeIndex = gvShape->GetNodeNameId("dust_body");

			glm::vec3 dustPos = getNodeIdPosition(dustNodeIndex);

			glm::vec3 wakePos;
			mc2ToRender(wakePos, dustPos);
 			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, wakePos);
					
			glm::quat effectRot;
			effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
			localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
			localResult = localToWorld * shapeOrigin;
				
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
	
			dustCloud->Start(&info);
			dustCloudStart = true;
		}
	
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
				
		if (dustNodeIndex == -1)
			dustNodeIndex = gvShape->GetNodeNameId("dust_body");

		glm::vec3 dustPos = getNodeIdPosition(dustNodeIndex);

		glm::vec3 wakePos;
		mc2ToRender(wakePos, dustPos);
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, wakePos);
				
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
		
 		mc2::math::Obb boundingBox;
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
		
		dustCloud->Execute(&info);
	}
	
	if (activity && isActivitying)
	{
 		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
				
		if (activityNodeIndex == -1)
			activityNodeIndex = gvShape->GetNodeNameId("activity_node");

		glm::vec3 dustPos = getNodeIdPosition(activityNodeIndex);

		glm::vec3 wakePos;
		mc2ToRender(wakePos, dustPos);
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, wakePos);
				
		
 		mc2::math::Obb boundingBox;
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

}

// The contact markers are drawn only at sensor levels 1..4, so they are transformed at draw time, not every update.
void GVAppearance::transformSensorShapes()
{
	TG_MultiShape* marker = sensorLevel > 1 ? sensorCircleShape : sensorTriangleShape;
	auto spin = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,sensorSpin * DEGREES_TO_RADS,0.0f));
	marker->SetFogRGB(0xffffffff);
	marker->SetLightList(eye->getWorldLights(),eye->getNumLights());
	marker->TransformMultiShape_HierarchyOnly (&poseXlat_,&spin);
	marker->ReserveCpuDraw();
}

long GVAppearance::update (bool animate) 
{
	if (!simPresentationPass && simulationOwned()) {
		if (unitAnim().lastAnimAdvanceFrame == static_cast<uint32_t>(turn)) { updateSimPose(); return TRUE; }
		unitAnim().lastAnimAdvanceFrame = static_cast<uint32_t>(turn);
	}
	if (simPresentationPass) presentEffects();

	if (!simPresentationPass || !simulationOwned()) {

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

	}

	if (simPresentationPass || !simulationOwned()) {
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
	// an actor slot is recycled for a live vehicle. The else-branch makes the
	// flag track status like the mech path does.
	if ((status == OBJECT_STATUS_DESTROYED) || (status == OBJECT_STATUS_DISABLED))
		gvShape->SetLightsOut(true);
	else
		gvShape->SetLightsOut(false);
	}

	// FREEZE-MOVERS-1: the VEHICLE track/limb animation had NO freeze guard at
	// all -- not the mech path's ANIM-CADENCE-FIX, not a capture check. Since
	// Mover::getLOSPosition (code/mover.cpp) calls appearance->update()
	// with the default animate=true, and LOS resolution still runs while movers
	// are frozen, a frozen vehicle's animation could keep ticking off the
	// wall-clock frameLength. Same mech-vs-vehicle asymmetry as sensorSpin.
	if ((!simPresentationPass || !simulationOwned()) && animate) mc2::sim::advanceLinearAnimation(unitAnim(), unitAnim().gvFrameRate, appearType->getNumFrames(unitAnim().gvAnimationState), frameLength);

	if (!simPresentationPass && simulationOwned() && actorHandle_ >= 0) { updateSimPose(); return TRUE; }

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
		mc2_vis::trace("vehicle", static_cast<const void*>(this), vs_);
		if (ownerOn)
		{
			glm::vec3 lpos = position;
			const float fwdWu = mc2_spotlight_diag::mover_light_fwd_offset();
			if (fwdWu > 0.0f)
			{
				glm::vec3 fwdv;
				fwdv.x = 0.0f; fwdv.y = -1.0f; fwdv.z = 0.0f;
				Rotate(fwdv, -rotation);
				lpos.x += fwdv.x * fwdWu;
				lpos.y += fwdv.y * fwdWu;
			}
			glm::mat4 l2w;
			glm::vec3 lposP;
			lposP.x = lpos.x; lposP.y = lpos.y; lposP.z = lpos.z;
			mc2::math::buildTranslation(l2w, lposP);
			mc2::math::buildRotation(l2w, mc2::math::Euler(0.0f, 0.0f, 0.0f));
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
		free(spotlightLights_[k]);
	}
	spotlightLights_.clear();
	spotlightSlotIds_.clear();
	spotlightNodeIds_.clear();
	spotlightsRegistered_ = false;
	renderShape.reset();
	renderLOD = -1;
	for (auto handle : renderTextures) if (handle != 0xffffffff) mc2::textures::release(handle);
	renderTextures.clear();

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
		destructFX.reset();
	}

	if (waterWake)
	{
		waterWake->Kill();
		waterWake.reset();
	}

	if (dustCloud)
	{
		dustCloud->Kill();
		dustCloud.reset();
	}

	if (activity)
	{
		activity->Kill();
		activity.reset();
	}

	appearanceTypeList->removeAppearance(appearType);
}

void GVAppearance::startWaterWake (void)
{
	if (!simPresentationPass) { unitAnim().wakeRequested = true; return; }

	
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
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
				
		glm::vec3 wakePos;
		wakePos.x = -position.x;
		wakePos.y = Terrain::waterElevation;	//Wake is at Water Level!
		wakePos.z = position.y;
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, wakePos);
				
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(90.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);

		waterWake->Start(&info);

		isWaking = true;
	}
}

void GVAppearance::stopWaterWake (void)
{
	if (!simPresentationPass) { unitAnim().wakeRequested = false; return; }

	if (waterWake && isWaking)		//Stop the effect if we are running it!!
	{
		waterWake->Kill();
	}
	
	isWaking = false;
}

void GVAppearance::startActivity (long effectId, bool loop)
{
	if (!simPresentationPass) { unitAnim().activityEffect = effectId; unitAnim().activityLoop = loop; return; }

	
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
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
		
		if (activityNodeIndex == -1)
			activityNodeIndex = gvShape->GetNodeNameId("activity_node");

		glm::vec3 nodePos = getNodeIdPosition(activityNodeIndex);

		glm::vec3 wakePos;
		mc2ToRender(wakePos, nodePos);	//Wake is at Water Level!
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, wakePos);
				
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);

		activity->Start(&info);
		
		isActivitying = true;
	}
}

void GVAppearance::stopActivity (void)
{
	if (!simPresentationPass) { unitAnim().activityEffect = -2; return; }

	if (activity && isActivitying)		//Stop the effect if we are running it!!
	{
		activity->Kill();
	}
	
	isActivitying = false;
}

TG_MultiShape* GVAppearance::previewShape (void)
{
	TG_MultiShape* shape = renderShape ? renderShape.get() : gvShape;
	if (!shape)
		return nullptr;
	if (!renderShape) shape->SetTextureHandle(0,localTextureHandle);
	shape->SetARGBHighLight(highlightColor);
	return shape;
}

bool GVAppearance::PerPolySelect (long mouseX, long mouseY)
{
	return false;	// movers are picked by rect (findMoverByMouse); no per-face CPU geometry
}

void GVAppearance::flashBuilding (float dur, float fDuration, DWORD color)
{
	duration = dur;
	flashDuration = fDuration;
	flashColor = color;
	drawFlash = true;
	currentFlash = flashDuration;
}

void GVAppearance::updateSimPose() {
    if (!gvShape) return;
    simPose->ready = false;
    glm::vec3 translation;
    mc2ToRender(translation, position);
    auto rootRotation = poseRotation();
    auto turret = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, unitAnim().turretRotation * DEGREES_TO_RADS, 0.0f));
    gvShape->SetNodeRotation(appearType->rotationalNodeId, &turret);
    gvShape->SetFrameNum(unitAnim().currentFrame);
    gvShape->TransformMultiShape_BuildRecipe(&translation, &rootRotation);
    simPose->root = position;
    simPose->heading = rotation;
    simPose->nodes.resize(appearType->getTotalNodes());
    for (long i = 0; i < appearType->getTotalNodes(); ++i) {
        if (i >= appearType->numSmokeNodes && i < appearType->numSmokeNodes + appearType->numWeaponNodes) {
            const auto index = i - appearType->numSmokeNodes;
            weaponNodeId[index] = gvShape->GetNodeNameId(appearType->nodeData[i].nodeId);
            if (weaponNodeId[index] < 0) weaponNodeId[index] = -2;
        }
        simPose->nodes[i] = gvShape->GetTransformedNodePosition(&translation, &rootRotation, appearType->nodeData[i].nodeId);
        if (simPose->nodes[i] == glm::vec3(0.0f)) simPose->nodes[i] = position;
    }
    simPose->shapeNodes.resize(gvShape->GetNumShapes());
    for (long i = 0; i < gvShape->GetNumShapes(); ++i)
        simPose->shapeNodes[i] = gvShape->GetTransformedNodePosition(&translation, &rootRotation, i);
    const auto hit = gvShape->GetNodeNameId("hitnode");
    simPose->hitLeft = hit >= 0 && static_cast<size_t>(hit) < simPose->shapeNodes.size()
        ? simPose->shapeNodes[hit] : position;
    simPose->hitRight = simPose->hitLeft;
    simPose->ready = true;
}
