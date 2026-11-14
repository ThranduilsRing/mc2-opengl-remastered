// MissionInterfaceManager: test-only fixtures and determinism verbs (spawn, teleport, freeze_movers, set_light, animate).
// Declared in missiongui.h; shared names in missiongui_internal.h.
#include <atomic>
#include "missiongui_internal.h"
#include "terrobj.h"
#include "bldng.h"
#include "../mclib/texture_assets.h"
#include "Gates/gate.h"
#include "Gates/preferences_values.h"
#include "mc2fmt.h"
#include "../RenderCore/TextureService.h"
#include "../GameOS/gameos/battle_bench.h"
#include "package_chassis.h"
#include "logistics.h"
#include "missionbegin.h"
#include "../GuiRuntime/PlayerFrontEnd.h"

extern bool justStartMission;
extern char missionName[1024];
namespace mc2vk { size_t deferredDestroyDepth(); }

namespace {
struct AbortReloadFixture {
    std::vector<std::string> targets;
    size_t cycle = 0;
    int phase = 0;
    std::chrono::steady_clock::time_point since;
    std::string current = "mc2_01", campaignFirst;
} abortReload;
}

// Remains live while the mission command channel is down in the front end.
void mc2AbortReloadFixtureUpdate()
{
    auto& f = abortReload;
    if (!f.phase) return;
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - f.since).count();
    if (f.phase == 1 && mission->isActive() && elapsed >= 60 && scenarioTime >= 60 && !eye->inMovieMode) {
        auto* iface = mission->missionInterface;
        if (!iface->isPaused()) iface->togglePause();
        iface->getControlGui()->getPauseWnd()->handleClick(PauseWindow::RESTART);
        f.phase = 2;
    } else if (f.phase == 2) {
        auto* pw = mission->missionInterface->getControlGui()->getPauseWnd();
        if (pw->confirmDialogUp() && LogisticsOKDialog::instance()->getEnterAnim().isDone()) {
            LogisticsOKDialog::instance()->handleMessage(aMSG_LEFTMOUSEDOWN, LogisticsScreen::YES);
            if (mc2_diag::tagRequested("ABORT_RELOAD")) std::fprintf(stderr, "[ABORT_RELOAD] cycle=%zu phase=abort mission=%s play_s=%.3f sim_s=%.3f\n",
                         f.cycle + 1, f.current.c_str(), elapsed, static_cast<double>(scenarioTime));
            f.phase = 3;
        }
    } else if (f.phase == 3 && !mission->isActive()) {
        if (mc2_diag::tagRequested("ABORT_RELOAD")) std::fprintf(stderr, "[ABORT_RELOAD] cycle=%zu phase=unload mission=%s deferred=%zu\n",
                     f.cycle + 1, f.current.c_str(), mc2vk::deferredDestroyDepth());
        logistics->start(log_SPLASH);
        logistics->getMissionBegin()->beginSplash();
        f.since = now;
        f.phase = 4;
    } else if (f.phase == 4 && elapsed >= 2) {
        const std::string next = f.targets[f.cycle++];
        if (mc2_diag::tagRequested("ABORT_RELOAD")) std::fprintf(stderr, "[ABORT_RELOAD] cycle=%zu phase=menu next=%s\n", f.cycle, next.c_str());
        if (next == "campaign") {
            PlayerFrontEnd::requestCampaignStart("campaign");
            f.phase = 5;
        } else {
            logistics->getMissionBegin()->end();
            std::strcpy(missionName, next.c_str());
            LogisticsData::instance->setSingleMission(next.c_str());
            LogisticsData::instance->setSkipLogistics(true);
            logistics->stop();
            Logistics::beginMission(nullptr, 0, nullptr);
            logistics->setLogisticsState(log_DONE);
            f.current = next;
            f.since = std::chrono::steady_clock::now();
            f.phase = f.cycle < f.targets.size() ? 1 : 0;
        }
    } else if (f.phase == 5 && mission->isActive() && scenarioTime >= 3 && eye->inMovieMode) {
        eye->forceMovieToEnd(); // Same request as ESC; let the mission script restore the HUD before winning.
    } else if (f.phase == 5 && mission->isActive() && scenarioTime >= 3 && !eye->inMovieMode) {
        f.campaignFirst = LogisticsData::instance->getCurrentMission();
        if (mc2_diag::tagRequested("ABORT_RELOAD")) std::fprintf(stderr, "[ABORT_RELOAD] phase=campaign_win mission=%s\n", f.campaignFirst.c_str());
        mission->forceTerminate(mis_PLAYER_WIN_BIG);
        f.phase = 6;
    } else if (f.phase == 6 && mission->isActive() &&
               LogisticsData::instance->getCurrentMission() != f.campaignFirst) {
        if (mc2_diag::tagRequested("ABORT_RELOAD")) std::fprintf(stderr, "[ABORT_RELOAD] phase=complete cycles=%zu campaign_next=%s\n",
                     f.cycle, LogisticsData::instance->getCurrentMission().c_str());
        f.phase = 0;
        gos_TerminateApplication();
    }
}


extern float MapCellDiagonal;

// Test-only loadout queue. Production fire, projectile and movement paths own the measurements.
namespace {
struct EquipmentProbe {
    long weapon, defense, engine, armor;
};
std::vector<EquipmentProbe> equipmentQueue;
BattleMechPtr equipmentSource = nullptr, equipmentTarget = nullptr;
glm::vec3 equipmentOrigin(0.0f), equipmentPrevious(0.0f);
float equipmentStarted = -1.0f, equipmentSampleTime = 0.0f, equipmentTopSpeed = 0.0f;
long equipmentBirths = 0, equipmentIntercepts = 0, equipmentAmmo = 0, equipmentDefenseAmmo = 0;
long equipmentArmorBefore = 0, equipmentFireResult = -1;
long equipmentChassis = -1;
bool equipmentRecording = false;
long equipmentWeaponIndex = -1, equipmentAttempts = 0, equipmentHits = 0, equipmentDebits = 0;
float equipmentLastShot = -1.0f;
std::string_view equipmentRefusal = "none";
long probeArmor(BattleMechPtr mech) {
    long sum = 0;
    for (int i = 0; i < mech->numArmorLocations; ++i) sum += mech->armor[i].maxArmor;
    return sum;
}
long probeAmmo(BattleMechPtr mech, long component) {
    return component ? mech->tallyAmmo(MasterComponent::masterList[component].getWeaponAmmoMasterId()) : 0;
}
}

void mc2EquipmentProbeEvent(std::string_view event, unsigned long target) {
    if (equipmentRecording && event == "birth") ++equipmentBirths;
    else if (equipmentRecording && equipmentSource && target == equipmentSource->getWatchID(false)) equipmentRefusal = event;
    if (equipmentStarted >= 0.0f && equipmentTarget && event == "intercept" &&
        target == equipmentTarget->getWatchID(false)) ++equipmentIntercepts;
}

void mc2EquipmentProbeTick() {
    if (equipmentQueue.empty() || !equipmentSource || !equipmentTarget) return;
    auto& job = equipmentQueue.front();
    auto* source = equipmentSource;
    auto* target = equipmentTarget;
    if (equipmentStarted < 0.0f) {
        mc2::options::runtimePreferences().useUnlimitedAmmo = false; // Probe finite ammo, independent of player settings.
        long baseline[] = {1};
        source->resetComponents(1, baseline);
        equipmentArmorBefore = probeArmor(source);
        long loadout[] = {job.engine, job.weapon, job.armor};
        long count = job.armor ? 3 : job.weapon ? 2 : 1;
        source->resetComponents(count, loadout);
        long defense[] = {1, job.defense};
        target->resetComponents(job.defense ? 2 : 1, defense);
        equipmentBirths = equipmentIntercepts = 0;
        equipmentAmmo = probeAmmo(source, job.weapon);
        equipmentDefenseAmmo = probeAmmo(target, job.defense);
        equipmentFireResult = -1;
        equipmentWeaponIndex = -1;
        equipmentAttempts = equipmentHits = equipmentDebits = 0;
        equipmentLastShot = -1.0f;
        for (long i = source->numOther; i < source->numOther + source->numWeapons; ++i)
            if (source->inventory[i].masterID == job.weapon) { equipmentWeaponIndex = i; break; }
        const long ammoIndex = equipmentWeaponIndex >= 0 ? source->inventory[equipmentWeaponIndex].ammoIndex : -1;
        const long ammoMaster = job.weapon ? MasterComponent::masterList[job.weapon].getWeaponAmmoMasterId() : 0;
        const bool binding = !job.weapon || (equipmentWeaponIndex >= 0 && ammoIndex >= 0 &&
            ammoIndex < source->getNumAmmoTypes() && source->getAmmoType(ammoIndex) == ammoMaster);
        mc2::fmt::print(stderr, "[EQUIPMENT_PROBE] binding weapon={} index={} actual={} ammo_index={} ammo_master={} valid={}\n",
            job.weapon, equipmentWeaponIndex, equipmentWeaponIndex >= 0 ? source->inventory[equipmentWeaponIndex].masterID : -1,
            ammoIndex, ammoMaster, binding);
        equipmentTopSpeed = 0.0f;
        source->setPosition(equipmentOrigin);
        auto pos = equipmentOrigin;
        extern float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2];
        const auto range = job.weapon ? MasterComponent::masterList[job.weapon].getWeaponRange() : WEAPON_RANGE_SHORT;
        pos.y += (WeaponRanges[range][0] + WeaponRanges[range][1]) * 0.5f * worldUnitsPerMeter;
        pos.z = land->getTerrainElevation(pos);
        target->setPosition(pos);
        // Keep this stationary test target alive across many real production volleys.
        for (int i = 0; i < target->numArmorLocations; ++i) target->armor[i].curArmor = 100000.0f;
        source->getPilot()->orderWait(false, ORDER_ORIGIN_PLAYER);
        target->getPilot()->orderWait(false, ORDER_ORIGIN_PLAYER);
        source->getPilot()->setSkill(MWS_GUNNERY, 100);
        for (long i = source->numOther; i < source->numOther + source->numWeapons; ++i)
            source->inventory[i].disabled = true;
        if (!job.weapon) {
            pos = equipmentOrigin;
            pos.x += 250.0f * worldUnitsPerMeter;
            pos.z = land->getTerrainElevation(pos);
            source->getPilot()->orderMoveToPoint(false, true, ORDER_ORIGIN_PLAYER, pos, -1, TACORDER_PARAM_RUN);
        }
        equipmentPrevious = source->getPosition();
        equipmentStarted = equipmentSampleTime = scenarioTime;
        mc2::fmt::print(stderr, "[EQUIPMENT_PROBE] begin weapon={} defense={} engine={} armor={} source={} target={}\n",
            job.weapon, job.defense, job.engine, job.armor, source->getPartId(), target->getPartId());
    }
    const float dt = scenarioTime - equipmentSampleTime;
    if (dt > 0.0f) {
        const auto delta = source->getPosition() - equipmentPrevious;
        equipmentTopSpeed = std::max(equipmentTopSpeed, glm::length(glm::vec2(delta.x, delta.y)) / (dt * worldUnitsPerMeter));
        equipmentPrevious = source->getPosition();
        equipmentSampleTime = scenarioTime;
    }
    const bool streak = job.weapon && MasterComponent::masterList[job.weapon].getWeaponStreak();
    const long volleys = streak ? 20 : 1;
    if (job.weapon && equipmentWeaponIndex >= 0 && equipmentAttempts < volleys &&
        scenarioTime - equipmentStarted >= 3.0f &&
        (equipmentLastShot < 0.0f || scenarioTime - equipmentLastShot >=
            MasterComponent::masterList[job.weapon].getWeaponRecycleTime() + 0.1f)) {
        const long index = equipmentWeaponIndex;
        const long before = probeAmmo(source, job.weapon), births = equipmentBirths;
        const long ammoIndex = source->inventory[index].ammoIndex;
        const bool binding = ammoIndex >= 0 && ammoIndex < source->getNumAmmoTypes() &&
            source->getAmmoType(ammoIndex) == MasterComponent::masterList[job.weapon].getWeaponAmmoMasterId();
        equipmentRefusal = "none";
        source->inventory[index].disabled = false;
        equipmentRecording = true;
        float damage = 0.0f;
        equipmentFireResult = source->fireWeapon(target, 0.0f, index, 0, MECH_ARMOR_LOCATION_CTORSO, nullptr, damage);
        equipmentRecording = false;
        source->inventory[index].disabled = true;
        ++equipmentAttempts;
        equipmentHits += damage > 0.0f;
        const long debit = before - probeAmmo(source, job.weapon);
        equipmentDebits += debit;
        equipmentLastShot = scenarioTime;
        mc2::fmt::print(stderr, "[EQUIPMENT_PROBE] shot weapon={} index={} actual={} attempt={} target_valid={} distance={:.4f} in_range={} binding={} fire={} refusal=mech_weapons.cpp/{} births={} debit={} hit={} damage={:.4f}\n",
            job.weapon, index, source->inventory[index].masterID, equipmentAttempts, !target->isDestroyed(),
            source->distanceFrom(target->getPosition()), source->weaponInRange(index, source->distanceFrom(target->getPosition()), MapCellDiagonal),
            binding, equipmentFireResult, equipmentRefusal, equipmentBirths - births, debit, damage > 0.0f, damage);
    }
    if (scenarioTime - equipmentStarted < (streak ? 180.0f : 12.0f) &&
        (!streak || equipmentAttempts < volleys || scenarioTime - equipmentLastShot < 9.0f)) return;
    mc2::fmt::print(stderr, "[EQUIPMENT_PROBE] result weapon={} defense={} engine={} armor={} fire={} births={} intercepts={} ammo={}->{} defense_ammo={}->{} top_mps={:.4f} max_mps={:.4f} armor_total={}->{} engine_master={} attempts={} hits={} debits={}\n",
        job.weapon, job.defense, job.engine, job.armor, equipmentFireResult, equipmentBirths, equipmentIntercepts,
        equipmentAmmo, probeAmmo(source, job.weapon), equipmentDefenseAmmo, probeAmmo(target, job.defense),
        equipmentTopSpeed, source->calcMaxSpeed(), equipmentArmorBefore, probeArmor(source), source->inventory[source->engine].masterID,
        equipmentAttempts, equipmentHits, equipmentDebits);
    equipmentQueue.erase(equipmentQueue.begin());
    equipmentStarted = -1.0f;
    if (equipmentQueue.empty()) mc2::fmt::print(stderr, "[EQUIPMENT_PROBE] complete\n");
}

//--------------------------------------------------------------------------
// FREEZE-MOVERS-1 (GAMEPLAY-DRIVER-3): deterministic mover freeze.
//
// WHY: any capture pose that frames a mover drifts run-to-run, because
// frameLength is wall-clock and every mover integrates against it. That
// produced a RETRACTED "+14.8 luma brighter" result which was really a turret
// barrel resting at a different aim angle in the two arms. Until movers can be
// pinned, no A/B that contains a vehicle or a mech is byte-comparable, so
// "vehicles look different from mechs" cannot be measured at all.
//
// HOW: this does NOT add a second freeze mechanism. Mission::update already has
// a proven "sim frozen, rendering continues" branch used by the pause path ---
// ObjectManager::updateAppearancesOnly(), code/objmgr.cpp. That branch
// still runs recalcBounds + setObjectParameters + updateGeometry (so the actor
// draws), calls Appearance::update(false) (so the gait does NOT advance), and
// deliberately never calls setMoverParameters (so torso twist and turret aim
// hold their last value --- exactly the drift that caused the retraction).
// All this verb does is widen that branch's predicate. See code/mission.cpp.
//
// CROSS-BUILD, not merely cross-run: `freeze_movers on <frame>` ARMS the freeze
// and engages it the first frame g_mc2FrameCounter >= <frame>, rather than
// whenever the driver's write happened to land. Two builds running the same
// mission with MC2_SMOKE_FIXED_TIMESTEP=1 advance sim by a fixed 1/30 per
// frame (code/mission.cpp, code/mechcmd2.cpp), so "frame N" is the
// same simulated instant in both --- which is what makes a pose reproducible
// across a rebuild instead of only within one process. Freezing at a
// wall-clock moment gives neither.
//
// Default: unset/off. mc2FreezeMoversActive() returns 0 and the predicate at
// mission.cpp is unchanged, so stock behaviour is byte-identical.
static bool s_freezeMoversOn      = false;
static long s_freezeMoversAt      = -2;   // -2 uninitialised, -1 disarmed, >=0 armed
static long s_freezeMoversEngaged = -1;   // MISSION frame the latch actually fired
// g_mc2FrameCounter is process-lifetime: it starts at 0 once and is never reset
// per mission (GameOS/gameos/gameosmain.cpp). Arming against it directly would
// make "<frame>" mean a different point in mission B than in mission A, which
// defeats the whole reproducibility claim, and would let a freeze armed in one
// mission fire in the next. So the latch works in MISSION-RELATIVE frames off
// this baseline, re-stamped by mc2FreezeMoversMissionReset() at Mission::init.
static long s_freezeMoversBase    = 0;

// Must be declared at FILE scope, not inside the extern "C" bodies below: a
// declaration inside an extern "C" function inherits C linkage and would look
// for an unmangled `g_mc2FrameCounter`, while the definition (mechcmd2.cpp) has
// C++ linkage (?g_mc2FrameCounter@@3IA). That mismatch is a link error, not a
// compile error, so it only surfaces at the very end of a full build.
extern std::atomic<uint32_t> g_mc2FrameCounter;

// Consulted by code/mission.cpp (the sim branch) and by the residual-drift
// sites in mclib/mech3d.cpp + mclib/gvactor.cpp. Self-latching so it does not
// matter which of them runs first in a frame.
// Frames since Mission::init. THIS is what `freeze_movers on <frame>` counts.
extern "C" long mc2FreezeMoversMissionFrame()
{
	return static_cast<long>(g_mc2FrameCounter.load(std::memory_order_relaxed)) - s_freezeMoversBase;
}

// Called from Mission::init so a freeze is scoped to the mission that armed it
// and <frame> means the same instant in every mission and every build. Without
// this a campaign or a multi-mission harness run could start mission B already
// frozen, with no command in mission B's own session having asked for it.
extern "C" void mc2FreezeMoversMissionReset()
{
    equipmentQueue.clear();
    equipmentSource = equipmentTarget = nullptr;
    equipmentStarted = -1.0f;
    equipmentRecording = false;
    equipmentChassis = -1;
	s_freezeMoversBase    = static_cast<long>(g_mc2FrameCounter.load(std::memory_order_relaxed));
	s_freezeMoversOn      = false;
	s_freezeMoversEngaged = -1;
	s_freezeMoversAt      = -2;   // re-read env so the gate re-arms per mission
}

extern "C" int mc2FreezeMoversActive()
{
	if (s_freezeMoversAt == -2) {
		// MC2_FREEZE_MOVERS_AT=<frame> lets a capture run freeze WITHOUT driving
		// the command channel at all. Unset -> -1 -> never engages.
		s_freezeMoversAt = -1;   // row default -1 = never engages
	}
	const long mf = mc2FreezeMoversMissionFrame();
	if (!s_freezeMoversOn && s_freezeMoversAt >= 0 && mf >= s_freezeMoversAt) {
		s_freezeMoversOn = true;
		s_freezeMoversEngaged = mf;
	}
	return s_freezeMoversOn ? 1 : 0;
}

// on<0 => disarm+thaw. atFrame<0 => engage immediately at the current frame.
extern "C" void mc2FreezeMoversSet(int on, long atFrame)
{
	if (s_freezeMoversAt == -2)
		(void)mc2FreezeMoversActive();     // fold in the env default first
	if (!on) {
		s_freezeMoversOn = false;
		s_freezeMoversAt = -1;
		s_freezeMoversEngaged = -1;
		return;
	}
	if (atFrame >= 0) {                    // arm for a deterministic frame
		s_freezeMoversAt = atFrame;
		s_freezeMoversOn = false;
		s_freezeMoversEngaged = -1;
		(void)mc2FreezeMoversActive();     // engage now if already past it
	} else {
		s_freezeMoversAt = -1;
		s_freezeMoversOn = true;
		s_freezeMoversEngaged = mc2FreezeMoversMissionFrame();
	}
}

extern "C" long mc2FreezeMoversArmedAt()   { return s_freezeMoversAt; }
extern "C" long mc2FreezeMoversEngagedAt() { return s_freezeMoversEngaged; }

void mc2BenchWalk(GameObjectManager& om, int rounds);   // code/objmgr_misc.cpp

// Test-only fixtures and determinism verbs (spawn, teleport, freeze_movers, set_light, animate). Returns true when a verb of this group matched.
bool MissionInterfaceManager::uiCmdFixture(UiCmdCtx& c)
{
    if (c.line.starts_with("equipment_probe ")) {
        long chassis = 0;
        EquipmentProbe job{};
        c.result = "blocked";
        c.note = "equipment_probe <chassis|0=clone> <weapon|0> <AMS|0> <engine> <armor|0>; needs MC2_TEST_SPAWN=1";
        if (!mc2::gate::get<"MC2_TEST_SPAWN">() || !ObjectManager || !Team::home ||
            sscanf(c.line.c_str() + 16, "%ld %ld %ld %ld %ld", &chassis, &job.weapon, &job.defense, &job.engine, &job.armor) != 5) return true;
        if (equipmentChassis != -1 && chassis != equipmentChassis) return true;
        for (long id : {job.weapon, job.defense, job.engine, job.armor})
            if (id < 0 || id >= MasterComponent::numComponents) return true;
        if (MasterComponent::masterList[job.engine].getForm() != COMPONENT_FORM_ENGINE ||
            (job.weapon && MasterComponent::masterList[job.weapon].getForm() != COMPONENT_FORM_WEAPON_MISSILE) ||
            (job.weapon && MasterComponent::masterList[job.weapon].isDefensiveWeapon()) ||
            (job.defense && !MasterComponent::masterList[job.defense].isDefensiveWeapon()) ||
            (job.armor && MasterComponent::masterList[job.armor].getForm() != COMPONENT_FORM_BULK)) return true;
        if (!equipmentSource) {
            MoverPtr model = nullptr;
            for (long i = 0; i < ObjectManager->getNumMovers(); ++i) {
                auto* mover = ObjectManager->getMover(i);
                if (mover && mover->isMech() && mover->getTeamId() == Team::home->getId()) { model = mover; break; }
            }
            if (!model) return true;
            equipmentOrigin = model->getPosition();
            for (int i = 0; i < 2; ++i) {
                const long before = ObjectManager->getNumMovers();
                const auto command = std::format("spawn {} {} {} {}", chassis ? std::to_string(chassis) : "clone",
                    equipmentOrigin.x, equipmentOrigin.y, Team::home->getId());
                UiCmdCtx spawn{command};
                uiCmdFixture(spawn);
                if (std::string_view(spawn.result) != "ok" || ObjectManager->getNumMovers() != before + 1) return true;
                auto* mech = dynamic_cast<BattleMech*>(ObjectManager->getMover(before));
                if (!mech) return true;
                if (i == 0) equipmentSource = mech; else equipmentTarget = mech;
            }
            equipmentChassis = chassis;
        }
        equipmentQueue.push_back(job);
        c.result = "ok";
        c.note = "queued production salvo/movement measurement";
        return true;
    }

	if (c.line.starts_with("abort_reload ")) {
		const std::string next = c.line.substr(13);
		if (!mc2::gate::get<"MC2_TEST_SPAWN">() ||
			(next != "mc2_01" && next != "mc2_10" && next != "mc2_17" && next != "mc2_03" && next != "campaign")) {
			c.result = "blocked"; c.note = "abort_reload <mc2_01|mc2_10|mc2_17|mc2_03|campaign> needs MC2_TEST_SPAWN=1";
		} else {
			abortReload.targets.push_back(next);
			if (!abortReload.phase) {
				justStartMission = false;
				abortReload.current = missionName;
				abortReload.since = std::chrono::steady_clock::now();
				abortReload.phase = 1;
			}
			c.result = "ok";
		}
		return true;
	}
	if (c.line.starts_with("force_state ")) {
		// force_state <partId> sensor <0..4> | armoff left|right  (needs MC2_TEST_SPAWN=1; pair with freeze_movers so the sim does not rewrite it)
		long pid = 0; char what[16] = {0}, arg[16] = {0};
		GameObjectPtr obj;
		if (!mc2::gate::get<"MC2_TEST_SPAWN">()) {
			c.result = "blocked"; c.note = "needs MC2_TEST_SPAWN=1";
		} else if (sscanf(c.line.c_str() + 12, "%ld %15s %15s", &pid, what, arg) != 3 || !(obj = ObjectManager->findByPartId(pid)) || !obj->getAppearance()) {
			c.result = "blocked"; c.note = "force_state <partId> sensor <n> | armoff left|right";
		} else if (!strcmp(what, "sensor")) {
			obj->getAppearance()->setSensorLevel(atol(arg));
			c.result = "ok";
		} else if (!strcmp(what, "armoff")) {
			if (!strcmp(arg, "left")) obj->getAppearance()->blowLeftArm(); else obj->getAppearance()->blowRightArm();
			c.result = "ok";
		} else if (obj->getObjectClass() == GROUNDVEHICLE && !strcmp(what, "destroy")) {
			obj->getObjectType()->handleDestruction(obj, nullptr);
			c.result = obj->isDestroyed() ? "ok" : "blocked";
			c.note = "destroyed=" + std::to_string(obj->isDestroyed());
		} else if (obj->getObjectClass() == BUILDING && (!strcmp(what, "team") || !strcmp(what, "destroy"))) {
			// Exercise the production capture/destruction writers, including their dirty notification.
			const bool before = mc2::terrainDirtyQueue().isRenderResident(obj->getHandle());
			if (!strcmp(what, "team")) obj->setTeamId(atol(arg), false);
			else static_cast<Building*>(obj)->setDamage(static_cast<Building*>(obj)->getDamageLevel());
			c.result = "ok";
			c.note = "renderResident=" + std::to_string(before) + "->" +
				std::to_string(mc2::terrainDirtyQueue().isRenderResident(obj->getHandle())) +
				" team=" + std::to_string(obj->getTeamId()) + " destroyed=" + std::to_string(obj->isDestroyed());
		} else {
			c.result = "blocked"; c.note = "force_state <partId> sensor <n> | armoff left|right";
		}
		return true;
	}
	if (c.line.starts_with("airstrike ")) {
		float x = 0, y = 0;
		if (!mc2::gate::get<"MC2_TEST_SPAWN">() || sscanf(c.line.c_str() + 10, "%f %f", &x, &y) != 2) {
			c.result = "blocked"; c.note = "airstrike <x> <y> needs MC2_TEST_SPAWN=1";
		} else {
			glm::vec3 pos(x, y, land ? land->getTerrainElevation(glm::vec3(x, y, 0)) : 0);
			IfaceCallStrike(ARTILLERY_LARGE, &pos, nullptr, true, false, 3.0f);
			c.result = "ok";
		}
		return true;
	}
	if (c.line.starts_with("support ")) {
		int id = 0; float x = 0, y = 0;
		if (!mc2::gate::get<"MC2_TEST_SPAWN">()) {
			c.result = "blocked"; c.note = "needs MC2_TEST_SPAWN=1";
		} else if (sscanf(c.line.c_str() + 8, "%d %f %f", &id, &x, &y) != 3 ||
			!Commander::home || !controlGui.getVehicleNameFromID(id)) {
			c.note = "support <vehicleID> <x> <y>";
		} else {
			glm::vec3 pos(x, y, 0.0f);
			if (land) pos.z = land->getTerrainElevation(pos);
			// Recovery needs a disabled target; use the selected mover for the fixture.
			MoverPtr salvage = nullptr;
			if (id == 147 && Team::home) {
				for (long i = 0; i < Team::home->getRosterSize(); ++i) {
					auto* mover = Team::home->getMover(i);
					if (mover && mover->isSelected()) { salvage = mover; break; }
				}
			}
			if (id == 147 && !salvage) {
				c.result = "blocked"; c.note = "select recovery target";
			} else {
				beginVtol(id, Commander::home->getId(), &pos, salvage);
				c.result = "ok";
			}
		}
		return true;
	}
	const std::string& line = c.line;
	const char*& result = c.result;
	std::string& note = c.note;
	std::string& extra = c.extra;
	if (line.rfind("bench_output ", 0) == 0) {
		result = mc2::gate::get<"MC2_TEST_SPAWN">() && mc2::battlebench::arm(line.c_str() + 13) ? "ok" : "blocked";
		return true;
	}
	if (line == "bench_attack") {
		if (mc2::gate::get<"MC2_TEST_SPAWN">()) { mc2::battlebench::attack = true; result = "ok"; }
		else result = "blocked";
		return true;
	}
	if (line.rfind("bench_freeze ", 0) == 0) {   // freeze the sim tick (existing freeze_movers path) and pin the camera
		const std::string arg = line.substr(13);
		if (!mc2::gate::get<"MC2_TEST_SPAWN">() || (arg != "on" && arg != "off")) result = "blocked";
		else {
			mc2FreezeMoversSet(arg == "on" ? 1 : 0, -1);
			if (arg == "on" && eye) eye->freezeScriptedCameraForCapture();
			result = "ok";
		}
		return true;
	}
	if (line.rfind("bench_walk ", 0) == 0) {     // prints [WALK_BENCH v1] lines to stderr
		int n = 0;
		if (!mc2::gate::get<"MC2_TEST_SPAWN">() || !ObjectManager || sscanf(line.c_str() + 11, "%d", &n) != 1 || n < 1) result = "blocked";
		else { mc2BenchWalk(*ObjectManager, n); result = "ok"; }
		return true;
	}
	if (line.rfind("bench_view ", 0) == 0) {
		float x = 0, y = 0, yaw = 0, altitude = 0, pitch = 0;
		if (!mc2::gate::get<"MC2_TEST_SPAWN">() || !eye ||
			sscanf(line.c_str() + 11, "%f %f %f %f %f", &x, &y, &yaw, &altitude, &pitch) != 5) result = "blocked";
		else {
			glm::vec3 pos(x, y, 0); if (land) pos.z = land->getTerrainElevation(pos);
			eye->setPosition(pos, false); eye->setRotation(glm::vec3(pitch, yaw, altitude));
			eye->freezeScriptedCameraForCapture(); result = "ok";
			if (mc2::battlebench::output) std::fprintf(mc2::battlebench::output,
				"{\"type\":\"view\",\"frame\":%llu,\"sim_s\":%.6f,\"pos\":[%.3f,%.3f],\"yaw\":%.3f,\"alt\":%.3f,\"pitch\":%.3f}\n",
				static_cast<unsigned long long>(mc2::battlebench::frame), static_cast<double>(scenarioTime), x, y, yaw, altitude, pitch);
		}
		return true;
	}

    if (line.rfind("texture_retire ",0)==0) {
        unsigned long long id=0;
        auto& service=RenderCore::textureService();
        if (!mc2::gate::get<"MC2_TEST_SPAWN">()) { result="blocked";note="needs MC2_TEST_SPAWN=1"; }
        else if (sscanf(line.c_str()+15,"%llu",&id)!=1) note="texture_retire <service UI id>";
        else {
            const auto handle=RenderCore::uiTextureHandle(id);
            // Fault injection is restricted to one service-owned page: never steal a manager/node reference.
            if (!service.isLive(handle) || service.nodeOf(handle)!=RenderCore::kNoTextureNode || service.users(handle)!=1) {
                result="blocked";note="needs a live sole-owned page; users="+std::to_string(service.users(handle));
            } else {
                result=service.release(handle) && !service.isLive(handle) ? "ok" : "blocked";
                note="page retired; live producer must request a new generation";
            }
        }
        return true;
    }
    if (line.rfind("paint ",0)==0) {
        long id=0; unsigned r=0,g=0,b=0;
        if (!mc2::gate::get<"MC2_TEST_SPAWN">()) { result="blocked";note="needs MC2_TEST_SPAWN=1"; }
        else if (sscanf(line.c_str()+6,"%ld %x %x %x",&id,&r,&g,&b)!=4) note="paint <partId> <red> <green> <blue> (hex)";
        else {
            auto* obj=ObjectManager ? ObjectManager->findByPartId(id) : nullptr;
            auto* app=obj ? obj->getAppearance() : nullptr;
            if (!app) { result="blocked";note="no appearance"; }
            else {
                app->resetPaintScheme(r,g,b);
                DWORD rr=0,gg=0,bb=0;app->getPaintScheme(rr,gg,bb);
                result=(rr==r && gg==g && bb==b) ? "ok" : "blocked";
                note="resetPaintScheme readback="+std::to_string(rr)+","+std::to_string(gg)+","+std::to_string(bb);
            }
        }
        return true;
    }
    if (line.rfind("material_copy ",0)==0 || line.rfind("material_state ",0)==0) {
        const bool copy=line.rfind("material_copy ",0)==0;
        long id=0,sourceId=0;unsigned slot=0;
        auto shapeOf=[](GameObjectPtr object)->TG_MultiShape* {
            auto* app=object ? object->getAppearance() : nullptr;
            if (auto* building=dynamic_cast<BldgAppearance*>(app)) return building->bldgShape;
            if (auto* tree=dynamic_cast<TreeAppearance*>(app)) return tree->treeShape;
            return nullptr;
        };
        if (!mc2::gate::get<"MC2_TEST_SPAWN">()) { result="blocked";note="needs MC2_TEST_SPAWN=1";return true; }
        const int count=sscanf(line.c_str()+(copy?14:15),"%ld %ld %u",&id,&sourceId,&slot);
        auto* obj=ObjectManager ? ObjectManager->findByPartId(id) : nullptr;
        auto* shape=shapeOf(obj);
        if (!shape || (copy && count<2) || slot>=static_cast<unsigned>(shape->GetNumTextures())) { result="blocked";note="material_copy <dstPartId> <srcPartId> [slot], material_state <partId>";return true; }
        if (copy) {
            auto* source=shapeOf(ObjectManager->findByPartId(sourceId));
            if (!source || slot>=static_cast<unsigned>(source->GetNumTextures()) || !mc2::textures::ready()) { result="blocked";note="source has no slot";return true; }
            const DWORD node=source->GetTextureHandle(slot),old=shape->GetTextureHandle(slot);
            if (node==0xffffffffu || node==old || !mc2::textures::sourceId(node)) { result="blocked";note="source slot must name different resident art";return true; }
            mc2::textures::retain(node); // adopts one manager/service reference for the destination
            shape->SetTextureHandle(slot,node);
            if (old!=0xffffffffu) mc2::textures::release(old);
            if (auto* building=dynamic_cast<BldgAppearance*>(obj->getAppearance())) building->needsFullBakeNextFrame=true;
            if (auto* tree=dynamic_cast<TreeAppearance*>(obj->getAppearance())) tree->needsFullBakeNextFrame=true;
        }
        shape->BindMaterials();result="ok";
        note="material_set="+std::to_string(shape->materialSet().base)+" node="+std::to_string(shape->GetTextureHandle(slot))+" type_defaults="+std::to_string(shape->usesTypeMaterials());
        return true;
    }
    // Deterministic tree-fall witness: arm the same owner state as a collision.
    // TerrainObject::update integrates the fall and submits its real appearance.
    if (line.rfind("tree_fall ", 0) == 0) {
        float x = 0.0f, y = 0.0f;
        if (!mc2::gate::get<"MC2_TEST_SPAWN">()) {
            result = "blocked"; note = "needs MC2_TEST_SPAWN=1";
        } else if (sscanf(line.c_str() + 10, "%f %f", &x, &y) != 2 || !ObjectManager) {
            note = "tree_fall <world x> <world y> (nearest intact tree within 512)";
        } else {
            TerrainObjectPtr nearest = nullptr;
            float best = 512.0f * 512.0f;
            for (long i = 0; i < ObjectManager->getNumTerrainObjects(); ++i) {
                auto* tree = ObjectManager->getTerrainObject(i);
                if (!tree || tree->getSubType() != TERROBJ_TREE ||
                    tree->getFlag(OBJECT_FLAG_FALLING) || tree->getFlag(OBJECT_FLAG_FALLEN)) continue;
                const auto pos = tree->getPosition();
                const float d = (pos.x - x) * (pos.x - x) + (pos.y - y) * (pos.y - y);
                if (d < best) { best = d; nearest = tree; }
            }
            if (nearest) {
                nearest->setFlag(OBJECT_FLAG_FALLING, true);
                nearest->setTangible(false);
                result = "ok";
                const auto pos = nearest->getPosition();
                note = "tree falling at " + std::to_string(pos.x) + "," + std::to_string(pos.y);
            } else { result = "blocked"; note = "no intact tree within 512"; }
        }
        return true;
    }
	if (line.compare(0, 6, "spawn ") == 0 || line.compare(0, 9, "teleport ") == 0) {
		// GAMEPLAY-DRIVER-1 test-only fixtures (gated MC2_TEST_SPAWN=1).
		//   spawn <clone|vehicle|infantry|objNum> <x> <y> [team] [variant] [pilotProfile]
		//   teleport <partId> <x> <y>
		const bool armed = mc2::gate::get<"MC2_TEST_SPAWN">();
		if (!armed) {
			result = "blocked"; note = "needs MC2_TEST_SPAWN=1";
		}
		else if (line.compare(0, 6, "spawn ") == 0) {
			char kind[40] = {0}, pilotProfile[50] = {0}; float x = 0, y = 0; int team = -1, variant = 0;
			const int nf = sscanf(line.c_str() + 6, "%39s %f %f %d %d %49s",
				kind, &x, &y, &team, &variant, pilotProfile);
			if (nf < 3) {
				note = "spawn <clone|vehicle|infantry|objNum> <x> <y> [team] [variant] [pilotProfile]";
			} else {
				const long homeCmdr = Commander::home ? Commander::home->getId() : 0;
				const long homeTeam = Team::home ? Team::home->getId() : 0;
				MoverInitData data; memset(&data, 0, sizeof(data));
				// Flash is shipped in mission.fst. The prototype's pmw00080 was absent,
				// leaving zero pilot skills and NaN status on every spawned chassis.
				strcpy(data.pilotFileName, "pmp_flash");
				if (nf >= 6) mc2::fmt::to_buf(data.pilotFileName, "{}", pilotProfile);
				strcpy(data.brainFileName, "pbrain");
				data.controlDataType = 0;
				data.rosterIndex = 255;
				data.gestureID = 2;
				data.active = true; data.exists = true; data.capturable = false;
				data.variant = static_cast<DWORD>(variant);
				data.baseColor = prefs.baseColor;
				data.highlightColor1 = prefs.highlightColor;
				data.highlightColor2 = prefs.highlightColor;
				data.commanderID = static_cast<char>(homeCmdr);
				data.teamID = static_cast<char>(((team >= 0) ? team : homeTeam));
				bool ok = true;
				if (strcmp(kind, "infantry") == 0) {
					strcpy(data.brainFileName, "infantry001");
					strcpy(data.csvFileName, "PM101100");
					data.objNumber = 53; data.variant = 0;
				} else if (strcmp(kind, "clone") == 0 || strcmp(kind, "vehicle") == 0) {
					// GAMEPLAY-DRIVER-2: `vehicle` clones an existing GROUND
					// VEHICLE (any team -- missions often have none on the
					// player roster) so a mech and a vehicle can be placed
					// side by side under identical light for a lighting A/B.
					const ObjectClass wantCls =
						(strcmp(kind, "vehicle") == 0) ? GROUNDVEHICLE : BATTLEMECH;
					Mover* tmpl = nullptr;
					Team* pTeam = Team::home;
					for (long i = 0; pTeam && i < pTeam->getRosterSize(); i++) {
						Mover* m = static_cast<Mover*>(pTeam->getMover(i));
						if (m && commanderIdIs(m->getCommander(), homeCmdr) &&
							m->getObjectClass() == wantCls) { tmpl = m; break; }
					}
					if (!tmpl && ObjectManager) {
						for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
							MoverPtr m = ObjectManager->getMover(i);
							if (m && m->getObjectClass() == wantCls) { tmpl = m; break; }
						}
					}
					if (!tmpl) {
						ok = false; result = "blocked";
						note = (wantCls == GROUNDVEHICLE)
							? "no ground vehicle in mission to clone"
							: "no player mech to clone";
					} else {
						data.objNumber = tmpl->getTypeHandle();
						data.variant = (wantCls == BATTLEMECH)
							? static_cast<DWORD>((static_cast<BattleMechPtr>(tmpl))->variantID) : 0;
					}
				} else if (strchr(kind, ':')) {
					// BT2018-FIRST-CLASS P3: a package chassis by qualified id (bt2018:marauder).
					// Its object-type number is a runtime allocation, so drivers name the id.
					const mc2pkg::PackageChassis* pc = packageChassisById(kind);
					if (!pc || pc->objTypeNum < 0) { ok = false; result = "blocked"; note = "no registered package chassis with that id"; }
					else data.objNumber = pc->objTypeNum;
				} else {
					char* end = nullptr; const long on = strtol(kind, &end, 10);
					if (!end || *end != '\0') { ok = false; note = "kind: clone|vehicle|infantry|<objNum>|<ns:chassis>"; }
					else data.objNumber = on;
				}
				if (ok) {
					glm::vec3 pos; pos.x = x; pos.y = y; pos.z = 0.0f;
					if (land) pos.z = land->getTerrainElevation(pos);
					data.position = pos; data.rotation = 0;
					// Tactical orders run through the pilot's AI loop, including home units.
					// CONTROL_PLAYER is the legacy keyboard/animation puppet mode.
					data.controlType = CONTROL_AI;
					const long h = mission ? mission->addMover(&data) : -1;
					MoverPtr nm = (h > 0) ? static_cast<MoverPtr>(ObjectManager->get(h)) : nullptr;
					if (!nm) { result = "blocked"; if (note.empty()) note = "addMover failed"; }
					else {
						// NOTE: do NOT call nm->setOnGUI(true) here. The
						// GAMEPLAY-DRIVER-1 prototype did, but Mission::
						// addMover already sets it (code/mission.cpp, just
						// before it returns the handle), so the extra call
						// was redundant -- and it is a setOnGUI() caller
						// outside the MOVER-COHERENCE S1a allowlist, which
						// the mover-registry contract (added after that
						// prototype was written) correctly rejects: every
						// setOnGUI(true) must be paired with the mission-
						// interface registration that addMover performs.
						if (!MPlayer && data.commanderID == static_cast<char>(homeCmdr))
							nm->setLocalMoverId(0);
						result = "ok";
						char b[192];
						// GAMEPLAY-DRIVER-2 HONESTY: report the spawned unit's
						// ACTUAL status rating in the ack. The 2026-07-23 run
						// acked five `spawn clone` as ok while all five were
						// hp=0.000 and ignored every move order; because
						// scenario_runner only substring-matches acks, the
						// report said ok=True. Surfacing hp here means a
						// driver can assert on it. A non-positive OR NaN hp on
						// a fresh spawn is invalid -- use `unit <id>` for detail.
						// The watch id joins the ack to COMBAT fire events.
						//
						// Written as !(hp > 0) and NOT as (hp <= 0): a
						// spawned BattleMech's getStatusRating() can come
						// back -nan(ind) (observed 2026-07-30, mc2_10,
						// `spawn clone`), and EVERY comparison against NaN
						// is false -- so the `<= 0` form let the exact case
						// the warning exists to catch slip through acking a
						// clean "ok".
						const float spawnHp = nm->getStatusRating();
						mc2::fmt::to_buf(b, "spawned id={:d} obj={:d} team={:d} hp={:.3f} wid={:d}{}", mc2::fmt::as_i32(nm->getPartId()), mc2::fmt::as_i32(static_cast<int>(data.objNumber)), mc2::fmt::as_i32(static_cast<int>(data.teamID)), spawnHp, mc2::fmt::as_u32(nm->getWatchID(false)), mc2::fmt::safe(!(spawnHp > 0.0f)
								? " WARNING:bad hp, spawned unit may be inert" : ""));
						note = b;
					}
				}
			}
		}
		else {   // teleport <partId> <x> <y>
			long pid = 0; float x = 0, y = 0;
			const int nf = sscanf(line.c_str() + 9, "%ld %f %f", &pid, &x, &y);
			if (nf < 3) { note = "teleport <partId> <x> <y>"; }
			else {
				GameObjectPtr obj = ObjectManager->findByPartId(pid);
				if (!obj || !obj->isMover()) { result = "blocked"; note = "no such mover"; }
				else {
					glm::vec3 pos; pos.x = x; pos.y = y; pos.z = 0.0f;
					if (land) pos.z = land->getTerrainElevation(pos);
					(static_cast<Mover*>(obj))->setTeleportPosition(pos);   // engine teleport hook
					result = "ok";
					char b[80]; mc2::fmt::to_buf(b, "teleport {:d} -> ({:.1f},{:.1f})", mc2::fmt::as_i32(pid), x, y); note = b;
				}
			}
		}
	}
	//---------------------------------------------------------------------
	// FREEZE-MOVERS-1: pin every mover so a pose that frames one is
	// byte-comparable. `on <frame>` is the form that matters -- it arms the
	// freeze for a DETERMINISTIC mission frame instead of whenever the
	// driver's write happened to land, which is what makes the pose
	// reproducible across a REBUILD and not merely across two runs of one
	// binary. Pair with MC2_SMOKE_FIXED_TIMESTEP=1 (without it frameLength
	// is wall-clock and frame N is a different simulated instant in each
	// build). Rides the existing pause branch in Mission::update; see the
	// design note at the top of this file.
	else if (line.compare(0, 13, "freeze_movers") == 0 &&
			 (line.size() == 13 || line[13] == ' ')) {
		extern std::atomic<uint32_t> g_mc2FrameCounter;
		char sub[32] = {0};
		long atFrame = -1;
		const int nf = (line.size() > 14)
			? sscanf(line.c_str() + 14, "%31s %ld", sub, &atFrame) : 0;
		const std::string s = (nf >= 1) ? std::string(sub) : std::string();
		char b[288];
		if (s == "off") {
			mc2FreezeMoversSet(0, -1);
			result = "ok";
			note = "thawed; movers resume simulating";
		} else if (s == "on") {
			mc2FreezeMoversSet(1, (nf >= 2) ? atFrame : -1L);
			result = "ok";
			if (nf >= 2)
				mc2::fmt::to_buf(b, "armed for MISSION frame {:d} (engagedAt={:d}, now missionFrame={:d}); REPRODUCIBLE ACROSS BUILDS only with MC2_SMOKE_FIXED_TIMESTEP=1", mc2::fmt::as_i32(mc2FreezeMoversArmedAt()), mc2::fmt::as_i32(mc2FreezeMoversEngagedAt()), mc2::fmt::as_i32(mc2FreezeMoversMissionFrame()));
			else
				mc2::fmt::to_buf(b, "frozen NOW at mission frame {:d} -- not cross-build reproducible; use `freeze_movers on <frame>` for that", mc2::fmt::as_i32(mc2FreezeMoversEngagedAt()));
			note = b;
		} else if (s == "status") {
			mc2::fmt::to_buf(b, "active={:d} armedAt={:d} engagedAt={:d} missionFrame={:d} absFrame={:d}", mc2::fmt::as_i32(mc2FreezeMoversActive()), mc2::fmt::as_i32(mc2FreezeMoversArmedAt()), mc2::fmt::as_i32(mc2FreezeMoversEngagedAt()), mc2::fmt::as_i32(mc2FreezeMoversMissionFrame()), mc2::fmt::as_u32(static_cast<unsigned long>(g_mc2FrameCounter.load(std::memory_order_relaxed))));
			note = b;
			result = "ok";
		} else {
			note = "freeze_movers on [frame] | off | status";
		}
	}
	//---------------------------------------------------------------------
	// SET-LIGHT-1: one named front door for the sun, so the standing
	// day-AND-night verdict rule is expressible in a scenario file instead
	// of depending on which mission happens to SHIP with the sun you need.
	// Delegates to the same two Camera entry points time_of_day / sun_pitch
	// use -- no parallel lighting mechanism.
	else if (line.compare(0, 10, "set_light ") == 0) {
		const std::string arg = line.substr(10);
		char b[256];
		if (!eye) {
			result = "blocked"; note = "no camera";
		} else if (arg.compare(0, 4, "alt ") == 0) {
			float pitch = 0.0f, yaw = 0.0f;
			const int nf = sscanf(arg.c_str() + 4, "%f %f", &pitch, &yaw);
			if (nf < 1) {
				note = "set_light alt <deg 0..360> [yawDeg]";
			} else {
				const float p = eye->setSunPitch(pitch, nf >= 2, yaw);
				result = "ok";
				mc2::fmt::to_buf(b, "sun altitude={:.1f}{} isNight={:d} (DIRECTION only -- ambient/sky/fog unchanged)", static_cast<double>(p), mc2::fmt::safe(nf >= 2 ? " +yaw" : ""), mc2::fmt::as_i32(eye->getIsNight() ? 1 : 0));
				note = b;
			}
		} else {
			// Named phases map onto Camera::setDaylightFraction, whose
			// contract is sunPitch == f*360: 0 = sun on the horizon rising,
			// 0.25 = noon, 0.5 = horizon setting, 0.75 = sun at nadir.
			static const struct { const char* name; float f; } kPhases[] = {
				{ "dawn",     0.000f }, { "morning",  0.125f },
				{ "day",      0.250f }, { "noon",     0.250f },
				{ "afternoon",0.375f }, { "dusk",     0.500f },
				{ "sunset",   0.500f }, { "night",    0.750f },
				{ "midnight", 0.750f },
			};
			float f = -1.0f;
			for (auto kPhase : kPhases)
				if (arg == kPhase.name) { f = kPhase.f; break; }
			if (f < 0.0f) {
				char* end = nullptr;
				const float v = strtof(arg.c_str(), &end);
				if (!arg.empty() && end && *end == '\0' && v >= 0.0f && v <= 1.0f)
					f = v;
			}
			if (f < 0.0f) {
				note = "set_light dawn|morning|day|noon|afternoon|dusk|sunset|"
					   "night|midnight|<0..1> | alt <deg> [yaw]";
			} else {
				const float pitch = eye->setDaylightFraction(f);
				result = "ok";
				mc2::fmt::to_buf(b, "{} -> fraction={:.3f} sunPitch={:.1f} isNight={:d} nightFactor={:.2f} (full sun+ambient+sky+fog recompute)", arg.c_str(), static_cast<double>(f), static_cast<double>(pitch), mc2::fmt::as_i32(eye->getIsNight() ? 1 : 0), static_cast<double>(eye->getNightFactor()));
				note = b;
			}
		}
	}
	//---------------------------------------------------------------------
	// ANIMATE-BUILDING-1: drive ONE building onto the animated path on
	// demand. code/terrobj.cpp's own comment says the animStartInvalidated
	// counter "may be zero in smoke missions if no registered-static
	// building animates" -- i.e. passive fly-throughs never exercise this
	// path, and both the black-quad and sunk-turret reports live on it.
	//
	// Two DIFFERENT routes off the static batch, deliberately both exposed:
	//   <gestureId>  setGesture() -> explicit invalidateStaticRegistration()
	//                on the idle->animated edge (mclib/bdactor.cpp).
	//   damaged      setObjStatus() -> swaps in bldgDmgShape and does NOT
	//                invalidate; the staticReg.shape != bldgShape mismatch
	//                drops it a frame LATER from render(). That one-frame
	//                asymmetry is a black-quad candidate in its own right.
	// Test-only world mutation, so gated behind MC2_TEST_SPAWN like
	// spawn/teleport rather than adding a second gate.
	else if (line.compare(0, 8, "animate ") == 0) {
		const bool armed = mc2::gate::get<"MC2_TEST_SPAWN">();
		long pid = 0;
		char what[32] = {0};
		const int nf = sscanf(line.c_str() + 8, "%ld %31s", &pid, what);
		if (!armed) {
			result = "blocked";
			note = "needs MC2_TEST_SPAWN=1 (animate mutates world state)";
		} else if (nf < 2) {
			note = "animate <partId> <gestureId 0..9|damaged|normal>  "
				   "(ids + animGestures from `list buildings`)";
		} else if (!ObjectManager) {
			result = "blocked"; note = "no object manager";
		} else {
			GameObjectPtr obj = ObjectManager->findByPartId(pid);
			if (obj && obj->getObjectClass() == BATTLEMECH && !strcmp(what, "damaged")) {
				static_cast<Mover*>(obj)->disable(DEBUGGER_DEATH);
				result = obj->isDisabled() && !obj->isDestroyed() ? "ok" : "blocked";
				note = "salvageable disabled mech";
				return true;
			}
			BldgAppearance* ba = obj
				? dynamic_cast<BldgAppearance*>(obj->getAppearance()) : nullptr;
			const uint32_t invBefore = g_bldgAnimGate_animStartInvalidated();
			long before = -1;
			int regBefore = 0;
			if (ba) { before = ba->getCurrentGestureId();
					  regBefore = ba->isStaticRegistered() ? 1 : 0; }
			if (!obj) {
				result = "blocked";
				note = "no such partId (try `list buildings`)";
			} else if (!ba) {
				result = "blocked";
				note = "that object has no BldgAppearance (not a building/gate/turret)";
			} else if (!strcmp(what, "damaged") || !strcmp(what, "normal")) {
				ba->setObjStatus(!strcmp(what, "damaged")
					? OBJECT_STATUS_DESTROYED : OBJECT_STATUS_NORMAL);
				result = "ok";
			} else {
				char* end = nullptr;
				const long g = strtol(what, &end, 10);
				if (!end || *end != '\0' || g < 0 || g >= MAX_BD_ANIMATIONS) {
					note = "gesture must be 0..9, or damaged|normal";
				} else if (!ba->hasAnimationData(g)) {
					result = "blocked";
					note = "that gesture has NO animation data for this type: "
						   "setGesture would ClearAnimation() and the object would "
						   "stay on the STATIC path. Pick one of animGestures "
						   "from `list buildings`.";
				} else if (g == before) {
					result = "blocked";
					note = "already in that gesture (setGesture early-returns)";
				} else {
					ba->setGesture(static_cast<unsigned long>(g));
					// setGesture silently early-returns when the object is
					// DESTROYED or DISABLED (mclib/bdactor.cpp), BEFORE
					// touching bdAnimationState. Acking a bare "ok" there
					// would be the exact "the ack describes the COMMAND, not
					// the WORLD" failure this harness exists to kill: after
					// `animate <id> damaged`, a following `animate <id> 3`
					// would report ok while doing nothing at all. So verify
					// the gesture actually moved.
					if (ba->getCurrentGestureId() == g) {
						result = "ok";
					} else {
						result = "blocked";
						note = "setGesture was a no-op (object DESTROYED/DISABLED?) "
							   "-- gesture unchanged; use `animate <id> normal` first";
					}
				}
			}
			if (!strcmp(result, "ok")) {
				char b[352];
				mc2::fmt::to_buf(b, "gesture {:d}->{:d} staticReg {:d}->{:d} staticNow={:d} eligible={:d} animStartInvalidated {:d}->{:d}{}", mc2::fmt::as_i32(before), mc2::fmt::as_i32(ba->getCurrentGestureId()), mc2::fmt::as_i32(regBefore), mc2::fmt::as_i32(ba->isStaticRegistered() ? 1 : 0), mc2::fmt::as_i32(ba->IsStaticNow() ? 1 : 0), mc2::fmt::as_i32(ba->isStaticEligible() ? 1 : 0), mc2::fmt::as_u32(static_cast<unsigned long>(invBefore)), mc2::fmt::as_u32(static_cast<unsigned long>(g_bldgAnimGate_animStartInvalidated())), mc2::fmt::safe((ba->IsStaticNow() ? "  WARNING: still on the static path"
									   : "  (now on the animated path)")));
				note = b;
			}
		}
	}
	else
		return false;
	return true;
}
