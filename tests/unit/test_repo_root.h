// Runtime checkout-root resolution for mc2_tests (force-included by tests/unit/CMakeLists.txt).
//
// Why not a compile definition: an absolute worktree path baked into an object
// file via -DMC2_REPO_ROOT="A:/Games/mc2-laneN" is not part of the shared
// compile-cache key, so a cache hit from a deleted worktree replays a dead path
// (W114: test_mc2str.cpp.obj carried A:/Games/mc2-lane2). Resolve at runtime
// instead: walk up from the test exe, then from the cwd, to the first directory
// containing tests/unit/fixtures.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#if defined(_WIN32)
#include <stdlib.h>
#endif

namespace mc2_test
{
inline bool isRoot(const std::filesystem::path &p)
{
	std::error_code ec;
	return std::filesystem::is_directory(p / "tests" / "unit" / "fixtures", ec);
}

inline std::filesystem::path walkUp(std::filesystem::path p)
{
	std::error_code ec;
	p = std::filesystem::absolute(p, ec);
	for (; !p.empty(); p = p.parent_path())
	{
		if (isRoot(p))
			return p;
		if (p == p.parent_path())
			break;
	}
	return {};
}

inline const std::string &repoRoot()
{
	static const std::string root = []() -> std::string {
		std::filesystem::path exe;
#if defined(_WIN32)
		char *pgm = nullptr;
		if (_get_pgmptr(&pgm) == 0 && pgm)
			exe = pgm;
#else
		std::error_code ec;
		exe = std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
		std::filesystem::path r;
		if (!exe.empty())
			r = walkUp(exe.parent_path());
		if (r.empty())
			r = walkUp(std::filesystem::current_path());
		return r.empty() ? std::string(".") : r.generic_string();
	}();
	return root;
}

inline std::string fixtureDir() { return repoRoot() + "/tests/unit/fixtures"; }
} // namespace mc2_test

#define MC2_REPO_ROOT (::mc2_test::repoRoot())
#define MC2_TEST_FIXTURE_DIR (::mc2_test::fixtureDir())
