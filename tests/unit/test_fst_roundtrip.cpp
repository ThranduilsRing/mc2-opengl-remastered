// tests/unit/test_fst_roundtrip.cpp
//
// MINIMAL-VIABLE-HARNESS-2 (TU-2): proves the loose-vs-FST resolution ladder
// end to end with NO new engine TU -- ffile.cpp's create/writeFast and
// fastfile.cpp's registry (FastFileInit/Find/Fini) are already linked by
// MINIMAL-VIABLE-HARNESS-1. This authors a tiny .fst in scratch, registers it,
// and drives File::open through both branches.
//
// THE CONTRACT everything relies on (file.cpp::File::open, READ branch):
//   1. A loose disk file is tried FIRST (_open). Only on INVALID_HANDLE_VALUE
//      does resolution fall through to FastFileFind (the FST).
//   => loose OVERRIDES packed; packed SERVES when loose is absent.
// This was previously untested; the initVariants / mech-CSV / .fit load paths
// all assume it. Pattern doc: docs/testing/minimal-viable-harness.md.
//
// FST authoring path mirrors the makefst tool: create(name,true) ->
// reserve(n) -> writeFast(entryName, buf, len) per entry -> close(). Entries
// are LZ/zlib compressed (compressed=true), matching every stock FST.

#include "doctest.h"

#include "file.h"
#include "ffile.h"
#include "fastfile.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace
{

// FST entry keys are stored/looked up lowercase with forward slashes
// (File::open lowercases + '\\'->'/' before FastFileFind; fastfile.cpp uses
// forward-slash keys). Keep the authored key already-normalized so the query
// path matches without surprises. A FLAT name in cwd is used deliberately so
// the loose-override half of the ladder can create the file without needing a
// data/objects/ subtree to exist (FastFileFind imposes no data/ prefix -- that
// gate is mod-search-only, exercised in test_paths_mod.cpp).
const char kFstEntryName[]  = "mvh_scratch_fst_entry.txt";
const char kFstPayload[]    = "packed-from-fst-archive";
const char kLoosePayload[]  = "LOOSE-OVERRIDE-WINS";

std::string scratchPath (const char *name)
{
	std::string p("mvh_scratch_");
	p += name;
	return p;
}

// Read a whole File (loose or FST-backed) into a std::string.
std::string readAll (File &f)
{
	const long len = (long)f.getLength();
	std::string out;
	out.resize(len > 0 ? (size_t)len : 0u);
	if (len > 0)
	{
		f.seek(0);
		f.read((MemoryPtr)&out[0], len);
	}
	return out;
}

// Write a loose disk file with raw CRT (bypasses File so the test controls the
// on-disk state the resolver sees).
void writeLoose (const char *path, const char *data, size_t len)
{
	FILE *fp = fopen(path, "wb");
	REQUIRE(fp != nullptr);
	fwrite(data, 1, len, fp);
	fclose(fp);
}

// One-time-per-case FST registry lifecycle. The engine allocates the
// fastFiles[] array at boot (mechcmd2.cpp); the harness owns it here.
struct FstRegistry
{
	explicit FstRegistry (const char *fstPath)
	{
		maxFastFiles = 4;
		numFastFiles = 0;
		fastFiles = (FastFile **)malloc(sizeof(FastFile *) * maxFastFiles);
		for (long i = 0; i < maxFastFiles; ++i) fastFiles[i] = nullptr;
		ok = FastFileInit(fstPath);
	}
	~FstRegistry ()
	{
		FastFileFini();   // frees fastFiles + closes archives
	}
	bool ok = false;
};

// Author a minimal single-entry FST on disk (compressed, like stock).
void authorFst (const std::string &fstPath, const char *entryName,
                const char *payload, int payloadLen)
{
	FastFile ff;
	REQUIRE(ff.create(fstPath.c_str(), /*compressed=*/true) == NO_ERR);
	REQUIRE(ff.reserve(1) == NO_ERR);
	// writeFast takes a non-const buffer (it may compress in place via a work
	// buffer, but the source is treated read-only here); copy to be safe.
	std::string buf(payload, payload + payloadLen);
	REQUIRE(ff.writeFast(entryName, (void *)buf.data(), payloadLen) == NO_ERR);
	ff.close();
}

} // namespace

TEST_SUITE("FstRoundtrip")
{

TEST_CASE("packed serves when loose absent; loose overrides packed")
{
	const std::string fstPath = scratchPath("archive.fst");
	authorFst(fstPath, kFstEntryName, kFstPayload, (int)strlen(kFstPayload));

	// Make sure no loose file with the entry's name exists from a prior run.
	remove(kFstEntryName);

	// Inner scope so the FstRegistry (and its open FST handle) is destroyed
	// before we remove the .fst file below -- Windows refuses to delete a file
	// that still has an open handle.
	{
		FstRegistry reg(fstPath.c_str());
		REQUIRE(reg.ok);

		SUBCASE("FST hit when no loose file exists")
		{
			// Sanity: the registry can find the entry directly.
			long h = -1;
			CHECK(FastFileFind(kFstEntryName, h) != nullptr);

			File f;
			REQUIRE(f.open(kFstEntryName) == NO_ERR);   // resolves via FST branch
			CHECK(readAll(f) == kFstPayload);
			f.close();
		}

		SUBCASE("loose file with the same name overrides the FST entry")
		{
			// Author a loose file at the exact (lowercased) resolve path. File::open
			// lowercases before _open, and the key is already lowercase, so the
			// loose path IS kFstEntryName.
			writeLoose(kFstEntryName, kLoosePayload, strlen(kLoosePayload));

			File f;
			REQUIRE(f.open(kFstEntryName) == NO_ERR);   // loose branch wins
			CHECK(readAll(f) == kLoosePayload);
			f.close();

			// Remove the loose override; resolution must fall back to the FST.
			remove(kFstEntryName);
			File f2;
			REQUIRE(f2.open(kFstEntryName) == NO_ERR);
			CHECK(readAll(f2) == kFstPayload);
			f2.close();
		}
	}

	remove(fstPath.c_str());
	remove(kFstEntryName);
}

TEST_CASE("fileExists reports loose (1) vs FST (2) tiers")
{
	// fileExists() encodes the same ladder as a destination bitmask:
	//   loose/hard-drive => bit 1, FastFile => bit 2.
	const std::string fstPath = scratchPath("archive2.fst");
	authorFst(fstPath, kFstEntryName, kFstPayload, (int)strlen(kFstPayload));
	remove(kFstEntryName);

	{
		FstRegistry reg(fstPath.c_str());
		REQUIRE(reg.ok);

		// No loose file: fileExists sees it only in the FST => tier 2.
		CHECK(fileExists(kFstEntryName) == 2);

		// Add a loose copy: now the hard-drive tier (1) short-circuits first.
		writeLoose(kFstEntryName, kLoosePayload, strlen(kLoosePayload));
		CHECK(fileExists(kFstEntryName) == 1);
	}

	remove(kFstEntryName);
	remove(fstPath.c_str());
}

} // TEST_SUITE("FstRoundtrip")
