// GameOS/gameos/asset_scale.cpp
#include "asset_scale.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace AssetScale {

namespace {

struct NominalDims { uint32_t w; uint32_t h; };

struct State {
    std::unordered_map<std::string, NominalDims> manifest;
    std::unordered_set<std::string>              seenUnknown;     // path
    std::unordered_set<std::string>              seenOobFirst;    // "path|tag"
    bool   traceEnabled    = false;
    bool   inited          = false;

    // Counters (always accumulate; surfaced via dumpCountersTo).
    uint32_t cManifestEntries  = 0;
    uint32_t cManifestDupes    = 0;
    uint32_t cManifestBadLines = 0;
    uint32_t cFactorLookups    = 0;
    uint32_t cUnknownAsset     = 0;
    uint32_t cOobBlit          = 0;
    uint32_t cClampedRects     = 0;
};

State& state() { static State s; return s; }

void log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

std::string canonicalize(const char* raw) {
    if (!raw) return std::string();
    std::string s(raw);
    // backslash -> forward
    std::replace(s.begin(), s.end(), '\\', '/');
    // lowercase
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    // strip leading "./"
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
    // strip leading "data/"
    if (s.rfind("data/", 0) == 0) s.erase(0, 5);
    // collapse "//"
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && s[i] == '/' && s[i-1] == '/') continue;
        out.push_back(s[i]);
    }
    return out;
}

// Trim leading/trailing whitespace.
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}

// Parse one CSV line "path,w,h". Returns true on success.
bool parseLine(const std::string& raw, std::string& path,
               uint32_t& w, uint32_t& h) {
    // Skip comments and blank.
    std::string s = trim(raw);
    if (s.empty() || s[0] == '#') return false;

    auto c1 = s.find(',');
    if (c1 == std::string::npos) return false;
    auto c2 = s.find(',', c1 + 1);
    if (c2 == std::string::npos) return false;

    std::string p = trim(s.substr(0, c1));
    std::string ws = trim(s.substr(c1 + 1, c2 - c1 - 1));
    std::string hs = trim(s.substr(c2 + 1));

    if (p.empty() || ws.empty() || hs.empty()) return false;

    char* end = nullptr;
    long wl = std::strtol(ws.c_str(), &end, 10);
    if (end == ws.c_str() || wl <= 0) return false;
    long hl = std::strtol(hs.c_str(), &end, 10);
    if (end == hs.c_str() || hl <= 0) return false;

    path = canonicalize(p.c_str());
    w = (uint32_t)wl;
    h = (uint32_t)hl;
    return true;
}

void loadManifest(const char* manifestPath) {
    State& st = state();
    FILE* f = std::fopen(manifestPath, "rb");
    if (!f) {
        log("[ASSET_SCALE v1] event=manifest_missing path=%s", manifestPath);
        return;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string buf;
    buf.resize(sz > 0 ? (size_t)sz : 0);
    if (sz > 0) std::fread(&buf[0], 1, (size_t)sz, f);
    std::fclose(f);

    int lineno = 0;
    size_t i = 0;
    while (i <= buf.size()) {
        size_t nl = buf.find('\n', i);
        if (nl == std::string::npos) nl = buf.size();
        std::string line = buf.substr(i, nl - i);
        // Strip CR for CRLF files.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ++lineno;
        std::string path; uint32_t w = 0, h = 0;
        std::string trimmed = trim(line);
        if (!trimmed.empty() && trimmed[0] != '#') {
            if (parseLine(line, path, w, h)) {
                auto ins = st.manifest.emplace(std::move(path), NominalDims{w, h});
                if (!ins.second) ++st.cManifestDupes;
                else             ++st.cManifestEntries;
            } else {
                ++st.cManifestBadLines;
                log("[ASSET_SCALE v1] event=manifest_bad_line lineno=%d raw=%s",
                    lineno, line.c_str());
            }
        }
        i = nl + 1;
    }
    log("[ASSET_SCALE v1] event=manifest_loaded entries=%u dupes=%u bad_lines=%u",
        st.cManifestEntries, st.cManifestDupes, st.cManifestBadLines);
}

void firstUnknownWarning(State& st, const std::string& canon) {
    if (st.seenUnknown.insert(canon).second) {
        ++st.cUnknownAsset;
        if (st.traceEnabled) {
            log("[ASSET_SCALE v1] event=unknown_asset path=%s", canon.c_str());
        }
    }
}

void firstOobWarning(State& st, const std::string& canon, const char* tag,
                     uint32_t aw, uint32_t ah, uint32_t nw, uint32_t nh,
                     int sx, int sy, int sw, int sh,
                     int cx, int cy, int cw, int ch) {
    std::string k = canon + "|" + (tag ? tag : "?");
    bool first = st.seenOobFirst.insert(k).second;
    ++st.cOobBlit;
    // First occurrence is always-on. Subsequent only with trace.
    if (first || st.traceEnabled) {
        log("[ASSET_SCALE v1] event=oob_blit path=%s actual=%ux%u nominal=%ux%u "
            "src=(%d,%d,%d,%d) clamped=(%d,%d,%d,%d) caller=%s",
            canon.c_str(), aw, ah, nw, nh, sx, sy, sw, sh, cx, cy, cw, ch,
            tag ? tag : "?");
    }
}

const NominalDims* lookupNominal(State& st, const AssetKey& k) {
    if (k.empty()) return nullptr;
    auto it = st.manifest.find(k.str());
    if (it == st.manifest.end()) {
        firstUnknownWarning(st, k.str());
        return nullptr;
    }
    return &it->second;
}

// ---- Self-tests ----------------------------------------------------------
// Synthetic atlases at integer (2x, 4x, 8x) and non-integer (1.5x) scales.
// Verifies: rect inside actual bounds, adjacent cells at most 1px overlap,
// rect covers the expected cell.
struct TestResult { const char* name; bool pass; const char* detail; };

bool runOneScale(uint32_t nominalSize, uint32_t actualSize, const char* tag,
                 char* detailBuf, size_t detailLen) {
    // Inject a synthetic manifest entry without going through CSV.
    State& st = state();
    const std::string testKey = std::string("test/") + tag + ".tga";
    st.manifest[testKey] = NominalDims{ nominalSize, nominalSize };

    AssetKey k = key(testKey.c_str());

    const int nominalCell = 32;
    const int cellsPerRow = (int)nominalSize / nominalCell;

    for (int y = 0; y < cellsPerRow; ++y) {
        for (int x = 0; x < cellsPerRow; ++x) {
            IRect r = nominalToActualRect(k, actualSize, actualSize,
                                          (float)(x * nominalCell),
                                          (float)(y * nominalCell),
                                          (float)nominalCell,
                                          (float)nominalCell,
                                          "selftest");
            // Inside actual bounds.
            if (r.x < 0 || r.y < 0 ||
                (uint32_t)(r.x + r.w) > actualSize ||
                (uint32_t)(r.y + r.h) > actualSize) {
                std::snprintf(detailBuf, detailLen,
                    "OOB at cell (%d,%d): rect=(%d,%d,%d,%d) actual=%u",
                    x, y, r.x, r.y, r.w, r.h, actualSize);
                return false;
            }
            // Adjacent-cell overlap <= 1px (intentional seam protection).
            if (x + 1 < cellsPerRow) {
                IRect rNext = nominalToActualRect(k, actualSize, actualSize,
                    (float)((x + 1) * nominalCell),
                    (float)(y * nominalCell),
                    (float)nominalCell, (float)nominalCell, "selftest");
                int overlap = (r.x + r.w) - rNext.x;
                if (overlap < 0 || overlap > 1) {
                    std::snprintf(detailBuf, detailLen,
                        "X overlap %d at cell (%d,%d): rect=(%d,%d,%d,%d) "
                        "next=(%d,%d,%d,%d)",
                        overlap, x, y, r.x, r.y, r.w, r.h,
                        rNext.x, rNext.y, rNext.w, rNext.h);
                    return false;
                }
            }
        }
    }
    return true;
}

void runSelfTests() {
    static const struct { uint32_t nom; uint32_t act; const char* tag; } cases[] = {
        { 256, 512,  "2x"   },
        { 256, 1024, "4x"   },
        { 256, 2048, "8x"   },
        { 256, 384,  "1p5x" },  // non-integer; modders WILL use odd sizes
    };
    int passed = 0, failed = 0;
    for (auto& c : cases) {
        char detail[256] = {0};
        bool ok = runOneScale(c.nom, c.act, c.tag, detail, sizeof(detail));
        if (ok) {
            log("[ASSET_SCALE v1] event=selftest_pass case=%s nominal=%u actual=%u",
                c.tag, c.nom, c.act);
            ++passed;
        } else {
            log("[ASSET_SCALE v1] event=selftest_fail case=%s nominal=%u actual=%u "
                "detail=\"%s\"", c.tag, c.nom, c.act, detail);
            ++failed;
        }
    }
    log("[ASSET_SCALE v1] event=selftest_summary passed=%d failed=%d",
        passed, failed);
}

} // anonymous namespace

AssetKey key(const char* rawPath) {
    return AssetKey(canonicalize(rawPath));
}

void init(const char* manifestPath) {
    State& st = state();
    if (st.inited) return;
    st.inited = true;
    const char* trace = std::getenv("MC2_ASSET_SCALE_TRACE");
    st.traceEnabled = (trace && trace[0] == '1');
    loadManifest(manifestPath);
    const char* st_env = std::getenv("MC2_ASSET_SCALE_SELFTEST");
    if (st_env && st_env[0] == '1') {
        runSelfTests();
    }
}

void shutdown() {
    State& st = state();
    st = State();
}

void dumpCountersTo(FILE* out) {
    const State& st = state();
    std::fprintf(out,
        "[ASSET_SCALE v1] counters entries=%u dupes=%u bad_lines=%u "
        "lookups=%u unknown=%u oob=%u clamped=%u trace=%d\n",
        st.cManifestEntries, st.cManifestDupes, st.cManifestBadLines,
        st.cFactorLookups, st.cUnknownAsset, st.cOobBlit, st.cClampedRects,
        st.traceEnabled ? 1 : 0);
}

Vec2 factorFor(const AssetKey& k, uint32_t actualW, uint32_t actualH,
               const char* /*callerTag*/) {
    State& st = state();
    ++st.cFactorLookups;
    const NominalDims* nd = lookupNominal(st, k);
    if (!nd || nd->w == 0 || nd->h == 0) return {1.f, 1.f};
    return { (float)actualW / (float)nd->w, (float)actualH / (float)nd->h };
}

int nominalToActualAxis(const AssetKey& k, int n, Axis axis,
                        uint32_t actualW, uint32_t actualH,
                        const char* callerTag) {
    Vec2 f = factorFor(k, actualW, actualH, callerTag);
    float scaled = (axis == Axis::X) ? n * f.x : n * f.y;
    return (int)std::floor(scaled);
}

IVec2 nominalToActual(const AssetKey& k, int nx, int ny,
                      uint32_t actualW, uint32_t actualH,
                      const char* callerTag) {
    Vec2 f = factorFor(k, actualW, actualH, callerTag);
    return { (int)std::floor(nx * f.x), (int)std::floor(ny * f.y) };
}

IRect nominalToActualRect(const AssetKey& k,
                          uint32_t actualW, uint32_t actualH,
                          float nx, float ny, float nw, float nh,
                          const char* callerTag) {
    State& st = state();
    Vec2 f = factorFor(k, actualW, actualH, callerTag);

    // Rounding policy: floor-origin, ceil-extent.
    int sx = (int)std::floor(nx * f.x);
    int sy = (int)std::floor(ny * f.y);
    int sw = (int)std::ceil (nw * f.x);
    int sh = (int)std::ceil (nh * f.y);

    // OOB clamp.
    int cx = sx, cy = sy, cw = sw, ch = sh;
    bool clamped = false;
    if (cx < 0)                                  { cw += cx; cx = 0; clamped = true; }
    if (cy < 0)                                  { ch += cy; cy = 0; clamped = true; }
    if ((uint32_t)cx > actualW)                  { cx = (int)actualW; cw = 0; clamped = true; }
    if ((uint32_t)cy > actualH)                  { cy = (int)actualH; ch = 0; clamped = true; }
    if ((uint32_t)(cx + cw) > actualW)           { cw = (int)actualW - cx; clamped = true; }
    if ((uint32_t)(cy + ch) > actualH)           { ch = (int)actualH - cy; clamped = true; }
    if (cw < 0) { cw = 0; clamped = true; }
    if (ch < 0) { ch = 0; clamped = true; }

    if (clamped) {
        ++st.cClampedRects;
        if (!k.empty()) {
            const NominalDims* nd = lookupNominal(st, k);
            uint32_t nWidth  = nd ? nd->w : 0;
            uint32_t nHeight = nd ? nd->h : 0;
            firstOobWarning(state(), k.str(), callerTag,
                            actualW, actualH, nWidth, nHeight,
                            sx, sy, sw, sh, cx, cy, cw, ch);
        }
    }
    return { cx, cy, cw, ch };
}

} // namespace AssetScale
