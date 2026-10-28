#include "doctest.h"
#include "brain_execution_policy.h"
#include "save_brain_section.h"
#include "file.h"
#include "Gates/gate_cold.h"

#include <filesystem>
#include <process.h>

void getBrainFixedTickState(uint32_t&, float&, float&);

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
