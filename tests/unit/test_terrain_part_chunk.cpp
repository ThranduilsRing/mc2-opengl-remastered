#include "doctest.h"
#include "terrain_part_chunk.h"
#include "tacorder_terrain_queue.h"
#include "save_struct_zero.h"
#include "save_field_probe.h"

bool operator==(const _GlobalPathStep& a, const _GlobalPathStep& b) {
	return a.startDoor == b.startDoor && a.thruArea == b.thruArea && a.goalDoor == b.goalDoor &&
		a.start == b.start && a.goal == b.goal && a.goalCell[0] == b.goalCell[0] &&
		a.goalCell[1] == b.goalCell[1] && a.costToGoal == b.costToGoal;
}

TEST_CASE("save fields: warrior queue tail independent member census") {
	using T = SavedQueueTargets;
	T sample;
	sample.targets = {0x01010101, 0x02020202};
	sample.curGlobalStep = 1;
	sample.lastGoalPath = {0x0303, 0x0404};
	SAVE_FIELD_SHAPE(sample, targets, globalPath, curGlobalStep, lastGoalPath);
	using S = GlobalPathStep;
	sample.globalPath = {save_probe::patterned<S>(&S::startDoor, &S::thruArea, &S::goalDoor,
		&S::start, &S::goal, &S::goalCell, &S::costToGoal)};
	SAVE_FIELD_SHAPE(sample.globalPath[0], startDoor, thruArea, goalDoor, start, goal, goalCell, costToGoal);
	CHECK(save_probe::memberBytes<S>(&S::startDoor, &S::thruArea, &S::goalDoor,
		&S::start, &S::goal, &S::goalCell, &S::costToGoal) == sizeof(S));
	CHECK(save_probe::memberBytes<T>(&T::targets, &T::globalPath, &T::curGlobalStep,
		&T::lastGoalPath) + 4 == sizeof(T));
	CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, &T::targets,
		&T::globalPath, &T::curGlobalStep, &T::lastGoalPath));
}

TEST_CASE("saved active order retains identity and mid-order execution state") {
	MC2_SAVE_STRUCT(TacticalOrder, active);
	active.id = 70001;
	active.code = TACTICAL_ORDER_ATTACK_POINT;
	active.targetWID = 80002;
	active.lastMoveGoal = glm::vec3(60000.0f, -50000.0f, 12.0f);
	active.attackParams.targetPoint = active.lastMoveGoal;
	active.stage = 2;
	active.statusCode = 3;
	active.time = 15.5f;
	active.moveParams.wayPath.curPoint = 2;
	MC2_SAVE_STRUCT(TacticalOrder, saved);
	copySavedTacticalOrder(saved, active);
	MC2_SAVE_STRUCT(TacticalOrder, loaded);
	copySavedTacticalOrder(loaded, saved);
	CHECK(loaded.id == active.id);
	CHECK(loaded.targetWID == active.targetWID);
	CHECK(loaded.lastMoveGoal == active.lastMoveGoal);
	CHECK(loaded.attackParams.targetPoint == active.attackParams.targetPoint);
	CHECK(loaded.stage == active.stage);
	CHECK(loaded.statusCode == active.statusCode);
	CHECK(loaded.time == active.time);
	CHECK(loaded.moveParams.wayPath.curPoint == 2);
}
namespace {
struct ChunkScope {
	long stride = gTerrainPartIdStride;
	terrain_part_chunk::Registry saved = std::move(terrain_part_chunk::registry());

	ChunkScope() { terrain_part_chunk::registry() = {}; }
	~ChunkScope() {
		gTerrainPartIdStride = stride;
		terrain_part_chunk::registry() = std::move(saved);
	}
};
}

TEST_CASE("compact terrain orders are dropped on load even for stale valid indices") {
	ChunkScope scope;
	gTerrainPartIdStride = 3072;
	const auto field = terrain_part_chunk::pack(MIN_TERRAIN_PART_ID + 42);
	terrain_part_chunk::registry() = {};
	CHECK_FALSE(terrain_part_chunk::unpack(field).has_value());
	terrain_part_chunk::pack(MIN_TERRAIN_PART_ID + 99);
	CHECK(terrain_part_chunk::unpack(field) == MIN_TERRAIN_PART_ID + 99);
	struct Queued { unsigned long packedData[2]; };
	for (auto code : {TACTICAL_ORDER_GUARD, TACTICAL_ORDER_ATTACK_OBJECT,
		TACTICAL_ORDER_MOVETO_OBJECT, TACTICAL_ORDER_JUMPTO_OBJECT,
		TACTICAL_ORDER_CAPTURE, TACTICAL_ORDER_REFIT, TACTICAL_ORDER_RECOVER, TACTICAL_ORDER_GETFIXED}) {
		Queued queue[] = {{static_cast<unsigned long>(code), 1UL << 9},
			{TACTICAL_ORDER_MOVETO_POINT, 1UL << 9}, {31, 1UL << 9},
			{TACTICAL_ORDER_ATTACK_OBJECT, 0}};
		char count = 4;
		int32_t index = 2;
		dropCompactTerrainOrdersOnLoad(queue, count, index);
		CHECK(count == 3);
		CHECK(index == 1);
		CHECK(queue[0].packedData[0] == TACTICAL_ORDER_MOVETO_POINT);
		CHECK(queue[1].packedData[0] == 31);
	}
	Queued queue[] = {{TACTICAL_ORDER_ATTACK_OBJECT, 1UL << 9}};
	char count = 1;
	int32_t index = 0;
	gTerrainPartIdStride = 720;
	dropCompactTerrainOrdersOnLoad(queue, count, index);
	CHECK(count == 1);
	gTerrainPartIdStride = 3072;
	dropCompactTerrainOrdersOnLoad(queue, count, index);
	CHECK(count == 0);
	CHECK(index == 0);
}

TEST_CASE("terrain chunks preserve stock and mod 20-bit wire values") {
	ChunkScope scope;
	for (long side : {720L, 900L, 1024L}) {
		gTerrainPartIdStride = side;
		for (long offset : {0L, 1L, side * side - 1}) {
			const long id = MIN_TERRAIN_PART_ID + offset;
			CHECK(terrain_part_chunk::pack(id) == static_cast<unsigned long>(offset));
			CHECK(terrain_part_chunk::unpack(offset) == id);
		}
	}
	CHECK(terrain_part_chunk::registry().reverse.empty());
}

TEST_CASE("terrain chunks distinguish big-map cells with identical low 20 bits") {
	ChunkScope scope;
	gTerrainPartIdStride = 3072;
	CHECK_FALSE(terrain_part_chunk::unpack(0).has_value());
	const long first = MIN_TERRAIN_PART_ID + 1;
	const long alias = first + (1L << 20);
	const long corner = MIN_TERRAIN_PART_ID + 3072L * 3072L - 1;
	const auto a = terrain_part_chunk::pack(first);
	const auto b = terrain_part_chunk::pack(alias);
	const auto c = terrain_part_chunk::pack(corner);
	CHECK(a != b);
	CHECK(b != c);
	CHECK(c <= terrain_part_chunk::kFieldMax);
	CHECK(terrain_part_chunk::pack(alias) == b);
	CHECK(terrain_part_chunk::unpack(a) == first);
	CHECK(terrain_part_chunk::unpack(b) == alias);
	CHECK(terrain_part_chunk::unpack(c) == corner);
	CHECK_FALSE(terrain_part_chunk::unpack(c + 1).has_value());
}

TEST_CASE("v2 queued targets survive stock and big map save load with a fresh registry") {
	ChunkScope scope;
	for (long side : {720L, 3072L}) {
		gTerrainPartIdStride = side;
		const long part = MIN_TERRAIN_PART_ID + side * side - 1;
		const auto compact = terrain_part_chunk::pack(part);
		// The watch table is restored by ObjectManager::Load; the queue carries its
		// full watch identity, never the process-local terrain registry index.
		SavedQueueTargets saved{{70001, 0, 80002}};
		mc2::save::Archive write;
		write(saved);
		REQUIRE(write.good());
		terrain_part_chunk::registry() = {};
		terrain_part_chunk::pack(MIN_TERRAIN_PART_ID + 99);
		if (side > 1024) CHECK(terrain_part_chunk::unpack(compact) != part);
		SavedQueueTargets loaded;
		REQUIRE(mc2::save::decodeRecord(write.bytes(), loaded));
		CHECK(loaded.targets == saved.targets);
	}
}

TEST_CASE("v2 queued target codec rejects overflow truncation and unknown versions") {
	SavedQueueTargets saved;
	saved.targets.resize(17);
	mc2::save::Archive tooMany;
	tooMany(saved);
	CHECK_FALSE(tooMany.good());
	saved.targets.resize(16, 70001);
	mc2::save::Archive write;
	write(saved);
	auto bytes = write.bytes();
	SavedQueueTargets loaded;
	bytes.pop_back();
	CHECK_FALSE(mc2::save::decodeRecord(bytes, loaded));
	bytes = write.bytes();
	bytes[4] = 3;
	CHECK_FALSE(mc2::save::decodeRecord(bytes, loaded));
}

TEST_CASE("v2 order paths retain 256 steps and a cursor beyond the legacy byte count") {
	SavedQueueTargets saved;
	saved.globalPath.resize(MAX_GLOBAL_PATH);
	saved.lastGoalPath.assign(MAX_GLOBAL_PATH, 29999);
	saved.curGlobalStep = 200;
	for (auto& step : saved.globalPath) {
		step.startDoor = -1;
		step.thruArea = 29999;
		step.goalDoor = 32765;
		step.start = glm::vec3(-60000.0f, 50000.0f, 0.0f);
		step.goal = glm::vec3(60000.0f, -50000.0f, 0.0f);
		step.goalCell[0] = step.goalCell[1] = 3071;
		step.costToGoal = 1000000;
	}
	mc2::save::Archive write;
	write(saved);
	REQUIRE(write.good());
	SavedQueueTargets loaded;
	REQUIRE(mc2::save::decodeRecord(write.bytes(), loaded));
	CHECK(loaded.globalPath.size() == MAX_GLOBAL_PATH);
	CHECK(loaded.curGlobalStep == 200);
	CHECK(loaded.lastGoalPath == saved.lastGoalPath);
	CHECK(loaded.globalPath.back().thruArea == 29999);
	CHECK(loaded.globalPath.back().goalDoor == 32765);
	CHECK(loaded.globalPath.back().goalCell[1] == 3071);
	CHECK(loaded.globalPath.back().goal == saved.globalPath.back().goal);
	saved.globalPath.resize(MAX_GLOBAL_PATH + 1);
	mc2::save::Archive overflow;
	overflow(saved);
	CHECK_FALSE(overflow.good());
}

// ---- Merge window 40: the four save changes together (WQT2 wide targets, R2 anim v3, pool-order trailer, exact clocks) ----
#include "unit_anim_state.h"
#include "simulation_save_section.h"
#include "save_exactness.h"
#include <cstdlib>

namespace {
struct FakeObj { int id; };
}

TEST_CASE("save v3 round trip: wide queue, anim v3 with jump phase, pool order and exact clocks together") {
	SavedQueueTargets queue;
	queue.targets = {70001, 0, 80002};
	queue.globalPath.resize(MAX_GLOBAL_PATH);
	queue.lastGoalPath.assign(MAX_GLOBAL_PATH, 29999);
	queue.curGlobalStep = 200;
	for (auto& step : queue.globalPath) step.thruArea = 29999;
	mc2::sim::UnitAnimState anim;
	anim.imported.activeClip = "atlas_moveJumpAirIdle";
	anim.imported.phase.jumpStarted = true;
	anim.imported.phase.landing = true;
	anim.imported.phase.prevGesture = 4;
	anim.imported.mode = mc2clip::Mode::Once;
	anim.imported.locomotion = true;
	std::vector<FakeObj> objs(5);
	FakeObj* list[6] = {nullptr};
	for (int i = 1; i <= 5; ++i) { objs[i - 1].id = i; list[i] = &objs[i - 1]; }
	std::vector<int32_t> turretOrder = {3, 1, 2}, buildingOrder = {5, 4};
	// One ulp above a 1/30 multiple: the legacy six-decimal text could not represent it.
	const float clock = std::nextafter(1.0f / 30.0f * 7.0f, 10.0f);

	mc2::save::Archive write;
	uint32_t tag = 0x4E413252u, version = 3;
	write(tag, version);
	mc2::save::Bytes blob;
	{
		mc2::save::Archive inner;
		int32_t handle = 7;
		inner(handle, anim, turretOrder, buildingOrder);
		REQUIRE(inner.good());
		blob = inner.bytes();
	}
	mc2::sim::serializeAnimationBlob(write, blob);
	queue.serialize(write);
	REQUIRE(write.good());

	mc2::save::Archive read(write.bytes());
	uint32_t rtag = 0, rversion = 0;
	read(rtag, rversion);
	CHECK(rtag == tag);
	CHECK(rversion == 3);
	read.sectionVersion = rversion;
	mc2::save::Bytes rblob;
	mc2::sim::serializeAnimationBlob(read, rblob);
	SavedQueueTargets rq;
	rq.serialize(read);
	REQUIRE(read.finish());
	CHECK(rq.targets == queue.targets);
	CHECK(rq.globalPath.size() == MAX_GLOBAL_PATH);
	CHECK(rq.curGlobalStep == 200);

	mc2::save::Archive inner(rblob);
	int32_t handle = 0;
	mc2::sim::UnitAnimState ranim;
	std::vector<int32_t> rturrets, rbuildings;
	inner(handle, ranim, rturrets, rbuildings);
	REQUIRE(inner.finish());
	CHECK(handle == 7);
	CHECK(ranim.imported.phase.landing);
	CHECK(ranim.imported.phase.jumpStarted);
	CHECK(ranim.imported.phase.prevGesture == 4);
	CHECK(ranim.imported.mode == mc2clip::Mode::Once);
	CHECK(ranim.imported.locomotion);

	FakeObj* turrets[3] = {&objs[0], &objs[1], &objs[2]};
	mc2::save::applyPoolOrder(std::span<FakeObj*>(turrets, 3), rturrets, list, 5);
	CHECK(turrets[0]->id == 3);
	CHECK(turrets[1]->id == 1);
	CHECK(turrets[2]->id == 2);
	FakeObj* buildings[2] = {&objs[3], &objs[4]};
	mc2::save::applyPoolOrder(std::span<FakeObj*>(buildings, 2), rbuildings, list, 5);
	CHECK(buildings[0]->id == 5);
	// A list that is not a permutation of the pool leaves it untouched.
	FakeObj* stray[3] = {&objs[0], &objs[1], &objs[2]};
	mc2::save::applyPoolOrder(std::span<FakeObj*>(stray, 3), std::vector<int32_t>{1, 2, 4}, list, 5);
	CHECK(stray[0]->id == 1);
	CHECK(stray[2]->id == 3);

	char line[255];
	mc2::save::exactFloatLine(line, "ActualTime", clock);
	const char* eq = std::strchr(line, '=');
	REQUIRE(eq);
	CHECK(std::strtof(eq + 1, nullptr) == clock);
}

TEST_CASE("a pre-change R2 animation record (v2, no selector memory) loads with defaults; bad modes reject") {
	mc2::sim::UnitAnimState v3;
	v3.imported.activeClip = "walk";
	v3.imported.clipTimeSec = 0.5f;
	v3.imported.phase.landing = true;
	v3.imported.mode = mc2clip::Mode::Once;
	v3.imported.locomotion = true;
	mc2::save::Archive current;
	current(v3);
	REQUIRE(current.good());
	// Synthesised v2 blob: a v2 archive writes the record without the selector memory.
	mc2::save::Archive legacy;
	legacy.sectionVersion = 2;
	legacy(v3);
	REQUIRE(legacy.good());
	CHECK(legacy.bytes().size() < current.bytes().size());
	mc2::sim::UnitAnimState loaded;
	mc2::save::Archive read(legacy.bytes());
	read.sectionVersion = 2;
	read(loaded);
	REQUIRE(read.finish());
	CHECK(loaded.imported.activeClip == "walk");
	CHECK(loaded.imported.clipTimeSec == 0.5f);
	CHECK_FALSE(loaded.imported.phase.landing);
	CHECK(loaded.imported.mode == mc2clip::Mode::Loop);
	CHECK_FALSE(loaded.imported.locomotion);
	// The v2 bytes read as v3 must not pass silently.
	mc2::sim::UnitAnimState wrong;
	mc2::save::Archive misread(legacy.bytes());
	misread(wrong);
	CHECK_FALSE(misread.finish());
	// imported is the last member: the final five bytes are modeInt (int32) and locomotion.
	auto bad = current.bytes();
	bad[bad.size() - 5] = 2;
	mc2::sim::UnitAnimState rejected;
	mc2::save::Archive badRead(bad);
	badRead(rejected);
	CHECK_FALSE(badRead.good());
}

TEST_CASE("a cursor of -1 (replan failed on a complex route) saves and loads; below -1 is rejected") {
	SavedQueueTargets saved;
	saved.globalPath.resize(2);
	saved.curGlobalStep = -1;
	mc2::save::Archive write;
	write(saved);
	REQUIRE(write.good());
	SavedQueueTargets loaded;
	REQUIRE(mc2::save::decodeRecord(write.bytes(), loaded));
	CHECK(loaded.curGlobalStep == -1);
	saved.curGlobalStep = -2;
	mc2::save::Archive bad;
	bad(saved);
	CHECK_FALSE(bad.good());
}

TEST_CASE("a dev-window v2 record (v3 layout stamped version 2) fails the v2 parse and loads as v3") {
	mc2::sim::UnitAnimState src;
	src.imported.activeClip = "walk";
	src.imported.phase.landing = true;
	src.imported.mode = mc2clip::Mode::Once;
	src.imported.locomotion = true;
	mc2::save::Archive oldWindow;      // de4413807..f8b4174c3 wrote exactly the v3 field list
	oldWindow(src);
	REQUIRE(oldWindow.good());
	mc2::sim::UnitAnimState asV2;
	mc2::save::Archive v2(oldWindow.bytes());
	v2.sectionVersion = 2;
	v2(asV2);
	CHECK_FALSE(v2.finish());          // trailing selector bytes: the loader sees a rejection and retries as v3
	mc2::sim::UnitAnimState asV3;
	mc2::save::Archive v3(oldWindow.bytes());
	v3.sectionVersion = 3;
	v3(asV3);
	REQUIRE(v3.finish());
	CHECK(asV3.imported.phase.landing);
	CHECK(asV3.imported.mode == mc2clip::Mode::Once);
	CHECK(asV3.imported.locomotion);
}

// ---- Merge 42 follow-up: animation tail (pool orders + sensor scan) and the v2 -> v3 retry over a multi-record section ----
namespace {
mc2::save::AnimationTail readTail(const std::vector<uint8_t>& bytes, bool& ok, int numTeams = 4, bool haveSensor = true) {
	mc2::save::Archive a(bytes);
	mc2::save::AnimationTail t;
	mc2::save::readAnimationTail(a, t, numTeams, haveSensor);
	ok = a.finish();
	return t;
}
std::vector<uint8_t> orders(bool withScan, int32_t cursor = 1) {
	mc2::save::Archive w;
	std::vector<int32_t> a{3, 5}, none;
	for (int i = 0; i < 6; ++i) w(i == 0 ? a : none);
	if (withScan) { bool los = true; w(cursor, los); }
	REQUIRE(w.good());
	return w.bytes();
}
}

TEST_CASE("animation tail: an absent tail keeps the defaults and loads") {
	bool ok = false;
	auto t = readTail({}, ok);
	CHECK(ok);
	CHECK_FALSE(t.hasScan);
	CHECK(t.order[0].empty());
	auto orderOnly = readTail(orders(false), ok);
	CHECK(ok);
	CHECK_FALSE(orderOnly.hasScan);
	CHECK(orderOnly.order[0].size() == 2);
}

TEST_CASE("animation tail: a present tail reads the orders and scan state") {
	bool ok = false;
	auto t = readTail(orders(true, 2), ok);
	CHECK(ok);
	CHECK(t.hasScan);
	CHECK(t.scanCursor == 2);
	CHECK(t.enemyInLOS);
	CHECK(t.order[0] == std::vector<int32_t>{3, 5});
}

TEST_CASE("animation tail: truncated bytes are rejected, not half-read") {
	bool ok = true;
	auto full = orders(true);
	for (size_t cut : {size_t(1), size_t(2), size_t(4)}) {
		ok = true;
		readTail(std::vector<uint8_t>(full.begin(), full.end() - cut), ok);
		CHECK_FALSE(ok);
	}
	auto partial = orders(false);
	partial.resize(3);                       // a partial first pool order
	readTail(partial, ok);
	CHECK_FALSE(ok);
}

TEST_CASE("animation tail: a bad scan cursor or a missing sensor manager rejects the section") {
	bool ok = true;
	readTail(orders(true, 4), ok, 4);      // cursor == numTeams
	CHECK_FALSE(ok);
	readTail(orders(true, -1), ok, 4);
	CHECK_FALSE(ok);
	readTail(orders(true, 1), ok, 4, false);
	CHECK_FALSE(ok);
	readTail(orders(true, 3), ok, 4);
	CHECK(ok);
}

TEST_CASE("v3 section with several records: reading it as v2 is rejected so the loader retries as v3") {
	auto sectionOk = [](const std::vector<uint8_t>& bytes, uint32_t version) {
		mc2::save::Archive a(bytes);
		a.sectionVersion = version;
		uint32_t state = 0, draws = 0, animState = 0, animDraws = 0, count = 0;
		a(state, draws, animState, animDraws, count);
		int32_t previous = 0;
		for (uint32_t i = 0; a.good() && i < count && i < 64; ++i) {
			int32_t handle = 0;
			mc2::sim::UnitAnimState anim;
			a(handle, anim);
			if (a.good() && handle <= previous) return false;
			previous = handle;
		}
		return a.finish();
	};
	mc2::save::Archive w;
	uint32_t none = 0, count = 3;
	w(none, none, none, none, count);
	for (int32_t handle : {2, 5, 9}) {
		mc2::sim::UnitAnimState anim;
		anim.imported.activeClip = "atlas_moveJumpAirIdle";
		anim.imported.phase.landing = true;
		anim.imported.mode = mc2clip::Mode::Once;
		anim.imported.locomotion = true;
		w(handle, anim);
	}
	REQUIRE(w.good());
	CHECK(sectionOk(w.bytes(), 3));
	CHECK_FALSE(sectionOk(w.bytes(), 2));
}
