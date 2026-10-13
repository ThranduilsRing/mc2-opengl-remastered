// tests/unit/test_mc2str.cpp
//
// mc2::str (mclib/mc2str.h): the helpers that carry EString's null-is-empty and case-insensitive-name behaviour over to std::string
// (step 4 batch B1, .claude/cleanup/cxx23-step4-b1-notes.md section 3a). The old class is the oracle: the corpus test runs the stock
// identifiers through EString::Compare (the Windows lstrcmpi) and through the new helpers and lists where they disagree.

#include "doctest.h"

#include "estring.h"
#include "mc2str.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifndef MC2_TEST_FIXTURE_DIR
#error "MC2_TEST_FIXTURE_DIR must be defined by tests/unit/CMakeLists.txt"
#endif

namespace
{
int signOf(int v) { return (v > 0) - (v < 0); }

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

TEST_CASE("ci_less keys a container by name, ignoring case; ci_total_less gives case variants a fixed order")
{
	std::map<std::string, int, mc2::str::ci_less> byName;
	byName["Bushwacker"] = 1;
	byName["BUSHWACKER"] = 2;                           // the same key
	byName["hunchback"] = 3;
	CHECK(byName.size() == 2);
	CHECK(byName.find(std::string_view("bushwacker")) != byName.end());          // transparent lookup by view, no temporary string
	CHECK(byName.find(std::string_view("BuShWaCkEr"))->second == 2);

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

// The stock identifiers (tests/unit/fixtures/stock_names.txt) through the old compare and the new one. Equality must agree on every
// name and on its upper- and lower-cased variants. Order may differ: lstrcmpi is a locale word sort (punctuation weighted apart), the new
// order is plain byte order of the folded text; the disagreements are counted, and listed by a run with doctest's -s (success output).
TEST_CASE("corpus: iequals agrees with the old lstrcmpi-based equality on the stock identifiers")
{
	std::vector<std::string> names = stockNames();
	REQUIRE(names.size() > 5000);

	std::vector<std::string> sorted = names;
	std::ranges::sort(sorted, mc2::str::ci_total_less{});

	int equalMismatch = 0;
	int caseVariantMismatch = 0;
	for (const std::string& n : sorted)
	{
		const std::string up = mc2::str::ascii_upper(n);
		const std::string lo = mc2::str::ascii_lower(n);
		if (!(EString(n.c_str()) == up.c_str()) || !(EString(n.c_str()) == lo.c_str()) || !mc2::str::iequals(n, up) || !mc2::str::iequals(n, lo))
			++caseVariantMismatch;
	}

	// Adjacent names in the folded order: the old compare and the new one must agree on equal-or-not, and the order disagreements are counted.
	int orderDisagree = 0;
	for (size_t i = 1; i < sorted.size(); ++i)
	{
		const std::string& a = sorted[i - 1];
		const std::string& b = sorted[i];
		const int oldCmp = EString(a.c_str()).Compare(b.c_str());
		const bool oldEqual = oldCmp == 0;
		if (oldEqual != mc2::str::iequals(a, b))
		{
			++equalMismatch;
			MESSAGE("equality differs: '", a, "' '", b, "' old=", oldCmp);
		}
		else if (signOf(oldCmp) != signOf(mc2::str::icompare(a, b)))
		{
			++orderDisagree;
			MESSAGE("order differs: '", a, "' '", b, "' old=", signOf(oldCmp), " new=", signOf(mc2::str::icompare(a, b)));
		}
	}
	MESSAGE("names=", sorted.size(), " equalMismatch=", equalMismatch, " caseVariantMismatch=", caseVariantMismatch, " adjacentOrderDisagreements=", orderDisagree);

	CHECK(equalMismatch == 0);
	CHECK(caseVariantMismatch == 0);
	// The disagreements are not a failure (the old order was a locale word sort) but a jump means the fold changed.
	CHECK(orderDisagree < 100);
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
}

// The three mech-camera call sites (logisticsmechdisplay, mechbayscreen_mechs, salvagemecharea) cut a chassis file name with
// Find('.') / Left / ReverseFind(separator) / Right on an EString. For every stock name that has an extension the helper gives the same text.
TEST_CASE("corpus: file_stem agrees with the EString Find / Left / ReverseFind / Right chain on the stock file names")
{
	int compared = 0;
	int differ = 0;
	for (const std::string& name : stockNames())
	{
		const std::string path = "data\\objects\\" + name;
		EString fileName(path.c_str());
		int index = fileName.Find('.');
		if (index < 0)
			continue;
		fileName = fileName.Left(index);
		index = fileName.ReverseFind('\\');
		fileName = fileName.Right(fileName.Length() - index - 1);
		++compared;
		if (std::string(static_cast<const char*>(fileName)) != mc2::str::file_stem(path))
		{
			++differ;
			MESSAGE("stem differs: '", path, "'");
		}
	}
	CHECK(compared > 3000);
	CHECK(differ == 0);
}

}
