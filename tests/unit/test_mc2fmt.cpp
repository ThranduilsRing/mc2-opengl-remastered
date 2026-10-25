// tests/unit/test_mc2fmt.cpp -- mc2fmt.h: snprintf-compatible to_buf / to_buf_n and the printf-argument helpers.
// Each case pins a behaviour the mechanical printf -> std::format conversion relies on.

#include "doctest.h"

#include "mc2fmt.h"
#include "msvc_printf_reference.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

TEST_SUITE("mc2fmt")
{

TEST_CASE("to_buf matches snprintf for a fitting result")
{
	char a[32], b[32];
	const int na = std::snprintf(a, sizeof a, "%02d:%02d|%s", 7, 5, "ab");
	const int nb = mc2::fmt::to_buf(b, "{:02d}:{:02d}|{}", 7, 5, "ab");
	CHECK(na == nb);
	CHECK(std::strcmp(a, b) == 0);
}

TEST_CASE("truncation: cap-1 characters, NUL terminated, would-be length returned")
{
	char a[8], b[8];
	const int na = std::snprintf(a, sizeof a, "%s", "0123456789");
	const int nb = mc2::fmt::to_buf(b, "{}", "0123456789");
	CHECK(na == 10);
	CHECK(nb == 10);
	CHECK(std::strcmp(a, "0123456") == 0);
	CHECK(std::strcmp(b, "0123456") == 0);
}

TEST_CASE("exact fit and one past")
{
	char b[6];
	CHECK(mc2::fmt::to_buf(b, "{}", "abcde") == 5);
	CHECK(std::strcmp(b, "abcde") == 0);
	CHECK(mc2::fmt::to_buf(b, "{}", "abcdef") == 6);
	CHECK(std::strcmp(b, "abcde") == 0);
}

TEST_CASE("cap 1 writes only the NUL; cap 0 writes nothing and still returns the length")
{
	char b[4] = {'x', 'x', 'x', 'x'};
	CHECK(mc2::fmt::to_buf_n(b, 1, "{}", "abc") == 3);
	CHECK(b[0] == '\0');
	CHECK(b[1] == 'x');
	char c[2] = {'y', 'y'};
	CHECK(mc2::fmt::to_buf_n(c, 0, "{}", "abc") == 3);
	CHECK(c[0] == 'y');
}

TEST_CASE("the bound is the array extent: sprintf into a small array cannot overrun")
{
	struct S { char small[4]; char guard[4]; } s = {{}, {'G', 'G', 'G', 'G'}};
	mc2::fmt::to_buf(s.small, "{}", "this is far too long");
	CHECK(std::memcmp(s.guard, "GGGG", 4) == 0);
	CHECK(std::strcmp(s.small, "thi") == 0);
}

TEST_CASE("safe() prints (null) where printf does, and leaves real strings alone")
{
	char a[16], b[16];
	std::snprintf(a, sizeof a, "%s", static_cast<const char*>(nullptr));
	mc2::fmt::to_buf(b, "{}", mc2::fmt::safe(nullptr));
	CHECK(std::strcmp(a, b) == 0);
	CHECK(std::strcmp(b, "(null)") == 0);
	std::string s("text");
	mc2::fmt::to_buf(b, "{}", mc2::fmt::safe(s.c_str()));
	CHECK(std::strcmp(b, "text") == 0);
}

TEST_CASE("%u of a negative int reinterprets, as printf does")
{
	char a[24], b[24];
	const int v = -1;
	std::snprintf(a, sizeof a, "%u|%x|%X|%o", v, v, v, v);
	mc2::fmt::to_buf(b, "{:d}|{:x}|{:X}|{:o}", mc2::fmt::as_u32(v), mc2::fmt::as_u32(v), mc2::fmt::as_u32(v), mc2::fmt::as_u32(v));
	CHECK(std::strcmp(a, b) == 0);
}

TEST_CASE("%ld is 32-bit on Windows, %lld and %zu are 64-bit")
{
	char a[64], b[64];
	const long l = -123456;
	const long long ll = 123456789012345LL;
	const size_t z = 42;
	std::snprintf(a, sizeof a, "%ld|%lld|%zu", l, ll, z);
	mc2::fmt::to_buf(b, "{:d}|{:d}|{:d}", mc2::fmt::as_i32(l), mc2::fmt::as_i64(ll), mc2::fmt::as_size(z));
	CHECK(std::strcmp(a, b) == 0);
}

TEST_CASE("%p is 16 upper-case hex digits without 0x on the MSVC CRT")
{
	char a[40], b[40];
	int x = 0;
	void* p = &x;
	corpusSnprintf(a, sizeof a, "%p", p);
	mc2::fmt::to_buf(b, "{:016X}", mc2::fmt::as_ptr(p));
	CHECK(std::strcmp(a, b) == 0);
	corpusSnprintf(a, sizeof a, "%p", static_cast<void*>(nullptr));
	mc2::fmt::to_buf(b, "{:016X}", mc2::fmt::as_ptr(static_cast<void*>(nullptr)));
	CHECK(std::strcmp(a, b) == 0);
}

TEST_CASE("%c prints the low byte")
{
	char a[8], b[8];
	std::snprintf(a, sizeof a, "[%c]", 65);
	mc2::fmt::to_buf(b, "[{:c}]", mc2::fmt::as_char(65));
	CHECK(std::strcmp(a, b) == 0);
}

TEST_CASE("float conversions agree with printf for the engine's usual specs")
{
	const double vals[] = {0.0, -0.0, 1.0, -2.5, 3.14159265358979, 1e-7, 123456789.125, 1e22};
	for (double v : vals) {
		char a[64], b[64];
		std::snprintf(a, sizeof a, "%f|%.2f|%8.3f|%e|%g", v, v, v, v, v);
		mc2::fmt::to_buf(b, "{:f}|{:.2f}|{:8.3f}|{:e}|{:g}", v, v, v, v, v);
		INFO("value " << v);
		CHECK(std::strcmp(a, b) == 0);
	}
}

TEST_CASE("print writes exactly what fprintf wrote, to the stream it was given")
{
	std::FILE* a = std::tmpfile();
	std::FILE* b = std::tmpfile();
	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);
	std::fprintf(a, "[%s] %d|%5.2f|%-6s|\n", "x", 42, 3.14159, "ab");
	mc2::fmt::print(b, "[{}] {:d}|{:5.2f}|{:<6}|\n", "x", mc2::fmt::as_i32(42), 3.14159, "ab");
	char ta[128] = {}, tb[128] = {};
	std::rewind(a);
	std::rewind(b);
	const size_t na = std::fread(ta, 1, sizeof ta - 1, a);
	const size_t nb = std::fread(tb, 1, sizeof tb - 1, b);
	CHECK(na == nb);
	CHECK(std::strcmp(ta, tb) == 0);
	std::fclose(a);
	std::fclose(b);
}

TEST_CASE("print is one write of arbitrary length and leaves an empty format alone")
{
	std::FILE* f = std::tmpfile();
	REQUIRE(f != nullptr);
	const std::string big(5000, 'z');
	mc2::fmt::print(f, "{}", big.c_str());
	mc2::fmt::print(f, "");
	mc2::fmt::print(f, "{}", "tail");
	std::rewind(f);
	std::string got(5100, '\0');
	const size_t n = std::fread(got.data(), 1, got.size(), f);
	CHECK(n == 5004);
	CHECK(got.compare(0, 5000, big) == 0);
	CHECK(got.compare(5000, 4, "tail") == 0);
	std::fclose(f);
}

namespace {
// Runs after the main thread's thread_local objects are destroyed. A wrong byte, or a crash, ends the test binary with status 3 / a fault.
void printAtExit()
{
	char got[32] = {};
	if (std::FILE* f = std::tmpfile()) {
		mc2::fmt::print(f, "exit:{:d}", mc2::fmt::as_i32(7));
		std::rewind(f);
		std::fread(got, 1, sizeof got - 1, f);
		std::fclose(f);
	}
	if (std::strcmp(got, "exit:7") != 0)
		std::_Exit(3);
}
}  // namespace

TEST_CASE("print works from an atexit handler: its buffer outlives the thread_local destructors")
{
	std::FILE* f = std::tmpfile();
	REQUIRE(f != nullptr);
	mc2::fmt::print(f, "{}", "warm the main thread's buffer");
	std::fclose(f);
	CHECK(std::atexit(printAtExit) == 0);
}

TEST_CASE("print returns the length printf would, and a negative value when the write fails")
{
	std::FILE* f = std::tmpfile();
	REQUIRE(f != nullptr);
	CHECK(mc2::fmt::print(f, "{:d}|{}", mc2::fmt::as_i32(42), "ab") == 5);
	CHECK(mc2::fmt::print(f, "") == 0);
	std::fclose(f);
	std::FILE* ro = std::fopen("tests/unit/CMakeLists.txt", "rb");
	if (ro) {
		CHECK(mc2::fmt::print(ro, "x") < 0);
		std::fclose(ro);
	}
}

TEST_CASE("debug_out accepts null and empty text")
{
	mc2::fmt::debug_out(nullptr);
	mc2::fmt::debug_out("");
	CHECK(true);
}

}  // TEST_SUITE
