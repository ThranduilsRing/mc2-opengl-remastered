// The script's declared extent bounds native output, including empty arrays and paired outputs.
#include "doctest.h"
#include "abl_exec_shim.h"
#include "abl_script_events.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <string>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

class GameObject;
extern GameObject* CurObject;
extern bool s_ablArgGuard;
void execGetRelativePositionToObject(void);
void execGetRelativePositionToPoint(void);


namespace {

std::span<float>* queuedOutput = nullptr;

template<class Fn>
std::string capture(Fn fn) {
	std::fflush(stderr);
	auto* sink = std::tmpfile();
	REQUIRE(sink != nullptr);
#ifdef _WIN32
	const int saved = _dup(_fileno(stderr));
	REQUIRE(saved >= 0);
	REQUIRE(_dup2(_fileno(sink), _fileno(stderr)) == 0);
#else
	const int saved = dup(fileno(stderr));
	REQUIRE(saved >= 0);
	REQUIRE(dup2(fileno(sink), fileno(stderr)) >= 0);
#endif
	fn();
	std::fflush(stderr);
#ifdef _WIN32
	_dup2(saved, _fileno(stderr));
	_close(saved);
#else
	dup2(saved, fileno(stderr));
	close(saved);
#endif
	std::rewind(sink);
	std::string log;
	char chunk[256];
	while (const auto count = std::fread(chunk, 1, sizeof(chunk), sink))
		log.append(chunk, count);
	std::fclose(sink);
	return log;
}

struct OutputCall {
	std::span<float> output;
	explicit OutputCall(std::span<float> out) : output(out) { queuedOutput = &output; }
	~OutputCall() { queuedOutput = nullptr; abl_test::args().clear(); CurObject = nullptr; }
};

}

// Existing position tests explicitly model real[3]; new calls carry their declared extent.
std::span<float> ABLi_popRealBuf(void) {
	if (queuedOutput) return *queuedOutput;
	auto& queue = abl_test::args().realPtrs;
	auto* address = queue.front();
	queue.pop_front();
	return address ? std::span<float>{address, 3} : std::span<float>{};
}

TEST_SUITE("ABL out arrays") {

TEST_CASE("event budget reset permits a second log line after exhaustion") {
    abl::resetEventBudgetsForTest();
    const auto first = capture([] {
        for (int i = 0; i < 17; ++i)
            abl::reportTruncated("getobjects_truncated", 1, 1);
    });
    CHECK(std::count(first.begin(), first.end(), '\n') == 16);
    CHECK(capture([] { abl::reportTruncated("getobjects_truncated", 1, 1); }).empty());
    abl::resetEventBudgetsForTest();
    const auto second = capture([] { abl::reportTruncated("getobjects_truncated", 1, 1); });
    CHECK(second.find("[ABL v1] event=getobjects_truncated ") == 0);
    CHECK(std::count(second.begin(), second.end(), '\n') == 1);
    abl::resetEventBudgetsForTest();
}

TEST_CASE("integer output publishes the clamped count and one shared reporter line") {
	int storage[] = {71, 72, 73, 74};
	const int source[] = {1, 2, 3};
	long count = -1;
	const auto log = capture([&] {
		count = abl::copyIntArray({storage, 2}, source, "outarray_test_truncated");
	});
	CHECK(count == 2);
	CHECK(storage[0] == 1);
	CHECK(storage[1] == 2);
	CHECK(storage[2] == 73);
	CHECK(storage[3] == 74);
	CHECK(log.find("[ABL v1] event=outarray_test_truncated ") == 0);
	CHECK(log.find("capacity=2 dropped=1") != std::string::npos);
	CHECK(std::count(log.begin(), log.end(), '\n') == 1);
}

TEST_CASE("exact fit and empty results do not log or alter trailing elements") {
	int storage[] = {71, 72, 73};
	const int source[] = {1, 2};
	const auto log = capture([&] {
		CHECK(abl::copyIntArray({storage, 2}, source, "outarray_fit_truncated") == 2);
		CHECK(abl::copyIntArray({storage, 2}, {}, "outarray_fit_truncated") == 0);
	});
	CHECK(log.empty());
	CHECK(storage[2] == 73);
}

TEST_CASE("unknown and zero extents never dereference a destination") {
	const int source[] = {1, 2, 3};
	CHECK(abl::copyIntArray({}, source, "outarray_empty_truncated") == 0);
	float real = 99.0f;
	const float reals[] = {1.0f, 2.0f};
	CHECK(abl::copyRealArray({&real, 0}, reals, "outarray_real_empty_truncated") == 0);
	CHECK(real == 99.0f);
}

TEST_CASE("paired outputs share their smaller declared extent and a single clamp") {
	int items[] = {7, 8, 9, 10};
	int quantities[] = {11, 12, 13};
	const auto log = capture([&] {
		const auto count = abl::clampArrayCount(1000, 2, "outarray_pair_truncated");
		std::fill_n(items, count, -1);
		std::fill_n(quantities, count, -1);
		CHECK(count == 2);
	});
	CHECK(items[2] == 9);
	CHECK(quantities[2] == 13);
	CHECK(std::count(log.begin(), log.end(), '\n') == 1);
	CHECK(abl::clampArrayCount(-2, 3, "outarray_pair_truncated") == 0);
}

TEST_CASE("the real position binding respects a one-element script output") {
	float storage[] = {1.0f, 2.0f, 3.0f, 4.0f};
	OutputCall call({storage, 1});
	abl_test::args().integers = {-1, 0};
	abl_test::args().reals = {45.0f, 100.0f};
	CurObject = nullptr;
	s_ablArgGuard = false;
	const auto log = capture([] { execGetRelativePositionToObject(); });
	CHECK(storage[0] == 0.0f);
	CHECK(storage[1] == 2.0f);
	CHECK(storage[3] == 4.0f);
	CHECK(log.find("[ABL v1] event=getrelativepositiontoobject_truncated ") == 0);
	CHECK(std::count(log.begin(), log.end(), '\n') == 1);
	CHECK(abl_test::args().integers.empty());
	CHECK(abl_test::args().reals.empty());
}

TEST_CASE("the real point binding consumes its arguments and bounds its output") {
	float input[] = {1.0f, 2.0f, 3.0f};
	float storage[] = {4.0f, 5.0f, 6.0f};
	OutputCall call({storage, 2});
	abl_test::args().realPtrs = {input};
	abl_test::args().integers = {0};
	abl_test::args().reals = {0.0f, 0.0f};
	const auto log = capture([] { execGetRelativePositionToPoint(); });
	CHECK(storage[0] == 0.0f);
	CHECK(storage[1] == 0.0f);
	CHECK(storage[2] == 6.0f);
	CHECK(log.find("[ABL v1] event=getrelativepositiontopoint_truncated ") == 0);
	CHECK(abl_test::args().realPtrs.empty());
}

}
