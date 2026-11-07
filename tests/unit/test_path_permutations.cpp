// tests/unit/test_path_permutations.cpp
//
// META-FIX design E (file-path fragility, issue #8 family): the permutation
// matrix locking the CANONICAL-KEY contract of the layered resolution ladder
// (mod > loose > FST). Every spelling of the same logical path — any
// separator x any case — must resolve to the SAME layer and the SAME bytes
// through every ladder API, and fileExists()'s tier must agree with
// File::open()'s outcome (0 <=> open fails, 1 <=> mod/loose, 2 <=> FST).
//
// RED-BEFORE/GREEN-AFTER GATE (batch-1 proof recipe step 2):
//   * FST-only x backslash x fileExists  — CONFIRMED-live bug pre-fix:
//     fileExists (mclib/file.cpp) passed the RAW name to FastFileFind while
//     File::open normalized first, so "data\\x\\y.fit" opened fine but
//     fileExists said 0. This suite FAILS against pre-fix file.cpp and
//     passes after the canonical-key chokepoint fix.
//   * FST-only x non-canonical x FastFileFind-direct — same class, callee
//     side (fastfile.cpp hashed the raw caller string).
//   * FitIniFile::open x FST-only x backslash — the direct regression lock
//     for historical issue #8 (would have failed pre-fb4d2e52).
//
// Fixture conventions copied from test_fst_roundtrip.cpp (FST authoring via
// create/writeFast) and test_paths_mod.cpp (scratch mod tree + env hygiene).
//
// KNOWN OUT-OF-SCOPE ASYMMETRY (documented, not asserted): File::open's
// numeric size-subdir strip ladder ("data/tgl/128/x.tga" -> "data/tgl/x.tga",
// file.cpp base-strip) has NO fileExists sibling, so a strip-resolved path
// opens fine while fileExists reports 0. Batch 1 deliberately fixes only the
// canonical-KEY class (case/separator); the strip asymmetry is a recorded
// follow-up candidate, so the exists<=>open assertion is scoped to non-strip
// cells here.

#include "doctest.h"
#include "Gates/gate_name.h"

#include "file.h"
#include "inifile.h"
#include "packet.h"
#include "ffile.h"
#include "fastfile.h"
#include "vfs.h"
#include "../../code/save_select.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>   // _mkdir / _rmdir
#else
#include "platform_io.h"
#endif

namespace
{

// Canonical logical path (lowercase, forward slash — the FST/loose key form).
const char kCanonical[] = "data/mvh_perm_data/entry.fit";

// Every case x separator spelling of the SAME logical path.
const char* kSpellings[] = {
	"data/mvh_perm_data/entry.fit",         // canonical
	"data\\mvh_perm_data\\entry.fit",       // backslash (stock .fit data bakes these)
	"DATA/MVH_PERM_DATA/ENTRY.FIT",         // uppercase
	"DATA\\MVH_PERM_DATA\\ENTRY.FIT",       // uppercase + backslash
	"Data/Mvh_Perm_Data/Entry.Fit",         // mixed case
	"Data\\Mvh_Perm_Data\\Entry.Fit",       // mixed case + backslash
	"data\\mvh_perm_data/entry.fit",        // mixed separators
	"Data/mvh_perm_data\\Entry.fit",        // mixed separators + mixed case
};
const int kNumSpellings = int(sizeof(kSpellings) / sizeof(kSpellings[0]));

// Distinct per-layer payloads: valid FIT text whose LayerId identifies the
// serving layer through FitIniFile, and whose raw bytes identify it through
// File. (1 = mod, 2 = loose, 3 = FST.)
std::string fitPayload (int layerId)
{
	char buf[256];
	snprintf(buf, sizeof(buf),
		"FITini\r\n\r\n[PermBlock]\r\nl LayerId = %d\r\n\r\nFITend\r\n", layerId);
	return std::string(buf);
}

std::string scratchPath (const char* name)
{
	return std::string("mvh_perm_") + name;
}

void writeRaw (const std::string& path, const std::string& data)
{
	FILE* fp = fopen(path.c_str(), "wb");
	REQUIRE(fp != nullptr);
	fwrite(data.data(), 1, data.size(), fp);
	fclose(fp);
}

std::string readAll (File& f)
{
	const long len = (long)f.getLength();
	std::string out;
	out.resize(len > 0 ? (size_t)len : 0u);
	if (len > 0) { f.seek(0); f.read((MemoryPtr)&out[0], len); }
	return out;
}

// ---- loose layer -----------------------------------------------------------
void buildLoose (const std::string& payload)
{
	_mkdir("data");
	_mkdir("data/mvh_perm_data");
	writeRaw(kCanonical, payload);
}

void removeLoose ()
{
	remove(kCanonical);
	_rmdir("data/mvh_perm_data");
	// leave data/ alone — may be the repo's real data dir.
}

// ---- FST layer (test_fst_roundtrip.cpp pattern) ----------------------------
struct FstRegistry
{
	explicit FstRegistry (const char* fstPath)
	{
		maxFastFiles = 4;
		numFastFiles = 0;
		fastFiles = (FastFile**)malloc(sizeof(FastFile*) * maxFastFiles);
		for (long i = 0; i < maxFastFiles; ++i) fastFiles[i] = nullptr;
		ok = FastFileInit(fstPath);
	}
	~FstRegistry () { FastFileFini(); }
	bool ok = false;
};

void authorFst (const std::string& fstPath, const char* entryName,
                const std::string& payload)
{
	FastFile ff;
	REQUIRE(ff.create(fstPath.c_str(), /*compressed=*/true) == NO_ERR);
	REQUIRE(ff.reserve(1) == NO_ERR);
	std::string buf = payload;
	REQUIRE(ff.writeFast(entryName, (void*)buf.data(), (int)buf.size()) == NO_ERR);
	ff.close();
}

// ---- mod layer (test_paths_mod.cpp pattern) --------------------------------
const char* kModsRoot = "mvh_perm_mods";
const char* kModId    = "permmod";

void buildModTree (const std::string& payload)
{
	const std::string base = std::string(kModsRoot) + "/" + kModId;
	_mkdir(kModsRoot);
	_mkdir(base.c_str());
	_mkdir((base + "/data").c_str());
	_mkdir((base + "/data/mvh_perm_data").c_str());
	writeRaw(base + "/" + kCanonical, payload);
}

void removeModTree ()
{
	const std::string base = std::string(kModsRoot) + "/" + kModId;
	remove((base + "/" + kCanonical).c_str());
	_rmdir((base + "/data/mvh_perm_data").c_str());
	remove((base + "/.modindex-cache").c_str());
	_rmdir((base + "/data").c_str());
	_rmdir(base.c_str());
	_rmdir(kModsRoot);
}

void setActiveMod (const char* modId)
{
	_putenv_s(mc2::gate::name<"MC2_ACTIVE_MOD">(),modId ? modId : "");
}

// ---- per-API probes ---------------------------------------------------------
// Layer id served through File::open (0 = open failed).
int layerViaFile (const char* spelling, std::string* bytesOut = nullptr)
{
	File f;
	if (f.open(spelling) != NO_ERR)
		return 0;
	std::string bytes = readAll(f);
	f.close();
	if (bytesOut) *bytesOut = bytes;
	size_t pos = bytes.find("LayerId = ");
	if (pos == std::string::npos) return -1;
	return bytes[pos + 10] - '0';
}

// Layer id served through FitIniFile::open (0 = open failed).
int layerViaFitIni (const char* spelling)
{
	FitIniFile fit;
	if (fit.open(spelling) != NO_ERR)
		return 0;
	long v = -1;
	bool ok = (fit.seekBlock("PermBlock") == NO_ERR) &&
	          (fit.readIdLong("LayerId", v) == NO_ERR);
	fit.close();
	return ok ? (int)v : -1;
}

} // namespace

TEST_SUITE("PathPermutations")
{

TEST_CASE("FST-only: every spelling resolves via the FST for every ladder API")
{
	const std::string fstPath = scratchPath("archive.fst");
	authorFst(fstPath, kCanonical, fitPayload(3));
	removeLoose();          // ensure no loose override survives a prior run
	setActiveMod(nullptr);
	InitModSearchPaths("mvh_perm_mods_nonexistent");  // stock mode, empty index

	{
		FstRegistry reg(fstPath.c_str());
		REQUIRE(reg.ok);

		for (int i = 0; i < kNumSpellings; ++i)
		{
			const char* sp = kSpellings[i];
			CAPTURE(sp);

			// File::open — pre-normalizes internally (S_strlwr + slash fold);
			// green pre-fix, stays green (funnels through fst_hash chokepoint).
			CHECK(layerViaFile(sp) == 3);

			// FitIniFile::open — the historical issue-#8 regression lock
			// (backslash spelling x FST-only).
			CHECK(layerViaFitIni(sp) == 3);

			// fileExists tier — THE red-before/green-after gate cell:
			// pre-fix, raw fName reached FastFileFind, so every non-canonical
			// spelling returned 0 while open() succeeded (file.cpp:853 class).
			CHECK(fileExists(sp) == 2);

			// FastFileFind direct — callee-side chokepoint (fastfile.cpp
			// hashed the raw caller string pre-fix).
			long h = -1;
			CHECK(FastFileFind(sp, h) != nullptr);
		}
	}
	remove(fstPath.c_str());
}

TEST_CASE("loose-only: every spelling resolves loose (tier 1), bytes identical")
{
	buildLoose(fitPayload(2));
	setActiveMod(nullptr);
	InitModSearchPaths("mvh_perm_mods_nonexistent");

	std::string canonicalBytes;
	REQUIRE(layerViaFile(kCanonical, &canonicalBytes) == 2);

	for (int i = 0; i < kNumSpellings; ++i)
	{
		const char* sp = kSpellings[i];
		CAPTURE(sp);
		std::string bytes;
		CHECK(layerViaFile(sp, &bytes) == 2);
		CHECK(bytes == canonicalBytes);         // identical bytes across spellings
		CHECK(layerViaFitIni(sp) == 2);
		CHECK(fileExists(sp) == 1);             // hard-drive tier
	}
	removeLoose();
}

TEST_CASE("layer priority is spelling-independent: mod > loose > FST")
{
	_putenv_s(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">(),"1");
	const std::string fstPath = scratchPath("archive3.fst");
	authorFst(fstPath, kCanonical, fitPayload(3));
	buildLoose(fitPayload(2));
	buildModTree(fitPayload(1));
	setActiveMod(kModId);
	InitModSearchPaths(kModsRoot);

	{
		FstRegistry reg(fstPath.c_str());
		REQUIRE(reg.ok);

		// All three layers present: the MOD must win for every spelling.
		for (int i = 0; i < kNumSpellings; ++i)
		{
			CAPTURE(kSpellings[i]);
			CHECK(layerViaFile(kSpellings[i]) == 1);
			CHECK(layerViaFitIni(kSpellings[i]) == 1);
			CHECK(fileExists(kSpellings[i]) == 1);  // mod = tier 1
		}

		// Drop the mod layer (stock mode): loose must win for every spelling.
		setActiveMod(nullptr);
		InitModSearchPaths(kModsRoot);
		for (int i = 0; i < kNumSpellings; ++i)
		{
			CAPTURE(kSpellings[i]);
			CHECK(layerViaFile(kSpellings[i]) == 2);
			CHECK(fileExists(kSpellings[i]) == 1);  // loose = tier 1
		}

		// Drop the loose layer: the FST must serve every spelling (tier 2).
		removeLoose();
		for (int i = 0; i < kNumSpellings; ++i)
		{
			CAPTURE(kSpellings[i]);
			CHECK(layerViaFile(kSpellings[i]) == 3);
			CHECK(fileExists(kSpellings[i]) == 2);
		}
	}

	remove(fstPath.c_str());
	removeModTree();
	_putenv_s(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">(),"");
}

TEST_CASE("absent path: clean miss on every API, every spelling")
{
	removeLoose();
	setActiveMod(nullptr);
	InitModSearchPaths("mvh_perm_mods_nonexistent");
	// No FST registry mounted at all.

	for (int i = 0; i < kNumSpellings; ++i)
	{
		CAPTURE(kSpellings[i]);
		File f;
		CHECK(f.open(kSpellings[i]) != NO_ERR);   // miss, no crash / no CD prompt
		CHECK(fileExists(kSpellings[i]) == 0);
		long h = -1;
		CHECK(FastFileFind(kSpellings[i], h) == nullptr);
		FitIniFile fit;
		CHECK(fit.open(kSpellings[i]) != NO_ERR);
	}
}

TEST_CASE("size-subdir strip ladder (File::open base-strip) is spelling-independent")
{
	// Loose file at data/mvh_perm_data/entry.fit; query the SIZE-SUBDIR form
	// data/mvh_perm_data/128/entry.fit — File::open's base-strip resolves it.
	buildLoose(fitPayload(2));
	setActiveMod(nullptr);
	InitModSearchPaths("mvh_perm_mods_nonexistent");

	const char* stripSpellings[] = {
		"data/mvh_perm_data/128/entry.fit",
		"data\\mvh_perm_data\\128\\entry.fit",
		"DATA/MVH_PERM_DATA/128/ENTRY.FIT",
	};
	for (const char* sp : stripSpellings)
	{
		CAPTURE(sp);
		CHECK(layerViaFile(sp) == 2);
		// NOTE (documented asymmetry, out of batch-1 scope): fileExists has
		// no base-strip probe, so it reports 0 here while open() succeeds.
		// The exists<=>open agreement contract is asserted on non-strip
		// cells only; strip parity is a recorded follow-up candidate.
	}
	removeLoose();
}

TEST_CASE("PacketFile::open resolves loose and FST-backed paks across spellings")
{
	// Author a tiny .pak (test_file_inifile.cpp round-trip pattern) at the
	// canonical loose path, then serve the same bytes from an FST.
	const char kPakCanonical[] = "data/mvh_perm_data/entry2.pak";
	const char* pakSpellings[] = {
		"data/mvh_perm_data/entry2.pak",
		"data\\mvh_perm_data\\entry2.pak",
		"DATA\\MVH_PERM_DATA\\ENTRY2.PAK",
	};
	const char p0[] = "perm-packet-payload";

	_mkdir("data");
	_mkdir("data/mvh_perm_data");
	{
		PacketFile out;
		REQUIRE(out.create(kPakCanonical) == NO_ERR);
		out.reserve(1);
		CHECK(out.writePacket(0, (MemoryPtr)p0, sizeof(p0)) == (int)sizeof(p0));
		out.close();
	}

	setActiveMod(nullptr);
	InitModSearchPaths("mvh_perm_mods_nonexistent");

	// Loose pak: every spelling opens and reads the same packet.
	for (const char* sp : pakSpellings)
	{
		CAPTURE(sp);
		PacketFile in;
		REQUIRE(in.open(sp) == NO_ERR);
		REQUIRE(in.seekPacket(0) == NO_ERR);
		char buf[64] = {0};
		REQUIRE((size_t)in.getPacketSize() <= sizeof(buf));
		in.readPacket(0, (unsigned char*)buf);
		CHECK(strcmp(buf, p0) == 0);
		in.close();
	}

	// FST-backed pak: pack the pak bytes into an FST, remove the loose copy.
	std::string pakBytes;
	{
		File f;
		REQUIRE(f.open(kPakCanonical) == NO_ERR);
		pakBytes = readAll(f);
		f.close();
	}
	const std::string fstPath = scratchPath("archive2.fst");
	authorFst(fstPath, kPakCanonical, pakBytes);
	remove(kPakCanonical);
	{
		FstRegistry reg(fstPath.c_str());
		REQUIRE(reg.ok);
		for (const char* sp : pakSpellings)
		{
			CAPTURE(sp);
			PacketFile in;
			REQUIRE(in.open(sp) == NO_ERR);     // backslash cell red pre-fix at
			                                    // the fileExists/FastFileFind
			                                    // layer of derived openers
			REQUIRE(in.seekPacket(0) == NO_ERR);
			char buf[64] = {0};
			REQUIRE((size_t)in.getPacketSize() <= sizeof(buf));
			in.readPacket(0, (unsigned char*)buf);
			CHECK(strcmp(buf, p0) == 0);
			in.close();
		}
	}
	remove(fstPath.c_str());
	_rmdir("data/mvh_perm_data");
}


// ===========================================================================
// mc2::vfs -- mount table + merged index (mclib/vfs.{h,cpp}).
// ===========================================================================
namespace
{
const char kVfsDir[] = "data/mvh_perm_data/";

void vfsPutLoose (const char* leaf, const std::string& payload)
{
	_mkdir("data");
	_mkdir("data/mvh_perm_data");
	writeRaw(std::string(kVfsDir) + leaf, payload);
}
void vfsDropLoose (const char* leaf)
{
	remove((std::string(kVfsDir) + leaf).c_str());
	_rmdir("data/mvh_perm_data");
}
void vfsPutMod (const char* leaf, const std::string& payload)
{
	const std::string base = std::string(kModsRoot) + "/" + kModId;
	_mkdir(kModsRoot);
	_mkdir(base.c_str());
	_mkdir((base + "/data").c_str());
	_mkdir((base + "/data/mvh_perm_data").c_str());
	writeRaw(base + "/" + kVfsDir + leaf, payload);
}
void vfsDropMod (const char* leaf)
{
	const std::string base = std::string(kModsRoot) + "/" + kModId;
	remove((base + "/" + kVfsDir + leaf).c_str());
	_rmdir((base + "/data/mvh_perm_data").c_str());
	remove((base + "/.modindex-cache").c_str());
	_rmdir((base + "/data").c_str());
	_rmdir(base.c_str());
	_rmdir(kModsRoot);
}
void vfsAuthorFst (const std::string& fstPath, const char* const* names, const std::string* payloads, int n)
{
	FastFile ff;
	REQUIRE(ff.create(fstPath.c_str(), /*compressed=*/true) == NO_ERR);
	REQUIRE(ff.reserve(n) == NO_ERR);
	for (int i = 0; i < n; ++i)
	{
		std::string buf = payloads[i];
		REQUIRE(ff.writeFast(names[i], (void*)buf.data(), (int)buf.size()) == NO_ERR);
	}
	ff.close();
}
std::string vfsBytesOf (const mc2::vfs::Entry& e)
{
	if (e.provider == mc2::vfs::Provider::Fst)
	{
		File f;
		REQUIRE(f.open(e.key.c_str()) == NO_ERR);
		std::string bytes = readAll(f);
		f.close();
		return bytes;
	}
	FILE* fp = fopen(e.physicalPath.c_str(), "rb");
	REQUIRE(fp != nullptr);
	std::string out;
	char buf[512];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
	fclose(fp);
	return out;
}
} // namespace

TEST_CASE("vfs index: mod-only, loose-only, FST-only and shadowed names report the right provider and bytes")
{
	_putenv_s(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">(),"1");
	const std::string fstPath = scratchPath("vfs_archive.fst");
	const char* fstNames[] = { "data/mvh_perm_data/fstonly.fit", "data/mvh_perm_data/shadow.fit" };
	const std::string fstPayloads[] = { fitPayload(3), fitPayload(3) };
	vfsAuthorFst(fstPath, fstNames, fstPayloads, 2);
	vfsPutLoose("loosely.fit", fitPayload(2));
	vfsPutLoose("Mixed.FIT", fitPayload(2));          // upper-case extension on disk
	vfsPutLoose("shadow.fit", fitPayload(2));
	vfsPutMod("modonly.fit", fitPayload(1));
	vfsPutMod("shadow.fit", fitPayload(1));
	setActiveMod(kModId);
	InitModSearchPaths(kModsRoot);

	{
		FstRegistry reg(fstPath.c_str());
		REQUIRE(reg.ok);
		mc2::vfs::rebuild();

		struct Cell { const char* leaf; mc2::vfs::Provider want; int layer; unsigned shadowed; };
		const Cell cells[] = {
			{ "modonly.fit",  mc2::vfs::Provider::Mod,   1, 0 },
			{ "loosely.fit",  mc2::vfs::Provider::Loose, 2, 0 },
			{ "Mixed.FIT",    mc2::vfs::Provider::Loose, 2, 0 },
			{ "fstonly.fit",  mc2::vfs::Provider::Fst,   3, 0 },
			{ "shadow.fit",   mc2::vfs::Provider::Mod,   1, 2 },   // mod hides loose AND FST
		};
		for (const Cell& c : cells)
		{
			CAPTURE(c.leaf);
			const auto e = mc2::vfs::lookup(std::string(kVfsDir) + c.leaf);
			REQUIRE(e.has_value());
			CHECK(e->provider == c.want);
			CHECK(e->shadowed == c.shadowed);
			CHECK(vfsBytesOf(*e) == fitPayload(c.layer));
			// Key and physical path stay separate: key is canonical, the physical path keeps its case.
			CHECK(e->key == mc2::vfs::key(std::string(kVfsDir) + c.leaf));
			if (c.want != mc2::vfs::Provider::Fst)
				CHECK(e->physicalPath.find(c.leaf) != std::string::npos);
			CHECK_FALSE(e->name.empty());
			if (c.want != mc2::vfs::Provider::Fst) CHECK(e->name == c.leaf);

			// Index lookup and the per-lookup ladder agree for every fixture key, every spelling.
			for (int i = 0; i < kNumSpellings; ++i)
			{
				std::string sp = kSpellings[i];
				sp.replace(sp.size() - 9, 9, "");         // drop "entry.fit" ...
				sp += c.leaf;                              // ... and splice the fixture leaf in
				CAPTURE(sp);
				const auto viaSpelling = mc2::vfs::lookup(sp);
				REQUIRE(viaSpelling.has_value());
				CHECK(viaSpelling->provider == c.want);
				CHECK(layerViaFile(sp.c_str()) == c.layer);
			}
		}

		// Case-insensitive extension filter; one entry per key, sorted.
		for (const char* ext : { ".fit", ".FIT", ".Fit" })
		{
			CAPTURE(ext);
			const auto list = mc2::vfs::enumerate(kVfsDir, ext);
			REQUIRE(list.size() == 5);
			const char* wantKeys[] = { "fstonly.fit", "loosely.fit", "mixed.fit", "modonly.fit", "shadow.fit" };
			for (size_t i = 0; i < list.size(); ++i)
				CHECK(list[i].key == std::string(kVfsDir) + wantKeys[i]);
		}
		CHECK(mc2::vfs::enumerate(kVfsDir, ".var").empty());

		// Absent names, '..' and absolute paths never resolve.
		CHECK_FALSE(mc2::vfs::lookup("data/mvh_perm_data/nope.fit").has_value());
		CHECK_FALSE(mc2::vfs::lookup("data/../mvh_perm_data/loosely.fit").has_value());
		CHECK_FALSE(mc2::vfs::lookup("C:/data/mvh_perm_data/loosely.fit").has_value());

		const mc2::vfs::Stats st = mc2::vfs::stats();
		CHECK(st.entries >= 5);
		CHECK(st.mounts >= 3);          // the FST archive, loose, the mod
		CHECK(st.bytes > 0);
	}

	remove(fstPath.c_str());
	vfsDropLoose("loosely.fit"); vfsDropLoose("Mixed.FIT"); vfsDropLoose("shadow.fit");
	vfsDropMod("modonly.fit"); vfsDropMod("shadow.fit");
	setActiveMod(nullptr);
	InitModSearchPaths("mvh_perm_mods_nonexistent");
	_putenv_s(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">(),"");
}

TEST_CASE("vfs index: a mod-only map appears in the MP map list (data/missions/*.fit)")
{
	_putenv_s(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">(),"1");
	const std::string base = std::string(kModsRoot) + "/" + kModId;
	_mkdir(kModsRoot);
	_mkdir(base.c_str());
	_mkdir((base + "/data").c_str());
	_mkdir((base + "/data/missions").c_str());
	writeRaw(base + "/data/missions/mvh_perm_map.fit", fitPayload(1));
	setActiveMod(kModId);
	InitModSearchPaths(kModsRoot);
	mc2::vfs::rebuild();

	// MPLoadMap::seedDialog lists exactly this enumeration (missionPath = "data/missions/").
	const auto maps = mc2::vfs::enumerate("data/missions/", ".fit");
	bool found = false;
	for (const auto& m : maps)
		if (m.key == "data/missions/mvh_perm_map.fit")
		{
			found = true;
			CHECK(m.provider == mc2::vfs::Provider::Mod);
			CHECK(m.name == "mvh_perm_map.fit");     // the writable basename addFile strips ".fit" from
		}
	CHECK(found);

	// Sorted by key, no duplicate keys.
	for (size_t i = 1; i < maps.size(); ++i)
		CHECK(maps[i - 1].key < maps[i].key);

	// Stock mode: the mod map disappears after the reload (rebuild is explicit, no watcher).
	setActiveMod(nullptr);
	InitModSearchPaths("mvh_perm_mods_nonexistent");
	for (const auto& m : mc2::vfs::enumerate("data/missions/", ".fit"))
		CHECK(m.key != "data/missions/mvh_perm_map.fit");

	remove((base + "/data/missions/mvh_perm_map.fit").c_str());
	_rmdir((base + "/data/missions").c_str());
	remove((base + "/.modindex-cache").c_str());
	_rmdir((base + "/data").c_str());
	_rmdir(base.c_str());
	_rmdir(kModsRoot);
	_putenv_s(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">(),"");
}

TEST_CASE("vfs join/key: one value-type helper, exactly one separator, canonical key")
{
	CHECK(mc2::vfs::key("Data\\Missions/Foo.FIT") == "data/missions/foo.fit");
	CHECK(mc2::vfs::join("data/missions", "a.fit") == "data/missions/a.fit");
	CHECK(mc2::vfs::join("data/missions/", "a.fit") == "data/missions/a.fit");
	CHECK(mc2::vfs::join("Data\\Missions\\", "\\A.FIT") == "data/missions/a.fit");
	CHECK(mc2::vfs::join("", "A.fit") == "a.fit");
}

TEST_CASE("newest save: (group, completedMissions, name) is order-independent")
{
	struct S { long g, m; const char* n; };
	const S saves[] = {
		{ 2, 5, "alpha" },
		{ 2, 5, "Bravo" },     // same rank as alpha: the name decides (case-insensitive, greatest wins)
		{ 2, 3, "zulu" },      // same group, FEWER missions: must NOT win just because it is seen last
		{ 1, 9, "charlie" },   // lower group: never wins despite more missions
	};
	int idx[] = { 0, 1, 2, 3 };
	int orders = 0;
	do {
		mc2::NewestSave best;
		for (int i : idx) best.consider(saves[i].g, saves[i].m, saves[i].n);
		CHECK(best.name == "Bravo");
		CHECK(best.group == 2);
		CHECK(best.missions == 5);
		++orders;
	} while (std::next_permutation(idx, idx + 4));
	CHECK(orders == 24);
	CHECK_FALSE(mc2::NewestSave().valid());
}

// Size-stripped alias rung and the ordered candidate chain (File::open order: mod -> loose -> stripped -> FST).
TEST_CASE("vfs index: size-stripped alias keeps its ladder position and the chain is ordered")
{
	_putenv_s(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">(),"1");
	const std::string fstPath = scratchPath("vfs_alias.fst");
	const char* fstNames[] = { "data/mvh_perm_data/128/a.fit", "data/mvh_perm_data/128/b.fit", "data/mvh_perm_data/128/c.fit" };
	const std::string fstPayloads[] = { fitPayload(3), fitPayload(3), fitPayload(3) };
	vfsAuthorFst(fstPath, fstNames, fstPayloads, 3);
	const std::string base = std::string(kModsRoot) + "/" + kModId;
	_mkdir("data"); _mkdir("data/mvh_perm_data"); _mkdir("data/mvh_perm_data/128");
	_mkdir(kModsRoot); _mkdir(base.c_str()); _mkdir((base + "/data").c_str());
	_mkdir((base + "/data/mvh_perm_data").c_str()); _mkdir((base + "/data/mvh_perm_data/128").c_str());
	writeRaw("data/mvh_perm_data/a.fit", fitPayload(2));          // stripped alias of a (FST literal below it)
	writeRaw("data/mvh_perm_data/b.fit", fitPayload(2));          // stripped alias of b
	writeRaw(base + "/data/mvh_perm_data/128/b.fit", fitPayload(1)); // mod literal beats the alias
	writeRaw("data/mvh_perm_data/128/c.fit", fitPayload(2));      // loose literal beats the alias
	writeRaw("data/mvh_perm_data/c.fit", fitPayload(4));
	setActiveMod(kModId);
	InitModSearchPaths(kModsRoot);
	{
		FstRegistry reg(fstPath.c_str());
		REQUIRE(reg.ok);
		mc2::vfs::rebuild();

		// a: no mod, no loose literal -> the stripped loose alias beats the FST literal.
		auto a = mc2::vfs::candidates("data/mvh_perm_data/128/a.fit");
		REQUIRE(a.size() == 2);
		CHECK(a[0].provider == mc2::vfs::Provider::Loose);
		CHECK(a[0].viaStripAlias);
		CHECK(vfsBytesOf(a[0]) == fitPayload(2));
		CHECK(a[1].provider == mc2::vfs::Provider::Fst);
		CHECK(layerViaFile("data/mvh_perm_data/128/a.fit") == 2);   // the live ladder agrees

		// b: mod literal > stripped alias > FST literal.
		auto b = mc2::vfs::candidates("DATA\\mvh_perm_data\\128\\B.FIT");
		REQUIRE(b.size() == 3);
		CHECK(b[0].provider == mc2::vfs::Provider::Mod);
		CHECK_FALSE(b[0].viaStripAlias);
		CHECK(b[1].viaStripAlias);
		CHECK(b[2].provider == mc2::vfs::Provider::Fst);
		CHECK(layerViaFile("data/mvh_perm_data/128/b.fit") == 1);

		// c: loose literal > stripped alias > FST literal.
		auto c = mc2::vfs::candidates("data/mvh_perm_data/128/c.fit");
		REQUIRE(c.size() == 3);
		CHECK_FALSE(c[0].viaStripAlias);
		CHECK(vfsBytesOf(c[0]) == fitPayload(2));
		CHECK(c[1].viaStripAlias);
		CHECK(vfsBytesOf(c[1]) == fitPayload(4));
		CHECK(c[2].provider == mc2::vfs::Provider::Fst);
		CHECK(layerViaFile("data/mvh_perm_data/128/c.fit") == 2);

		// An aliased key is not a directory entry: enumeration lists literal keys only.
		const auto list = mc2::vfs::enumerate("data/mvh_perm_data/128/", ".fit");
		CHECK(list.size() == 3);
	}
	remove(fstPath.c_str());
	remove("data/mvh_perm_data/a.fit"); remove("data/mvh_perm_data/b.fit"); remove("data/mvh_perm_data/c.fit");
	remove("data/mvh_perm_data/128/c.fit"); _rmdir("data/mvh_perm_data/128"); _rmdir("data/mvh_perm_data");
	remove((base + "/data/mvh_perm_data/128/b.fit").c_str()); _rmdir((base + "/data/mvh_perm_data/128").c_str());
	_rmdir((base + "/data/mvh_perm_data").c_str()); remove((base + "/.modindex-cache").c_str());
	_rmdir((base + "/data").c_str()); _rmdir(base.c_str()); _rmdir(kModsRoot);
	setActiveMod(nullptr);
	InitModSearchPaths("mvh_perm_mods_nonexistent");
	_putenv_s(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">(),"");
}

namespace
{
bool vfsLooksLikeFit (const mc2::vfs::Entry& e)
{
	return vfsBytesOf(e).compare(0, 6, "FITini") == 0;
}
}

TEST_CASE("vfs index: a corrupt or unreadable winner falls back to the next candidate, precedence intact")
{
	_putenv_s(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">(),"1");
	const std::string fstPath = scratchPath("vfs_corrupt.fst");
	const char* fstNames[] = { "data/mvh_perm_data/fall.fit" };
	const std::string fstPayloads[] = { fitPayload(3) };
	vfsAuthorFst(fstPath, fstNames, fstPayloads, 1);
	vfsPutLoose("fall.fit", fitPayload(2));
	vfsPutMod("fall.fit", "garbage, not a FIT file");
	setActiveMod(kModId);
	InitModSearchPaths(kModsRoot);
	{
		FstRegistry reg(fstPath.c_str());
		REQUIRE(reg.ok);
		mc2::vfs::rebuild();
		const std::string k = "data/mvh_perm_data/fall.fit";

		// Precedence is unchanged: the (corrupt) mod is still the lookup winner.
		const auto win = mc2::vfs::lookup(k);
		REQUIRE(win.has_value());
		CHECK(win->provider == mc2::vfs::Provider::Mod);
		CHECK(win->shadowed == 2);

		// A content-validating predicate skips it: the loose candidate serves.
		auto r1 = mc2::vfs::resolve(k, &vfsLooksLikeFit);
		REQUIRE(r1.has_value());
		CHECK(r1->provider == mc2::vfs::Provider::Loose);
		CHECK(vfsBytesOf(*r1) == fitPayload(2));

		// Corrupt the loose file as well: the FST candidate serves.
		writeRaw(std::string(kVfsDir) + "fall.fit", "also garbage");
		auto r2 = mc2::vfs::resolve(k, &vfsLooksLikeFit);
		REQUIRE(r2.has_value());
		CHECK(r2->provider == mc2::vfs::Provider::Fst);
		CHECK(vfsBytesOf(*r2) == fitPayload(3));

		// Default readability (physical file opens): delete the mod file under the stale index.
		writeRaw(std::string(kVfsDir) + "fall.fit", fitPayload(2));
		const std::string modFile = std::string(kModsRoot) + "/" + kModId + "/" + k;
		remove(modFile.c_str());
		auto r3 = mc2::vfs::resolve(k);
		REQUIRE(r3.has_value());
		CHECK(r3->provider == mc2::vfs::Provider::Loose);
	}
	remove(fstPath.c_str());
	vfsDropLoose("fall.fit"); vfsDropMod("fall.fit");
	setActiveMod(nullptr);
	InitModSearchPaths("mvh_perm_mods_nonexistent");
	_putenv_s(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">(),"");
}

} // TEST_SUITE("PathPermutations")
