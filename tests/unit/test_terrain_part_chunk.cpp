#include "doctest.h"
#include "terrain_part_chunk.h"
#include "tacorder_terrain_queue.h"
#include "save_struct_zero.h"

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
