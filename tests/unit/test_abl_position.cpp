// tests/unit/test_abl_position.cpp
//
// The real getRelativePositionToObject binding (code/ablmc2_position.cpp) against a scripted argument queue: a script's real[3] output
// is defined whatever object it names, and a script that passes no output variable is rejected with a log line instead of faulting.

#include "doctest.h"

#include "abl_exec_shim.h"
#include "ablmc2_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

// Runs fn with the process stderr redirected into a temporary file and returns what it wrote.
template <class Fn>
std::string captureStderr(Fn fn) {
	std::fflush(stderr);
	std::FILE* sink = std::tmpfile();
	REQUIRE(sink != nullptr);
#ifdef _WIN32
	const int saved = _dup(_fileno(stderr));
	_dup2(_fileno(sink), _fileno(stderr));
#else
	const int saved = dup(fileno(stderr));
	dup2(fileno(sink), fileno(stderr));
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
	std::string text;
	std::rewind(sink);
	char chunk[256];
	while (const size_t n = std::fread(chunk, 1, sizeof chunk, sink))
		text.append(chunk, n);
	std::fclose(sink);
	return text;
}

// Queues the arguments of one getRelativePositionToObject call: object id, angle, distance, flags, output.
void queueCall(float* out) {
	auto& a = abl_test::args();
	a.integers = {-1, 0};  // -1 names the current object
	a.reals = {45.0f, 100.0f};
	a.realPtrs = {out};
}

// A GameObject of the given class without running its constructor: the binding reads only objectClass (through the non-virtual isMover).
// A real GameObject needs the whole object-pool runtime to construct.
struct FakeObject {
	alignas(GameObject) unsigned char storage[sizeof(GameObject)] = {};
	explicit FakeObject(ObjectClass c) { reinterpret_cast<GameObject*>(storage)->objectClass = c; }
	GameObject* get() { return reinterpret_cast<GameObject*>(storage); }
};

}  // namespace

TEST_SUITE("ABL position builtins")
{

TEST_CASE("getRelativePositionToObject on an object that is not a mover writes the zero vector over the script's value")
{
	FakeObject building(BUILDING);
	float pos[3] = {-2.5e18f, 7.0f, 1.0e9f};
	abl_test::args().clear();
	queueCall(pos);
	CurObject = building.get();
	execGetRelativePositionToObject();
	CHECK(pos[0] == 0.0f);
	CHECK(pos[1] == 0.0f);
	CHECK(pos[2] == 0.0f);
	CHECK(abl_test::args().realPtrs.empty());
	abl_test::args().clear();
}

TEST_CASE("getRelativePositionToObject on an unknown id writes the zero vector")
{
	float pos[3] = {5.0f, 6.0f, 7.0f};
	abl_test::args().clear();
	queueCall(pos);
	CurObject = nullptr;
	execGetRelativePositionToObject();
	CHECK(pos[0] == 0.0f);
	CHECK(pos[1] == 0.0f);
	CHECK(pos[2] == 0.0f);
	abl_test::args().clear();
}

TEST_CASE("getRelativePositionToObject with no output variable logs one bad_argument line and returns, even with the argument guard off")
{
	FakeObject building(BUILDING);
	s_ablArgGuard = false;
	abl_test::args().clear();
	queueCall(nullptr);
	CurObject = building.get();
	const std::string log = captureStderr([] { execGetRelativePositionToObject(); });
	CHECK(log.find("[ABL v1] event=bad_argument builtin=getRelativePositionToObject argument=relPos") == 0);
	CHECK(std::count(log.begin(), log.end(), '\n') == 1);
	CHECK(abl_test::args().integers.empty());  // every argument was still consumed: the interpreter stack stays balanced
	abl_test::args().clear();
}

}
