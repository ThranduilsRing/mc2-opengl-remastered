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

#include <unordered_map>
#include <string>
#include <vector>
#include <optional>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "../GameOS/gameos/utils/mc2log.h"

namespace
{
// Largest in-RAM file image a FastFile entry may claim. The biggest stock member is tens of megabytes; this only has to stop a corrupt
// size (a mod archive with a damaged entry reads back as up to 4 GB) from reaching the allocator.
constexpr unsigned long kMaxInRamImage = 1ul << 30;

bool imageSizePlausible (unsigned long size)
{
	return size <= kMaxInRamImage;
}
} // namespace

struct ModFileEntry {
    std::string path;   // absolute path to real file
    std::string modId;  // owning mod id (for logging)
};

static std::unordered_map<std::string, ModFileEntry> g_modIndex;
// Shadowed-by map: key -> list of mod ids that were overridden at index time.
// Built alongside g_modIndex in IndexModData; queried at level-2 emit.
static std::unordered_map<std::string, std::vector<std::string>> g_shadowedBy;
static std::string g_activeModId;
static std::unordered_map<std::string, int> g_modPriority;
// logFileResolve: 0=off, 1=level-1 stdout tags, 2=level-1+JSONL trace
static int logFileResolve = 0;

// META-FIX design E: resolve counters for the level>=1 shutdown summary
// ("[file-resolve] opens=N misses=M"). Unconditional integer bumps (no
// output unless the gate is on); opens = successful File::open resolutions,
// misses = full-ladder misses from File::open AND fileExists probes.
static unsigned long g_resolveOpens = 0;
static unsigned long g_resolveMisses = 0;

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
            _snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c));
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
static void EmitResolveRecord(
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

    if (shadowed) {
        fprintf(g_traceFile,
            "{\"v\":1,\"t\":%.3f,\"key\":%s,\"req\":%s,\"layer\":%s,\"path\":%s,\"shadowed\":%s}\n",
            t, keyJ, reqJ, layerJ, pathJ, shadowBuf);
    } else {
        fprintf(g_traceFile,
            "{\"v\":1,\"t\":%.3f,\"key\":%s,\"req\":%s,\"layer\":%s,\"path\":%s}\n",
            t, keyJ, reqJ, layerJ, pathJ);
    }
}

static bool IsAbsolutePath(const char* p) {
    if (!p || !p[0]) return false;
    if (p[0] == '\\' || p[0] == '/') return true;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
        return true;
    return false;
}

static bool StartsWithDataPath(const char* p) {
    return _strnicmp(p, "data/", 5) == 0 || _strnicmp(p, "data\\", 5) == 0;
}

static bool ShouldSearchMods(const char* fileName) {
    if (!fileName || !fileName[0])         return false;
    if (IsAbsolutePath(fileName))          return false;
    if (strstr(fileName, "..") != nullptr) return false;
    if (!StartsWithDataPath(fileName))     return false;
    return true;
}

static std::string NormalizeKey(const char* p) {
    // Delegates to the leaf-TU normalizer (mclib/fst_hash.cpp) so the loose-file
    // index key contract is unit-testable game-free (GAMEOS-PATHSEP-HARNESS-1).
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
    _snprintf(pattern, sizeof(pattern), "%s*", dataDir);
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
        _snprintf(fullChild, sizeof(fullChild), "%s%s", dataDir, fd.cFileName);
        fullChild[sizeof(fullChild) - 1] = '\0';
        _snprintf(relChild, sizeof(relChild), "%s%s", relBase, fd.cFileName);
        relChild[sizeof(relChild) - 1] = '\0';

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char fullSub[MAX_PATH], relSub[MAX_PATH];
            _snprintf(fullSub, sizeof(fullSub), "%s/", fullChild);
            _snprintf(relSub,  sizeof(relSub),  "%s/", relChild);
            fullSub[sizeof(fullSub) - 1] = '\0';
            relSub[sizeof(relSub) - 1]   = '\0';
            IndexModData(idx, shadowed, fullSub, relSub, modId, skippedDotCount);
        } else {
            std::string key = NormalizeKey(relChild);
            auto existing = idx.find(key);
            if (existing == idx.end()) {
                idx[key] = { fullChild, modId };
            } else {
                if (logFileResolve) {
                    printf("[mod-dup] %s  winner=[%s]  loser=[%s]\n",
                           key.c_str(), existing->second.modId.c_str(), modId);
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
    _snprintf(pat, sizeof(pat), "%s*", dataDir);
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
            _snprintf(sub, sizeof(sub), "%s%s/", dataDir, fd.cFileName);
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
    const char* cachePath,
    const std::string& modId,
    const ModCacheSignature& sig,
    const std::vector<std::pair<std::string,ModFileEntry>>& entries
) {
    FILE* f = fopen(cachePath, "wb");
    if (!f) return;
    char sigLine[64];
    formatModCacheSigLine(sig, sigLine, sizeof(sigLine));
    fprintf(f, "%s\n", kCacheHeader);
    fprintf(f, "modid %s\n", modId.c_str());
    fprintf(f, "%s\n", sigLine);
    fprintf(f, "count %zu\n", entries.size());
    for (auto& e : entries)
        fprintf(f, "%s\t%s\t%s\n", e.first.c_str(), e.second.path.c_str(), e.second.modId.c_str());
    fclose(f);
}

// Load cache into idx (first-wins, same as IndexModData).
// Returns number of entries loaded, or nullopt on format error.
static std::optional<int> LoadModIndexCache(
    std::unordered_map<std::string, ModFileEntry>& idx,
    const char* cachePath,
    const char* expectedModId,
    const ModCacheSignature& expectedSig,
    bool* sigMismatch   // out: header/modid ok but signature differs (stale)
) {
    if (sigMismatch) *sigMismatch = false;
    FILE* f = fopen(cachePath, "rb");
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
    if (strcmp(line + 6, expectedModId) != 0) { fclose(f); return std::nullopt; }
    // sig (MOD-INDEX-CACHE-MTIME-1): stored tree signature vs the one just
    // recomputed from disk -- ANY mismatch means files changed since index time.
    if (!fgets(line, sizeof(line), f)) { fclose(f); return std::nullopt; }
    line[strcspn(line, "\r\n")] = '\0';
    ModCacheSignature storedSig;
    if (!parseModCacheSigLine(line, &storedSig)) { fclose(f); return std::nullopt; }
    if (!modCacheSignatureMatches(storedSig, expectedSig)) {
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
        std::string modid  = tab2 + 1;
        if (idx.find(key) == idx.end())
            idx[key] = { path, modid };
        loaded++;
    }
    fclose(f);
    return loaded;
}

// Try to load the cache for one mod layer (dataDir, modId) into idx.
// Returns true if cache was valid and loaded, false if a fresh scan is needed.
static bool TryLoadModCache(
    std::unordered_map<std::string, ModFileEntry>& idx,
    const char* absModRoot,   // e.g. "A:/Games/.../mods/"
    const char* modId,        // e.g. "mc2x-pbr"
    const char* dataDir       // e.g. "...mods/mc2x-pbr/data/"
) {
    if (mc2::gate::get<"MC2_REBUILD_MOD_CACHE">()) {
        printf("[mod-cache] refresh modid='%s' reason=MC2_REBUILD_MOD_CACHE\n", modId);
        fflush(stdout);
        return false;
    }

    char cachePath[MAX_PATH];
    _snprintf(cachePath, sizeof(cachePath), "%s%s/.modindex-cache", absModRoot, modId);
    cachePath[sizeof(cachePath)-1] = '\0';

    // MOD-INDEX-CACHE-MTIME-1: recompute the tree signature (one recursive
    // metadata walk, load-time only) and compare against the one stored in
    // the cache. Catches content edits, deep adds/removes, and renames --
    // the v1 "dir mtimes, 2 levels" rule missed all three.
    ModCacheSignature diskSig;
    ComputeModDataSignature(dataDir, &diskSig);

    bool sigMismatch = false;
    auto loaded = LoadModIndexCache(idx, cachePath, modId, diskSig, &sigMismatch);
    if (!loaded) {
        printf("[mod-cache] refresh modid='%s' reason=%s (files=%llu maxMtime=%llu)\n",
               modId,
               sigMismatch ? "signature-mismatch (mod files changed)"
                           : "cache missing/stale-format",
               diskSig.fileCount, diskSig.maxMtime);
        fflush(stdout);
        return false;
    }

    printf("[mod-cache] hit modid='%s' loaded=%d files (sig files=%llu maxMtime=%llu)\n",
           modId, *loaded, diskSig.fileCount, diskSig.maxMtime);
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
    for (auto& [key, entry] : layer)
        if (idx.find(key) == idx.end())
            idx[key] = entry;

    // Collect layer entries for cache (all entries this mod owns in the layer map).
    std::vector<std::pair<std::string,ModFileEntry>> fresh(layer.begin(), layer.end());

    char cachePath[MAX_PATH];
    _snprintf(cachePath, sizeof(cachePath), "%s%s/.modindex-cache", absModRoot, modId);
    cachePath[sizeof(cachePath)-1] = '\0';
    // Signature is taken AFTER the index walk so it reflects the exact tree
    // state the entries were built from (the cache file itself is dot-named ->
    // invisible to the signature walk, so writing it never dirties the sig).
    ModCacheSignature sig;
    ComputeModDataSignature(dataDir, &sig);
    WriteModIndexCache(cachePath, modId, sig, fresh);
    printf("[mod-cache] miss modid='%s' indexed=%zu wrote cache (sig files=%llu maxMtime=%llu)\n",
           modId, fresh.size(), sig.fileCount, sig.maxMtime);
    if (logFileResolve >= 1 && skippedDot > 0)
        printf("[mod] [%s] skipped %d dot-entr%s (ruling C4)\n",
               modId, skippedDot, skippedDot == 1 ? "y" : "ies");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Minimal JSON string field extractor — no full parser needed for mod.json.
static std::string JsonGetString(const char* json, const char* key) {
    char needle[128];
    _snprintf(needle, sizeof(needle), "\"%s\"", key);
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
    _snprintf(needle, sizeof(needle), "\"%s\"", key);
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

struct ModJson { std::string id, name; std::vector<std::string> deps; };

// Read mod.json at jsonPath. Returns nullopt if file not found or has no id.
static std::optional<ModJson> ReadModJson(const char* jsonPath) {
    HANDLE fh = CreateFileA(jsonPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return std::nullopt;
    char buf[4096] = {};
    DWORD read = 0;
    ReadFile(fh, buf, sizeof(buf)-1, &read, nullptr);
    CloseHandle(fh);
    buf[read] = '\0';

    ModJson mj{JsonGetString(buf, "id"), JsonGetString(buf, "name"), JsonGetStringArray(buf, "dependencies")};
    if (mj.id.empty()) return std::nullopt;
    return mj;
}

// O(1) lookup. Returns false immediately when no mod active (g_modIndex empty).
// req: original filename as requested by caller (before lowercasing), used for level-2 trace.
//      May be nullptr — falls back to fileName.
static std::optional<int> TryModOpen(const char* fileName, const char* req = nullptr) {
    if (g_modIndex.empty()) return std::nullopt;
    if (!ShouldSearchMods(fileName)) return std::nullopt;
    std::string key = NormalizeKey(fileName);
    auto it = g_modIndex.find(key);
    if (it == g_modIndex.end()) {
        if (logFileResolve) { printf("[mod-miss] %s\n", key.c_str()); fflush(stdout); }
        // MISS record emitted by the caller after the full ladder, not here.
        return std::nullopt;
    }
    int h = _open(it->second.path.c_str(), _O_RDONLY | _O_BINARY);
    if (h == -1) return std::nullopt;
    if (logFileResolve) {
        printf("[mod-hit] [%s] %s -> %s\n",
               it->second.modId.c_str(), key.c_str(), it->second.path.c_str());
        fflush(stdout);
    }
    if (logFileResolve >= 2) {
        // Build layer string "mod:<id>".
        char layer[256];
        _snprintf(layer, sizeof(layer), "mod:%s", it->second.modId.c_str());
        layer[sizeof(layer)-1] = '\0';
        // Lookup shadowed list for this key.
        auto sit = g_shadowedBy.find(key);
        const std::vector<std::string>* sh = (sit != g_shadowedBy.end()) ? &sit->second : nullptr;
        static const std::vector<std::string> emptyShadow;
        const std::vector<std::string>* shOut = sh ? sh : &emptyShadow;
        EmitResolveRecord(key.c_str(), req ? req : fileName, layer, it->second.path.c_str(), shOut);
    }
    return h;
}

// Returns the owning mod id for a given data path (normalized), or nullptr if not in mod index.
// Used for diagnostic logging only — O(1) lookup.
std::vector<unsigned char> ReadArchiveBytes(const char* dataPath, size_t maxBytes) {
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

int LookupModPriority(const char* dataPath) {
    const char* owner = LookupModOwner(dataPath);
    auto it = owner ? g_modPriority.find(owner) : g_modPriority.end();
    return it == g_modPriority.end() ? 1000000 : it->second;
}

const char* LookupModOwner(const char* dataPath) {
    if (g_modIndex.empty()) return nullptr;
    std::string key = NormalizeKey(dataPath);
    auto it = g_modIndex.find(key);
    if (it == g_modIndex.end()) return nullptr;
    return it->second.modId.c_str();
}

ModFileSource ResolveModFileSource(std::string_view dataPath) {
    ModFileSource source{std::nullopt,g_activeModId.empty()};
    const std::string logical(dataPath);
    if (!ShouldSearchMods(logical.c_str())) return source;
    const auto it = g_modIndex.find(NormalizeKey(logical.c_str()));
    if (it == g_modIndex.end() || _access(it->second.path.c_str(),4) != 0) return source;
    source.path = it->second.path;
    source.editable = it->second.modId == g_activeModId;
    return source;
}

// No-ops — mod is selected at launcher time, not at campaign-select time.
// Kept for call-site compatibility in logisticsdata.cpp / logisticsdialog.cpp / mission.cpp.
void ActivateCampaignMod(const char* /*campaignFitName*/) {}
void ActivateModForMission(const char* /*missionFitKey*/) {}
void DeactivateMod() {}

// Enumerate campaign .fit files from the active mod index.
// Called by logisticsdialog.cpp to inject mod campaigns into the campaign list UI.
// Skips "data/campaign/campaign.fit" (base game manifest) — only individual campaigns.
void EnumerateModCampaignFiles(ModCampaignCallback cb, void* userData) {
    if (g_modIndex.empty()) return;
    static const std::string prefix  = "data/campaign/";
    static const std::string ext     = ".fit";
    static const std::string exclude = "data/campaign/campaign.fit";
    for (auto& [key, entry] : g_modIndex) {
        if (key == exclude) continue;
        if (key.compare(0, prefix.size(), prefix) != 0) continue;
        if (key.size() <= prefix.size() + ext.size()) continue;
        if (key.compare(key.size() - ext.size(), ext.size(), ext) != 0) continue;
        std::string fname = key.substr(prefix.size(), key.size() - prefix.size() - ext.size());
        cb(fname.c_str(), entry.path.c_str(), userData);
    }
}

void EnumerateModFiles(const char* keyPrefix, ModFitCallback cb, void* userData) {
    if (!keyPrefix || !cb) return;
    const std::string prefix = NormalizeKey(keyPrefix);
    std::vector<std::string> keys;
    for (const auto& [key, entry] : g_modIndex)
        if (key.compare(0, prefix.size(), prefix) == 0) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) cb(key.c_str(), userData);
}

void EnumerateModFitFiles(const char* keyPrefix, ModFitCallback cb, void* userData) {
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
    for (auto& h : hits)
        cb(h.second.c_str(), userData);
}

void InitModSearchPaths(const char* modsRoot) {
    g_modIndex.clear();
    g_modPriority.clear();
    g_shadowedBy.clear();
    g_activeModId.clear();

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
                printf("[mod] warning: MC2_RESOLVE_TRACE_FILE=%s could not be opened for append\n", tracePath);
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
                printf("[file-resolve] opens=%lu misses=%lu\n",
                       g_resolveOpens, g_resolveMisses);
                fflush(stdout);
            });
        }
    }

    // MC2_ACTIVE_MOD is re-read here on every mount: the editor's mod picker changes it
    // with _putenv_s at run time, so no reader may cache it in a static.
    const std::string envModStr = mc2::gate::str<"MC2_ACTIVE_MOD">();
    const char* envMod = envModStr.c_str();
    const bool stockMode = !envMod[0];
    if (stockMode) {
        // Stock campaign: no active mod. Add-on layers (MC2_MOD_DEPS) may still mount
        // (e.g. "stock + Omnitech mechs"). With NO deps either, this is pure base game.
        if (mc2::gate::str<"MC2_MOD_DEPS">().empty()) {
            printf("[mod] base game mode (MC2_ACTIVE_MOD not set, MC2_MOD_DEPS not set)\n");
            fflush(stdout);
            return;
        }
        printf("[mod] stock campaign + add-ons mode (MC2_MOD_DEPS set, no MC2_ACTIVE_MOD)\n");
        fflush(stdout);
        envMod = "";  // downstream treats the active mod as absent
    }
    g_activeModId = stockMode ? std::string() : std::string(envMod);

    // Resolve modsRoot to absolute path so index keys stay valid regardless of CWD changes.
    char absRoot[MAX_PATH];
    if (!GetFullPathNameA(modsRoot, sizeof(absRoot), absRoot, nullptr)) {
        printf("[mod] failed to resolve mods root: %s\n", modsRoot); fflush(stdout);
        return;
    }
    size_t rootLen = strlen(absRoot);
    if (rootLen > 0 && absRoot[rootLen-1] != '\\' && absRoot[rootLen-1] != '/') {
        absRoot[rootLen]   = '/';
        absRoot[rootLen+1] = '\0';
    }

    // Read active mod's mod.json for dependencies.
    char jsonPath[MAX_PATH];
    _snprintf(jsonPath, sizeof(jsonPath), "%s%s/mod.json", absRoot, envMod);
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
    } else {
        // No mod.json — treat the folder name as the id. Dependencies (the compat
        // layer) come from MC2_MOD_DEPS instead (see below) — the launcher lets the
        // player pick a campaign + a compatibility layer separately, so metadata-less
        // third-party campaigns (e.g. MCO packs) need no authored mod.json.
        modId   = envMod;
        modName = envMod;
        printf("[mod] no mod.json for '%s'; folder name as id, deps from MC2_MOD_DEPS\n", envMod);
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
    // Priority order: base data < dep[N-1] < ... < dep[0] < active mod.
    // Index is first-wins, so scan highest priority first.
    // Duplicates: winner keeps its slot; loser is logged when MC2_LOG_FILE_RESOLVE=1.
    {
        // Build display string: "base < dep[N-1] < ... < dep[0] < active"
        std::string order = "base";
        for (int i = static_cast<int>(deps.size())-1; i >= 0; i--) order += " < " + deps[i];
        order += " < "; order += modId;
        printf("[mod] load order (lowest to highest priority): %s\n", order.c_str());
        fflush(stdout);
    }

    g_modPriority[modId] = 0;
    for (size_t i = 0; i < deps.size(); ++i) g_modPriority.try_emplace(deps[i], int(i + 1));
    // Index active mod first (highest priority). Skipped entirely in stock mode.
    char activeDataDir[MAX_PATH];
    _snprintf(activeDataDir, sizeof(activeDataDir), "%s%s/data/", absRoot, envMod);
    activeDataDir[sizeof(activeDataDir)-1] = '\0';
    if (!stockMode && GetFileAttributesA(activeDataDir) != INVALID_FILE_ATTRIBUTES) {
        size_t before = g_modIndex.size();
        if (!TryLoadModCache(g_modIndex, absRoot, modId.c_str(), activeDataDir))
            IndexModDataCached(g_modIndex, g_shadowedBy, absRoot, modId.c_str(), activeDataDir, "data/");
        printf("[mod] [1/%zu] active '%s': %zu files\n",
               deps.size()+1, envMod, g_modIndex.size() - before);
    }

    // Index dependencies in declared order (dep[0] > dep[1] > ...).
    for (size_t i = 0; i < deps.size(); i++) {
        const std::string& dep = deps[i];
        char depDataDir[MAX_PATH];
        _snprintf(depDataDir, sizeof(depDataDir), "%s%s/data/", absRoot, dep.c_str());
        depDataDir[sizeof(depDataDir)-1] = '\0';
        if (GetFileAttributesA(depDataDir) == INVALID_FILE_ATTRIBUTES) {
            printf("[mod] dep '%s' not found at %s, skipping\n", dep.c_str(), depDataDir);
            fflush(stdout);
            continue;
        }
        size_t before = g_modIndex.size();
        if (!TryLoadModCache(g_modIndex, absRoot, dep.c_str(), depDataDir))
            IndexModDataCached(g_modIndex, g_shadowedBy, absRoot, dep.c_str(), depDataDir, "data/");
        printf("[mod] [%zu/%zu] dep '%s': %zu new files\n",
               i+2, deps.size()+1, dep.c_str(), g_modIndex.size() - before);
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
            const std::string& k = it->first;  // NormalizeKey: lowercase, '/' separators
            const bool inMissions = (k.rfind("data/missions/", 0) == 0);
            const bool isAbx = (k.size() >= 4 && k.compare(k.size() - 4, 4, ".abx") == 0);
            if (inMissions && isAbx) { it = g_modIndex.erase(it); ++erased; }
            else { ++it; }
        }
        printf("[mod] stock-AI guard: dropped %zu dep ABL override(s) under data/missions/\n", erased);
        fflush(stdout);
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
            if (sit != g_shadowedBy.end()) e.shadowedModIds = sit->second;
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
            printf("[mod] conflict: %s\n", line.c_str());
        fflush(stdout);
    }

    printf("[mod] ready: active='%s' total=%zu files\n", envMod, g_modIndex.size());
    fflush(stdout);
}

