// Mech3DAppearance: Mech3DAppearance::update, arm destruction, smoke, water wake and ejection.
// Declared in the class headers; shared names in mech3d_internal.h.
#include "mech3d_internal.h"
#include "../RenderCore/axis_convert.h"

long Mech3DAppearance::update (bool animate) 
{

#ifdef _DEBUG
	if (!logFile)
	{
		logFile = new File;
		long result = logFile->create("gesture.log");
		if (result != NO_ERR)
		{
			delete logFile;
			logFile = NULL;
		}
	}
#endif

	// DEGRADE-DON'T-CRASH: partial import with no base shape -> nothing to
	// animate; the many mechShape-> derefs below are unsafe. Return as if the
	// frame advanced successfully so callers (encyclopedia preview, mech bay)
	// keep running.
	if (!mechShape)
		return TRUE;

	if (nodeRecycle)
	{
		for (long i=0;i<mechType->numWeaponNodes;i++)
		{
			if (nodeRecycle[i] > 0.0f)
			{
				nodeRecycle[i] -= frameLength;
				if (nodeRecycle[i] < 0.0f)
					nodeRecycle[i] = 0.0f;
			}
		}
	}

	oncePerFrame = false;
	bool setFirstFrame = false;

	if ((currentGestureId == GestureHitFront) ||
		(currentGestureId == GestureHitBack) || 
		(currentGestureId == GestureHitLeft) || 
		(currentGestureId == GestureHitRight))
	{
		if (atTransitionToNextGesture)
		{
			currentGestureId = hitGestureId;
			hitGestureId = -1;
			setFirstFrame = true;
			long firstFrame = mechType->gestures[currentGestureId].frameStart;

			if (firstFrame < 0)			//Start at end of animation.  Probably to reverse.  Like StandToPark
				firstFrame = mechType->getNumFrames(currentGestureId)-1;

			inReverse = false;
			if (mechType->getFrameRate(currentGestureId) < 0.0)
				inReverse = true;

			atTransitionToNextGesture = false;
			
			currentFrame = firstFrame;
		}
	}
	
	//If mech is in 2 and does NOT have a target.
	//Fix please
	// FREEZE-MOVERS-1: idleTime is a wall-clock accumulator that FLIPS THE
	// GESTURE (2 -> 13) once it passes idleMAX. Left running under a freeze it
	// silently changes which animation a "pinned" mech is posed in, so two
	// arms of the same A/B can differ by a whole gesture. Frozen only when
	// freeze_movers armed it; stock and capture paths are untouched.
	if (!inCombatMode && (currentGestureId == 2) && (mc2FreezeMoversActive() == 0))
	{
		idleTime += frameLength;
		if (idleTime > idleMAX)
		{
			currentGestureId = 13;
		}
	}
	else
	{
		idleTime = 0.0f;
	}
	
	if ((currentGestureId == 13) && atTransitionToNextGesture)
	{
		currentGestureId = 2;
		
		currentFrame = 0;
	}
	
	if (checkStop() && !inDebugMoveMode && !jumpSetup && (getVelocityMagnitude() != 0.0f) && (currentGestureId != GestureJump))
	{
		currentGestureId = 2;		//Force us to the stand gesture
		oldStateGoal = 1;			//We always came FROM STAND
		currentStateGoal = -1;		//Not going anywhere
		transitionState = 0;		//No Transition necessary.  So be ready at beginning if we need one!
		currentFrame = 0;			//In the "how did this ever work file"  Duh, need to reset the frame!
	}
	
	long newGestureId = -1;			//Assume we do not need to transition.
	
	if (currentStateGoal != -1)
	{
		// Find out how we get to Goal.  If result is
		// -1, we are already at goal.
		long arrayIdx = (oldStateGoal * GESTURE_OFFSET_SIZE) + (currentStateGoal *  MAX_TRANSITION_GESTURES) + transitionState;
		newGestureId = transitionArray[arrayIdx];

		if (atTransitionToNextGesture)	//This is only set when we are ready for the next gesture if these is one!
		{
			transitionState++;		//Ready to go to next state.  Increment the transition counter for next frame!

			long firstFrame = currentFrame;
			if (newGestureId == -1)		//We have reached our goal.
			{
				oldStateGoal = currentStateGoal;
				if (oldStateGoal == MECH_STATE_JUMPING)
					oldStateGoal = 1;		//We are always standing AFTER a jump or a fall!
				
				currentStateGoal = -1;
				transitionState = 0;
				atTransitionToNextGesture = false;
				
				if (mechType->getFrameRate(currentGestureId) < 0.0)
					inReverse = true;
			}
			else
			// reset all frames to zero for this gesture.
			// NOTE:  This may be a two way gesture.  Most will be
			// set to zero, but some may need to be set to last.
			{
				currentGestureId = newGestureId;
				setFirstFrame = true;
				firstFrame = mechType->gestures[currentGestureId].frameStart;
				if (currentGestureId == GestureJump)
					firstFrame = 0;			//We store information in the firstFrame of the jump.  They always start at 0!

				if (firstFrame < 0)			//Start at end of animation.  Probably to reverse.  Like StandToPark
					firstFrame = mechType->getNumFrames(currentGestureId)-1;

				inReverse = false;
				if (mechType->getFrameRate(currentGestureId) < 0.0)
					inReverse = true;

				atTransitionToNextGesture = false;
			}

			currentFrame = firstFrame;
		}
	}
		
	if ((status == OBJECT_STATUS_DESTROYED) || (status == OBJECT_STATUS_DISABLED))
	{
		// This will bypass animation if we go the old way.
		// Just set stateGoal to 7 or 8 if its not.
		if (currentStateGoal < 7)
		{
			if (RollDice(50))
			{
				setGestureGoal(7);
			}
			else
			{
				setGestureGoal(8);
			}
		}
	}

	mechFrameRate = mechType->getFrameRate(currentGestureId);

	if (mechFrameRate < 0.0)
		mechFrameRate = -mechFrameRate;

	if ((currentGestureId == GestureJump) && !inJump)
	{
		inJump = true;
		jumpSetup = false;
		
		// Distance is straight line distance.  This is only
		// the case if the mech is FACING the correct way.  If
		// not, we must increase distance based on facing.
		float takeoffFrame = mechType->getJumpTakeoffFrame();
		float landingFrame = mechType->getJumpLandFrame();

		Stuff::Vector3D ownerPosition = position;
		Stuff::Vector3D targetPosition = jumpDestination;
		jumpVelocity.Subtract(targetPosition,ownerPosition);
					
		float velMag = jumpVelocity.GetLength() / ((landingFrame - takeoffFrame) / 30.0f);
		jumpVelocity.Normalize(jumpVelocity);
		jumpVelocity *= velMag;
		
		// Jump Velocity is now a velocity vector which 
		// will move the mech WITHOUT turning to the landing point.
	}
	
	if (currentGestureId == GestureJump && inJump)
	{
		if (currentFrame >= mechType->getJumpTakeoffFrame())
		{
			jumpAirborne = true;
		}
		
		if ((currentFrame >= mechType->getJumpLandFrame()) && jumpAirborne)
		{
			jumpAirborne = false;
			jumpVelocity.Zero();
			
			if (jumpJetEffect)
			{
				jumpJetEffect->Stop();	//Done drawing it.
			}
		}
	}
 	
	fallen = FALSE;
	if ((currentGestureId == GestureFallenForward) || (currentGestureId == GestureFallenBackward))
	{
		fallen = TRUE;
		if (currentStateGoal != -1)
			atTransitionToNextGesture = TRUE;
			
		//Try playing the helicopter dust cloud for grins to see how it looks!
		// Only play once!!!  Until they get back up.
		if (!fallDust)
		{
			playEjection();
			fallDust = true;
		}
	}
	else if (currentGestureId == GesturePark)
	{
		fallDust = false;
		if (currentStateGoal != -1)
			atTransitionToNextGesture = TRUE;
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
				frameInc = mechFrameRate * frameLength;
			}
			
			// Always ASSUME we cannot transition yet.
			// Saves myriad compares below.
			atTransitionToNextGesture = FALSE;
					
			if ((frameInc != 0.0f) || (singleStepMode))
			{
				if (!setFirstFrame)		//DO NOT ANIMATE ON FIRST FRAME!  Wait a bit!
				{
					// ANIM-CADENCE-FIX (gate MC2_ANIM_CADENCE_FIX): advance the gait at MOST
					// once per render frame. Mover::getLOSPosition() (mover.cpp) calls a
					// second appearance->update() in combat to refresh weapon-node geometry,
					// which otherwise ticks the animation a 2nd time the same frame => visible
					// double-step (proven: 19390 advanced_twice events, gestures 2/4/7 == the
					// getLOSPosition gesture filter). If we already advanced this
					// g_mc2FrameCounter, skip the increment; downstream geometry still
					// recomputes at the current frame, so the LOS refresh gets valid nodes
					// without an extra gait step.
					// DEFAULT-ON killswitch (user-confirmed fix, mc2_17 Catapult/Bushwacker):
					// disable with MC2_ANIM_CADENCE_FIX=0 to A/B the double-step.
					static const bool s_animCadenceFix = mc2::gate::get<"MC2_ANIM_CADENCE_FIX">();
					const bool _alreadyAdvanced = (s_animCadenceFix &&
						lastAnimAdvanceFrame == g_mc2FrameCounter);
					// VK-MECH-CAPTURE-DETERMINISM-1: during a frozen visual-capture
					// sweep (sim paused) the render path still ticks this gait via the
					// wall-clock frameLength, so a mech's pose drifted run-to-run and
					// forced OBJ_BOX masking in the near-oracle. Freeze the gait advance
					// while the capture sim is frozen. Capture-mode-only: the shim is 0
					// whenever no MC2_VISUAL_* capture is active -> byte-identical live
					// behavior. See gos_visual_capture.cpp mc2CaptureGaitFreezeActive().
					// FREEZE-MOVERS-1: the cadence guard alone is NOT enough under
					// freeze_movers. Mover::getLOSPosition (code/mover.cpp)
					// calls appearance->update() with the DEFAULT animate=true, and
					// LOS resolution still runs while movers are frozen. Because the
					// normal walk is skipped, lastAnimAdvanceFrame was never stamped
					// this frame, so _alreadyAdvanced is false and the first
					// LOS-triggered call would silently tick the gait -- precisely
					// the run-to-run pose drift this feature exists to remove.
					const bool _captureFroze = (mc2CaptureGaitFreezeActive() != 0) ||
					                           (mc2FreezeMoversActive() != 0);
					if (!_alreadyAdvanced && !_captureFroze)
					{
						if (inReverse)
							currentFrame -= frameInc;
						else
							currentFrame += frameInc;
						lastAnimAdvanceFrame = g_mc2FrameCounter;
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
							const uint32_t _ac_frame = g_mc2FrameCounter;
							auto _ac_it = s_lastAdvanceFrame.find((void*)this);
							const bool _ac_double = (_ac_it != s_lastAdvanceFrame.end() && _ac_it->second == _ac_frame);
							s_lastAdvanceFrame[(void*)this] = _ac_frame;
							if (_ac_double)
							{
								++s_ac_doubles;
								if (mc2_diag::tagEnabled("ANIM_CADENCE"))
								{
									char _ac_buf[256];
									snprintf(_ac_buf, sizeof(_ac_buf),
									         "{\"event\":\"advanced_twice_same_frame\",\"ptr\":\"%p\",\"gesture\":%d,\"frame\":%u,\"frameInc\":%.4f,\"doubles\":%llu,\"advances\":%llu}",
									         (void*)this, (int)currentGestureId, _ac_frame, frameInc,
									         (unsigned long long)s_ac_doubles, (unsigned long long)s_ac_advances);
									mc2_diag::writeEvent("ANIM_CADENCE", 1, (uint64_t)_ac_frame, _ac_buf);
								}
							}
						}
					}
				}

				if (currentFrame >= mechType->getNumFrames(currentGestureId))
				{
					if ((currentStateGoal == -1) && (hitGestureId == -1) && (currentGestureId != GestureIdle))		//Are we just Looping or transitioning?
					{
						currentFrame -= mechType->getNumFrames(currentGestureId);
					}
					else
					{
						// We are at the end and ready to transition
						// Force us back to the previous frame so draw
						// doesn't go off into lala land.
						atTransitionToNextGesture = TRUE;
						currentFrame = mechType->getNumFrames(currentGestureId) - 1.0f;
						
						if (currentGestureId == GestureJump)
						{
							inJump = false;
							jumpSetup = false;
							oldStateGoal = 1;		//We always started a Jump standing!
						}
					}
				}
	
				if (currentFrame < 0)
				{
					if ((currentStateGoal == -1) && (hitGestureId == -1))
					{
						currentFrame += mechType->getNumFrames(currentGestureId);
					}
					else
					{
						// We are at the end and ready to transition
						// Force us back to the previous frame so draw
						// doesn't go off into lala land.
						atTransitionToNextGesture = TRUE;
						currentFrame = 0.0f;
					}
				}
			}
			else		//This is a  single frame gesture.  We are ALWAYS ready to transition in this case!
			{
				if (currentStateGoal != -1)
					atTransitionToNextGesture = TRUE;
			}
		}
	}

	if (currentFrame < 0.0f)
		currentFrame = 0.0f;
//		STOP(("CurrentFrame of animation can never be less then zero.  Frame %f,  NumFrames %f, GestureId %d",currentFrame,mechType->getNumFrames(currentGestureId),currentGestureId));
		
	if ((currentFrame > 0.0f) && (currentFrame >= mechType->getNumFrames(currentGestureId)))
		currentFrame = 0.0f;
		
#ifdef _DEBUG
	if (logFile && (inDebugMoveMode))
	{
		char msg[1024];
		sprintf(msg,"Gesture: %d, Frame: %f, SGoal: %d, OGoal: %d, Rot: %f",currentGestureId,currentFrame,currentStateGoal,oldStateGoal,rotation);
		logFile->writeLine(msg);
	}
#endif

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
		mc2_vis::trace("mech", (const void*)this, vs_);
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
		if ((turn < 3) || gpuVis || (currentGestureId == GestureJump) || g_useGpuStaticProps)
			updateGeometry();

		// Slice B1: refresh per-actor LightsData UBO slot index for the
		// GPU mech batcher path. Mirrors bdactor.cpp pattern. No-op
		// when killswitch is off; cheap (one hash lookup + dedup-cache
		// slot write) when on. Source: msl.cpp.
		if (g_useGpuMechs && mechShape) {
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
	Stuff::Point3D xlatPosition;
	Stuff::UnitQuaternion qRotation;
 	if (leftArmOff && leftArm && leftArmRecalc())
	{
		float speed = dVel[1].GetLength();
		if (speed)
		{
			Stuff::Vector3D velDiff = dAcc[1];
			velDiff *= frameLength;
			dVel[1].Add(dVel[1],velDiff);
			speed = dVel[1].GetLength(); 
			if (speed < Stuff::SMALL)
			{
				dVel[1].x = dVel[1].y = dVel[1].z = 0.0;
			}
				
			Stuff::Vector3D posDiff = dVel[1];
			posDiff *= frameLength;
			leftArmPos.Add(leftArmPos,posDiff);
			float elev = land->getTerrainElevation(leftArmPos); 
			if (leftArmPos.z < elev)
			{
				leftArmPos.z = elev;
				dRacc[1].Zero();
				dRVel[1].Zero();
				dTime[1] -= frameLength;
				if (dTime[1] < 0.0)
					dVel[1].x = dVel[1].y = dVel[1].z = 0.0;
			}
			
			Stuff::Vector3D rvDiff = dRacc[1];
			rvDiff *= frameLength;
			dRVel[1].Add(dRVel[1],rvDiff);
				
			Stuff::Vector3D rotDiff = dRVel[1];
			rotDiff *= frameLength;
			dRot[1].Add(dRot[1],rotDiff);
		}

		mc2ToRender(xlatPosition, leftArmPos);
		
		qRotation = Stuff::EulerAngles(dRot[1].x * DEGREES_TO_RADS, dRot[1].y * DEGREES_TO_RADS, dRot[1].z * DEGREES_TO_RADS);

        // sebi: update texture handle, it will not be updated it updateGeometry 
        // is not caaled which is not correct and leads to 
        // "Flags do not match either set of vertex Data" (see txmmgr.h)
		leftArm->SetTextureHandle(0,localTextureHandle);

		leftArm->SetFogRGB(0xffffffff);
		leftArm->SetLightList(eye->getWorldLights(),eye->getNumLights());
		leftArm->TransformMultiShape(&xlatPosition,&qRotation);
			
		if (leftArmSmoke)
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
					
			mc2ToRender(leftShoulderPos, leftArmPos);
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(leftShoulderPos);
					
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			
			Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
			bool result = leftArmSmoke->Execute(&info);
			if (!result)
			{
				leftArmSmoke->Kill();
				delete leftArmSmoke;
				leftArmSmoke = nullptr;
			}
		}
	}
	
	if (rightArmOff && rightArm && rightArmRecalc())
	{
		float speed = dVel[0].GetLength();
		if (speed)
		{
			Stuff::Vector3D velDiff = dAcc[0];
			velDiff *= frameLength;
			dVel[0].Add(dVel[0],velDiff);
			speed = dVel[0].GetLength(); 
			if (speed < Stuff::SMALL)
			{
				dVel[0].x = dVel[0].y = dVel[0].z = 0.0;
			}
				
			Stuff::Vector3D posDiff = dVel[0];
			posDiff *= frameLength;
			rightArmPos.Add(rightArmPos,posDiff);
			float elev = land->getTerrainElevation(rightArmPos); 
			if (rightArmPos.z < elev)
			{
				rightArmPos.z = elev;
				dRacc[0].Zero();
				dRVel[0].Zero();
				dTime[0] -= frameLength;
				if (dTime[0] < 0.0)
					dVel[0].x = dVel[0].y = dVel[0].z = 0.0;
			}
			
			Stuff::Vector3D rvDiff = dRacc[0];
			rvDiff *= frameLength;
			dRVel[0].Add(dRVel[0],rvDiff);
				
			Stuff::Vector3D rotDiff = dRVel[0];
			rotDiff *= frameLength;
			dRot[0].Add(dRot[0],rotDiff);
		}

		mc2ToRender(xlatPosition, rightArmPos);
		
		qRotation = Stuff::EulerAngles(dRot[0].x * DEGREES_TO_RADS, dRot[0].y * DEGREES_TO_RADS, dRot[0].z * DEGREES_TO_RADS);

        // sebi: update texture handle, it will not be updated it updateGeometry 
        // is not caaled which is not correct and leads to 
        // "Flags do not match either set of vertex Data" (see txmmgr.h)
		rightArm->SetTextureHandle(0,localTextureHandle);

		rightArm->SetFogRGB(0xffffffff);
		rightArm->SetLightList(eye->getWorldLights(),eye->getNumLights());
		rightArm->TransformMultiShape(&xlatPosition,&qRotation);
		
		if (rightArmSmoke)
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
					
			mc2ToRender(rightShoulderPos, rightArmPos);
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(rightShoulderPos);
					
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			
			Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,&boundingBox);
			bool result = rightArmSmoke->Execute(&info);
			if (!result)
			{
				rightArmSmoke->Kill();
				delete rightArmSmoke;
				rightArmSmoke = nullptr;
			}
		}
	}
	} // end Arms zone

 	nextStep = prevStep = false;
	
	return TRUE;
}

void Mech3DAppearance::blowLeftArm (void)
{
	if (leftArmOff)
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
			gosASSERT(leftShoulderBoom != NULL);
			
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
			gosASSERT(leftArmSmoke != NULL);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		}
	}
	
	if (leftArmNodeIndex == -1)
		leftArmNodeIndex = mechShape->GetNodeNameId("joint_luarm");

	Stuff::Vector3D leftNodePos = getNodeIdPosition(leftArmNodeIndex);
	leftArmPos = leftNodePos;

 	if (leftShoulderBoom)
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		mc2ToRender(leftShoulderPos, leftNodePos);
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(leftShoulderPos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
		leftShoulderBoom->Start(&info);
	}

 	if (leftArmSmoke)
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		mc2ToRender(leftShoulderPos, leftArmPos);
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(leftShoulderPos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
		leftArmSmoke->Start(&info);
	}
	
	leftArmOff = true;
	
	long xlatBase = 12.0f + 50.0 / 2.0f;
	long upBase = 25.0f + 100.0;
	long rotBase = 25.0f + 100.0;
	dVel[1].x = RandomNumber(xlatBase * 2.0) - xlatBase;
	dVel[1].y = RandomNumber(xlatBase * 2.0) - xlatBase;
	dVel[1].z = RandomNumber(upBase) + upBase;

	dRVel[1].x = RandomNumber(rotBase * 2.0) - rotBase;
	dRVel[1].y = RandomNumber(rotBase * 2.0) - rotBase;
	dRVel[1].z = RandomNumber(rotBase * 2.0) - rotBase;

	dRot[1].Zero();

	dAcc[1].x = dVel[1].x * 0.1f;
	dAcc[1].y = dVel[1].y * 0.1f;
	dAcc[1].z = dVel[1].z * 0.5f;
	dAcc[1].Negate(dAcc[1]);

	dRacc[1].x = dRVel[1].x * 0.1f;
	dRacc[1].y = dRVel[1].y * 0.1f;
	dRacc[1].z = dRVel[1].z * 0.1f;
	dRacc[1].Negate(dRacc[1]);

	dTime[1] = 4.0f;

	mechShape->StopUsing("joint_luarm");
}

void Mech3DAppearance::blowRightArm (void)
{
	if (rightArmOff)
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
			gosASSERT(rightShoulderBoom != NULL);
		
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
			gosASSERT(rightArmSmoke != NULL);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		}
	}
	
	if (rightArmNodeIndex == -1)
		rightArmNodeIndex = mechShape->GetNodeNameId("joint_ruarm");

	Stuff::Vector3D rightNodePos = getNodeIdPosition(rightArmNodeIndex);
	rightArmPos = rightNodePos;

 	if (rightShoulderBoom)
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		mc2ToRender(rightShoulderPos, rightNodePos);
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(rightShoulderPos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
		rightShoulderBoom->Start(&info);
	}

 	if (rightArmSmoke)
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		mc2ToRender(rightShoulderPos, rightArmPos);
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(rightShoulderPos);
				
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
		rightArmSmoke->Start(&info);
	}

	rightArmOff = true;

	long xlatBase = 12.0f + 50.0 / 2.0f;
	long upBase = 25.0f + 100.0;
	long rotBase = 25.0f + 100.0;
	dVel[0].x = RandomNumber(xlatBase * 2.0) - xlatBase;
	dVel[0].y = RandomNumber(xlatBase * 2.0) - xlatBase;
	dVel[0].z = RandomNumber(upBase) + upBase;

	dRVel[0].x = RandomNumber(rotBase * 2.0) - rotBase;
	dRVel[0].y = RandomNumber(rotBase * 2.0) - rotBase;
	dRVel[0].z = RandomNumber(rotBase * 2.0) - rotBase;

	dRot[0].Zero();

	dAcc[0].x = dVel[0].x * 0.1f;
	dAcc[0].y = dVel[0].y * 0.1f;
	dAcc[0].z = dVel[0].z * 0.5f;
	dAcc[0].Negate(dAcc[0]);

	dRacc[0].x = dRVel[0].x * 0.1f;
	dRacc[0].y = dRVel[0].y * 0.1f;
	dRacc[0].z = dRVel[0].z * 0.1f;
	dRacc[0].Negate(dRacc[0]);

	dTime[0] = 4.0f;
	
	mechShape->StopUsing("joint_ruarm");
}
		
void Mech3DAppearance::startSmoking (long smokeLvl)
{
	
	
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
				gosASSERT(criticalSmoke != NULL);
				
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
				gosASSERT(smokeEffect != NULL);
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if (smokeLvl != -1)
	{
		if ((isSmoking == -1) || ((isSmoking != smokeLvl) && (smokeLvl != -1)))
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;

			Stuff::Vector3D smokeNodePos = getNodePosition(0);	//Always SMOKE if it exists!!
			Stuff::Point3D smokePos;
			mc2ToRender(smokePos, smokeNodePos);

			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(smokePos);

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
	
	if (isHelicopter || currentGestureId == 20)
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
				gosASSERT(waterWake != NULL);
				
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
		if (!inReverse)
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		else
			localToWorld.Multiply(gosFX::Effect_Into_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);

		waterWake->Start(&info);
		isWaking = true;
	}
}

void Mech3DAppearance::stopWaterWake (void)
{
	if (waterWake && isWaking)		//Stop the effect if we are running it!!
		waterWake->Kill();
	
	isWaking = false;
}

void Mech3DAppearance::playEjection (void)
{
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
				gosASSERT(helicopterDustCloud != NULL);
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if (!isDusting && helicopterDustCloud)		//Start the effect if we are not running it yet!!
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
				
		Stuff::Point3D wakePos;
		mc2ToRender(wakePos, position);	//dustCloud is just centered on position.
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
				
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);

		helicopterDustCloud->Start(&info);
		isDusting = true;
	}
}
