#include "vk_scene_renderer.h"
#include "sim_tick_cost.h"
#include "command_replay.h"
// Mission: the per-frame Mission::update with its MC2_MISSION_SPLIT timing, mover bridge, soak hooks and coherence audit.
// Declared in the class headers; shared names in mission_internal.h.
#include "mission_internal.h"
#include "sim_world_trace.h"
#include "sim_tick_clock.h"
#include "../mclib/sim_overlap.h"
#include "../mclib/texture_assets.h"
#include <optional>
#include "move_recon_stats.h"
#include <utility>
#include "../GameOS/gameos/utils/mc2log.h"


// FREEZE-MOVERS-1: ONE predicate for "the simulation tick is suspended".
//
// Every site below was previously written `!missionInterface->isPaused()`. If
// the freeze only widened the ObjectManager branch and left these alone, the
// sim would be half-frozen, which is worse than either extreme:
//   * texture-source frame updates -- its own comment says it must not run while
//     paused because it "uncaches things which only objectManager->update can
//     cache back in", and under a freeze that call is exactly what is skipped.
//     Half-freezing would uncache textures with nothing to restore them, i.e.
//     manufacture missing/black texture artifacts -- the very defect class this
//     harness exists to REPRODUCE, which would make it a confound.
//   * SensorManager::update / updateCollisions -- keep mutating contact and
//     drawFlags state, so two "frozen" arms could still render differently.
//   * the mission ABL brain -- keeps issuing orders and rewriting mission state.
//   * scenarioTime -- drives gosFX ExecuteInfo, so effects keep animating.
// Returns false unless the verb / MC2_FREEZE_MOVERS_AT armed it, so the stock
// path is byte-identical.
static inline bool mc2SimTickFrozen(MissionInterfaceManager* mim)
{
	return (mim && mim->isPaused()) || (mc2FreezeMoversActive() != 0);
}

//: True for the second and later sim ticks of one render frame (set by Mission::update).
static bool s_extraTickInFrame = false;

static void simClockTraceTick(float scenarioTimeNow)
{
	static const bool s_traceEnabled = mc2::gate::get<"MC2_SIM_CLOCK_TRACE">();
	if (!s_traceEnabled)
		return;

	static bool s_seeded = false;
	static uint64_t s_wallStartMs = 0;
	static float s_scenarioStart = 0.0f;
	static uint64_t s_frameCount = 0;
	static uint64_t s_lastEmitWallMs = 0;

	uint64_t nowMs = static_cast<uint64_t>(timeGetTime());
	if (!s_seeded)
	{
		s_wallStartMs = nowMs;
		s_scenarioStart = scenarioTimeNow;
		s_lastEmitWallMs = nowMs;
		s_seeded = true;
	}
	++s_frameCount;

	if (nowMs - s_lastEmitWallMs >= 1000)
	{
		double wallElapsedSec = static_cast<double>((nowMs - s_wallStartMs)) / 1000.0;
		double simElapsedSec = static_cast<double>((scenarioTimeNow - s_scenarioStart));
		double ratio = (wallElapsedSec > 0.0) ? (simElapsedSec / wallElapsedSec) : 0.0;
		fprintf(stderr,
			"[SIM-CLOCK-TRACE] wall=%.3fs sim=%.3fs ratio(sim/wall)=%.3f frames=%llu frameLength=%.5f\n",
			wallElapsedSec, simElapsedSec, ratio,
			static_cast<unsigned long long>(s_frameCount), static_cast<double>(frameLength));
		fflush(stderr);
		s_lastEmitWallMs = nowMs;
	}
}
namespace {
	static const bool s_misSplit = mc2::gate::get<"MC2_MISSION_SPLIT">();
	enum class MisPhase : int { MS_LAND_UPDATE=0, MS_PATHMGR, MS_CLEAR_BLOCKS, MS_CLEAR_VERTS,
	       MS_TERRAIN_TEX, MS_GEOMETRY, MS_OBJMGR,
	       MS_TOTAL, MS_CLEARARRAYS, MS_INTERFACE, MS_CAMERA, MS_VTOL, MS_WEATHER, MS_WAYPOINTS, MS_PRESENT,
	       MS_COUNT };
	using enum MisPhase;
	static constexpr int MS_COUNT_I = std::to_underlying(MisPhase::MS_COUNT);
	static const char* s_misNames[MS_COUNT_I] = {
		"land_update","pathmgr","clearBlocks","clearVerts","terrainTex","geometry","objmgr",
		"TOTAL","clearArrays","interface","camera","vtol","weather","waypoints","present" };
	static unsigned long long s_misNs[MS_COUNT_I]  = {0};
	static unsigned long long s_misMax[MS_COUNT_I] = {0};
	static unsigned long long s_misFrames = 0;
	static bool s_misAtexit = false;
	static void misEmit() {
		if (!s_misSplit) return;
		std::printf("[MISSION_SPLIT v1] event=shutdown frames=%llu", s_misFrames);
		for (int i = 0; i < MS_COUNT_I; i++)
			std::printf(" %s={avg_us:%.1f,max_us:%.1f}", s_misNames[i],
				s_misFrames ? static_cast<double>(s_misNs[i])/s_misFrames/1000.0 : 0.0,
				static_cast<double>(s_misMax[i])/1000.0);
		std::printf("\n"); std::fflush(stdout);
	}
	struct MisScope {
		int idx; std::chrono::steady_clock::time_point t0;
		explicit MisScope(MisPhase i) : idx(std::to_underlying(i)) {
			if (s_misSplit) t0 = std::chrono::steady_clock::now();
		}
		~MisScope() {
			if (!s_misSplit) return;
			unsigned long long ns = (unsigned long long)
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - t0).count();
			s_misNs[idx] += ns;
			if (ns > s_misMax[idx]) s_misMax[idx] = ns;
		}
	};
}

namespace {
	// --- CRASH-SOAK harness (MC2_SOAK_AUTOWIN) -------------------------------
	// Auto-win the running mission after MC2_SOAK_WIN_AFTER_SEC of scenario
	// time so a campaign booted via MC2_BOOT_TO_BAY walks itself end-to-end
	// unattended. Gated entirely on the env flag; default OFF = byte-identical.
	static const bool s_soakAutoWin = mc2::gate::get<"MC2_SOAK_AUTOWIN">();
	static float soakWinAfterSec() {
		return mc2::gate::get<"MC2_SOAK_WIN_AFTER_SEC">();   // the row clamps to >= 0
	}

	// --- MC2_SOAK_KILL_ENEMY -------------------------------------------------
	// When set, kills one random live enemy BattleMech before autowin fires so
	// the salvage screen has a candidate, and then programmatically salvages it
	// into LogisticsData inventory.  Default OFF = byte-identical when unset.
	static const bool s_soakKillEnemy = mc2::gate::get<"MC2_SOAK_KILL_ENEMY">();

	// Stores the variantName of the enemy killed this mission so the next
	// mission can assert it reached the load-bay inventory.
	static char s_soakKilledVariant[64] = {};
	static bool s_soakKillDone = false;      // armed once per mission
	static bool s_soakSalvageDone = false;   // salvage commit fired once per mission
	static bool s_soakInventoryChecked = false; // load-bay check fired once per mission

	// --- MC2_SOAK_SALVAGE_TRADEMOVER (ISSUE-42a repro fixture) ----------------
	// Requires MC2_SOAK_KILL_ENEMY + MC2_SOAK_AUTOWIN. Instead of the Phase-2
	// addMechToInventory shortcut, exercise the REAL mid-mission salvage-craft
	// flow (missiongui.cpp doMove salvage: recover -> setStatus(SHUTDOWN,force)
	// -> tradeMover -> orderPowerUp) on ONE disabled enemy mech before autowin,
	// so LogisticsData::setMissionCompleted's recovered-mech carry-over branch
	// is what the Phase-3 load-bay check verifies. Default OFF = byte-identical.
	static const bool s_soakSalvageTrademover = mc2::gate::get<"MC2_SOAK_SALVAGE_TRADEMOVER">();
	static bool s_soakTradeDone = false;     // armed once per mission

	// --- MC2_SOAK_PILOT_PROMOTE ----------------------------------------------
	// Grants a deployed pilot enough skill delta to cross the next rank
	// threshold, then runs the same promotePilot() call the UI uses.
	// Throttled to mission-index 2 and 5 within a campaign (1-based).
	// Default OFF = byte-identical when unset.
	static const bool s_soakPilotPromote = mc2::gate::get<"MC2_SOAK_PILOT_PROMOTE">();
	static bool s_soakPromoteFired = false;  // armed once per mission

	// --- MC2_FIXTURE_ORDER_LIVENESS (META-FIX design F) -----------------------
	// Behavioral order-liveness fixture: proves issued orders DO something
	// (issues #17 reluctant fire / #10 scripted stalls class). Requires
	// MC2_SOAK_AUTOWIN. Phases (fractions of the autowin timer, trademover
	// block shape): 0.2 = COMMANDER-origin unaimed ATTACK_TO_DESTROY per armed
	// home mech at the nearest live enemy (PLAYER origin never installs the
	// tac order — warrior.cpp orderAttackObject/setGeneralTacOrder trap);
	// 0.5 = COMMANDER-origin move order per home mech (300 m toward map
	// center — the move order replaces the attack order, giving the move leg
	// a clean 0.4*win displacement window; the REVIEWED order (move 0.2,
	// attack 0.3) self-sabotaged the move bound: a mech whose target spawns
	// in range stands and fires without ever covering 25 m — measured 1/11
	// stalled on stock mc2_24); 0.9 = one self-asserting summary line
	//   ORDER_LIVENESS_SUMMARY issued_move=N moved=N refused=N stalled=N verdict=PASS|FAIL
	// (moved = net displacement >= 25 m; refused = classified refusal via
	// LastMoveCalcErr; dead/disabled units exempt). [ORDER_FIXTURE] lines mark
	// each issued order. run_smoke.py hard-fails a missing summary or
	// verdict=FAIL ONLY when MC2_FIXTURE_ORDER_LIVENESS is set in the runner
	// env (VEG-SMOKE-FLOOR activation pattern); consumed by
	// scripts/check-order-liveness.py. Default OFF = byte-identical.
	static const bool s_fixtureOrderLiveness =
		mc2::gate::get<"MC2_FIXTURE_ORDER_LIVENESS">();
	struct OrderLivenessEntry {
		unsigned long   wid;
		glm::vec3 startPos;
		glm::vec3 goalPos;
		float issuedAt;
		bool arrived;
		int maxGlobalSteps;
	};
	static const int kOrderLivenessCap = 32;   // bounded, s_firedWIDs precedent
	static OrderLivenessEntry s_olEntries[kOrderLivenessCap];
	static int  s_olIssuedMove   = 0;
	static bool s_olMoveIssued   = false;
	static bool s_olAttackIssued = false;
	static bool s_olSummaryDone  = false;
} // namespace


// Handle the developer hotkeys.
void Mission::updateDevHotkeys() {
	if ((missionLineChanged + 50) < turn)
	{
		#ifndef FINAL
		if (userInput->getKeyDown(KEY_X) && userInput->ctrl() && !userInput->alt() && !userInput->shift())
		{
			useCollisions ^= true;
			missionLineChanged = turn;
		}		
	
		if (userInput->getKeyDown(KEY_X) && userInput->ctrl() && userInput->alt() && userInput->shift())
		{
			saveInMissionSave = true;
			missionLineChanged = turn;
		}		

		if (userInput->getKeyDown(KEY_Z) && userInput->ctrl() && userInput->alt() && userInput->shift())
		{
			loadInMissionSave = true;
			missionLineChanged = turn;
		}		

		if (userInput->getKeyDown(KEY_Y) && userInput->ctrl() && !userInput->alt() && !userInput->shift())
		{
			DisplayCameraAngle ^= true;
			missionLineChanged = turn;
		}		

 			if (userInput->getKeyDown(KEY_N) && userInput->ctrl() && !userInput->alt() && !userInput->shift())
		{
			useSensors ^= true;
			missionLineChanged = turn;
		}

		if (userInput->getKeyDown(KEY_P) && userInput->ctrl() && userInput->alt() && userInput->shift())
		{
			LogisticsData::instance->setResourcePoints(100000);
			missionLineChanged = turn;
		}		

		if (userInput->getKeyDown(KEY_C) && userInput->ctrl() && userInput->alt() && userInput->shift())
		{
			neverEndingStory = true;
			missionLineChanged = turn;
		}

		// DEV HOTKEY (Ctrl+Alt+Shift+K): disable ALL enemy mechs on demand, the same
		// way MC2_SOAK_KILL_ENEMY does (OBJECT_STATUS_DISABLED, not destroyed) so they
		// stay salvageable. Lets you grab salvage without a full firefight. Disabled in
		// FINAL builds (this whole block is #ifndef FINAL).
		if (userInput->getKeyDown(KEY_K) && userInput->ctrl() && userInput->alt() && userInput->shift())
		{
			int killed = 0;
			for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
			{
				BattleMech* pM = ObjectManager->getMech(mi);
				if (!pM) continue;
				if (pM->isDisabled() || pM->isDestroyed()) continue;
				if (Team::home && pM->getTeamId() == Team::home->id) continue;  // skip friendlies
				pM->setStatus(OBJECT_STATUS_DISABLED, /*force=*/true);
				++killed;
			}
			printf("[DEVHOTKEY] kill-all-enemy killed=%d (Ctrl+Alt+Shift+K)\n", killed);
			fflush(stdout);
			missionLineChanged = turn;
		}

		{
			// Shadow softness (Poisson disk radius) — edge-triggered (single step
			// per press, not continuous while held) with 0.1 step for fine control.
			if (gos_GetKeyStatus(KEY_LBRACKET) == KEY_PRESSED) {
				float s = max(gos_GetTerrainShadowSoftness() - 0.1f, 0.1f);
				gos_SetTerrainShadowSoftness(s);
				CB_PRINTF("[SHADOW-KEY] [: softness={:.1f}", s);
			}
			if (gos_GetKeyStatus(KEY_RBRACKET) == KEY_PRESSED) {
				float s = min(gos_GetTerrainShadowSoftness() + 0.1f, 10.0f);
				gos_SetTerrainShadowSoftness(s);
				CB_PRINTF("[SHADOW-KEY] ]: softness={:.1f}", s);
			}
		}
		#endif
	}
}

// The tick-mode clock: one call per sim tick. A tick is always kTickSeconds of sim time, whatever the frame rate or game
// speed. The in-mission save runs at the tick boundary, before the clock moves.
void Mission::updateTickClock(bool advanceTime) {
	if (saveInMissionSave)
	{
		char savegame_path[1024];
		S_snprintf(savegame_path, sizeof(savegame_path)/sizeof(savegame_path[0]), "%s" PATH_SEPARATOR "testgame.ims", savePath);
		save(savegame_path);
		saveInMissionSave = false;
	}

	if (advanceTime)
	{
		scenarioTime += mc2::sim::kTickSecondsF;
		// Counts the ticks that advanced the clock, so SIMID frame N means tick N under the harness clock.
		if (SmokeMode::fixedTimestepEnabled())
			SmokeMode::fixedTimestepOnSimFrame(static_cast<double>(scenarioTime));
	}
	simClockTraceTick(scenarioTime);
	resumeTickAudio();
	actualTime = scenarioTime;
}

// Update the interface, camera, terrain, weather, waypoints and the path manager.
void Mission::updateFrontEnd() {
	GameMap->clearCellDebugs(0);
	{ ZoneScopedN("Mission.clearArrays"); MisScope _ms(MS_CLEARARRAYS); mc2::VkSceneRenderer::beginFrame(); }

	if (missionInterface)
		{ ZoneScopedN("GameLogic.Mission.Interface"); MisScope _ms(MS_INTERFACE); missionInterface->update(); }

	{ ZoneScopedN("GameLogic.Mission.Camera"); MisScope _ms(MS_CAMERA); eye->update(); }

	// MVP-PUBLISH-EARLY-HOIST (gate MC2_MVP_PUBLISH_EARLY, default ON, =0
	// reverts). Publish THIS frame's world-to-clip matrix right after
	// eye->update() so the terrain compute snapshot taken later in this
	// update phase sees the current-frame camera, not the previous frame's.
	// The render-phase publish is kept and rewrites the identical matrix,
	// so gate OFF is byte-identical and A/B stays clean.
	//
	// Guard mirrors the render-phase publish predicate (eye->active &&
	// turn > 1). eye is always the live GameCamera here; SimpleCamera
	// pans and the editor run their own loops and self-publish, so this
	// never clobbers their publish.
	{
		static const bool s_mvpPublishEarly = mc2::gate::get<"MC2_MVP_PUBLISH_EARLY">();
		if (s_mvpPublishEarly && eye && eye->active && turn > 1)
		{
			gos_SetWorldToClipGL(eye->worldToClipGL());
		}
	}



	{ ZoneScopedN("GameLogic.Mission.Terrain"); MisScope _ms(MS_LAND_UPDATE); land->update(); }

	//ALWAYS update weather AFTER the camera.  May change the lights!
	if (useNonWeaponEffects)
		{ ZoneScopedN("GameLogic.Mission.Weather"); MisScope _ms(MS_WEATHER); weather->update(); }		//Should the rain fall during a pause?

	{ ZoneScopedN("GameLogic.Mission.Waypoints"); MisScope _ms(MS_WAYPOINTS); missionInterface->updateWaypoints(); }

#ifdef USE_PATH_COST_TABLE
	GlobalMoveMap[0]->resetPathCostTable();
#endif
	if (KillAmbientLight) {
	}
}

// The path manager serves queued path requests: sim, once per tick.
void Mission::updatePathManager() {
	{ ZoneScopedN("GameLogic.PathManager"); MisScope _ms(MS_PATHMGR); PathManager->update(); }
}

// Presentation consumes the last completed tick's snapshots, including during pause.
void Mission::updatePresentationOnly(bool moversOnly, float alpha) {
	mc2::overlap::assertSimQuiescent("updatePresentationOnly");   // presentation writes appearance state; never while a tick is in flight
	if (!moversOnly)
		gpu_cull::substrate_frameBegin();
    if (mc2::gate::get<"MC2_PRESENTATION_DISABLED">()) return;
	ZoneScopedN("GameLogic.Mission.Presentation");
	MisScope _ms(MS_PRESENT);
	const float renderLength = frameLength;
	mc2::sim::PresentationScope presentationScope;
	ObjectManager->setPresentationAlpha(alpha);
	{ ZoneScopedN("GameLogic.Presentation.Appearances");
	if (moversOnly)
		ObjectManager->updateAppearancesOnly( false, true, false );
	else
		ObjectManager->updateAppearancesOnly( true, true, true );
	}
	{ ZoneScopedN("GameLogic.Presentation.Cull"); ObjectManager->updatePresentationCull(); }
	{ ZoneScopedN("GameLogic.Presentation.VTol"); missionInterface->updateVTol(); }
	{ ZoneScopedN("GameLogic.Presentation.Damp"); updateDisplacementDamp(); }
	ObjectManager->setPresentationAlpha(1.0f);
	frameLength = renderLength;
}

// Clear the terrain object flags and process the terrain geometry.
void Mission::updateTerrainGeometry() {
	//-----------------------------------------------------------
	// Lastly, process the terrain geometry which loads textures
	// Must do this to keep from Locking during the updateRenders phase
	// Also reset the object flags because we recalc those during geometry!
	{ ZoneScopedN("Mission::update clearObjBlocksActive"); MisScope _ms(MS_CLEAR_BLOCKS); land->clearObjBlocksActive(); }
	{ ZoneScopedN("Mission::update clearObjVerticesActive"); MisScope _ms(MS_CLEAR_VERTS); land->clearObjVerticesActive(); }
	{ ZoneScopedN("Mission::update terrainTextures->update"); MisScope _ms(MS_TERRAIN_TEX); land->terrainTextures->update(); }

	{ ZoneScopedN("GameLogic.Mission.TerrainGeometry"); MisScope _ms(MS_GEOMETRY); land->geometry(); }
}

void Mission::advanceCombatClock(float seconds)
{
	const auto result = combatClock.advance(seconds);
	if (result != mc2::combat::ClockStep::advanced && !combatClock.rejectionReported) {
		combatClock.rejectionReported = true;
		MC2_EVENT(Core, "COMBAT_RETIRE", 1, "reject reason={} type=-",
			result == mc2::combat::ClockStep::clock_step ? "clock_step" : "clock_overflow");
	}
}

// Begin the GPU cull frame and update the objects, or only their appearances while frozen.
void Mission::updateObjectManager() {
	if (mc2SimTickFrozen(missionInterface) && !MPlayer) return;
	{ ZoneScopedN("GameLogic.Mission.TickPrep");
	ObjectManager->snapshotMoverPoses();
	beginOrderSimulationTick();
	advanceCombatClock(frameLength); }
	MisScope _ms(MS_OBJMGR);
	ObjectManager->update(true, true, true);
}

// Update the mover visual-displacement damp.
void Mission::updateDisplacementDamp() {
	// TERRAIN-REAUTH-UNPIN-1 Half B: near-object displacement fade, mover
	// half. Gathers live mover positions in coarse-cell space and stamps
	// them into the visual-displacement damp map (min-combined with the
	// static building damp) so units always stand on TRUE gameplay height
	// with no visual pop. Rides MC2_TERRAIN_VISUAL_DISPLACE; whole block is
	// a single cheap bool check when the displace gate / damp map is off.
	if (gos_TerrainLodChunk_VisualDampWanted())
	{
		ZoneScopedN("GameLogic.Mission.VisualDampMovers");
		static std::vector<float> s_moverCells;
		s_moverCells.clear();
		const float halfMap = Terrain::worldUnitsMapSide * 0.5f;
		const float wupv    = Terrain::worldUnitsPerVertex;   // 128
		for (long mi = 0; mi < ObjectManager->getNumMovers(); ++mi)
		{
			MoverPtr mv = ObjectManager->getMover(mi);
			if (!mv || !mv->getExists())
				continue;
			glm::vec3 mp = mv->getPosition();
			s_moverCells.push_back((mp.x + halfMap) / wupv);
			s_moverCells.push_back((halfMap - mp.y) / wupv);
		}
		static const float s_objfadeRadiusWu = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE_RADIUS">();   // the row clamps to [32, 1024]
		gos_TerrainLodChunk_UpdateVisualDampMovers(
			s_moverCells.data(), static_cast<int>((s_moverCells.size() / 2)),
			s_objfadeRadiusWu / wupv, 48.0f / wupv);
	}
}

// Update sensors and collisions.
void Mission::updateSimSystems() {
	{ ZoneScopedN("GameLogic.Mission.VTol"); MisScope _ms(MS_VTOL); missionInterface->updateVTol(); }
	if (useSensors && ( !mc2SimTickFrozen(missionInterface) || MPlayer ) )
		{ ZoneScopedN("GameLogic.Mission.Sensors"); SensorManager->update(); }

	if (useCollisions && ( !mc2SimTickFrozen(missionInterface) || MPlayer ) )
		{ ZoneScopedN("GameLogic.Mission.Collisions"); ObjectManager->updateCollisions(); }
}

// Flush the ABL trace and emit the mover bridge, the identity hash and the coherence audit.
void Mission::updateTraceHooks() {
	// Refresh after mission ABL, without advancing a controller twice.
	if (mc2::replay::active()) missionInterface->updateVTol();
	mc2_abl_trace::flushFrame(g_mc2FrameCounter, getBrainTickIndex());

	// Runtime-bridge v0: emit [MOVER v2] mover.state to stdout (env-gated,
	// throttled to MC2_BRIDGE_MOVER_PERIOD_SEC). Read-only; no-op unless
	// MC2_BRIDGE_MOVER_STATE=1.
	if (s_bridgeMoverState)
		bridgeEmitMoverState(scenarioTime);

	// SIM-IDENTITY-HASH-1: one world-state hash line per sim frame (opt-in,
	// MC2_SIM_IDENTITY_TRACE). Diffing two runs' logs names the FIRST
	// divergent sim frame directly -- the bridge above needs ~58k per-mover
	// lines and a bespoke script to answer the same question.
	//
	// simFrame is the 0-based tick (turn - 1) and equals the capture sidecar's
	// `sim_frame` (gos_visual_capture.cpp write_sidecar) so a trace line and
	// a manifest taken in the same run carry the SAME frame number. That
	// derivation is only meaningful under the fixed clock; with the fixed
	// clock off it pins to 0 and the `t=` field is the usable diff axis --
	// which is correct, because determinism is not claimed at all then.
	mc2SimIdentityTraceFrame(
		static_cast<double>(scenarioTime),
		turn - 1);   // 0-based, like the sidecar's sim_frame and the committed baselines

	// META-FIX design A: mover-registry coherence audit (~1 Hz walk over
	// home-commander movers + force-bar icons; read-only). No-op unless
	// MC2_MOVER_COHERENCE_AUDIT=1 (static latch; gate-off = one bool).
	if (s_moverCoherenceAudit)
		auditMoverCoherence(scenarioTime, missionInterface);
}

// Decide whether the mission is over and start the termination countdown.
void Mission::updateTermination() {
	const auto oldTermination = terminationResult;
	const bool oldStarted = terminationCounterStarted;
	if (!MPlayer && !terminationCounterStarted)
	{
		bool playerForceAllDead = true;
		for (long i=0;i<ObjectManager->getNumMovers();i++)
		{
			MoverPtr mover = ObjectManager->getMover(i);
			if (mover->getCommanderId() == Commander::home->getId())
				if (!mover->isDisabled() && !mover->isDestroyed() && mover->isOnGUI()) {
					playerForceAllDead = false;
					break;
				}
		}

		if (playerForceAllDead && !neverEndingStory)
		{
			terminationResult = mis_PLAYER_LOST_BIG;
			terminationCounterStarted = true;
			missionTerminationTime = actualTime + 5.0/*seconds*/;
		}
	}

	if (!neverEndingStory && terminationCounterStarted) 
	{
		if (missionTerminationTime <= actualTime) {
			if (ControlGui::instance->resultsDone()) {
				scenarioResult = terminationResult;

				if ( !scenarioResult )
				{
					// mission brain frequently trashes the termination result
					if (!MPlayer) {
						int status = Team::home->objectives.Status();
						if (OS_SUCCESSFUL == status) 
							scenarioResult = mis_PLAYER_WIN_BIG;
						else
							scenarioResult = mis_PLAYER_LOST_BIG;
					}
				}

			}
			else if (!MPlayer) {
				bool playerForceAllDead = true;
				for (long i=0;i<ObjectManager->getNumMovers();i++)
				{
					MoverPtr mover = ObjectManager->getMover(i);
					if (mover->getCommanderId() == Commander::home->getId())
						if (!mover->isDisabled() && !mover->isDestroyed() && mover->isOnGUI()) {
							playerForceAllDead = false;
							break;
						}
				}

				if (playerForceAllDead)
				{
					terminationResult = mis_PLAYER_LOST_BIG;
					terminationCounterStarted = true;
					missionTerminationTime = actualTime + 5.0/*seconds*/;
				}
			}
		}
	} 
	else 
	{
		//As long as the ABL script returns -1, we won't do this.  NEEDED for Tutorials
		if (terminationResult != -1)
		{
			if (MPlayer) 
			{
				if (MPlayer->calcMissionStatus()) 
				{
					terminationCounterStarted = true;
					missionTerminationTime = actualTime + 5.0/*seconds*/;
					if (MPlayer->playerInfo[MPlayer->commanderID].winner)
						terminationResult = mis_PLAYER_WIN_BIG;
					else
						terminationResult = mis_PLAYER_LOST_BIG;

				}
			}
			else if (1 <= Team::home->objectives.Count()) 
			{
				int status = Team::home->objectives.Status();
				if (0 < Team::home->numPrimaryObjectives) 
				{
					if (OS_UNDETERMINED != status) 
					{
						if (!neverEndingStory)
						{
							if (OS_SUCCESSFUL == status) 
							{
								terminationResult = mis_PLAYER_WIN_BIG;
								terminationCounterStarted = true;
								missionTerminationTime = actualTime + 5.0/*seconds*/;
							} 
							else 
							{
								terminationResult = mis_PLAYER_LOST_BIG;
								terminationCounterStarted = true;
								missionTerminationTime = actualTime + 5.0/*seconds*/;
							}
						}
					}
				}
			}
		}
	}

	if (!MPlayer && !terminationCounterStarted)
	{
		if ((m_timeLimit != -1.0f) && (actualTime > m_timeLimit) && !neverEndingStory)
		{
			terminationResult = mis_PLAYER_LOST_BIG;
			terminationCounterStarted = true;
			missionTerminationTime = actualTime + 5.0/*seconds*/;
		}
	}
	if (auto* observation = objectiveObservation(); observation &&
	    (oldTermination != terminationResult || oldStarted != terminationCounterStarted)) {
		observation->Emit("resolve", 0, std::format("{{\"scope\":\"mission\",\"stage\":\"termination\",\"old\":{},\"new\":{},\"started\":{}}}",
			oldTermination, terminationResult, terminationCounterStarted));
	}
}

// Handle the frame-rate toggle and the cheat keys and show the frame rate.
void Mission::updateCheatsAndFrameRate() {
	if (userInput->getKeyDown(KEY_F) && !userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		showFrameRate ^= true;
		missionLineChanged = turn;
	}

#ifndef FINAL
	bool cheatWin = (userInput->getKeyDown(KEY_W) && userInput->shift() && userInput->ctrl() && userInput->alt());
	bool cheatLose = (userInput->getKeyDown(KEY_L) && userInput->shift() && userInput->ctrl() && userInput->alt()); 
	if (cheatWin)
		scenarioResult = mis_PLAYER_WIN_BIG;
	else if (cheatLose)
		scenarioResult = mis_PLAYER_LOST_BIG;

	//FORCE these to play to thwart any ABL bugs.
	// OPPOSITE for a reason!!!!!!!!
	// Duane decided to switch the order for our conveniance.  Thanks!!
	if ((terminationResult) && (terminationResult != -1) && (terminationResult < mis_PLAYER_DRAW))
	{
		soundSystem->playDigitalMusic(WIN_TUNE_0);
	}
	else if (terminationResult > mis_PLAYER_DRAW)
	{
		soundSystem->playDigitalMusic(LOSE_TUNE_0);
	}

	if (showFrameRate)
	{
		char text[1024];
		(*std::format_to_n(text,sizeof(text) - 1,"FrameRate: {:f}",1.0f/frameLength).out = '\0');
	
		DWORD width, height;
		Stuff::Vector4D moveHere;
		moveHere.x = 0.0f;
		moveHere.y = 0.0f;
	
		gos_TextSetAttributes (gosFontHandle, 0, gosFontScale, false, true, false, false);
		gos_TextStringLength(&width,&height,text);
	
		moveHere.z = width;
		moveHere.w = height;
	
		globalFloatHelp->setFloatHelp(text,moveHere,SD_GREEN,XP_BLACK,1.0f,true,false,false,false);
	}
#endif
}

// Win the mission after the soak delay.
void Mission::updateSoakAutoWin() {
	// CRASH-SOAK auto-win (always compiled; gated on MC2_SOAK_AUTOWIN env).
	// Independent of the #ifndef FINAL cheat path above so the harness works
	// in RelWithDebInfo regardless of FINAL. Fires once per mission after
	// MC2_SOAK_WIN_AFTER_SEC of scenario time. mis_PLAYER_WIN_BIG returned
	// from update() ends the mission instantly (no countdown).
	if (s_soakAutoWin && !scenarioResult)
	{
		static bool s_soakWinEmitted = false;
		const float winAfter = soakWinAfterSec();
		// Re-arm for each new mission: scenarioTime resets to 0 on start().
		if (scenarioTime < winAfter)
			s_soakWinEmitted = false;
		if (!s_soakWinEmitted && scenarioTime > winAfter)
		{
			scenarioResult = mis_PLAYER_WIN_BIG;
			s_soakWinEmitted = true;
			printf("[SOAK] autowin mission=%s t=%.1f\n",
				missionFileName, static_cast<float>(scenarioTime));
			fflush(stdout);
		}
	}
}

// Run the soak kill, salvage and load-bay phases.
void Mission::updateSoakKillEnemy() {
	// --- MC2_SOAK_KILL_ENEMY ---------------------------------------------
	// Three phases per mission pair.
	// Phase 1 (current mission, t=60% win): disable one enemy mech.
	// Phase 2 (current mission, post-win):  commit it to inventory.
	// Phase 3 (next mission, first frame):  verify it is in load bay.
	// Re-arm clears kill/salvage flags but intentionally does NOT clear
	// s_soakKilledVariant until AFTER Phase 3 has fired, so the cross-
	// mission variant name survives the mission boundary.
	if (s_soakKillEnemy && s_soakAutoWin)
	{
		// PHASE 3 (next mission, first frames): load-bay check using the
		// variant name carried over from last mission.  Must run BEFORE
		// re-arm to avoid s_soakKilledVariant being cleared too early.
		if (!s_soakInventoryChecked && s_soakSalvageDone &&
			s_soakKilledVariant[0] &&
			scenarioTime >= 0.0f && scenarioTime < 1.0f)
		{
			LogisticsVariant* pChkVar =
				LogisticsData::instance
					? LogisticsData::instance->getVariant(s_soakKilledVariant)
					: nullptr;
			int present = 0;
			if (pChkVar && LogisticsData::instance)
				present = LogisticsData::instance->getVariantsInInventory(
					pChkVar, /*bIncludeForceGroup=*/true);
			printf("[SOAK] salvage-in-loadbay name=%s present=%d\n",
				s_soakKilledVariant, present > 0 ? 1 : 0);
			fflush(stdout);
			s_soakInventoryChecked = true;
		}

		// Re-arm each new mission (scenarioTime resets to 0 on start()).
		// Only clears variant name after Phase 3 has had its window.
		if (scenarioTime < soakWinAfterSec() * 0.5f)
		{
			s_soakKillDone = false;
			s_soakSalvageDone = false;
			s_soakTradeDone = false;
			if (s_soakInventoryChecked)
			{
				s_soakInventoryChecked = false;
				s_soakKilledVariant[0] = '\0';
			}
		}

		// PHASE 1: "cheat mode" — disable EVERY enemy mech at 60% of the win
		// timer so they are all DISABLED (not DESTROYED) and therefore all
		// salvageable. This maximally stresses the salvage system and mech-bay
		// capacity over a campaign. Remember the first variant for the Phase 3
		// load-bay round-trip check.
		const float killAt = soakWinAfterSec() * 0.6f;
		if (!s_soakKillDone && scenarioTime > killAt && !scenarioResult)
		{
			int killed = 0;
			for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
			{
				BattleMech* pM = ObjectManager->getMech(mi);
				if (!pM) continue;
				if (pM->isDisabled() || pM->isDestroyed()) continue;
				if (Team::home && pM->getTeamId() == Team::home->id) continue;
				// OBJECT_STATUS_DISABLED: salvage screen keeps it (isDisabled
				// true, isDestroyed false).
				pM->setStatus(OBJECT_STATUS_DISABLED, /*force=*/true);
				if (!s_soakKilledVariant[0])
				{
					strncpy(s_soakKilledVariant, pM->variantName,
						sizeof(s_soakKilledVariant) - 1);
					s_soakKilledVariant[sizeof(s_soakKilledVariant) - 1] = '\0';
				}
				++killed;
			}
			printf("[SOAK] kill-enemy killed=%d first=%s\n",
				killed, s_soakKilledVariant[0] ? s_soakKilledVariant : "(none)");
			fflush(stdout);
			s_soakKillDone = true;
		}

		// PHASE 1.5 (ISSUE-42a, MC2_SOAK_SALVAGE_TRADEMOVER): before autowin
		// fires, run the REAL mid-mission salvage-craft flow on ONE disabled
		// enemy mech — the exact sequence the salvage VTOL executes in
		// missiongui.cpp (recover / setStatus(SHUTDOWN,force) / tradeMover /
		// orderPowerUp). The mech becomes a home-commander mover, so mission
		// completion routes it through LogisticsData::setMissionCompleted's
		// recovered-mech carry-over branch (the path issue #42a suspects)
		// instead of the Phase-2 addMechToInventory shortcut (Phase 2 skips
		// it once it is home-team). Phase 3 then asserts the SAME variant
		// reached the load bay. Pair with MC2_LOG_LOGISTICS_FLOW +
		// MC2_DIAG_TAGS=LOGISTICS for per-unit cull/resolve/mech_lost JSONL.
		const float tradeAt = soakWinAfterSec() * 0.8f;
		if (s_soakSalvageTrademover && !s_soakTradeDone && s_soakKillDone &&
			scenarioTime > tradeAt && !scenarioResult)
		{
			BattleMech* pTrade = nullptr;
			for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
			{
				BattleMech* pM = ObjectManager->getMech(mi);
				if (!pM) continue;
				if (!pM->isDisabled() || pM->isDestroyed()) continue;
				if (Team::home && pM->getTeamId() == Team::home->id) continue;
				// Prefer the mech Phase 1 recorded so Phase 3's load-bay
				// assert tracks the traded mech; else first candidate.
				if (s_soakKilledVariant[0] &&
					strcmp(pM->variantName, s_soakKilledVariant) == 0)
				{
					pTrade = pM;
					break;
				}
				if (!pTrade)
					pTrade = pM;
			}
			char* newPilotName = (pTrade && LogisticsData::instance)
				? const_cast<char*>(LogisticsData::instance->getBestPilot(pTrade->tonnage))
				: nullptr;
			if (pTrade && newPilotName && Team::home && Commander::home)
			{
				// Mirror missiongui.cpp salvage-craft recovery verbatim.
				while ((static_cast<MoverPtr>(pTrade))->recover() == false)
					;
				if (pTrade->isDisabled())
				{
					pTrade->setStatus(OBJECT_STATUS_SHUTDOWN, true);
					if (pTrade->getSensorSystem())
						pTrade->getSensorSystem()->broken = false;
					(static_cast<MoverPtr>(pTrade))->timeLeft = 1.0f;
					(static_cast<MoverPtr>(pTrade))->exploding = false;
				}
				tradeMover(pTrade, Team::home->getId(),
					Commander::home->getId(), newPilotName, "pbrain");
				pTrade->getPilot()->orderPowerUp(true, ORDER_ORIGIN_SELF);
				strncpy(s_soakKilledVariant, pTrade->variantName,
					sizeof(s_soakKilledVariant) - 1);
				s_soakKilledVariant[sizeof(s_soakKilledVariant) - 1] = '\0';
				printf("[SOAK] salvage-trademover name=%s variant=%s pilot=%s\n",
					pTrade->getName() ? pTrade->getName() : "(null)",
					s_soakKilledVariant, newPilotName);
				fflush(stdout);
			}
			else
			{
				printf("[SOAK] salvage-trademover no-candidate\n");
				fflush(stdout);
			}
			s_soakTradeDone = true;
		}

		// PHASE 2: After autowin, salvage ALL disabled enemy mechs into the
		// inventory (mirrors SalvageMechScreen -> addMechToInventory, done for
		// every salvageable mech). This is where mech-bay capacity gets stressed.
		if (!s_soakSalvageDone && scenarioResult)
		{
			int salvCount = 0;
			if (LogisticsData::instance)
			{
				for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
				{
					BattleMech* pM = ObjectManager->getMech(mi);
					if (!pM) continue;
					if (!pM->isDisabled() || pM->isDestroyed()) continue;
					if (Team::home && pM->getTeamId() == Team::home->id) continue;
					LogisticsVariant* pVar =
						LogisticsData::instance->getVariant(pM->variantName);
					if (!pVar) continue;
					LogisticsData::instance->addMechToInventory(
						pVar, static_cast<LogisticsPilot*>(nullptr), 0, /*bSubtractPts=*/false);
					++salvCount;
				}
			}
			printf("[SOAK] salvage-check items=%d first=%s\n",
				salvCount, s_soakKilledVariant[0] ? s_soakKilledVariant : "(none)");
			fflush(stdout);
			s_soakSalvageDone = true;
		}
	}
}

// Issue the order-liveness fixture orders and report their outcome.
void Mission::updateOrderLiveness() {
	// --- MC2_FIXTURE_ORDER_LIVENESS (META-FIX design F) -------------------
	// See the namespace block above for the contract. Grammar of the
	// summary/order lines is reviewed contract — do not normalize.
	if (s_fixtureOrderLiveness && s_soakAutoWin && ObjectManager)
	{
		extern long LastMoveCalcErr;   // code/warrior.cpp:436 — same signal
		                               // MC2_BRAIN_FREEZE_TRACE reads
		const float olWin = soakWinAfterSec();

		if (scenarioTime < olWin * 0.1f)
		{
			s_olMoveIssued = s_olAttackIssued = s_olSummaryDone = false;
			s_olIssuedMove = 0;
		}

		// PHASE A (t = 20% win): COMMANDER-origin unaimed attack per armed
		// home mech at the nearest live enemy mover. Unaimed (-1) — the #9
		// aimed leg is MC2_AIMED_FIRE_FIXTURE's job. Runs FIRST so the
		// attack leg (hits/nofire evidence) accrues while the move phase
		// below later replaces the combat order for a clean displacement
		// window.
		if (!s_olAttackIssued && scenarioTime > olWin * 0.2f && !scenarioResult)
		{
			for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
			{
				BattleMech* pM = ObjectManager->getMech(mi);
				if (!pM || pM->isDisabled() || pM->isDestroyed()) continue;
				if (!Team::home || pM->getTeamId() != Team::home->id) continue;
				MechWarriorPtr olPilot = pM->getPilot();
				if (!olPilot || pM->numWeapons <= 0 || !pM->canFireWeapons())
					continue;
				MoverPtr olEnemy = nullptr;
				float olBest = 1e30f;
				for (long ei = 0; ei < ObjectManager->getNumMovers(); ++ei)
				{
					MoverPtr pE = ObjectManager->getMover(ei);
					if (!pE || pE->isDisabled() || pE->isDestroyed()) continue;
					if (pE->getTeamId() == Team::home->id) continue;
					const float d = pM->distanceFrom(pE->getPosition());
					if (d < olBest) { olBest = d; olEnemy = pE; }
				}
				if (!olEnemy) continue;
				olPilot->orderAttackObject(false, ORDER_ORIGIN_COMMANDER,
					olEnemy, ATTACK_TO_DESTROY, ATTACKMETHOD_RANGED,
					FIRERANGE_OPTIMAL, -1, -1, TACORDER_PARAM_PURSUE);
				printf("[ORDER_FIXTURE] kind=attack wid=%lu tgt=%lu t=%.1f\n",
					static_cast<unsigned long>(pM->getWatchID()),
					static_cast<unsigned long>(olEnemy->getWatchID()),
					static_cast<float>(scenarioTime));
			}
			fflush(stdout);
			s_olAttackIssued = true;
		}

		// PHASE M (t = 50% win): COMMANDER-origin move order per home mech,
		// 300 m toward map center; record start position per watch-ID.
		// Replaces the attack order — displacement is measured from HERE
		// to the 0.9*win summary (a clean 0.4*win window).
		if (!s_olMoveIssued && s_olAttackIssued &&
			scenarioTime > olWin * 0.5f && !scenarioResult)
		{
			extern float metersPerWorldUnit;
			const float stepWU = (metersPerWorldUnit > 0.0f)
				? (mc2::gate::get<"MC2_FIXTURE_ORDER_MOVE_M">() / metersPerWorldUnit) : mc2::gate::get<"MC2_FIXTURE_ORDER_MOVE_M">();
			for (int mi = 0; mi < ObjectManager->numMechs &&
				 s_olIssuedMove < kOrderLivenessCap; ++mi)
			{
				BattleMech* pM = ObjectManager->getMech(mi);
				if (!pM || pM->isDisabled() || pM->isDestroyed()) continue;
				if (!Team::home || pM->getTeamId() != Team::home->id) continue;
				MechWarriorPtr olPilot = pM->getPilot();
				if (!olPilot) continue;
				glm::vec3 olPos = pM->getPosition();
				glm::vec3 olGoal = olPos;
				const float olLen = sqrtf(olPos.x * olPos.x + olPos.y * olPos.y);
				if (olLen > 1.0f)
				{
					olGoal.x = olPos.x - (olPos.x / olLen) * stepWU;
					olGoal.y = olPos.y - (olPos.y / olLen) * stepWU;
				}
				else
					olGoal.x = olPos.x + stepWU;
				olPilot->orderMoveToPoint(false, true, ORDER_ORIGIN_COMMANDER,
					olGoal, -1, TACORDER_PARAM_RUN);
				// Keep a terrain-target queue pending while the move is active so the
				// save/load fixture exercises the process-local compact-id boundary.
				GameObjectPtr queueTarget = nullptr;
				for (long oi = 1; oi <= ObjectManager->getMaxObjects(); ++oi) {
					auto* candidate = ObjectManager->get(oi);
					if (candidate && !candidate->isMover() && candidate->getPartId() >= MIN_TERRAIN_PART_ID &&
						(!queueTarget || candidate->getPartId() > queueTarget->getPartId())) queueTarget = candidate;
				}
				if (queueTarget) {
					olPilot->clearTacOrderQueue();
					TacticalOrder queued;
					queued.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_OBJECT);
					queued.targetWID = queueTarget->getWatchID();
					queued.pack(nullptr, nullptr);
					olPilot->addQueuedTacOrder(queued);
					queued.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_POINT);
					queued.attackParams.targetPoint = olGoal;
					queued.pack(nullptr, nullptr);
					olPilot->addQueuedTacOrder(queued);
					queued.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_GUARD);
					queued.targetWID = pM->getWatchID();
					queued.pack(nullptr, nullptr);
					olPilot->addQueuedTacOrder(queued);
					olPilot->setTacOrderQueueLooping(true);
					olPilot->getNextQueuedTacOrder(&queued); // Save a nonzero loop cursor.
					olPilot->lockTacOrderQueue();
				}
				s_olEntries[s_olIssuedMove].wid = pM->getWatchID();
				s_olEntries[s_olIssuedMove].startPos = olPos;
				s_olEntries[s_olIssuedMove].goalPos = olGoal;
				s_olEntries[s_olIssuedMove].issuedAt = scenarioTime;
				s_olEntries[s_olIssuedMove].arrived = false;
				s_olEntries[s_olIssuedMove].maxGlobalSteps = 0;
				mc2::fmt::print_out("[ORDER_FIXTURE] kind=route wid={:d} start_x_wu={:.3f} start_y_wu={:.3f} goal_x_wu={:.3f} goal_y_wu={:.3f} distance_m={:.3f}\n",
					mc2::fmt::as_u64(static_cast<unsigned long long>(pM->getWatchID())),
					olPos.x, olPos.y, olGoal.x, olGoal.y, pM->distanceFrom(olGoal));
				++s_olIssuedMove;
				printf("[ORDER_FIXTURE] kind=move wid=%lu tgt=0 t=%.1f\n",
					static_cast<unsigned long>(pM->getWatchID()), static_cast<float>(scenarioTime));
			}
			fflush(stdout);
			s_olMoveIssued = true;
		}

		for (int i = 0; i < s_olIssuedMove; ++i) {
			auto& entry = s_olEntries[i];
			GameObjectPtr obj = ObjectManager->getByWatchID(entry.wid);
			if (entry.arrived || !obj || obj->isDestroyed() || obj->isDisabled()) continue;
			if (obj->isMover()) {
				if (auto* pilot = static_cast<Mover*>(obj)->getPilot())
					entry.maxGlobalSteps = std::max(entry.maxGlobalSteps, static_cast<int>(pilot->getMoveNumGlobalSteps()));
			}
			const float remaining = obj->distanceFrom(entry.goalPos);
			if (remaining > 10.0f) continue;
			entry.arrived = true;
			const auto pos = obj->getPosition();
			mc2::fmt::print_out("[ORDER_FIXTURE] kind=arrival wid={:d} travel_s={:.3f} moved_m={:.3f} to_goal_m={:.3f} x_wu={:.3f} y_wu={:.3f} global_steps={:d} recon={:d} global_solve_ms={:.3f} global_solve_max_ms={:.3f} local_solve_ms={:.3f}\n",
				mc2::fmt::as_u64(static_cast<unsigned long long>(entry.wid)), static_cast<double>(scenarioTime - entry.issuedAt),
				obj->distanceFrom(entry.startPos), remaining, pos.x, pos.y, entry.maxGlobalSteps,
				static_cast<int>(g_moveReconEnabled), g_moveRecon_astar_global_ns / 1.0e6,
				g_moveRecon_max_astar_global_ns / 1.0e6, g_moveRecon_astar_local_ns / 1.0e6);
			fflush(stdout);
		}

		// PHASE S (t = 90% win): self-asserting summary. moved = net
		// displacement >= 25 m since the move order; dead/disabled units
		// are exempt (destroyed-before-compliance is not a dead-end);
		// non-movers with a classified path refusal count as refused
		// (classified counts as signal, not silence).
		if (!s_olSummaryDone && s_olMoveIssued && scenarioTime > olWin * 0.9f)
		{
			int olMoved = 0, olRefused = 0, olStalled = 0;
			for (int i = 0; i < s_olIssuedMove; ++i)
			{
				GameObjectPtr obj =
					ObjectManager->getByWatchID(s_olEntries[i].wid);
				if (!obj || obj->isDestroyed() || obj->isDisabled())
					continue;   // dead-unit exemption
				const float distM =
					obj->distanceFrom(s_olEntries[i].startPos);   // meters
				mc2::fmt::print_out("[ORDER_FIXTURE] kind=progress wid={:d} moved_m={:.0f} to_goal_m={:.0f}\n",
					mc2::fmt::as_u64(static_cast<unsigned long long>(s_olEntries[i].wid)), static_cast<double>(distM),
					static_cast<double>(obj->distanceFrom(s_olEntries[i].goalPos)));
				if (distM >= 25.0f)
					++olMoved;
				else if (LastMoveCalcErr != 0)
					++olRefused;
				else
					++olStalled;
			}
			const bool olPass = (s_olIssuedMove > 0) && (olStalled == 0);
			printf("ORDER_LIVENESS_SUMMARY issued_move=%d moved=%d "
				"refused=%d stalled=%d verdict=%s\n",
				s_olIssuedMove, olMoved, olRefused, olStalled,
				olPass ? "PASS" : "FAIL");
			fflush(stdout);
			s_olSummaryDone = true;
		}
	}
}

// Promote a deployed pilot on the soak missions.
void Mission::updateSoakPilotPromote() {
	// --- MC2_SOAK_PILOT_PROMOTE ------------------------------------------
	// Fires on campaign mission numbers 2 and 5 (via getCurrentMissionNum).
	// Grants enough gunnery to cross the next rank threshold, then runs
	// promotePilot() exactly as pilotreviewarea.cpp:215 does.
	// Throttle: getCurrentMissionNum() % 10 == 2 or == 5 — roughly 2
	// firings per 10-mission stretch without hammering every mission.
	if (s_soakPilotPromote && s_soakAutoWin)
	{
		// Re-arm at start of each mission, and count missions played once each
		// (edge-triggered) so the throttle does not depend on
		// getCurrentMissionNumber() semantics (not a clean 1..N per campaign).
		static int  s_soakMissionCount   = 0;
		static bool s_soakMissionCounted = false;
		if (scenarioTime < soakWinAfterSec() * 0.5f)
		{
			if (!s_soakMissionCounted) { ++s_soakMissionCount; s_soakMissionCounted = true; }
			s_soakPromoteFired = false;
		}
		else
		{
			s_soakMissionCounted = false;
		}

		// Throttle: ~twice per campaign — the 2nd and 5th missions played.
		const bool throttlePass =
			(s_soakMissionCount == 2 || s_soakMissionCount == 5);

		const float promoteAt = soakWinAfterSec() * 0.7f;
		if (!s_soakPromoteFired && throttlePass &&
			scenarioTime > promoteAt && !scenarioResult)
		{
			LogisticsPilot* lPilot = nullptr;
			const char* pilotName = "(none)";
			for (int wi = 1; wi <= MechWarrior::numWarriors && !lPilot; ++wi)
			{
				MechWarriorPtr mw = MechWarrior::warriorList[wi];
				if (!mw) continue;  // getPilot() below gates to real deployed pilots
				LogisticsPilot* lp =
					LogisticsData::instance
						? LogisticsData::instance->getPilot(mw->getName())
						: nullptr;
				if (!lp) continue;
				lPilot = lp;
				pilotName = lp->getName().c_str();
			}

			if (lPilot)
			{
				const long rankBefore = lPilot->getRank();
				// soakAddGunnery bumps both lifetime gunnery and newGunnery
				// delta so promotePilot() sees a mission gain large enough
				// to cross a rank threshold from any starting rank.
				// 11 pts guarantees crossing regardless of start (worst case
				// GREEN->REGULAR needs avg 50+epsilon; 11 pts covers all).
				lPilot->soakAddGunnery(11.0f);
				bool promoted = lPilot->promotePilot();
				const long rankAfter = lPilot->getRank();
				printf("[SOAK] pilot-promote pilot=%s rankBefore=%ld rankAfter=%ld ok=%d\n",
					pilotName, rankBefore, rankAfter, promoted ? 1 : 0);
				fflush(stdout);
			}
			else
			{
				printf("[SOAK] pilot-promote no-deployed-pilot\n");
				fflush(stdout);
			}
			s_soakPromoteFired = true;
		}
	}
}

#include "battle_bench_fixture.h"
// One pass of the mission update, in the legacy order. Two parts:
//   frame part: dev hotkeys, interface, camera, terrain, weather, waypoints, terrain geometry (render frame time);
//   sim part:   clock, ABL timers, path manager, objects, sensors, collisions, brains, termination (one tick).
// The legacy pass (tickMode false) runs both under one frameLength. In tick mode the first tick of a frame runs both, with
// frameLength switched between the render frame and the tick length around each step, and further ticks run the sim part
// alone. Returns true when the mission brain ended the update (earlyResult).
bool Mission::updatePass(bool tickMode, bool runFrame, bool runSim, bool advanceTime, long& earlyResult)
{
	const float renderLength = frameLength;
    mc2::diagnostic::TickCost tickCost(s_misSplit && runSim);
	auto simLength = [&] { tickCost.begin(); if (tickMode) frameLength = mc2::sim::kTickSecondsF; };
	auto frameLen = [&] { tickCost.end(); frameLength = renderLength; };

	// Every opened tick must complete, including the mission brain's 9999 return.
	std::optional<mc2::sim::SimulationOrderCompletion<Mission>> orderTickCompletion;
	std::optional<mc2::world_trace::TickScope> worldOrderTick;
	std::optional<mc2::battlebench::Tick> benchTick;   // one battle-bench sample per sim tick
	if (runSim)
	{
		benchTick.emplace();
		mc2BattleBenchObserve();
		tickCost.begin();
		orderTickCompletion.emplace(*this);
		turn++;
		worldOrderTick.emplace(turn);

		memset(ObjectManager->moverLineOfSightTable, -1, ObjectManager->maxMovers*ObjectManager->maxMovers);

#ifdef LAB_ONLY
		MCTimeLOSCalc = 0;
		MCTimeAnimationCalc = 0;
#endif
		simLength();
		if (timerManager)
			timerManager->update();
	}

	if (runFrame)
	{
		frameLen();
		updateDevHotkeys();
	}
	if (runSim)
	{
		simLength();
		updateTickClock(advanceTime);
	}
	if (runSim)
		updateSaveFixtures();
	ZoneScopedN("GameLogic.Mission.Update");
	if (runFrame)
	{
		frameLen();
		updateFrontEnd();
	}
	if (runSim)
	{
		simLength();
		updatePathManager();
	}
	if (runFrame)
	{
		frameLen();
		updateTerrainGeometry();
	}
	if (runSim)
	{
		simLength();
		updateObjectManager();
		updateSimSystems();

		if (missionBrain)
		{
			if ( !mc2SimTickFrozen(missionInterface) || MPlayer )
			{
				mc2_abl_trace::FireScope ablFire("mission", -1);
				long ablStmts;
				{ ZoneScopedN("GameLogic.AI.BrainExecute"); ablStmts = missionBrain->execute(); }
				ablFire.record(ablStmts, NumStateTransitions);
				long missionResult = missionBrain->getInteger();
				if (missionResult == 9999)
				{
					terminationResult = 9999;
					if (auto* observation = objectiveObservation()) {
						observation->Emit("resolve", 0, std::format("{{\"scope\":\"mission\",\"stage\":\"final\",\"old\":{},\"new\":9999}}", observation->finalResult));
						observation->finalResult = 9999;
					}
					orderTickCompletion.reset();
					frameLen();
					earlyResult = terminationResult;
					return true;
				}
				if (!MPlayer)
					terminationResult = missionResult;
				// A forced outcome must survive the mission brain's result.
				if (forcedTerminationResult != mis_PLAYING) {
					terminationResult = forcedTerminationResult;
					terminationCounterStarted = true;
				}
			}
		}

		{ ZoneScopedN("GameLogic.Mission.RenderSnapshots"); ObjectManager->extractRenderSnapshots(); }
		updateTraceHooks();
		updateTermination();
	}
	if (runFrame)
	{
		frameLen();
		updateCheatsAndFrameRate();
	}
	if (runSim)
	{
		simLength();
		updateSoakAutoWin();
		updateSoakKillEnemy();
		updateOrderLiveness();
		updateSoakPilotPromote();
	}
	// Queue completion belongs to the tick, including its fixed frame length.
	orderTickCompletion.reset();
	frameLen();

#ifdef LAB_ONLY
	MCTimeMissionTotal 		= MCTimeTerrainUpdate +
								MCTimeCameraUpdate +
								MCTimeWeatherUpdate +
								MCTimePathManagerUpdate +
								MCTimeRunBrainUpdate +
								MCTimePath1Update +
								MCTimePath2Update +
								MCTimePath3Update +
								MCTimePath4Update +
								MCTimePath5Update +
								MCTimeCalcPath1Update +
								MCTimeCalcPath2Update +
								MCTimeCalcPath3Update +
								MCTimeCalcPath4Update +
								MCTimeCalcPath5Update +
								MCTimeCalcGoal1Update +
								MCTimeCalcGoal2Update +
								MCTimeCalcGoal3Update +
								MCTimeCalcGoal4Update +
								MCTimeCalcGoal5Update +
								MCTimeCalcGoal6Update +
								MCTimeTerrainGeometry +
								MCTimeInterfaceUpdate +
								MCTimeCraterUpdate +
								MCTimeTXMManagerUpdate +
								MCTimeSensorUpdate +
								MCTimeLOSUpdate +
								MCTimeCollisionUpdate +
								MCTimeMissionScript+
								MCTimeTerrainObjectsUpdate +
								MCTimeMechsUpdate +
								MCTimeVehiclesUpdate +
								MCTimeTurretsUpdate +
								MCTimeAllElseUpdate;
#endif
	return false;
}

namespace {
mc2::sim::TickClock s_tickClock;
long s_turnAfterLastPass = -1;

// G7 overlap: the kick at the tail of frame N polls the tick clock and runs the banked ticks' sim part on the sim worker;
// the next Mission::update finds what happened here and runs only the frame part.
struct OverlapPre {
	bool valid = false;
	const Mission* owner = nullptr;
	int ticks = 0;
	bool ended = false;
	long earlyResult = 0;
	long turn = 0;   // Mission::turn after the worker finished
};
OverlapPre s_pre;
}

// Tail of frame N, main thread, after the render CPU work is done: bank frame N+1's ticks and start them on the sim worker.
// Not eligible (the serial path runs them at the top of the next frame): first frame, multiplayer, paused or frozen,
// a pending in-mission save or load.
void Mission::overlapKick()
{
	if (!mc2::gate::get<"MC2_SIM_OVERLAP">() || !mc2::gate::get<"MC2_DETERMINISTIC_RNG">() || !active || s_pre.valid)
		return;
	if (MPlayer || mc2SimTickFrozen(missionInterface) || loadInMissionSave || saveInMissionSave ||
	    LastTimeGetTime == 0xffffffff || turn != s_turnAfterLastPass)
		return;
	const float renderLength = frameLength;
	const double realSeconds = pollRealFrameSeconds(renderLength);
	const int ticks = s_tickClock.advance(realSeconds, simSpeedGetMultiplier());
	s_pre.valid = true;
	s_pre.owner = this;
	s_pre.ticks = ticks;
	s_pre.turn = turn;
	if (ticks == 0)
		return;
	mc2::overlap::runOnWorker([this, ticks, renderLength] {
		frameLength = renderLength;
		long early = 0;
		bool ended = false;
		for (int tick = 0; tick < ticks; ++tick)
		{
			s_extraTickInFrame = tick > 0;
			if (tick > 0)
				++g_mc2FrameCounter;
			ended = updatePass(true, false, true, true, early);
			s_extraTickInFrame = false;
			if (ended || loadInMissionSave)
				break;
		}
		s_pre.ended = ended;
		s_pre.earlyResult = early;
		s_pre.turn = turn;
		if (!ended)
			s_turnAfterLastPass = turn;
	});
}

namespace {
void overlapKickHook() { if (mission) mission->overlapKick(); }
void overlapJoinHook() { mc2::overlap::waitWorker(); mc2::textures::commitTickTextures(); }
const bool s_overlapHooksInstalled = (mc2::overlap::installHooks({overlapKickHook, overlapJoinHook}), true);
}

// Per-frame mission update. Normal play runs on the fixed 30 Hz sim tick (code/sim_tick_clock.h): the frame's
// real time x game speed is banked, whole ticks run out of the bank, and a frame that banks no tick only redraws the
// unchanged sim state. A paused or frozen sim and multiplayer keep the old one-pass-per-frame update.
long Mission::update (void)
{
	ZoneScopedN("Mission::update");
	if (s_misSplit) {
		if (!s_misAtexit) { s_misAtexit = true; std::atexit(misEmit); }
		++s_misFrames;
	}
	MisScope _msTotal(MS_TOTAL);  // whole Mission::update; TOTAL - sum(subcalls) = unbracketed
	mc2::overlap::waitWorker();   // a kicked tick is always finished by the main loop's join; this is the safety net
	const OverlapPre pre = s_pre;
	s_pre = OverlapPre{};
	if (active)
	{
		long earlyResult = 0;
		const bool usePre = pre.valid && pre.owner == this && pre.turn == turn;
		if (turn != s_turnAfterLastPass && !usePre)
			s_tickClock.reset();   // a new mission or a loaded save: no banked time carries over
		const float renderLength = frameLength;   // DoGameLogic: this render frame's length (1/30 s per virtual frame under the harness clock)

		if (MPlayer || mc2SimTickFrozen(missionInterface))
		{
			s_tickClock.reset();
			pollRealFrameSeconds(renderLength);   // rebase the wall credit so a resume does not burst
			// Multiplayer runs one tick per frame, paused or frozen runs the frame part alone: no wall time reaches the sim.
			if (updatePass(true, true, MPlayer != nullptr, true, earlyResult))
				return earlyResult;
			updatePresentationOnly(false, 1.0f);
		}
		else if (usePre)
		{
			// G7: this frame's ticks already ran on the sim worker during the previous frame's tail; only the frame part is left.
			if (updatePass(true, true, false, false, earlyResult))
				return earlyResult;
			if (pre.ended)
				return pre.earlyResult;
			updatePresentationOnly(false, static_cast<float>(s_tickClock.alpha()));
		}
		else
		{
			const bool firstFrame = (LastTimeGetTime == 0xffffffff);
			const double realSeconds = pollRealFrameSeconds(renderLength);
			const int ticks = firstFrame ? 1 : s_tickClock.advance(realSeconds, simSpeedGetMultiplier());
			if (ticks == 0)
			{
				// No tick is due: the frame part runs (camera and UI stay at render rate), the sim stands still.
				if (updatePass(true, true, false, false, earlyResult))
					return earlyResult;
				updatePresentationOnly(false, static_cast<float>(s_tickClock.alpha()));
			}
			for (int tick = 0; tick < ticks; ++tick)
			{
				// One logic pass per frame-counter value: per-frame-keyed sim paths (animation advance, the brain tick
				// accumulator, light-row stamps) see each tick as its own frame.
				s_extraTickInFrame = tick > 0;
				if (tick > 0)
					++g_mc2FrameCounter;
				const bool ended = updatePass(true, tick == 0, true, !firstFrame, earlyResult);
				s_extraTickInFrame = false;
				if (ended)
					return earlyResult;
				if (loadInMissionSave) break;   // let the outer loop load before another tick
			}
			if (ticks > 0)
				updatePresentationOnly(false, static_cast<float>(s_tickClock.alpha()));
		}
		s_turnAfterLastPass = turn;
	}

	if (auto* observation = objectiveObservation(); observation && scenarioResult != mis_PLAYING &&
	    observation->finalResult != static_cast<int>(scenarioResult)) {
		observation->Emit("resolve", 0, std::format("{{\"scope\":\"mission\",\"stage\":\"final\",\"old\":{},\"new\":{}}}",
			observation->finalResult, scenarioResult));
		observation->finalResult = static_cast<int>(scenarioResult);
	}
	return scenarioResult;
}
