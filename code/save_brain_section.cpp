#include "save_brain_section.h"

#include "brain_mission_var_store.h"
#include "brain_special_dispatch.h"
#include "mech_brain_runtime.h"
#include "brain_task_queue.h"
#include "warrior.h"

#include <cstring>
#include <utility>

void getBrainFixedTickState(uint32_t& tickIndex, float& accum, float& simLast);
void setBrainFixedTickState(uint32_t tickIndex, float accum, float simLast);

namespace mc2::save {
namespace {

// Fixed-width text field: bytes up to the first NUL, zero padded, so equal state gives equal bytes.
template<size_t N> void text(Archive& a, char (&field)[N]) {
    std::array<std::uint8_t, N> buf{};
    if (!a.reading()) std::memcpy(buf.data(), field, strnlen(field, N - 1));
    a(buf);
    if (a.reading() && a.good()) {
        buf[N - 1] = 0;
        std::memcpy(field, buf.data(), N);
    }
}

template<class T> void u8(Archive& a, T& v) {
    auto raw = static_cast<std::uint8_t>(v);
    a(raw);
    if (a.reading() && a.good()) v = static_cast<T>(raw);
}

void wid(Archive& a, unsigned long& v) {
    auto raw = static_cast<std::uint32_t>(v);
    a(raw);
    if (a.reading() && a.good()) v = raw;
}

void count(Archive& a, int& n, int cap) {
    auto raw = static_cast<std::uint32_t>(n < 0 ? 0 : n);
    a(raw);
    if (!a.good()) return;
    if (raw > static_cast<std::uint32_t>(cap)) { a.reject("brain_count"); return; }
    n = static_cast<int>(raw);
}

void runtime(Archive& a, MechBrainRuntime& r) {
    u8(a, r.mode);
    if (a.good() && std::to_underlying(r.mode) > 2) { a.reject("brain_mode"); return; }
    a(r.activeOpordSlot, r.opordFallbackPolicy, r.completionFlags, r.weightsNormalized, r.pendingEventMask,
      r.initialHoldPushed, r.dispatchEffectApplied, r.ejectEffectApplied, r.guardEffectApplied, r.moveToEffectApplied,
      r.attackEffectApplied, r.retreatEffectApplied, r.lastRequestOrdersFrame);
    for (float& w : r.tacticWeights) a(w);
    auto tactic = static_cast<std::int32_t>(r.selectedTactic);
    a(tactic);
    if (a.reading() && a.good()) r.selectedTactic = tactic;
    text(a, r.currentState);
    text(a, r.prevState);
    int n = r.varStore.count;
    count(a, n, VarStore::kVarStoreCap);
    for (int i = 0; a.good() && i < n; ++i) {
        text(a, r.varStore.entries[i].key);
        text(a, r.varStore.entries[i].value);
        u8(a, r.varStore.entries[i].scope);
    }
    if (a.reading() && a.good()) r.varStore.count = n;
    a(r.patrolWaypointCount, r.patrolWaypointIndex);
    if (a.good() && r.patrolWaypointCount > 8) { a.reject("brain_patrol_count"); return; }
    for (auto& wp : r.patrolWaypoints) for (float& c : wp) a(c);
    a(r.patrolLoop, r.patrolActive, r.patrolStarted, r.engageRadius);
    wid(a, r.engageTargetWID);
    a(r.guardHold, r.opordCursor, r.swAttackerHelpRadius, r.swDefenderHelpRadius, r.swRequestHelp, r.swReturnToPost,
      r.swWakeOnAttack, r.swPoweredDown);
    for (auto& t : r.opordType) a(t);
    for (float& c : r.postPos) a(c);
    a(r.postSet, r.sentryAsleep, r.sentryWoken, r.scoutObserveOnly, r.escortMoving);
    wid(a, r.escortTargetWID);
    wid(a, r.helpTargetWID);
    a(r.waitStateCount, r.flowFiredCount);
    if (a.good() && (r.waitStateCount > MechBrainRuntime::kBrainWaitCap || r.flowFiredCount > MechBrainRuntime::kBrainFlowFiredCap)) {
        a.reject("brain_wait_count"); return;
    }
    for (auto& w : r.waitStates) a(w.verbIndex, w.armed, w.satisfied, w.deadlineMs);
    for (auto& f : r.flowFiredIdx) a(f);
}

struct QueueImage {
    std::vector<BrainTaskEntry> entries;
    std::uint32_t seq = 0;
};

void queue(Archive& a, QueueImage& q) {
    a(q.seq);
    int n = static_cast<int>(q.entries.size());
    count(a, n, BrainTaskQueue::MAX_BRAIN_TASKS);
    if (a.reading() && a.good()) q.entries.assign(static_cast<size_t>(n), BrainTaskEntry{});
    for (auto& e : q.entries) {
        a(e.priority_tier, e.insertion_frame_ms, e.stable_seq_id, e.warrior_id);
        u8(a, e.type);
        a.raw(e._reserved);
    }
}

void missionVars(Archive& a, MissionVarStore& s) {
    int n = s.count;
    count(a, n, MissionVarStore::kMissionVarStoreCap);
    for (int i = 0; a.good() && i < n; ++i) {
        auto& e = s.entries[i];
        text(a, e.key);
        text(a, e.value);
        a(e.lastWriterWID, e.lastWriteTick);
    }
    if (a.reading() && a.good()) s.count = n;
}

} // namespace

Bytes saveBrainSection() {
    std::uint32_t tickIndex = 0;
    float accum = 0.0f, simLast = -1.0f;
    getBrainFixedTickState(tickIndex, accum, simLast);
    std::vector<std::uint32_t> ids;
    for (std::uint32_t i = 0; i < MAX_WARRIORS; ++i) {
        MechWarrior* w = MechWarrior::warriorList[i];
        if (w && w->isUsed() && (w->getBrainRuntime() || w->getBrainTaskQueue())) ids.push_back(i);
    }
    if (ids.empty() && !g_missionVarStore.count && !tickIndex) return {};
    Archive a;
    a(tickIndex, accum, simLast);
    missionVars(a, g_missionVarStore);
    auto n = static_cast<std::uint32_t>(ids.size());
    a(n);
    for (std::uint32_t id : ids) {
        MechWarrior* w = MechWarrior::warriorList[id];
        bool hasRuntime = w->getBrainRuntime() != nullptr, hasQueue = w->getBrainTaskQueue() != nullptr;
        a(id, hasRuntime, hasQueue);
        if (hasRuntime) runtime(a, *w->getBrainRuntime());
        if (hasQueue) {
            const BrainTaskQueue& live = *w->getBrainTaskQueue();
            QueueImage q;
            q.seq = live.seq();
            q.entries.assign(live.data(), live.data() + live.numPending());
            queue(a, q);
        }
    }
    return a.good() ? a.bytes() : Bytes{};
}

bool restoreBrainSection(std::span<const std::uint8_t> payload) {
    Archive a(payload);
    std::uint32_t tickIndex = 0, n = 0;
    float accum = 0.0f, simLast = -1.0f;
    MissionVarStore vars;
    vars.reset();
    a(tickIndex, accum, simLast);
    missionVars(a, vars);
    a(n);
    if (a.good() && n > MAX_WARRIORS) a.reject("brain_warrior_count");
    struct Staged { std::uint32_t id = 0; bool hasRuntime = false, hasQueue = false; MechBrainRuntime rt{}; QueueImage q; };
    std::vector<Staged> staged;
    for (std::uint32_t i = 0; a.good() && i < n; ++i) {
        Staged s;
        a(s.id, s.hasRuntime, s.hasQueue);
        if (!a.good()) break;
        if (s.id >= MAX_WARRIORS || !MechWarrior::warriorList[s.id] || !MechWarrior::warriorList[s.id]->isUsed()) { a.reject("brain_warrior"); break; }
        if (s.hasRuntime) runtime(a, s.rt);
        if (s.hasQueue) queue(a, s.q);
        staged.push_back(std::move(s));
    }
    if (!a.finish()) return false;
    setBrainFixedTickState(tickIndex, accum, simLast);
    g_missionVarStore = vars;
    for (auto& s : staged) {
        MechWarrior* w = MechWarrior::warriorList[s.id];
        if (s.hasRuntime) {
            w->setBrainRuntimeMode(s.rt.mode);
            MechBrainRuntime* live = w->getBrainRuntime();
            // specialBody / specialIndex come from the mission's _specials.fit, not the save; the live pair stays.
            s.rt.specialBody = std::move(live->specialBody);
            s.rt.specialIndex = std::move(live->specialIndex);
            *live = std::move(s.rt);
            applyCachedSpecialBodyToRuntime(live);
        }
        if (s.hasQueue) w->ensureBrainTaskQueue()->restore(s.q.entries.data(), static_cast<int>(s.q.entries.size()), s.q.seq);
    }
    return true;
}

} // namespace mc2::save
