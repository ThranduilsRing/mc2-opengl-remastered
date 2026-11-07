#include "sim_rng_state.h"
//---------------------------------------------------------------------------
// SIM-IDENTITY-HASH-1 -- engine-side collector for the world-state hash.
//
// Walks ObjectManager's live movers into mc2_sim_identity::MoverRecord and
// folds them with the pure kernel in mclib/sim_identity.h. Strictly READ-ONLY:
// getters only, every deref null-guarded, no allocation on the per-frame path.
//
// Two consumers:
//   1. mc2SimIdentityWriteJson()  -- the capture sidecar / manifest block.
//      Called once per bookmark capture from GameOS/gameos/gos_visual_capture.cpp
//      via an extern "C" shim (same pattern as mc2VisualCaptureSetPaused).
//   2. mc2SimIdentityTraceFrame() -- one line per SIM frame into the run log,
//      so two runs' logs can be diffed to find the FIRST divergent sim frame.
//
// WHY A TRACE LINE PER SIM FRAME, NOT PER MOVER
//      The pre-existing instrument (MC2_BRIDGE_MOVER_STATE, code/mission.cpp)
//      emits one line PER MOVER per period -- ~58k lines/run on mc2_10, which
//      DETERMINISM-RATCHET-2 then had to post-process with a bespoke script to
//      find one differing `target` field. This emits ONE hash line per sim
//      frame (~30/s), so `diff run_a.log run_b.log | head -1` names the first
//      divergent sim frame with no tooling at all. The per-mover detail is
//      opt-in on top (MC2_SIM_IDENTITY_TRACE_DETAIL) once you know the frame.
//
// GATE DEFAULTS -- READ THEM HERE, THIS FILE IS THE READER
//      MC2_SIM_IDENTITY           default ON   (unset => ON)
//      MC2_SIM_IDENTITY_TRACE     default OFF  (unset => OFF)
//      MC2_SIM_IDENTITY_TRACE_DETAIL default OFF
//   All three parse "0"/"false"/"off"/"no" as OFF and anything else set as ON.
//   Note the deliberate contrast with MC2_BRIDGE_MOVER_STATE two files over,
//   which is a presence row and therefore ENABLES on `=0`. That is the
//   seventeen-documented-gate-lies failure mode; the Std rows read here do not
//   reproduce it.
//
//   MC2_SIM_IDENTITY defaults ON on purpose, and the purpose is anti-wallpaper:
//   a refuse-to-grade gate whose input is default-OFF would find the block
//   missing in every pre-existing manifest and return UNGRADED for everything,
//   which is precisely the failure the brief warns about. Cost is a few
//   hundred getter calls once per capture; the per-FRAME path is opt-in.
//---------------------------------------------------------------------------

#include <algorithm>
#include "command_replay.h"
#include "save_archive.h"
#include "objmgr.h"
#include "mover.h"
#include "dcontact.h"
#include "warrior.h"
#include "bldng.h"
#include "turret.h"
#include "combat_sim_hash.h"
#include "vtol_simulation.h"
#include "weather.h"
#include "team.h"
#include "../mclib/abl.h"
#include "../mclib/mathfunc.h"
#include "../mclib/appear.h"
#include "../mclib/sim_identity.h"
#include "sim_identity.h"   // C-linkage declaration of mc2CaptureFocusMechEx (definition below is not a new extern shim)
#include "../mclib/sim_trace.h"
#include "../mclib/appearance_rng.h"
#include "Gates/gate.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "mc2fmt.h"

namespace {

using mc2_sim_identity::MoverRecord;

bool simIdentityEnabled() {
	static const bool on = true;
	return on;
}

bool simIdentityTrace() {
	static const bool on = mc2::gate::get<"MC2_SIM_IDENTITY_TRACE">();
	return on;
}

bool simIdentityTraceDetail() {
	static const bool on =
		mc2::gate::get<"MC2_SIM_IDENTITY_TRACE_DETAIL">();
	return on;
}

//
// MC2_BRIDGE_MOVER_STATE caps at 64 movers (code/mission.cpp kCap). For a
// human-read debug table a cap is a kindness; for a HASH it is a correctness
// hole -- a digest over a truncated population would agree between two arms
// while movers 65+ diverged, i.e. the instrument would lie in exactly the
// direction this whole slice exists to stop. The vector is sized once from
// getNumMovers() and reused across frames.
void collectMovers(std::vector<MoverRecord>& out) {
	out.clear();
	if (!ObjectManager)
		return;
	const long total = ObjectManager->getNumMovers();
	if (total <= 0)
		return;
	out.reserve(static_cast<size_t>(total));

	for (long i = 0; i < total; ++i) {
		MoverPtr mover = ObjectManager->getMover(i);
		if (!mover)
			continue;

		MoverRecord r;
		r.id   = static_cast<int32_t>(mover->getPartId());
		r.team = static_cast<int32_t>(mover->getTeamId());

		glm::vec3 pos = mover->getPosition();
		r.px  = pos.x;
		r.py  = pos.y;
		r.pz  = pos.z;
		r.rot = mover->getRotation();

		float dmg = mover->getDamageLevel();
		float hpFrac = 1.0f - dmg;
		hpFrac = std::clamp(hpFrac, 0.0f, 1.0f);
		r.hp = hpFrac;

		// Appearance drives what is actually DRAWN: which gesture is playing and
		// how far into it. Both are public virtuals on the Appearance base with
		// safe defaults (mclib/appear.h:339,381), so a mover whose appearance is
		// a different subclass still yields a defined value rather than a branch.
		AppearancePtr app = mover->getAppearance();
		if (app) {
			r.gesture   = static_cast<int32_t>(app->getCurrentGestureId());
			r.animFrame = static_cast<int32_t>(app->getFrameNumber());
		}

		MechWarriorPtr pilot = mover->getPilot();
		if (pilot) {
			if (simIdentityTraceDetail() && mover->getPartId() == 724) {
				auto& o = *pilot->getMoveOrders();
				mc2::fmt::print_err("[LOADFIELD] t={:.4f} time={} next={} new={} type={} goal={} global={}/{} p0={}/{}/{} p1={}/{}/{} wait={} yield={} yieldstate={} laststep={} request={} state={}/{} run={} throttle={}\n",
					scenarioTime, o.time, o.nextUpdate, o.newGoal, o.pathType, o.goalType,
					o.curGlobalStep, o.numGlobalSteps, o.path[0]->curStep, o.path[0]->numSteps, o.path[0]->numStepsWhenNotPaused,
					o.path[1]->curStep, o.path[1]->numSteps, o.path[1]->numStepsWhenNotPaused,
					o.waitForPointTime, o.yieldTime, o.yieldState, o.timeOfLastStep, pilot->getMovePathRequest() != nullptr,
					o.moveState, o.moveStateGoal, o.run, static_cast<int>(o.speedThrottle));
			}
			r.status     = static_cast<int32_t>(pilot->getStatus());
			r.brainState = static_cast<int32_t>(pilot->getBrainState());
			r.moveState  = static_cast<int32_t>(pilot->getMoveState());
			// getMovePath(long) is a PURE accessor; the zero-arg getMovePath()
			// MUTATES (swaps path slots) and must never be called from a
			// read-only observer -- calling it here would make the act of
			// hashing the world change the world.
			MovePathPtr mp = pilot->getMovePath(0);
			if (mp) {
				r.pathStep  = static_cast<int32_t>(mp->curStep);
				r.pathSteps = static_cast<int32_t>(mp->numStepsWhenNotPaused);
			}
			GameObjectPtr tgt = pilot->getCurrentTarget();
			if (tgt)
				r.target = static_cast<int32_t>(tgt->getPartId());
		}

		out.push_back(r);
	}
}

// Scratch reused across calls so the per-sim-frame trace path allocates nothing
// in steady state.
std::vector<MoverRecord>& scratch() {
	static std::vector<MoverRecord> s;
	return s;
}

using mc2_sim_identity::ObjectRecord;
using mc2_sim_identity::AblRecord;

// Every building and turret: the state the simulation steps outside the mover
// list (capture, damage, a turret's aim and target). Same discipline as the
// movers: partId keys, no pointer values, read-only accessors.
void collectObjects(std::vector<ObjectRecord>& out) {
	out.clear();
	if (!ObjectManager)
		return;
	const long nb = ObjectManager->getNumBuildings();
	const long nt = ObjectManager->getNumTurrets();
	out.reserve(static_cast<size_t>((nb > 0 ? nb : 0) + (nt > 0 ? nt : 0)));
	for (long i = 0; i < nb; ++i) {
		BuildingPtr b = ObjectManager->getBuilding(i);
		if (!b)
			continue;
		ObjectRecord r;
		r.id        = static_cast<int32_t>(b->getPartId());
		r.kind      = 1;
		r.team      = static_cast<int32_t>(b->getTeamId());
		r.commander = static_cast<int32_t>(b->getCommanderId());
		r.status    = static_cast<int32_t>(b->getStatus());
		r.damage    = b->getDamage();
		out.push_back(r);
	}
	for (long i = 0; i < nt; ++i) {
		TurretPtr t = ObjectManager->getTurret(i);
		if (!t)
			continue;
		ObjectRecord r;
		r.id     = static_cast<int32_t>(t->getPartId());
		r.kind   = 2;
		r.team   = static_cast<int32_t>(t->getTeamId());
		r.status = static_cast<int32_t>(t->getStatus());
		r.damage = t->getDamage();
		r.rot    = t->turretRotation;
		r.target = static_cast<int32_t>(t->targetWID);
		out.push_back(r);
	}
}

// Every ABL module instance in registry order: FSM state, init flag and the
// static variables a brain keeps between calls.
void collectAbl(std::vector<AblRecord>& out) {
	out.clear();
	const long n = ABLi_getNumModuleInstances();
	if (n <= 0)
		return;
	out.reserve(static_cast<size_t>(n));
	for (long i = 0; i < n; ++i) {
		ABLModulePtr m = ABLi_getModuleInstance(i);
		if (!m)
			continue;
		AblRecord r;
		r.id         = static_cast<int32_t>(m->getId());
		r.handle     = static_cast<int32_t>(m->getHandle());
		r.state      = static_cast<int32_t>(m->getStateHandle());
		r.prevState  = static_cast<int32_t>(m->getPrevStateHandle());
		r.initCalled = m->getInitCalled() ? 1 : 0;
		r.statics    = m->digestStatics(mc2_sim_identity::kFnvOffset);
		out.push_back(r);
	}
}

std::vector<ObjectRecord>& objectScratch() {
	static std::vector<ObjectRecord> s;
	return s;
}

std::vector<AblRecord>& ablScratch() {
	static std::vector<AblRecord> s;
	return s;
}

} // namespace

//---------------------------------------------------------------------------
// extern "C" shims. Declared in GameOS/gameos/gos_visual_capture.cpp -- the
// GameOS translation unit cannot include objmgr.h/mover.h (their quote-includes
// are not on its include path), the same reason mc2VisualCaptureSetPaused
// exists as a shim rather than a direct call.
//---------------------------------------------------------------------------

// Emit the "sim_identity" JSON object body (no enclosing braces, no trailing
// comma) into `buf`, at most `cap-1` chars plus NUL. Returns chars written, or
// 0 when disabled / no movers -- in which case the caller emits nothing and the
// comparator will correctly report the block ABSENT rather than empty.
extern "C" int mc2SimIdentityWriteJson(char* buf, int cap,
                                       double scenarioTimeNow, long simFrame) {
	if (!buf || cap <= 1)
		return 0;
	buf[0] = '\0';
	if (!simIdentityEnabled() || !ObjectManager)
		return 0;

	// NOTE: an EMPTY mover set still emits a full block (mover_count 0).
	// Returning nothing here would make "this mission has no movers, so there
	// is no mover-desync to worry about" indistinguishable from "nothing
	// measured this" -- and the comparator treats those two very differently
	// (the first grades, the second cannot). Absence must mean exactly one
	// thing: an exe without this feature, or an operator who turned it off.
	std::vector<MoverRecord> recs;
	collectMovers(recs);

	// digestWorld sorts in place; take all three off the SAME sorted buffer.
	const uint64_t graded = mc2_sim_identity::digestWorld(
		recs, mc2_sim_identity::DIGEST_GRADED);
	const uint64_t visual = mc2_sim_identity::digestWorld(
		recs, mc2_sim_identity::DIGEST_VISUAL);
	const uint64_t strict = mc2_sim_identity::digestWorld(
		recs, mc2_sim_identity::DIGEST_STRICT);

	char hg[17], hv[17], hs[17];
	mc2_sim_identity::toHex16(graded, hg);
	mc2_sim_identity::toHex16(visual, hv);
	mc2_sim_identity::toHex16(strict, hs);

	int n = 0;
	int w = mc2::fmt::to_buf_n(buf + n, static_cast<size_t>((cap - n)), "  \"sim_identity\": {{\n    \"schema\": \"SIM_IDENTITY_V{:d}\",\n    \"digest\": \"{}\",\n    \"digest_visual\": \"{}\",\n    \"digest_strict\": \"{}\",\n    \"mover_count\": {:d},\n    \"truncated\": false,\n    \"scenario_time\": {:.3f},\n    \"sim_frame\": {:d},\n    \"quantum\": {{ \"pos\": {:.9g}, \"ang\": {:.9g}, \"frac\": {:.9g} }},\n    \"movers\": [", mc2::fmt::as_u32(mc2_sim_identity::kSchemaVersion), mc2::fmt::safe(hg), mc2::fmt::safe(hv), mc2::fmt::safe(hs), mc2::fmt::as_i32(static_cast<int>(recs.size())), scenarioTimeNow, mc2::fmt::as_i32(simFrame), static_cast<double>(mc2_sim_identity::kPosQuantum), static_cast<double>(mc2_sim_identity::kAngQuantum), static_cast<double>(mc2_sim_identity::kFracQuantum));
	if (w < 0 || w >= cap - n) { buf[0] = '\0'; return 0; }
	n += w;

	for (size_t i = 0; i < recs.size(); ++i) {
		const MoverRecord& r = recs[i];
		char hm[17];
		mc2_sim_identity::toHex16(mc2_sim_identity::digestMoverVisual(r), hm);
		// Raw (unquantized) pos/rot/hp are emitted so the comparator can print a
		// TRUE delta beside the quantum. Without them a quantum-boundary
		// straddle and a real divergence are indistinguishable -- both would
		// arrive as nothing but "the digests differ".
		w = mc2::fmt::to_buf_n(buf + n, static_cast<size_t>((cap - n)), "{}\n      {{\"id\":{:d},\"team\":{:d},\"vd\":\"{}\",\"pos\":[{:.4f},{:.4f},{:.4f}],\"rot\":{:.4f},\"hp\":{:.5f},\"gesture\":{:d},\"anim\":{:d},\"status\":{:d},\"target\":{:d},\"brain\":{:d},\"mstate\":{:d},\"pstep\":{:d},\"psteps\":{:d}}}", mc2::fmt::safe(i ? "," : ""), mc2::fmt::as_i32(r.id), mc2::fmt::as_i32(r.team), mc2::fmt::safe(hm), r.px, r.py, r.pz, r.rot, r.hp, mc2::fmt::as_i32(r.gesture), mc2::fmt::as_i32(r.animFrame), mc2::fmt::as_i32(r.status), mc2::fmt::as_i32(r.target), mc2::fmt::as_i32(r.brainState), mc2::fmt::as_i32(r.moveState), mc2::fmt::as_i32(r.pathStep), mc2::fmt::as_i32(r.pathSteps));
		if (w < 0 || w >= cap - n) {
			// Ran out of room mid-array. Emitting a half-written array would
			// hand the comparator a truncated population that hashes fine --
			// the exact lie this slice exists to prevent. Emit nothing.
			buf[0] = '\0';
			return 0;
		}
		n += w;
	}

	w = mc2::fmt::to_buf_n(buf + n, static_cast<size_t>((cap - n)), "\n    ]\n  }}");
	if (w < 0 || w >= cap - n) { buf[0] = '\0'; return 0; }
	n += w;
	return n;
}

// One line per SIM frame into the run log. Opt-in (MC2_SIM_IDENTITY_TRACE).
// Called from the fixed-step sim advance; see code/mission.cpp.
extern "C" void mc2SimIdentityTraceFrame(double scenarioTimeNow, long simFrame) {
	if ((!simIdentityEnabled() || !simIdentityTrace()) && !mc2::replay::active()) return;
    if (!ObjectManager)
		return;

	std::vector<MoverRecord>& recs = scratch();
	collectMovers(recs);
	if (recs.empty())
		return;

	const uint64_t graded = mc2_sim_identity::digestWorld(
		recs, mc2_sim_identity::DIGEST_GRADED);
	const uint64_t visual = mc2_sim_identity::digestWorld(
		recs, mc2_sim_identity::DIGEST_VISUAL);
	const uint64_t strict = mc2_sim_identity::digestWorld(
		recs, mc2_sim_identity::DIGEST_STRICT);

	char hg[17], hv[17], hs[17];
	mc2_sim_identity::toHex16(graded, hg);
	mc2_sim_identity::toHex16(visual, hv);
	mc2_sim_identity::toHex16(strict, hs);

	// Buildings, turrets, ABL state and the per-stream draw counts ride on the
	// same line as their own fields, so the first differing line still IS the
	// first divergent sim frame and the field that moved names the population.
	std::vector<ObjectRecord>& objs = objectScratch();
	collectObjects(objs);
	std::vector<AblRecord>& abl = ablScratch();
	collectAbl(abl);
	char ho[17], ha[17];
	mc2_sim_identity::toHex16(mc2_sim_identity::digestObjects(objs), ho);
	mc2_sim_identity::toHex16(mc2_sim_identity::digestAbl(abl), ha);

    if (mc2::replay::active()) {
        mc2::save::Archive animation;
        mc2::save::Archive objects;
        auto sceneryHash = mc2_sim_identity::digestObjects(objs);
        auto combatHash = combatSimulationHash(*ObjectManager);
        auto vtolHash = vtolSimulationHash();
        objects(sceneryHash, combatHash, vtolHash);
        for (long i = 1; i <= ObjectManager->getMaxObjects(); ++i) {
            auto* mover = ObjectManager->get(i);
            if (!mover || !mover->getExists()) continue;
            if (!mover->getAppearance() || !mover->getAppearance()->simulationOwned()) continue;
            auto& a = *mover->getAppearance()->simulationAnimation();
            auto id = mover->getPartId();
            animation(i, id, a);
            auto& pose = *mover->getAppearance()->simPose;
            animation(pose.root.x, pose.root.y, pose.root.z, pose.heading);
            for (auto& node : pose.nodes) animation(node.x, node.y, node.z);
            for (auto& node : pose.shapeNodes) animation(node.x, node.y, node.z);
            animation(pose.hitLeft.x, pose.hitLeft.y, pose.hitLeft.z, pose.hitRight.x, pose.hitRight.y, pose.hitRight.z);
            animation(pose.cockpit.x, pose.cockpit.y, pose.cockpit.z);
        }
        std::uint64_t animHash = 14695981039346656037ull;
        for (auto byte : animation.bytes()) animHash = (animHash ^ byte) * 1099511628211ull;
        std::uint64_t objectHash = 14695981039346656037ull;
        for (auto byte : objects.bytes()) objectHash = (objectHash ^ byte) * 1099511628211ull;
        const std::uint64_t rngHash = (static_cast<std::uint64_t>(mc2_anim_rng::state()) << 32) ^
            mc2_anim_rng::drawCount() ^ (static_cast<std::uint64_t>(SimRandomDraws()) * 1099511628211ull) ^ mc2GetSimRngState();
        mc2::replay::hashes(static_cast<std::uint64_t>(turn), {strict,
            objectHash, mc2_sim_identity::digestAbl(abl), animHash, rngHash});
    }

	// Fixed-arity, fixed-width: two runs' logs diff cleanly and the first
	// differing line IS the first divergent sim frame.
	// The line and its newline go out in ONE fwrite (print_out); flushing first keeps it from straddling a stdio buffer boundary, where
	// another thread's stderr write could land mid-line (meta-0220 P2#7).
	fflush(stdout);
	mc2::fmt::print_out("[SIMID v1] simframe={:d} t={:.3f} n={:d} graded={} visual={} strict={} nobj={:d} objects={} nabl={:d} abl={} rng={:d}/{:d}/{:d} animrng={:d}\n",
	       mc2::fmt::as_i32(simFrame), scenarioTimeNow, mc2::fmt::as_i32(static_cast<int>(recs.size())), mc2::fmt::safe(hg), mc2::fmt::safe(hv), mc2::fmt::safe(hs),
	       mc2::fmt::as_i32(static_cast<int>(objs.size())), mc2::fmt::safe(ho), mc2::fmt::as_i32(static_cast<int>(abl.size())), mc2::fmt::safe(ha),
	       mc2::fmt::as_u32(static_cast<uint32_t>(SimRandomDraws())), mc2::fmt::as_u32(mc2_appearance_rng::drawCount()), mc2::fmt::as_u32(weather ? weather->drawCount() : 0u), mc2::fmt::as_u32(mc2_anim_rng::drawCount()));
	// Flush the whole line out at once (the buffer was empty, so this is one write): left buffered it was written later in chunks cut at
	// the stdio buffer size, where a stderr Vulkan/driver message landed inside it (W127: 'animrng=27' + ' to VK_FORMAT_R8').
	fflush(stdout);

#if MC2_SIM_TRACE
	mc2_sim_trace::setTime(scenarioTimeNow);
	MC2_SIM_TRACE_LINE("RNGF sim={:d} app={:d} wx={:d}", static_cast<uint32_t>(static_cast<uint32_t>(SimRandomDraws())), static_cast<uint32_t>(mc2_appearance_rng::drawCount()), static_cast<uint32_t>(weather ? weather->drawCount() : 0u));
#endif
	if (simIdentityTraceDetail()) {
		for (const auto & r : recs) {
			char hm[17];
			mc2_sim_identity::toHex16(mc2_sim_identity::digestMoverVisual(r), hm);
			mc2::fmt::print_out("[SIMID v1 mover] simframe={:d} id={:d} vd={} pos={:.4f},{:.4f},{:.4f} rot={:.4f} hp={:.5f} gesture={:d} anim={:d} status={:d} target={:d} brain={:d} mstate={:d} pstep={:d}/{:d}\n", mc2::fmt::as_i32(simFrame), mc2::fmt::as_i32(r.id), mc2::fmt::safe(hm), r.px, r.py, r.pz, r.rot, r.hp, mc2::fmt::as_i32(r.gesture), mc2::fmt::as_i32(r.animFrame), mc2::fmt::as_i32(r.status), mc2::fmt::as_i32(r.target), mc2::fmt::as_i32(r.brainState), mc2::fmt::as_i32(r.moveState), mc2::fmt::as_i32(r.pathStep), mc2::fmt::as_i32(r.pathSteps));
		}
	}
	fflush(stdout);
}

// MC2_CAPTURE_FOCUS: find the nth live player-team mech so a bookmark capture can
// frame it. READ-ONLY (getters only); runs once per capture pose, never in the sim.
// Returns 1 and fills xyz[3] on success, 0 when there is no such mech.
extern "C" int mc2CaptureFocusMech(int n, float* xyz)
{
	if (!ObjectManager || !xyz || n < 0 || !Team::home)
		return 0;
	const long total = ObjectManager->getNumMovers();
	int seen = 0;
	for (long i = 0; i < total; ++i) {
		MoverPtr mover = ObjectManager->getMover(i);
		if (!mover || !mover->isMech() || mover->isDisabled() || mover->isDestroyed())
			continue;
		if (mover->getTeamId() != Team::home->getId())
			continue;
		if (seen++ != n)
			continue;
		const glm::vec3 pos = mover->getPosition();
		xyz[0] = pos.x; xyz[1] = pos.y; xyz[2] = pos.z;
		return 1;
	}
	return 0;
}

extern bool ShowMovers;
void mc2CaptureFocusSetShowMovers(int on) { ShowMovers = on != 0; }

// Walk variant for the all-mech campaign capture (MC2_CAPTURE_FOCUS=mech:all|enemy|home).
// Same read-only rules as above; counts every live mech matching the team selector.
int mc2CaptureFocusMechEx(int n, int sel, float* xyz, int* id, int* team, char* name, int* total, int* contact)
{
	if (total) *total = 0;
	if (!ObjectManager || !xyz || n < 0 || !Team::home)
		return 0;
	const long count = ObjectManager->getNumMovers();
	int seen = 0, hit = 0;
	for (long i = 0; i < count; ++i) {
		MoverPtr mover = ObjectManager->getMover(i);
		if (!mover || !mover->isMech() || mover->isDisabled() || mover->isDestroyed())
			continue;
		const bool home = mover->getTeamId() == Team::home->getId();
		if ((sel == 0 && !home) || (sel == 1 && home))
			continue;
		++seen;
		if (seen - 1 != n)
			continue;
		const glm::vec3 pos = mover->getPosition();
		xyz[0] = pos.x; xyz[1] = pos.y; xyz[2] = pos.z;
		if (id) *id = static_cast<int>(mover->getPartId());
		if (team) *team = static_cast<int>(mover->getTeamId());
			if (contact) *contact = home ? 0 : (mover->conStat == CONTACT_VISUAL ? 1 : (ShowMovers ? 3 : 2));
		if (name) {
			const char* nm = mover->getName();
			size_t j = 0;
			for (; nm && nm[j] && j < 63; ++j)
				name[j] = (nm[j] == ' ' || nm[j] == '"' || nm[j] == 92) ? '_' : nm[j];
			name[j] = 0;
		}
		hit = 1;
	}
	if (total) *total = seen;
	return hit;
}
