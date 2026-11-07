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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc2::vfs {

enum class Provider { Mod, Loose, Fst };

const char* providerName(Provider p);

// Logical key (fst_hash.cpp normalization: lowercase, '\\' -> '/').
std::string key(std::string_view path);
// Value-type join of a directory and a leaf into one logical key (exactly one '/' between).
std::string join(std::string_view dir, std::string_view leaf);

struct Entry {
	std::string key;           // logical key
	Provider provider = Provider::Fst;
	std::string mountName;     // "fst:<archive>", "loose", or the mod id
	std::string physicalPath;  // case-preserved disk path; empty for FST entries
	std::string name;          // writable basename (physical case; key leaf for FST entries)
	unsigned shadowed = 0;     // lower-precedence candidates behind this one in the chain
	bool viaStripAlias = false; // served by the size-stripped alias (data/tgl/128/x.tga -> loose data/tgl/x.tga)
};

struct Stats {
	size_t entries = 0, mounts = 0, bytes = 0;
	double buildMs = 0;
};

// Explicit (re)build, e.g. after a mod reload or in tests.
void rebuild();
void invalidate();             // the next lookup/enumerate rebuilds
Stats stats();

// First candidate of the ladder: mod -> loose -> size-stripped alias -> FST (File::open order).
std::optional<Entry> lookup(std::string_view logicalKey);
// The whole ORDERED candidate chain for a key (winner first), including the size-stripped alias rung.
// A shadowed mod whose physical file cannot be derived is still listed (empty physicalPath).
std::vector<Entry> candidates(std::string_view logicalKey);
// First candidate that `readable` accepts (default: physical file opens; FST always accepted), so a corrupt or
// unreadable winner falls back to the next provider without changing precedence.
std::optional<Entry> resolve(std::string_view logicalKey, bool (*readable)(const Entry&) = nullptr);
// TODO(cook lane): the index identifies files by path only. Content identity needs real content hashes;
// do NOT build it on ComputeModDataSignature (file count + max mtime), a cache-staleness check.
// Direct children of dirKey ("data/missions/") whose name ends in ext (".fit", case-insensitive; empty = all),
// one entry per key, sorted by key.
std::vector<Entry> enumerate(std::string_view dirKey, std::string_view ext = {});

} // namespace mc2::vfs

// ---- file.cpp internals (the resolution ladder's mod layer and counters) ----
#include <optional>
inline constexpr unsigned long kMaxInRamImage = 1ul << 30;
inline bool imageSizePlausible(unsigned long size) { return size <= kMaxInRamImage; }
extern int logFileResolve;
extern unsigned long g_resolveOpens;
extern unsigned long g_resolveMisses;
std::string NormalizeKey(const char* p);
void EmitResolveRecord(const char* key, const char* req, const char* layer, const char* path,
                       const std::vector<std::string>* shadowed);
std::optional<int> TryModOpen(const char* fileName, const char* req = nullptr);
