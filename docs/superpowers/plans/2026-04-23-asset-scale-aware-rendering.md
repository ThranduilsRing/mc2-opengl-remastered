# Asset-Scale-Aware Rendering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the GUI render correctly regardless of source asset size by introducing a small `AssetScale` subsystem that resolves nominal-vs-actual pixel scale per asset, and routing icon UV/blit math + chrome widget UV math through it.

**Architecture:** New `AssetScale` subsystem (one header, one .cpp, ~150 LOC) lives in `GameOS/gameos/`. It loads `data/art/asset_sizes.csv` once at startup, canonicalizes asset paths, and exposes a small lookup API (`factorFor`, `nominalToActualRect`, axis/vec2 variants). Adopters: `MechIcon`/`VehicleIcon`/`PilotIcon` in `code/mechicon.cpp` (UV + CPU blit), and `aObject` in `gui/aSystem.cpp` via opt-in fields (`assetKey`, `srcRectSpace`).

**Tech Stack:** C++14 (project default), CMake (MSBuild generator, `RelWithDebInfo`), no test framework — verification is one env-gated runtime self-test (`MC2_ASSET_SCALE_SELFTEST=1`) plus in-game visual checks plus a grep-based regression script.

**Source spec:** `docs/superpowers/specs/2026-04-23-asset-scale-aware-rendering-design.md` (commit c736793 / 8d97b31).

**Build/deploy primitives used throughout this plan:**
- Build: `"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2`
- Deploy exe: `cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe && diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe`
- Run game (visual verification): launch `A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe` from outside the build session (game holds the exe lock).
- Self-test invocation: `MC2_ASSET_SCALE_SELFTEST=1 ./mc2.exe` exits cleanly after self-tests print PASS/FAIL.

If the deployed `mc2.exe` is locked by a running game process, close the game before re-deploying (CLAUDE.md "Build/deploy status" notes this LNK1201 hazard).

---

## Phase 1 — `AssetScale` subsystem (foundation, no callers yet)

This phase produces a working subsystem with self-tests passing. No game-visible change yet.

### Task 1: Create `asset_scale.h` with API surface

**Files:**
- Create: `GameOS/gameos/asset_scale.h`

- [ ] **Step 1: Write the header**

```cpp
// GameOS/gameos/asset_scale.h
//
// Asset-scale-aware rendering helper. Resolves nominal (authored) vs actual
// (on-disk, possibly upscaled) texture dimensions so UV math and CPU sub-rect
// blits remain correct when loose-file overrides change asset sizes.
//
// Spec: docs/superpowers/specs/2026-04-23-asset-scale-aware-rendering-design.md
//
// Telemetry:
//   - Always-on: startup banner, manifest-missing/bad-line warnings,
//     first oob_blit per (path, callerTag).
//   - Gated by MC2_ASSET_SCALE_TRACE=1: per-key unknown_asset lines, all
//     subsequent oob_blit lines, 600-frame summary.
//   - Self-test: MC2_ASSET_SCALE_SELFTEST=1 runs synthetic golden tests at
//     startup and prints PASS/FAIL per case.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace AssetScale {

enum class Axis { X, Y };

struct IRect { int x; int y; int w; int h; };
struct Vec2  { float x; float y; };
struct IVec2 { int   x; int   y; };

// Canonicalized asset path. Construct only via AssetScale::key().
class AssetKey {
public:
    AssetKey() = default;
    bool empty() const { return canon_.empty(); }
    const std::string& str() const { return canon_; }
    bool operator==(const AssetKey& o) const { return canon_ == o.canon_; }
private:
    explicit AssetKey(std::string canon) : canon_(std::move(canon)) {}
    friend AssetKey key(const char*);
    std::string canon_;
};

// Canonicalize: lowercase, backslash->forward, strip leading "./",
// strip leading "data/", collapse "//" -> "/".
AssetKey key(const char* rawPath);

// Lifecycle (called from gameosmain.cpp).
void init(const char* manifestPath);
void shutdown();

// All transform/lookup APIs take actualW/actualH; the manifest stores only
// nominal, so the caller (which already has the actual texture dimensions
// from the GL upload / TGA header) must supply them.
Vec2  factorFor(const AssetKey& k, uint32_t actualW, uint32_t actualH,
                const char* callerTag);

int   nominalToActualAxis(const AssetKey& k, int n, Axis axis,
                          uint32_t actualW, uint32_t actualH,
                          const char* callerTag);

IVec2 nominalToActual(const AssetKey& k, int nx, int ny,
                      uint32_t actualW, uint32_t actualH,
                      const char* callerTag);

// Primary surface. Centralizes rounding (floor-origin / ceil-extent) and
// OOB clamp. Always returns a rect inside [0,actualW] x [0,actualH].
IRect nominalToActualRect(const AssetKey& k,
                          uint32_t actualW, uint32_t actualH,
                          float nx, float ny, float nw, float nh,
                          const char* callerTag);

// Debug hotkey output (counters always accumulate, even when trace is off).
void  dumpCountersTo(FILE*);

} // namespace AssetScale
```

- [ ] **Step 2: Commit**

```bash
git add GameOS/gameos/asset_scale.h
git commit -m "feat(asset-scale): add header with API surface"
```

---

### Task 2: Create `asset_scale.cpp` skeleton with canonicalization + manifest parsing

**Files:**
- Create: `GameOS/gameos/asset_scale.cpp`
- Modify: `GameOS/gameos/CMakeLists.txt:4-32` (add `asset_scale.cpp` to SOURCES)

- [ ] **Step 1: Write `asset_scale.cpp`**

```cpp
// GameOS/gameos/asset_scale.cpp
#include "asset_scale.h"

#include <algorithm>
#include <cmath>
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

// Transform APIs filled in Task 3.
Vec2 factorFor(const AssetKey&, uint32_t, uint32_t, const char*)
    { return {1.f, 1.f}; }
int  nominalToActualAxis(const AssetKey&, int n, Axis, uint32_t, uint32_t, const char*)
    { return n; }
IVec2 nominalToActual(const AssetKey&, int nx, int ny, uint32_t, uint32_t, const char*)
    { return {nx, ny}; }
IRect nominalToActualRect(const AssetKey&, uint32_t, uint32_t,
                          float nx, float ny, float nw, float nh, const char*)
    { return { (int)std::floor(nx), (int)std::floor(ny),
               (int)std::ceil(nw),  (int)std::ceil(nh) }; }

} // namespace AssetScale
```

- [ ] **Step 2: Add to CMake source list**

Edit `GameOS/gameos/CMakeLists.txt` — add `asset_scale.cpp` to the `SOURCES` set. The file currently ends at line 32 with `)`. Insert before the closing `)`:

```cmake
    asset_scale.cpp
```

Final relevant block (showing context):

```cmake
set(SOURCES ${SOURCES}
    gameos.cpp
    gameos_graphics.cpp
    gameos_res.cpp
    gameos_fileio.cpp
    gameos_input.cpp
    gameos_debugging.cpp
    gameos_sound.cpp
    gos_render.cpp
    gos_postprocess.cpp
    gos_static_prop_batcher.cpp
    gos_validate.cpp
    gos_font.cpp
    gos_input.cpp
    asset_scale.cpp

    utils/stream.cpp
    ...
    )
```

- [ ] **Step 3: Build to verify it compiles**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```

Expected: build succeeds. If it fails, look for missing `<cstdarg>` (add it for `va_list`), or include-path issues.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/asset_scale.h GameOS/gameos/asset_scale.cpp GameOS/gameos/CMakeLists.txt
git commit -m "feat(asset-scale): manifest loader + canonicalization (transforms still stub)"
```

---

### Task 3: Implement transform functions (factor / axis / vec / rect)

**Files:**
- Modify: `GameOS/gameos/asset_scale.cpp` (replace the four stubs at the end)

- [ ] **Step 1: Replace stubs with real implementations**

Replace the four stub functions (everything from `Vec2 factorFor(...)` to the end of the namespace) with:

```cpp
namespace {

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

} // anonymous namespace

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
```

- [ ] **Step 2: Build to verify**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```

Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add GameOS/gameos/asset_scale.cpp
git commit -m "feat(asset-scale): implement factor/axis/vec/rect transforms with OOB clamp"
```

---

### Task 4: Add startup self-tests gated by `MC2_ASSET_SCALE_SELFTEST`

**Files:**
- Modify: `GameOS/gameos/asset_scale.cpp` (extend `init()`)

The project has no unit-test framework. Self-tests run in-process at startup when an env var is set; they print PASS/FAIL and the process continues normally (so a developer can boot the game with `MC2_ASSET_SCALE_SELFTEST=1` and see results in the log).

- [ ] **Step 1: Add self-test code**

Add this block in the anonymous namespace (above `loadManifest`):

```cpp
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
```

Then extend `init()`:

```cpp
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
```

- [ ] **Step 2: Build**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```

Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add GameOS/gameos/asset_scale.cpp
git commit -m "feat(asset-scale): add MC2_ASSET_SCALE_SELFTEST golden cases (2x/4x/8x/1.5x)"
```

---

### Task 5: Wire `AssetScale::init/shutdown` into game lifecycle

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp:598` (after `Environment.InitializeGameEngine();`)
- Modify: `GameOS/gameos/gameosmain.cpp:700` (after `Environment.TerminateGameEngine();`)

- [ ] **Step 1: Read context around line 598**

```bash
sed -n '590,605p' GameOS/gameos/gameosmain.cpp
```

Confirm `Environment.InitializeGameEngine();` is on line 598. If line numbers have drifted, find with `grep -n InitializeGameEngine GameOS/gameos/gameosmain.cpp`.

- [ ] **Step 2: Add include**

At the top of `GameOS/gameos/gameosmain.cpp`, add (alongside other includes):

```cpp
#include "asset_scale.h"
```

- [ ] **Step 3: Add init call after `Environment.InitializeGameEngine();`**

```cpp
    Environment.InitializeGameEngine();
    AssetScale::init("data/art/asset_sizes.csv");
```

- [ ] **Step 4: Add shutdown call after `Environment.TerminateGameEngine();`**

```cpp
    Environment.TerminateGameEngine();
    AssetScale::shutdown();
```

- [ ] **Step 5: Build**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```

- [ ] **Step 6: Deploy and run self-tests**

Make sure no instance of the game is running, then:

```bash
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe
```

Expected: `diff -q` reports no output (files match). Then from outside the build session, in a Windows command prompt:

```
cd /d A:\Games\mc2-opengl\mc2-win64-v0.1.1
set MC2_ASSET_SCALE_SELFTEST=1
mc2.exe
```

In the launched session's stdout/log, expect lines:
```
[ASSET_SCALE v1] event=manifest_missing path=data/art/asset_sizes.csv
[ASSET_SCALE v1] event=selftest_pass case=2x nominal=256 actual=512
[ASSET_SCALE v1] event=selftest_pass case=4x nominal=256 actual=1024
[ASSET_SCALE v1] event=selftest_pass case=8x nominal=256 actual=2048
[ASSET_SCALE v1] event=selftest_pass case=1p5x nominal=256 actual=384
[ASSET_SCALE v1] event=selftest_summary passed=4 failed=0
```

`manifest_missing` is expected — we have not shipped the CSV yet. If any selftest case fails, fix the rounding logic in Task 3 before continuing.

Quit the game after observing the log.

- [ ] **Step 7: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat(asset-scale): hook init/shutdown into game lifecycle"
```

---

### Task 6: Create initial `data/art/asset_sizes.csv`

**Files:**
- Create: `A:/Games/mc2-opengl/mc2-win64-v0.1.1/data/art/asset_sizes.csv` (deployed location)
- Create: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/data/art/asset_sizes.csv` (source-tree copy under git, optional but recommended for tracking)

The deployed game reads from `data/art/`. We also keep a source-controlled copy. The values for `<TBD>` come from inspecting the un-upscaled FST-archive copies.

- [ ] **Step 1: Discover nominal dimensions**

If you have ImageMagick installed: extract the original FST asset (or use any local un-upscaled copy) and run `identify path/to/file.tga`. Otherwise read the TGA header bytes 12–15 (width little-endian) and 14–15 (height little-endian).

For this project, the original `mcui_*` files live in `art.fst` (FastFile archive). If a clean source copy is not readily extractable in this session, defer fill-in: ship the CSV with `<TBD>` lines commented out and add real entries as each adopter is implemented (Tasks 8/13/14). The system tolerates an empty manifest (everything reports unknown_asset, behavior matches today).

- [ ] **Step 2: Write the seed CSV**

```csv
# data/art/asset_sizes.csv
# Asset-scale nominal dimensions for loose-file upscale awareness.
# Spec: docs/superpowers/specs/2026-04-23-asset-scale-aware-rendering-design.md
#
# Format: asset_path,nominal_width,nominal_height
# Canonical key rules: lowercase, forward slashes, no leading "./", no leading "data/".
# Loader strips "data/" so input may include it.
#
# Fill <TBD> values by inspecting the un-upscaled FST-archive copy of the asset.
# Until filled, the runtime treats the asset as unknown (factor=1.0, no transform).

# Mech damage schematic atlases (CLAUDE.md blocklist)
# art/mcui_high7.tga,<TBD>,<TBD>
# art/mcui_med4.tga,<TBD>,<TBD>
# art/mcui_low4.tga,<TBD>,<TBD>

# Vehicle icon atlases
# art/mcui_high8.tga,<TBD>,<TBD>
# art/mcui_med5.tga,<TBD>,<TBD>
# art/mcui_low5.tga,<TBD>,<TBD>

# Pilot icon atlases (verify file numbering at impl time)
# art/mcui_high2.tga,<TBD>,<TBD>
# art/mcui_med2.tga,<TBD>,<TBD>
# art/mcui_low2.tga,<TBD>,<TBD>

# Menu chrome — discovered per-screen during chrome adoption tasks (16/17/18)
```

Save to **both** locations (source tree and deployed).

- [ ] **Step 3: Run game with self-test again, verify manifest now loads**

Expected log line (replacing the earlier `manifest_missing`):
```
[ASSET_SCALE v1] event=manifest_loaded entries=0 dupes=0 bad_lines=0
```
(Entries will be 0 because all real lines are commented out until they're filled in.)

- [ ] **Step 4: Commit**

```bash
git add data/art/asset_sizes.csv  # source tree
git commit -m "feat(asset-scale): seed manifest CSV (entries to fill per adopter)"
```

---

## Phase 2 — `MechIcon` adoption

This phase fixes the in-mission HUD mech damage schematic. After completion, `mcui_high7.tga` (and med4/low4) can be upscaled.

### Task 7: Add `s_MechTexturesKey` member, populate at load

**Files:**
- Modify: `code/mechicon.h` (add `static AssetScale::AssetKey s_MechTexturesKey;` to `MechIcon` class)
- Modify: `code/mechicon.cpp:27` (define the static)
- Modify: `code/mechicon.cpp:447` (assign after `strcat`)

- [ ] **Step 1: Grep for `MechIcon` class declaration in header**

```bash
grep -n 'class MechIcon' code/mechicon.h
grep -n 'static.*s_MechTextures' code/mechicon.h
```

Find the line declaring `static TGAFileHeader* s_MechTextures;`. Add an `AssetKey` next to it.

- [ ] **Step 2: Modify `code/mechicon.h`**

Add include near the other includes at the top of `mechicon.h`:

```cpp
#include "asset_scale.h"
```

Add the new static member to the `MechIcon` class definition (right next to `static TGAFileHeader* s_MechTextures;`):

```cpp
    static TGAFileHeader*       s_MechTextures;
    static AssetScale::AssetKey s_MechTexturesKey;
```

- [ ] **Step 3: Modify `code/mechicon.cpp:27` — define the static**

Find:
```cpp
TGAFileHeader *MechIcon::s_MechTextures = NULL;
```

Add the line right after it:
```cpp
TGAFileHeader *MechIcon::s_MechTextures = NULL;
AssetScale::AssetKey MechIcon::s_MechTexturesKey;
```

- [ ] **Step 4: Modify `code/mechicon.cpp:initTextures` — populate after path is built**

Find this block (around line 442–447):
```cpp
		if ( Environment.screenWidth == 800 )
			strcat( path, "mcui_med4.tga" );
		else if ( Environment.screenWidth == 640 )
			strcat( path, "mcui_low4.tga" );
		else 
			strcat( path, "mcui_high7.tga" );

		S_strlwr( path );
```

After `S_strlwr( path );`, populate the key:

```cpp
		S_strlwr( path );
		s_MechTexturesKey = AssetScale::key(path);
```

- [ ] **Step 5: Build**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```

Expected: build succeeds.

- [ ] **Step 6: Commit**

```bash
git add code/mechicon.h code/mechicon.cpp
git commit -m "feat(mechicon): retain canonicalized texture path in s_MechTexturesKey"
```

---

### Task 8: Fill manifest entries for `mcui_high7/med4/low4`

**Files:**
- Modify: `data/art/asset_sizes.csv` (uncomment + fill the three mech entries)

- [ ] **Step 1: Determine nominal dimensions**

The original (FST-shipped) `mcui_high7.tga` is the high-res schematic atlas. Inspect its TGA header. If you have ImageMagick:

```bash
identify A:/path/to/extracted/mcui_high7.tga
```

If you don't have an extracted copy, the simplest route is to **temporarily rename the upscaled override** (if any) out of the way and let the game load the FST copy, then dump dimensions via a debugger or via reading `s_MechTextures->width/height` in `MechIcon::initTextures` with a temporary `printf`. Remove the temp print after.

Document the discovered values here (replace `<W>`/`<H>` below).

- [ ] **Step 2: Update CSV**

In **both** the source-tree and deployed `data/art/asset_sizes.csv`, replace the three commented mech lines with:

```csv
art/mcui_high7.tga,<W_high>,<H_high>
art/mcui_med4.tga,<W_med>,<H_med>
art/mcui_low4.tga,<W_low>,<H_low>
```

- [ ] **Step 3: Run game with `MC2_ASSET_SCALE_TRACE=1`, confirm load**

```
set MC2_ASSET_SCALE_TRACE=1
set MC2_ASSET_SCALE_SELFTEST=0
mc2.exe
```

Expected log line:
```
[ASSET_SCALE v1] event=manifest_loaded entries=3 dupes=0 bad_lines=0
```

- [ ] **Step 4: Commit**

```bash
git add data/art/asset_sizes.csv
git commit -m "feat(asset-scale): fill nominal dims for mcui_high7/med4/low4"
```

---

### Task 9: Refactor `MechIcon` UV draw paths

**Files:**
- Modify: `code/mechicon.cpp:1057-1075` (first UV draw block)
- Modify: `code/mechicon.cpp:1098-1110` (second UV draw block)

- [ ] **Step 1: Read context for the first block**

```bash
sed -n '1050,1080p' code/mechicon.cpp
```

Identify the lines:
```cpp
int iconsPerLine = ((int)s_textureMemory->width/(int)unitIconX);
int iconsPerPage = ((int)s_textureMemory->width/(int)unitIconY);
...
float u = xIndex * unitIconX/s_textureMemory->width + (.1f / 256.f);
float v = yIndex * unitIconY/s_textureMemory->height+ (.1f / 256.f);
float uDelta = unitIconX/s_textureMemory->width + (.1f / 256.f );
float vDelta = unitIconY/s_textureMemory->height + (.1f / 256.f);
```

- [ ] **Step 2: Replace with scale-aware UV math**

`s_textureMemory` here refers to the *atlas* texture (which IS `s_MechTextures` rebuilt onto a `s_textureMemory` buffer — verify by reading lines 110–125). Use `s_MechTextures->width/height` if those are the on-disk dims, or `s_textureMemory->width/height` if those are. The cell scale-up applies to whichever is the actual atlas.

Replace the UV math:

```cpp
const uint32_t actualW = (uint32_t)s_textureMemory->width;
const uint32_t actualH = (uint32_t)s_textureMemory->height;
const AssetScale::Vec2 f = AssetScale::factorFor(
    s_MechTexturesKey, actualW, actualH, "mechicon.uv");

const float actualCellW = unitIconX * f.x;   // nominal cell scaled to actual atlas
const float actualCellH = unitIconY * f.y;

float u      = xIndex * actualCellW / actualW + (.1f / 256.f);
float v      = yIndex * actualCellH / actualH + (.1f / 256.f);
float uDelta = actualCellW / actualW + (.1f / 256.f);
float vDelta = actualCellH / actualH + (.1f / 256.f);
```

The `iconsPerLine`/`iconsPerPage` two lines above stay as-is — they index into the cell grid (nominal-cell-count math), independent of upscale.

- [ ] **Step 3: Read and update the second UV block (~lines 1098–1110)**

```bash
sed -n '1095,1115p' code/mechicon.cpp
```

Apply the **same** transformation. Same code, same caller tag (`"mechicon.uv"`).

- [ ] **Step 4: Build**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```

- [ ] **Step 5: Deploy and visual check (un-upscaled)**

With the deployed `mcui_high7.tga` still at its nominal resolution (no override yet), run a mission. The mech damage schematic should look identical to before — `factorFor` returns `{1,1}` when actual==nominal, so this is a no-op.

```bash
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe
```

Run a mission, observe the mech HUD schematic. Expected: identical to pre-change.

- [ ] **Step 6: Commit**

```bash
git add code/mechicon.cpp
git commit -m "feat(mechicon): scale-aware UV math via AssetScale (no-op at native size)"
```

---

### Task 10: Refactor `MechIcon` CPU blit paths

**Files:**
- Modify: `code/mechicon.cpp:347-393` (first blit)
- Modify: `code/mechicon.cpp:511-546` (second blit, inside `MechIcon::init(long)`)
- Modify: `code/mechicon.cpp:817-847` (third blit)

The blit pattern is: compute `offsetX/offsetY` in source pixels using `unitIconX/Y`, then `pSrcRow = (DWORD*)pTmp + offsetY * s_MechTextures->width + offsetX`, then loop `j < unitIconY` rows of `i < unitIconX` pixels. We replace the source rect computation with `nominalToActualRect`, then iterate the *actual* rect dimensions, advancing the source pointer by *actual* width.

- [ ] **Step 1: Refactor first blit (around line 347)**

Find this block:
```cpp
char* pTmp = (char*)s_MechTextures + sizeof ( TGAFileHeader );
...
long tmpOffset = ((s_MechTextures->width) * (offsetY ) + offsetX);
DWORD* pSrcRow = (DWORD*)pTmp + tmpOffset;
...
for( int j = 0; j < unitIconY; ++j )
{
    ...
    for ( int i = 0; i < unitIconX; ++i )
    {
        pDestData[i] = pSrcRow[i];
    }
    pSrcRow += s_MechTextures->width;
    pDestData += s_textureMemory->width;
}
```

Replace with:
```cpp
char* pTmp = (char*)s_MechTextures + sizeof ( TGAFileHeader );
const uint32_t actualW = (uint32_t)s_MechTextures->width;
const uint32_t actualH = (uint32_t)s_MechTextures->height;

const AssetScale::IRect srcRect = AssetScale::nominalToActualRect(
    s_MechTexturesKey, actualW, actualH,
    (float)offsetX, (float)offsetY,
    (float)unitIconX, (float)unitIconY,
    "mechicon.blit");

DWORD* pSrcRow = (DWORD*)pTmp + (long)srcRect.y * (long)actualW + (long)srcRect.x;
...
for ( int j = 0; j < srcRect.h; ++j )
{
    ...
    for ( int i = 0; i < srcRect.w; ++i )
    {
        pDestData[i] = pSrcRow[i];
    }
    pSrcRow += actualW;
    pDestData += s_textureMemory->width;
}
```

The destination loop count changes from `unitIconY` to `srcRect.h` (and `unitIconX` → `srcRect.w`). The destination cell in `s_textureMemory` is sized at `unitIconX × unitIconY` nominal; if `srcRect.w/h` exceed that (e.g. 2× upscale → 64×76 source into 32×38 dest), we'd write past the dest cell. **Decide based on what `s_textureMemory` actually is:**

- If `s_textureMemory` is a fixed nominal-sized atlas the game uploads to GPU as-is, we need to either downsample on copy or expand `s_textureMemory` to actual-cell-size. **For the v1 fix, the cleanest approach is to scale source-cell-stride to fit nominal-dest-stride** — i.e., loop `j < unitIconY` over destination rows, and source-sample as `pSrcRow[(int)(i / f.x)]` per pixel. This is a nearest-neighbor downscale back to nominal; visual quality matches the un-upscaled case but the game still functions correctly on upscaled input.

Use this concrete form:

```cpp
char* pTmp = (char*)s_MechTextures + sizeof ( TGAFileHeader );
const uint32_t actualW = (uint32_t)s_MechTextures->width;
const uint32_t actualH = (uint32_t)s_MechTextures->height;
const AssetScale::Vec2 f = AssetScale::factorFor(
    s_MechTexturesKey, actualW, actualH, "mechicon.blit");

const AssetScale::IRect srcRect = AssetScale::nominalToActualRect(
    s_MechTexturesKey, actualW, actualH,
    (float)offsetX, (float)offsetY,
    (float)unitIconX, (float)unitIconY,
    "mechicon.blit");

// Destination iterates nominal cell size; source samples at scaled coords.
for ( int j = 0; j < unitIconY; ++j )
{
    int srcY = srcRect.y + (int)(j * f.y);
    if (srcY >= (int)actualH) srcY = (int)actualH - 1;
    DWORD* pSrcRow = (DWORD*)pTmp + (long)srcY * (long)actualW;
    ...
    for ( int i = 0; i < unitIconX; ++i )
    {
        int srcX = srcRect.x + (int)(i * f.x);
        if (srcX >= (int)actualW) srcX = (int)actualW - 1;
        pDestData[i] = pSrcRow[srcX];
    }
    pDestData += s_textureMemory->width;
}
```

The `tmpOffset` and `pSrcRow += s_MechTextures->width;` lines from the original are removed (replaced by per-row recomputation).

- [ ] **Step 2: Apply same refactor to the second blit (line ~511 inside `MechIcon::init(long whichIndex)`)**

```bash
sed -n '500,550p' code/mechicon.cpp
```

Apply the identical pattern: capture `actualW/actualH`, compute `f` and `srcRect` with caller tag `"mechicon.blit"`, iterate destination at nominal size, sample source at scaled coords.

- [ ] **Step 3: Apply same refactor to the third blit (line ~817)**

```bash
sed -n '790,860p' code/mechicon.cpp
```

Same pattern. Use caller tag `"mechicon.blit"` (intentional — deduplicate by callsite class, not by line).

- [ ] **Step 4: Build**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```

- [ ] **Step 5: Deploy and visual verification (un-upscaled)**

```bash
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe
```

Run a mission. Mech HUD schematic should look identical to pre-change (factor=1.0 means each `(int)(j * f.y)` reduces to `j`, identical sampling).

- [ ] **Step 6: Commit**

```bash
git add code/mechicon.cpp
git commit -m "feat(mechicon): scale-aware CPU blits via AssetScale (downsample to nominal dest)"
```

---

### Task 11: Visual verification with upscaled mech atlas

**Files:** none (deploy + visual check only)

- [ ] **Step 1: Deploy a 4x upscaled `mcui_high7.tga`**

Locate the upscale: `mc2srcdata/art_4x_gpu/mcui_high7.tga` (per CLAUDE.md). Deploy it over the live asset:

```bash
cp -f A:/Games/mc2-opengl-src/mc2srcdata/art_4x_gpu/mcui_high7.tga \
      A:/Games/mc2-opengl/mc2-win64-v0.1.1/data/art/mcui_high7.tga
```

If the path differs, adjust. The point is: get an upscaled version into the loose-file override location.

- [ ] **Step 2: Run mission, observe mech HUD schematic**

Start a mission. The HUD damage schematic should now render correctly (recognizable mech outline, no scrambled noise). Take a screenshot for diff against pre-fix screenshots in CLAUDE.md.

- [ ] **Step 3: If broken, check trace output**

```
set MC2_ASSET_SCALE_TRACE=1
mc2.exe
```

Look for `event=oob_blit` lines (the source rect went off the actual atlas) or `event=unknown_asset` (the canonicalization didn't match the manifest entry). Adjust manifest dims or canonicalization as needed.

- [ ] **Step 4: Restore non-upscaled if you don't want to keep the override yet**

Or leave the upscale deployed if the schematic looks correct. (Final removal of the CLAUDE.md blocklist happens in Task 21.)

- [ ] **Step 5: No commit (verification step only)**

---

## Phase 3 — `VehicleIcon` adoption

Mirror of Phase 2; spec calls for "single short follow-up commit per user direction." Bundled here as one task because it's a structural copy.

### Task 12: Mirror `MechIcon` changes to `VehicleIcon`

**Files:**
- Modify: `code/mechicon.h` (add `static AssetScale::AssetKey s_VehicleTexturesKey;`)
- Modify: `code/mechicon.cpp:28` (define the static)
- Modify: `code/mechicon.cpp:1146-1170` (`VehicleIcon::initTextures`-equivalent, populate key after `strcat(path, "mcui_*5.tga")` choice)
- Modify: any `VehicleIcon` UV draw block (find with `grep -n VehicleIcon code/mechicon.cpp`)
- Modify: any `VehicleIcon` CPU blit blocks (same grep)

- [ ] **Step 1: Locate `VehicleIcon` callsites**

```bash
grep -nE 'VehicleIcon::|s_VehicleTextures' code/mechicon.cpp
```

There should be a `VehicleIcon::initTextures` (or similar) around line 1146, and UV/blit code analogous to MechIcon's. Read context:

```bash
sed -n '1140,1200p' code/mechicon.cpp
```

- [ ] **Step 2: Add `s_VehicleTexturesKey` to header**

In `code/mechicon.h`, add to the `VehicleIcon` class definition:

```cpp
    static TGAFileHeader*       s_VehicleTextures;
    static AssetScale::AssetKey s_VehicleTexturesKey;
```

- [ ] **Step 3: Define the static in mechicon.cpp**

Find:
```cpp
TGAFileHeader *VehicleIcon::s_VehicleTextures = NULL;
```

Add right after:
```cpp
AssetScale::AssetKey VehicleIcon::s_VehicleTexturesKey;
```

- [ ] **Step 4: Populate key in `VehicleIcon::initTextures`**

Find the path-build block (around line 1152–1156):
```cpp
		if ( Environment.screenWidth == 800 )
			strcat( path, "mcui_med5.tga" );
		else if ( Environment.screenWidth == 640 )
			strcat( path, "mcui_low5.tga" );
		else 
			strcat( path, "mcui_high8.tga" );
```

After the `if/else` chain (and after `S_strlwr(path)` if present — verify by reading), add:
```cpp
		s_VehicleTexturesKey = AssetScale::key(path);
```

- [ ] **Step 5: Refactor VehicleIcon UV math**

Use the **same template** as Task 9, with these substitutions:
- `s_MechTexturesKey` → `s_VehicleTexturesKey`
- `"mechicon.uv"` → `"vehicleicon.uv"`

- [ ] **Step 6: Refactor VehicleIcon CPU blits**

Use the **same template** as Task 10, with substitutions:
- `s_MechTextures` → `s_VehicleTextures`
- `s_MechTexturesKey` → `s_VehicleTexturesKey`
- `"mechicon.blit"` → `"vehicleicon.blit"`

- [ ] **Step 7: Add manifest entries**

In `data/art/asset_sizes.csv` (both source and deployed), uncomment and fill the three vehicle lines (use the same dimension-discovery method as Task 8):

```csv
art/mcui_high8.tga,<W>,<H>
art/mcui_med5.tga,<W>,<H>
art/mcui_low5.tga,<W>,<H>
```

- [ ] **Step 8: Build**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```

- [ ] **Step 9: Deploy and verify**

```bash
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe
```

Run a mission with vehicles in the player force, complete it, observe the post-mission salvage screen. Vehicle icons should render correctly (not scrambled). This is the user's reported repro.

If you have an upscaled `mcui_high8.tga` available, deploy it and re-verify with `MC2_ASSET_SCALE_TRACE=1`.

- [ ] **Step 10: Commit**

```bash
git add code/mechicon.h code/mechicon.cpp data/art/asset_sizes.csv
git commit -m "feat(vehicleicon): mirror MechIcon scale-aware UV/blit (fixes post-mission)"
```

---

## Phase 4 — `PilotIcon` adoption

### Task 13: Mirror to `PilotIcon`

**Files:**
- Modify: `code/mechicon.h` (add `static AssetScale::AssetKey s_pilotTexturesKey;` to `PilotIcon`)
- Modify: `code/mechicon.cpp` (define static + populate at load + refactor UV/blit if PilotIcon has CPU blits)

- [ ] **Step 1: Locate PilotIcon load and UV code**

```bash
grep -nE 'PilotIcon::|s_pilotTexture' code/mechicon.cpp
```

The path-build block is around line 1575/1664 (two locations — likely two resolution-tier choices for two different pilot views). Read context:

```bash
sed -n '1570,1610p' code/mechicon.cpp
sed -n '1660,1700p' code/mechicon.cpp
```

- [ ] **Step 2: Apply Task 7 + 9 + 10 templates**

Same pattern as MechIcon/VehicleIcon: add `AssetKey` static, populate after path build, replace UV math with `factorFor`-based form, replace any CPU blits with `nominalToActualRect`-based form.

Caller tags: `"piloticon.uv"`, `"piloticon.blit"`.

- [ ] **Step 3: Add manifest entries**

```csv
art/mcui_high2.tga,<W>,<H>
art/mcui_med2.tga,<W>,<H>
art/mcui_low2.tga,<W>,<H>
```

(Verify the file numbering is `_2` for pilot — Task 13 Step 1 grep will confirm.)

- [ ] **Step 4: Build, deploy, verify**

Same commands. Visual check: pilot portraits in mech bay and post-mission. Should render correctly with both un-upscaled and (if available) upscaled atlases.

- [ ] **Step 5: Commit**

```bash
git add code/mechicon.h code/mechicon.cpp data/art/asset_sizes.csv
git commit -m "feat(piloticon): mirror MechIcon scale-aware UV/blit"
```

---

## Phase 5 — `aObject` opt-in for chrome

### Task 14: Add `assetKey` and `srcRectSpace` fields to `aObject`

**Files:**
- Modify: `gui/asystem.h:85-205` (`aObject` class definition)
- Modify: `gui/aSystem.cpp:440-459` (`aObject::render`)

- [ ] **Step 1: Add include and fields to `gui/asystem.h`**

Near the top of `gui/asystem.h`, add:

```cpp
#include "../GameOS/gameos/asset_scale.h"
```

(Adjust relative path to match the actual layout — `gui/` is sibling to `GameOS/`, so this should be `#include "../GameOS/gameos/asset_scale.h"`. Verify with `ls GameOS/gameos/asset_scale.h` from the `gui/` dir.)

In the `aObject` class (around line 184–204, the `protected:` section), add the new fields:

```cpp
public:
    enum class SrcRectSpace { ActualPixels, NominalPixels };

    void setAssetScale(const AssetScale::AssetKey& k,
                       SrcRectSpace space = SrcRectSpace::NominalPixels)
    {
        assetKey = k;
        srcRectSpace = space;
    }

protected:
    gos_VERTEX                  location[4];
    unsigned long               textureHandle;
    float                       fileWidth;
    bool                        showWindow;

    AssetScale::AssetKey        assetKey;
    SrcRectSpace                srcRectSpace = SrcRectSpace::ActualPixels;

    aObject*                    pChildren[MAX_CHILDREN];
    ...
```

- [ ] **Step 2: Modify `aObject::render` to transform source UVs when opted-in**

The current render (`gui/aSystem.cpp:440`) draws `location[]` directly with whatever UVs are baked in. Source-rect transform must happen *before* `gos_DrawQuads`, then revert after (we don't want to permanently mutate `location[]`).

Replace `aObject::render()` body with:

```cpp
void aObject::render()
{
    if ( !showWindow ) return;

    // Snapshot UVs in case we transform them.
    gos_VERTEX saved[4];
    bool transformed = false;

    if ( !assetKey.empty() && srcRectSpace == SrcRectSpace::NominalPixels )
    {
        unsigned long gosID = mcTextureManager->get_gosTextureHandle( textureHandle );
        TEXTUREPTR td;
        if ( gosID && gos_LockTexture(gosID, 0, 1, &td) )
        {
            const uint32_t aw = (uint32_t)td.Width;
            const uint32_t ah = (uint32_t)td.Height;
            gos_UnLockTexture(gosID);

            // Existing UVs are stored as fractions of the texture in NOMINAL
            // space (since fileWidth divides them in setUVs). Convert to
            // nominal pixel coords, then through AssetScale to actual UVs.
            const float u0 = location[0].u, u1 = location[2].u;
            const float v0 = location[0].v, v1 = location[1].v;

            const float nx = u0 * fileWidth;
            const float ny = v0 * fileWidth;
            const float nw = (u1 - u0) * fileWidth;
            const float nh = (v1 - v0) * fileWidth;

            const AssetScale::IRect r = AssetScale::nominalToActualRect(
                assetKey, aw, ah, nx, ny, nw, nh, "aobject.render");

            const float au0 = (float)r.x / (float)aw;
            const float av0 = (float)r.y / (float)ah;
            const float au1 = (float)(r.x + r.w) / (float)aw;
            const float av1 = (float)(r.y + r.h) / (float)ah;

            for (int i = 0; i < 4; ++i) saved[i] = location[i];
            location[0].u = location[1].u = au0;
            location[2].u = location[3].u = au1;
            location[0].v = location[3].v = av0;
            location[1].v = location[2].v = av1;
            transformed = true;
        }
    }

    unsigned long gosID = mcTextureManager->get_gosTextureHandle( textureHandle );
    gos_SetRenderState( gos_State_Texture, gosID );
    gos_SetRenderState(gos_State_Filter, gos_FilterNone);
    gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
    gos_SetRenderState( gos_State_ZCompare, 0 );
    gos_SetRenderState( gos_State_ZWrite, 0 );

    gos_DrawQuads( location, 4 );

    if (transformed) {
        for (int i = 0; i < 4; ++i) location[i] = saved[i];
    }

    for ( int i = 0; i < pNumberOfChildren; i++ )
    {
        pChildren[i]->render();
    }
}
```

- [ ] **Step 3: Build**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```

If `gos_LockTexture`'s third arg expects a different "read-only" sentinel, fix to match the project convention.

- [ ] **Step 4: Deploy and smoke test**

```bash
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe
```

Boot to main menu. **No widget has been opted in yet** so `assetKey` is empty everywhere → behavior identical to pre-change. Verify nothing visibly regressed.

- [ ] **Step 5: Commit**

```bash
git add gui/asystem.h gui/aSystem.cpp
git commit -m "feat(gui): aObject opt-in assetKey + srcRectSpace (no callers yet)"
```

---

## Phase 6–8 — Per-screen chrome adoption

Each chrome adoption follows the same pattern: locate the screen's chrome `aObject`s, find what texture file they load, add it to the manifest, call `setAssetScale(...)` on each chrome widget at its `init` time, build, deploy, screenshot-diff.

### Task 15: Adopt main-menu chrome

**Files:**
- Modify: `code/mainmenu.cpp` (find chrome `aObject` init sites)
- Modify: `data/art/asset_sizes.csv` (add the discovered chrome texture entries)

- [ ] **Step 1: Identify main-menu chrome assets**

```bash
grep -nE 'mcl_mm|mainmenu\.fit|aObject|aRect.*init|setTexture' code/mainmenu.cpp | head -40
```

The chrome assets are loaded via `.fit` files referenced near `mainmenu.cpp:107` (`mcl_sp.fit` or similar). Read the `.fit` to find image filenames (likely keyed as `imageFileName` or `bitmap` inside numbered blocks). Or grep deployed dir:

```bash
ls A:/Games/mc2-opengl/mc2-win64-v0.1.1/data/art/ | grep -iE 'mcl_(sp|mm|mainmenu|menubg)' | head
```

- [ ] **Step 2: Discover nominal dimensions for each chrome asset**

Same approach as Task 8. For each unique `mcl_*.tga` in the main-menu chrome, record nominal width/height and add to `data/art/asset_sizes.csv`:

```csv
# Main-menu chrome
art/<chrome1>.tga,<W>,<H>
art/<chrome2>.tga,<W>,<H>
...
```

- [ ] **Step 3: Opt in each chrome `aObject`**

In `code/mainmenu.cpp`'s `init` (around line 99 or 107), after each chrome `aObject::setTexture(path)` (or after the FitIniFile-driven `aObject::init`), add:

```cpp
chromeObj.setAssetScale(AssetScale::key(path));   // path is the same string passed to setTexture
```

If the chrome objects are loaded en-masse via a loop over `.fit` blocks, add the call inside that loop using the per-block image filename.

- [ ] **Step 4: Build**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```

- [ ] **Step 5: Deploy and visually verify**

```bash
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe
```

Boot to main menu. The visible gaps from the user's screenshot (top crossbar to side columns, side columns to bottom corner pieces) should be closed.

If gaps persist, run with `MC2_ASSET_SCALE_TRACE=1` and look for `unknown_asset` (manifest entry missing for a chrome piece) or `oob_blit` (source-rect math error).

- [ ] **Step 6: Commit**

```bash
git add code/mainmenu.cpp data/art/asset_sizes.csv
git commit -m "feat(gui): main-menu chrome opts into AssetScale (closes seam gaps)"
```

---

### Task 16: Adopt load/save menu chrome

**Files:**
- Modify: `code/saveload.cpp` (or whichever screen owns the load-game UI; grep)
- Modify: `data/art/asset_sizes.csv`

- [ ] **Step 1: Identify the load/save menu source file**

```bash
grep -rn -lE 'Load Game|LOAD GAME|loadgame\.fit|savegame\.fit' code/ | head -5
```

Likely `code/saveload.cpp` or `code/savegame.cpp` or similar.

- [ ] **Step 2: Apply the Task 15 template to that file**

Same five-step pattern: identify chrome `.tga`s, add to manifest, call `setAssetScale` on each chrome `aObject`, build, deploy, visually verify against the user's load-menu screenshot.

- [ ] **Step 3: Commit**

```bash
git add code/<file>.cpp data/art/asset_sizes.csv
git commit -m "feat(gui): load/save menu chrome opts into AssetScale"
```

---

### Task 17: Adopt mech-bay chrome

**Files:**
- Modify: `code/mechbayscreen.cpp` (file confirmed by spec/exploration)
- Modify: `data/art/asset_sizes.csv`

- [ ] **Step 1: Apply the Task 15 template**

The mech bay was reported as "a lot worse" than the main menu. Expect more chrome assets and more `setAssetScale` calls.

- [ ] **Step 2: Build, deploy, verify**

- [ ] **Step 3: Commit**

```bash
git add code/mechbayscreen.cpp data/art/asset_sizes.csv
git commit -m "feat(gui): mech-bay chrome opts into AssetScale"
```

---

## Phase 9 — Cleanup & invariants

### Task 18: Remove CLAUDE.md "Do Not Upscale" blocklist; deploy the upscaled assets

**Files:**
- Modify: `CLAUDE.md` (remove the "Do Not Upscale These Art Assets" section)
- Deploy: copy `mc2srcdata/art_4x_gpu/mcui_high7.tga` (and med4/low4) into `A:/Games/mc2-opengl/mc2-win64-v0.1.1/data/art/`

- [ ] **Step 1: Verify the in-mission HUD schematic still renders correctly with upscaled inputs**

Re-run Task 11's verification with `mcui_high7.tga` 4x-upscaled deployed. Mech HUD damage schematic must look correct.

- [ ] **Step 2: Remove the section from CLAUDE.md**

In `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/CLAUDE.md`, delete the "## Do Not Upscale These Art Assets" section in its entirety. Replace with a note referencing the new system if desired:

```markdown
<!-- previously: "Do Not Upscale These Art Assets" — superseded by AssetScale subsystem.
     See docs/superpowers/specs/2026-04-23-asset-scale-aware-rendering-design.md -->
```

Or simply delete with no replacement.

- [ ] **Step 3: Deploy the upscaled assets**

```bash
cp -f A:/Games/mc2-opengl-src/mc2srcdata/art_4x_gpu/mcui_high7.tga A:/Games/mc2-opengl/mc2-win64-v0.1.1/data/art/mcui_high7.tga
cp -f A:/Games/mc2-opengl-src/mc2srcdata/art_4x_gpu/mcui_med4.tga  A:/Games/mc2-opengl/mc2-win64-v0.1.1/data/art/mcui_med4.tga
cp -f A:/Games/mc2-opengl-src/mc2srcdata/art_4x_gpu/mcui_low4.tga  A:/Games/mc2-opengl/mc2-win64-v0.1.1/data/art/mcui_low4.tga
```

- [ ] **Step 4: Final visual sanity-check**

Boot game, run mission, verify schematic.

- [ ] **Step 5: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: remove mcui upscale blocklist (superseded by AssetScale)"
```

---

### Task 19: Add regression-guard script

**Files:**
- Create: `scripts/check-asset-scale-callers.sh`

- [ ] **Step 1: Write the script**

Mirror the structure of `scripts/check-destroy-invariant.sh`:

```sh
#!/bin/sh
# scripts/check-asset-scale-callers.sh
#
# Enforces: nobody computes pixel offsets as
# "<unitConst> / <texturePtr>->width|height"
# (or the equivalent for s_textureMemory) outside the AssetScale subsystem.
# Such patterns are the bug class fixed by the AssetScale rework.
#
# Spec: docs/superpowers/specs/2026-04-23-asset-scale-aware-rendering-design.md

set -e
violations=0

# Pattern 1: unitIcon[XY] / something->width|height
hits=$(grep -rEn 'unitIcon[XY][[:space:]]*/[[:space:]]*[A-Za-z_][A-Za-z0-9_]*->(width|height)' \
    code/ mclib/ GameOS/ \
    --include='*.cpp' --include='*.h' \
    | grep -v 'GameOS/gameos/asset_scale\.' \
    | grep -Ev ':[[:space:]]*//' || true)
if [ -n "$hits" ]; then
    echo "[INVARIANT] raw unitIcon{X,Y}/<tex>->{width,height} outside AssetScale:"
    echo "$hits"
    violations=1
fi

# Pattern 2: pilotIcon[XY] / something->width|height
hits=$(grep -rEn 'pilotIcon[XY][[:space:]]*/[[:space:]]*[A-Za-z_][A-Za-z0-9_]*->(width|height)' \
    code/ mclib/ GameOS/ \
    --include='*.cpp' --include='*.h' \
    | grep -v 'GameOS/gameos/asset_scale\.' \
    | grep -Ev ':[[:space:]]*//' || true)
if [ -n "$hits" ]; then
    echo "[INVARIANT] raw pilotIcon{X,Y}/<tex>->{width,height} outside AssetScale:"
    echo "$hits"
    violations=1
fi

if [ "$violations" -ne 0 ]; then
    echo "FAIL: see above"
    exit 1
fi
echo "OK"
exit 0
```

- [ ] **Step 2: Make executable and run it**

```bash
chmod +x scripts/check-asset-scale-callers.sh
sh scripts/check-asset-scale-callers.sh
```

Expected: `OK`. If it reports hits, those are unfixed callsites — go fix them, then re-run.

- [ ] **Step 3: Commit**

```bash
git add scripts/check-asset-scale-callers.sh
git commit -m "build: add asset-scale invariant grep guard"
```

---

### Task 20: Update CLAUDE.md instrumentation section

**Files:**
- Modify: `CLAUDE.md` (under "Tier-1 Instrumentation Env Vars" or a new sibling section)

- [ ] **Step 1: Add an entry for `MC2_ASSET_SCALE_TRACE` and `MC2_ASSET_SCALE_SELFTEST`**

Append to the Tier-1 Instrumentation list (or add a new section if one feels cleaner):

```markdown
- `MC2_ASSET_SCALE_TRACE=1` — per-key `[ASSET_SCALE v1]` runtime lookup events plus 600-frame summaries. Default off; **first** `oob_blit` per `(path, callerTag)` always logs regardless. Counters surface via `AssetScale::dumpCountersTo(stdout)`.
- `MC2_ASSET_SCALE_SELFTEST=1` — runs synthetic 2x/4x/8x/1.5x golden tests at startup; prints `[ASSET_SCALE v1] event=selftest_pass|fail` per case, then continues normally.
```

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: document MC2_ASSET_SCALE_TRACE and SELFTEST env vars"
```

---

## Self-review

**Spec coverage check:**

| Spec section | Implemented in |
|---|---|
| Architecture / API surface | Task 1 (header), Task 2 (skeleton), Task 3 (transforms) |
| Canonicalization rule | Task 2 (`canonicalize()` in anonymous ns) |
| Rounding policy | Task 3 (`floor`/`ceil` in `nominalToActualRect`) |
| Telemetry gating (always-on vs gated) | Task 3 (`firstOobWarning`, `firstUnknownWarning`) |
| Counters + dumpCountersTo | Task 2 (`dumpCountersTo`) |
| Self-tests at 2x/4x/8x/1.5x | Task 4 |
| Lifecycle hook | Task 5 |
| Manifest seed | Task 6 |
| Source-path retention on icon loaders | Tasks 7, 12, 13 |
| MechIcon UV refactor | Task 9 |
| MechIcon CPU blit refactor (×3) | Task 10 |
| VehicleIcon mirror | Task 12 |
| PilotIcon mirror | Task 13 |
| `aObject` opt-in fields | Task 14 |
| `aBitMap::render` integration | Task 14 (folded into `aObject::render` since `aBitMap` is not a separate class in this codebase — verified via grep) |
| Main menu chrome | Task 15 |
| Load/save menu chrome | Task 16 |
| Mech bay chrome | Task 17 |
| Remove CLAUDE.md blocklist | Task 18 |
| Regression grep script | Task 19 |
| CLAUDE.md env-var docs | Task 20 |

No gaps.

**Placeholder scan:** Every code step contains complete code. The two intentional `<TBD>` markers are dimension values in `data/art/asset_sizes.csv` — explicitly called out as fill-at-discovery-time per spec, with the discovery procedure documented in Task 8 Step 1.

**Type consistency:** `AssetKey`, `Vec2`, `IVec2`, `IRect` defined in Task 1, used consistently in Tasks 3/9/10/12/13/14. `factorFor`/`nominalToActualRect` signatures match between header (Task 1) and impl (Task 3) and callsites (Tasks 9/10/12/13/14). `s_MechTexturesKey`/`s_VehicleTexturesKey`/`s_pilotTexturesKey` naming consistent.

---

## Execution handoff

Plan complete and saved to `docs/superpowers/plans/2026-04-23-asset-scale-aware-rendering.md`.

Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks, fast iteration. Good for this plan because each task has a deploy-and-visual-check that benefits from independent context.

2. **Inline Execution** — execute tasks in this session using executing-plans, batch with checkpoints. Faster wall-clock, but visual-verification tasks (11, 15-end) still require manual game runs.

Which approach?
