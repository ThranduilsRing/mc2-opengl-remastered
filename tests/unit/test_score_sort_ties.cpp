// tests/unit/test_score_sort_ties.cpp
//
// The size of the sort-rule decision, pinned. golden/sortlist_ties.h holds the permutations the OLD SortList (MSVC CRT qsort over a padded slot array)
// produced for tie patterns, recorded before the class was deleted. The same patterns
// are run through mc2::sort_scored here: every row where the CRT order is not "score, then fill slot" is a place where the simulation's outcome can
// change, and the number of such rows is pinned so that a change of the rule or of the table stays visible.
// The table is a record: the class that generated it is gone (git history before 29f95649a holds it and the generator test).

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

struct Pattern
{
	const char* name;
	int minN;
	std::vector<float> (*make)(int n);
};

std::vector<float> fill(int n, float (*f)(int i, int n))
{
	std::vector<float> v(n);
	for (int i = 0; i < n; ++i)
		v[i] = f(i, n);
	return v;
}

const Pattern kPatterns[] = {
	{"equal", 2, [](int n) { return fill(n, [](int, int) { return 1.0f; }); }},
	{"pairs", 2, [](int n) { return fill(n, [](int i, int) { return float(i / 2); }); }},
	{"runs3", 3, [](int n) { return fill(n, [](int i, int) { return float(i / 3); }); }},
	{"ascending", 2, [](int n) { return fill(n, [](int i, int) { return float(i); }); }},
	{"descending", 2, [](int n) { return fill(n, [](int i, int n2) { return float(n2 - 1 - i); }); }},
	{"alternating", 2, [](int n) { return fill(n, [](int i, int) { return float(i % 2); }); }},
	{"head_equal", 3, [](int n) { return fill(n, [](int i, int) { return float(i < 2 ? 0 : i); }); }},
	{"tail_equal", 3, [](int n) { return fill(n, [](int i, int n2) { return float(i >= n2 - 2 ? 100 : i); }); }},
	// Not strict weak orders under the old comparators: a NaN compared equal to everything. +-inf lay beyond the old sentinels.
	{"nan_mid", 3, [](int n) { return fill(n, [](int i, int n2) { return i == n2 / 2 ? kNaN : float(i); }); }},
	{"neginf_last", 3, [](int n) { return fill(n, [](int i, int n2) { return i == n2 - 1 ? -kInf : float(i); }); }},
	{"posinf_first", 3, [](int n) { return fill(n, [](int i, int) { return i == 0 ? kInf : float(i); }); }},
};

// The ids of the sorted items by the new rule, comma separated.
std::string newRuleOrder(const std::vector<float>& scores, bool descending)
{
	std::vector<mc2::Scored> items(scores.size());
	for (std::size_t i = 0; i < scores.size(); ++i)
	{
		items[i].score = scores[i];
		items[i].id = static_cast<std::int32_t>(i);
	}
	mc2::sort_scored(items, descending ? mc2::SortOrder::Descending : mc2::SortOrder::Ascending);
	std::string out;
	for (std::size_t i = 0; i < items.size(); ++i)
	{
		if (i)
			out += ",";
		out += std::to_string(items[i].id);
	}
	return out;
}

struct TieRow
{
	int n;
	const char* pattern;
	bool descending;
	int cap;
	const char* ids;
};

#include "golden/sortlist_ties.h"

const std::vector<float>* findScores(const char* name, int n, std::vector<float>& storage)
{
	for (const Pattern& p : kPatterns)
		if (std::string(p.name) == name)
		{
			storage = p.make(n);
			return &storage;
		}
	return nullptr;
}
} // namespace

TEST_SUITE("score_sort_ties")
{

TEST_CASE("ST1 the old CRT tie table against the new rule: how many recorded orders change")
{
	REQUIRE(sizeof(kTieTable) / sizeof(kTieTable[0]) > 100);
	int differ = 0;
	int tieRows = 0;
	for (const TieRow& row : kTieTable)
	{
		std::vector<float> storage;
		const std::vector<float>* scores = findScores(row.pattern, row.n, storage);
		REQUIRE(scores != nullptr);
		const bool hasNaN = std::any_of(scores->begin(), scores->end(), [](float f) { return std::isnan(f); });
		if (!hasNaN && std::string(row.pattern) != "neginf_last")
		{
			++tieRows;
			if (newRuleOrder(*scores, row.descending) != std::string(row.ids))
				++differ;
		}
	}
	// Rows (tie patterns without NaN / -inf) where qsort's order is not (score, fill slot). Pinned from the generated table.
	INFO("tie rows " << tieRows << ", differ " << differ);
	CHECK(tieRows == int(MC2_HS2_EXPECT_TIE_ROWS));
	CHECK(differ == int(MC2_HS2_EXPECT_DIFFER));
}

TEST_CASE("ST2 the small-range signature of the CRT qsort is gone: equal items are not reversed or rotated")
{
	// The old class returned "1,0" for two equal items and "1,2,0" for three, in both directions.
	CHECK(newRuleOrder({5.0f, 5.0f}, true) == "0,1");
	CHECK(newRuleOrder({5.0f, 5.0f, 5.0f}, true) == "0,1,2");
	CHECK(newRuleOrder({5.0f, 5.0f, 5.0f}, false) == "0,1,2");
}

TEST_CASE("ST3 the cases the old class got wrong: -inf stays in the prefix, a NaN no longer breaks the order")
{
	// Old: a -inf score sorted below the -3.4E38 sentinels, so a sentinel slot entered the filled prefix ("0,2,198").
	CHECK(newRuleOrder({5.0f, -kInf, 3.0f}, true) == "0,2,1");
	// Old: a NaN compared equal to everything, so the result was whatever the CRT did. Now: NaN last, the numbers in order.
	CHECK(newRuleOrder({1.0f, kNaN, 3.0f, 2.0f}, true) == "2,3,0,1");
	CHECK(newRuleOrder({1.0f, kNaN, 3.0f, 2.0f}, false) == "0,3,2,1");
}

} // TEST_SUITE
