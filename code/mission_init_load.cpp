#include "brain_execution_policy.h"
#include "../GameOS/gameos/gos_render.h"
#include "../GameOS/gameos/vk_render_infra.h"
#include "../GameOS/gameos/diagnostic_trace.h"
#include "../GameOS/gameos/gos_static_prop_registry.h"
#include <atomic>
#include <chrono>
#include "../GameAdapters/SkyRenderAdapter.h"
// Mission: the second half of the Mission::init load sequence -- warriors and brains, movers and parts,
// terrain objects, commanders, the HUD and the geometry finalize. The first half and the orchestrator are in mission_init.cpp.
#include "mission_internal.h"
#include "missiongui.h"
#include "mission_init_frame.h"
#include "static_prop_prewarm.h"
#include "spawn_tree_clear.h"
#include "render_resource_owners.h"
#include "mc2fmt.h"
#include "../mclib/content_ir.h"
#include "../mclib/texture_assets.h"
#include "../mclib/mlr/mlrtexturepool.hpp"

#include "content_error.h"
#include "../GameOS/gameos/load_scope.h"

using enum MissionLoadType;

// Read the warrior count and set up the brain-load counters.
void Mission::initWarriorsHeader(MissionInitFrame& f) {
	auto& missionName = f.missionName; auto& result = f.result; auto& numWarriors = f.numWarriors;
	auto& loadBrainParameters = f.loadBrainParameters; auto& soakBrainsLoaded = f.soakBrainsLoaded; auto& soakBrainsFailed = f.soakBrainsFailed;
	loadProgress = 41.0f;

	MechWarrior::setup();

	result = missionFile->seekBlock("Warriors");
	// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: no [Warriors] block in mission .fit: %s",
		missionName ? missionName : "(null)");

	result = missionFile->readIdULong("NumWarriors",numWarriors);
	// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
	if (!MC2_VERIFY(result == NO_ERR, "Mission::init: [Warriors] NumWarriors read failed"))
		numWarriors = 0;	// log mode: degrade instead of looping over an uninitialized count
	// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1): the
	// 1-based warrior loops below index warriorList[numWarriors] -- a
	// file-supplied count >= MAX_WARRIORS reads past the static array.
	if (!MC2_VERIFY(static_cast<long>(numWarriors) < MAX_WARRIORS,
			"Mission::init: NumWarriors %lu exceeds MAX_WARRIORS-1 (%d) -- bounding",
			numWarriors, MAX_WARRIORS - 1))
		numWarriors = MAX_WARRIORS - 1;

	loadBrainParameters = (result == NO_ERR);
	// CRASH-SOAK harness: tally how many mission AI brains (e.g. "magicx" on
	// MCO missions) load vs fail, so the soak runner can confirm AI is active.
	soakBrainsLoaded = 0;
	soakBrainsFailed = 0;
}

// Load each warrior and its brain.
void Mission::loadWarriors(MissionInitFrame& f) {
	bool loggedEmptyBrain = false;
	auto& missionName = f.missionName; auto& result = f.result; auto& numErrors = f.numErrors;
	auto& numLinesProcessed = f.numLinesProcessed; auto& numWarriors = f.numWarriors; auto& soakBrainsLoaded = f.soakBrainsLoaded;
	auto& soakBrainsFailed = f.soakBrainsFailed;
	if (numWarriors)
	{
		for (long i = 1; i <= numWarriors; i++)
		{
			graphics::pump_loading_events();
			char warriorName[12];
			mc2::fmt::to_buf(warriorName, "Warrior{:d}", mc2::fmt::as_i32(i));
			
			result = missionFile->seekBlock(warriorName);
			Assert(result == NO_ERR, i, " Could not find Warrior Number Block ");

			char warriorFile[100];
			result = missionFile->readIdString("Profile", warriorFile, 99);
			Assert(result == NO_ERR, 0, " Could not find Warrior Profile in Warrior Number Block ");
			// QUICKSTART-PILOT-DROP-RECON-1: an empty Profile means the warrior
			// fit never loads and the pilot degrades to the silent "Bubba"
			// default (portrait 0, no radio). Editor re-saves have shipped
			// blanked profiles before — fail LOUD, not silent.
			MC2_VERIFY(warriorFile[0] != '\0',
				"Mission::init: %s has EMPTY Profile in %s — pilot will default to Bubba (blanked by an editor re-save?)",
				warriorName, missionName);

			MechWarriorPtr pilot = MechWarrior::newWarrior();
			if (!pilot)
				STOP(("Too many pilots in this mission!"));
			
			FullPathFileName pilotFullFileName;
			pilotFullFileName.init(warriorPath, warriorFile, ".fit");
			
			FitIniFile pilotFile;
		
			long result = pilotFile.open(pilotFullFileName);
			// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: warrior profile open failed: %s",
				static_cast<const char*>(pilotFullFileName));
			result = pilot->init(&pilotFile);
			// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: warrior init failed: %s",
				static_cast<const char*>(pilotFullFileName));
			
			pilotFile.close();
			
			char moduleName[128];
			result = missionFile->readIdString("Brain", moduleName, 127);
			// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: %s missing Brain key", warriorName);
			
			//------------------------------------------------------------
			// For now, all mplayer brains are pbrain. Need to change when
			// we allow ai brains in mplayer...
			long numErrors, numLinesProcessed;
			FullPathFileName brainFileName;
			if (MPlayer) {
				pilot->setBrainName("pbrain");
				brainFileName.init(warriorPath, "pbrain", ".abl");
				}
			else {
				pilot->setBrainName(moduleName);
				brainFileName.init(warriorPath, moduleName, ".abl");
			}
			
			// BRAIN-EMPTY-BRAIN-SKIP-1: inline Brain{} (TechScript/Enhanced) missions leave the
			// warrior "Brain" field empty — the brain lives in the mission.fit Brain{} block, not a
			// .abl file. Without a guard the empty name builds "warriors/.abl" and ABLi_preProcess
			// STOPs (FATAL: missing ABL), so the mission never reaches the inline-Brain consumers
			// (e.g. BRAIN-MISSIONFIT-OPORD-CONSUMER-1 below). Gate MC2_BRAIN_INLINE_EMPTY_SKIP
			// (always on). Stock missions always name a brain, so stock is byte-identical — the
			// guard only fires on an empty, non-MPlayer name.
			if (!MPlayer && moduleName[0] == '\0')
			{
				pilot->setBrainName("");
				if (!loggedEmptyBrain) {
					loggedEmptyBrain = true;
					mc2::fmt::print_err("[BRAIN_EMPTY_SKIP] mission={} (empty ABL brain name)\n", mc2::fmt::safe(missionName));
				}
			}
			else
			{
				long moduleHandle = ABLi_preProcess(brainFileName, &numErrors, &numLinesProcessed);
				if (moduleHandle >= 0 && numErrors == 0)
					soakBrainsLoaded++;
				else
				{
					{ soakBrainsFailed++; mc2::content::error(mc2::content::Kind::MissionData, static_cast<const char*>(brainFileName), moduleHandle < 0 ? "brain ABL failed to load; unit has no brain (idle)" : "brain ABL compiled with errors; unit brain unusable"); }
				}

#ifdef _DEBUG
				long error =
#endif
					pilot->setBrain(moduleHandle);
				gosASSERT(error == 0);
			}
		}
	}
}

// Arm the soak auto-win when requested or when a brain failed to load.
void Mission::soakAutowinCheck(MissionInitFrame& f) {
	auto& soakBrainsLoaded = f.soakBrainsLoaded; auto& soakBrainsFailed = f.soakBrainsFailed;
	if (mc2::gate::get<"MC2_SOAK_AUTOWIN">() || soakBrainsFailed > 0)
	{
		mc2::fmt::print_out("[SOAK] abl mission brains loaded={:d} failed={:d}\n",
			mc2::fmt::as_i32(soakBrainsLoaded), mc2::fmt::as_i32(soakBrainsFailed));
		fflush(stdout);
	}
}

// Load the per-warrior brain parameters.
void Mission::loadBrainParamsPhase(MissionInitFrame& f) {
	auto& result = f.result; auto& numWarriors = f.numWarriors; auto& loadBrainParameters = f.loadBrainParameters;
	if (loadBrainParameters) {
		for (unsigned long i = 1; i <= numWarriors; i++) {
			if (MechWarrior::warriorList[i] && !MechWarrior::warriorList[i]->getBrain())
				continue;
			// Exhausting the warrior slots can leave a missing pilot.
			if (!MC2_VERIFY(MechWarrior::warriorList[i] != nullptr,
					"Mission::init: warriorList[%lu] is NULL (loadBrainParameters)", i))
				continue;	// log mode: skip the missing warrior
			result = MechWarrior::warriorList[i]->loadBrainParameters(missionFile, i);
		}

	}
}

// Precedence and OPORD population must agree on eligibility.
bool missionFitOpordApplies(const MissionFitBrain& brain, unsigned long numWarriors, bool enabled) {
	if (!enabled || brain.warriorIndex < 1 || static_cast<unsigned long>(brain.warriorIndex) > numWarriors ||
		brain.warriorIndex >= MAX_WARRIORS || !MechWarrior::warriorList[brain.warriorIndex]) return false;
	BrainArchetypeDefaults defaults;
	brainArchetypeLookup(brain.archetype, defaults);
	const auto type = brainOpordTypeId(brain.primary.type);
	return !defaults.playerControlled && type != 9 && type != 255;
}

// Load the mission-fit order sets for the enhanced brains.
void Mission::loadMissionFitOpord(MissionInitFrame& f) {
	auto& missionFileName = f.missionFileName; auto& numWarriors = f.numWarriors;
	const bool opordEnabled = mc2::brainpolicy::techBrain();
	File authoredFile;
	if (authoredFile.open(missionFileName) == NO_ERR) {
		authoredMissionText.resize(authoredFile.getLength());
		authoredFile.read(reinterpret_cast<MemoryPtr>(authoredMissionText.data()), static_cast<long>(authoredMissionText.size()));
	}
	// BRAIN-MISSIONFIT-OPORD-CONSUMER-1: drive declarative patrol from the inline Brain{}
	// PrimaryOPORD blocks in mission.fit (carver_v_enhanced format). FitIniFile cannot see
	// brace-blocks, so raw-scan the mission.fit text via the File class (VFS/mod-overlay aware —
	// NOT fopen). Gate MC2_BRAIN_MISSIONFIT_OPORD (default OFF). Populates the existing
	// MechBrainRuntime patrol structures (BRAIN-OPORD-COREPATROL-1); the brain bundle drives them
	// (the patrol FIELDS persist through later _ai.fit/tactic loaders, which only touch mode).
	if (opordEnabled) {
		File mfFile;
		if (mfFile.open(missionFileName) == NO_ERR) {
			unsigned long mfSz = mfFile.getLength();
			char* mfBuf = static_cast<char*>(malloc(mfSz + 1));
			if (mfBuf) {
				mfFile.read(reinterpret_cast<MemoryPtr>(mfBuf), static_cast<long>(mfSz));
				mfBuf[mfSz] = '\0';
				MissionFitBrain* mfBrains = static_cast<MissionFitBrain*>(malloc(sizeof(MissionFitBrain) * 128));
				int mfN = mfBrains ? parseMissionFitBrains(mfBuf, mfBrains, 128) : 0;
				int mfPatrol = 0, mfGuard = 0, mfSentry = 0, mfEscort = 0, mfOther = 0;
				for (int r = 0; r < mfN; ++r) {
					const MissionFitBrain& b = mfBrains[r];
					if (!missionFitOpordApplies(b, numWarriors, opordEnabled)) continue;
					uint8_t pType = brainOpordTypeId(b.primary.type);
					BrainArchetypeDefaults ad; brainArchetypeLookup(b.archetype, ad);  // archetype presets; switches override
					MechWarriorPtr w = MechWarrior::warriorList[b.warriorIndex];
					if (!w->getBrainRuntime()) w->setBrainRuntimeMode(BrainRuntimeMode::Enhanced);
					MechBrainRuntime* rt = w->getBrainRuntime();
					if (!rt) continue;
					// EngageRadius = per-unit COMMITMENT radius: the team SHARES the contact (all know the
					// enemy exists) but a unit only attacks once the threat is within its own EngageRadius,
					// so distant units keep patrolling/holding instead of the whole map swarming one sighting.
					rt->engageRadius         = (b.engageRadius >= 0) ? b.engageRadius : ad.engageRadius;
					rt->swAttackerHelpRadius = (b.attackerHelpRadius >= 0) ? b.attackerHelpRadius : ad.attackerHelpRadius;
					rt->swDefenderHelpRadius = (b.defenderHelpRadius >= 0) ? b.defenderHelpRadius : ad.defenderHelpRadius;
					rt->swRequestHelp        = (b.requestHelp  >= 0) ? static_cast<int8_t>(b.requestHelp)  : static_cast<int8_t>(ad.requestHelp);
					rt->swReturnToPost       = (b.returnToPost >= 0) ? static_cast<int8_t>(b.returnToPost) : static_cast<int8_t>(ad.returnToPost);
					rt->swWakeOnAttack       = static_cast<int8_t>(b.wakeOnAttack);
					rt->swPoweredDown        = static_cast<int8_t>(b.poweredDown);
					rt->opordType[0] = pType;
					rt->opordType[1] = brainOpordTypeId(b.secondary.type);
					rt->opordType[2] = brainOpordTypeId(b.tertiary.type);
					rt->opordCursor  = 0;
					// Load weighted Tactics (MC2_TACTIC_WEIGHTS selects per tick; _WEIGHTS_B applies to attacks).
					for (int ti = 0; ti < b.tacticCount; ++ti) {
						int tidx = brainTacticNameToIndex(b.tacticName[ti]);
						if (tidx >= 0 && tidx < tactic_table::kCount) rt->tacticWeights[tidx] = b.tacticWeight[ti];
					}
					if (w->getVehicle()) {
						glm::vec3 vp = w->getVehicle()->getPosition();
						rt->postPos[0] = vp.x; rt->postPos[1] = vp.y; rt->postPos[2] = vp.z; rt->postSet = true;
					}
					switch (pType) {
						case 0: {   // Patrol
							int wc = (b.primary.waypointCount > 8) ? 8 : b.primary.waypointCount;
							if (wc <= 0) { rt->guardHold = true; ++mfGuard; break; }
							for (int wi = 0; wi < wc; ++wi) {
								glm::vec3 wp; wp.x = b.primary.waypoints[wi].x; wp.y = b.primary.waypoints[wi].y; wp.z = 0.0f;
								float z = land ? land->getTerrainElevation(wp) : 0.0f;
								rt->patrolWaypoints[wi][0] = b.primary.waypoints[wi].x;
								rt->patrolWaypoints[wi][1] = b.primary.waypoints[wi].y;
								rt->patrolWaypoints[wi][2] = z;
							}
							rt->patrolWaypointCount = static_cast<uint8_t>(wc);
							rt->patrolWaypointIndex = 0;
							rt->patrolLoop   = b.primary.loop;
							rt->patrolActive = true;
							++mfPatrol; break;
						}
						case 1:     // Guard
							rt->guardHold = true; ++mfGuard; break;
						case 3:     // Sentry
							rt->guardHold = true; rt->sentryAsleep = (rt->swPoweredDown == 1); ++mfSentry; break;
						case 4: {   // Escort
							int ei = b.primary.escortTargetIndex;
							if (ei >= 1 && static_cast<unsigned long>(ei) <= numWarriors && MechWarrior::warriorList[ei] &&
							    MechWarrior::warriorList[ei]->getVehicle())
								rt->escortTargetWID = MechWarrior::warriorList[ei]->getVehicle()->getWatchID();
							rt->escortMoving = true; rt->guardHold = true; ++mfEscort; break;
						}
						case 2: {   // MoveTo - move to destination once, then hold + engage
							int wc = (b.primary.waypointCount > 8) ? 8 : b.primary.waypointCount;
							if (wc > 0) {
								for (int wi = 0; wi < wc; ++wi) {
									glm::vec3 wp; wp.x = b.primary.waypoints[wi].x; wp.y = b.primary.waypoints[wi].y; wp.z = 0.0f;
									float z = land ? land->getTerrainElevation(wp) : 0.0f;
									rt->patrolWaypoints[wi][0] = b.primary.waypoints[wi].x;
									rt->patrolWaypoints[wi][1] = b.primary.waypoints[wi].y;
									rt->patrolWaypoints[wi][2] = z;
								}
								rt->patrolWaypointCount = static_cast<uint8_t>(wc);
								rt->patrolWaypointIndex = 0;
								rt->patrolLoop = false;   // move once to destination, do not loop
								rt->patrolActive = true;
							}
							rt->guardHold = true; ++mfOther; break;
						}
						case 7:     // Attack - aggressive: engage any detected enemy (no EngageRadius limit)
							rt->guardHold = true; rt->engageRadius = 0.0f; ++mfOther; break;
						case 8: {   // Withdraw - move to rally point (primary waypoint) once, then hold
							int wc = (b.primary.waypointCount > 8) ? 8 : b.primary.waypointCount;
							if (wc > 0) {
								for (int wi = 0; wi < wc; ++wi) {
									glm::vec3 wp; wp.x = b.primary.waypoints[wi].x; wp.y = b.primary.waypoints[wi].y; wp.z = 0.0f;
									float z = land ? land->getTerrainElevation(wp) : 0.0f;
									rt->patrolWaypoints[wi][0] = b.primary.waypoints[wi].x;
									rt->patrolWaypoints[wi][1] = b.primary.waypoints[wi].y;
									rt->patrolWaypoints[wi][2] = z;
								}
								rt->patrolWaypointCount = static_cast<uint8_t>(wc);
								rt->patrolWaypointIndex = 0;
								rt->patrolLoop = false;   // move once to destination, do not loop
								rt->patrolActive = true;
							}
							rt->guardHold = true; ++mfOther; break;
						}
						case 5:     // Ambush - hidden until a threat is near, then strike (reuses sentry asleep/woken)
							rt->guardHold = true; rt->sentryAsleep = true; ++mfOther; break;
						case 6: {   // Scout - move the route observing only; never engages
							int wc = (b.primary.waypointCount > 8) ? 8 : b.primary.waypointCount;
							if (wc > 0) {
								for (int wi = 0; wi < wc; ++wi) {
									glm::vec3 wp; wp.x = b.primary.waypoints[wi].x; wp.y = b.primary.waypoints[wi].y; wp.z = 0.0f;
									float z = land ? land->getTerrainElevation(wp) : 0.0f;
									rt->patrolWaypoints[wi][0] = b.primary.waypoints[wi].x;
									rt->patrolWaypoints[wi][1] = b.primary.waypoints[wi].y;
									rt->patrolWaypoints[wi][2] = z;
								}
								rt->patrolWaypointCount = static_cast<uint8_t>(wc);
								rt->patrolWaypointIndex = 0;
								rt->patrolLoop = b.primary.loop;
								rt->patrolActive = true;
							}
							rt->scoutObserveOnly = true; ++mfOther; break;
						}
						default:    // MoveTo / Attack / Withdraw / Ambush / Scout -> hold + engage (verb wiring TBD)
							rt->guardHold = true; ++mfOther; break;
					}
					if (missionFitOpordApplies(b, numWarriors, opordEnabled))
						authoredOpordPilots.push_back(static_cast<std::uint32_t>(b.warriorIndex));
				}
				mc2::fmt::print_err("[MISSIONFIT_OPORD] parsed {:d} Brain{{}} record(s): patrol={:d} guard={:d} sentry={:d} escort={:d} other={:d}\n", mc2::fmt::as_i32(mfN), mc2::fmt::as_i32(mfPatrol), mc2::fmt::as_i32(mfGuard), mc2::fmt::as_i32(mfSentry), mc2::fmt::as_i32(mfEscort), mc2::fmt::as_i32(mfOther));
				if (mfBrains) free(mfBrains);
				free(mfBuf);
			}
			mfFile.close();
		}
	}
}

// Load the brain runtime tables.
void Mission::loadBrainRuntime(MissionInitFrame& f) {
	auto& missionName = f.missionName; auto& numWarriors = f.numWarriors;
	// BRAIN-RUNTIME-1B: load per-unit Brain blocks from <missionName>_ai.fit.
	// Gate: MC2_BRAIN_RUNTIME=1 (default OFF). File absence is silent (no fixture = all Legacy).
	// Single-warrior fixture only: one Brain block keyed "Brain". unitRef "Warrior%d" -> idx.
	// MC2_BRAIN_RUNTIME_FORCE_MODE (if set) overrides all per-unit modes as a post-load global.
	if (mc2::brainpolicy::techBrain()) {
		char aiFitName[256];
		// missionName is the raw name like "mc2_01"; missionPath is "data/missions/".
		mc2::fmt::to_buf(aiFitName, "{}{}_ai.fit", mc2::fmt::safe(missionPath), mc2::fmt::safe(missionName));
		mc2::fmt::print_err("[BRAIN_RT] mission load: policy=techbrain mission={} seeking {}\n", mc2::fmt::safe(missionName), mc2::fmt::safe(aiFitName));
		std::fflush(stderr);
		FitIniFile aiFit;
		if (aiFit.open(aiFitName) == NO_ERR) {
			// Seek the single "Brain" block (single-warrior fixture; multi-block not supported this slice).
			if (aiFit.seekBlock("Brain") == NO_ERR) {
				char unitRefBuf[32] = {};
				long modeValLong = 0;
				long unitRefResult  = aiFit.readIdString("unitRef",  unitRefBuf, 31);
				long modeResult     = aiFit.readIdLong("mode", modeValLong);
				if (unitRefResult == NO_ERR && modeResult == NO_ERR) {
					// Parse "Warrior%d" -> 1-based index.
					int warriorIdx = -1;
					std::sscanf(unitRefBuf, "Warrior%d", &warriorIdx);
					if (warriorIdx >= 1 && warriorIdx <= static_cast<int>(numWarriors)) {
						MechWarriorPtr w = MechWarrior::warriorList[warriorIdx];
						if (w) {
							BrainRuntimeMode fitMode = BrainRuntimeMode::Legacy;
							switch (modeValLong) {
								case 1: fitMode = BrainRuntimeMode::Hybrid;   break;
								case 2: fitMode = BrainRuntimeMode::Enhanced; break;
								default: break;
							}
							// setBrainRuntimeMode allocates brainRuntime if needed + sets mode.
							w->setBrainRuntimeMode(fitMode);
							mc2::fmt::print_err("[BRAIN_RT] FIT load warriorIdx={:d} unitRef={} mode={}\n", mc2::fmt::as_i32(warriorIdx), mc2::fmt::safe(unitRefBuf), mc2::fmt::safe(MechBrainRuntime::modeString(fitMode)));
							std::fflush(stderr);
						}
					} else {
						mc2::fmt::print_err("[BRAIN_RT] WARNING: _ai.fit Brain block unitRef={} idx={:d} out of range [1..{:d}]\n", mc2::fmt::safe(unitRefBuf), mc2::fmt::as_i32(warriorIdx), mc2::fmt::as_u32(static_cast<unsigned long>(numWarriors)));
					}
				}
			}
			aiFit.close();
		}
		// MC2_BRAIN_RUNTIME_FORCE_MODE overrides all per-unit modes (post-load global override).
		if (mc2::gate::isSet<"MC2_BRAIN_RUNTIME_FORCE_MODE">()) {
			BrainRuntimeMode forced = BrainRuntimeMode::Legacy;   // "legacy" and any other text force Legacy
			if      (mc2::gate::is<"MC2_BRAIN_RUNTIME_FORCE_MODE", "hybrid">())   forced = BrainRuntimeMode::Hybrid;
			else if (mc2::gate::is<"MC2_BRAIN_RUNTIME_FORCE_MODE", "enhanced">()) forced = BrainRuntimeMode::Enhanced;
			for (unsigned long i = 1; i <= numWarriors; i++) {
				MechWarriorPtr w = MechWarrior::warriorList[i];
				if (w && w->getBrainRuntime())
					w->setBrainRuntimeMode(forced);
			}
		}
		aiFit.close();
	}
}

// Load the tactic weight tables.
void Mission::loadTacticWeights(MissionInitFrame& f) {
	auto& missionName = f.missionName; auto& numWarriors = f.numWarriors;
	// TACTIC-WEIGHTS-A: load [Tactics] block from <missionName>_ai.fit into tacticWeights[].
	// Gate: MC2_TACTIC_WEIGHTS=1 (default OFF).
	// File absence is silent. Block absence is silent (all weights stay zero).
	// Format: one float entry per tactic by short name, e.g. "f StopAndFire = 0.30"
	// Normalisation is enforced at load time (sum→1); weightsNormalized flag set.
	// Both raw-zero and absent-block warriors keep tacticWeights all-zero (FIT fallback
	// uses uniform weights via the all-zero branch in applyPilotModulation).
	if (mc2::gate::get<"MC2_TACTIC_WEIGHTS">()) {
		char tacFitName[256];
		mc2::fmt::to_buf(tacFitName, "{}{}_ai.fit", mc2::fmt::safe(missionPath), mc2::fmt::safe(missionName));
		FitIniFile tacFit;
		if (tacFit.open(tacFitName) == NO_ERR) {
			if (tacFit.seekBlock("Tactics") == NO_ERR) {
				struct { const char* name; int idx; } kTacticKeys[] = {
#define MC2_TACTIC_KEY(L, S, V, N, C, B, E) { N, L },
					MC2_TACTIC_TABLE(MC2_TACTIC_KEY)
#undef MC2_TACTIC_KEY
				};
				float loadedWeights[NUM_TACTICS] = {};
				for (auto& kv : kTacticKeys) {
					float val = 0.0f;
					if (tacFit.readIdFloat(kv.name, val) == NO_ERR)
						loadedWeights[kv.idx] = (val < 0.0f) ? 0.0f : val;
				}
				float sum = 0.0f;
				for (const auto loadedWeight : loadedWeights) sum += loadedWeight;
				if (sum > 0.0f) {
					float inv = 1.0f / sum;
					for (auto & loadedWeight : loadedWeights) loadedWeight *= inv;
				}
				int loadCount = 0;
				for (unsigned long i = 1; i <= numWarriors; i++) {
					MechWarriorPtr w = MechWarrior::warriorList[i];
					if (w) {
						if (!w->getBrainRuntime()) {
							// Allocate runtime so tactic weights are accessible even for Legacy warriors.
							w->setBrainRuntimeMode(BrainRuntimeMode::Legacy);
						}
						MechBrainRuntime* rt = w->getBrainRuntime();
						if (rt) {
							std::memcpy(rt->tacticWeights, loadedWeights, sizeof(rt->tacticWeights));
							rt->weightsNormalized = 1;
							loadCount++;
						}
					}
				}
				mc2::fmt::print_err("[BRAIN_TACTIC_SELECT] [Tactics] loaded from {}: {:d} warriors weighted\n", mc2::fmt::safe(tacFitName), mc2::fmt::as_i32(loadCount));
				std::fflush(stderr);
			}
			tacFit.close();
		}
		tacFit.close();
	}
}

// Resolve which brain dispatch mode each warrior runs.
void Mission::loadBrainDispatch(MissionInitFrame& f) {
	auto& missionName = f.missionName; auto& numWarriors = f.numWarriors;
	// TECHSCRIPT-SPECIAL-DISPATCH-1A: load per-warrior BrainSpecial body from <missionName>_specials.fit.
	// Gate: MC2_BRAIN_DISPATCH=1 (default OFF). Requires MC2_BRAIN_RUNTIME=1 + MC2_BRAIN_RUNTIME_APPLY=1.
	// File absence is silent. Only warriors with brainRuntime allocated (Enhanced mode) are loaded.
	//
	// TECHSCRIPT-SPECIAL-DISPATCH-1C: gate MC2_BRAIN_DISPATCH_FSM_TODO=1 (requires MC2_BRAIN_DISPATCH=1).
	// Runs a MISSION-LEVEL raw-text scan of the specials file for
	// "; TODO: manual ABL line: <payload>" comments (stripped by FitIniFile).
	// Emits [BRAIN_DISPATCH_FSM_TODO] summary + detail lines. Information only — no behavior change.
	// Scan is done once per mission (not per-warrior) because the file is mission-level.
	// Gate-OFF: byte-identical to pre-1C; no extra parsing, no extra traces.
	{
		const bool dispatchOn   = mc2::brainpolicy::techBrain();
		const bool fsmTodoOn    = mc2::brainpolicy::techBrain();
		const bool dispatchVarOn = mc2::brainpolicy::techBrain();
		const bool missionVarOn  = mc2::brainpolicy::techBrain();

		// GAP-A: reset the mission-level specials cache at every mission load (unconditional;
		// produces no trace, so gate-OFF runs stay byte-identical). Prevents a stale cache
		// from a prior dispatch-ON mission leaking into a later one.
		resetMissionSpecialCache();

		// TECHSCRIPT-DISPATCH-1D-M: reset mission-scope Var store at every mission load.
		// resetMissionVarStore() is always safe to call (no-op if store already empty).
		// Gate: only reset when MC2_BRAIN_VAR_MISSION=1 to avoid noise in gate-OFF runs.
		if (missionVarOn) {
			resetMissionVarStore();
		}

		if (fsmTodoOn && !dispatchOn) {
			mc2::fmt::print_err("[BRAIN_DISPATCH_FSM_TODO] WARN: FSM todo requires the TechBrain policy — inert\n");
			std::fflush(stderr);
		}
		if (dispatchVarOn && !dispatchOn) {
			mc2::fmt::print_err("[BRAIN_DISPATCH_VAR] WARNING: dispatch var requires the TechBrain policy -- var handling is INERT\n");
			std::fflush(stderr);
		}

		if (dispatchOn) {
			// DISPATCH-EFFECT-UNITEJECT-1: MC2_BRAIN_SPECIAL_FIT allows test-fixture override.
			// When set, ALL warriors load from <override>_specials.fit instead of <mission>_specials.fit.
			// Intended for smoke gate verification only. Default: missionName (standard behavior).
			const std::string specialFitText = mc2::gate::str<"MC2_BRAIN_SPECIAL_FIT">();   // "" when unset or empty
			const char* specialFitName = specialFitText.empty() ? missionName : specialFitText.c_str();

			mc2::fmt::print_err("[BRAIN_DISPATCH] mission load: policy=techbrain mission={} special_fit={}\n", mc2::fmt::safe(missionName), mc2::fmt::safe(specialFitName));
			std::fflush(stderr);

			// 1A/1B: per-warrior verb parse (only for warriors with brainRuntime allocated).
			// TECHSCRIPT-CALL-CHAIN-1A: pass specialIndex so the raw scanner populates it.
			// The index is per-warrior-runtime (each warrior gets its own copy); content
			// is identical across warriors for the same mission (all read the same file).
			// This is acceptable for 1A: index is small (<10 blocks), mission-ephemeral.
			//
			// GAP-A MULTI-WARRIOR SPECIALS: parse mission-level specials body ONCE into the
			// dispatch cache. Warriors whose brainRuntime is allocated lazily in runBrain
			// (not named in _ai.fit) copy from this cache on first tick, so every warrior
			// dispatches -- not just the _ai.fit-recorded ones.
			cacheMissionSpecialBody(specialFitName);
			for (unsigned long i = 1; i <= numWarriors; i++) {
				MechWarriorPtr w = MechWarrior::warriorList[i];
				if (w && w->getBrainRuntime()) {
					parseBrainSpecialBody(specialFitName, w->getBrainRuntime()->specialBody,
					                      &w->getBrainRuntime()->specialIndex);
				}
			}

			// TECHSCRIPT-SPECIAL-DISPATCH-1C: FSM-TODO mission-level scan + trace.
			// Done ONCE per mission (the specials file is mission-level, not per-warrior).
			// Gate-OFF: this entire block is skipped; no ifstream open, no extra traces.
			if (fsmTodoOn) {
				// Use a temporary BrainSpecialBody for the scan — trace output is mission-level.
				// fsmTodos are INFORMATION ONLY; they are not stored persistently here.
				// (Future per-warrior storage is deferred — the file has one FSM skeleton shared
				// by all warriors; per-warrior active-state gating is out of 1C scope.)
				BrainSpecialBody tmpBody;
				scanFsmTodosFromFile(specialFitName, tmpBody);

				int nStateDef = 0, nStateEnd = 0, nTrans = 0, nTransBack = 0, nOther = 0;
				for (const FsmTodoEntry& e : tmpBody.fsmTodos) {
					switch (e.kind) {
						case FsmTodoKind::STATE_DEF:  ++nStateDef;  break;
						case FsmTodoKind::STATE_END:  ++nStateEnd;  break;
						case FsmTodoKind::TRANS:      ++nTrans;     break;
						case FsmTodoKind::TRANS_BACK: ++nTransBack; break;
						case FsmTodoKind::OTHER_TODO: ++nOther;     break;
					}
				}

				// Summary line (always emitted when FSM_TODO gate is on, even if count=0).
				// wid=-1 signals mission-level (no specific warrior context).
				mc2::fmt::print_err("[BRAIN_DISPATCH_FSM_TODO] mission={} wid=-1 stateDefs={:d} transitions={:d} otherTodos={:d}\n",
					mc2::fmt::safe(missionName), mc2::fmt::as_i32(nStateDef), mc2::fmt::as_i32(nTrans + nTransBack), mc2::fmt::as_i32(nOther));
				std::fflush(stderr);

				// Detail lines: STATE_DEF, TRANS, TRANS_BACK, STATE_END only.
				// OTHER_TODO is counted in summary but NOT detail-traced (noise suppression).
				for (const FsmTodoEntry& e : tmpBody.fsmTodos) {
					switch (e.kind) {
						case FsmTodoKind::STATE_DEF:
							mc2::fmt::print_err("[BRAIN_DISPATCH_FSM_TODO] kind=STATE_DEF name={} wid=-1\n", e.name.c_str());
							std::fflush(stderr);
							break;
						case FsmTodoKind::STATE_END:
							mc2::fmt::print_err("[BRAIN_DISPATCH_FSM_TODO] kind=STATE_END wid=-1\n");
							std::fflush(stderr);
							break;
						case FsmTodoKind::TRANS:
							mc2::fmt::print_err("[BRAIN_DISPATCH_FSM_TODO] kind=TRANS target={} wid=-1\n", e.name.c_str());
							std::fflush(stderr);
							break;
						case FsmTodoKind::TRANS_BACK:
							mc2::fmt::print_err("[BRAIN_DISPATCH_FSM_TODO] kind=TRANS_BACK wid=-1\n");
							std::fflush(stderr);
							break;
						case FsmTodoKind::OTHER_TODO:
							break;
					}
				}
			} // fsmTodoOn
		} // dispatchOn
	}
}

// Count the mechs, vehicles and terrain objects and size the object manager.
void Mission::initObjectCounts(MissionLoadType loadType, MissionInitFrame& f) {
	auto& pakFile = f.pakFile;
	loadProgress = 48.5f;
	long numMechs = 0;
	long numVehicles = 0;
	for (int i = 1; i < (numParts + 1); i++) 
	{
		graphics::pump_loading_events();
		ObjectTypePtr objType = ObjectManager->loadObjectType(parts[i].objNumber, parts[i].csvFile[0] != '\0');
		if (!objType)
			objType = ObjectManager->getObjectType(parts[i].objNumber);
		if (objType)
			switch (objType->getObjectTypeClass()) 
			{
				case BATTLEMECH_TYPE:
					numMechs++;
					break;
				case VEHICLE_TYPE:
					numVehicles++;
					break;
			}

		float increment = 6.5f/(numParts + 1);
		loadProgress += increment;
	}

	switch (loadType) 
	{
		case MISSION_LOAD_SP_QUICKSTART:
		case MISSION_LOAD_SP_LOGISTICS:
			break;
		case MISSION_LOAD_MP_QUICKSTART:
		case MISSION_LOAD_MP_LOGISTICS:
			numMechs = 64;
			numVehicles = 64;
			break;
	}

	loadProgress = 55.0f;

	pakFile.seekPacket( 1 );
	ObjectManager->countTerrainObjects(&pakFile, (numMechs + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM) + (numVehicles + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM)/* + ObjectManager->maxElementals*/ + 1);
	loadProgress = 58.0f;
	ObjectManager->setNumObjects(numMechs, numVehicles, 0, -1, -1, -1, 100, 50, 0, 130, -1);
}

// Initialise GPU culling, the frame-job pool, terrain lighting and shadow priming.
void Mission::initGpuAndLighting() {
	// C0-3 + C1b GPU authority flip: init GPU cull substrate SSBO sized to
	// worst-case per-frame record count. Now includes BOTH dynamic actors
	// (getMaxObjects() + 25% headroom) AND static prop instances (appended by
	// GpuStaticPropRegistry::flush() before compute_dispatch()). Static props
	// at wolfman zoom can reach ~4096+ visible instances; add a flat 8192
	// record headroom beyond the dynamic actor count to cover them safely.
	{
		const uint32_t maxActors = static_cast<uint32_t>(ObjectManager->getMaxObjects());
		const uint32_t staticPropHeadroom = 8192u;  // visible static props at wolfman zoom
		gpu_cull::substrate_init(maxActors + maxActors / 4u + staticPropHeadroom);
	}
	// C1a: init GPU visibility compute pipeline (shadow/diagnostic mode).
	// No-op if MC2_GPU_CULL env var is not set (default off).
	gpu_cull::compute_init();
	frameJobsInit();  // FRAME-JOBS-1: fixed worker pool for parallel CPU prep
	// Phase 1: terrain lighting GPU compute — per-mission init alongside gpu_cull.
	// CRITICAL: use realVerticesMapSide * realVerticesMapSide, NOT getNumVertices()
	// (getNumVertices() returns 0 at this point — set per-frame by makeLists).
	// Terrain::realVerticesMapSide set during land->init() at mission.cpp:2222, before here.
	// No-op if MC2_TERRAIN_LIGHTING_GPU env var is not set (default off at Stage 1).
	gos_terrain_lighting::mission_init(
		static_cast<uint32_t>(Terrain::realVerticesMapSide * Terrain::realVerticesMapSide),
		64u);
	// C2: init async readback ring buffer.
	// Same maxActors sizing as substrate_init — readback slot must hold one flag per actor.
	// No-op if MC2_GPU_CULL_READBACK env var is not set (default off).
	{
		const uint32_t maxActors = static_cast<uint32_t>(ObjectManager->getMaxObjects());
		const uint32_t staticPropHeadroom = 8192u;
		gpu_cull::readback_init(maxActors + maxActors / 4u + staticPropHeadroom);
	}
	// CP-1: new mission - re-prime the world-fixed static shadow map and the
	// static light matrix (both are process-scoped and otherwise carry over
	// the previous mission's state into this mission).

}

// Spawn the movers and vehicles for each commander.
void Mission::initSpawnActors(MissionLoadType loadType) {
	long curMech = 0;
	long curVehicle = 0;
	for (long t = 0; t < 8; t++)
		for (long i = 1; i < (numParts + 1); i++) 
		{
			graphics::pump_loading_events();
			bool loadEm = true;
			if (loadType == MISSION_LOAD_MP_LOGISTICS)
				loadEm = false;
			if (loadType == MISSION_LOAD_SP_LOGISTICS)
				if (parts[i].commanderID == 0 && !parts[i].destroyed)
					loadEm = false;
			if (loadType == MISSION_LOAD_MP_QUICKSTART)
				if (parts[i].commanderID == -1)
					loadEm = false;
			if (loadEm) {
				ObjectTypePtr objType = ObjectManager->getObjectType(parts[i].objNumber);
				if (objType)
					switch (objType->getObjectTypeClass()) 
					{
						case BATTLEMECH_TYPE:
							if (parts[i].teamId == t) 
							{
								BattleMechPtr mech = ObjectManager->getMech(curMech++);
								createPartObject(i, mech);
								float increment = 10.0f/(numParts + 1);
								loadProgress += increment;
							}
							break;
						case VEHICLE_TYPE:
							if (parts[i].teamId == t) 
							{
								GroundVehiclePtr vehicle = ObjectManager->getVehicle(curVehicle++);
								createPartObject(i, vehicle);
								float increment = 10.0f/(numParts + 1);
								loadProgress += increment;
							}
							break;
					}
				}
			else 
			{
				MechWarrior::freeWarrior(MechWarrior::warriorList[parts[i].pilot]);
			}
		}

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeMoverPartsLoad=x1-x;
#endif
}

// Load the terrain objects and build the mover lists.
void Mission::initTerrainObjectsLoad(char (*commandersToLoad)[3], MissionInitFrame& f) {
	auto& pakFile = f.pakFile;
	loadProgress = 68.0f;
	mission_phase_mark("actor_spawn_ready"); // LOAD-PHASE-FACTS-1: mover/vehicle/part object init done

	{
		ZoneScopedN("Mission::init ObjectManager::loadTerrainObjects"); MC2_LOAD_SCOPE("Mission::init ObjectManager::loadTerrainObjects");
		const auto lapT0 = std::chrono::steady_clock::now();
		ObjectManager->loadTerrainObjects(&pakFile, loadProgress, 30);
		if (mc2_diag::tagRequested("LOAD_LAPS")) mc2::content::reportLoadLaps("terrain_objects",
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lapT0).count(), f.missionName);
	}
	mission_phase_mark("terrain_objects_ready");
	{ ZoneScopedN("Mission::init ObjectManager::primeTerrainObjectsForMissionLoad"); MC2_LOAD_SCOPE("Mission::init ObjectManager::primeTerrainObjectsForMissionLoad"); ObjectManager->primeTerrainObjectsForMissionLoad(loadProgress, 2.0f); }
	// Movers were placed before trees existed. Do the same tree contact as
	// walking, before static registration can retain an intact spawn tree.
	unsigned spawnTrees = 0;
	for (long i = 0; i < ObjectManager->numMechs; ++i) {
		auto* mech = ObjectManager->getMech(i);
		if (!mech) continue;
		spawnTrees += mc2::clearSpawnTrees(*mech,
			std::span(ObjectManager->terrainObjects, static_cast<size_t>(ObjectManager->numTerrainObjects)),
			[](auto* tree, auto* mover) {
				tree->getObjectType()->handleCollision(tree, mover);
				tree->primeAppearanceForMissionLoad();
			});
	}
	if (mc2::gate::get<"MC2_DIAG_VERBOSE">()) mc2::fmt::print_err("[SPAWN_TREES] cleared={}\n", spawnTrees);
	{ ZoneScopedN("Mission::init Track B static-prop registration walk"); MC2_LOAD_SCOPE("Mission::init Track B static-prop registration walk"); ObjectManager->registerStaticPropsForMissionLoad(); }
	mission_phase_mark("texture_prewarm_ready"); // LOAD-PHASE-FACTS-1: terrain-object load + static-prop residency prewarm done

	loadProgress = 98.0f;

	ObjectManager->buildMoverLists();

	if (MPlayer)
		MPlayer->initSpecialBuildings(commandersToLoad);
}

// Apply the start-of-mission disables and load the team objectives.
void Mission::initDisableAndObjectives(MissionInitFrame& f) {
	auto& result = f.result; auto& maxTeamID = f.maxTeamID;
	{
		m_timeLimit = -1.0f	/*seconds  IF -1.0 mission never times out!!! -fs*/;
		long result = 0;
		result = missionFile->seekBlock("MissionSettings");
		if (NO_ERR == result)
		{
			float tmpFloat = 0.0;
			result = missionFile->readIdFloat("TimeLimit", tmpFloat);
			if (NO_ERR == result)
			{
				m_timeLimit = tmpFloat;
			}
		}

		if ( MPlayer )
			m_timeLimit = MPlayer->missionSettings.timeLimit;
	}

	if (NumDisableAtStart) {
		for (long i = 0; i < NumDisableAtStart; i++)
			ObjectManager->getByWatchID(parts[DisableAtStart[i]].objectWID)->setDebugFlag(OBJECT_DFLAG_DISABLE, true);
	}

	for (int i = 0; i <= maxTeamID; i++)
		Team::teams[i]->loadObjectives(missionFile);

/*	numObjectives = 0; // this refers to the number of *old* objectives
	objectives = 0;

	warning1 = FALSE;
	warning2 = FALSE;

	Team::home->objectives.Clear();
	Team::home->objectives.Read(missionFile);

	Team::home->numPrimaryObjectives = 0;
	CObjectives::EIterator it = Team::home->objectives.Begin();
	while (!it.IsDone())
	{
		if (1 == (*it)->Priority())
		{
			Team::home->numPrimaryObjectives += 1;
		}
		it++;
	}
	ReadNavMarkers(missionFile, Team::home->objectives);
*/
#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeObjectiveLoad=x-x1;
#endif
}

// Finish the commanders, rosters and mission clocks.
void Mission::initCommanders(char (*commandersToLoad)[3], MissionInitFrame& f) {
	auto& result = f.result;
	for (long curCommanderId = 0; curCommanderId < MAX_MC_PLAYERS; curCommanderId++) {
		long numGroups = 0;
		char headingStr[128];
		
		if (commandersToLoad[curCommanderId][0] == -1)
			continue;

		// TEAM-COMMANDER-OWNERSHIP-1 audit: this reads commandersToLoad back to
		// INDEX + DEREF Commander::commanders[] below, but (unlike the setTeam site
		// at :2851) only checked != -1. A caller-supplied id >= MAX_COMMANDERS is an
		// OOB read (always a bug -> bound it up front). The NULL-slot case (init
		// loops only created [0..maxCommanderID]) is NOT gated here: the original
		// only dereferences the slot INSIDE the group-block while loop, so a NULL
		// slot with no CommanderNGroup:0 block is benign and must stay silent to
		// keep stock byte-identical. The null-check therefore gates only the actual
		// dereference site, once per commander that truly has group blocks.
		long ctlCommander = commandersToLoad[curCommanderId][0];
		if (!MC2_VERIFY_BOUNDS(ctlCommander, MAX_COMMANDERS, "Mission::init commander-groups commandersToLoad"))
			continue;

		mc2::fmt::to_buf(headingStr, "Commander{:d}Group:{:d}", mc2::fmt::as_i32(curCommanderId), mc2::fmt::as_i32(numGroups));
		result = missionFile->seekBlock(headingStr);
		// A group block exists for this commander -> the loop below WILL deref the
		// slot. Verify it was created before entering (matches the original's
		// reachable deref exactly; skips the malformed commander's groups in log
		// mode instead of AVing on the uninitialized slot).
		if (result == NO_ERR &&
			!MC2_VERIFY_NOTNULL(Commander::commanders[ctlCommander], "Mission::init commander-groups Commander::commanders[]"))
			continue;
		while (result == NO_ERR) {
			bool pointSet = FALSE;
			long groupMates[MAX_MOVERGROUP_COUNT_START];
			result = missionFile->readIdLongArray("Mates", groupMates, MAX_MOVERGROUP_COUNT_START);
			Assert(result == NO_ERR, result, " could not find Mates in Group in Scenario File ");
			for (long curMate = 0; curMate < MAX_MOVERGROUP_COUNT_START; curMate++) {
				if ((groupMates[curMate] > 0) && parts[groupMates[curMate]].objectWID) {
					ObjectManager->setPartId(ObjectManager->getByWatchID(parts[groupMates[curMate]].objectWID), ctlCommander, numGroups, curMate);
					Commander::commanders[ctlCommander]->getGroup(numGroups)->add(static_cast<MoverPtr>(ObjectManager->getByWatchID(parts[groupMates[curMate]].objectWID)));
					if (!pointSet) {
						Commander::commanders[ctlCommander]->getGroup(numGroups)->selectPoint(true);
						pointSet = true;
					}
				}
			}
			
			numGroups++;
			mc2::fmt::to_buf(headingStr, "Commander{:d}Group:{:d}", mc2::fmt::as_i32(curCommanderId), mc2::fmt::as_i32(numGroups));
			result = missionFile->seekBlock(headingStr);
		}
	}

	for (int i = 0; i < Team::numTeams; i++)
		Team::teams[i]->buildRoster();

	//---------------------------------------------------------------------------------
	// If we're not playing multiplayer, make sure all home commander movers have their
	// localMoverId set to 0, so the iface can at least check if a mover is player
	// controlled...
	if (!MPlayer)
		Commander::home->setLocalMoverId(0);

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeCommanderLoad=x1-x;
#endif

	LastTimeGetTime = 0xffffffff;
	scenarioTime = 0.0;
	MissionStartTime = 0;
	runningTime = 0.0;
	actualTime = 0.0;
}

// Close gamesys.fit and create the weather and the camera.
void Mission::initWeatherCamera(MissionInitFrame& f) {
	auto& gameSystemFile = f.gameSystemFile; auto& result = f.result;
	gameSystemFile->close();
	gameSystemFile.reset();

	missionWeather = std::make_unique_for_overwrite<Weather>();
	weather = missionWeather.get();
	weather->init(missionFile);
	
	missionCamera = std::make_unique_for_overwrite<GameCamera>();
	eye = missionCamera.get();
	eye->init();
	gosASSERT(eye != nullptr);

	mc2SetMissionLightProfileName(missionFileName);   // LOCAL-LIGHTS-4: before the profile applies in init
	result = eye->init(missionFile);
	gosASSERT(result == NO_ERR);

	eye->inMovieMode = false;

	loadProgress = 99.0;

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeMiscLoad=x-x1;
#endif
}

// Create the mission interface, load its screen and finish the pilot log.
void Mission::initInterface(MissionInitFrame& f) {
	auto& result = f.result; auto& pakFile = f.pakFile;
	missionInterface = new MissionInterfaceManager;
	gosASSERT(missionInterface != nullptr);
	
	{ MC2_LOAD_SCOPE("interface.initTacMap"); missionInterface->initTacMap( &pakFile, 2 ); }
	
	FullPathFileName missionScreenName;
	missionScreenName.init(artPath,"missionScrn",".fit");
	
	FitIniFile missionLoader;
	result = missionLoader.open(missionScreenName);
	if (result != NO_ERR)
		STOP(("Unable to open mission screen layout %s", static_cast<const char*>(missionScreenName)));
	
	{ MC2_LOAD_SCOPE("interface.init"); missionInterface->init(&missionLoader); }
	{ MC2_LOAD_SCOPE("interface.initMechs"); missionInterface->initMechs(); }
	missionLoader.close();

	// MISSION-START-HOVER-TARGET-LIFETIME-1: this MissionInterfaceManager is freshly
	// constructed, but the file-scope hover/target caches and the static `target` pointer
	// survive across missions. Clear them now (begin boundary) so the first Mission::update
	// can't pick a stale/freed object from the previous mission. Picking stays SUPPRESSED
	// until Mission::start() arms it (world fully live).
	MissionInterfaceManager::invalidateHoverTarget();
	
	userInput->setMouseCursor(mState_NORMAL);

	loadProgress = 100.0;

	

	if (tempSpecialAreaFootPrints) {
		std::free(tempSpecialAreaFootPrints);
		tempSpecialAreaFootPrints = nullptr;
		tempNumSpecialAreas = 0;
	}

	{ MC2_LOAD_SCOPE("interface.initOptimalCells"); Mover::initOptimalCells(32); }


	if (CombatLog)
		MechWarrior::logPilots(CombatLog);
}

// Scale the lab timing counters and close the mission file.
void Mission::initLabTimingAndClose() {
#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeGUILoad=x1-x;
#endif

#ifdef LAB_ONLY	
	MCTimeABLLoad       *= OneOverProcessorSpeed;
	MCTimeMiscToTeamLoad*= OneOverProcessorSpeed;
	MCTimeTeamLoad      *= OneOverProcessorSpeed;
	MCTimeObjectLoad    *= OneOverProcessorSpeed;
	MCTimeTerrainLoad   *= OneOverProcessorSpeed;
	MCTimeMoveLoad      *= OneOverProcessorSpeed;
	MCTimeMissionABLLoad*= OneOverProcessorSpeed;
	MCTimeWarriorLoad   *= OneOverProcessorSpeed;
	MCTimeMoverPartsLoad*= OneOverProcessorSpeed;
	MCTimeObjectiveLoad *= OneOverProcessorSpeed;
	MCTimeCommanderLoad *= OneOverProcessorSpeed;
	MCTimeMiscLoad      *= OneOverProcessorSpeed;
	MCTimeGUILoad       *= OneOverProcessorSpeed;
#endif

	missionFile->close();
	missionFile = nullptr;
}

// Finalise the static-prop and mech geometry and the GPU cull buffers.
void Mission::initFinalizeGeometry() {
	// Seed gameplay attachments before the first LOS/weapon query. Camera
	// extraction must never supply a mover's initial node palette.
	for (long i = 1; i <= ObjectManager->getMaxObjects(); ++i) {
		graphics::pump_loading_events();
		auto* object = ObjectManager->get(i);
		if (!object || !object->getExists()) continue;
		auto* appearance = object->getAppearance();
		if (!appearance || !appearance->simulationOwned()) continue;
		const int team = object->getTeamId();
		const int relation = team >= 0 && Team::home ? Team::getRelation(team, Team::home->getId()) : 0;
		appearance->setObjectParameters(object->getPosition(), object->getRotation(), object->drawFlags, team, relation);
		appearance->refreshSimPose();
	}
	MC2_LOAD_SCOPE("finalize.total");
	ObjectManager->snapshotMoverPoses();
	ObjectManager->extractRenderSnapshots();
	// All actors have been spawned and have called registerType() into the
	// GPU static-prop batcher. Upload the shared VBO/IBO/VAO now so that
	// submit()/flush() can reference immutable geometry for the rest of the
	// mission. Safe here: GL context is live (textures/shadow FBOs already
	// exist by this point) and this is the unconditional tail of map-load.
	{ MC2_LOAD_SCOPE("finalize.staticPropGeometry"); GameAdapters::StaticProp::finalizeGeometry(); }
	graphics::pump_loading_events();
	{ MC2_LOAD_SCOPE("finalize.mechGeometry"); GpuMechBatcher::instance().finalizeGeometry(); }
	graphics::pump_loading_events();



	// STATIC-REG-PREWARM-QUEUE-1: bake light slots for registered static props
	// that are off-screen (never reach render() → never cleared their
	// needsFullBakeNextFrame H4 latch). Must run AFTER finalizeGeometry() (geometry
	// is final) AND after eye->init() (world lights are valid — eye is created at
	// mission.cpp:3589 before we reach here). Guarded by MC2_STATIC_REG_PREWARM=1.
	// updateLights() is called first so activeLights is populated (numActiveLights > 0);
	// without it activeLights is empty at load time and SetLightList gets zero lights.
	{ ZoneScopedN("Mission::init prewarmStaticPropLightBakes"); MC2_LOAD_SCOPE("Mission::init prewarmStaticPropLightBakes");
	  eye->primeActiveLightsForPrewarm();
	  prewarmStaticPropLightBakes(*ObjectManager, eye); }
	{ ZoneScopedN("Mission::init buildStaticShadowCasterSet"); MC2_LOAD_SCOPE("Mission::init buildStaticShadowCasterSet");
	  ObjectManager->buildStaticShadowCasterSet(); }
	mission_phase_mark("gpu_finalize_ready"); // LOAD-PHASE-FACTS-1: geometry finalize + indirect-buffer build + light-bake prewarm done

	// Vegetation card system — notify adapter that terrain + move map are ready.
	// land and GameMap are both stable by this point (terrain init + MOVE_readData
	// both completed above). No-op when MC2_VEGETATION_CARDS is unset.
	const auto vegetationStart = std::chrono::steady_clock::now();
	{ MC2_LOAD_SCOPE("finalize.vegetation"); GameAdapters::Vegetation::missionLoaded(land, GameMap); }
	if (mc2_diag::tagRequested("LOAD_TIMING"))
		mc2::fmt::print_out("[LOAD_TIMING] load=Vegetation::missionLoaded elapsed_ms={:.3f}\n",
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - vegetationStart).count());
    // Camera activation selects this same source later. Select it before the
    // load barrier so sky/water upload the mission HDRI rather than the boot sky.
    GameAdapters::Sky::setSkyNumber(static_cast<int>(theSkyNumber));
    // The load barrier bakes the static shadows (F1); they key on the terrain sun, which the first frame's camera sets. Set the same one now.
    { const glm::vec3 sun = eye->sunDirection(); gos_SetTerrainLightDir(sun.x, sun.y, sun.z);
      MC2_LOAD_SCOPE("finalize.forestOcclusion"); GpuStaticPropRegistry::bakeForestOcclusion(sun.x, sun.y, sun.z); } // forest light bake needs the sun
    // Effect specifications register their images before actors spawn. Admit
    // those images now, including effects first seen later along the camera path.
    { ZoneScopedN("TextureSource.PrewarmEffectTextures"); MC2_LOAD_SCOPE("TextureSource.PrewarmEffectTextures");
      if (auto* pool = MidLevelRenderer::MLRTexturePool::Instance) pool->ForceLoadImages(); }

	if (missionCamera) missionCamera->prepareCompass();
	gos_SetHudScaleActive(true);
	gos_SetHudCanvasActive(true);
	if (missionInterface) missionInterface->getControlGui()->prepareHudFrame();
	{ MC2_LOAD_SCOPE("finalize.prewarmCpuTextures"); mc2::textures::prewarmCpuTextures(); }
	{ MC2_LOAD_SCOPE("finalize.prewarmFirstFrame"); prewarmMoverGpuTextures(*ObjectManager); prewarmHudSheets(); }
    { MC2_LOAD_SCOPE("finalize.prewarmPipelines"); mc2PrewarmMissionPipelines(); }
	mission_phase_mark("render_prewarm_ready");
}

