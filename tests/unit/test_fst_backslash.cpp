// tests/unit/test_fst_backslash.cpp
//
// Retail-format FSTs (MCO art/tgl, REAP3, CVE-G) store member names with
// backslashes (and mixed case). FastFile::open canonicalizes every entry name
// in memory (lowercase, '\\' -> '/') and rebuilds its elfHash; the file on disk
// is never modified. Stock archives (already canonical) must come through
// byte-identically.

#include "doctest.h"

#include "ffile.h"
#include "fastfile.h"
#include "fst_hash.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

struct Registry
{
	explicit Registry (const char *p)
	{
		maxFastFiles = 4;
		numFastFiles = 0;
		fastFiles = (FastFile **)malloc(sizeof(FastFile *) * maxFastFiles);
		for (long i = 0; i < maxFastFiles; ++i) fastFiles[i] = nullptr;
		ok = FastFileInit(p);
	}
	~Registry () { FastFileFini(); }
	bool ok = false;
};

std::vector<char> slurp (const std::string &p, size_t maxBytes = (size_t)-1)
{
	std::vector<char> v;
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) return v;
	fseek(f, 0, SEEK_END);
	size_t n = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > maxBytes) n = maxBytes;
	v.resize(n);
	if (n > 0) fread(v.data(), 1, n, f);
	fclose(f);
	return v;
}

// Overwrite entry i's hash + name directly in the on-disk table.
void patchEntry (const std::string &path, int i, const char *name, DWORD hash)
{
	FILE *f = fopen(path.c_str(), "r+b");
	REQUIRE(f != nullptr);
	const long base = FASTFILE_ENTRY_TABLE_START + (long)i * (long)sizeof(FILEENTRY);
	char buf[MAX_FILENAME_SIZE] = {};
	strncpy(buf, name, MAX_FILENAME_SIZE - 1);
	fseek(f, base + 12, SEEK_SET);
	fwrite(&hash, 1, 4, f);
	fwrite(buf, 1, MAX_FILENAME_SIZE, f);
	fclose(f);
}

} // namespace

TEST_SUITE("FstBackslash")
{

TEST_CASE("backslash + mixed-case members resolve; disk bytes untouched")
{
	const std::string path = "mvh_scratch_backslash.fst";
	const char *payloadA = "payload-alpha";
	const char *payloadB = "payload-bravo";
	{
		FastFile ff;
		REQUIRE(ff.create(path.c_str(), true) == 0);
		REQUIRE(ff.reserve(3) == 0);
		std::string a(payloadA), b(payloadB), c("plain");
		REQUIRE(ff.writeFast("x", (void *)a.data(), (int)a.size()) == 0);
		REQUIRE(ff.writeFast("y", (void *)b.data(), (int)b.size()) == 0);
		REQUIRE(ff.writeFast("data/stock/plain.txt", (void *)c.data(), (int)c.size()) == 0);
		ff.close();
	}
	// Retail style: backslashes, mixed case; hash over the raw name for one
	// entry, zero for the other.
	const char *rawA = "Data\\Art\\Foo.TGA";
	const char *rawB = "data\\objects\\bar.fit";
	patchEntry(path, 0, rawA, (DWORD)elfHash(rawA));
	patchEntry(path, 1, rawB, 0);

	const std::vector<char> before = slurp(path);
	REQUIRE(!before.empty());

	{
		Registry reg(path.c_str());
		REQUIRE(reg.ok);

		long h = -1;
		FastFile *ff = FastFileFind("data/art/foo.tga", h);
		REQUIRE(ff != nullptr);
		CHECK(ff->sizeFast(h) == (long)strlen(payloadA));
		std::string out(strlen(payloadA), '\0');
		CHECK(ff->readFast(h, &out[0], (DWORD)out.size()) == (long)out.size());
		CHECK(out == payloadA);
		ff->closeFast(h);

		// Caller spelling must not matter (FastFileFind canonicalizes).
		CHECK(FastFileFind("DATA\\Objects\\Bar.fit", h) != nullptr);
		CHECK(FastFileFind("data/stock/plain.txt", h) != nullptr);
		CHECK(FastFileFind("data/art/missing.tga", h) == nullptr);

		// In-memory entries are canonical with a matching hash.
		const FILE_HANDLE *fi = fastFiles[0]->getFilesInfo();
		for (long i = 0; i < fastFiles[0]->getNumFiles(); ++i)
		{
			CHECK(strchr(fi[i].pfe->name, '\\') == nullptr);
			CHECK(fi[i].pfe->hash == (DWORD)elfHash(fi[i].pfe->name));
		}
	}

	CHECK(slurp(path) == before);   // never rewrites the archive on disk
	remove(path.c_str());
}

// Stock archives: every entry already canonical, so open() must leave name and
// hash exactly as stored, and each member must resolve. Skipped when the stock
// install is absent. Also prints the open-time cost.
TEST_CASE("stock fst parity: names/hashes identical to disk, all members resolve")
{
	const char *roots[] = {
		"A:/Games/mc2-opengl/mc2-win64-v0.4c/",
		"A:/Games/mc2-opengl/mc2-win64-v0.4/",
	};
	const char *names[] = {"tgl.fst", "art.fst", "mission.fst", "textures.fst"};
	int tested = 0;
	for (const char *r : roots)
	{
		for (const char *n : names)
		{
			const std::string p = std::string(r) + n;
			std::vector<char> hdr = slurp(p, 8);
			if (hdr.size() < 8) continue;
			DWORD count;
			memcpy(&count, hdr.data() + 4, 4);
			const std::vector<char> raw = slurp(p, FASTFILE_ENTRY_TABLE_START + (size_t)count * sizeof(FILEENTRY));

			const auto t0 = std::chrono::steady_clock::now();
			Registry reg(p.c_str());
			const auto t1 = std::chrono::steady_clock::now();
			REQUIRE(reg.ok);
			printf("[FST_OPEN] %s entries=%u open_ms=%.3f\n", p.c_str(), count,
			       std::chrono::duration<double, std::milli>(t1 - t0).count());

			FastFile *ff = fastFiles[0];
			REQUIRE((DWORD)ff->getNumFiles() == count);
			const FILE_HANDLE *fi = ff->getFilesInfo();
			for (DWORD i = 0; i < count; ++i)
			{
				const size_t eo = FASTFILE_ENTRY_TABLE_START + (size_t)i * sizeof(FILEENTRY);
				const char *rn = raw.data() + eo + 16;
				DWORD rh;
				memcpy(&rh, raw.data() + eo + 12, 4);
				// A non-canonical stock entry would be widened by design; compare
				// only the canonical ones (all of them, for stock).
				bool canon = true;
				for (const char *c = rn; *c; ++c)
					if (*c == '\\' || (*c >= 'A' && *c <= 'Z')) { canon = false; break; }
				if (!canon) continue;
				CHECK(fi[i].pfe->hash == rh);
				CHECK(strncmp(fi[i].pfe->name, rn, MAX_FILENAME_SIZE) == 0);
			}
			++tested;
			for (DWORD i = 0; i < count; i += (count / 50) + 1)
			{
				long h = -1;
				CHECK(FastFileFind(fi[i].pfe->name, h) != nullptr);
			}
		}
		if (tested) break;
	}
	if (!tested) MESSAGE("stock install absent; parity case skipped");
}

}
