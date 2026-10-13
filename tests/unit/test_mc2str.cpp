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
	CHECK(orderDisagree <= 20);           // measured: 20 adjacent pairs
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

// The campaign code cut ".fit" out of a mission name with EString::Remove(EString): the FIRST occurrence, compared exactly.
TEST_CASE("erase_first agrees with EString::Remove(EString) on mission-name shapes")
{
	const char* cases[] = {"m0101.fit", "m0101", "a.fit.fit", "A.FIT", "x.fitness", ".fit", "", "m.fi.fit", "fit"};
	for (const char* text : cases)
	{
		EString old(text);
		old.Remove(EString(".fit"));
		CHECK(mc2::str::erase_first(text, ".fit") == std::string(old.Data() ? old.Data() : ""));
	}
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
