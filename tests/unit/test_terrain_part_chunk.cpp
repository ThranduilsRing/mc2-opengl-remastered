#include "doctest.h"
#include "terrain_part_chunk.h"

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
