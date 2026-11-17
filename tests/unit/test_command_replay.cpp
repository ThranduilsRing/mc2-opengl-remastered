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

TEST_CASE("command replay blocks fixture mutations only during recording") {
    for (const auto command : {"teleport 512 -2450 -5500", "spawn clone 0 0", "freeze_movers on",
                               "freeze_movers", "force_state 512 sensor 4", "time_scale 2", "end_mission win",
                               "equipment_probe 0 0 0 1 0", "bench_freeze 1", "animate 512 damaged",
                               "airstrike 0 0", "support repair", "tree_fall 0 0", "bench_attack",
                               "toggle_pause", "press dialog.YES", "future_fixture 1"}) {
        CAPTURE(command);
        CHECK(mc2::replay::blocksFixtureMutation(command, true));
        CHECK_FALSE(mc2::replay::blocksFixtureMutation(command, false));
    }
    for (const auto command : {"select all", "order move 0 0", "order attack 513", "order jump 0 0",
                               "order hold_fire", "order powerup", "order powerdown", "order withdraw", "status",
                               "list", "unit 512", "mission_status", "light_state", "camera 0 0",
                               "camera_follow 512", "debugview off", "screenshot proof"}) {
        CAPTURE(command);
        CHECK_FALSE(mc2::replay::blocksFixtureMutation(command, true));
    }
}

TEST_CASE("command replay fixture refusal permanently fails recording without a success footer") {
    ReplayEnvironment fixture;
    fixture.recording();
    mc2::replay::begin("unit", 123);
    mc2::replay::hashes(1, {1,2,3,4,5});
    mc2::replay::failRecording("equipment_probe");
    mc2::replay::hashes(2, {1,2,3,4,5});
    CHECK(mc2::replay::failed());
    CHECK_FALSE(mc2::replay::complete());
    CHECK(mc2::replay::active()); // Subsequent fixtures remain guarded until shutdown.
    std::ifstream file(fixture.path);
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    CHECK(text.find("\nE ") == std::string::npos);
    CHECK(text.find("\nH 2 ") == std::string::npos);
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
    { std::ofstream file(fixture.path); file << "MC2REPLAY 2 \"unit\" 123 123 " << mc2_anim_rng::state()
        << " \"unit-mission-mod-fingerprint\"\nH 1 1 2 3 4 5\n"; }
    fixture.playing();
    CHECK_THROWS(mc2::replay::begin("unit", 123));
}

TEST_CASE("command replay rejects previous and unknown versions with a specific diagnostic") {
    ReplayEnvironment fixture;
    fixture.playing();
    for (unsigned version : {1u, 3u}) {
        { std::ofstream file(fixture.path); file << "MC2REPLAY " << version << " \"unit\" 123 123 "
            << mc2_anim_rng::state() << " \"unit-mission-mod-fingerprint\"\n"; }
        CHECK_THROWS_WITH(mc2::replay::begin("unit", 123),
            ("unsupported replay format version " + std::to_string(version) + "; expected 2").c_str());
    }
}
