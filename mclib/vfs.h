// mclib/vfs.h -- the mod/loose/FST virtual file system module (was mclib/file_mod.inc).
//
// Mount table + ONE merged in-memory index (UE pak mounting / PhysFS model):
//   mounts, in fixed order: base FST archives, then loose data/, then mods in load order.
//   index: normalized logical key -> {mount, physical path (case preserved) or FST entry}.
//   A later mount overwrites an earlier entry, so provider precedence (mod > loose > FST)
//   is resolved once at build time; shadowed entries are counted for diagnostics.
// lookup / enumerate read the index. The index is built lazily on first use and again after
// InitModSearchPaths / FastFileInit / FastFileFini (or an explicit rebuild()); never per frame.
// Writes (saves, prefs, transcripts, crash bundles) are NOT in the index: they stay physical.
// Keys are logical (lowercase, '/'); physical paths keep their on-disk case.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <map>

class FastFile;

namespace mc2::vfs {

enum class Provider { Mod, Loose, Pak, Fst };
class PakZip;

const char* providerName(Provider p);

// Logical key (fst_hash.cpp normalization: lowercase, '\\' -> '/').
std::string key(std::string_view path);
// Value-type join of a directory and a leaf into one logical key (exactly one '/' between).
std::string join(std::string_view dir, std::string_view leaf);

// Where a candidate's bytes come from and how it ranks. M1 of the container plan (rev 4): authority is the
// candidate's position in the resolution chain (mod priority order, loose, size-strip alias, then FST archives in
// REGISTRATION order). Storage and provenance never reorder it: a later archive does not outrank an earlier one
// because it is user-modified; provenance only decides whether a cooked copy of this source may be served.
struct SourceRecord {
	unsigned authorityRank = 0;  // 0 = the winner; strictly increasing down the chain
};

struct Entry {
	std::string key;           // logical key
	Provider provider = Provider::Fst;
	std::string mountName;     // "fst:<archive>", "loose", or the mod id
	std::string physicalPath;  // case-preserved disk path; empty for FST entries
	std::string name;          // writable basename (physical case; key leaf for FST entries)
	unsigned shadowed = 0;     // lower-precedence candidates behind this one in the chain
	bool viaStripAlias = false; // served by the size-stripped alias (data/tgl/128/x.tga -> loose data/tgl/x.tga)
	SourceRecord source;        // authority of this candidate (equals its index in candidates())
	std::shared_ptr<PakZip> pak;
};

struct Stats {
	size_t entries = 0, mounts = 0, bytes = 0;
	size_t pakEligible = 0;    // pak entries that won a chain slot (0 = no pak mounted or none eligible)
	double buildMs = 0;
};

// Explicit (re)build, e.g. after a mod reload or in tests.
void rebuild();
void invalidate();             // the next lookup/enumerate rebuilds
// Bumped by every invalidate()/rebuild(); callers that cache a rank derived from the index drop the cache when it changes.
uint64_t generation();
Stats stats();

// First candidate of the ladder: mod -> loose -> size-stripped alias -> FST (File::open order).
std::optional<Entry> lookup(std::string_view logicalKey);
// The whole ORDERED candidate chain for a key (winner first), including the size-stripped alias rung.
// A shadowed mod whose physical file cannot be derived is still listed (empty physicalPath).
std::vector<Entry> candidates(std::string_view logicalKey);
// First candidate that `readable` accepts (default: physical file opens; FST always accepted), so a corrupt or
// unreadable winner falls back to the next provider without changing precedence.
std::optional<Entry> resolve(std::string_view logicalKey, bool (*readable)(const Entry&) = nullptr);
// Content identity: the fastHash of the bytes the winning candidate serves for `path` (0 if unreadable).
// Persisted by (absolute path, size, mtime), or (archive path/size/mtime, member) for FSTs.
// verify bypasses both memos, including a same-size same-mtime edit. Rebuild drops the in-memory snapshot.
// Hash cost: one read per stamp miss, stats only on a hit. Never derived from
// ComputeModDataSignature (file count + max mtime), which is a cache-staleness hint only.
uint64_t contentDigest(std::string_view path, bool verify = false);
void flushContentDigests(); // atomic snapshot beside the cook caches; failure only costs another hash next run
// Direct children of dirKey ("data/missions/") whose name ends in ext (".fit", case-insensitive; empty = all),
// one entry per key, sorted by key.
std::vector<Entry> enumerate(std::string_view dirKey, std::string_view ext = {}, bool recursive = false);
// In-memory match cache. A missing owned key is absent, even if installed locally.
void setMatchData(std::map<std::string, std::vector<uint8_t>> files, bool (*owns)(std::string_view));
void clearMatchData();
bool matchDataActive();

// Alias key of the base-game campaign.fit while a mod's campaign.fit shadows it (absent otherwise); the campaign list shows it first.
inline constexpr std::string_view kStockCampaignKey = "data/campaign/stock_campaign.fit";

// The read-open behind File::open and fileExists. The merged index decides (mod > loose > size-stripped loose > FST,
// the first candidate that opens wins); the disk is probed only on an index miss (a file written after the build)
// and, before the index, for relative paths outside data/ (the loose mount indexes data/ only).
struct OpenedFile {
	int fd = -1;                    // mod / loose hit: an open read-only binary descriptor the caller owns
	::FastFile* fst = nullptr;      // FST hit: the archive and its member (FastFileFind semantics)
	long fstMember = -1;
	std::string layer;              // "mod", "base-loose", "base-strip", "fastfile"; empty = miss
	std::string owner;              // mod id, "loose" or "fst" (crash-bundle attribution)
	std::string physicalPath;
	std::optional<std::vector<uint8_t>> pakBytes;
	bool found() const { return fd != -1 || fst != nullptr || pakBytes.has_value(); }
};
// probeOnly reports an indexed pak hit through layer without decoding its bytes.
OpenedFile openRead(const char* path, const char* req = nullptr, bool probeOnly = false);
// A file created under data/ after the index was built joins the loose mount (writes stay physical).
void noteWrite(std::string_view path);
// Write guard (mod VFS design section 6.3, always on): a CREATE under the stock data/ tree while MC2_ACTIVE_MOD is set logs one
// `[MOD_WRITE_GUARD] path=... mod=... action=warn` line per path (at most 64 paths). The design defines no engine redirect into
// the scratch layer, so the write itself is never changed. data/savegame/ is the known runtime save path and is exempt.
// Returns true when a line was logged; with no active mod it does nothing.
bool guardWrite(std::string_view path);
// Test hook: forget the paths already reported.
void resetWriteGuard();

} // namespace mc2::vfs

// ---- file.cpp internals (the resolution ladder's mod layer and counters) ----
#include <optional>
inline constexpr unsigned long kMaxInRamImage = 1ul << 30;
inline bool imageSizePlausible(unsigned long size) { return size <= kMaxInRamImage; }
extern int logFileResolve;
extern unsigned long g_resolveOpens;
extern unsigned long g_resolveMisses;
std::string NormalizeKey(std::string_view p);
void EmitResolveRecord(const char* key, const char* req, const char* layer, const char* path,
                       const std::vector<std::string>* shadowed);
