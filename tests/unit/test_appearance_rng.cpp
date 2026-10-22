// The appearance-side random stream: replayable from its seed, in range, and separate from the simulation's sequence.
#include "doctest.h"
#include "mclib/appearance_rng.h"
#include "mclib/det_rng.h"

#include <vector>

namespace ar = mc2_appearance_rng;

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
