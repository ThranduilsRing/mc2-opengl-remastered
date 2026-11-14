// tests/unit/test_vfs_content_digest.cpp
//
// M1 of the stock-asset container plan (rev 4): every VFS candidate carries a SourceRecord (its authority rank =
// its place in the resolution chain), and the content digest is computed from the bytes of the winning candidate,
// not memoised by path + size. Offline, game-free; scratch files only, removed at the end of each case.
//
//   * mod > loose rank order, and the digest follows the winner (the mod's bytes)
//   * duplicate key in two FST archives: registration order is the authority order (the earlier archive wins)
//   * stale-memo fault injection: a same-size edit MUST change the digest (path+size memo would not)
//   * bound: a same-size edit with a restored mtime is caught by explicit verification (also used on TXM misses)

#include "doctest.h"

#include "fast_hash.h"
#include "ffile.h"
#include "fastfile.h"
#include "file.h"
#include "paths.h"
#include "vfs.h"
#include "terrain_cache_key.h"
#include "Gates/gate_cold.h"
#include "Gates/gate_name.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

#ifdef _WIN32
#include <direct.h>
#else
#include "platform_io.h"
#endif
#include "platform_windows.h"

namespace fs = std::filesystem;

namespace
{
const char* kLooseKey = "data/mvh_scratch_data/digest.txt";
const char* kModsRoot = "mvh_scratch_digest_mods";
const char* kModId = "digestmod";
static constexpr const char* kEnvActiveMod = mc2::gate::name<"MC2_ACTIVE_MOD">();
static constexpr const char* kEnvRebuildCache = mc2::gate::name<"MC2_REBUILD_MOD_CACHE">();

uint64_t digestOf(const std::string& bytes)
{
	return mc2::content::fastHash(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
}

void writeFile(const std::string& path, const std::string& bytes)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	REQUIRE(out.good());
	out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void authorFst(const std::string& fstPath, const char* entry, const std::string& payload)
{
	FastFile ff;
	REQUIRE(ff.create(fstPath.c_str(), true) == NO_ERR);
	REQUIRE(ff.reserve(1) == NO_ERR);
	std::string buf = payload;
	REQUIRE(ff.writeFast(entry, buf.data(), static_cast<int>(buf.size())) == NO_ERR);
	ff.close();
}

struct LooseScratch
{
	LooseScratch() { _mkdir("data"); _mkdir("data/mvh_scratch_data"); }
	~LooseScratch()
	{
		std::error_code ec;
		fs::remove(kLooseKey, ec);
		fs::remove("data/mvh_scratch_data", ec);
	}
};
} // namespace

TEST_SUITE("VfsContentDigest")
{

TEST_CASE("same-size edit changes the digest (stale path+size memo fault injection)")
{
	LooseScratch scratch;
	writeFile(kLooseKey, "AAAAAAAA-first-content");
	mc2::vfs::rebuild();
	const uint64_t first = mc2::vfs::contentDigest(kLooseKey);
	CHECK(first == digestOf("AAAAAAAA-first-content"));
	CHECK(mc2::vfs::contentDigest(kLooseKey) == first);   // memo hit, same value

	// Same size, different bytes, mtime moved: the old (path, size) memo returned `first` here.
	writeFile(kLooseKey, "BBBBBBBB-other-content");
	fs::last_write_time(kLooseKey, fs::last_write_time(kLooseKey) + std::chrono::seconds(7));
	const uint64_t second = mc2::vfs::contentDigest(kLooseKey);
	CHECK(second == digestOf("BBBBBBBB-other-content"));
	CHECK(second != first);

	// A forced verification also catches an edit with a restored mtime.
	const auto keepTime = fs::last_write_time(kLooseKey);
	writeFile(kLooseKey, "CCCCCCCC-third-content");
	fs::last_write_time(kLooseKey, keepTime);
	mc2::vfs::rebuild();
	CHECK(mc2::vfs::contentDigest(kLooseKey, true) == digestOf("CCCCCCCC-third-content"));

	// Unreadable path: 0, never a stale value.
	fs::remove(kLooseKey);
	CHECK(mc2::vfs::contentDigest(kLooseKey) == 0);
	mc2::vfs::rebuild();
}

// Trigger: mclib/vfs_index.cpp, mclib/terrtxm.cpp.
TEST_CASE("persistent digest snapshot hits, verifies, and rejects damaged files")
{
	LooseScratch scratch;
	const std::string memo = "cache/vfs/digests-0000000000000001.memo";
	std::ifstream saved(memo, std::ios::binary);
	const bool existed = saved.good();
	const std::string backup((std::istreambuf_iterator<char>(saved)), {});
	saved.close();
	struct Restore {
		std::string path, bytes; bool existed;
		~Restore() {
			if (existed) writeFile(path, bytes);
			else { std::error_code ec; fs::remove(path, ec); }
			mc2::vfs::rebuild();
		}
	} restore{memo, backup, existed};
	writeFile(kLooseKey, "AAAA");
	mc2::vfs::rebuild();
	const auto first = mc2::vfs::contentDigest(kLooseKey, true);
	mc2::vfs::flushContentDigests();
	REQUIRE(fs::exists(memo));
	const auto stamp = fs::last_write_time(kLooseKey);
	writeFile(kLooseKey, "BBBB");
	fs::last_write_time(kLooseKey, stamp);
	mc2::vfs::rebuild();
	CHECK(mc2::vfs::contentDigest(kLooseKey) == first); // disk hit after dropping the memory memo
	const auto second = mc2::vfs::contentDigest(kLooseKey, true);
	CHECK(second == digestOf("BBBB"));
	mc2::TerrainCacheKey oldKey("txm-composite-v3-digest"), newKey("txm-composite-v3-digest");
	std::array<uint8_t, 8> a{}, b{};
	for (unsigned i = 0; i < 8; ++i) { a[i] = first >> (8 * i); b[i] = second >> (8 * i); }
	oldKey.add(kLooseKey, a); newKey.add(kLooseKey, b);
	CHECK(oldKey.key() != newKey.key());
	// Same stamps with different bytes distinguish a rejected snapshot from a disk hit.
	mc2::vfs::flushContentDigests();
	std::ifstream valid(memo, std::ios::binary);
	const std::string good((std::istreambuf_iterator<char>(valid)), {});
	valid.close();
	for (const bool truncate : {false, true}) {
		auto bad = good;
		if (truncate) bad.resize(bad.size() / 2); else bad.back() ^= 0x40;
		writeFile(memo, bad);
		writeFile(kLooseKey, truncate ? "DDDD" : "CCCC");
		fs::last_write_time(kLooseKey, stamp);
		mc2::vfs::rebuild();
		CHECK(mc2::vfs::contentDigest(kLooseKey) == digestOf(truncate ? "DDDD" : "CCCC"));
	}
}

TEST_CASE("mod override outranks loose and the digest follows the winner")
{
	_putenv_s(kEnvRebuildCache, "1");
	const std::string modData = std::string(kModsRoot) + "/" + kModId + "/data/mvh_scratch_data";
	fs::create_directories(modData);
	LooseScratch scratch;
	writeFile(kLooseKey, "loose-bytes");
	writeFile(modData + "/digest.txt", "mod---bytes");
	_putenv_s(kEnvActiveMod, kModId);
	InitModSearchPaths(kModsRoot);
	mc2::vfs::rebuild();

	const auto chain = mc2::vfs::candidates(kLooseKey);
	REQUIRE(chain.size() >= 2);
	CHECK(chain[0].provider == mc2::vfs::Provider::Mod);
	CHECK(chain[1].provider == mc2::vfs::Provider::Loose);
	for (size_t i = 0; i < chain.size(); ++i) CHECK(chain[i].source.authorityRank == i);
	CHECK(mc2::vfs::contentDigest(kLooseKey) == digestOf("mod---bytes"));

	// Mod edited in place, same size: the digest follows.
	writeFile(modData + "/digest.txt", "MOD---BYTES");
	const auto modFile = fs::path(modData) / "digest.txt";
	fs::last_write_time(modFile, fs::last_write_time(modFile) + std::chrono::seconds(5));
	CHECK(mc2::vfs::contentDigest(kLooseKey) == digestOf("MOD---BYTES"));

	_putenv_s(kEnvActiveMod, "");
	InitModSearchPaths(kModsRoot);
	mc2::vfs::rebuild();
	CHECK(mc2::vfs::contentDigest(kLooseKey) == digestOf("loose-bytes"));

	std::error_code ec;
	fs::remove_all(kModsRoot, ec);
	_putenv_s(kEnvRebuildCache, "");
}

TEST_CASE("duplicate FST key keeps registration order as authority order")
{
	// p28 P1: a later archive must not outrank an earlier one (FastFileFind returns the first registered).
	const char* entry = "mvh_scratch_dup_entry.txt";
	const std::string a = "mvh_scratch_dup_a.fst", b = "mvh_scratch_dup_b.fst";
	authorFst(a, entry, "from-first-archive");
	authorFst(b, entry, "from-later-archive");
	{
		maxFastFiles = 4;
		numFastFiles = 0;
		fastFiles = static_cast<FastFile**>(malloc(sizeof(FastFile*) * maxFastFiles));
		for (long i = 0; i < maxFastFiles; ++i) fastFiles[i] = nullptr;
		REQUIRE(FastFileInit(a.c_str()));
		REQUIRE(FastFileInit(b.c_str()));
		mc2::vfs::rebuild();

		const auto chain = mc2::vfs::candidates(entry);
		REQUIRE(chain.size() == 2);
		CHECK(chain[0].mountName.find("dup_a") != std::string::npos);
		CHECK(chain[1].mountName.find("dup_b") != std::string::npos);
		CHECK(chain[0].source.authorityRank == 0);
		CHECK(chain[1].source.authorityRank == 1);
		CHECK(mc2::vfs::contentDigest(entry) == digestOf("from-first-archive"));
		mc2::vfs::flushContentDigests();
		mc2::vfs::rebuild();
		CHECK(mc2::vfs::contentDigest(entry) == digestOf("from-first-archive"));
		FastFileFini();
	}
	// The persisted member memo must follow an archive replacement, even for the same logical member.
	authorFst(a, entry, "replacement-archive-content");
	fs::last_write_time(a, fs::last_write_time(a) + std::chrono::seconds(7));
	fastFiles = static_cast<FastFile**>(calloc(static_cast<size_t>(maxFastFiles), sizeof(FastFile*)));
	REQUIRE(FastFileInit(a.c_str()));
	mc2::vfs::rebuild();
	CHECK(mc2::vfs::contentDigest(entry) == digestOf("replacement-archive-content"));
	FastFileFini();
	mc2::vfs::rebuild();
	std::error_code ec;
	fs::remove(a, ec);
	fs::remove(b, ec);
}

} // TEST_SUITE
