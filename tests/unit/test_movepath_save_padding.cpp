// SAVE-MOVEPATH-PAD-1: a MovePath record in a save has zero padding however the live object was built, copied or loaded.
// The save write point (SaveableMoveOrders::copy) canonicalizes the staged record; copyTo is the load direction, so an old record with
// nonzero padding that is loaded and re-saved comes out clean.
#include "doctest.h"
#include "warrior.h"

#include <cstddef>
#include <cstring>
#include <vector>

namespace {

// Every padding byte position of a MovePath record, computed from the layout (the static_asserts in move.h pin it).
std::vector<size_t> paddingPositions() {
	std::vector<size_t> pos;
	for (int i = 0; i < MAX_STEPS_PER_MOVEPATH; ++i) {
		const size_t step = offsetof(MovePath, stepList) + sizeof(PathStep) * i;
		for (size_t b = offsetof(PathStep, direction) + sizeof(char); b < offsetof(PathStep, area); ++b)
			pos.push_back(step + b);
		for (size_t b = offsetof(PathStep, area) + sizeof(short); b < sizeof(PathStep); ++b)
			pos.push_back(step + b);
	}
	for (size_t b = offsetof(MovePath, marked) + sizeof(bool); b < offsetof(MovePath, globalStep); ++b)
		pos.push_back(b);
	for (size_t b = offsetof(MovePath, globalStep) + sizeof(long); b < sizeof(MovePath); ++b)
		pos.push_back(b);
	return pos;
}

void fillMeaningful(MovePath& p) {
	p.goal = glm::vec3(1.0f, 2.0f, 3.0f);
	p.target = glm::vec3(4.0f, 5.0f, 6.0f);
	p.numSteps = p.numStepsWhenNotPaused = 7;
	p.curStep = 2;
	p.cost = 99;
	p.marked = true;
	p.globalStep = 5;
	for (int i = 0; i < MAX_STEPS_PER_MOVEPATH; ++i) {
		p.stepList[i].cell[0] = static_cast<short>(i);
		p.stepList[i].cell[1] = static_cast<short>(i + 1);
		p.stepList[i].distanceToGoal = static_cast<float>(i) * 0.5f;
		p.stepList[i].destination = glm::vec3(static_cast<float>(i));
		p.stepList[i].direction = static_cast<char>(i & 7);
		p.stepList[i].area = static_cast<short>(i * 3);
	}
}

void scribblePadding(MovePath& p, unsigned char value) {
	unsigned char* raw = reinterpret_cast<unsigned char*>(&p);
	for (size_t b : paddingPositions())
		raw[b] = value;
}

bool paddingIsZero(const MovePath& p) {
	const unsigned char* raw = reinterpret_cast<const unsigned char*>(&p);
	for (size_t b : paddingPositions())
		if (raw[b] != 0)
			return false;
	return true;
}

bool sameBytes(const MovePath& a, const MovePath& b) {
	return std::memcmp(&a, &b, sizeof(MovePath)) == 0;
}

} // namespace

TEST_CASE("MovePath record: padding positions are the ones the layout leaves") {
	// One gap byte per step plus 3 after 'marked'.
	CHECK(paddingPositions().size() == static_cast<size_t>(MAX_STEPS_PER_MOVEPATH) + 3);
}

TEST_CASE("SaveableMoveOrders::copy: records differing only in padding emit identical zero-padded bytes") {
	static MovePath a0, a1, b0, b1;
	fillMeaningful(a0); fillMeaningful(a1); fillMeaningful(b0); fillMeaningful(b1);
	scribblePadding(a0, 0xAA); scribblePadding(a1, 0xAA);
	scribblePadding(b0, 0x55); scribblePadding(b1, 0x55);
	REQUIRE(!sameBytes(a0, b0));

	static MoveOrders liveA{}, liveB{};
	liveA.path[0] = &a0; liveA.path[1] = &a1;
	liveB.path[0] = &b0; liveB.path[1] = &b1;
	static SaveableMoveOrders savedA{}, savedB{};
	savedA.copy(liveA);
	savedB.copy(liveB);

	for (int i = 0; i < 2; ++i) {
		CHECK(paddingIsZero(savedA.path[i]));
		CHECK(paddingIsZero(savedB.path[i]));
		CHECK(sameBytes(savedA.path[i], savedB.path[i]));
	}
	CHECK(savedA.path[0].numSteps == 7);
	CHECK(savedA.path[0].stepList[9].area == 27);
}

TEST_CASE("old record with nonzero padding: copyTo (load) then copy (re-save) writes zero padding") {
	static MovePath old0, old1, scratch0, scratch1;
	fillMeaningful(old0); fillMeaningful(old1);

	// The record as an old save holds it, padding included.
	static SaveableMoveOrders loaded{};
	loaded.path[0] = old0;
	loaded.path[1] = old1;
	scribblePadding(loaded.path[0], 0xCD);
	scribblePadding(loaded.path[1], 0xEE);
	REQUIRE(!paddingIsZero(loaded.path[0]));

	// Load direction, then save again.
	static MoveOrders live{};
	live.path[0] = &scratch0; live.path[1] = &scratch1;
	loaded.copyTo(live);
	static SaveableMoveOrders resaved{};
	resaved.copy(live);

	for (int i = 0; i < 2; ++i) {
		CHECK(paddingIsZero(resaved.path[i]));
		CHECK(resaved.path[i].cost == 99);
		CHECK(resaved.path[i].stepList[199].distanceToGoal == doctest::Approx(99.5f));
	}
}
