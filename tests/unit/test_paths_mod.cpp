// tests/unit/test_paths_mod.cpp
//
// MINIMAL-VIABLE-HARNESS-2 (TU-3): brings mclib/paths.cpp (path string globals)
// under test as a leaf TU, and exercises the mods/ resolution order that the
// moddability arc wants proven -- offline, game-free. The mod-index machinery
// (InitModSearchPaths / TryModOpen / g_modIndex) lives in file.cpp, already
// linked by MINIMAL-VIABLE-HARNESS-1; paths.cpp only supplies the real engine
// path globals so tests reference them instead of stand-ins.
// Pattern doc: docs/testing/minimal-viable-harness.md.
//
// THE mods/ CONVENTION proven here (file.cpp::InitModSearchPaths):
//   layout   <modsRoot>/<modId>/data/<subpath>  (e.g. .../testmod/data/objects/x)
//   activate MC2_ACTIVE_MOD=<modId>
//   key      NormalizeKey("data/objects/x")  (lowercase, forward slash)
//   priority active mod is consulted FIRST in File::open, so a mod override
//            wins over BOTH loose disk AND FST. Stock mode (env unset) => the
//            index is empty and resolution is byte-identical to base game.
//   gating   ShouldSearchMods requires a relative path under data/; absolute
//            paths and any path containing ".." are never mod-resolved.
//
// NOTE: this test mutates process env (MC2_ACTIVE_MOD) and global mod state.
// It restores the env and clears the index (InitModSearchPaths("") in stock
// mode) at the end so it does not perturb other suites in the same binary.

#include "doctest.h"

#include "file.h"
#include "paths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <direct.h>   // _mkdir / _rmdir
#include <windows.h>  // (paths.cpp uses PATH_SEPARATOR; nothing needed at runtime here)

namespace
{

// Scratch mods/ tree with an override file. The query key lives under a
// scratch subdir of data/ (data/mvh_scratch_data/...) so it satisfies the
// ShouldSearchMods "must start with data/" gate WITHOUT ever touching the
// repo's real data/ content -- and every file + dir it creates is removed at
// the end of each case.
//   mods root : <cwd>/mvh_scratch_mods
//   override  : <cwd>/mvh_scratch_mods/testmod/data/mvh_scratch_data/entry.txt
//   loose     : <cwd>/data/mvh_scratch_data/entry.txt
const char *kModsRoot     = "mvh_scratch_mods";
const char *kModId        = "testmod";
const char *kRelDataPath  = "data/mvh_scratch_data/entry.txt";  // query key
const char *kModPayload   = "FROM-ACTIVE-MOD";
const char *kLoosePayload = "from-loose-base-data";

void writeFile (const std::string &path, const char *data)
{
	FILE *fp = fopen(path.c_str(), "wb");
	REQUIRE(fp != nullptr);
	fwrite(data, 1, strlen(data), fp);
	fclose(fp);
}

std::string readAll (File &f)
{
	const long len = (long)f.getLength();
	std::string out;
	out.resize(len > 0 ? (size_t)len : 0u);
	if (len > 0) { f.seek(0); f.read((MemoryPtr)&out[0], len); }
	return out;
}

// Create the scratch mod override tree on disk.
void buildModTree ()
{
	const std::string base = std::string(kModsRoot) + "/" + kModId;
	_mkdir(kModsRoot);
	_mkdir(base.c_str());
	_mkdir((base + "/data").c_str());
	_mkdir((base + "/data/mvh_scratch_data").c_str());
	writeFile(base + "/" + kRelDataPath, kModPayload);
}

// Fully remove every file + dir the scratch mod tree created (deepest first).
void removeModTree ()
{
	const std::string base = std::string(kModsRoot) + "/" + kModId;
	remove((base + "/" + kRelDataPath).c_str());
	_rmdir((base + "/data/mvh_scratch_data").c_str());
	_rmdir((base + "/data").c_str());
	// mod cache file the indexer writes (.modindex-cache) -- delete BEFORE the
	// dir rmdir, or the non-empty rmdir fails and leaves the tree behind.
	remove((base + "/.modindex-cache").c_str());
	_rmdir(base.c_str());
	_rmdir(kModsRoot);
}

// Create / remove the scratch loose override tree under the real data/ dir.
void buildLooseTree ()
{
	_mkdir("data");
	_mkdir("data/mvh_scratch_data");
	writeFile(kRelDataPath, kLoosePayload);
}

void removeLooseTree ()
{
	remove(kRelDataPath);
	_rmdir("data/mvh_scratch_data");
	// leave data/ itself alone -- it may be the repo's real data dir.
}

void setActiveMod (const char *modId)
{
	// _putenv_s so getenv() inside InitModSearchPaths sees it.
	if (modId) _putenv_s("MC2_ACTIVE_MOD", modId);
	else       _putenv_s("MC2_ACTIVE_MOD", "");
}

} // namespace

TEST_SUITE("PathsAndMods")
{

TEST_CASE("paths.cpp exposes the engine's real path string globals")
{
	// Leaf-TU sanity: the globals link and carry their default data/ layout.
	// PATH_SEPARATOR is '/' here (LINUX_BUILD is defined for this target,
	// matching the main build).
	CHECK(strncmp(objectPath, "data/objects/", 13) == 0);
	CHECK(strncmp(artPath,    "data/art/",     9) == 0);
	CHECK(strncmp(missionPath,"data/missions/",14) == 0);
}

TEST_CASE("active mod override wins over loose base data (mods/ resolution order)")
{
	// Force a fresh index each run so a stale .modindex-cache can't leak.
	_putenv_s("MC2_REBUILD_MOD_CACHE", "1");
	buildModTree();

	// A loose base-data copy at the query path -- the mod must still win.
	buildLooseTree();

	setActiveMod(kModId);
	InitModSearchPaths(kModsRoot);

	SUBCASE("File::open serves the mod copy, not the loose copy")
	{
		File f;
		REQUIRE(f.open(kRelDataPath) == NO_ERR);
		CHECK(readAll(f) == kModPayload);
		f.close();
	}

	SUBCASE("LookupModOwner attributes the key to the active mod")
	{
		const char *owner = LookupModOwner(kRelDataPath);
		REQUIRE(owner != nullptr);
		CHECK(strcmp(owner, kModId) == 0);
	}

	// --- Deactivate: stock mode empties the index; loose copy resolves. ---
	setActiveMod(nullptr);
	InitModSearchPaths(kModsRoot);

	SUBCASE("stock mode falls back to loose base data")
	{
		CHECK(LookupModOwner(kRelDataPath) == nullptr);
		File f;
		REQUIRE(f.open(kRelDataPath) == NO_ERR);
		CHECK(readAll(f) == kLoosePayload);
		f.close();
	}

	// cleanup
	removeLooseTree();
	removeModTree();
	_putenv_s("MC2_REBUILD_MOD_CACHE", "");
}

TEST_CASE("mod search is gated: absolute paths and .. are never mod-resolved")
{
	_putenv_s("MC2_REBUILD_MOD_CACHE", "1");
	buildModTree();
	setActiveMod(kModId);
	InitModSearchPaths(kModsRoot);

	// Relative under data/ IS mod-resolved (control).
	CHECK(LookupModOwner(kRelDataPath) != nullptr);

	// A traversal attempt is rejected by ShouldSearchMods (contains "..").
	CHECK(LookupModOwner("data/../data/mvh_scratch_data/entry.txt") == nullptr);

	// A path that does not start under data/ is never mod-resolved.
	CHECK(LookupModOwner("mvh_scratch_data/entry.txt") == nullptr);

	// Restore stock mode so later suites see a clean, empty index.
	setActiveMod(nullptr);
	InitModSearchPaths(kModsRoot);
	removeModTree();
	_putenv_s("MC2_REBUILD_MOD_CACHE", "");
}

} // TEST_SUITE("PathsAndMods")
