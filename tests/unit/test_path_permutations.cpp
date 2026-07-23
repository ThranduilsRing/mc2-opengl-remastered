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

#include "file.h"
#include "inifile.h"
#include "packet.h"
#include "ffile.h"
#include "fastfile.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <direct.h>   // _mkdir / _rmdir

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
	_putenv_s("MC2_ACTIVE_MOD", modId ? modId : "");
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
	_putenv_s("MC2_REBUILD_MOD_CACHE", "1");
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
	_putenv_s("MC2_REBUILD_MOD_CACHE", "");
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

} // TEST_SUITE("PathPermutations")
