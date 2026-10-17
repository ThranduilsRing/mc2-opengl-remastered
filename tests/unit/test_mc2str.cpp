// tests/unit/test_mc2str.cpp
//
// String helpers preserve null-as-empty and ASCII case-insensitive names.

#include "doctest.h"

#include "mc2str.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#ifndef MC2_TEST_FIXTURE_DIR
#error "MC2_TEST_FIXTURE_DIR must be defined by tests/unit/CMakeLists.txt"
#endif

namespace
{
std::vector<std::string> stockNames()
{
	std::vector<std::string> names;
	std::ifstream in(std::string(MC2_TEST_FIXTURE_DIR) + "/stock_names.txt");
	std::string line;
	while (std::getline(in, line))
		if (!line.empty() && line[0] != '#')
			names.push_back(line);
	return names;
}
} // namespace

TEST_SUITE("mc2str")
{

TEST_CASE("from_c: null is empty, a pointer is copied")
{
	CHECK(mc2::str::from_c(nullptr).empty());
	CHECK(mc2::str::from_c("").empty());
	CHECK(mc2::str::from_c("abc") == "abc");
}

TEST_CASE("iequals folds ASCII letters only")
{
	using mc2::str::iequals;
	CHECK(iequals("Abc", "aBC"));
	CHECK(iequals("", ""));
	CHECK(iequals("mc2_01.fit", "MC2_01.FIT"));
	CHECK_FALSE(iequals("abc", "abcd"));
	CHECK_FALSE(iequals("abc", "abd"));
	CHECK_FALSE(iequals("a-b", "a_b"));
	CHECK_FALSE(iequals("\xE9", "\xC9"));                // bytes >= 0x80 are never folded
	CHECK(iequals("\xE9", "\xE9"));
	CHECK_FALSE(iequals(std::string_view("a\0b", 3), "a"));          // a view is the whole range, not up to a NUL
}

TEST_CASE("icompare orders by the folded bytes, a prefix first")
{
	using mc2::str::icompare;
	CHECK(icompare("abc", "ABD") < 0);
	CHECK(icompare("Zeta", "alpha") > 0);
	CHECK(icompare("alpha", "Zeta") < 0);
	CHECK(icompare("ab", "ABC") < 0);
	CHECK(icompare("", "a") < 0);
	CHECK(icompare("", "") == 0);
	CHECK(icompare("ABC", "abc") == 0);
}

TEST_CASE("ci_total_less gives case variants a fixed order")
{
	std::vector<std::string> names{"b", "A", "a", "B", "Zeta", "alpha"};
	std::ranges::sort(names, mc2::str::ci_total_less{});
	CHECK(names == std::vector<std::string>{"A", "a", "alpha", "B", "b", "Zeta"});
	CHECK_FALSE(mc2::str::ci_total_less{}("a", "a"));
	CHECK(mc2::str::ci_total_less{}("A", "a"));
	CHECK_FALSE(mc2::str::ci_total_less{}("a", "A"));
}

TEST_CASE("ascii_lower and ascii_upper leave every byte above 0x7F alone")
{
	CHECK(mc2::str::ascii_lower("aBc-1_X") == "abc-1_x");
	CHECK(mc2::str::ascii_upper("aBc-1_x") == "ABC-1_X");
	CHECK(mc2::str::ascii_upper("\xE9") == "\xE9");
	CHECK(mc2::str::ascii_lower("\xC9") == "\xC9");
	CHECK(mc2::str::ascii_upper("").empty());
	CHECK(mc2::str::ascii_upper(std::string_view("a\0b", 3)) == std::string("A\0B", 3));
}

TEST_CASE("corpus: iequals matches every stock identifier and its ASCII case variants")
{
	const std::vector<std::string> names = stockNames();
	REQUIRE(names.size() > 5000);
	for (const std::string& name : names)
	{
		CHECK(mc2::str::iequals(name, name));
		CHECK(mc2::str::iequals(name, mc2::str::ascii_upper(name)));
		CHECK(mc2::str::iequals(name, mc2::str::ascii_lower(name)));
	}
}

// ci_total_less orders the pilot and mech lists, so it must be a strict total order: irreflexive, antisymmetric, transitive, and two names are
// unordered in both directions only when they are the same bytes.
TEST_CASE("corpus: ci_total_less is a strict total order over the stock identifiers")
{
	const std::vector<std::string> all = stockNames();
	REQUIRE(all.size() > 5000);
	std::vector<std::string> sample;
	for (size_t i = 0; i < all.size(); i += 40)
	{
		sample.push_back(all[i]);
		sample.push_back(mc2::str::ascii_upper(all[i]));        // a case variant of every sampled name
	}
	const mc2::str::ci_total_less less;
	bool irreflexive = true, antisymmetric = true, total = true, transitive = true;
	for (const std::string& a : sample)
	{
		irreflexive = irreflexive && !less(a, a);
		for (const std::string& b : sample)
		{
			antisymmetric = antisymmetric && !(less(a, b) && less(b, a));
			total = total && (less(a, b) || less(b, a) || a == b);
		}
	}
	CHECK(irreflexive);
	CHECK(antisymmetric);
	CHECK(total);
	for (size_t i = 0; i < sample.size() && transitive; i += 3)
		for (const std::string& b : sample)
			if (less(sample[i], b))
				for (size_t k = 0; k < sample.size(); k += 5)
					if (less(b, sample[k]) && !less(sample[i], sample[k]))
						transitive = false;
	CHECK(transitive);
}

TEST_CASE("erase_first cuts only the first exact mission extension")
{
	const char* cases[] = {"m0101.fit", "m0101", "a.fit.fit", "A.FIT", "x.fitness", ".fit", "", "m.fi.fit", "fit"};
	const char* expected[] = {"m0101", "m0101", "a.fit", "A.FIT", "xness", "", "", "m.fi", "fit"};
	for (std::size_t i = 0; i < std::size(cases); ++i)
		CHECK(mc2::str::erase_first(cases[i], ".fit") == expected[i]);
	CHECK(mc2::str::erase_first("abc", "") == "abc");
}

TEST_CASE("file_stem is the name between the last path separator and the first dot")
{
	CHECK(mc2::str::file_stem("data\\objects\\madcat.csv") == "madcat");
	CHECK(mc2::str::file_stem("data/objects/madcat.csv") == "madcat");
	CHECK(mc2::str::file_stem("madcat.csv") == "madcat");
	CHECK(mc2::str::file_stem("madcat") == "madcat");
	CHECK(mc2::str::file_stem("a.b\\madcat.tar.gz") == "madcat");   // a dot in a directory does not cut the name
	CHECK(mc2::str::file_stem("data\\objects\\").empty());
	CHECK(mc2::str::file_stem("").empty());
	CHECK(mc2::str::file_stem("data\\objects\\.hidden").empty());     // the dot starts the extension: nothing is left of it
	CHECK(mc2::str::file_stem(".x").empty());
	CHECK(mc2::str::file_stem("data/objects\\madcat.csv") == "madcat");   // mixed separators: the last one of either kind
	CHECK(mc2::str::file_stem("data\\objects/madcat.csv") == "madcat");
}

}
