// tests/unit/test_paths_mod.cpp
//
// This test brings mclib/paths.cpp (path string globals)
// under test as a leaf TU, and exercises the mods/ resolution order --
// offline, game-free. The mod-index machinery
// (InitModSearchPaths / TryModOpen / g_modIndex) lives in file.cpp;
// paths.cpp only supplies the real engine
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
#ifdef _WIN32
#include <io.h>       // _dup / _dup2 / _close for captureStdout
#endif
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

// Capture the engine printf output of one call (the mod-cache hit/miss lines).
template <class Fn>
std::string captureStdout (Fn fn)
{
	std::fflush(stdout);
	std::FILE *sink = std::tmpfile();
	REQUIRE(sink != nullptr);
	const int saved = _dup(_fileno(stdout));
	REQUIRE(saved >= 0);
	REQUIRE(_dup2(_fileno(sink), _fileno(stdout)) == 0);
	fn();
	std::fflush(stdout);
	_dup2(saved, _fileno(stdout));
	_close(saved);
	std::rewind(sink);
	std::string text;
	char buf[512];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, sink)) > 0)
		text.append(buf, n);
	std::fclose(sink);
	return text;
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

// Env names held once so the test does not grow the LG20 literal ratchet.
static constexpr const char* kEnvActiveMod = "MC2_ACTIVE_MOD";
static constexpr const char* kEnvRebuildCache = "MC2_REBUILD_MOD_CACHE";
static constexpr const char* kEnvModDeps = "MC2_MOD_DEPS";

void setActiveMod (const char *modId)
{
	// _putenv_s so getenv() inside InitModSearchPaths sees it.
	if (modId) _putenv_s(kEnvActiveMod, modId);
	else       _putenv_s(kEnvActiveMod, "");
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
	_putenv_s(kEnvRebuildCache, "1");
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
	_putenv_s(kEnvRebuildCache, "");
}

TEST_CASE("mod search is gated: absolute paths and .. are never mod-resolved")
{
	_putenv_s(kEnvRebuildCache, "1");
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
	_putenv_s(kEnvRebuildCache, "");
}

// LG05 optional guards: ReadModJson (found: id/name/deps -> InitModSearchPaths
// mounts the declared dep; not found / no id: the folder name becomes the id),
// TryModOpen (found: an open handle; not found: no handle and every caller
// falls through), and LoadModIndexCache (found: the cached entries; malformed
// or missing: nullopt and a fresh walk). All values are scratch-tree content.

namespace
{
const char *kJsonModsRoot = "mvh_scratch_json_mods";
const char *kJsonFolder  = "jsonfolder";       // the mods/ directory name
const char *kJsonModId   = "jsontestmod";      // the id INSIDE mod.json
const char *kDepId       = "jsondep";

// A mod.json whose scalars are spelled with escapes/odd spacing: the reader
// must return the values themselves, not the raw JSON text.
const char *kModJson =
	"{ \"name\" : \"Json Test Mod\",\n"
	"  \"id\"  : \"jsontestmod\",\n"
	"  \"dependencies\" : [ \"jsondep\" ] }\n";

std::string jsonBase () { return std::string(kJsonModsRoot) + "/" + kJsonFolder; }
std::string depBase  () { return std::string(kJsonModsRoot) + "/" + kDepId; }

void removeJsonTree ()
{
	const std::string bases[] = { jsonBase(), depBase() };
	for (const std::string &b : bases)
	{
		remove((b + "/data/mvh_scratch_data/mod.txt").c_str());
		remove((b + "/data/mvh_scratch_data/dep.txt").c_str());
		remove((b + "/.modindex-cache").c_str());
		remove((b + "/mod.json").c_str());
		_rmdir((b + "/data/mvh_scratch_data").c_str());
		_rmdir((b + "/data").c_str());
		_rmdir(b.c_str());
	}
	_rmdir(kJsonModsRoot);
}

// Each mod layer needs its own file name: index keys are the paths under
// data/, and the first layer to claim a key wins.
const char *kDepRelPath  = "data/mvh_scratch_data/dep.txt";
const char *kModRelPath  = "data/mvh_scratch_data/mod.txt";
} // namespace

TEST_CASE("mod.json found: its id and declared dependency are what get mounted")
{
	removeJsonTree();
	_mkdir(kJsonModsRoot);
	_mkdir(jsonBase().c_str());
	_mkdir((jsonBase() + "/data").c_str());
	_mkdir((jsonBase() + "/data/mvh_scratch_data").c_str());
	writeFile(jsonBase() + "/mod.json", kModJson);
	writeFile(jsonBase() + "/" + kModRelPath, "FROM-JSON-MOD");

	_mkdir(depBase().c_str());
	_mkdir((depBase() + "/data").c_str());
	_mkdir((depBase() + "/data/mvh_scratch_data").c_str());
	writeFile(depBase() + "/" + kDepRelPath, "FROM-JSON-DEP");

	_putenv_s(kEnvModDeps, "");
	_putenv_s(kEnvRebuildCache, "1");
	setActiveMod(kJsonFolder);
	InitModSearchPaths(kJsonModsRoot);

	// Found: the active layer is named by the id INSIDE mod.json, not by the
	// mods/ folder the env variable names; only ReadModJson yields that.
	SUBCASE("the json id owns the active mod's file")
	{
		const char *owner = LookupModOwner(kModRelPath);
		REQUIRE(owner != nullptr);
		CHECK(std::string(owner) == kJsonModId);
	}
	// ...and its "dependencies" array reached the index as a mounted layer,
	// which only ReadModJson's deps result can cause (MC2_MOD_DEPS is empty).
	SUBCASE("the json dependencies array mounts the dep layer")
	{
		const char *owner = LookupModOwner(kDepRelPath);
		REQUIRE(owner != nullptr);
		CHECK(std::string(owner) == kDepId);
	}

	setActiveMod(nullptr);
	InitModSearchPaths(kJsonModsRoot);
	_putenv_s(kEnvModDeps, "");
	removeJsonTree();
	_putenv_s(kEnvRebuildCache, "");
}

TEST_CASE("mod.json not found or without an id: the folder name becomes the id")
{
	removeJsonTree();
	_mkdir(kJsonModsRoot);
	_mkdir(jsonBase().c_str());
	_mkdir((jsonBase() + "/data").c_str());
	_mkdir((jsonBase() + "/data/mvh_scratch_data").c_str());
	writeFile(jsonBase() + "/" + kModRelPath, "FROM-NAMELESS-MOD");

	_putenv_s(kEnvModDeps, "");
	_putenv_s(kEnvRebuildCache, "1");

	SUBCASE("no mod.json")
	{
		remove((jsonBase() + "/mod.json").c_str());
		setActiveMod(kJsonFolder);
		InitModSearchPaths(kJsonModsRoot);
		const char *owner = LookupModOwner(kModRelPath);
		REQUIRE(owner != nullptr);
		CHECK(std::string(owner) == kJsonFolder);
	}

	SUBCASE("mod.json with no id field")
	{
		writeFile(jsonBase() + "/mod.json", "{ ""name"" : ""No Id Here"" }" "\n");
		setActiveMod(kJsonFolder);
		InitModSearchPaths(kJsonModsRoot);
		const char *owner = LookupModOwner(kModRelPath);
		REQUIRE(owner != nullptr);
		CHECK(std::string(owner) == kJsonFolder);
	}

	setActiveMod(nullptr);
	InitModSearchPaths(kJsonModsRoot);
	_putenv_s(kEnvModDeps, "");
	removeJsonTree();
	_putenv_s(kEnvRebuildCache, "");
}

TEST_CASE("mod-index cache: a missing or malformed cache is a clean miss")
{
	removeJsonTree();
	_mkdir(kJsonModsRoot);
	_mkdir(jsonBase().c_str());
	_mkdir((jsonBase() + "/data").c_str());
	_mkdir((jsonBase() + "/data/mvh_scratch_data").c_str());
	writeFile(jsonBase() + "/" + kModRelPath, "FROM-UNCACHED-MOD");

	_putenv_s(kEnvModDeps, "");
	_putenv_s(kEnvRebuildCache, "1");
	setActiveMod(kJsonFolder);
	InitModSearchPaths(kJsonModsRoot);                 // miss: fresh walk writes the cache
	REQUIRE(LookupModOwner(kModRelPath) != nullptr);

	// Missing: the cache is deleted. A dot-named file is invisible to the tree
	// signature, so the next load differs only in the cache -- a pure miss.
	remove((jsonBase() + "/.modindex-cache").c_str());
	_putenv_s(kEnvRebuildCache, "");
	std::string out = captureStdout([] { InitModSearchPaths(kJsonModsRoot); });
	CHECK(out.find("[mod-cache] refresh") != std::string::npos);
	CHECK(LookupModOwner(kModRelPath) != nullptr);

	// Found: the cache the miss just wrote is read back on the next load.
	out = captureStdout([] { InitModSearchPaths(kJsonModsRoot); });
	CHECK(out.find("[mod-cache] hit") != std::string::npos);
	CHECK(LookupModOwner(kModRelPath) != nullptr);

	setActiveMod(nullptr);
	InitModSearchPaths(kJsonModsRoot);
	_putenv_s(kEnvModDeps, "");
	removeJsonTree();
	_putenv_s(kEnvRebuildCache, "");
}
TEST_CASE("mod-index cache: a cache whose count line is negative is a miss, not a partial index")
{
	removeJsonTree();
	_mkdir(kJsonModsRoot);
	_mkdir(jsonBase().c_str());
	_mkdir((jsonBase() + "/data").c_str());
	_mkdir((jsonBase() + "/data/mvh_scratch_data").c_str());
	writeFile(jsonBase() + "/" + kModRelPath, "FROM-BAD-CACHE-MOD");

	_putenv_s(kEnvModDeps, "");
	_putenv_s(kEnvRebuildCache, "1");
	setActiveMod(kJsonFolder);
	InitModSearchPaths(kJsonModsRoot);                 // miss: writes a valid cache
	REQUIRE(LookupModOwner(kModRelPath) != nullptr);

	// Keep the header, modid and REAL signature line (any change to the tree
	// would make this a signature mismatch, not a format error); only the
	// count line is negative. LoadModIndexCache returns nullopt for it, so the
	// load must fall back to a fresh walk -- never to an empty index.
	const std::string cachePath = jsonBase() + "/.modindex-cache";
	std::string lines;
	{
		FILE *fp = fopen(cachePath.c_str(), "rb");
		REQUIRE(fp != nullptr);
		char line[512];
		for (int i = 0; i < 3 && fgets(line, sizeof line, fp); ++i)
			lines += line;                             // header, modid, sig
		fclose(fp);
	}
	{
		FILE *fp = fopen(cachePath.c_str(), "wb");
		REQUIRE(fp != nullptr);
		fputs(lines.c_str(), fp);
		fputs("count -1\n", fp);
		fclose(fp);
	}
	_putenv_s(kEnvRebuildCache, "");
	const std::string out = captureStdout([] { InitModSearchPaths(kJsonModsRoot); });
	CHECK(out.find("[mod-cache] miss") != std::string::npos);
	// The signature matched, so this is the format-error path (the negative
	// count), not a stale-tree rebuild.
	CHECK(out.find("cache missing/stale-format") != std::string::npos);
	REQUIRE(LookupModOwner(kModRelPath) != nullptr);
	CHECK(std::string(LookupModOwner(kModRelPath)) == std::string(kJsonFolder));

	setActiveMod(nullptr);
	InitModSearchPaths(kJsonModsRoot);
	_putenv_s(kEnvModDeps, "");
	removeJsonTree();
	_putenv_s(kEnvRebuildCache, "");
}
} // TEST_SUITE("PathsAndMods")
