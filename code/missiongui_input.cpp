// MissionInterfaceManager: save/load, rollovers and the gameplay / mech pick.
// Declared in the class headers; shared names in missiongui_shared_internal.h.
#include "missiongui_shared_internal.h"
#include <utility>


void MissionInterfaceManager::Save (FitIniFilePtr file)
{
	//If VTOL or Karnov are active AND HAVEN'T FINISHED THEIR OPERATION, save them, what vehicle they are going to drop or what mech they were fixing.
	long vtolNum = Commander::home->getId();

	if (vTol[vtolNum] && paintingVtol[vtolNum])
	{
		file->writeBlock("VTOLData");

		file->writeIdFloat("VtolPosX",vPos[vtolNum].x);
		file->writeIdFloat("VtolPosY",vPos[vtolNum].y);
		file->writeIdFloat("VtolPosZ",vPos[vtolNum].z);

		file->writeIdLong("VehicleID",vehicleID[vtolNum]);

		if (mechToRecover[vtolNum])
			file->writeIdLong("SalvageMechWID", mechToRecover[vtolNum]->getWatchID());
		else
			file->writeIdLong("SalvageMechWID", 0);

		const auto& state = *vTol[vtolNum]->simulationAnimation();
		file->writeIdLong("VTOLFrame",state.currentFrame);
		file->writeIdLong("VTOLGesture",state.bdAnimationState);
		file->writeBlock("R2VTOLAnimation");
		file->writeIdLong("Version", 1);
		file->writeIdFloat("Frame", state.currentFrame);
		file->writeIdBoolean("SetFirstFrame", state.setFirstFrame);
		file->writeIdBoolean("CanTransition", state.canTransition);
		file->writeIdLong("BeamSerial", static_cast<long>(state.ejectionSerial));
		file->writeIdLong("LastTick", static_cast<long>(state.lastAnimAdvanceFrame));
		file->writeIdFloat("RecoveryStart", vTolTime[vtolNum]);
		file->writeIdBoolean("VehicleDropped", vehicleDropped[vtolNum]);
		file->writeIdBoolean("MechRecovered", mechRecovered[vtolNum]);
		file->writeIdLong("ReinforcementWID", reinforcement ? reinforcement->getWatchID() : 0);
	}
}

void MissionInterfaceManager::Load (FitIniFilePtr file)
{
	long vtolNum = Commander::home->getId();
	long commanderID = vtolNum;

	if (file->seekBlock("VTOLData") == NO_ERR)
	{
		file->readIdFloat("VtolPosX",vPos[vtolNum].x);
		file->readIdFloat("VtolPosY",vPos[vtolNum].y);
		file->readIdFloat("VtolPosZ",vPos[vtolNum].z);

		file->readIdLong("VehicleID",vehicleID[vtolNum]);

		if (vehicleID[vtolNum] != 147)		//NOT a recovery vehicle.  Standard VTOL
		{
			AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "vtol" );
			vTol[commanderID] = std::make_unique<BldgAppearance>();
			vTol[commanderID]->init( appearanceType );
		}
		else
		{
			AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "karnov" );
			vTol[commanderID] = std::make_unique<BldgAppearance>();
			vTol[commanderID]->init( appearanceType );
		}

		long mechWID = 0;
		file->readIdLong("SalvageMechWID", mechWID);
		mechToRecover[vtolNum] = static_cast<MoverPtr>(ObjectManager->getByWatchID(mechWID));

		float savedFrame = 0;
		file->readIdFloat("VTOLFrame",savedFrame);

		long gestureId = 0;
		file->readIdLong("VTOLGesture",gestureId);
		vTol[vtolNum]->setGesture(gestureId);
		vTol[vtolNum]->simulationAnimation()->currentFrame = savedFrame;

		vehicleFile = controlGui.getVehicleNameFromID(vehicleID[vtolNum]);

		{
			if (vehicleID[vtolNum] != 147)		//NOT a recovery vehicle.  Standard VTOL
			{
				if (!dustCloud[commanderID] && prefs.useNonWeaponEffects)
				{
					if (strcmp(weaponEffects->GetEffectName(VTOL_DUST_CLOUD),"NONE") != 0)
					{
						unsigned flags = 0;

						Check_Object(gosFX::EffectLibrary::Instance);
						gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(VTOL_DUST_CLOUD));

						if (gosEffectSpec)
						{
							dustCloud[commanderID] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
							gosASSERT(dustCloud[commanderID] != nullptr);

							MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
						}
					}
				}
			}
			else						//IS a recovery vehicle.  DO NOT create VTOL, use KARNOV instead.  NO Vehicle created either
			{
				mechRecovered[commanderID] = false;	//Need to know when mech is done so Karnov can fly away.

				if (!dustCloud[commanderID] && prefs.useNonWeaponEffects)
				{
					if (strcmp(weaponEffects->GetEffectName(KARNOV_DUST_CLOUD),"NONE") != 0)
					{
						unsigned flags = 0;

						Check_Object(gosFX::EffectLibrary::Instance);
						gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(KARNOV_DUST_CLOUD));

						if (gosEffectSpec)
						{
							dustCloud[commanderID] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
							gosASSERT(dustCloud[commanderID] != nullptr);

							MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
						}
					}
				}

				if (!recoveryBeam[commanderID])
				{
					if (strcmp(weaponEffects->GetEffectName(KARNOV_RECOVERY_BEAM),"NONE") != 0)
					{
						unsigned flags = 0;

						Check_Object(gosFX::EffectLibrary::Instance);
						gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(KARNOV_RECOVERY_BEAM));

						if (gosEffectSpec)
						{
							recoveryBeam[commanderID] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
							gosASSERT(recoveryBeam[commanderID] != nullptr);

							MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
						}
					}
				}
			}
		}

		/*
		switch (vehicleID)
		{
			case 120:	//Minelayer
				soundSystem->playSupportSample(SUPPORT_MINELAYER);
			break;

			case 182:	//Repair
				soundSystem->playSupportSample(SUPPORT_REPAIR);
			break;

			case 221:	//Scout
				soundSystem->playSupportSample(SUPPORT_SCOUT);
			break;

			case 147:	//Recovery
				soundSystem->playSupportSample(SUPPORT_RECOVER);
			break;

			case 415:	//Artillery
				soundSystem->playSupportSample(SUPPORT_RECOVER);
			break;
		}
		*/

		if (vehicleID[commanderID] != 147)
			soundSystem->playDigitalSample(VTOL_ANIMATE,vPos[commanderID]);
		else
			soundSystem->playDigitalSample(SALVAGE_CRAFT,vPos[commanderID]);

		glm::vec3 newPos = vPos[commanderID];
		float rotation = 0.f;  // this needs to be pointing 180 degrees from drop point

		vTol[commanderID]->setObjectParameters( newPos, 0, false, Team::home->id, 0);
		// Geometry is rebuilt during presentation; loading does not advance the clock.
		vTol[commanderID]->setInView( true );
		vTol[commanderID]->setVisibility( 1, 1 );
		vTol[commanderID]->rotation = rotation;

		if (dustCloud[commanderID])
		{
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			glm::mat4	localResult;

			glm::vec3 dustPos = vTol[commanderID]->position;
			glm::vec3 wakePos;
			mc2ToRender(wakePos, dustPos);

			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, wakePos);

			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);

			dustCloud[commanderID]->SetLoopOff();
			dustCloud[commanderID]->SetExecuteOn();
			dustCloud[commanderID]->Start(&info);
		}

		paintingVtol[commanderID] = 1;
		vTolSpeed = -75.f;

		vehicleDropped[commanderID] = false;
		mechRecovered[commanderID] = false;
		if (file->seekBlock("R2VTOLAnimation") == NO_ERR) {
			long version = 0, serial = 0, reinforcementWID = 0;
			file->readIdLong("Version", version);
			if (version != 1) Fatal(0, "Unsupported R2 VTOL animation section");
			auto& state = *vTol[commanderID]->simulationAnimation();
			file->readIdFloat("Frame", state.currentFrame);
			file->readIdBoolean("SetFirstFrame", state.setFirstFrame);
			file->readIdBoolean("CanTransition", state.canTransition);
			file->readIdLong("BeamSerial", serial);
			state.ejectionSerial = static_cast<uint32_t>(serial);
			long lastTick = 0;
			file->readIdLong("LastTick", lastTick);
			state.lastAnimAdvanceFrame = static_cast<uint32_t>(lastTick);
			file->readIdFloat("RecoveryStart", vTolTime[commanderID]);
			file->readIdBoolean("VehicleDropped", vehicleDropped[commanderID]);
			file->readIdBoolean("MechRecovered", mechRecovered[commanderID]);
			file->readIdLong("ReinforcementWID", reinforcementWID);
			reinforcement = static_cast<MoverPtr>(ObjectManager->getByWatchID(reinforcementWID));
		}

		controlGui.disableAllVehicleButtons();
	}
}

void MissionInterfaceManager::updateRollovers()
{
/*	For all (both LOS and non-LOS) the run cursors, use strings 45149/45150
	For all the jump cursors, use strings 45151/45152
	For all the walk waypoint cursors, use strings 45153/45154
	For all the run waypoint cursors, use strings 45155/45156
	For all the jump waypoint cursors, use strings 45157/45158
	For all the patrol path cursors, use strings 45159/45160
	For all the standard attack cursors, use strings 45161/45162
	For all the ammo conservation cursors, use strings 45163/45164
	For all the fire from current position cursors, use strings 45165/45166
	For all the short-range attack cursors, use strings 45167/45168
	For all the medium-range attack cursors, use strings 45169/45170
	For all the long-range attack cursors, use strings 45171/45172
	For all the force-fire attack cursors, use strings 45173/45174
	For the guard cursor, use strings 45147/45148 (one each for right-click and left-click models).*/


	long cursor = userInput->getMouseCursor();

	if ( ( cursor >= mState_RUN1 && cursor <= mState_RUN1 + 64) )
	{
		helpTextID = 0;
		controlGui.setRolloverHelpText( IDS_RUN_CURSOR_LEFT_HELP );
	}

	else if ( ( cursor >= mState_JUMP1 && cursor <= mState_JUMP1 + 64 ) )
	{
		helpTextID = 0;
		controlGui.setRolloverHelpText( IDS_JUMP_CURSOR_LEFT_HELP );
	}


 
	else
	{


		switch (cursor)
		{
			case mState_GUARD:
				if ( target )
				{
					helpTextID = 0;
					controlGui.setRolloverHelpText( IDS_GUARD_RECIPIENT_LEFT_HELP );
				}

			break;

			case mState_JUMPWAYPT:
			case mState_JUMPWAYPT_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_JUMP_WAYPOINT_CURSOR_LEFT_HELP );
				break;
			case mState_RUNWAYPT:
			case mState_RUNWAYPT_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_RUN_WAYPOINT_CURSOR_LEFT_HELP );

				break;
			case mState_WALKWAYPT:
			case mState_WALKWAYPT_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_WALK_WAYPOINT_CURSOR_LEFT_HELP );

			break;

			case mState_LINK:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_PATROL_CURSOR_LEFT_HELP );
				break;

			case mState_GENERIC_ATTACK:
			case mState_ATTACK_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_ATTACK_CURSOR_LEFT_HELP );
				break;

			case mState_ENERGY_WEAPONS:
			case mState_ENERGY_WEAPONS_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_AMMO_CONSERVE_CURSOR_LEFT_HELP );
				break;

			case mState_CURPOS_ATTACK:
			case mState_CURPOS_ATTACK_LOS:
				helpTextID = 0;
				if ( gos_GetKeyStatus( (gosEnum_KeyIndex)commands[std::to_underlying(FORCE_FIRE_KEY)].key ) == KEY_HELD )
					controlGui.setRolloverHelpText(  IDS_FORCE_FIRE_CURSOR_LEFT_HELP );
				else
					controlGui.setRolloverHelpText( IDS_FIRE_FROM_CURRENT_POSITION_CURSOR_LEFT_HELP );
				break;

			case mState_SHRTRNG_ATTACK:
			case mState_SHRTRNG_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_SHORT_RANGE_CURSOR_LEFT_HELP );
				break;

			case mState_MEDRNG_ATTACK:
			case mState_MEDRNG_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_MEDIUM_RANGE_CURSOR_LEFT_HELP );
				break;
			
			case mState_LONGRNG_ATTACK:
			case mState_LONGRNG_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_LONG_RANGE_CURSOR_LEFT_HELP );
				break;



		}
	}

}


//
// The M1.6 inline machinery (substrate gate, gesture gates, mover gate,
// viewport query, off-screen guard, coord scaling, lookupAtPixel call)
// migrated to code/gameplay_pick.cpp at M2-pre T1+T2 as
// tryGameplayPick(). This function is now a thin caller wrapper that:
//   1. Applies the category gate (IsStaticPropPickEnabled) -- STAYS in
//      caller; M2.6 mech-pickup will have its own category gate.
//   2. Builds a GameplayPickRequest from the 7 input params.
//   3. Dispatches to tryGameplayPick(req).
//   4. Switches on result.outcome to do category-specific side effects
//      (debug-state mutation + [GAMEPLAY_PICK v1] kind=StaticProp hit/miss logs).
//
// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md
// Sections 4 (algorithm), 6 (caller responsibilities), 11 (Section 11
// invariant preserved via Outcome::gated no-op).
//
// Original M1.6 spec still authoritative for behavior:
// docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md
// Sections 3 (detection condition), 4 (fallback order), 5 (y-flip), 11
// (forbidden behaviors), Q6 (4-site mover observable), Q8 (legacy
// preservation invariant).
void MissionInterfaceManager::tryStaticPropPick(bool moverSelectedThisFrame,
                                                bool shiftDn,
                                                bool leftClicked,
                                                bool bGui,
                                                bool bLeftDouble,
                                                int  mouseX,
                                                int  mouseY)
{
    // Category gate STAYS in caller. Fast path: env-OFF default. Two
    // cached bools; no per-frame getenv. M2.6 mech-pickup will gate on
    // its own IsMechPickEnabled() before dispatching to the spine.
    if (!RenderWorld::IsStaticPropPickEnabled())
        return;

    GameplayPickRequest req;
    req.mouseX                  = mouseX;
    req.mouseY                  = mouseY;
    req.shiftDn                 = shiftDn;
    req.leftClicked             = leftClicked;
    req.bGui                    = bGui;
    req.bLeftDouble             = bLeftDouble;
    req.moverSelectedThisFrame  = moverSelectedThisFrame;

    const GameplayPickResult r = tryGameplayPick(req);

    switch (r.outcome) {
    case GameplayPickResult::Outcome::hit: {
        // M2.6 latent-bug fix: post-M2.5, the substrate may return a
        // Mech handle. The static-prop caller MUST guard before
        // consuming -- a Mech handle has no recipe index, and the M2.6
        // mech caller (tryMechPick) owns mech-kind hits.
        if (r.lookup.kind != RenderWorld::RenderObjectKind::StaticProp) {
            // Not our category. Silent skip; the mech caller (or future
            // terrain/VFX caller) handles its own kinds.
            break;
        }
        // Update RenderWorld debug state. Single-slot; latest wins.
        RenderWorld::setLastGameplayPick(RenderWorld::RenderObjectKind::StaticProp,
                                         r.lookup,
                                         r.ctx.mouseX, r.ctx.mouseY,
                                         r.ctx.glX,    r.ctx.glY);
        // Sample back the unified debug-state struct so the log can
        // include the recipeIndex (LookupResult itself does not carry
        // it; the recipe lookup is done inside setLastGameplayPick).
        const RenderWorld::GameplaySelectionDebugState picked =
            RenderWorld::getLastGameplayPick();
        // Unified hit log (META-FIX of the M1.6 static-prop schema);
        // coord-diag fields BYTE-IDENTICAL to M1.6 to keep the
        // user-driven canary semantically stable across rename.
        MC2_DIAGFE(stderr,
            "[GAMEPLAY_PICK v1] hit kind=StaticProp handle=%u idx=%u gen=%u "
            "recipe=%d screen=(%d,%d) gl=(%d,%d) fbo=(%d,%d) "
            "vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)\n",
            r.lookup.handle.bits,
            static_cast<unsigned>(r.lookup.handle.index()),
            static_cast<unsigned>(r.lookup.handle.generation()),
            static_cast<int>(picked.recipeIndex),
            r.ctx.mouseX, r.ctx.mouseY,
            r.ctx.glX,    r.ctx.glY,
            r.ctx.fboX,   r.ctx.fboY,
            r.ctx.vMulX,  r.ctx.vMulY,
            r.ctx.vAddX,  r.ctx.vAddY,
            r.ctx.drawableWidth, r.ctx.drawableHeight);
        break;
    }
    case GameplayPickResult::Outcome::miss: {
        // Clear the unified debug-state struct on empty Shift+click so
        // a stale prior pick does not survive an empty-click gesture.
        RenderWorld::clearLastGameplayPick();
        // Verbose miss log only when MC2_STATIC_PROP_PICK_DEBUG=1;
        // coord-diag BYTE-IDENTICAL to M1.6 modulo the schema prefix.
        if (RenderWorld::IsStaticPropPickDebugEnabled()) {
            MC2_DIAGFE(stderr,
                "[GAMEPLAY_PICK v1] miss kind=StaticProp screen=(%d,%d) gl=(%d,%d) "
                "fbo=(%d,%d) vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) "
                "draw=(%d,%d)\n",
                r.ctx.mouseX, r.ctx.mouseY,
                r.ctx.glX,    r.ctx.glY,
                r.ctx.fboX,   r.ctx.fboY,
                r.ctx.vMulX,  r.ctx.vMulY,
                r.ctx.vAddX,  r.ctx.vAddY,
                r.ctx.drawableWidth, r.ctx.drawableHeight);
        }
        break;
    }
    case GameplayPickResult::Outcome::gated:
    case GameplayPickResult::Outcome::skipped:
        // Section 11 invariant: no-op, no log. The legacy path either
        // already consumed this click (gated) or a gesture/substrate
        // gate failed (skipped) before any lookup ran.
        break;
    }
}

void MissionInterfaceManager::tryMechPick(bool moverSelectedThisFrame,
                                          bool shiftDn,
                                          bool leftClicked,
                                          bool bGui,
                                          bool bLeftDouble,
                                          int  mouseX,
                                          int  mouseY)
{
    if (!RenderWorld::IsMechPickEnabled())
        return;

    GameplayPickRequest req;
    req.mouseX                  = mouseX;
    req.mouseY                  = mouseY;
    req.shiftDn                 = shiftDn;
    req.leftClicked             = leftClicked;
    req.bGui                    = bGui;
    req.bLeftDouble             = bLeftDouble;
    req.moverSelectedThisFrame  = moverSelectedThisFrame;

    const GameplayPickResult r = tryGameplayPick(req);

    switch (r.outcome) {
    case GameplayPickResult::Outcome::hit: {
        // Kind guard: M2.6 caller only handles Mech kind. Static-prop
        // and future terrain/VFX kinds are owned by their callers.
        if (r.lookup.kind != RenderWorld::RenderObjectKind::Mech)
            break;

        BattleMech* bm =
            GameAdapters::Mech::findMechByHandle(r.lookup.handle);
        if (bm == nullptr) {
            // Stale handle race (mech destroyed between readback and
            // resolver) -- impossible in practice (single-threaded), but
            // defensive design treats it as a benign miss for the click.
            if (RenderWorld::IsMechPickDebugEnabled()) {
                MC2_DIAGFE(stderr,
                    "[GAMEPLAY_PICK v1] miss kind=Mech reason=stale_handle "
                    "handle=%u idx=%u gen=%u screen=(%d,%d) gl=(%d,%d)\n",
                    r.lookup.handle.bits,
                    static_cast<unsigned>(r.lookup.handle.index()),
                    static_cast<unsigned>(r.lookup.handle.generation()),
                    r.ctx.mouseX, r.ctx.mouseY,
                    r.ctx.glX,    r.ctx.glY);
            }
            break;
        }

        // Fog-of-war gate. Mirrors the FULL CPU-pick predicate at
        // code/missiongui.cpp:1272-1278 verbatim:
        //   target->isMover() &&
        //   !ShowMovers &&
        //   !(MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]) &&
        //   target->getTeamId() != Team::home->getId() &&
        //   !target->isDisabled() &&
        //   ((Mover*)target)->conStat < CONTACT_SENSOR_QUALITY_1
        // i.e. fog SUPPRESSES the pick ONLY when all five hold; ShowMovers
        // (debug) and the multiplayer-defeat carve-out are the bypasses
        // the CPU pick honors and that M2.6 MUST mirror or the inspect
        // log will be silent on a mech the CPU pick successfully selects.
        //
        // MC2_MECH_PICK_PIERCE_FOG=1 short-circuits the whole predicate.
        const bool pierce = RenderWorld::IsMechPickPierceFogEnabled();
        bool visible;
        if (pierce) {
            visible = true;
        } else {
            const bool showMovers = (ShowMovers != 0);
            const bool mpDefeat   = (MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]);
            const bool hostile    = (bm->getTeamId() != Team::home->getId());
            const bool disabled   = bm->isDisabled();
            const bool sub_q1     = ((static_cast<Mover*>(bm))->conStat < CONTACT_SENSOR_QUALITY_1);
            const bool fogSuppresses =
                !showMovers && !mpDefeat && hostile && !disabled && sub_q1;
            visible = !fogSuppresses;
        }
        if (!visible) {
            // Fog-gated; silent on default path so the substrate cannot
            // become a sensor cheat. Diagnostic log under DEBUG=1.
            if (RenderWorld::IsMechPickDebugEnabled()) {
                MC2_DIAGFE(stderr,
                    "[GAMEPLAY_PICK v1] gated kind=Mech reason=fog_of_war "
                    "handle=%u screen=(%d,%d) gl=(%d,%d)\n",
                    r.lookup.handle.bits,
                    r.ctx.mouseX, r.ctx.mouseY,
                    r.ctx.glX,    r.ctx.glY);
            }
            break;
        }

        RenderWorld::setLastGameplayPick(RenderWorld::RenderObjectKind::Mech,
                                         r.lookup,
                                         r.ctx.mouseX, r.ctx.mouseY,
                                         r.ctx.glX,    r.ctx.glY);
        // Log line carries handle bits/index/generation + the resolved
        // BattleMech pointer (debug; not a stable cookie). NO
        // gameObjectId / partId fields per CRITICAL-1.
        MC2_DIAGFE(stderr,
            "[GAMEPLAY_PICK v1] hit kind=Mech handle=%u idx=%u gen=%u "
            "mech=%p screen=(%d,%d) gl=(%d,%d) "
            "fbo=(%d,%d) vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)%s\n",
            r.lookup.handle.bits,
            static_cast<unsigned>(r.lookup.handle.index()),
            static_cast<unsigned>(r.lookup.handle.generation()),
            static_cast<void*>(bm),
            r.ctx.mouseX, r.ctx.mouseY,
            r.ctx.glX,    r.ctx.glY,
            r.ctx.fboX,   r.ctx.fboY,
            r.ctx.vMulX,  r.ctx.vMulY,
            r.ctx.vAddX,  r.ctx.vAddY,
            r.ctx.drawableWidth, r.ctx.drawableHeight,
            pierce ? " pierce_fog=1" : "");
        break;
    }
    case GameplayPickResult::Outcome::miss: {
        // Mech caller silent on plain miss (background-pixel Shift+click
        // is high-frequency for terrain but uninteresting for mechs).
        // Verbose miss only under MC2_MECH_PICK_DEBUG=1.
        if (RenderWorld::IsMechPickDebugEnabled()) {
            MC2_DIAGFE(stderr,
                "[GAMEPLAY_PICK v1] miss kind=Mech screen=(%d,%d) gl=(%d,%d)\n",
                r.ctx.mouseX, r.ctx.mouseY,
                r.ctx.glX,    r.ctx.glY);
        }
        break;
    }
    case GameplayPickResult::Outcome::gated:
    case GameplayPickResult::Outcome::skipped:
        break;
    }
}
