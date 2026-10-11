// tests/unit/test_printf_corpus.cpp -- every printf format the engine converted to mc2::fmt::to_buf, checked against snprintf.
//
// The cases themselves are generated (printf_corpus_gen.inc, by tools/tidy/printf_corpus.py from printf_corpus.jsonl): one per distinct
// (printf format, std::format string) pair, fed an edge-value table per conversion at full size and truncated to 8 bytes.

#include "doctest.h"

#include "mc2fmt.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>

namespace {
constexpr long long kEdgeI32[] = {0, 1, -1, 7, 42, -42, 255, 65535, 1000000, INT_MAX, INT_MIN};
constexpr long long kEdgeU32[] = {0, 1, 255, 4096, 0xDEADBEEFll, 0xFFFFFFFFll, -1ll};
constexpr long long kEdgeI64[] = {0, 1, -1, 123456789012345ll, LLONG_MAX, LLONG_MIN};
constexpr unsigned long long kEdgeU64[] = {0, 1, 0xFFFFFFFFull, 0x100000000ull, ULLONG_MAX};
constexpr long long kEdgeCHR[] = {65, 97, 122, 32, 48, 126};
constexpr double kEdgeDBL[] = {0.0, -0.0, 1.0, -1.5, 3.14159265358979, 1e-7, 123456.789, 1e22, 0.5, 2.5,
                                std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
void* const kEdgePTR[] = {nullptr, reinterpret_cast<void*>(1), reinterpret_cast<void*>(static_cast<uintptr_t>(0x7ff612345678ull))};
const char* const kStrNonNull[] = {"", "a", "hello world", "{braces}", "100%", "0123456789012345678901234567890123456789"};
const char* const kStrNull[] = {"", "a", "hello world", "{braces}", nullptr, "0123456789012345678901234567890123456789"};
}  // namespace

TEST_SUITE("printf corpus")
{

TEST_CASE("the edge tables themselves have the shapes the generator assumes")
{
	CHECK(std::size(kEdgeI32) > 0);
	CHECK(std::size(kStrNull) == std::size(kStrNonNull));
}

// KNOWN DIVERGENCE (not a conversion bug): the NaN the CPU makes for 0/0 has its sign bit set. The MSVC CRT prints it as -nan(ind) (and a signalling
// NaN as nan(snan)); std::format prints -nan. No harness parser accepts either spelling, so the positive quiet NaN and +-inf are in the edge
// table and this case pins the one difference instead of excluding it silently.
TEST_CASE("known divergence: the sign-bit-set (indefinite) NaN prints -nan(ind) under printf and -nan under std::format")
{
	const double v = -std::numeric_limits<double>::quiet_NaN();
	char a[32], b[32];
	std::snprintf(a, sizeof a, "%f", v);
	mc2::fmt::to_buf(b, "{:f}", v);
	CHECK(std::string(a) == "-nan(ind)");
	CHECK(std::string(b) == "-nan");
}

// The comparison the generated cases make must be able to fail: a wrong conversion (%x written as {:d}) is caught.
TEST_CASE("the corpus comparison detects a wrong conversion" * doctest::should_fail())
{
	char a[32], b[32];
	std::snprintf(a, sizeof a, "%x", 255u);
	mc2::fmt::to_buf(b, "{:d}", 255u);
	CHECK(std::strcmp(a, b) == 0);
}

#include "printf_corpus_gen.inc"

}  // TEST_SUITE
