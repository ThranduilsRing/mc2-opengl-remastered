#ifndef MOD_CACHE_SIG_H
#define MOD_CACHE_SIG_H
// MOD-INDEX-CACHE-MTIME-1 -- pure validity-signature kernel for the mod index
// disk cache (mods/<id>/.modindex-cache, mclib/file.cpp).
//
// The v1 cache invalidated on "cache mtime vs data/ dir mtimes (2 levels)".
// That misses (a) content edits to an EXISTING file (file mtime changes, dir
// mtime does not) and (b) files added/removed DEEPER than 2 levels (only the
// immediate parent dir mtime changes) -- both bit live: new equipment def
// files were invisible until the cache was hand-deleted (modder trap).
//
// v2 stores a cheap signature computed from ONE full recursive walk of the
// mod's data/ tree at index time:
//   fileCount = number of indexed (non-dot) files
//   maxMtime  = max last-write FILETIME (as uint64) over all non-dot files
//               AND directories (dirs catch renames: a rename keeps the file
//               mtime + count but bumps the parent dir mtime)
// On load the walk is recomputed and compared; any mismatch rebuilds. The
// walk is load-time-only and touches metadata, not content -- cheap.
//
// This header is pure/IO-free (doctest target): the sig-line parse/format and
// the match rule. The FindFirstFile walk lives in mclib/file.cpp.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

struct ModCacheSignature {
    unsigned long long fileCount = 0;
    unsigned long long maxMtime  = 0;   // FILETIME as uint64 (0 = none seen)
};

inline bool modCacheSignatureMatches(const ModCacheSignature& a,
                                     const ModCacheSignature& b) {
    return a.fileCount == b.fileCount && a.maxMtime == b.maxMtime;
}

// Cache line: "sig <fileCount> <maxMtime>" (both unsigned decimal).
// Returns no value on any malformation (wrong tag, missing/garbage numbers,
// trailing junk) -- caller treats that as a stale cache and rebuilds.
inline std::optional<ModCacheSignature> parseModCacheSigLine(const char* line) {
    if (!line) return std::nullopt;
    if (std::strncmp(line, "sig ", 4) != 0) return std::nullopt;
    const char* p = line + 4;
    char* end = nullptr;
    unsigned long long files = std::strtoull(p, &end, 10);
    if (end == p || *end != ' ') return std::nullopt;
    p = end + 1;
    unsigned long long mtime = std::strtoull(p, &end, 10);
    if (end == p || *end != '\0') return std::nullopt;
    return ModCacheSignature{files, mtime};
}

inline void formatModCacheSigLine(const ModCacheSignature& sig,
                                  char* buf, size_t bufLen) {
    std::snprintf(buf, bufLen, "sig %llu %llu", sig.fileCount, sig.maxMtime);
}

#endif // MOD_CACHE_SIG_H
