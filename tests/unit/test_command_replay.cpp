#include "doctest.h"
#include "command_replay.h"
#include "appearance_rng.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include <filesystem>
#include <fstream>

// The platform RNG is not linked into the standalone tier.
std::uint32_t mc2GetSimRngState() { return 123; }

namespace {
struct ReplayEnvironment {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "mc2-unit-command-replay.txt";
    mc2::gate::ScopedOverride<"MC2_COMMAND_REPLAY_RECORD"> emptyRecord{""};
    mc2::gate::ScopedOverride<"MC2_COMMAND_REPLAY_PLAY"> emptyPlay{""};
    mc2::gate::ScopedOverride<"MC2_REPLAY_CONTENT"> content{"unit-mission-mod-fingerprint"};
    mc2::gate::ScopedOverride<"MC2_COMMAND_REPLAY_TICKS"> ticks{"2"};
    std::optional<mc2::gate::ScopedOverride<"MC2_COMMAND_REPLAY_RECORD">> recorder;
    std::optional<mc2::gate::ScopedOverride<"MC2_COMMAND_REPLAY_PLAY">> player;
    ReplayEnvironment() {
        mc2_anim_rng::reseed(123);
    }
    void recording() { recorder.emplace(path.string().c_str()); }
    void playing() { recorder.reset(); player.emplace(path.string().c_str()); }
    ~ReplayEnvironment() {
        recorder.reset(); player.reset();
        mc2::replay::begin("unit", 123);
        std::filesystem::remove(path);
    }
};
struct Permit : mc2::sim::OrderAdmissionValidator {
    bool permitted(const mc2::sim::OrderSource&, const mc2::sim::OrderEnvelope&) const override { return true; }
};
}

TEST_CASE("command replay preserves accepted ingress tick identity and detects subsystem divergence") {
    using namespace mc2::sim;
    ReplayEnvironment fixture;
    fixture.recording();
    mc2::replay::begin("unit", 123);
    TacticalOrderQueue original;
    REQUIRE(original.registerSource({7, SourceKind::UI}));
    OrderEnvelope command;
    command.sourceId = 7;
    command.issuer = CommanderId{0};
    command.recipients = {{42, 3}};
    command.payload.kind = OrderKind::Stop;
    command.payload.orderId = 1;
    Permit permit;
    REQUIRE(mc2::replay::submit(original, command, permit));
    CHECK_FALSE(mc2::replay::submit(original, command, permit)); // rejected duplicates are absent from the recording
    const std::array<std::uint64_t, 5> first{11,22,33,44,55}, second{12,23,34,45,56};
    mc2::replay::hashes(1, first);
    CHECK_FALSE(mc2::replay::complete());
    mc2::replay::hashes(2, second);
    CHECK(mc2::replay::complete());
    fixture.playing();
    mc2::replay::begin("unit", 123);
    TacticalOrderQueue replay;
    mc2::replay::pump(replay, permit);
    const auto batch = replay.beginTick(1);
    REQUIRE(batch);
    REQUIRE(batch->size() == 1);
    CHECK(batch->front().recipients == command.recipients);
    CHECK(batch->front().sourceId == 7);
    CHECK(batch->front().applyTick == 1);
    mc2::replay::hashes(1, first);
    mc2::replay::hashes(2, second);
    CHECK(mc2::replay::complete());
    CHECK_FALSE(mc2::replay::failed());
    mc2::replay::begin("unit", 123);
    mc2::replay::hashes(1, first);
    auto changed = second; changed[3] ^= 1;
    mc2::replay::hashes(2, changed);
    CHECK(mc2::replay::failed());
    CHECK_FALSE(mc2::replay::complete());
    mc2::gate::ScopedOverride<"MC2_REPLAY_CONTENT"> changedContent{"different-mod"};
    CHECK_THROWS(mc2::replay::begin("unit", 123));
}

TEST_CASE("command replay rejects truncated recordings") {
    ReplayEnvironment fixture;
    { std::ofstream file(fixture.path); file << "MC2REPLAY 1 \"unit\" 123 123 " << mc2_anim_rng::state()
        << " \"unit-mission-mod-fingerprint\"\nH 1 1 2 3 4 5\n"; }
    fixture.playing();
    CHECK_THROWS(mc2::replay::begin("unit", 123));
}
