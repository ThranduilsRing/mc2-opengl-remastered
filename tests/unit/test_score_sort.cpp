// tests/unit/test_score_sort.cpp
//
// mc2::sort_scored (mclib/score_sort.h): the ordering rule of the candidate lists the simulation sorts (contacts by distance or value, movers by
// threat, weapons by attack chance). WHY: equal scores must come out in the order the items were filled, whatever the library's sort does, and a
// NaN must never sit between two numbers; the old SortList (qsort over a padded array) gave neither guarantee, and step 4 B2.4 replaces it.

#include "doctest.h"

#include "score_sort.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace
{
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// The ids after sorting `scores` (id = position in the input), comma separated.
std::string order(const std::vector<float>& scores, mc2::SortOrder dir)
{
	std::vector<mc2::Scored> items(scores.size());
	for (std::size_t i = 0; i < scores.size(); ++i)
	{
		items[i].score = scores[i];
		items[i].id = static_cast<std::int32_t>(i);
	}
	mc2::sort_scored(items, dir);
	std::string out;
	for (std::size_t i = 0; i < items.size(); ++i)
	{
		if (i)
			out += ",";
		out += std::to_string(items[i].id);
	}
	return out;
}
} // namespace

TEST_SUITE("score_sort")
{

TEST_CASE("SS1 distinct scores sort in the requested direction")
{
	const std::vector<float> v = {3.0f, 9.0f, 1.0f, 7.0f};
	CHECK(order(v, mc2::SortOrder::Descending) == "1,3,0,2");
	CHECK(order(v, mc2::SortOrder::Ascending) == "2,0,3,1");
}

TEST_CASE("SS2 equal scores keep the fill order in both directions")
{
	// The CRT qsort the old list used gave "1,0" for two equal items and "1,2,0" for three.
	CHECK(order({5.0f, 5.0f}, mc2::SortOrder::Descending) == "0,1");
	CHECK(order({5.0f, 5.0f}, mc2::SortOrder::Ascending) == "0,1");
	CHECK(order({5.0f, 5.0f, 5.0f}, mc2::SortOrder::Descending) == "0,1,2");
	CHECK(order({1.0f, 2.0f, 2.0f, 2.0f, 1.0f}, mc2::SortOrder::Descending) == "1,2,3,0,4");
	CHECK(order({1.0f, 2.0f, 2.0f, 2.0f, 1.0f}, mc2::SortOrder::Ascending) == "0,4,1,2,3");
}

TEST_CASE("SS3 a NaN sorts after every number in both directions and NaNs keep the fill order")
{
	const std::vector<float> v = {1.0f, kNaN, 3.0f, kNaN, 2.0f};
	CHECK(order(v, mc2::SortOrder::Descending) == "2,4,0,1,3");
	CHECK(order(v, mc2::SortOrder::Ascending) == "0,4,2,1,3");
	CHECK(order({kNaN, kNaN, kNaN}, mc2::SortOrder::Descending) == "0,1,2");
}

TEST_CASE("SS4 minus zero equals plus zero, infinities are ordinary numbers")
{
	CHECK(order({0.0f, -0.0f, 0.0f}, mc2::SortOrder::Descending) == "0,1,2");
	CHECK(order({-0.0f, 0.0f}, mc2::SortOrder::Ascending) == "0,1");
	CHECK(order({1.0f, kInf, -kInf, kInf}, mc2::SortOrder::Descending) == "1,3,0,2");
	CHECK(order({1.0f, kInf, -kInf, kInf}, mc2::SortOrder::Ascending) == "2,0,1,3");
}

TEST_CASE("SS5 the id travels with its score, including the weapon slot none (-1), and a preset fillSlot is ignored")
{
	mc2::Scored items[3];
	items[0] = {-999.0f, -1, 77u};
	items[1] = {4.0f, 12, 5u};
	items[2] = {4.0f, 9, 0u};
	mc2::sort_scored(items, mc2::SortOrder::Descending);
	CHECK(items[0].id == 12);
	CHECK(items[1].id == 9);
	CHECK(items[2].id == -1);
	CHECK(items[2].score == -999.0f);
}

TEST_CASE("SS6 empty and single-item spans are left alone, and only the span is touched")
{
	mc2::Scored one[1] = {{2.0f, 7, 0u}};
	mc2::sort_scored(std::span<mc2::Scored>{}, mc2::SortOrder::Ascending);
	mc2::sort_scored(one, mc2::SortOrder::Ascending);
	CHECK(one[0].id == 7);

	// Items past the span (the unsorted tail a caller keeps) stay where they are.
	mc2::Scored four[4] = {{1.0f, 0, 0u}, {3.0f, 1, 0u}, {2.0f, 2, 0u}, {9.0f, 3, 0u}};
	mc2::sort_scored(std::span<mc2::Scored>(four, 3), mc2::SortOrder::Descending);
	CHECK(four[0].id == 1);
	CHECK(four[1].id == 2);
	CHECK(four[2].id == 0);
	CHECK(four[3].id == 3);
}

TEST_CASE("SS7 random lists with many ties equal a stable sort on the same key")
{
	std::uint32_t state = 12345u;
	const auto next = [&state] {
		state = state * 1664525u + 1013904223u;
		return state >> 16;
	};
	for (int round = 0; round < 200; ++round)
	{
		const int n = 1 + static_cast<int>(next() % 255);
		std::vector<float> scores(n);
		for (float& s : scores)
		{
			const std::uint32_t r = next() % 9;
			s = (r == 8) ? kNaN : static_cast<float>(r % 4); // 0..3 plus NaN: heavy ties
		}
		for (const mc2::SortOrder dir : {mc2::SortOrder::Descending, mc2::SortOrder::Ascending})
		{
			std::vector<int> expect(n);
			for (int i = 0; i < n; ++i)
				expect[i] = i;
			std::stable_sort(expect.begin(), expect.end(), [&](int a, int b) {
				const bool na = std::isnan(scores[a]);
				const bool nb = std::isnan(scores[b]);
				if (na || nb)
					return !na && nb;
				return dir == mc2::SortOrder::Descending ? scores[a] > scores[b] : scores[a] < scores[b];
			});
			std::string want;
			for (int i = 0; i < n; ++i)
				want += (i ? "," : "") + std::to_string(expect[i]);
			CHECK(order(scores, dir) == want);
		}
	}
}

} // TEST_SUITE
