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
#include <string>
#include <string_view>
#include <vector>

class FastFile;

namespace mc2::vfs {

enum class Provider { Mod, Loose, Fst };

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
};

struct Stats {
	size_t entries = 0, mounts = 0, bytes = 0;
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
// A physical winner is memoised by (path, size, mtime) and re-hashed on any change of either; an FST winner is
// memoised per index generation. The memo is cleared on every rebuild, which bounds a same-size same-mtime edit to
// the next rebuild. Hash cost: one read of the file per content change, one stat per query. Never derived from
// ComputeModDataSignature (file count + max mtime), which is a cache-staleness hint only.
uint64_t contentDigest(std::string_view path);
// Direct children of dirKey ("data/missions/") whose name ends in ext (".fit", case-insensitive; empty = all),
// one entry per key, sorted by key.
std::vector<Entry> enumerate(std::string_view dirKey, std::string_view ext = {});

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
	bool found() const { return fd != -1 || fst != nullptr; }
};
OpenedFile openRead(const char* path, const char* req = nullptr);
// A file created under data/ after the index was built joins the loose mount (writes stay physical).
void noteWrite(std::string_view path);

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
