#include "doctest.h"
#include "brain_execution_policy.h"
#include "save_brain_section.h"
#include "save_field_probe.h"
#include "mech_brain_runtime.h"
#include "brain_task_queue.h"
#include "file.h"
#include "Gates/gate_cold.h"

#include <filesystem>
#include <process.h>

void getBrainFixedTickState(uint32_t&, float&, float&);

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
        CHECK(save_probe::memberBytes<T>(&T::priority_tier, &T::insertion_frame_ms,
            &T::stable_seq_id, &T::warrior_id, &T::type, &T::_reserved) + 3 == sizeof(T));
        CHECK(save_probe::roundtrip(sample, mc2::save::serializeBrainTask, &T::priority_tier,
            &T::insertion_frame_ms, &T::stable_seq_id, &T::warrior_id, &T::type, &T::_reserved));
    }
    using T = MechBrainRuntime;
    auto sample = save_probe::patterned<T>(BRAIN_SCALARS);
    sample.currentState[31] = sample.prevState[31] = 0;
    sample.patrolWaypointCount = sample.patrolWaypointIndex = 2;
    sample.waitStateCount = sample.flowFiredCount = 2;
    CHECK(save_probe::roundtrip(sample, mc2::save::serializeBrainRuntime, BRAIN_SCALARS));
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
    CHECK_FALSE(save_probe::roundtrip(sample, mc2::save::serializeBrainRuntime,
        &T::playerBestDistance, &T::playerProgressTime, &T::playerFightSince, &T::playerBypassUntil,
        &T::playerRecoverUntil, &T::playerRegroupUntil, &T::playerRecoveries,
        &T::playerHealthTarget, &T::playerTargetHealth));
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
