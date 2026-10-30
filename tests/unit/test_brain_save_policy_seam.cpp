#include "doctest.h"
#include "brain_execution_policy.h"
#include "save_brain_section.h"
#include "save_field_probe.h"
#include "mech_brain_runtime.h"
#include "brain_task_queue.h"
#include "brain_mission_var_store.h"
#include "warrior.h"
#include "file.h"
#include "Gates/gate_cold.h"

#include <filesystem>
#include <process.h>

void getBrainFixedTickState(uint32_t&, float&, float&);
void setBrainFixedTickState(uint32_t, float, float);

bool operator==(const BrainTaskEntry& a, const BrainTaskEntry& b) {
    return std::tie(a.priority_tier, a.insertion_frame_ms, a.stable_seq_id, a.warrior_id, a.type) ==
        std::tie(b.priority_tier, b.insertion_frame_ms, b.stable_seq_id, b.warrior_id, b.type) &&
        std::memcmp(a._reserved, b._reserved, sizeof(a._reserved)) == 0;
}

TEST_CASE("save fields: BRN1 queue image retains sequence and every task byte") {
    using T = mc2::save::BrainQueueValue;
    using E = BrainTaskEntry;
    T sample;
    sample.seq = 0x01010101;
    sample.entries = {save_probe::patterned<E>(&E::priority_tier, &E::insertion_frame_ms,
        &E::stable_seq_id, &E::warrior_id, &E::type, &E::_reserved)};
    SAVE_FIELD_SHAPE(sample, entries, seq);
    CHECK(save_probe::memberBytes<T>(&T::entries, &T::seq) + 4 == sizeof(T));
    CHECK(save_probe::roundtrip(sample, mc2::save::serializeBrainQueue, &T::entries, &T::seq));
    mc2::save::Archive writer;
    mc2::save::serializeBrainQueue(writer, sample);
    REQUIRE(writer.good());
    CHECK(writer.bytes().size() == 8 + sizeof(E) - 3);
}

TEST_CASE("save fields: BRN1 section fixed tick and mission variables") {
    struct Restore {
        uint32_t tick;
        float accum, last;
        MissionVarStore vars = g_missionVarStore;
        mc2::brainpolicy::Policy policy = mc2::brainpolicy::current();
        std::array<MechWarrior*, MAX_WARRIORS> warriors;
        Restore() {
            getBrainFixedTickState(tick, accum, last);
            std::copy(std::begin(MechWarrior::warriorList), std::end(MechWarrior::warriorList), warriors.begin());
            std::fill(std::begin(MechWarrior::warriorList), std::end(MechWarrior::warriorList), nullptr);
        }
        ~Restore() {
            setBrainFixedTickState(tick, accum, last);
            g_missionVarStore = vars;
            mc2::brainpolicy::g_policy = policy;
            std::copy(warriors.begin(), warriors.end(), std::begin(MechWarrior::warriorList));
        }
    } restore;
    using E = MissionVarEntry;
    SAVE_FIELD_SHAPE(g_missionVarStore, entries, count);
    SAVE_FIELD_SHAPE(g_missionVarStore.entries[0], key, value, lastWriterWID, lastWriteTick);
    CHECK(save_probe::memberBytes<E>(&E::key, &E::value, &E::lastWriterWID, &E::lastWriteTick) == sizeof(E));
    using S = MissionVarStore;
    CHECK(save_probe::memberBytes<S>(&S::entries, &S::count) == sizeof(S));
    mc2::brainpolicy::g_policy = mc2::brainpolicy::Policy::TechBrain;
    setBrainFixedTickState(0x01010101, 0.125f, 0.25f);
    g_missionVarStore = {};
    g_missionVarStore.count = 2;
    for (int i = 0; i < 2; ++i) {
        auto& entry = g_missionVarStore.entries[i];
        std::memset(entry.key, 'A' + i, 31);
        std::memset(entry.value, 'a' + i, 31);
        entry.lastWriterWID = (i + 1) * 0x02020202;
        entry.lastWriteTick = (i + 1) * 0x03030303;
    }
    const auto expected = g_missionVarStore;
    const auto bytes = mc2::save::saveBrainSection();
    REQUIRE_FALSE(bytes.empty());
    setBrainFixedTickState(0, 0, 0);
    g_missionVarStore = {};
    REQUIRE(mc2::save::restoreBrainSection(bytes));
    uint32_t tick = 0;
    float accum = 0, last = 0;
    getBrainFixedTickState(tick, accum, last);
    CHECK(tick == 0x01010101);
    CHECK(accum == 0.125f);
    CHECK(last == 0.25f);
    REQUIRE(g_missionVarStore.count == 2);
    for (int i = 0; i < 2; ++i) {
        const auto& entry = g_missionVarStore.entries[i];
        CHECK(std::strcmp(entry.key, expected.entries[i].key) == 0);
        CHECK(std::strcmp(entry.value, expected.entries[i].value) == 0);
        CHECK(entry.lastWriterWID == expected.entries[i].lastWriterWID);
        CHECK(entry.lastWriteTick == expected.entries[i].lastWriteTick);
    }
}

#define BRAIN_SCALARS &T::mode, &T::activeOpordSlot, &T::opordFallbackPolicy, &T::completionFlags, \
    &T::tacticWeights, &T::weightsNormalized, &T::selectedTactic, &T::pendingEventMask, \
    &T::lastRequestOrdersFrame, &T::initialHoldPushed, &T::dispatchEffectApplied, &T::ejectEffectApplied, \
    &T::guardEffectApplied, &T::moveToEffectApplied, &T::attackEffectApplied, &T::retreatEffectApplied, \
    &T::currentState, &T::prevState, &T::patrolWaypointCount, &T::patrolWaypointIndex, \
    &T::patrolWaypoints, &T::patrolLoop, &T::patrolActive, &T::patrolStarted, &T::engageRadius, \
    &T::engageTargetWID, &T::guardHold, &T::opordType, &T::opordCursor, &T::swAttackerHelpRadius, \
    &T::swDefenderHelpRadius, &T::swRequestHelp, &T::swReturnToPost, &T::swWakeOnAttack, \
    &T::swPoweredDown, &T::postPos, &T::postSet, &T::sentryAsleep, &T::sentryWoken, \
    &T::scoutObserveOnly, &T::escortTargetWID, &T::escortMoving, &T::helpTargetWID, \
    &T::waitStateCount, &T::flowFiredIdx, &T::flowFiredCount

TEST_CASE("save fields: BRN1 task and runtime scalar census") {
    {
        using T = BrainTaskEntry;
        auto sample = save_probe::patterned<T>(&T::priority_tier, &T::insertion_frame_ms,
            &T::stable_seq_id, &T::warrior_id, &T::type, &T::_reserved);
        SAVE_FIELD_SHAPE(sample, priority_tier, insertion_frame_ms, stable_seq_id, warrior_id, type, reserved);
        CHECK(save_probe::memberBytes<T>(&T::priority_tier, &T::insertion_frame_ms,
            &T::stable_seq_id, &T::warrior_id, &T::type, &T::_reserved) + 3 == sizeof(T));
        CHECK(save_probe::roundtrip(sample, mc2::save::serializeBrainTask, &T::priority_tier,
            &T::insertion_frame_ms, &T::stable_seq_id, &T::warrior_id, &T::type, &T::_reserved));
    }
    using T = MechBrainRuntime;
    CHECK(save_probe::memberBytes<T>(BRAIN_SCALARS, &T::_pad, &T::specialBody,
        &T::varStore, &T::specialIndex, &T::playerObjective, &T::playerLastContact,
        &T::playerBestDistance, &T::playerProgressTime, &T::playerFightSince,
        &T::playerBypassUntil, &T::playerRecoverUntil, &T::playerRegroupUntil,
        &T::playerRecoveries, &T::playerHealthTarget, &T::playerTargetHealth,
        &T::waitStates, &T::pendingIntents, &T::pendingIntentCount) + 29 == sizeof(T));
    auto sample = save_probe::patterned<T>(BRAIN_SCALARS);
    SAVE_FIELD_SHAPE(sample, mode, activeOpordSlot, opordFallbackPolicy, completionFlags,
        tacticWeights, weightsNormalized, selectedTactic, pendingEventMask, pad,
        lastRequestOrdersFrame, initialHoldPushed, dispatchEffectApplied, ejectEffectApplied,
        guardEffectApplied, moveToEffectApplied, attackEffectApplied, retreatEffectApplied,
        currentState, prevState, specialBody, varStore, specialIndex, patrolWaypointCount,
        patrolWaypointIndex, patrolWaypoints, patrolLoop, patrolActive, patrolStarted,
        engageRadius, engageTargetWID, guardHold, playerObjective, playerLastContact,
        playerBestDistance, playerProgressTime, playerFightSince, playerBypassUntil,
        playerRecoverUntil, playerRegroupUntil, playerRecoveries, playerHealthTarget,
        playerTargetHealth, opordType, opordCursor, swAttackerHelpRadius, swDefenderHelpRadius,
        swRequestHelp, swReturnToPost, swWakeOnAttack, swPoweredDown, postPos, postSet,
        sentryAsleep, sentryWoken, scoutObserveOnly, escortTargetWID, escortMoving,
        helpTargetWID, waitStates, waitStateCount, flowFiredIdx, flowFiredCount,
        pendingIntents, pendingIntentCount);
    SAVE_FIELD_SHAPE(sample.varStore, entries, count);
    SAVE_FIELD_SHAPE(sample.varStore.entries[0], key, value, scope);
    SAVE_FIELD_SHAPE(sample.waitStates[0], verbIndex, armed, satisfied, deadlineMs);
    sample.currentState[31] = sample.prevState[31] = 0;
    sample.patrolWaypointCount = sample.patrolWaypointIndex = 2;
    sample.waitStateCount = sample.flowFiredCount = 2;
    sample.varStore.count = 2;
    for (int i = 0; i < sample.varStore.count; ++i) {
        auto& entry = sample.varStore.entries[i];
        std::memset(entry.key, 'A' + i, sizeof(entry.key) - 1);
        std::memset(entry.value, 'a' + i, sizeof(entry.value) - 1);
        entry.key[31] = entry.value[31] = 0;
        entry.scope = static_cast<VarScope>(i);
    }
    for (size_t i = 0; i < std::size(sample.waitStates); ++i) {
        auto& state = sample.waitStates[i];
        state.verbIndex = static_cast<decltype(state.verbIndex)>(i + 1);
        state.armed = state.satisfied = true;
        state.deadlineMs = uint32_t(i + 1) * 0x01010101;
    }
    CHECK(save_probe::roundtrip(sample, mc2::save::serializeBrainRuntime, BRAIN_SCALARS));
    mc2::save::Archive writer;
    mc2::save::serializeBrainRuntime(writer, sample);
    REQUIRE(writer.good());
    T decoded{};
    mc2::save::Archive reader(writer.bytes());
    mc2::save::serializeBrainRuntime(reader, decoded);
    REQUIRE(reader.finish());
    REQUIRE(decoded.varStore.count == sample.varStore.count);
    for (int i = 0; i < sample.varStore.count; ++i) {
        CHECK(std::strcmp(decoded.varStore.entries[i].key, sample.varStore.entries[i].key) == 0);
        CHECK(std::strcmp(decoded.varStore.entries[i].value, sample.varStore.entries[i].value) == 0);
        CHECK(decoded.varStore.entries[i].scope == sample.varStore.entries[i].scope);
    }
    for (size_t i = 0; i < std::size(sample.waitStates); ++i) {
        CHECK(decoded.waitStates[i].verbIndex == sample.waitStates[i].verbIndex);
        CHECK(decoded.waitStates[i].armed == sample.waitStates[i].armed);
        CHECK(decoded.waitStates[i].satisfied == sample.waitStates[i].satisfied);
        CHECK(decoded.waitStates[i].deadlineMs == sample.waitStates[i].deadlineMs);
    }
    // Explicit debt: these live player-command fields are absent from BRN1.
    // A loss is detected, never mistaken for successful whole-runtime coverage.
    sample.playerBestDistance = 1.25f;
    sample.playerProgressTime = 2.25f;
    sample.playerFightSince = 3.25f;
    sample.playerBypassUntil = 4.25f;
    sample.playerRecoverUntil = 5.25f;
    sample.playerRegroupUntil = 6.25f;
    sample.playerRecoveries = 0x07070707;
    sample.playerHealthTarget = 0x08080808;
    sample.playerTargetHealth = 9.25f;
    mc2::save::Archive lossWriter;
    mc2::save::serializeBrainRuntime(lossWriter, sample);
    REQUIRE(lossWriter.good());
    // Valid, identical bytes prove the nine changes were ignored, not rejected.
    CHECK(lossWriter.bytes() == writer.bytes());
    const auto knownLoss = [&](auto member, const char* field) {
        CAPTURE(field);
        CHECK_FALSE(save_probe::roundtrip(sample, mc2::save::serializeBrainRuntime, member));
    };
    knownLoss(&T::playerBestDistance, "playerBestDistance");
    knownLoss(&T::playerProgressTime, "playerProgressTime");
    knownLoss(&T::playerFightSince, "playerFightSince");
    knownLoss(&T::playerBypassUntil, "playerBypassUntil");
    knownLoss(&T::playerRecoverUntil, "playerRecoverUntil");
    knownLoss(&T::playerRegroupUntil, "playerRegroupUntil");
    knownLoss(&T::playerRecoveries, "playerRecoveries");
    knownLoss(&T::playerHealthTarget, "playerHealthTarget");
    knownLoss(&T::playerTargetHealth, "playerTargetHealth");
}

TEST_CASE("BrainSavePolicy/sidecars select TechBrain only from a temporary mod overlay") {
    namespace fs = std::filesystem;
    using namespace mc2::brainpolicy;
    mc2::gate::ScopedOverride<"MC2_ACTIVE_MOD"> stock("");
    mc2::gate::ScopedOverride<"MC2_MOD_DEPS"> noDeps("");
    mc2::gate::ScopedOverride<"MC2_BRAIN_POLICY"> automatic("auto");
    mc2::gate::ScopedOverride<"MC2_BRAIN_SPECIAL_FIT"> noExplicitFit("");
    struct Sandbox {
        fs::path repo = fs::current_path();
        fs::path dir = fs::temp_directory_path() / ("mc2-brain-policy-" + std::to_string(_getpid()));
        Policy previous = current();
        Sandbox() {
            fs::create_directories(dir / "data/missions");
            fs::create_directories(dir / "mods/w81/data/missions");
            for (const auto name : {"mc2_01_specials.fit", "mc2_01_ai.fit"})
                fs::copy_file(repo / "tests/fixtures/brain_runtime" / name, dir / "data/missions" / name,
                              fs::copy_options::overwrite_existing);
            fs::current_path(dir);
        }
        ~Sandbox() {
            fs::current_path(repo);
            g_policy = previous;
            InitModSearchPaths("mods/");
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    } sandbox;
    InitModSearchPaths("mods/");
    decide("data/missions/missing.fit", "data/missions/", "mc2_01");
    CHECK(current() == Policy::Legacy);
    SUBCASE("overlay specials") {
        fs::copy_file("data/missions/mc2_01_specials.fit", "mods/w81/data/missions/mc2_01_specials.fit");
        mc2::gate::ScopedOverride<"MC2_ACTIVE_MOD"> active("w81");
        InitModSearchPaths("mods/");
        decide("data/missions/missing.fit", "data/missions/", "mc2_01");
        CHECK(techBrain());
    }
    SUBCASE("overlay ai") {
        fs::copy_file("data/missions/mc2_01_ai.fit", "mods/w81/data/missions/mc2_01_ai.fit");
        mc2::gate::ScopedOverride<"MC2_ACTIVE_MOD"> active("w81");
        InitModSearchPaths("mods/");
        decide("data/missions/missing.fit", "data/missions/", "mc2_01");
        CHECK(techBrain());
    }
    SUBCASE("explicit special fit") {
        mc2::gate::ScopedOverride<"MC2_BRAIN_SPECIAL_FIT"> explicitFit("mc2_01_specials.fit");
        decide("data/missions/missing.fit", "data/missions/", "mc2_01");
        CHECK(techBrain());
    }
    SUBCASE("explicit policy") {
        mc2::gate::ScopedOverride<"MC2_BRAIN_POLICY"> forced("techbrain");
        decide("data/missions/missing.fit", "data/missions/", "mc2_01");
        CHECK(techBrain());
    }
}

TEST_CASE("BrainSavePolicy/Legacy drops BRN1 without parsing or changing the fixed tick") {
    using namespace mc2::brainpolicy;
    struct RestorePolicy {
        Policy previous = current();
        ~RestorePolicy() { g_policy = previous; }
    } restore;
    uint32_t before = 0, after = 0;
    float accumBefore = 0, lastBefore = 0, accumAfter = 0, lastAfter = 0;
    getBrainFixedTickState(before, accumBefore, lastBefore);
    g_policy = Policy::Legacy;
    CHECK(mc2::save::restoreBrainSection({})); // Even malformed optional data is ignored under Legacy.
    getBrainFixedTickState(after, accumAfter, lastAfter);
    CHECK(after == before);
    CHECK(accumAfter == accumBefore);
    CHECK(lastAfter == lastBefore);
    g_policy = Policy::TechBrain;
    CHECK_FALSE(mc2::save::restoreBrainSection({})); // The active reader still validates its payload.
}
