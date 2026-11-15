// The appearance-side random stream: replayable from its seed, in range, and separate from the simulation's sequence.
#include "doctest.h"
#include "mclib/appearance_rng.h"
#include "mclib/det_rng.h"

#include <vector>
#include "unit_anim_state.h"
#include "save_archive.h"
#include "simulation_save_section.h"
#include "unit_animation_tick.h"

namespace ar = mc2_appearance_rng;

// Trigger paths: code/mission_update.cpp, mclib/unit_animation_tick.h.
TEST_CASE("presentation animation pause holds phase and resumes without catchup") {
    mc2::sim::UnitAnimState state;
    state.currentFrame = 3.25f;
    for (int frame = 0; frame < 120; ++frame) {
        mc2::sim::advanceLinearAnimation(state, 30, 60,
            mc2::sim::presentationAnimationSeconds(1.0f / 60.0f, true));
    }
    CHECK(state.currentFrame == 3.25f);
    mc2::sim::advanceLinearAnimation(state, 30, 60,
        mc2::sim::presentationAnimationSeconds(1.0f / 60.0f, false));
    CHECK(state.currentFrame == 3.75f);
}
TEST_CASE("gameplay linear clips transition on fixed ticks in either direction") {
    mc2::sim::UnitAnimState state;
    state.setFirstFrame = true;
    mc2::sim::advanceLinearAnimation(state, 30, 3, 1.0f / 30.0f);
    CHECK(state.currentFrame == 0);
    for (int i = 0; i < 3; ++i) mc2::sim::advanceLinearAnimation(state, 30, 3, 1.0f / 30.0f);
    CHECK(state.currentFrame == 2);
    CHECK(state.canTransition);
    state.isReversed = state.isLooping = true;
    state.currentFrame = 0;
    mc2::sim::advanceLinearAnimation(state, 30, 3, 1.0f / 30.0f);
    CHECK(state.currentFrame == 2);
}

TEST_CASE("mission animation payload exceeds 64K and rejects truncation") {
    mc2::save::Bytes original(100000, 0x5a), decoded;
    mc2::save::Archive write;
    mc2::sim::serializeAnimationBlob(write, original);
    REQUIRE(write.good());
    mc2::save::Archive read(write.bytes());
    mc2::sim::serializeAnimationBlob(read, decoded);
    REQUIRE(read.finish());
    CHECK(original == decoded);
    auto truncated = write.bytes(); truncated.pop_back();
    mc2::save::Archive invalid(truncated);
    decoded.clear();
    mc2::sim::serializeAnimationBlob(invalid, decoded);
    CHECK_FALSE(invalid.good());
    CHECK(decoded.empty());
}

TEST_CASE("unit animation save preserves gameplay clocks and imported clip transitions") {
    mc2::sim::UnitAnimState before;
    before.currentGestureId = 4;
    before.currentFrame = 3.25f;
    before.inJump = true;
    before.jumpVelocity = {1, 2, 3};
    before.nodeUsed = {1, 2};
    before.nodeRecycle = {0.25f, 0.5f};
    before.activityEffect = 42;
    before.ejectionSerial = 3;
    before.imported.activeClip = "walk";
    before.imported.pendingClip = "hit_front";
    before.imported.pendingStreak = 2;
    before.imported.clipTimeSec = 0.375f;
    before.imported.lastFrame = 19;
    mc2::save::Archive write;
    write(before);
    REQUIRE(write.good());
    mc2::sim::UnitAnimState after;
    mc2::save::Archive read(write.bytes());
    read(after);
    REQUIRE(read.finish());
    CHECK(after.imported.pendingClip == "hit_front");
    CHECK(after.imported.activeClip == "walk");
    CHECK(after.currentFrame == 3.25f);
    CHECK(after.nodeRecycle == before.nodeRecycle);
    mc2::save::Archive canonical;
    canonical(after);
    CHECK(canonical.bytes() == write.bytes());
    before.imported.activeClip.assign(257, 'x');
    mc2::save::Archive invalid;
    invalid(before);
    CHECK_FALSE(invalid.good());
}

TEST_CASE("animation_rng: cosmetic draws cannot alter gameplay animation and saved state resumes") {
    mc2_anim_rng::reseed(17);
    const auto first = mc2_anim_rng::randomNumber(1000);
    const auto saved = mc2_anim_rng::state(), draws = mc2_anim_rng::drawCount();
    const auto next = mc2_anim_rng::randomNumber(1000);
    mc2_anim_rng::restore(saved, draws);
    for (int i = 0; i < 1000; ++i) ar::randomNumber(1000);
    CHECK(mc2_anim_rng::randomNumber(1000) == next);
    CHECK(mc2_anim_rng::drawCount() == draws + 1);
    mc2_anim_rng::reseed(17);
    CHECK(mc2_anim_rng::randomNumber(1000) == first);
}

TEST_CASE("appearance_rng: the same seed replays the same sequence and draw count") {
	ar::reseed(0x1234u);
	std::vector<long> first;
	for (int i = 0; i < 200; ++i)
		first.push_back(ar::randomNumber(1000));
	for (int i = 0; i < 50; ++i)
		first.push_back(ar::rollDice(50) ? 1 : 0);
	const uint32_t draws = ar::drawCount();
	CHECK(draws == 250u);

	ar::reseed(0x1234u);
	CHECK(ar::drawCount() == 0u);
	for (int i = 0; i < 200; ++i)
		CHECK(ar::randomNumber(1000) == first[(size_t)i]);
	for (int i = 0; i < 50; ++i)
		CHECK((ar::rollDice(50) ? 1 : 0) == first[(size_t)(200 + i)]);
	CHECK(ar::drawCount() == draws);
}

TEST_CASE("appearance_rng: values stay in range and a non-positive range draws nothing") {
	ar::reseed(7u);
	for (int i = 0; i < 10000; ++i) {
		const long v = ar::randomNumber(17);
		CHECK(v >= 0);
		CHECK(v < 17);
	}
	const uint32_t before = ar::drawCount();
	CHECK(ar::randomNumber(0) == 0);
	CHECK(ar::randomNumber(-5) == 0);
	CHECK(ar::drawCount() == before);
}

TEST_CASE("appearance_rng: rollDice honours the extremes") {
	ar::reseed(99u);
	for (int i = 0; i < 1000; ++i) {
		CHECK_FALSE(ar::rollDice(0));
		CHECK(ar::rollDice(100));
	}
}

TEST_CASE("appearance_rng: the sequence is not the simulation's for the same seed") {
	// The simulation stream is the bare LCG seeded with the mission seed; the appearance stream must not replay it, or a
	// mission would roll its combat and its decals from the same numbers.
	const uint32_t seed = mc2_det_rng::hashMissionName("mc2_24");
	uint32_t sim = seed;
	ar::reseed(seed);
	bool differs = false;
	for (int i = 0; i < 20 && !differs; ++i)
		differs = (ar::randomNumber(32768) != (long)mc2_det_rng::next15(sim));
	CHECK(differs);
}

TEST_CASE("imported-mech jump/land clip memory survives a save and load mid-jump") {
    mc2::sim::UnitAnimState before;
    mc2clip::Phase live;
    // Airborne, then the jump gesture ends: the land clip is owed.
    mc2clip::select(mc2clip::kGestureJump, 0, 0, 0, false, false, "atlas_moveJumpUpStart", live);
    mc2clip::select(4, 0, 0, 0, false, false, "atlas_moveJumpAirIdle", live);
    REQUIRE(live.landing);
    before.imported.phase = live;
    before.imported.mode = mc2clip::Mode::Once;
    before.imported.locomotion = true;
    mc2::save::Archive write;
    write(before);
    REQUIRE(write.good());
    mc2::sim::UnitAnimState after;
    mc2::save::Archive read(write.bytes());
    read(after);
    REQUIRE(read.finish());
    CHECK(after.imported.phase.landing);
    CHECK(after.imported.phase.prevGesture == live.prevGesture);
    CHECK(after.imported.mode == mc2clip::Mode::Once);
    CHECK(after.imported.locomotion);
    // The restored selector picks the same next clip as the uninterrupted one.
    auto resumed = after.imported.phase;
    const auto a = mc2clip::select(4, 0, 0, 0, false, true, "atlas_moveJumpDownLand", live);
    const auto b = mc2clip::select(4, 0, 0, 0, false, true, "atlas_moveJumpDownLand", resumed);
    CHECK(std::string(a.clip) == b.clip);
    CHECK(a.mode == b.mode);
}
