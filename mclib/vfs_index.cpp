// vfs_index.cpp -- the mc2::vfs mount table and merged index, split from vfs.cpp (file-size ratchet; pure move).
// The mod layer (InitModSearchPaths and its state) stays in vfs.cpp and is read through vfs_internal.h.
#include "vfs.h"
#include "file.h"
#include "ffile.h"
#include "fastfile.h"
#include <string.h>
#include "platform_io.h"
#include <ctype.h>
#include <errno.h>
#include "platform_windows.h"
#include <gameos.hpp>
#include "platform_str.h"
#include "fst_hash.h"
#include "mod_conflict.h"
#include "../GameOS/gameos/crash_content.h"
#include <atomic>
#include <unordered_map>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <cctype>
#include <string_view>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "Gates/render_options.h"
#include "../GameOS/gameos/utils/mc2log.h"
#include "mc2fmt.h"
#include "fast_hash.h"
#include "../GameOS/gameos/cook_cache.h"
#include <bit>
#include <filesystem>
#include <fstream>
#include <span>
#include "vfs_internal.h"
#include "pak_zip.h"

// ===========================================================================
// mc2::vfs -- mount table + ONE merged index (see vfs.h).
// Mount order (later overwrites earlier): base FST archives, loose data/, mods.
// The mod layer is already merged by InitModSearchPaths (g_modIndex = priority-resolved
// winners, g_shadowedBy = losers); the index folds it in as the last mounts.
// ===========================================================================
#include <map>
#include <mutex>
#include <chrono>
#include <set>

namespace mc2::vfs {
namespace {

struct Index {
	// key -> ORDERED candidate chain, winner first: mods (priority order), loose, FST archives (registration
	// order). Ordered map: prefix ranges and stable sort for free.
	std::map<std::string, std::vector<Entry>> entries;
	Stats st;
	bool built = false;
};
Index g_index;
std::mutex g_indexMutex;

// Content-digest memo: provider identity -> stat stamp + digest, persisted between runs.
struct DigestStamp { uint64_t size = 0; int64_t mtime = 0; };
std::mutex g_digestMutex;
std::map<std::string, std::pair<DigestStamp, uint64_t>> g_digestMemo;
bool g_digestLoaded = false, g_digestDirty = false;
constexpr mc2cache::CacheKind digestKind{"digests-", ".memo", 1, 16 * 1024 * 1024};
constexpr std::string_view digestDir = "cache/vfs";

// One bounded, checksummed snapshot. A partial or incompatible snapshot is a miss in its entirety.
void loadDigestsLocked()
{
	if (g_digestLoaded) return;
	g_digestLoaded = true;
	const auto bytes = mc2cache::readEntry(digestDir, digestKind, 1);
	if (!bytes || bytes->size() < 16) return;
	const std::span<const uint8_t> data(*bytes);
	if (mc2cache::getLE(data, 0, 8) != mc2::content::fastHash(data.subspan(8)) ||
	    mc2cache::getLE(data, 8, 4) != 1) return;
	size_t pos = 16;
	const auto count = mc2cache::getLE(data, 12, 4);
	decltype(g_digestMemo) loaded;
	for (uint64_t i = 0; i < count; ++i) {
		if (data.size() - pos < 28) return;
		const auto length = mc2cache::getLE(data, pos, 4);
		DigestStamp stamp;
		stamp.size = mc2cache::getLE(data, pos + 4, 8);
		stamp.mtime = std::bit_cast<int64_t>(mc2cache::getLE(data, pos + 12, 8));
		const auto digest = mc2cache::getLE(data, pos + 20, 8);
		pos += 28;
		if (!length || length > 8192 || length > data.size() - pos || !digest) return;
		std::string name(reinterpret_cast<const char*>(data.data() + pos), length);
		pos += length;
		loaded.emplace(std::move(name), std::pair{stamp, digest});
	}
	if (pos == data.size()) g_digestMemo = std::move(loaded);
}
static std::atomic<uint64_t> g_generation{1};

void put(Index& ix, const std::string& k, Provider p, const std::string& mount,
         const std::string& phys, const std::string& name)
{
	Entry e;
	e.key = k; e.provider = p; e.mountName = mount; e.physicalPath = phys; e.name = name;
	ix.entries[k].push_back(std::move(e));   // appended in precedence order within a provider class
}

// Numeric size-subdir strip, same rule as File::open: data/tgl/128/foo.tga -> data/tgl/foo.tga. Empty if none.
std::string stripSizeDir(const std::string& k)
{
	for (size_t p = 0; p < k.size(); ++p) {
		if (k[p] != '/') continue;
		size_t q = p + 1;
		while (q < k.size() && k[q] >= '0' && k[q] <= '9') ++q;
		if (q > p + 1 && q < k.size() && k[q] == '/') return k.substr(0, p + 1) + k.substr(q + 1);
	}
	return std::string();
}

// Recursive loose scan under dir (physical, trailing '/'); rel is the logical prefix ("data/").
void scanLoose(Index& ix, const std::string& dir, const std::string& rel, size_t& count)
{
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (fd.cFileName[0] == '.') continue;   // same dot rule as the mod scan (ruling C4)
		const std::string leaf = fd.cFileName;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			scanLoose(ix, dir + leaf + "/", rel + leaf + "/", count);
		} else {
			put(ix, key(rel + leaf), Provider::Loose, "loose", dir + leaf, leaf);
			++count;
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

size_t estimateBytes(const Index& ix)
{
	size_t b = 0;
	for (const auto& [k, chain] : ix.entries) {
		b += 48 /*map node*/ + k.capacity() + sizeof(chain) + chain.capacity() * sizeof(Entry);
		for (const Entry& e : chain)
			b += e.key.capacity() + e.mountName.capacity() + e.physicalPath.capacity() + e.name.capacity();
	}
	return b;
}

void buildLocked()
{
	const auto t0 = std::chrono::steady_clock::now();
	Index ix;
	size_t mounts = 0, pakEligible = 0;
	{
		std::lock_guard<std::mutex> dl(g_digestMutex);
		g_digestMemo.clear();
		g_digestLoaded = false;
		g_digestDirty = false;
	}
	g_generation.fetch_add(1, std::memory_order_relaxed);

	// 1. base FST archives, in registration order; the earlier archive wins (FastFileFind order),
	//    so the chain keeps registration order (earliest first).
	for (long a = 0; fastFiles && a < numFastFiles; ++a) {
		FastFile* ff = fastFiles[a];
		if (!ff) continue;
		const FILE_HANDLE* fh = ff->getFilesInfo();
		const long n = ff->getNumFiles();
		const char* an = ff->getFileName();
		const std::string mount = std::string("fst:") + (an ? an : "?");
		size_t cnt = 0;
		for (long i = 0; fh && i < n; ++i) {
			if (!fh[i].pfe) continue;
			const std::string k = key(fh[i].pfe->name);
			const size_t slash = k.rfind('/');
			put(ix, k, Provider::Fst, mount, std::string(), slash == std::string::npos ? k : k.substr(slash + 1));
			++cnt;
		}
		++mounts;
		mc2::fmt::print_out("[VFS] mount fst name={} files={:d} priority=0\n", mc2::fmt::safe(mount.c_str()), mc2::fmt::as_size(cnt));
	}

	// 2. loose data/
	{
		size_t cnt = 0;
		scanLoose(ix, "data/", "data/", cnt);
		++mounts;
		mc2::fmt::print_out("[VFS] mount loose name=loose files={:d} priority=1\n", mc2::fmt::as_size(cnt));
	}

	// 3. mods in load order (g_modIndex already holds the priority-resolved winner per key).
	{
		std::map<std::string, size_t> perMod;
		for (const auto& [k, me] : internal::modIndex()) {
			const size_t slash = me.path.find_last_of("/\\");
			put(ix, k, Provider::Mod, me.modId, me.path,
			    slash == std::string::npos ? me.path : me.path.substr(slash + 1));
			++perMod[me.modId];
			// Shadowed mods stay in the chain behind the winner, in priority order (lower value wins), so a
			// corrupt winner can fall back to the next mod without breaking precedence.
			auto sh = internal::modShadowedBy().find(k);
			if (sh != internal::modShadowedBy().end()) {
				const auto& [shadowedKey, shadowedIds] = *sh;
				std::vector<std::string> ids = shadowedIds;
				auto prio = [](const std::string& id) {
					auto pr = internal::modPriority().find(id.substr(0, id.rfind("#scratch")));
					if (pr == internal::modPriority().end()) return 0;
					const auto& [modId, priority] = *pr;
					return priority;
				};
				std::stable_sort(ids.begin(), ids.end(), [&](const std::string& a, const std::string& b) { return prio(a) < prio(b); });
				for (const std::string& id : ids) {
					const size_t h = id.rfind("#scratch");
					const std::string phys = internal::modsAbsRoot().empty() ? std::string()
						: (h != std::string::npos ? internal::modsAbsRoot() + id.substr(0, h) + "/.scratch/" + k
						                          : internal::modsAbsRoot() + id + "/" + k);
					const size_t ks = k.rfind('/');
					put(ix, k, Provider::Mod, id, phys, ks == std::string::npos ? k : k.substr(ks + 1));
				}
			}
		}
		for (const auto& [id, cnt] : perMod) {
			++mounts;
			auto pr = internal::modPriority().find(id);
			int priority = 0;
			if (pr != internal::modPriority().end()) {
				const auto& [modId, value] = *pr;
				priority = value;
			}
			mc2::fmt::print_out("[VFS] mount mod name={} files={:d} priority={:d}\n", mc2::fmt::safe(id.c_str()),
			                    mc2::fmt::as_size(cnt), priority);
		}
	}

	// Precedence: mod, then loose, then FST. Stable: within a class the insertion order (mod priority,
	// FST registration order) is kept. Then record how many candidates sit behind each one.
	for (auto& [k, chain] : ix.entries) {
		std::stable_sort(chain.begin(), chain.end(),
		                 [](const Entry& a, const Entry& b) { return static_cast<int>(a.provider) < static_cast<int>(b.provider); });
		chain.shrink_to_fit();
		for (size_t i = 0; i < chain.size(); ++i) {
			chain[i].shadowed = static_cast<unsigned>(chain.size() - 1 - i);
			chain[i].source.authorityRank = static_cast<unsigned>(i);
		}
	}
	// A representation replaces its source at that archive's rank. In particular a matching
	// later source must never jump over an earlier FST that has since gained the same key.
	std::error_code pakEc;
	if (mc2::gate::get<"MC2_PAK">() && std::filesystem::is_regular_file("paks/stock.mc2pak", pakEc)) {
		auto pak = std::make_shared<PakZip>("paks/stock.mc2pak");
		if (pak->valid()) {
			std::map<std::string, bool> eligible;
			for (const auto& src : pak->sources()) {
				bool matches = true;
				for (long a = 0; fastFiles && a < numFastFiles; ++a) {
					auto* ff = fastFiles[a];
					if (!ff || key(std::filesystem::path(ff->getFileName()).filename().string()) != src.name) continue;
					std::error_code presentEc;
					if (!std::filesystem::is_regular_file(ff->getFileName(), presentEc)) continue; // registered but missing on disk: absent, not modified
					std::ifstream in(ff->getFileName(), std::ios::binary); // path-audit: loose-only (root FST container TOC hash)
					const auto len = uint64_t{8} + static_cast<uint64_t>(ff->getNumFiles()) * 266;
					std::vector<uint8_t> toc(static_cast<size_t>(len));
					in.read(reinterpret_cast<char*>(toc.data()), static_cast<std::streamsize>(len));
					const auto hash = std::format("{:016x}", mc2::content::fastHash(toc));
					matches = matches && in.good() && static_cast<uint64_t>(ff->fileSize()) == src.size && src.tocHash == hash;
				}
				eligible[src.name] = matches;
				if (!matches) {
					size_t skipped = 0;
					for (const auto& [k, e] : pak->entries()) if (e.source == src.name) ++skipped;
					mc2::fmt::print_out("[PAK] source={} state=modified skipped={}\n", src.name, skipped);
				}
			}
			size_t count = 0;
			// Per-entry path normalization over every FST was ~65k filesystem::path + key() calls at mount.
			std::set<std::string> registeredNames;
			for (long a = 0; fastFiles && a < numFastFiles; ++a)
				if (fastFiles[a]) registeredNames.insert(key(std::filesystem::path(fastFiles[a]->getFileName()).filename().string()));
			std::map<std::string, std::string> mountSource; // FST mount name -> normalized file name
			for (const auto& [k, pe] : pak->entries()) {
				if (k == "mc2pak.json" || !eligible[pe.source]) continue;
				auto& chain = ix.entries[k];
				auto pos = chain.end();
				const bool registered = registeredNames.contains(pe.source);
				for (auto it = chain.begin(); it != chain.end(); ++it) {
					if (it->provider != Provider::Fst) continue;
					if (!registered) { pos = it; break; }
					auto& srcName = mountSource[it->mountName];
					if (srcName.empty()) srcName = key(std::filesystem::path(it->mountName.substr(4)).filename().string());
					if (srcName == pe.source) { pos = it; break; }
				}
				// Absent sources are below every currently registered FST: absence cannot promote a representation.
				if (!registered) pos = chain.end();
				Entry e; e.key = k; e.provider = Provider::Pak; e.mountName = "pak";
				e.name = k.substr(k.find_last_of('/') + 1); e.pak = pak;
				chain.insert(pos, std::move(e)); ++count;
			}
			for (auto& [k, chain] : ix.entries) for (size_t i = 0; i < chain.size(); ++i) {
				chain[i].source.authorityRank = static_cast<unsigned>(i);
				chain[i].shadowed = static_cast<unsigned>(chain.size() - i - 1);
			}
			++mounts; pakEligible += count;
			mc2::fmt::print_out("[VFS] mount pak name=paks/stock.mc2pak files={} eligible={}\n", pak->entries().size() - 1, count);
		}
	}
	// A campaign mod that ships data/campaign/campaign.fit wins that key, which hid the stock campaign from the
	// campaign list. Keep the stock one reachable under its own key: the chain minus the mod entries. The copies
	// keep the real key (FST and pak members are found by it); only the leaf name differs.
	if (const auto it = ix.entries.find("data/campaign/campaign.fit"); it != ix.entries.end()) {
		const auto& [campaignKey, campaignChain] = *it;
		std::vector<Entry> base;
		if (campaignChain.front().provider == Provider::Mod) {
			for (const Entry& e : campaignChain)
				if (e.provider != Provider::Mod) base.push_back(e);
		}
		if (!base.empty()) {
			for (size_t i = 0; i < base.size(); ++i) {
				base[i].name = std::string(kStockCampaignKey.substr(kStockCampaignKey.rfind('/') + 1));
				base[i].shadowed = static_cast<unsigned>(base.size() - 1 - i);
				base[i].source.authorityRank = static_cast<unsigned>(i);
			}
			ix.entries[std::string(kStockCampaignKey)] = std::move(base);
		}
	}
	ix.st.entries = ix.entries.size();
	ix.st.mounts = mounts;
	ix.st.pakEligible = pakEligible;
	ix.st.bytes = estimateBytes(ix);
	ix.st.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	ix.built = true;
	mc2::fmt::print_out("[VFS] index entries={:d} mounts={:d} bytes={:d} build_ms={:.1f}\n",
	                    mc2::fmt::as_size(ix.st.entries), mc2::fmt::as_size(ix.st.mounts),
	                    mc2::fmt::as_size(ix.st.bytes), ix.st.buildMs);
	fflush(stdout);
	g_index = std::move(ix);
}

} // namespace

const char* providerName(Provider p)
{
	switch (p) { case Provider::Mod: return "mod"; case Provider::Loose: return "loose"; case Provider::Pak: return "pak"; default: return "fst"; }
}

std::string key(std::string_view path)
{
	return NormalizeKey(path);
}

std::string join(std::string_view dir, std::string_view leaf)
{
	std::string d(dir);
	while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
	size_t b = 0;
	while (b < leaf.size() && (leaf[b] == '/' || leaf[b] == '\\')) ++b;
	return key(d.empty() ? std::string(leaf.substr(b)) : d + "/" + std::string(leaf.substr(b)));
}

void rebuild()
{
	std::lock_guard<std::mutex> lk(g_indexMutex);
	buildLocked();
}

uint64_t generation() { return g_generation.load(std::memory_order_relaxed); }

void invalidate()
{
	std::lock_guard<std::mutex> lk(g_indexMutex);
	g_index.built = false;
	g_generation.fetch_add(1, std::memory_order_relaxed);
}

Stats stats()
{
	std::lock_guard<std::mutex> lk(g_indexMutex);
	if (!g_index.built) buildLocked();
	return g_index.st;
}

std::vector<Entry> candidates(std::string_view logicalKey)
{
	// Same guard as the mod layer: absolute paths and '..' never resolve.
	std::vector<Entry> out;
	if (internal::isAbsolutePath(logicalKey) || logicalKey.find("..") != std::string_view::npos ||
		logicalKey.find('\0') != std::string_view::npos) return out;
	// File::open runs on worker threads too: the chain is copied out under the lock, so a rebuild or noteWrite
	// on another thread never invalidates it.
	std::lock_guard<std::mutex> lk(g_indexMutex);
	if (!g_index.built) buildLocked();
	const std::string k = key(logicalKey);
	const auto it = g_index.entries.find(k);
	if (it != g_index.entries.end()) {
		const auto& [entryKey, entries] = *it;
		out = entries;
	}

	// Alias rung, File::open order: mod -> loose -> size-stripped loose -> FST. Only a loose file can serve the strip.
	const std::string stripped = stripSizeDir(k);
	if (!stripped.empty()) {
		const auto sit = g_index.entries.find(stripped);
		if (sit != g_index.entries.end()) {
			const auto& [entryKey, entries] = *sit;
			for (const Entry& c : entries)
				if (c.provider == Provider::Loose) {
					Entry a = c;
					a.key = k;
					a.viaStripAlias = true;
					size_t pos = 0;
					while (pos < out.size() && out[pos].provider != Provider::Fst && out[pos].provider != Provider::Pak) ++pos;
					out.insert(out.begin() + static_cast<std::ptrdiff_t>(pos), std::move(a));
					break;
				}
		}
	}
	out.erase(std::remove_if(out.begin(), out.end(), [](const Entry& e) {
		return e.provider == Provider::Pak && !e.pak->find(e.key);
	}), out.end());
	// Last rung (after the stale-pak erase): weapon/FX art that a campaign mod (MCO) ships only under a larger
	// data/tgl/<N>/ size dir than the requested one (ObjectTextureSize). Only Mod/Loose can serve it: an FST member is
	// opened by the requested path and a pak member by the requested key, so those stay ghosts and are not carried.
	if (out.empty() && k.rfind("data/tgl/", 0) == 0) {
		size_t q = 9;
		while (q < k.size() && k[q] >= '0' && k[q] <= '9') ++q;
		if (q > 9 && q < k.size() && k[q] == '/' && q - 9 <= 5) {
			const unsigned want = static_cast<unsigned>(std::stoul(k.substr(9, q - 9)));
			for (const unsigned sz : {64u, 128u, 256u, 512u, 1024u, 2048u, 4096u, 8192u}) {   // nearest larger size first
				if (sz <= want) continue;
				const auto hit = g_index.entries.find("data/tgl/" + std::to_string(sz) + k.substr(q));
				if (hit == g_index.entries.end()) continue;
				const auto& [hitKey, hitEntries] = *hit;
				for (const Entry& c : hitEntries) {
					if (c.provider != Provider::Mod && c.provider != Provider::Loose) continue;
					Entry a = c;
					a.key = k;
					out.push_back(std::move(a));
				}
				if (!out.empty()) break;
			}
		}
	}
	for (size_t i = 0; i < out.size(); ++i) {
		out[i].shadowed = static_cast<unsigned>(out.size() - 1 - i);
		out[i].source.authorityRank = static_cast<unsigned>(i);
	}
	return out;
}

void flushContentDigests()
{
	std::lock_guard<std::mutex> dl(g_digestMutex);
	if (!g_digestDirty) return;
	std::vector<uint8_t> bytes;
	mc2cache::putLE(bytes, 0, 8);
	mc2cache::putLE(bytes, 1, 4);
	mc2cache::putLE(bytes, g_digestMemo.size(), 4);
	for (const auto& [name, memo] : g_digestMemo) {
		const auto& [stamp, digest] = memo;
		mc2cache::putLE(bytes, name.size(), 4);
		mc2cache::putLE(bytes, stamp.size, 8);
		mc2cache::putLE(bytes, static_cast<uint64_t>(stamp.mtime), 8);
		mc2cache::putLE(bytes, digest, 8);
		bytes.insert(bytes.end(), name.begin(), name.end());
	}
	const auto hash = mc2::content::fastHash(std::span<const uint8_t>(bytes).subspan(8));
	for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(hash >> (8 * i));
	if (bytes.size() <= digestKind.maxBytes && mc2cache::writeEntry(digestDir, digestKind, 1, bytes))
		g_digestDirty = false;
}

uint64_t contentDigest(std::string_view path, bool verify)
{
	if (path.empty()) return 0;
	const std::string k = key(path);
	// Follow provider precedence with stat probes; memo hits must not open the source.
	std::string physical;
	bool fst = false;
	std::string archive;
	if (const auto e = resolve(k, [](const Entry& candidate) {
		if (candidate.provider == Provider::Fst || candidate.provider == Provider::Pak) return true;
		std::error_code ec;
		return std::filesystem::is_regular_file(candidate.physicalPath, ec) && std::ifstream(candidate.physicalPath, std::ios::binary).is_open(); // path-audit: loose-only; probe the normalized ladder candidate's physical file.
	})) {
		if (e->provider == Provider::Pak) {
			// Memoized by the pak file stamp; a miss hashes the actual served bytes (fallback after a corrupt entry included).
			constexpr const char* pakFile = "paks/stock.mc2pak";
			std::error_code pec;
			DigestStamp pstamp;
			pstamp.size = std::filesystem::file_size(pakFile, pec);
			if (pec) return 0;
			const auto pmt = std::filesystem::last_write_time(pakFile, pec);
			if (pec) return 0;
			pstamp.mtime = static_cast<int64_t>(pmt.time_since_epoch().count());
			const std::string pakMemoKey = "pak:" + std::filesystem::absolute(pakFile, pec).lexically_normal().generic_string() + "\n" + k;
			{
				std::lock_guard<std::mutex> dl(g_digestMutex);
				loadDigestsLocked();
				const auto it = g_digestMemo.find(pakMemoKey);
				if (!verify && it != g_digestMemo.end()) {
					const auto& [memoKey, memo] = *it;
					const auto& [memoStamp, memoDigest] = memo;
					(void)memoKey;
					if (memoStamp.size == pstamp.size && memoStamp.mtime == pstamp.mtime) return memoDigest;
				}
			}
			File input;
			if (input.open(std::string(path).c_str()) != NO_ERR) return 0;
			std::vector<uint8_t> bytes(input.fileSize());
			if (input.read(bytes.data(), static_cast<long>(bytes.size())) != static_cast<long>(bytes.size())) return 0;
			uint64_t digest = mc2::content::fastHash(bytes);
			if (!digest) digest = 1;
			std::lock_guard<std::mutex> dl(g_digestMutex);
			g_digestMemo[pakMemoKey] = {pstamp, digest};
			g_digestDirty = true;
			return digest;
		}
		if (e->provider == Provider::Fst) {
			fst = true;
			archive = e->mountName.substr(4);
		} else physical = e->physicalPath;
	} else {
		physical = std::string(path);   // not indexed (absolute path or written after the build): the file itself
	}
	DigestStamp stamp;
	{
		std::error_code ec;
		const auto source = fst ? archive : physical;
		const auto size = std::filesystem::file_size(source, ec);
		if (ec) return 0;
		const auto mt = std::filesystem::last_write_time(source, ec);
		if (ec) return 0;
		stamp.size = size;
		stamp.mtime = static_cast<int64_t>(mt.time_since_epoch().count());
	}
	std::error_code pathError;
	const auto source = std::filesystem::absolute(fst ? archive : physical, pathError).lexically_normal().generic_string();
	if (pathError) return 0;
	const std::string memoKey = fst ? "fst:" + source + "\n" + k : "file:" + source;
	{
		std::lock_guard<std::mutex> dl(g_digestMutex);
		loadDigestsLocked();
		const auto it = g_digestMemo.find(memoKey);
		if (!verify && it != g_digestMemo.end()) {
			const auto& [storedKey, memo] = *it;
			const auto& [memoStamp, memoDigest] = memo;
			(void)storedKey;
			if (memoStamp.size == stamp.size && memoStamp.mtime == stamp.mtime) return memoDigest;
		}
	}
	// Miss: hash the content once.
	uint64_t digest = 14695981039346656037ull;
	std::vector<uint8_t> chunk(64 * 1024);
	if (fst) {
		File input;
		if (input.open(k.c_str()) != NO_ERR) return 0;
		unsigned long left = input.fileSize();
		while (left) {
			const auto count = std::min<unsigned long>(left, static_cast<unsigned long>(chunk.size()));
			if (input.read(chunk.data(), static_cast<long>(count)) != static_cast<long>(count)) return 0;
			digest = mc2::content::fastHash(std::span<const uint8_t>(chunk.data(), count), digest);
			left -= count;
		}
	} else {
		std::ifstream in(physical, std::ios::binary);
		if (!in) return 0;
		uint64_t total = 0;
		while (in) {
			in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
			const auto got = static_cast<size_t>(in.gcount());
			if (!got) break;
			digest = mc2::content::fastHash(std::span<const uint8_t>(chunk.data(), got), digest);
			total += got;
		}
		if (total != stamp.size) return 0;   // changed while hashing: no memo, the caller retries
	}
	std::error_code stampError;
	const auto finalTime = std::filesystem::last_write_time(fst ? archive : physical, stampError);
	if (stampError || static_cast<int64_t>(finalTime.time_since_epoch().count()) != stamp.mtime) return 0;
	if (!digest) digest = 1;
	std::lock_guard<std::mutex> dl(g_digestMutex);
	g_digestMemo[memoKey] = {stamp, digest};
	g_digestDirty = true;
	return digest;
}

std::optional<Entry> lookup(std::string_view logicalKey)
{
	std::vector<Entry> c = candidates(logicalKey);
	if (c.empty()) return std::nullopt;
	return std::move(c.front());
}

std::optional<Entry> resolve(std::string_view logicalKey, bool (*readable)(const Entry&))
{
	for (Entry& e : candidates(logicalKey)) {
		bool ok = true;
		if (readable) ok = readable(e);
		else if (e.provider == Provider::Pak) ok = e.pak->find(e.key) != nullptr;
		else if (e.provider != Provider::Fst) {
			FILE* fp = e.physicalPath.empty() ? nullptr : fopen(e.physicalPath.c_str(), "rb"); // path-audit: loose-only; ladder probes its normalized candidate's physical file.
			ok = fp != nullptr;
			if (fp) fclose(fp);
		}
		if (ok) return std::move(e);
	}
	return std::nullopt;
}

OpenedFile openRead(const char* path, const char* req, bool probeOnly)
{
	OpenedFile r;
	if (!path || !path[0]) return r;
	const std::string k = key(path);
	auto openPhysical = [&](const std::string& phys, const char* layer, const std::string& owner) {
		const int h = phys.empty() ? -1 : _open(phys.c_str(), _O_RDONLY | _O_BINARY); // path-audit: loose-only; ladder opens the selected mod/loose physical file.
		if (h == -1) return false;
		r.fd = h; r.layer = layer; r.owner = owner; r.physicalPath = phys;
		return true;
	};
	// A physical loose probe, File::open's historical order: the path itself, then the size-stripped alias.
	auto looseProbe = [&] {
		if (openPhysical(path, "base-loose", "loose")) return true;
		const std::string stripped = stripSizeDir(path);
		return openPhysical(stripped, "base-strip", "loose");
	};
	// The loose mount indexes data/ only; any other relative path is probed on disk before the index (old order).
	const bool looseIndexed = internal::shouldSearchMods(path);
	bool done = !looseIndexed && looseProbe();
	std::vector<std::string> shadowedMods;
	if (!done) {
		const std::vector<Entry> chain = candidates(path);
		for (size_t i = 0; i < chain.size() && !done; ++i) {
			const Entry& e = chain[i];
			if (e.provider == Provider::Pak) {
				if (probeOnly && e.pak->find(e.key)) { r.layer = r.owner = "pak"; done = true; continue; }
				if (const auto* pe = e.pak->find(e.key)) r.pakBytes = e.pak->read(*pe);
				if (r.pakBytes) { r.layer = r.owner = "pak"; done = true; }
				continue;
			}
			if (e.provider == Provider::Fst) {
				long member = -1;
				if (FastFile* ff = FastFileFind(e.key == k ? path : e.key.c_str(), member)) {
					r.fst = ff; r.fstMember = member; r.layer = "fastfile"; r.owner = "fst";
					done = true;
				}
				continue;
			}
			if (e.provider == Provider::Mod) {
				done = openPhysical(e.physicalPath, "mod", e.mountName);
				if (done)
					for (size_t j = i + 1; j < chain.size(); ++j)
						if (chain[j].provider == Provider::Mod) shadowedMods.push_back(chain[j].mountName);
				continue;
			}
			done = openPhysical(e.physicalPath, e.viaStripAlias ? "base-strip" : "base-loose", "loose");
		}
	}
	// Index miss: only a file written after the index was built can still be on disk.
	if (!done && looseIndexed) done = looseProbe();
	if (!done) return r;
	if (r.layer == "mod") {
		if (logFileResolve) {
			mc2::fmt::print_out("[mod-hit] [{}] {} -> {}\n", r.owner.c_str(), k.c_str(), r.physicalPath.c_str());
			fflush(stdout);
		}
		if (logFileResolve >= 2)
			EmitResolveRecord(k.c_str(), req ? req : path, ("mod:" + r.owner).c_str(), r.physicalPath.c_str(), &shadowedMods);
	} else if (logFileResolve >= 2) {
		EmitResolveRecord(k.c_str(), req ? req : path, r.layer.c_str(), r.physicalPath.c_str(), nullptr);
	}
	return r;
}

void noteWrite(std::string_view path)
{
	if (!internal::shouldSearchMods(path)) return;   // only the loose data/ mount is indexed
	const std::string raw(path);          // owned physical path for the index and CRT probes
	std::lock_guard<std::mutex> lk(g_indexMutex);
	if (!g_index.built) return;                    // the next build scans it
	const std::string k = key(raw);
	std::vector<Entry>& chain = g_index.entries[k];
	for (const Entry& e : chain)
		if (e.provider == Provider::Loose) return;
	Entry e;
	const size_t slash = raw.find_last_of("/\\");
	e.key = k; e.provider = Provider::Loose; e.mountName = "loose"; e.physicalPath = raw;
	e.name = slash == std::string::npos ? raw : raw.substr(slash + 1);
	size_t pos = 0;
	while (pos < chain.size() && chain[pos].provider == Provider::Mod) ++pos;
	chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(pos), std::move(e));
	for (size_t i = 0; i < chain.size(); ++i) chain[i].shadowed = static_cast<unsigned>(chain.size() - 1 - i);
	g_index.st.entries = g_index.entries.size();
}

static std::mutex g_writeGuardMutex;
static std::set<std::string> g_writeGuardSeen;

void resetWriteGuard()
{
	std::lock_guard<std::mutex> lk(g_writeGuardMutex);
	g_writeGuardSeen.clear();
}

bool guardWrite(std::string_view path)
{
	if (!internal::shouldSearchMods(path)) return false;   // only the relative data/ tree is stock data
	const std::string k = key(path);
	if (k.compare(0, 13, "data/savegame") == 0) return false;
	const std::string mod = mc2::gate::str<"MC2_ACTIVE_MOD">();
	if (mod.empty() || mod == "mc2-industrial-kit" || mod == "mc2-vehicle-kit") return false;   // same "no user layer" rule as the mount
	{
		std::lock_guard<std::mutex> lk(g_writeGuardMutex);
		if (g_writeGuardSeen.size() >= 64) return false;
		const auto [pos, added] = g_writeGuardSeen.insert(k);
		(void)pos;
		if (!added) return false;
	}
	mc2::fmt::print_out("[MOD_WRITE_GUARD] path={} mod={} action=warn\n", k.c_str(), mod.c_str());
	fflush(stdout);
	return true;
}

std::vector<Entry> enumerate(std::string_view dirKey, std::string_view ext)
{
	std::lock_guard<std::mutex> lk(g_indexMutex);
	if (!g_index.built) buildLocked();
	std::string dir = key(dirKey);
	if (!dir.empty() && dir.back() != '/') dir += '/';
	const std::string e = key(ext);
	std::vector<Entry> out;
	for (auto it = g_index.entries.lower_bound(dir); it != g_index.entries.end(); ++it) {
		const auto& [k, entries] = *it;
		if (k.compare(0, dir.size(), dir) != 0) break;
		if (k.find('/', dir.size()) != std::string::npos) continue;   // direct children only
		if (!e.empty() && (k.size() < dir.size() + e.size() || k.compare(k.size() - e.size(), e.size(), e) != 0)) continue;
		out.push_back(entries.front());   // the winner; one entry per key
	}
	return out;   // std::map order == sorted by key
}

} // namespace mc2::vfs
