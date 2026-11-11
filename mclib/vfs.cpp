//---------------------------------------------------------------------------
// Mod overlay — session-scoped, single active mod.
//
// Selected by launcher via MC2_ACTIVE_MOD env var (set before spawning mc2.exe).
// When unset: base game mode — g_modIndex empty, zero overhead, mods/ ignored.
// When set:   active mod + declared dependencies indexed into g_modIndex.
//
// Priority: active mod > dependency N..0 > base data/ > FastFiles > CD
// File lookup: O(1) hash, no per-file disk probing.
// Diagnostics: MC2_LOG_FILE_RESOLVE=1 (level-1 tags) or =2 (level-1 + JSONL trace)

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
#include <filesystem>
#include <fstream>
#include <span>
#include "vfs_internal.h"


// ModFileEntry lives in vfs_internal.h (shared with vfs_index.cpp).

static std::unordered_map<std::string, ModFileEntry> g_modIndex;
// Shadowed-by map: key -> list of mod ids that were overridden at index time.
// Built alongside g_modIndex in IndexModData; queried at level-2 emit.
static std::unordered_map<std::string, std::vector<std::string>> g_shadowedBy;
static std::string g_activeModId;
static std::string g_modsAbsRoot;   // absolute mods root with trailing '/', set by InitModSearchPaths
static std::unordered_map<std::string, int> g_modPriority;
// mod.json "capabilities" per mounted mod id (active + deps). A package capability switches on the
// engine paths that package needs (imported mechs, package chassis, equipment ADD) for that package only.
static std::unordered_map<std::string, std::vector<std::string>> g_modCapabilities;
// logFileResolve: 0=off, 1=level-1 stdout tags, 2=level-1+JSONL trace
int logFileResolve = 0;

// META-FIX design E: resolve counters for the level>=1 shutdown summary
// ("[file-resolve] opens=N misses=M"). Unconditional integer bumps (no
// output unless the gate is on); opens = successful File::open resolutions,
// misses = full-ladder misses from File::open AND fileExists probes.
unsigned long g_resolveOpens = 0;
unsigned long g_resolveMisses = 0;

// Level-2 JSONL trace state (all guarded by logFileResolve >= 2).
static FILE* g_traceFile = nullptr;
static LARGE_INTEGER g_startQPC;   // QueryPerformanceCounter at init
static LARGE_INTEGER g_qpcFreq;    // QueryPerformanceFrequency result

// Return elapsed seconds since InitModSearchPaths, using QPC.
// Returns 0.0 when QPC unavailable (should never happen on Win Vista+).
static double TraceElapsedSec() {
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now)) return 0.0;
    if (g_qpcFreq.QuadPart == 0) return 0.0;
    return static_cast<double>(now.QuadPart - g_startQPC.QuadPart) /
           static_cast<double>(g_qpcFreq.QuadPart);
}

// JSON-escape a string into buf (NUL-terminated).  buf must be at least 3×srcLen+3.
// Only escapes the characters required by RFC 8259 (backslash, quote, control chars).
static void JsonEscape(const char* src, char* buf, size_t bufSize) {
    size_t out = 0;
    buf[out++] = '"';
    for (const char* p = src; *p && out + 4 < bufSize; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c == '\\') { buf[out++] = '\\'; buf[out++] = '\\'; }
        else if (c == '"') { buf[out++] = '\\'; buf[out++] = '"'; }
        else if (c < 0x20) {
            // write \uXXXX
            char esc[8];
            mc2::fmt::to_buf(esc, "\\u{:04x}", static_cast<unsigned>(c));
            for (int i = 0; esc[i] && out + 1 < bufSize; ++i)
                buf[out++] = esc[i];
        } else {
            buf[out++] = static_cast<char>(c);
        }
    }
    if (out + 2 <= bufSize) { buf[out++] = '"'; buf[out] = '\0'; }
    else { buf[bufSize - 1] = '\0'; }
}

// Emit one JSONL record to g_traceFile.
// layer: "mod:<id>", "base-loose", "base-strip", "fastfile", "cd", or "MISS"
// path:  absolute or logical path (empty string for MISS / fastfile)
// key:   NormalizeKey(fileName)
// req:   original fileName as received by the caller
// shadowed: may be nullptr (omit field) or a vector of strings
void EmitResolveRecord(
    const char* key,
    const char* req,
    const char* layer,
    const char* path,
    const std::vector<std::string>* shadowed
) {
    if (!g_traceFile) return;

    double t = TraceElapsedSec();

    // Scratch buffers sized for MAX_PATH strings.
    char keyJ[MAX_PATH * 4], reqJ[MAX_PATH * 4], layerJ[256], pathJ[MAX_PATH * 4];
    JsonEscape(key    ? key    : "", keyJ,   sizeof(keyJ));
    JsonEscape(req    ? req    : "", reqJ,   sizeof(reqJ));
    JsonEscape(layer  ? layer  : "", layerJ, sizeof(layerJ));
    JsonEscape(path   ? path   : "", pathJ,  sizeof(pathJ));

    // Build shadowed array inline.
    char shadowBuf[4096];
    shadowBuf[0] = '\0';
    if (shadowed) {
        size_t pos = 0;
        shadowBuf[pos++] = '[';
        for (size_t i = 0; i < shadowed->size() && pos + 256 < sizeof(shadowBuf); ++i) {
            if (i > 0) { shadowBuf[pos++] = ','; }
            char sv[256];
            JsonEscape((*shadowed)[i].c_str(), sv, sizeof(sv));
            size_t svLen = strlen(sv);
            if (pos + svLen + 2 < sizeof(shadowBuf)) {
                memcpy(shadowBuf + pos, sv, svLen);
                pos += svLen;
            }
        }
        shadowBuf[pos++] = ']';
        shadowBuf[pos]   = '\0';
    }

    const char* layerIndex = layer && std::string_view(layer).ends_with("#scratch")
        ? ",\"layer_index\":0" : "";
    if (shadowed) {
        mc2::fmt::print(g_traceFile,
            "{{\"v\":1,\"t\":{:.3f},\"key\":{},\"req\":{},\"layer\":{},\"path\":{},\"shadowed\":{}{}}}\n",
            t, mc2::fmt::safe(keyJ), mc2::fmt::safe(reqJ), mc2::fmt::safe(layerJ), mc2::fmt::safe(pathJ), mc2::fmt::safe(shadowBuf), mc2::fmt::safe(layerIndex));
    } else {
        mc2::fmt::print(g_traceFile,
            "{{\"v\":1,\"t\":{:.3f},\"key\":{},\"req\":{},\"layer\":{},\"path\":{}{}}}\n",
            t, mc2::fmt::safe(keyJ), mc2::fmt::safe(reqJ), mc2::fmt::safe(layerJ), mc2::fmt::safe(pathJ), mc2::fmt::safe(layerIndex));
    }
}

static bool IsAbsolutePath(std::string_view p) {
    if (p.empty()) return false;
    if (p[0] == '\\' || p[0] == '/') return true;
    if (p.size() >= 2 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
        return true;
    return false;
}

static bool StartsWithDataPath(std::string_view p) {
    return p.size() >= 5 && NormalizeKey(p.substr(0, 5)) == "data/";
}

static bool ShouldSearchMods(std::string_view fileName) {
    if (fileName.empty())                  return false;
    if (IsAbsolutePath(fileName))          return false;
    if (fileName.find("..") != std::string_view::npos) return false;
    if (fileName.find('\0') != std::string_view::npos) return false;
    if (!StartsWithDataPath(fileName))     return false;
    return true;
}

std::string NormalizeKey(std::string_view p) {
    return fst_normalize_loose_key(p);
}

// Index all files under dataDir (absolute, trailing '/') into idx (first-wins).
// relBase should be "data/" so keys come out like "data/missions/foo.fit".
// shadowed: parallel map recording which mod ids lost for each key (populated at level 2).
// skippedDotCount: if non-null, incremented for each dot-prefixed entry skipped at any
//   level of recursion.  Used by IndexModDataCached for the optional summary log.
static void IndexModData(
    std::unordered_map<std::string, ModFileEntry>& idx,
    std::unordered_map<std::string, std::vector<std::string>>& shadowed,
    const char* dataDir, const char* relBase, const char* modId,
    int* skippedDotCount = nullptr
) {
    char pattern[MAX_PATH];
    mc2::fmt::to_buf(pattern, "{}*", mc2::fmt::safe(dataDir));
    pattern[sizeof(pattern) - 1] = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        // Unified dot-dir/dot-file skip rule (ruling C4 in
        // docs/superpowers/strategy/superpowers-execution-roadmap.md):
        // Any entry whose name starts with '.' is skipped from g_modIndex
        // indexing.  This replaces four ad-hoc carve-outs (.scratch/,
        // .modproject/, .playtest/, .modindex-cache) -- none of those dirs
        // may ship their own per-dir skip logic.  Applies to both directories
        // and files so that dot-files (e.g. .gitignore) inside a mod's data/
        // tree are also excluded; no deployed mod (mods/mc2x-compat) relies
        // on a dot-file being indexed.
        if (fd.cFileName[0] == '.') {
            if (skippedDotCount) ++(*skippedDotCount);
            continue;
        }

        char fullChild[MAX_PATH], relChild[MAX_PATH];
        mc2::fmt::to_buf(fullChild, "{}{}", mc2::fmt::safe(dataDir), mc2::fmt::safe(fd.cFileName));
        fullChild[sizeof(fullChild) - 1] = '\0';
        mc2::fmt::to_buf(relChild, "{}{}", mc2::fmt::safe(relBase), mc2::fmt::safe(fd.cFileName));
        relChild[sizeof(relChild) - 1] = '\0';

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char fullSub[MAX_PATH], relSub[MAX_PATH];
            mc2::fmt::to_buf(fullSub, "{}/", mc2::fmt::safe(fullChild));
            mc2::fmt::to_buf(relSub, "{}/", mc2::fmt::safe(relChild));
            fullSub[sizeof(fullSub) - 1] = '\0';
            relSub[sizeof(relSub) - 1]   = '\0';
            IndexModData(idx, shadowed, fullSub, relSub, modId, skippedDotCount);
        } else {
            std::string key = NormalizeKey(relChild);
            auto existing = idx.find(key);
            if (existing == idx.end()) {
                idx[key] = { fullChild, modId };
            } else {
                const auto& [existingKey, existingEntry] = *existing;
                if (logFileResolve) {
                    mc2::fmt::print_out("[mod-dup] {}  winner=[{}]  loser=[{}]\n",
                           key.c_str(), existingEntry.modId.c_str(), mc2::fmt::safe(modId));
                    fflush(stdout);
                }
                // Track for level-2 shadowed[] field (built regardless of log level so
                // the map is ready when a level-2 run starts; overhead is one vector push
                // per dup which is rare — only fires when two mods ship the same file).
                shadowed[key].push_back(modId);
            }
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

// ---------------------------------------------------------------------------
// Mod index disk cache — converts O(N files × FindNextFile) startup scan to
// a single sequential file read on repeat runs.
//
// Cache file: mods/<modId>/.modindex-cache  (plain text, tab-separated)
// Invalidation (MOD-INDEX-CACHE-MTIME-1, v2): a validity SIGNATURE stored in
//   the cache -- non-dot file count + max mtime over all non-dot files AND
//   dirs from ONE full recursive walk of data/ -- recomputed and compared on
//   load. (v1 compared cache mtime vs 2-level dir mtimes, which missed content
//   edits and deep adds: new def files stayed invisible until the cache was
//   hand-deleted.) A v1 cache has no sig line -> rebuilds once.
// Force rebuild: MC2_REBUILD_MOD_CACHE=1
//
// Cache format:
//   MC2MODIDX 2\n
//   modid <id>\n
//   sig <fileCount> <maxMtimeU64>\n
//   count <N>\n
//   <key>\t<absPath>\t<modId>\n   (N lines)
// ---------------------------------------------------------------------------
#include "mod_cache_sig.h"   // pure sig parse/format/match (doctest-covered)

static const char* kCacheHeader = "MC2MODIDX 2";
static const int   kCacheVersion = 2;

// One full recursive metadata walk of the mod's data/ tree: count non-dot
// files, track max last-write time over non-dot files AND directories (dir
// mtimes catch renames, which keep file mtimes + count unchanged). Skip rule
// mirrors IndexModData (ruling C4: any '.'-prefixed entry is invisible).
// Load-time only -- metadata reads, no content I/O.
static void ComputeModDataSignature(const char* dataDir, ModCacheSignature* sig) {
    WIN32_FILE_ATTRIBUTE_DATA da;
    if (GetFileAttributesExA(dataDir, GetFileExInfoStandard, &da)) {
        ULARGE_INTEGER t;
        t.LowPart = da.ftLastWriteTime.dwLowDateTime;
        t.HighPart = da.ftLastWriteTime.dwHighDateTime;
        if (t.QuadPart > sig->maxMtime) sig->maxMtime = t.QuadPart;
    }

    char pat[MAX_PATH];
    mc2::fmt::to_buf(pat, "{}*", mc2::fmt::safe(dataDir));
    pat[sizeof(pat)-1] = '\0';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;   // same skip rule as the indexer
        ULARGE_INTEGER t;
        t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (t.QuadPart > sig->maxMtime) sig->maxMtime = t.QuadPart;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char sub[MAX_PATH];
            mc2::fmt::to_buf(sub, "{}{}/", mc2::fmt::safe(dataDir), mc2::fmt::safe(fd.cFileName));
            sub[sizeof(sub)-1] = '\0';
            ComputeModDataSignature(sub, sig);
        } else {
            sig->fileCount++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// Write g_modIndex (filtered to entries with matching modId, for one mod layer) to cache.
// We write ALL entries from the map — the cache is per-call-to-IndexModData, so we
// pass the entries vector directly.
static void WriteModIndexCache(
    mc2::czstring cachePath,
    std::string_view modId,
    const ModCacheSignature& sig,
    const std::vector<std::pair<std::string,ModFileEntry>>& entries
) {
    FILE* f = fopen(cachePath, "wb");
    if (!f) return;
    char sigLine[64];
    formatModCacheSigLine(sig, sigLine, sizeof(sigLine));
    mc2::fmt::print(f, "{}\n", mc2::fmt::safe(kCacheHeader));
    mc2::fmt::print(f, "modid {}\n", modId);
    mc2::fmt::print(f, "{}\n", mc2::fmt::safe(sigLine));
    mc2::fmt::print(f, "count {:d}\n", mc2::fmt::as_size(entries.size()));
    for (auto& [key, entry] : entries)
        mc2::fmt::print(f, "{}\t{}\t{}\n", key.c_str(), entry.path.c_str(), entry.modId.c_str());
    fclose(f);
}

// Load cache into idx (first-wins, same as IndexModData).
// Returns number of entries loaded, or nullopt on format error.
static std::optional<int> LoadModIndexCache(
    std::unordered_map<std::string, ModFileEntry>& idx,
    std::unordered_map<std::string, std::vector<std::string>>& shadowed,
    mc2::czstring cachePath,
    std::string_view expectedModId,
    const ModCacheSignature& expectedSig,
    std::string_view dataDir,
    bool* sigMismatch   // out: header/modid ok but signature differs (stale)
) {
    if (sigMismatch) *sigMismatch = false;
    FILE* f = fopen(cachePath, "rb"); // path-audit: loose-only; physical mod-index cache, not game data.
    if (!f) return std::nullopt;

    char line[MAX_PATH * 3];
    // header (a v1 cache fails here -> one-time rebuild after this change)
    if (!fgets(line, sizeof(line), f)) { fclose(f); return std::nullopt; }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, kCacheHeader) != 0) { fclose(f); return std::nullopt; }
    // modid
    if (!fgets(line, sizeof(line), f)) { fclose(f); return std::nullopt; }
    line[strcspn(line, "\r\n")] = '\0';
    if (strncmp(line, "modid ", 6) != 0) { fclose(f); return std::nullopt; }
    if (std::string_view(line + 6) != expectedModId) { fclose(f); return std::nullopt; }
    // sig (MOD-INDEX-CACHE-MTIME-1): stored tree signature vs the one just
    // recomputed from disk -- ANY mismatch means files changed since index time.
    if (!fgets(line, sizeof(line), f)) { fclose(f); return std::nullopt; }
    line[strcspn(line, "\r\n")] = '\0';
    const auto storedSig = parseModCacheSigLine(line);
    if (!storedSig) { fclose(f); return std::nullopt; }
    if (!modCacheSignatureMatches(*storedSig, expectedSig)) {
        if (sigMismatch) *sigMismatch = true;
        fclose(f);
        return std::nullopt;
    }
    // count
    if (!fgets(line, sizeof(line), f)) { fclose(f); return std::nullopt; }
    line[strcspn(line, "\r\n")] = '\0';
    if (strncmp(line, "count ", 6) != 0) { fclose(f); return std::nullopt; }
    int count = atoi(line + 6);
    if (count < 0) { fclose(f); return std::nullopt; }

    int loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        char* tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = '\0';
        char* tab2 = strchr(tab1+1, '\t');
        if (!tab2) continue;
        *tab2 = '\0';
        std::string key    = line;
        std::string path   = tab1 + 1;
        if (loaded == 0) {
            // A cache copied from another install (deploy/clone) carries that install's absolute paths and
            // still matches the signature; every _open would then fail silently. Rebuild instead.
            const size_t n = dataDir.size();
            bool same = path.size() >= n;
            for (size_t i = 0; same && i < n; ++i) {
                char a = path[i] == '\\' ? '/' : static_cast<char>(tolower(static_cast<unsigned char>(path[i])));
                char b = dataDir[i] == '\\' ? '/' : static_cast<char>(tolower(static_cast<unsigned char>(dataDir[i])));
                same = a == b;
            }
            if (!same) { fclose(f); return std::nullopt; }
        }
        std::string modid  = tab2 + 1;
        auto existing = idx.find(key);
        if (existing == idx.end()) {
            idx[key] = { path, modid };
        } else {
            const auto& [existingKey, existingEntry] = *existing;
            // Same loser bookkeeping as IndexModData, so the conflict report
            // does not depend on whether the layer came from cache or a walk.
            if (logFileResolve) {
                mc2::fmt::print_out("[mod-dup] {}  winner=[{}]  loser=[{}]\n",
                       key.c_str(), existingEntry.modId.c_str(), modid.c_str());
                fflush(stdout);
            }
            shadowed[key].push_back(modid);
        }
        loaded++;
    }
    fclose(f);
    return loaded;
}

// Try to load the cache for one mod layer (dataDir, modId) into idx.
// Returns true if cache was valid and loaded, false if a fresh scan is needed.
static bool TryLoadModCache(
    std::unordered_map<std::string, ModFileEntry>& idx,
    std::unordered_map<std::string, std::vector<std::string>>& shadowed,
    const char* absModRoot,   // e.g. "A:/Games/.../mods/"
    const char* modId,        // e.g. "mc2x-pbr"
    const char* dataDir       // e.g. "...mods/mc2x-pbr/data/"
) {
    if (mc2::gate::get<"MC2_REBUILD_MOD_CACHE">()) {
        mc2::fmt::print_out("[mod-cache] refresh modid='{}' reason=MC2_REBUILD_MOD_CACHE\n", mc2::fmt::safe(modId));
        fflush(stdout);
        return false;
    }

    char cachePath[MAX_PATH];
    mc2::fmt::to_buf(cachePath, "{}{}/.modindex-cache", mc2::fmt::safe(absModRoot), mc2::fmt::safe(modId));
    cachePath[sizeof(cachePath)-1] = '\0';

    // MOD-INDEX-CACHE-MTIME-1: recompute the tree signature (one recursive
    // metadata walk, load-time only) and compare against the one stored in
    // the cache. Catches content edits, deep adds/removes, and renames --
    // the v1 "dir mtimes, 2 levels" rule missed all three.
    ModCacheSignature diskSig;
    ComputeModDataSignature(dataDir, &diskSig);

    bool sigMismatch = false;
    auto loaded = LoadModIndexCache(idx, shadowed, cachePath, modId, diskSig, dataDir, &sigMismatch);
    if (!loaded) {
        mc2::fmt::print_out("[mod-cache] refresh modid='{}' reason={} (files={:d} maxMtime={:d})\n",
               mc2::fmt::safe(modId),
               mc2::fmt::safe(sigMismatch ? "signature-mismatch (mod files changed)"
                           : "cache missing/stale-format"),
               mc2::fmt::as_u64(diskSig.fileCount), mc2::fmt::as_u64(diskSig.maxMtime));
        fflush(stdout);
        return false;
    }

    mc2::fmt::print_out("[mod-cache] hit modid='{}' loaded={:d} files (sig files={:d} maxMtime={:d})\n",
           mc2::fmt::safe(modId), mc2::fmt::as_i32(*loaded), mc2::fmt::as_u64(diskSig.fileCount), mc2::fmt::as_u64(diskSig.maxMtime));
    fflush(stdout);
    return true;
}

// Build fresh index for one mod layer and write cache.
static void IndexModDataCached(
    std::unordered_map<std::string, ModFileEntry>& idx,
    std::unordered_map<std::string, std::vector<std::string>>& shadowed,
    const char* absModRoot,
    const char* modId,
    const char* dataDir,
    const char* relBase
) {
    // Snapshot keys before to identify newly added entries after the walk.
    std::unordered_map<std::string, ModFileEntry> layer;
    std::unordered_map<std::string, std::vector<std::string>> layerShadowed;
    int skippedDot = 0;
    IndexModData(layer, layerShadowed, dataDir, relBase, modId, &skippedDot);

    // Merge layer shadowed into global shadowed.
    for (auto& [key, entries] : layerShadowed)
        for (auto& s : entries)
            shadowed[key].push_back(s);

    // Merge into idx (first-wins).
    // A key already owned by a higher layer is a cross-layer duplicate: the walk above
    // only saw this layer, so record the loser here (the conflict report reads it).
    for (auto& [key, entry] : layer) {
        auto existing = idx.find(key);
        if (existing == idx.end()) {
            idx[key] = entry;
        } else {
            const auto& [existingKey, existingEntry] = *existing;
            if (logFileResolve) {
                mc2::fmt::print_out("[mod-dup] {}  winner=[{}]  loser=[{}]\n",
                       key.c_str(), existingEntry.modId.c_str(), mc2::fmt::safe(modId));
                fflush(stdout);
            }
            shadowed[key].push_back(modId);
        }
    }

    // Collect layer entries for cache (all entries this mod owns in the layer map).
    std::vector<std::pair<std::string,ModFileEntry>> fresh(layer.begin(), layer.end());

    char cachePath[MAX_PATH];
    mc2::fmt::to_buf(cachePath, "{}{}/.modindex-cache", mc2::fmt::safe(absModRoot), mc2::fmt::safe(modId));
    cachePath[sizeof(cachePath)-1] = '\0';
    // Signature is taken AFTER the index walk so it reflects the exact tree
    // state the entries were built from (the cache file itself is dot-named ->
    // invisible to the signature walk, so writing it never dirties the sig).
    ModCacheSignature sig;
    ComputeModDataSignature(dataDir, &sig);
    WriteModIndexCache(cachePath, modId, sig, fresh);
    mc2::fmt::print_out("[mod-cache] miss modid='{}' indexed={:d} wrote cache (sig files={:d} maxMtime={:d})\n",
           mc2::fmt::safe(modId), mc2::fmt::as_size(fresh.size()), mc2::fmt::as_u64(sig.fileCount), mc2::fmt::as_u64(sig.maxMtime));
    if (logFileResolve >= 1 && skippedDot > 0)
        mc2::fmt::print_out("[mod] [{}] skipped {:d} dot-entr{} (ruling C4)\n",
               mc2::fmt::safe(modId), mc2::fmt::as_i32(skippedDot), mc2::fmt::safe(skippedDot == 1 ? "y" : "ies"));
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Minimal JSON string field extractor — no full parser needed for mod.json.
static std::string JsonGetString(const char* json, const char* key) {
    char needle[128];
    mc2::fmt::to_buf(needle, "\"{}\"", mc2::fmt::safe(key));
    needle[sizeof(needle)-1] = '\0';
    const char* p = strstr(json, needle);
    if (!p) return {};
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return {};
    p++;
    const char* end = strchr(p, '"');
    if (!end) return {};
    return std::string(p, static_cast<size_t>((end - p)));
}

// Extract string array from "key": ["a","b",...].
static std::vector<std::string> JsonGetStringArray(const char* json, const char* key) {
    char needle[128];
    mc2::fmt::to_buf(needle, "\"{}\"", mc2::fmt::safe(key));
    needle[sizeof(needle)-1] = '\0';
    const char* p = strstr(json, needle);
    if (!p) return {};
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '[') return {};
    p++;
    const char* end = strchr(p, ']');
    if (!end) return {};

    std::vector<std::string> result;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) p++;
        if (*p == '"') {
            p++;
            const char* se = strchr(p, '"');
            if (!se || se > end) break;
            result.push_back(std::string(p, static_cast<size_t>((se - p))));
            p = se + 1;
        } else { p++; }
    }
    return result;
}

struct ModJson { std::string id, name; std::vector<std::string> deps, capabilities; };

// Read mod.json at jsonPath. Returns nullopt if file not found or has no id.
static std::optional<ModJson> ReadModJson(const char* jsonPath) {
    HANDLE fh = CreateFileA(jsonPath, GENERIC_READ, FILE_SHARE_READ, nullptr, // path-audit: loose-only; manifest of the specific mod being mounted.
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return std::nullopt;
    char buf[4096] = {};
    DWORD read = 0;
    ReadFile(fh, buf, sizeof(buf)-1, &read, nullptr);
    CloseHandle(fh);
    buf[read] = '\0';

    ModJson mj{JsonGetString(buf, "id"), JsonGetString(buf, "name"), JsonGetStringArray(buf, "dependencies"),
               JsonGetStringArray(buf, "capabilities")};
    if (mj.id.empty()) return std::nullopt;
    return mj;
}


// Returns the owning mod id for a given data path (normalized), or nullptr if not in mod index.
// Used for diagnostic logging only — O(1) lookup.
std::vector<unsigned char> ReadArchiveBytes(mc2::czstring dataPath, size_t maxBytes) {
    if (!dataPath) return {};
    long handle = -1;
    auto* archive = FastFileFind(dataPath, handle);
    if (!archive) return {};
    struct Member {
        FastFile* archive;
        long handle;
        ~Member() { archive->closeFast(handle); }
    } member{archive, handle};
    const long count = archive->sizeFast(handle);
    if (count <= 0 || static_cast<size_t>(count) >= maxBytes) return {};
    std::vector<unsigned char> bytes(static_cast<size_t>(count));
    if (archive->readFast(handle, bytes.data(), count) != count) return {};
    return bytes;
}

int LookupModPriority(mc2::czstring dataPath) {
    const char* owner = LookupModOwner(dataPath);
    // Source and cooked-sidecar keys can differ. A legacy loose TGA must also
    // outrank the kit's KTX, while the kit still outranks packed stock assets.
    if (owner && (std::string_view(owner) == "mc2-industrial-kit" || std::string_view(owner) == "mc2-vehicle-kit")) return 999999;
    auto it = owner ? g_modPriority.find(owner) : g_modPriority.end();
    if (it != g_modPriority.end()) {
        const auto& [modId, priority] = *it;
        return priority;
    }
    return _access(dataPath, 4) == 0 ? 999998 : 1000000;
}

const char* LookupModOwner(mc2::czstring dataPath) {
    if (g_modIndex.empty()) return nullptr;
    std::string key = NormalizeKey(dataPath ? std::string_view(dataPath) : std::string_view{});
    auto it = g_modIndex.find(key);
    if (it == g_modIndex.end()) return nullptr;
    const auto& [modKey, entry] = *it;
    return entry.modId.c_str();
}

ModFileSource ResolveModFileSource(std::string_view dataPath) {
    ModFileSource source{std::nullopt,g_activeModId.empty()};
    if (!ShouldSearchMods(dataPath)) return source;
    const auto it = g_modIndex.find(NormalizeKey(dataPath));
    if (it == g_modIndex.end()) return source;
    const auto& [key, entry] = *it;
    if (_access(entry.path.c_str(),4) != 0) return source;
    source.path = entry.path;
    source.editable = entry.modId == g_activeModId;
    return source;
}

const char* LookupModAbsolutePath(std::string_view dataPath) {
    const std::string key = NormalizeKey(dataPath);
    if (const auto it = g_modIndex.find(key); it != g_modIndex.end()) {
        const auto& [indexKey, entry] = *it;
        return entry.path.c_str();
    }
    return static_cast<const char*>(nullptr);
}

static bool capabilityEquals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

bool ModHasCapability(std::string_view modId, std::string_view capability) {
    for (const auto& [id, caps] : g_modCapabilities)
        if (id == modId)
            return std::ranges::any_of(caps, [&](std::string_view c) { return capabilityEquals(c, capability); });
    return false;
}

bool ModCapabilityMounted(std::string_view capability) {
    return std::ranges::any_of(g_modCapabilities, [&](const auto& kv) {
        const auto& [id, caps] = kv;
        return ModHasCapability(id, capability); });
}

bool ModOwnerHasCapability(std::string_view dataPath, std::string_view capability) {
    const char* owner = LookupModOwner(std::string(dataPath).c_str());
    return owner && ModHasCapability(owner, capability);
}

// No-ops — mod is selected at launcher time, not at campaign-select time.
// Kept for call-site compatibility in logisticsdata.cpp / logisticsdialog.cpp / mission.cpp.
void ActivateCampaignMod(mc2::czstring /*campaignFitName*/) {}
void ActivateModForMission(mc2::czstring /*missionFitKey*/) {}
void DeactivateMod() {}

void EnumerateModFiles(std::string_view keyPrefix, ModFitCallback cb, void* userData) {
    if (keyPrefix.empty() || !cb) return;
    const std::string prefix = NormalizeKey(keyPrefix);
    std::vector<std::string> keys;
    for (const auto& [key, entry] : g_modIndex)
        if (key.compare(0, prefix.size(), prefix) == 0) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) cb(key.c_str(), userData);
}

void EnumerateModFitFiles(mc2::czstring keyPrefix, ModFitCallback cb, void* userData) {
    if (g_modIndex.empty() || !keyPrefix || !cb) return;
    std::string prefix = NormalizeKey(keyPrefix);
    static const std::string ext = ".fit";
    // Collect matching absolute paths, keyed by filename for sort stability.
    std::vector<std::pair<std::string, std::string>> hits; // (filename, absPath)
    for (auto& [key, entry] : g_modIndex) {
        if (key.compare(0, prefix.size(), prefix) != 0) continue;
        if (key.size() < ext.size()) continue;
        if (key.compare(key.size() - ext.size(), ext.size(), ext) != 0) continue;
        std::string fname = key.substr(prefix.size());
        hits.push_back({ fname, entry.path });
    }
    std::sort(hits.begin(), hits.end()); // sort by filename
    for (auto& [filename, path] : hits)
        cb(path.c_str(), userData);
}

// Mod duplicate-content scan (informational; never changes which file wins).
// For every indexed key that a lower layer (another mod / scratch / the loose stock
// file in the install) also provides, compare the bytes of winner and loser and report
// the pair once if they differ. Stock is probed as the loose file at the install-relative
// key only: files that exist solely inside a FastFile archive are not compared.
// Lines are "[CONTENT] mod-dup ..." (no "kind=", so mod_smoke's content-error scrape
// does not count them) and one "[CONTENT] mod-dup summary" line.
static bool ModFilesEqualBytes(const std::string& a, const std::string& b) {
    FILE* fa = fopen(a.c_str(), "rb"); // path-audit: loose-only; compare this physical winner, without overlay resolution.
    FILE* fb = fopen(b.c_str(), "rb"); // path-audit: loose-only; compare this physical loser, without overlay resolution.
    bool equal = fa && fb;
    if (equal) {
        static char bufA[65536], bufB[65536];
        for (;;) {
            const size_t na = fread(bufA, 1, sizeof(bufA), fa);
            const size_t nb = fread(bufB, 1, sizeof(bufB), fb);
            if (na != nb || memcmp(bufA, bufB, na) != 0) { equal = false; break; }
            if (na == 0) break;
        }
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return equal;
}

static bool ModFileSize(const std::string& path, unsigned long long& size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    size = (static_cast<unsigned long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return true;
}

static void ScanModDuplicateContent(const char* absRoot, const std::string& activeId) {
    const auto t0 = GetTickCount64();
    size_t scanned = 0, conflicts = 0, identical = 0, printed = 0;
    const size_t printCap = logFileResolve ? static_cast<size_t>(-1) : 200;
    // Startup guard: the scan stops after 200 ms (unlimited when MC2_LOG_FILE_RESOLVE is
    // set, for full-count audits) and the summary reports partial=1 unscanned_keys=N.
    const unsigned long long budgetMs = logFileResolve ? 0ull : 200ull;
    size_t unscanned = 0;
    bool partial = false;
    std::vector<std::string> keys;
    keys.reserve(g_modIndex.size());
    for (const auto& [key, entry] : g_modIndex) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    for (const std::string& key : keys) {
        if (partial || (budgetMs && GetTickCount64() - t0 > budgetMs)) { partial = true; ++unscanned; continue; }
        const ModFileEntry& win = g_modIndex.at(key);
        std::vector<std::pair<std::string, std::string>> losers;   // layer label, path
        if (auto sit = g_shadowedBy.find(key); sit != g_shadowedBy.end()) {
            const auto& [shadowedKey, ids] = *sit;
            for (const std::string& id : ids) {
                const size_t h = id.rfind("#scratch");
                std::string path = h != std::string::npos
                    ? std::string(absRoot) + id.substr(0, h) + "/.scratch/" + key
                    : std::string(absRoot) + id + "/" + key;
                losers.emplace_back(id, std::move(path));
            }
        }
        unsigned long long sz = 0;
        if (ModFileSize(key, sz)) losers.emplace_back("stock", key);
        if (losers.empty()) continue;
        ++scanned;
        unsigned long long winSize = 0;
        if (!ModFileSize(win.path, winSize)) continue;
        std::string differing;
        for (const auto& [layer, path] : losers) {
            unsigned long long loserSize = 0;
            if (!ModFileSize(path, loserSize)) continue;
            if (loserSize == winSize && ModFilesEqualBytes(win.path, path)) continue;
            if (!differing.empty()) differing += ",";
            differing += layer;
        }
        if (differing.empty()) { ++identical; continue; }
        ++conflicts;
        if (printed < printCap) {
            ++printed;
            mc2::fmt::print_out("[CONTENT] mod-dup path={} winner={} losers={}\n", key.c_str(), win.modId.c_str(), differing.c_str());
        }
    }
    mc2::fmt::print_out("[CONTENT] mod-dup summary: active='{}' files_with_lower_layer={:d} differing={:d} identical={:d} printed={:d} partial={:d} unscanned_keys={:d} scan_ms={:d}\n",
           activeId.c_str(), mc2::fmt::as_size(scanned), mc2::fmt::as_size(conflicts), mc2::fmt::as_size(identical), mc2::fmt::as_size(printed), mc2::fmt::as_i32(partial ? 1 : 0), mc2::fmt::as_size(unscanned), mc2::fmt::as_u64(static_cast<unsigned long long>(GetTickCount64() - t0)));
    fflush(stdout);
}

void InitModSearchPaths(mc2::czstring modsRoot) {
    mc2::vfs::invalidate();
    g_modIndex.clear();
    g_modPriority.clear();
    g_shadowedBy.clear();
    g_activeModId.clear();
    g_modCapabilities.clear();

    // Parse MC2_LOG_FILE_RESOLVE: absent/empty -> 0; any non-numeric non-empty -> 1 (compat);
    // numeric -> atoi (so "1" -> 1, "2" -> 2).
    {
        const std::string envLevelStr = mc2::gate::str<"MC2_LOG_FILE_RESOLVE">();
        const char* envLevel = envLevelStr.c_str();
        if (!envLevel[0]) {
            logFileResolve = 0;
        } else {
            // Check if purely numeric.
            bool isNum = true;
            for (const char* p = envLevel; *p; ++p)
                if (*p < '0' || *p > '9') { isNum = false; break; }
            // Numeric value taken literally (so "0" means OFF); any
            // non-numeric legacy value ("true", "on", ...) maps to level 1.
            logFileResolve = isNum ? atoi(envLevel) : 1;
        }
    }

    // Level-2 trace file setup.
    if (g_traceFile) { fclose(g_traceFile); g_traceFile = nullptr; }
    if (logFileResolve >= 2) {
        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_startQPC);
        const std::string tracePathStr = mc2::gate::str<"MC2_RESOLVE_TRACE_FILE">();
        const char* tracePath = tracePathStr.c_str();
        if (tracePath[0]) {
            g_traceFile = fopen(tracePath, "a");
            if (!g_traceFile)
                mc2::fmt::print_out("[mod] warning: MC2_RESOLVE_TRACE_FILE={} could not be opened for append\n", mc2::fmt::safe(tracePath));
        }
        // If MC2_RESOLVE_TRACE_FILE unset at level 2: skip JSONL (per spec).
    }

    // META-FIX design E tier 3 (ii): level>=1 shutdown summary. Registered
    // once; prints the process-lifetime resolve totals so a nightly log scan
    // can spot silent-loss growth without the level-2 JSONL trace.
    if (logFileResolve >= 1) {
        static bool s_summaryRegistered = false;
        if (!s_summaryRegistered) {
            s_summaryRegistered = true;
            atexit([]() {
                mc2::fmt::print_out("[file-resolve] opens={:d} misses={:d}\n",
                       mc2::fmt::as_ulong(g_resolveOpens), mc2::fmt::as_ulong(g_resolveMisses));
                fflush(stdout);
            });
        }
    }

    // MC2_ACTIVE_MOD is re-read here on every mount: the editor's mod picker changes it
    // with _putenv_s at run time, so no reader may cache it in a static.
    std::string envModStr = mc2::gate::str<"MC2_ACTIVE_MOD">();
    if (envModStr == "mc2-industrial-kit" || envModStr == "mc2-vehicle-kit") envModStr.clear(); // Bundled art is never an active user layer.
    const char* envMod = envModStr.c_str();
    const bool stockMode = !envMod[0];
    const bool industrialKit = mc2::options::current().remasteredBuildings &&
        GetFileAttributesA("mods/mc2-industrial-kit/mod.json") != INVALID_FILE_ATTRIBUTES;
    const bool vehicleKit = mc2::options::current().remasteredBuildings &&
        !mc2::options::lowCost() && GetFileAttributesA("mods/mc2-vehicle-kit/mod.json") != INVALID_FILE_ATTRIBUTES;
    if (stockMode) {
        // Stock campaign: no active mod. Add-on layers (MC2_MOD_DEPS) may still mount
        // (e.g. "stock + Omnitech mechs"). With NO deps either, this is pure base game.
        if (!industrialKit && !vehicleKit && mc2::gate::str<"MC2_MOD_DEPS">().empty()) {
            mc2::fmt::print_out("[mod] base game mode (MC2_ACTIVE_MOD not set, MC2_MOD_DEPS not set)\n");
            fflush(stdout);
            return;
        }
        mc2::fmt::print_out("[mod] stock campaign + add-ons mode (MC2_MOD_DEPS set, no MC2_ACTIVE_MOD)\n");
        fflush(stdout);
        envMod = "";  // downstream treats the active mod as absent
    }
    g_activeModId = stockMode ? std::string() : std::string(envMod);

    // Resolve modsRoot to absolute path so index keys stay valid regardless of CWD changes.
    char absRoot[MAX_PATH];
    if (!GetFullPathNameA(modsRoot, sizeof(absRoot), absRoot, nullptr)) {
        mc2::fmt::print_out("[mod] failed to resolve mods root: {}\n", mc2::fmt::safe(modsRoot)); fflush(stdout);
        return;
    }
    size_t rootLen = strlen(absRoot);
    if (rootLen > 0 && absRoot[rootLen-1] != '\\' && absRoot[rootLen-1] != '/') {
        absRoot[rootLen]   = '/';
        absRoot[rootLen+1] = '\0';
    }
    g_modsAbsRoot = absRoot;   // the VFS index derives shadowed-mod physical paths from it

    // Read active mod's mod.json for dependencies.
    char jsonPath[MAX_PATH];
    mc2::fmt::to_buf(jsonPath, "{}{}/mod.json", mc2::fmt::safe(absRoot), mc2::fmt::safe(envMod));
    jsonPath[sizeof(jsonPath)-1] = '\0';

    std::string modId, modName;
    std::vector<std::string> deps;
    if (stockMode) {
        // Stock campaign: no active mod, so no active mod.json. Add-on layers come
        // solely from MC2_MOD_DEPS (parsed below).
        modId = "";
        modName = "";
    } else if (auto mj = ReadModJson(jsonPath)) {
        modId = mj->id; modName = mj->name; deps = mj->deps;
        if (!mj->capabilities.empty()) g_modCapabilities[modId] = mj->capabilities;
    } else {
        // No mod.json — treat the folder name as the id. Dependencies (the compat
        // layer) come from MC2_MOD_DEPS instead (see below) — the launcher lets the
        // player pick a campaign + a compatibility layer separately, so metadata-less
        // third-party campaigns (e.g. MCO packs) need no authored mod.json.
        modId   = envMod;
        modName = envMod;
        mc2::fmt::print_out("[mod] no mod.json for '{}'; folder name as id, deps from MC2_MOD_DEPS\n", mc2::fmt::safe(envMod));
    }

    // MC2_MOD_DEPS: launcher-supplied compatibility layer(s), comma/semicolon
    // separated, highest-priority-first. Appended after any mod.json deps and
    // de-duplicated. This is how the player's "compatibility layer" selection
    // reaches the engine without baking it into a campaign's mod.json.
    const std::string envDepsStr = mc2::gate::str<"MC2_MOD_DEPS">();
    if (!envDepsStr.empty()) {
        std::string s(envDepsStr);
        size_t i = 0;
        while (i < s.size()) {
            size_t j = s.find_first_of(",;", i);
            if (j == std::string::npos) j = s.size();
            std::string d = s.substr(i, j - i);
            size_t a = d.find_first_not_of(" \t");
            size_t b = d.find_last_not_of(" \t");
            if (a != std::string::npos) {
                d = d.substr(a, b - a + 1);
                if (!d.empty() && std::find(deps.begin(), deps.end(), d) == deps.end())
                    deps.push_back(d);
            }
            i = j + 1;
        }
    }
    // Shipped art is the lowest mod layer; campaign and user add-ons win.
    std::erase(deps, "mc2-industrial-kit");
    if (industrialKit)
        deps.push_back("mc2-industrial-kit");
    std::erase(deps, "mc2-vehicle-kit");
    if (vehicleKit) deps.push_back("mc2-vehicle-kit");
    // Priority order: base data < dep[N-1] < ... < dep[0] < active mod.
    // Index is first-wins, so scan highest priority first.
    // Duplicates: winner keeps its slot; loser is logged when MC2_LOG_FILE_RESOLVE=1.
    {
        // Build display string: "base < dep[N-1] < ... < dep[0] < active"
        std::string order = "base";
        for (int i = static_cast<int>(deps.size())-1; i >= 0; i--) order += " < " + deps[i];
        order += " < "; order += modId;
        mc2::fmt::print_out("[mod] load order (lowest to highest priority): {}\n", order.c_str());
        fflush(stdout);
    }

    g_modPriority[modId] = 0;
    for (size_t i = 0; i < deps.size(); ++i) g_modPriority.try_emplace(deps[i], static_cast<int>(i + 1));
    // Layer 0 (scratch overrides): mods/<active>/.scratch/data/,
    // indexed BEFORE the active mod so first-wins puts it on top. Always walked fresh
    // (never cached: small, and a cache would hide edits). Presence of files is the
    // opt-in; MC2_SCRATCH=0 is the kill-switch. Engine read-only.
    if (!stockMode && mc2::gate::get<"MC2_SCRATCH">()) {
        const std::string scrDir = std::string(absRoot) + envMod + "/.scratch/data/";
        if (GetFileAttributesA(scrDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            size_t before = g_modIndex.size();
            const std::string scrId = modId + "#scratch";
            IndexModData(g_modIndex, g_shadowedBy, scrDir.c_str(), "data/", scrId.c_str());
            if (g_modIndex.size() > before) {
                g_modPriority[scrId] = 0;
                mc2::fmt::print_out("[mod] scratch layer ACTIVE: {:d} files ({})\n", mc2::fmt::as_size(g_modIndex.size() - before), scrDir.c_str());
                fflush(stdout);
            }
        }
    }

    // Index active mod first (highest priority). Skipped entirely in stock mode.
    char activeDataDir[MAX_PATH];
    mc2::fmt::to_buf(activeDataDir, "{}{}/data/", mc2::fmt::safe(absRoot), mc2::fmt::safe(envMod));
    activeDataDir[sizeof(activeDataDir)-1] = '\0';
    if (!stockMode && GetFileAttributesA(activeDataDir) != INVALID_FILE_ATTRIBUTES) {
        crashbundle_mod(modId.c_str(), 0);
        size_t before = g_modIndex.size();
        if (!TryLoadModCache(g_modIndex, g_shadowedBy, absRoot, modId.c_str(), activeDataDir))
            IndexModDataCached(g_modIndex, g_shadowedBy, absRoot, modId.c_str(), activeDataDir, "data/");
        mc2::fmt::print_out("[mod] [1/{:d}] active '{}': {:d} files\n",
               mc2::fmt::as_size(deps.size()+1), mc2::fmt::safe(envMod), mc2::fmt::as_size(g_modIndex.size() - before));
    }

    // Index dependencies in declared order (dep[0] > dep[1] > ...).
    for (size_t i = 0; i < deps.size(); i++) {
        const std::string& dep = deps[i];
        char depDataDir[MAX_PATH];
        mc2::fmt::to_buf(depDataDir, "{}{}/data/", mc2::fmt::safe(absRoot), dep);
        depDataDir[sizeof(depDataDir)-1] = '\0';
        if (GetFileAttributesA(depDataDir) == INVALID_FILE_ATTRIBUTES) {
            mc2::fmt::print_out("[mod] dep '{}' not found at {}, skipping\n", dep.c_str(), mc2::fmt::safe(depDataDir));
            fflush(stdout);
            continue;
        }
        {
            const std::string depJson = std::string(absRoot) + dep + "/mod.json";
            if (auto dj = ReadModJson(depJson.c_str()); dj && !dj->capabilities.empty())
                g_modCapabilities[dep] = dj->capabilities;
        }
        size_t before = g_modIndex.size();
        if (!TryLoadModCache(g_modIndex, g_shadowedBy, absRoot, dep.c_str(), depDataDir))
            IndexModDataCached(g_modIndex, g_shadowedBy, absRoot, dep.c_str(), depDataDir, "data/");
        crashbundle_mod(dep.c_str(), static_cast<int>(i + 1));
        mc2::fmt::print_out("[mod] [{:d}/{:d}] dep '{}': {:d} new files\n",
               mc2::fmt::as_size(i+2), mc2::fmt::as_size(deps.size()+1), dep.c_str(), mc2::fmt::as_size(g_modIndex.size() - before));
    }

    // STOCK-AI GUARD (load-bearing invariant): with no active campaign, a dep add-on
    // layer must not override stock's core ABL libraries (data/missions/*.abx, i.e.
    // corebrain/orders/miscfunc). Those are campaign-brain plumbing the mechs/weapons
    // do not need; letting them override stock would alter/break stock AI. Erase any
    // such dep-supplied key so the engine falls back to stock's ABL. g_modIndex holds
    // only dep/active entries (base is the fallback, never indexed), so in stock mode
    // every match is a dep override. Runs after both fresh-index and cache-load paths.
    if (stockMode) {
        size_t erased = 0;
        for (auto it = g_modIndex.begin(); it != g_modIndex.end(); ) {
            const auto& [k, entry] = *it;  // NormalizeKey: lowercase, '/' separators
            const bool inMissions = (k.rfind("data/missions/", 0) == 0);
            const bool isAbx = (k.size() >= 4 && k.compare(k.size() - 4, 4, ".abx") == 0);
            if (inMissions && isAbx) { it = g_modIndex.erase(it); ++erased; }
            else { ++it; }
        }
        mc2::fmt::print_out("[mod] stock-AI guard: dropped {:d} dep ABL override(s) under data/missions/\n", mc2::fmt::as_size(erased));
        fflush(stdout);
    }

    // Install-overlay mods predate mods/<id>. Keep their loose files above the
    // bundled kit for every reader (File, texture probes and importer adapters),
    // including when the index came from cache. Other mounted mods still win.
    for (auto it = g_modIndex.begin(); it != g_modIndex.end(); ) {
        const auto& [key, entry] = *it;
        if ((entry.modId == "mc2-industrial-kit" || entry.modId == "mc2-vehicle-kit") && _access(key.c_str(), 4) == 0)
            it = g_modIndex.erase(it);
        else ++it;
    }

    // MOD-LOAD-ORDER-CONFLICT-REPORT-1: never-silent conflict report.
    // Runs ONLY when mods are indexed (zero mods returned earlier => zero new
    // output; stock stays byte-identical). Facts gathered here, classification
    // is the pure shared mclib/mod_conflict.h (mirrored offline by
    // mc2_modlint --mod). Stock-existence probe is limited to data/defs/ keys
    // (the def-override visibility contract) to keep init cheap.
    {
        std::vector<ModIndexedEntry> entries;
        entries.reserve(g_modIndex.size());
        for (const auto& [key, entry] : g_modIndex) {
            ModIndexedEntry e;
            e.key = key;
            e.winnerModId = entry.modId;
            auto sit = g_shadowedBy.find(key);
            if (sit != g_shadowedBy.end()) {
                const auto& [shadowedKey, ids] = *sit;
                e.shadowedModIds = ids;
            }
            // A scratch override of its own project is intentional, not a conflict.
            if (const size_t h = e.winnerModId.rfind("#scratch"); h != std::string::npos) {
                const std::string base = e.winnerModId.substr(0, h);
                std::erase(e.shadowedModIds, base);
            }
            if (key.rfind("data/defs/", 0) == 0)
                e.stockExists = (GetFileAttributesA(key.c_str()) != INVALID_FILE_ATTRIBUTES);
            // Skip clean non-def entries early — only entries that can produce a
            // finding are classified (keeps the vector small on big mods).
            if (e.shadowedModIds.empty() && !e.stockExists &&
                !modKeyUsesReservedCoreNamespace(e.key))
                continue;
            entries.push_back(std::move(e));
        }
        std::sort(entries.begin(), entries.end(),
                  [](const ModIndexedEntry& a, const ModIndexedEntry& b) { return a.key < b.key; });
        std::vector<ModConflictFinding> findings = classifyModConflicts(entries);
        for (const std::string& line : formatModConflictReport(findings))
            mc2::fmt::print_out("[mod] conflict: {}\n", line.c_str());
        fflush(stdout);
    }

    ScanModDuplicateContent(absRoot, modId);

    mc2::fmt::print_out("[mod] ready: active='{}' total={:d} files\n", mc2::fmt::safe(envMod), mc2::fmt::as_size(g_modIndex.size()));
    fflush(stdout);
    mc2::vfs::invalidate();
}

// ---------------------------------------------------------------------------
// vfs_internal.h: read-only views of the mod layer for mc2::vfs (vfs_index.cpp).
// ---------------------------------------------------------------------------
namespace mc2::vfs::internal {
const std::unordered_map<std::string, ModFileEntry>& modIndex() { return g_modIndex; }
const std::unordered_map<std::string, std::vector<std::string>>& modShadowedBy() { return g_shadowedBy; }
const std::unordered_map<std::string, int>& modPriority() { return g_modPriority; }
const std::string& modsAbsRoot() { return g_modsAbsRoot; }
bool shouldSearchMods(std::string_view fileName) { return ShouldSearchMods(fileName); }
bool isAbsolutePath(std::string_view p) { return IsAbsolutePath(p); }
} // namespace mc2::vfs::internal
