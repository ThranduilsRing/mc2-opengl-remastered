// Mech3DAppearance: Mech3DAppearance::update, arm destruction, smoke, water wake and ejection.
// Declared in the class headers; shared names in mech3d_internal.h.
#include "render_snap.h"
#include "terrain_runtime.h"
#include <atomic>
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "mech3d_internal.h"
#include "sim_overlap.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"
#include "anim_freeze.h"
#include "appearance_rng.h"

// Tags are fixed at startup; the per-mech-per-tick lookup builds a temp string, so cache it.
static bool animCadenceTagEnabled() { static const bool on = mc2_diag::tagEnabled("ANIM_CADENCE"); return on; }

long Mech3DAppearance::update (bool animate) 
{
	if (simPresentationPass) presentEffects();


	// DEGRADE-DON'T-CRASH: partial import with no base shape -> nothing to
	// animate; the many mechShape-> derefs below are unsafe. Return as if the
	// frame advanced successfully so callers (encyclopedia preview, mech bay)
	// keep running.
	// A battle mech's tick pass never looks at the draw shape (presentation may swap it): it asks the type.
	if (simulationOwned() && !simPresentationPass ? (!mechType || !mechType->mechShape[0]) : !mechShape)
		return TRUE;

	// A render frame with no sim tick re-poses the mech from unchanged sim state. The gesture state machine, the idle
	// timer, the dice and the weapon recycle clocks are SIM state and advance only on a tick (R1).
	if (!simPresentationPass)
	{
	// A battle mech reads the sim inputs the tick fed it (setSimInput); a preview mech (no owner) reads its own draw state.
	// These locals shadow the members for this block only, so the state machine below is unchanged.
	const bool simOwned = simulationOwned();
	const long status = simOwned ? simStatus_ : this->status;
	const glm::vec3 position = simOwned ? simPosition_ : this->position;
	const bool inDebugMoveMode = simOwned ? simDebugMove_ : this->inDebugMoveMode;
	const auto previousGesture = unitAnim().currentGestureId;
	const auto previousFrame = unitAnim().currentFrame;
	if (!unitAnim().nodeRecycle.empty())
	{
		for (long i=0;i<mechType->numWeaponNodes;i++)
		{
			if (unitAnim().nodeRecycle[i] > 0.0f)
			{
				unitAnim().nodeRecycle[i] -= frameLength;
				if (unitAnim().nodeRecycle[i] < 0.0f)
					unitAnim().nodeRecycle[i] = 0.0f;
			}
		}
	}

	unitAnim().oncePerFrame = false;
	bool setFirstFrame = false;

	if ((unitAnim().currentGestureId == GestureHitFront) ||
		(unitAnim().currentGestureId == GestureHitBack) || 
		(unitAnim().currentGestureId == GestureHitLeft) || 
		(unitAnim().currentGestureId == GestureHitRight))
	{
		if (unitAnim().atTransitionToNextGesture)
		{
			unitAnim().currentGestureId = unitAnim().hitGestureId;
			unitAnim().hitGestureId = -1;
			setFirstFrame = true;
			long firstFrame = mechType->gestures[unitAnim().currentGestureId].frameStart;

			if (firstFrame < 0)			//Start at end of animation.  Probably to reverse.  Like StandToPark
				firstFrame = mechType->getNumFrames(unitAnim().currentGestureId)-1;

			unitAnim().inReverse = false;
			if (mechType->getFrameRate(unitAnim().currentGestureId) < 0.0)
				unitAnim().inReverse = true;

			unitAnim().atTransitionToNextGesture = false;
			
			unitAnim().currentFrame = firstFrame;
		}
	}
	
	// Idle is a fresh one-shot, not a continuation of the previous gait.
	if (!unitAnim().inCombatMode && (unitAnim().currentGestureId == GestureStand))
	{
		unitAnim().idleTime += frameLength;
		if (unitAnim().idleTime > idleMAX)
		{
			unitAnim().currentGestureId = GestureIdle;
			unitAnim().currentFrame = 0.0f;
			unitAnim().inReverse = false;
			// Root cause of the idle/reverse bug: the flag left over from the previous gesture's
			// end made idle exit on its first tick. Clear it on ENTRY only.
			unitAnim().atTransitionToNextGesture = false;
			unitAnim().idleTime = 0.0f;
			setFirstFrame = true;
		}
	}
	else
	{
		unitAnim().idleTime = 0.0f;
	}
	
	if ((unitAnim().currentGestureId == GestureIdle) && unitAnim().atTransitionToNextGesture)
	{
		unitAnim().currentGestureId = 2;
		
		unitAnim().currentFrame = 0;
		// Leave atTransitionToNextGesture set (pre-W73 timing): a pending stand->walk fires at once.
	}
	
	if (checkStop() && !inDebugMoveMode && !unitAnim().jumpSetup && (getVelocityMagnitude() != 0.0f) && (unitAnim().currentGestureId != GestureJump))
	{
		unitAnim().currentGestureId = 2;		//Force us to the stand gesture
		unitAnim().oldStateGoal = 1;			//We always came FROM STAND
		unitAnim().currentStateGoal = -1;		//Not going anywhere
		unitAnim().transitionState = 0;		//No Transition necessary.  So be ready at beginning if we need one!
		unitAnim().currentFrame = 0;			//In the "how did this ever work file"  Duh, need to reset the frame!
		// Braking a reverse walk must not leave the following idle playing backwards.
		unitAnim().inReverse = false;
	}
	
	long newGestureId = -1;			//Assume we do not need to transition.
	
	if (unitAnim().currentStateGoal != -1)
	{
		// Find out how we get to Goal.  If result is
		// -1, we are already at goal.
		long arrayIdx = (unitAnim().oldStateGoal * GESTURE_OFFSET_SIZE) + (unitAnim().currentStateGoal *  MAX_TRANSITION_GESTURES) + unitAnim().transitionState;
		newGestureId = transitionArray[arrayIdx];

		if (unitAnim().atTransitionToNextGesture)	//This is only set when we are ready for the next gesture if these is one!
		{
			unitAnim().transitionState++;		//Ready to go to next state.  Increment the transition counter for next frame!

			long firstFrame = unitAnim().currentFrame;
			if (newGestureId == -1)		//We have reached our goal.
			{
				unitAnim().oldStateGoal = unitAnim().currentStateGoal;
				if (unitAnim().oldStateGoal == MECH_STATE_JUMPING)
					unitAnim().oldStateGoal = 1;		//We are always standing AFTER a jump or a fall!
				
				unitAnim().currentStateGoal = -1;
				unitAnim().transitionState = 0;
				unitAnim().atTransitionToNextGesture = false;
				
				if (mechType->getFrameRate(unitAnim().currentGestureId) < 0.0)
					unitAnim().inReverse = true;
			}
			else
			// reset all frames to zero for this gesture.
			// NOTE:  This may be a two way gesture.  Most will be
			// set to zero, but some may need to be set to last.
			{
				unitAnim().currentGestureId = newGestureId;
				setFirstFrame = true;
				firstFrame = mechType->gestures[unitAnim().currentGestureId].frameStart;
				if (unitAnim().currentGestureId == GestureJump)
					firstFrame = 0;			//We store information in the firstFrame of the jump.  They always start at 0!

				if (firstFrame < 0)			//Start at end of animation.  Probably to reverse.  Like StandToPark
					firstFrame = mechType->getNumFrames(unitAnim().currentGestureId)-1;

				unitAnim().inReverse = false;
				if (mechType->getFrameRate(unitAnim().currentGestureId) < 0.0)
					unitAnim().inReverse = true;

				unitAnim().atTransitionToNextGesture = false;
			}

			unitAnim().currentFrame = firstFrame;
		}
	}
		
	if ((status == OBJECT_STATUS_DESTROYED) || (status == OBJECT_STATUS_DISABLED))
	{
		// This will bypass animation if we go the old way.
		// Just set stateGoal to 7 or 8 if its not.
		if (unitAnim().currentStateGoal < 7)
		{
			if (simulationOwned() ? mc2_anim_rng::rollDice(50) : mc2_appearance_rng::rollDice(50))
			{
				setGestureGoal(7);
			}
			else
			{
				setGestureGoal(8);
			}
		}
	}

	unitAnim().mechFrameRate = mechType->getFrameRate(unitAnim().currentGestureId);

	if (unitAnim().mechFrameRate < 0.0)
		unitAnim().mechFrameRate = -unitAnim().mechFrameRate;

	if ((unitAnim().currentGestureId == GestureJump) && !unitAnim().inJump)
	{
		unitAnim().inJump = true;
		unitAnim().jumpSetup = false;
		
		// Distance is straight line distance.  This is only
		// the case if the mech is FACING the correct way.  If
		// not, we must increase distance based on facing.
		float takeoffFrame = mechType->getJumpTakeoffFrame();
		float landingFrame = mechType->getJumpLandFrame();

		glm::vec3 ownerPosition = position;
		glm::vec3 targetPosition = unitAnim().jumpDestination;
		unitAnim().jumpVelocity = targetPosition - ownerPosition;
					
		float velMag = glm::length(unitAnim().jumpVelocity) / ((landingFrame - takeoffFrame) / 30.0f);
		unitAnim().jumpVelocity = glm::normalize(unitAnim().jumpVelocity);
		unitAnim().jumpVelocity *= velMag;
		
		// Jump Velocity is now a velocity vector which 
		// will move the mech WITHOUT turning to the landing point.
	}
	
	if (unitAnim().currentGestureId == GestureJump && unitAnim().inJump)
	{
		if (unitAnim().currentFrame >= mechType->getJumpTakeoffFrame())
		{
			unitAnim().jumpAirborne = true;
		}
		
		if ((unitAnim().currentFrame >= mechType->getJumpLandFrame()) && unitAnim().jumpAirborne)
		{
			unitAnim().jumpAirborne = false;
			unitAnim().jumpVelocity = glm::vec3(0.0f);
			// The jet effect stops in presentation (updateGeometry) once it sees the landing; the tick owns no effect objects.
		}
	}
 	
	unitAnim().fallen = FALSE;
	if ((unitAnim().currentGestureId == GestureFallenForward) || (unitAnim().currentGestureId == GestureFallenBackward))
	{
		unitAnim().fallen = TRUE;
		if (unitAnim().currentStateGoal != -1)
			unitAnim().atTransitionToNextGesture = TRUE;
			
		//Try playing the helicopter dust cloud for grins to see how it looks!
		// Only play once!!!  Until they get back up.
		if (!fallDust)
		{
			playEjection();
			fallDust = true;
		}
	}
	else if (unitAnim().currentGestureId == GesturePark)
	{
		fallDust = false;
		if (unitAnim().currentStateGoal != -1)
			unitAnim().atTransitionToNextGesture = TRUE;
	}
	else	//Normal Frame Increment
	{
		fallDust = false;
		if (animate)
		{
			float frameInc = 0.0f;
			if (singleStepMode && nextStep)
			{
				frameInc = 1.0f;
			}
			else if (singleStepMode && prevStep)
			{
				frameInc = -1.0f;
			}
			else if (!singleStepMode)
			{
				// Make sure animation runs no faster than mechFrameRate fps.
				frameInc = unitAnim().mechFrameRate * frameLength;
			}
			
			// Always ASSUME we cannot transition yet.
			// Saves myriad compares below.
			unitAnim().atTransitionToNextGesture = FALSE;
					
			if ((frameInc != 0.0f) || (singleStepMode))
			{
				if (!setFirstFrame)		//DO NOT ANIMATE ON FIRST FRAME!  Wait a bit!
				{
					// Gameplay animation advances once per authoritative tick.
					const bool _alreadyAdvanced = (unitAnim().lastAnimAdvanceFrame == static_cast<uint32_t>(turn));
					if (!_alreadyAdvanced)
					{
						if (unitAnim().inReverse)
							unitAnim().currentFrame -= frameInc;
						else
							unitAnim().currentFrame += frameInc;
						unitAnim().lastAnimAdvanceFrame = static_cast<uint32_t>(turn);
					}
				}

				if (frameInc != 0.0f)
				{
					// ANIM-CADENCE-GUARD-1 (non-fatal, recon — FRAME-CURRENTNESS-GUARDS-1).
					// Make the "exactly one gait advance per mech appearance per frame"
					// invariant observable. Recon (ANIM-MECH-DOUBLE-STEP) proved a single
					// advance site today, but guard it so a future second advance path
					// (worker prepass / touch-split) can't silently reintroduce the
					// double-step. Per-appearance last-advance frame stamp via static map
					// (no class-layout change → no full relink); a repeat advance in the
					// same g_mc2FrameCounter == double advance. Map touched only when
					// MC2_ANIM_CADENCE_GUARD set; events under MC2_DIAG_TAGS=ANIM_CADENCE.
					{
						static const bool s_animCadGuard = mc2::gate::get<"MC2_ANIM_CADENCE_GUARD">();
						if (s_animCadGuard)
						{
							static std::unordered_map<void*, uint32_t> s_lastAdvanceFrame;
							static uint64_t s_ac_advances = 0, s_ac_doubles = 0;
							++s_ac_advances;
							const uint32_t _ac_frame = mc2::overlap::simulationFrame(g_mc2FrameCounter);
							auto _ac_it = s_lastAdvanceFrame.find(static_cast<void*>(this));
							const bool _ac_double = (_ac_it != s_lastAdvanceFrame.end() && _ac_it->second == _ac_frame);
							s_lastAdvanceFrame[static_cast<void*>(this)] = _ac_frame;
							if (_ac_double)
							{
								++s_ac_doubles;
								if (animCadenceTagEnabled())
								{
									char _ac_buf[256];
									mc2::fmt::to_buf(_ac_buf, "{{\"event\":\"advanced_twice_same_frame\",\"ptr\":\"{:016X}\",\"gesture\":{:d},\"frame\":{:d},\"frameInc\":{:.4f},\"doubles\":{:d},\"advances\":{:d}}}", mc2::fmt::as_ptr(static_cast<void*>(this)), mc2::fmt::as_i32(static_cast<int>(unitAnim().currentGestureId)), mc2::fmt::as_u32(_ac_frame), frameInc, mc2::fmt::as_u64(static_cast<unsigned long long>(s_ac_doubles)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_ac_advances)));
									mc2_diag::writeEvent("ANIM_CADENCE", 1, static_cast<uint64_t>(_ac_frame), _ac_buf);
								}
							}
						}
					}
				}

				if (unitAnim().currentFrame >= mechType->getNumFrames(unitAnim().currentGestureId))
				{
					if ((unitAnim().currentStateGoal == -1) && (unitAnim().hitGestureId == -1) && (unitAnim().currentGestureId != GestureIdle))		//Are we just Looping or transitioning?
					{
						unitAnim().currentFrame -= mechType->getNumFrames(unitAnim().currentGestureId);
					}
					else
					{
						// We are at the end and ready to transition
						// Force us back to the previous frame so draw
						// doesn't go off into lala land.
						unitAnim().atTransitionToNextGesture = TRUE;
						unitAnim().currentFrame = mechType->getNumFrames(unitAnim().currentGestureId) - 1.0f;
						
						if (unitAnim().currentGestureId == GestureJump)
						{
							unitAnim().inJump = false;
							unitAnim().jumpSetup = false;
							unitAnim().oldStateGoal = 1;		//We always started a Jump standing!
						}
					}
				}
	
				if (unitAnim().currentFrame < 0)
				{
					if ((unitAnim().currentStateGoal == -1) && (unitAnim().hitGestureId == -1))
					{
						unitAnim().currentFrame += mechType->getNumFrames(unitAnim().currentGestureId);
					}
					else
					{
						// We are at the end and ready to transition
						// Force us back to the previous frame so draw
						// doesn't go off into lala land.
						unitAnim().atTransitionToNextGesture = TRUE;
						unitAnim().currentFrame = 0.0f;
					}
				}
			}
			else		//This is a  single frame gesture.  We are ALWAYS ready to transition in this case!
			{
				if (unitAnim().currentStateGoal != -1)
					unitAnim().atTransitionToNextGesture = TRUE;
			}
		}
	}

	if (unitAnim().currentFrame < 0.0f)
		unitAnim().currentFrame = 0.0f;
//		STOP(("CurrentFrame of animation can never be less then zero.  Frame %f,  NumFrames %f, GestureId %d",currentFrame,mechType->getNumFrames(currentGestureId),currentGestureId));
		
	if ((unitAnim().currentFrame > 0.0f) && (unitAnim().currentFrame >= mechType->getNumFrames(unitAnim().currentGestureId)))
		unitAnim().currentFrame = 0.0f;
	if (animCadenceTagEnabled() &&
		(previousGesture != unitAnim().currentGestureId || unitAnim().currentGestureId == GestureIdle || turn % 30 == 0))
	{
		mc2::fmt::print_out("[ANIM_STATE] tick={} actor={} type={} gesture={}->{} frame={:.3f}->{:.3f} frames={:.0f} rate={:.1f} idle={:.3f} ready={} reverse={} goal={} oldGoal={} combat={}\n",
			turn, actorHandle_, mechType->name, previousGesture, unitAnim().currentGestureId,
			previousFrame, unitAnim().currentFrame, static_cast<float>(mechType->getNumFrames(unitAnim().currentGestureId)),
			unitAnim().mechFrameRate, unitAnim().idleTime, unitAnim().atTransitionToNextGesture,
			unitAnim().inReverse, unitAnim().currentStateGoal, unitAnim().oldStateGoal, unitAnim().inCombatMode);
	}
	} // end !simPresentationPass
		
	if (!simPresentationPass && (simulationOwned() || mc2::overlap::onSimWorker())) { updateSimPose(); return TRUE; }
	mc2::overlap::assertMainThread("Mech3DAppearance::update presentation");

	// LIGHT-CULL-RADIUS-1 freshness (batch-6 item 5 ext a/c): the light
	// owner-intent writers live in updateGeometry, which is frustum-culled
	// below — an OFF-SCREEN mech never refreshed its light state, so (a) a
	// mech shutting down off-screen kept emitting, and (c) FoW/status flips
	// while culled left ghost pools with no renderable unit. Recompute the
	// enable predicate + a coarse light position EVERY frame here (cheap:
	// a few flag writes; updateGeometry overwrites with node-accurate
	// positions whenever it runs right after).
	if (pointLight || !spotlightLights_.empty())
	{
		// GHOST-LIGHT-VISIBILITY-1: keep the off-screen freshness refresh in
		// lockstep with the updateGeometry predicate — a sensor-blip-only
		// enemy must not keep ownerEnabled latched while culled.
		// VISIBILITY-SEMANTICS-1: same job-(c) predicate as updateGeometry,
		// so a culled mover can never latch a stale emission state.
		const mc2_vis::MoverVis vs_ = buildVisState();
		const bool ownerOn =
			(eye && eye->isNight && !InEditor && mc2_vis::emitsLight(vs_));
		mc2_vis::trace("mech", static_cast<const void*>(this), vs_);
		if (pointLight)
			// The legacy `sensorLevel > 4` here excluded every home mech whose
			// BattleMech::render() had just reset sensorLevel to 0 (mech.cpp)
			// — the "no mech searchlights at all" regression. Job (c) again.
			pointLight->ownerEnabled =
				(!InEditor && mc2_vis::emitsLight(vs_)) ? 1 : 0;
		if (ownerOn && !spotlightLights_.empty())
		{
			// Coarse per-frame position: mover position + forward bias, so
			// an off-screen truck's pool keeps moving with it instead of
			// freezing at the last on-screen node position.
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

	// Under the GPU static-prop killswitch always run updateGeometry so
	// mechShape->TransformMultiShape runs every frame, even when the
	// broken cull (inView) thinks the mech is off-screen. Without this,
	// TG_Shape::Render silently returns on stale listOfVertices and the
	// mech geometry disappears (health bar still drawn via screenPos).
	// C3: route inView input to GPU-lagged visibility when killswitch is enabled.
	// PRESERVE cascade gate structure: turn<3 and GestureJump conditions are unchanged.
	{
		const bool gpuVis = s_gpuCullLifecycle
			? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_))
			: inView;
		if ((turn < 3) || gpuVis || (unitAnim().currentGestureId == GestureJump))
			updateGeometry();

		// Slice B1: refresh per-actor LightsData UBO slot index for the
		// GPU mech batcher path. Mirrors bdactor.cpp pattern. No-op
		// when killswitch is off; cheap (one hash lookup + dedup-cache
		// slot write) when on. Source: msl.cpp.
		if (mechShape) {
			mechShape->CacheGpuLightData();
		}
	}

	if (baseRootNodeHeight == -99999.9f)
	{
		if (rootNodeIndex == -1)
			rootNodeIndex = mechShape->GetNodeNameId("joint_root");

		bool oldInView = inView;
		setVisibilityGatesFromLegacy(true);
	 	baseRootNodeHeight = (getNodeIdPosition(rootNodeIndex).z - position.z);
		setVisibilityGatesFromLegacy(oldInView);
	}

	// D-gpu-pose-instrument: Arms stage — leftArm/rightArm dynamics +
	// transform when blown off. Per user: arms attached by default;
	// these `*ArmOff && *Arm && *ArmRecalc()` paths only fire for
	// blown-arm mechs. Tracy zone fires unconditionally so we can
	// confirm typical mechs have ~0 mass here.
	{ ZoneScopedN("Mech3D.UpdateGeometry.Arms");
	// If arms are off, process their geometry here!
	// MUST do every frame.  We don't know where the arms are!!!
	glm::vec3 xlatPosition;
	glm::quat qRotation;
 	if (unitAnim().leftArmOff && leftArm && leftArmRecalc())
	{
		float speed = glm::length(dVel[1]);
		if (speed)
		{
			glm::vec3 velDiff = dAcc[1];
			velDiff *= frameLength;
			dVel[1] = dVel[1] + velDiff;
			speed = glm::length(dVel[1]); 
			if (speed < Stuff::SMALL)
			{
				dVel[1].x = dVel[1].y = dVel[1].z = 0.0;
			}
				
			glm::vec3 posDiff = dVel[1];
			posDiff *= frameLength;
			leftArmPos = leftArmPos + posDiff;
			float elev = land->getTerrainElevation(leftArmPos); 
			if (leftArmPos.z < elev)
			{
				leftArmPos.z = elev;
				dRacc[1] = glm::vec3(0.0f);
				dRVel[1] = glm::vec3(0.0f);
				dTime[1] -= frameLength;
				if (dTime[1] < 0.0)
					dVel[1].x = dVel[1].y = dVel[1].z = 0.0;
			}
			
			glm::vec3 rvDiff = dRacc[1];
			rvDiff *= frameLength;
			dRVel[1] = dRVel[1] + rvDiff;
				
			glm::vec3 rotDiff = dRVel[1];
			rotDiff *= frameLength;
			dRot[1] = dRot[1] + rotDiff;
		}

		mc2ToRender(xlatPosition, leftArmPos);
		
		qRotation = mc2::math::quatFromEuler(mc2::math::Euler(dRot[1].x * DEGREES_TO_RADS, dRot[1].y * DEGREES_TO_RADS, dRot[1].z * DEGREES_TO_RADS));

        // sebi: update texture handle, it will not be updated it updateGeometry 
        // is not caaled which is not correct and leads to 
        // "Flags do not match either set of vertex Data" (see render_submissions.h)
		leftArm->SetTextureHandle(0,localTextureHandle);

		leftArm->SetFogRGB(0xffffffff);
		leftArm->SetLightList(eye->getWorldLights(),eye->getNumLights());
		leftArm->TransformMultiShape_HierarchyOnly(&xlatPosition,&qRotation);
		leftArm->ReserveCpuDraw();
			
		if (leftArmSmoke)
		{
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			glm::mat4	localResult;
					
			mc2ToRender(leftShoulderPos, leftArmPos);
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, leftShoulderPos);
					
			glm::quat effectRot;
			effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
			localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
			localResult = localToWorld * shapeOrigin;
			
			mc2::math::Obb boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
			bool result = leftArmSmoke->Execute(&info);
			if (!result)
			{
				leftArmSmoke->Kill();
				leftArmSmoke.reset();
			}
		}
	}
	
	if (unitAnim().rightArmOff && rightArm && rightArmRecalc())
	{
		float speed = glm::length(dVel[0]);
		if (speed)
		{
			glm::vec3 velDiff = dAcc[0];
			velDiff *= frameLength;
			dVel[0] = dVel[0] + velDiff;
			speed = glm::length(dVel[0]); 
			if (speed < Stuff::SMALL)
			{
				dVel[0].x = dVel[0].y = dVel[0].z = 0.0;
			}
				
			glm::vec3 posDiff = dVel[0];
			posDiff *= frameLength;
			rightArmPos = rightArmPos + posDiff;
			float elev = land->getTerrainElevation(rightArmPos); 
			if (rightArmPos.z < elev)
			{
				rightArmPos.z = elev;
				dRacc[0] = glm::vec3(0.0f);
				dRVel[0] = glm::vec3(0.0f);
				dTime[0] -= frameLength;
				if (dTime[0] < 0.0)
					dVel[0].x = dVel[0].y = dVel[0].z = 0.0;
			}
			
			glm::vec3 rvDiff = dRacc[0];
			rvDiff *= frameLength;
			dRVel[0] = dRVel[0] + rvDiff;
				
			glm::vec3 rotDiff = dRVel[0];
			rotDiff *= frameLength;
			dRot[0] = dRot[0] + rotDiff;
		}

		mc2ToRender(xlatPosition, rightArmPos);
		
		qRotation = mc2::math::quatFromEuler(mc2::math::Euler(dRot[0].x * DEGREES_TO_RADS, dRot[0].y * DEGREES_TO_RADS, dRot[0].z * DEGREES_TO_RADS));

        // sebi: update texture handle, it will not be updated it updateGeometry 
        // is not caaled which is not correct and leads to 
        // "Flags do not match either set of vertex Data" (see render_submissions.h)
		rightArm->SetTextureHandle(0,localTextureHandle);

		rightArm->SetFogRGB(0xffffffff);
		rightArm->SetLightList(eye->getWorldLights(),eye->getNumLights());
		rightArm->TransformMultiShape_HierarchyOnly(&xlatPosition,&qRotation);
		rightArm->ReserveCpuDraw();
		
		if (rightArmSmoke)
		{
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			glm::mat4	localResult;
					
			mc2ToRender(rightShoulderPos, rightArmPos);
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, rightShoulderPos);
					
			glm::quat effectRot;
			effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,rotation * DEGREES_TO_RADS,0.0f));
			localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
			localResult = localToWorld * shapeOrigin;
			
			mc2::math::Obb boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
			bool result = rightArmSmoke->Execute(&info);
			if (!result)
			{
				rightArmSmoke->Kill();
				rightArmSmoke.reset();
			}
		}
	}
	} // end Arms zone

 	nextStep = prevStep = false;
	
	return TRUE;
}

void Mech3DAppearance::blowLeftArm (void)
{
	if (!simPresentationPass) { unitAnim().leftArmOff = true; return; }

	if (unitAnim().leftArmOff)
		return;
		
	// For now, the shoulder just gets a location which does not change.
	// Can easily move to a node if a node becomes available.
	if (strcmp(weaponEffects->GetEffectName(SHOULDER_POP_ID),"NONE") != 0)
	{
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(SHOULDER_POP_ID));
		
		if (gosEffectSpec)
		{
			leftShoulderBoom = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(leftShoulderBoom != nullptr);
			
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		}
	}
		
	if (strcmp(weaponEffects->GetEffectName(ARM_FLYING_ID),"NONE") != 0)
	{
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(ARM_FLYING_ID));
		
		if (gosEffectSpec)
		{
			leftArmSmoke = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(leftArmSmoke != nullptr);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		}
	}
	
	if (leftArmNodeIndex == -1)
		leftArmNodeIndex = mechShape->GetNodeNameId("joint_luarm");

	glm::vec3 leftNodePos = getNodeIdPosition(leftArmNodeIndex);
	leftArmPos = leftNodePos;

 	if (leftShoulderBoom)
	{
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
				
		mc2ToRender(leftShoulderPos, leftNodePos);
		mc2snap::liftRenderY(leftShoulderPos, TerrainRuntime::visualHeightDelta(position));   // UNIT-VISUAL-SNAP-2: effect anchor follows the drawn mesh
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, leftShoulderPos);
				
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
		
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
		leftShoulderBoom->Start(&info);
	}

 	if (leftArmSmoke)
	{
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
				
		mc2ToRender(leftShoulderPos, leftArmPos);
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, leftShoulderPos);
				
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
		
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
		leftArmSmoke->Start(&info);
	}
	
	unitAnim().leftArmOff = true;
	
	long xlatBase = 12.0f + 50.0 / 2.0f;
	long upBase = 25.0f + 100.0;
	long rotBase = 25.0f + 100.0;
	dVel[1].x = mc2_appearance_rng::randomNumber(xlatBase * 2.0) - xlatBase;
	dVel[1].y = mc2_appearance_rng::randomNumber(xlatBase * 2.0) - xlatBase;
	dVel[1].z = mc2_appearance_rng::randomNumber(upBase) + upBase;

	dRVel[1].x = mc2_appearance_rng::randomNumber(rotBase * 2.0) - rotBase;
	dRVel[1].y = mc2_appearance_rng::randomNumber(rotBase * 2.0) - rotBase;
	dRVel[1].z = mc2_appearance_rng::randomNumber(rotBase * 2.0) - rotBase;

	dRot[1] = glm::vec3(0.0f);

	dAcc[1].x = dVel[1].x * 0.1f;
	dAcc[1].y = dVel[1].y * 0.1f;
	dAcc[1].z = dVel[1].z * 0.5f;
	dAcc[1] = -dAcc[1];

	dRacc[1].x = dRVel[1].x * 0.1f;
	dRacc[1].y = dRVel[1].y * 0.1f;
	dRacc[1].z = dRVel[1].z * 0.1f;
	dRacc[1] = -dRacc[1];

	dTime[1] = 4.0f;

	mechShape->StopUsing("joint_luarm");
}

void Mech3DAppearance::blowRightArm (void)
{
	if (!simPresentationPass) { unitAnim().rightArmOff = true; return; }

	if (unitAnim().rightArmOff)
		return;
		
	// For now, the shoulder just gets a location which does not change.
	// Can easily move to a node if a node becomes available.
	if (strcmp(weaponEffects->GetEffectName(SHOULDER_POP_ID),"NONE") != 0)
	{
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(SHOULDER_POP_ID));
		
		if (gosEffectSpec)
		{
			rightShoulderBoom = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(rightShoulderBoom != nullptr);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		}
	}
		
	if (strcmp(weaponEffects->GetEffectName(ARM_FLYING_ID),"NONE") != 0)
	{
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(ARM_FLYING_ID));
		
		if (gosEffectSpec)
		{
			rightArmSmoke = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(rightArmSmoke != nullptr);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		}
	}
	
	if (rightArmNodeIndex == -1)
		rightArmNodeIndex = mechShape->GetNodeNameId("joint_ruarm");

	glm::vec3 rightNodePos = getNodeIdPosition(rightArmNodeIndex);
	rightArmPos = rightNodePos;

 	if (rightShoulderBoom)
	{
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
				
		mc2ToRender(rightShoulderPos, rightNodePos);
		mc2snap::liftRenderY(rightShoulderPos, TerrainRuntime::visualHeightDelta(position));   // UNIT-VISUAL-SNAP-2: effect anchor follows the drawn mesh
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, rightShoulderPos);
				
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
		
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
		rightShoulderBoom->Start(&info);
	}

 	if (rightArmSmoke)
	{
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
				
		mc2ToRender(rightShoulderPos, rightArmPos);
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, rightShoulderPos);
				
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
		
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
		rightArmSmoke->Start(&info);
	}

	unitAnim().rightArmOff = true;

	long xlatBase = 12.0f + 50.0 / 2.0f;
	long upBase = 25.0f + 100.0;
	long rotBase = 25.0f + 100.0;
	dVel[0].x = mc2_appearance_rng::randomNumber(xlatBase * 2.0) - xlatBase;
	dVel[0].y = mc2_appearance_rng::randomNumber(xlatBase * 2.0) - xlatBase;
	dVel[0].z = mc2_appearance_rng::randomNumber(upBase) + upBase;

	dRVel[0].x = mc2_appearance_rng::randomNumber(rotBase * 2.0) - rotBase;
	dRVel[0].y = mc2_appearance_rng::randomNumber(rotBase * 2.0) - rotBase;
	dRVel[0].z = mc2_appearance_rng::randomNumber(rotBase * 2.0) - rotBase;

	dRot[0] = glm::vec3(0.0f);

	dAcc[0].x = dVel[0].x * 0.1f;
	dAcc[0].y = dVel[0].y * 0.1f;
	dAcc[0].z = dVel[0].z * 0.5f;
	dAcc[0] = -dAcc[0];

	dRacc[0].x = dRVel[0].x * 0.1f;
	dRacc[0].y = dRVel[0].y * 0.1f;
	dRacc[0].z = dRVel[0].z * 0.1f;
	dRacc[0] = -dRacc[0];

	dTime[0] = 4.0f;
	
	mechShape->StopUsing("joint_ruarm");
}
		
void Mech3DAppearance::startSmoking (long smokeLvl)
{
	if (!simPresentationPass) { unitAnim().smokeLevel = smokeLvl; return; }

	
	
	if (!useNonWeaponEffects)
		return;

	if ((smokeLvl > 0) && !criticalSmoke)
	{
   		if (strcmp(weaponEffects->GetEffectName(CRITICAL_SMOKE_ID),"NONE") != 0)
   		{
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(CRITICAL_SMOKE_ID));
			
			if (gosEffectSpec)
			{
				criticalSmoke = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(criticalSmoke != nullptr);
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if ((smokeLvl == 0) && !smokeEffect)
	{
   		if (strcmp(weaponEffects->GetEffectName(MECH_SMOKE_ID),"NONE") != 0)
   		{
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(MECH_SMOKE_ID));
			
			if (gosEffectSpec)
			{
				smokeEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(smokeEffect != nullptr);
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if (smokeLvl != -1)
	{
		if ((isSmoking == -1) || ((isSmoking != smokeLvl) && (smokeLvl != -1)))
		{
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;

			glm::vec3 smokeNodePos = getNodePosition(0);	//Always SMOKE if it exists!!
			glm::vec3 smokePos;
			mc2ToRender(smokePos, smokeNodePos);
			mc2snap::liftRenderY(smokePos, TerrainRuntime::visualHeightDelta(position));   // UNIT-VISUAL-SNAP-2: effect anchor follows the drawn mesh

			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, smokePos);

			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);

			if (smokeLvl > 0)
				criticalSmoke->Start(&info);
			else
				smokeEffect->Start(&info);
		}
	}
	else
	{
		if (smokeEffect)
			smokeEffect->Kill();
			
		if (criticalSmoke)
			criticalSmoke->Kill();
	}
	
	isSmoking = smokeLvl;
}

void Mech3DAppearance::startWaterWake (void)
{
	if (!simPresentationPass) { unitAnim().wakeRequested = true; return; }

	
	if (isHelicopter || unitAnim().currentGestureId == 20)
		return;

	if (!useNonWeaponEffects)
		return;

	if (useNonWeaponEffects && !waterWake)
	{
   		if (strcmp(weaponEffects->GetEffectName(MECH_WATER_WAKE),"NONE") != 0)
   		{
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(MECH_WATER_WAKE));
			
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
		if (!unitAnim().inReverse)
			localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		else
			localToWorld = mc2::math::multiply(gosFX::Effect_Into_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);

		waterWake->Start(&info);
		isWaking = true;
	}
}

void Mech3DAppearance::stopWaterWake (void)
{
	if (!simPresentationPass) { unitAnim().wakeRequested = false; return; }

	if (waterWake && isWaking)		//Stop the effect if we are running it!!
		waterWake->Kill();
	
	isWaking = false;
}

void Mech3DAppearance::playEjection (void)
{
	if (!simPresentationPass) { ++unitAnim().ejectionSerial; return; }

	if (InEditor)
		return;

	
	if (useNonWeaponEffects && !helicopterDustCloud)
	{
   		if (strcmp(weaponEffects->GetEffectName(HELICOPTER_DUST_CLOUD),"NONE") != 0)
   		{
			unsigned flags = gosFX::Effect::ExecuteFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(HELICOPTER_DUST_CLOUD));
			
			if (gosEffectSpec)
			{
				helicopterDustCloud = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(helicopterDustCloud != nullptr);
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if (!isDusting && helicopterDustCloud)		//Start the effect if we are not running it yet!!
	{
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
				
		glm::vec3 wakePos;
		mc2ToRender(wakePos, position);	//dustCloud is just centered on position.
		
 		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, wakePos);
				
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);

		helicopterDustCloud->Start(&info);
		isDusting = true;
	}
}
