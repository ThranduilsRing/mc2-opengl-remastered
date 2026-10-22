// tests/unit/test_fst_roundtrip.cpp
//
// Proves the loose-vs-FST resolution ladder end to end with NO new engine TU --
// ffile.cpp's create/writeFast and fastfile.cpp's registry
// (FastFileInit/Find/Fini) are already linked by the unit-test harness. This
// authors a tiny .fst in scratch, registers it, and drives File::open through
// both branches.
//
// THE CONTRACT everything relies on (file.cpp::File::open, READ branch):
//   1. A loose disk file is tried FIRST (_open). Only on INVALID_HANDLE_VALUE
//      does resolution fall through to FastFileFind (the FST).
//   => loose OVERRIDES packed; packed SERVES when loose is absent.
// The initVariants / mech-CSV / .fit load paths all assume it.
//
// FST authoring path mirrors the makefst tool: create(name,true) ->
// reserve(n) -> writeFast(entryName, buf, len) per entry -> close(). Entries
// are LZ/zlib compressed (compressed=true), matching every stock FST.

#include "doctest.h"

#include "file.h"
#include "ffile.h"
#include "fastfile.h"

#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iterator>
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

TEST_CASE("legacy archive keys resolve canonically without rewriting the archive")
{
    const auto fstPath = scratchPath("legacy_keys.fst");
    constexpr char packedName[] = "DaTa\\ArT\\mvh_scratch_legacy_key.txt";
    constexpr char canonicalName[] = "data/art/mvh_scratch_legacy_key.txt";
    authorFst(fstPath, packedName, kFstPayload, static_cast<int>(std::size(kFstPayload) - 1));
    const auto archiveBytes = [&] {
        std::ifstream archive(fstPath, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(archive), {});
    };
    const auto before = archiveBytes();
    {
        FstRegistry registry(fstPath.c_str());
        REQUIRE(registry.ok);
        CHECK(fileExists(canonicalName) == 2);
        File file;
        REQUIRE(file.open(canonicalName) == NO_ERR);
        CHECK(readAll(file) == kFstPayload);
        file.close();
        File variant;
        REQUIRE(variant.open("DATA\\ART\\MVH_SCRATCH_LEGACY_KEY.TXT") == NO_ERR);
        CHECK(readAll(variant) == kFstPayload);
    }
    CHECK(archiveBytes() == before);
    std::filesystem::remove(fstPath);
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

// A corrupt archive entry that claims an image over the 1 GiB cap is refused before any allocation, and the entry is released so
// the archive does not keep a handle open for a file that never opened.
TEST_CASE("an FST entry whose uncompressed size is implausible is rejected and released")
{
	const std::string fstPath = scratchPath("archive3.fst");
	authorFst(fstPath, kFstEntryName, kFstPayload, (int)strlen(kFstPayload));
	remove(kFstEntryName);

	// realSize of the first entry: after the 4-byte version and 4-byte count, at offset 8 inside FILEENTRY.
	const DWORD lyingSize = 0x50000000u;
	{
		FILE *fp = fopen(fstPath.c_str(), "r+b");
		REQUIRE(fp != nullptr);
		REQUIRE(fseek(fp, FASTFILE_ENTRY_TABLE_START + 8, SEEK_SET) == 0);
		REQUIRE(fwrite(&lyingSize, 1, sizeof(lyingSize), fp) == sizeof(lyingSize));
		fclose(fp);
	}

	{
		FstRegistry reg(fstPath.c_str());
		REQUIRE(reg.ok);

		long h = -1;
		FastFile *ff = FastFileFind(kFstEntryName, h);
		REQUIRE(ff != nullptr);
		CHECK(ff->sizeFast(h) == (long)lyingSize);
		ff->closeFast(h);

		File f;
		CHECK(f.open(kFstEntryName) == FILE_NOT_OPEN);
		CHECK_FALSE(f.isOpen());
		CHECK(f.getFilename() == nullptr);
		// Whatever slot File::open took, no slot of the archive is left in use.
		REQUIRE(ff->getNumFiles() > 0);
		for (long slot = 0; slot < ff->getNumFiles(); ++slot)
			CHECK(ff->sizeFast(slot) == -1);
	}

	remove(fstPath.c_str());
}

// LG17 span guard: FastFile::writeFileEntries takes only the span now (the old
// (FILE_HANDLE* files, int num_files) pair is gone). reserve() writes the whole
// zeroed table through it (four entries here), and writeFast() then rewrites
// one entry through a one-element span at that entry's offset. An incomplete
// table or a wrong offset shows up as a failed lookup or the wrong bytes.
TEST_CASE("multi-entry archive: reserve's span writes the whole table, writeFast's span one entry")
{
	const char *names[] = {"mvh_scratch_fst_a.txt", "mvh_scratch_fst_b.txt",
	                       "mvh_scratch_fst_c.txt", "mvh_scratch_fst_d.txt"};
	const char *payloads[] = {kFstPayload, "second-entry-payload", "third", "fourth-entry-payload"};
	const int count = 4;
	const std::string fstPath = scratchPath("archive4.fst");

	{
		FastFile ff;
		REQUIRE(ff.create(fstPath.c_str(), /*compressed=*/true) == NO_ERR);
		REQUIRE(ff.reserve(count) == NO_ERR);
		REQUIRE(ff.getNumFiles() == count);
		for (int i = 0; i < count; ++i)
			CHECK(ff.getFilesInfo()[i].inuse == FALSE);
		for (int i = 0; i < count; ++i) {
			std::string buf(payloads[i]);
			REQUIRE(ff.writeFast(names[i], (void *)buf.data(), (int)buf.size()) == NO_ERR);
		}
		ff.close();
	}

	{
		FstRegistry reg(fstPath.c_str());
		REQUIRE(reg.ok);
		for (int i = 0; i < count; ++i) {
			long h = -1;
			FastFile *ff = FastFileFind(names[i], h);   // opens slot h by name
			REQUIRE(ff != nullptr);
			const long len = ff->sizeFast(h);
			REQUIRE(len == (long)strlen(payloads[i]));
			std::string got((size_t)len, '\0');
			REQUIRE(ff->readFast(h, &got[0], (DWORD)len) == len);
			CHECK(got == payloads[i]);
			ff->closeFast(h);
		}
	}

	remove(fstPath.c_str());
}
} // TEST_SUITE("FstRoundtrip")
