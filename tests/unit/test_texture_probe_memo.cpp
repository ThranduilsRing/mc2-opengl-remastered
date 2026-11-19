// HITCH-ATTRIB-2: destroy-time texture probes (mech fall shapes, tree damage shapes) must hit a memo primed at load.
#include "texture_probe_memo.h"
#include "doctest.h"
#include <functional>
#include <string>
#include <vector>

namespace {
struct FakeVfs {
	unsigned rawProbes = 0;
	bool operator()(const char* p) { ++rawProbes; return std::string(p).find("missing") == std::string::npos; }
};
// One type = the texture paths its destroy-time swap probes.
using Type = std::vector<std::string>;
}

TEST_CASE("texture_probe_memo: after the load-time prime, destroying every type performs zero uncached probes") {
	mc2::TextureProbeMemo memo;
	FakeVfs vfs;
	const std::vector<Type> mechs = {{"tgl/256/a_mech.tga", "tgl/256/missing_a.tga"}, {"tgl/256/b_mech.tga"}};
	const std::vector<Type> trees = {{"tgl/256/tree_dmg.tga", "tgl/256/missing_t.tga"}};
	// load: prime (production calls the memo from Mech3DAppearance::init / TreeAppearanceType::init)
	for (const auto* group : {&mechs, &trees})
		for (const Type& t : *group)
			for (const std::string& p : t) memo.resolve(p.c_str(), 1, std::ref(vfs));
	const uint64_t uncachedAfterLoad = memo.uncachedCount();
	const unsigned rawAfterLoad = vfs.rawProbes;
	CHECK(uncachedAfterLoad == 5);
	// destroy every type: the same queries the setObjStatus paths make
	for (const auto* group : {&mechs, &trees})
		for (const Type& t : *group)
			for (const std::string& p : t) (void)memo.resolve(p.c_str(), 1, std::ref(vfs));
	CHECK(memo.uncachedCount() - uncachedAfterLoad == 0);
	CHECK(vfs.rawProbes - rawAfterLoad == 0);
}

TEST_CASE("texture_probe_memo: unprimed destroy probes (the base behaviour the prime removes)") {
	mc2::TextureProbeMemo memo;
	FakeVfs vfs;
	CHECK_FALSE(memo.resolve("tgl/256/missing_a.tga", 1, std::ref(vfs)));
	CHECK(memo.uncachedCount() == 1);
	CHECK(vfs.rawProbes == 1);
}

TEST_CASE("texture_probe_memo: a new VFS generation drops remembered answers") {
	mc2::TextureProbeMemo memo;
	FakeVfs vfs;
	memo.resolve("tgl/256/a.tga", 1, std::ref(vfs));
	memo.resolve("tgl/256/a.tga", 2, std::ref(vfs));
	CHECK(vfs.rawProbes == 2);
}
