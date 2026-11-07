// tests/unit/test_prefs_init.cpp
// Deterministic preference init (code/prefs_init.h): every options.cfg x orgprefs.cfg presence combination, resulting bytes
// against the old behavior (sniffer top tier = hiprefs.cfg copied over both files; minprefs.cfg seeds a missing orgprefs.cfg only).
#include "doctest.h"
#include "prefs_init.h"

#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {
struct Dir {
	fs::path p;
	Dir() {
		p = fs::temp_directory_path() / "mc2_prefs_init_test";
		fs::remove_all(p);
		fs::create_directories(p);
		put("hiprefs.cfg", "HI\r\n[x]\n");
		put("minprefs.cfg", "MIN\n");
	}
	~Dir() { std::error_code ec; fs::remove_all(p, ec); }
	void put(const char* n, const std::string& s) { std::ofstream(p / n, std::ios::binary) << s; }
	bool has(const char* n) const { return fs::exists(p / n); }
	std::string get(const char* n) const {
		std::ifstream f(p / n, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(f), {});
	}
};
const std::string HI = "HI\r\n[x]\n";
}  // namespace

TEST_SUITE("PrefsInit") {

TEST_CASE("options and orgprefs both missing: both become the top tier") {
	Dir d;
	CHECK(mc2::prefs::initPreferences(d.p));
	CHECK(d.get("options.cfg") == HI);
	CHECK(d.get("orgprefs.cfg") == HI);
}

TEST_CASE("options missing, orgprefs present: both overwritten with the top tier like the old sniffer") {
	Dir d;
	d.put("orgprefs.cfg", "OLDORG\n");
	CHECK(mc2::prefs::initPreferences(d.p));
	CHECK(d.get("options.cfg") == HI);
	CHECK(d.get("orgprefs.cfg") == HI);
}

TEST_CASE("options present, orgprefs missing: options untouched, orgprefs seeded from minprefs") {
	Dir d;
	d.put("options.cfg", "USER\n");
	CHECK_FALSE(mc2::prefs::initPreferences(d.p));
	CHECK(d.get("options.cfg") == "USER\n");
	CHECK(d.get("orgprefs.cfg") == "MIN\n");
}

TEST_CASE("both present: nothing touched") {
	Dir d;
	d.put("options.cfg", "USER\n");
	d.put("orgprefs.cfg", "ORG\n");
	CHECK_FALSE(mc2::prefs::initPreferences(d.p));
	CHECK(d.get("options.cfg") == "USER\n");
	CHECK(d.get("orgprefs.cfg") == "ORG\n");
}

TEST_CASE("hiprefs.cfg absent: minprefs.cfg is the fallback") {
	Dir d;
	fs::remove(d.p / "hiprefs.cfg");
	CHECK(mc2::prefs::initPreferences(d.p));
	CHECK(d.get("options.cfg") == "MIN\n");
	CHECK(d.get("orgprefs.cfg") == "MIN\n");
}

}
