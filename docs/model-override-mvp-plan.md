# Model Override MVP Implementation Plan (`MODEL-OVERRIDE-MVP-1`)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a modder replace one stock visual model (static prop, then tree) by dropping a glTF/GLB into a mod folder and declaring it in `model_overrides/models.json`, render-only, with stock gameplay/collision frozen and stock fallback on any failure.

**Architecture:** A new engine-independent `model_override_registry` translation unit parses the manifest into plain structs (unit-testable, no engine deps). The existing `TG_TypeMultiShape::LoadFromFile` probe funnel (`mclib/msl.cpp:438`) gains an override-resolution step keyed by `<class>:<appearanceName>`, and the static-prop/tree appearance loaders in `mclib/bdactor.cpp` are redirected from the direct `LoadTGMultiShapeFromASE(fullPath)` call to `LoadFromFile(basename, class)`. Render bounds (`OBBRadius`, `highZ`) are already recomputed in `BldgAppearance::init` from the loaded shape's box — the override rides that path; the work is verifying the override mesh's box is accurate and proving collision (`cellsCovered`) is untouched.

**Tech Stack:** C++14 (engine TU discipline — no `std::filesystem`/`optional`/`string_view` in production), `nlohmann/json` (vendored single-header, isolated to one TU), existing Assimp importer (`mclib/assimp_importer.cpp`, `ENABLE_ASSIMP_IMPORTER`), CMake 3.10, MSVC `RelWithDebInfo`, Python smoke harness (`scripts/run_smoke.py`).

**Reads (authoritative recon):**
- `docs/model-override-system-recon.md` (this branch) — identity, hook, bounds, material findings.
- Importer surface: `mclib/assimp_importer.h:35` (`ImportGeometryFromFile`).
- Probe funnel: `mclib/msl.cpp:438-479` (`LoadFromFile`).
- Prop/tree loaders: `mclib/bdactor.cpp:211-252` (buildings), `:3389-3434` (trees).
- Bounds recompute: `mclib/bdactor.cpp:693-744` (`OBBRadius`/`highZ`).
- Collision (must stay stock): `mclib/bdactor.cpp:2763-2827`, `mclib/terrobj.cpp:1442-1479` (`cellsCovered`).

---

## Hard constraints (carried from the brief — every task obeys)

No broad asset-system rewrite · no collision replacement · no mission/editor work · no `tgl.fst` mutation · no stock-asset mutation · no mech/vehicle replacement (mech path passes `class=nullptr` → resolver skips) · no asset-viewer UI work (docs/validation notes only) · no `git add -A` (stage explicit paths only).

---

## File Structure

**Created:**
- `3rdparty/include/nlohmann/json.hpp` — vendored single-header JSON (v3.11.3). **Idempotent** — if the mod-profile-launcher branch already added this exact file/version, skip Task 1.1's download and reuse it.
- `THIRD_PARTY_NOTICES.md` — license attribution (create if missing; append nlohmann entry).
- `mclib/model_override_registry.h` — public API: override record struct + lookup.
- `mclib/model_override_registry.cpp` — manifest parse + validation + lookup. **Only engine TU that includes `nlohmann/json.hpp`.**
- `tests/model_override/test_model_override_registry.cpp` — standalone unit test runner (no engine link).
- `tests/model_override/CMakeLists.txt` — `model_override_tests` target.
- `tests/model_override/fixtures/` — JSON fixtures (valid, invalid-not-renderonly, invalid-not-stock-fallback, malformed, missing-source).
- `data/model_overrides/models.json` — empty stock manifest (`{"overrides":[]}`) so the default path resolves with zero overrides.
- `docs/model-override-mvp-notes.md` — running validation/decision log (filled during slices 3–4).

**Modified:**
- `mclib/msl.h` — extend `LoadFromFile` signature with an optional `overrideClass` parameter.
- `mclib/msl.cpp:438-479` — insert override resolution at the top of `LoadFromFile`.
- `mclib/bdactor.cpp` — redirect static-prop base + tree base load call sites from `LoadTGMultiShapeFromASE(fullPath)` to `LoadFromFile(aseFileName, class)`.
- `mclib/CMakeLists.txt` — add `model_override_registry.cpp`; add `3rdparty/include` to include dirs.
- `CMakeLists.txt` — add `tests/model_override` subdirectory (gated by existing `MC2_BUILD_TESTS`).
- `scripts/run_smoke.py` — add model-override smoke cases (no-mod identity, override-present, missing-source fallback).
- `scripts/check-json-isolation.sh` — extend (or create) to also assert only the two allowed TUs include `nlohmann/json.hpp`.

**Out of scope (documented, not built):** damage-state overrides, LOD overrides, normal/emissive/ORM material wiring, alpha-blend, cook cache (`.tglc`), asset-viewer 3D preview, mech/vehicle overrides, collision re-derivation.

---

## Slice 1 — `MODEL-OVERRIDE-REGISTRY-0`

**Goal:** Engine-independent registry that loads `model_overrides/models.json`, validates MVP rules, logs invalid entries, never crashes.

### Task 1: Vendor nlohmann/json + isolation check

**Files:**
- Create: `3rdparty/include/nlohmann/json.hpp`
- Create/append: `THIRD_PARTY_NOTICES.md`
- Create: `scripts/check-json-isolation.sh`

- [ ] **Step 1: Vendor the header (skip if already present from mod-profile branch)**

```bash
cd "$(git rev-parse --show-toplevel)"
test -f 3rdparty/include/nlohmann/json.hpp || {
  mkdir -p 3rdparty/include/nlohmann
  curl -fsSL -o 3rdparty/include/nlohmann/json.hpp \
    https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
}
test -s 3rdparty/include/nlohmann/json.hpp && echo "json.hpp present"
```
Expected: `json.hpp present`.

- [ ] **Step 2: Append license note**

Append to `THIRD_PARTY_NOTICES.md` (create with a `# Third-Party Notices` header if missing):

```markdown
## nlohmann/json v3.11.3
MIT License. Source: https://github.com/nlohmann/json
Single-header, vendored at 3rdparty/include/nlohmann/json.hpp.
```

- [ ] **Step 3: Isolation guard script**

Create `scripts/check-json-isolation.sh`:

```bash
#!/bin/sh
# Only these TUs may include nlohmann/json. Fail otherwise.
# (Per-file case match — a space-joined grep -v -F pattern is WRONG: it treats
#  the whole allowlist as one literal and never matches either path.)
set -e
hits=$(grep -rl 'nlohmann/json' --include='*.cpp' --include='*.h' \
  mclib GameOS RenderCore code tests 2>/dev/null || true)
status=0
for f in $hits; do
  case "$f" in
    mclib/model_override_registry.cpp) ;;
    tests/model_override/test_model_override_registry.cpp) ;;
    */model_override_registry.cpp) ;;
    */test_model_override_registry.cpp) ;;
    *) echo "json isolation violated: $f"; status=1 ;;
  esac
done
[ "$status" -eq 0 ] && echo "json isolation OK"
exit "$status"
```

- [ ] **Step 4: Run guard (passes — no includes yet)**

Run: `sh scripts/check-json-isolation.sh`
Expected: `json isolation OK`

- [ ] **Step 5: Commit**

```bash
git add 3rdparty/include/nlohmann/json.hpp THIRD_PARTY_NOTICES.md scripts/check-json-isolation.sh
git commit -m "chore(modoverride): vendor nlohmann/json + json-isolation guard"
```

### Task 2: Registry header (API + record struct)

**Files:**
- Create: `mclib/model_override_registry.h`

- [ ] **Step 1: Write the header**

```cpp
// model_override_registry.h — MODEL-OVERRIDE-MVP-1 Slice 1.
// Engine-independent parse of model_overrides/models.json. No TG_*/GameOS deps.
// Render-only model overrides keyed by "<class>:<appearanceName>". MVP rules:
// renderOnly must be true, fallback must be "stock". Invalid entries are logged
// and dropped, never fatal. See docs/model-override-mvp-plan.md.
#pragma once
#include <string>
#include <vector>

struct ModelOverrideRecord {
    std::string overrideClass;   // "staticprop" | "tree" — NORMALIZED lowercase
    std::string appearanceName;  // from "replaces" after ':' — NORMALIZED lowercase
    std::string sourceRelPath;   // .glb/.gltf relative to manifest dir (validated safe)
    float       scale = 1.0f;    // MVP requires exactly 1.0
    // MVP invariants (validated at load; entry dropped + logged if violated):
    //   type=="model", renderOnly==true, fallback=="stock", scale==1.0,
    //   safe relative .glb/.gltf source, class in {staticProp,tree},
    //   class field (if present) agrees with replaces. Duplicate key: first wins.
};

class ModelOverrideRegistry {
public:
    // Parse a manifest file. manifestDir is the directory holding it, used to
    // resolve sourceRelPath. Returns the number of VALID records loaded.
    // Malformed JSON / missing file => 0 valid records, no throw, logs once.
    int loadFromFile(const std::string& manifestPath,
                     const std::string& manifestDir);

    // Lookup by class + appearance name. Returns nullptr if no override.
    // overrideClass==nullptr (mech path) always returns nullptr.
    const ModelOverrideRecord* resolve(const char* overrideClass,
                                       const char* appearanceName) const;

    int count() const { return (int)records_.size(); }

    // Process-wide singleton, lazily loaded once from the default manifest
    // (data/model_overrides/models.json) on first access.
    static ModelOverrideRegistry& instance();

private:
    std::vector<ModelOverrideRecord> records_;
    std::string manifestDir_;
};
```

- [ ] **Step 2: Commit**

```bash
git add mclib/model_override_registry.h
git commit -m "feat(modoverride): registry header + ModelOverrideRecord"
```

### Task 3: Failing unit test for parse + validation

**Files:**
- Create: `tests/model_override/test_model_override_registry.cpp`
- Create: `tests/model_override/fixtures/valid.json`
- Create: `tests/model_override/fixtures/not_renderonly.json`
- Create: `tests/model_override/fixtures/bad_fallback.json`
- Create: `tests/model_override/fixtures/malformed.json`

- [ ] **Step 1: Write fixtures**

`tests/model_override/fixtures/valid.json`:
```json
{ "overrides": [
  { "type":"model","class":"staticProp","replaces":"staticProp:example_name",
    "source":"source/props/example.glb","renderOnly":true,"scale":1.0,"fallback":"stock" }
] }
```
`tests/model_override/fixtures/not_renderonly.json`:
```json
{ "overrides": [
  { "type":"model","class":"staticProp","replaces":"staticProp:x",
    "source":"a.glb","renderOnly":false,"fallback":"stock" }
] }
```
`tests/model_override/fixtures/bad_fallback.json`:
```json
{ "overrides": [
  { "type":"model","class":"staticProp","replaces":"staticProp:x",
    "source":"a.glb","renderOnly":true,"fallback":"impostor" }
] }
```
`tests/model_override/fixtures/malformed.json`:
```json
{ "overrides": [ { "class": "staticProp",
```
`tests/model_override/fixtures/bad_scale.json` (scale != 1.0 → dropped in MVP):
```json
{ "overrides": [
  { "type":"model","class":"staticProp","replaces":"staticProp:x",
    "source":"a.glb","renderOnly":true,"scale":2.0,"fallback":"stock" }
] }
```
`tests/model_override/fixtures/unsafe_paths.json` (every entry must be rejected):
```json
{ "overrides": [
  { "type":"model","class":"staticProp","replaces":"staticProp:absu",
    "source":"/etc/x.glb","renderOnly":true,"fallback":"stock" },
  { "type":"model","class":"staticProp","replaces":"staticProp:dotdot",
    "source":"../x.glb","renderOnly":true,"fallback":"stock" },
  { "type":"model","class":"staticProp","replaces":"staticProp:drive",
    "source":"C:/x.glb","renderOnly":true,"fallback":"stock" },
  { "type":"model","class":"staticProp","replaces":"staticProp:ext",
    "source":"x.png","renderOnly":true,"fallback":"stock" }
] }
```
`tests/model_override/fixtures/dup.json` (duplicate normalized key → first wins, second dropped):
```json
{ "overrides": [
  { "type":"model","class":"staticProp","replaces":"staticProp:Dup_Name",
    "source":"first.glb","renderOnly":true,"fallback":"stock" },
  { "type":"model","class":"staticProp","replaces":"  staticProp : dup_name ",
    "source":"second.glb","renderOnly":true,"fallback":"stock" }
] }
```
`tests/model_override/fixtures/non_object_entry.json` (array element not an object → skipped, no crash):
```json
{ "overrides": [ 42, "junk",
  { "type":"model","class":"tree","replaces":"tree:ok",
    "source":"ok.gltf","renderOnly":true,"fallback":"stock" }
] }
```

- [ ] **Step 2: Write the test runner (takes fixtures dir as argv[1] so the binary stays out of the source tree)**

```cpp
// Minimal assert-based runner. Returns nonzero on any failure.
// Fixtures dir comes from argv[1] (CMake passes the source-tree path); the
// compiled binary lives in the build tree — never written into source.
#include "model_override_registry.h"   // include dir set by CMake; no ../ paths
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); ++failures; } }while(0)

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "fixtures";
    auto fx = [&](const char* f){ return dir + "/" + f; };

    {   // valid manifest: one record, fields parsed, typed key split + normalized
        ModelOverrideRegistry r;
        int n = r.loadFromFile(fx("valid.json"), dir);
        CHECK(n == 1);
        CHECK(r.count() == 1);
        const ModelOverrideRecord* m = r.resolve("staticProp", "example_name");
        CHECK(m != nullptr);
        if (m) {
            CHECK(m->overrideClass == "staticprop");      // normalized lower
            CHECK(m->appearanceName == "example_name");   // normalized lower
            CHECK(m->scale == 1.0f);
            CHECK(m->sourceRelPath == "source/props/example.glb");
        }
        // resolve is case-insensitive on both args (normalized internally)
        CHECK(r.resolve("STATICPROP", "Example_Name") != nullptr);
        CHECK(r.resolve("tree", "example_name") == nullptr);   // class scoped
        CHECK(r.resolve(nullptr, "example_name") == nullptr);  // mech path
        CHECK(r.resolve("staticProp", nullptr) == nullptr);    // null guard
    }
    {   // renderOnly=false → dropped
        ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("not_renderonly.json"), dir) == 0);
    }
    {   // fallback!="stock" → dropped
        ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("bad_fallback.json"), dir) == 0);
    }
    {   // scale!=1.0 → dropped (MVP requires scale==1.0)
        ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("bad_scale.json"), dir) == 0);
    }
    {   // every unsafe path rejected: absolute, .., drive letter, bad ext
        ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("unsafe_paths.json"), dir) == 0);
    }
    {   // duplicate normalized key: first wins, second dropped
        ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("dup.json"), dir) == 1);
        const ModelOverrideRecord* m = r.resolve("staticProp", "dup_name");
        CHECK(m != nullptr);
        if (m) CHECK(m->sourceRelPath == "first.glb");   // first wins
    }
    {   // non-object array entries skipped; the one valid object survives
        ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("non_object_entry.json"), dir) == 1);
        CHECK(r.resolve("tree", "ok") != nullptr);
    }
    {   // malformed JSON → 0, no crash
        ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("malformed.json"), dir) == 0);
    }
    {   // missing file → 0, no crash
        ModelOverrideRegistry r;
        CHECK(r.loadFromFile(fx("does_not_exist.json"), dir) == 0);
    }
    printf(failures ? "TESTS FAILED (%d)\n" : "ALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
```

- [ ] **Step 3: CMake target (binary in build tree; fixtures dir passed in, NOT copied/emitted into source)**

Create `tests/model_override/CMakeLists.txt`:
```cmake
add_executable(model_override_tests
  test_model_override_registry.cpp
  ${CMAKE_SOURCE_DIR}/mclib/model_override_registry.cpp)
target_include_directories(model_override_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/mclib
  ${CMAKE_SOURCE_DIR}/3rdparty/include)
target_compile_features(model_override_tests PRIVATE cxx_std_17)
# Binary stays in the build tree (default RUNTIME_OUTPUT_DIRECTORY). Fixtures
# live in the source tree and are passed as argv[1] via the test command — we
# do NOT set RUNTIME_OUTPUT_DIRECTORY into the source dir.
add_test(NAME model_override_tests
  COMMAND model_override_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
```

Add to root `CMakeLists.txt` near other test wiring (guarded by existing `MC2_BUILD_TESTS`):
```cmake
if(MC2_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/model_override)
endif()
```

- [ ] **Step 4: Configure + build, verify it FAILS to link/compile**

Run: `cmake --build build --target model_override_tests`
Expected: FAIL — `model_override_registry.cpp` does not exist yet (link/compile error).

- [ ] **Step 5: Do NOT commit the red test**

Leave the failing test + fixtures + CMake **uncommitted**. They are committed together with the passing implementation in Task 4 Step 6 (one green commit — never a committed red state).

### Task 4: Implement the registry

**Files:**
- Create: `mclib/model_override_registry.cpp`
- Modify: `mclib/CMakeLists.txt`

- [ ] **Step 1: Implement**

```cpp
// model_override_registry.cpp — MODEL-OVERRIDE-MVP-1 Slice 1.
// ONLY engine TU permitted to include nlohmann/json (scripts/check-json-isolation.sh).
#include "model_override_registry.h"
#include <nlohmann/json.hpp>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <utility>   // std::move

using nlohmann::json;

static void logDrop(const char* why, const std::string& key) {
    fprintf(stderr, "[MODOVERRIDE] dropped '%s': %s\n", key.c_str(), why);
    fflush(stderr);
}

// Trim ASCII whitespace + lowercase. Used to normalize class & appearance name
// so manifest keys match the engine's case-insensitive appearance lookup
// (mclib/apprtype.cpp:230 S_stricmp) and tolerate stray spaces around ':'.
static std::string normalizeKey(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    std::string out = s.substr(b, e - b);
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

// Reject unsafe / non-glTF source paths. Modder assets must be a relative path
// under the manifest dir ending in .glb/.gltf — no absolute paths, no parent
// escapes, no drive letters.
static bool isSafeSource(const std::string& s) {
    if (s.empty()) return false;
    if (s[0] == '/' || s[0] == '\\') return false;                 // absolute (POSIX/UNC)
    if (s.size() >= 2 && s[1] == ':') return false;                // drive letter C:
    if (s.find("..") != std::string::npos) return false;           // parent escape
    // extension (case-insensitive) must be .glb or .gltf
    std::string low = s; for (char& c : low) c = (char)std::tolower((unsigned char)c);
    const bool glb  = low.size() >= 4 && low.compare(low.size() - 4, 4, ".glb")  == 0;
    const bool gltf = low.size() >= 5 && low.compare(low.size() - 5, 5, ".gltf") == 0;
    return glb || gltf;
}

int ModelOverrideRegistry::loadFromFile(const std::string& manifestPath,
                                        const std::string& manifestDir) {
    records_.clear();
    manifestDir_ = manifestDir;

    std::ifstream in(manifestPath.c_str());
    if (!in.is_open()) {
        // Absent manifest is normal (no mods). Not an error.
        return 0;
    }
    json root;
    try { in >> root; }
    catch (const std::exception& e) {
        fprintf(stderr, "[MODOVERRIDE] parse error in %s: %s\n",
                manifestPath.c_str(), e.what());
        return 0;
    }
    if (!root.is_object() || !root.contains("overrides") ||
        !root["overrides"].is_array()) {
        fprintf(stderr, "[MODOVERRIDE] %s: missing 'overrides' array\n",
                manifestPath.c_str());
        return 0;
    }

    for (const auto& e : root["overrides"]) {
        // Guard non-object array entries (numbers, strings, null) — skip, no crash.
        if (!e.is_object()) { logDrop("entry is not an object", "<non-object>"); continue; }

        std::string replaces = e.value("replaces", std::string());
        const std::string key = replaces.empty() ? "<no-replaces>" : replaces;

        if (e.value("type", std::string()) != "model") { logDrop("type!=model", key); continue; }
        if (!e.value("renderOnly", false))             { logDrop("renderOnly!=true", key); continue; }
        if (e.value("fallback", std::string()) != "stock") { logDrop("fallback!=stock", key); continue; }

        // MVP requires scale == 1.0 exactly; any other value is rejected.
        const float scale = e.value("scale", 1.0f);
        if (scale != 1.0f) { logDrop("scale!=1.0 (MVP requires 1.0)", key); continue; }

        // typed key "<class>:<appearanceName>" (split on FIRST ':' so a stray
        // space-padded "  staticProp : name " still parses, then normalize).
        size_t colon = replaces.find(':');
        if (colon == std::string::npos) { logDrop("replaces not '<class>:<name>'", key); continue; }
        std::string cls  = normalizeKey(replaces.substr(0, colon));
        std::string name = normalizeKey(replaces.substr(colon + 1));
        if (cls.empty() || name.empty()) { logDrop("empty class or name in replaces", key); continue; }

        // MVP allows only staticProp + tree (normalized). mech/vehicle excluded.
        if (cls != "staticprop" && cls != "tree") { logDrop("class not staticProp|tree", key); continue; }
        // class field (if present) must agree with the typed key (normalized).
        if (e.contains("class")) {
            std::string clsField = normalizeKey(e.value("class", std::string()));
            if (clsField != cls) { logDrop("class field disagrees with replaces", key); continue; }
        }

        std::string source = e.value("source", std::string());
        if (!isSafeSource(source)) { logDrop("unsafe/non-glTF source path", key); continue; }

        // Duplicate normalized key: first valid entry wins, later ones dropped.
        bool dup = false;
        for (const auto& r : records_) {
            if (r.overrideClass == cls && r.appearanceName == name) { dup = true; break; }
        }
        if (dup) { logDrop("duplicate key (first entry wins)", key); continue; }

        ModelOverrideRecord rec;
        rec.overrideClass  = cls;     // normalized lower
        rec.appearanceName = name;    // normalized lower
        rec.sourceRelPath  = source;  // kept verbatim (validated safe)
        rec.scale          = scale;   // == 1.0f
        records_.push_back(std::move(rec));
    }
    return (int)records_.size();
}

const ModelOverrideRecord* ModelOverrideRegistry::resolve(
        const char* overrideClass, const char* appearanceName) const {
    if (!overrideClass || !appearanceName) return nullptr;  // mech path / guard
    const std::string cls  = normalizeKey(overrideClass);
    const std::string name = normalizeKey(appearanceName);
    for (const auto& r : records_) {
        if (r.overrideClass == cls && r.appearanceName == name)
            return &r;
    }
    return nullptr;
}

ModelOverrideRegistry& ModelOverrideRegistry::instance() {
    static ModelOverrideRegistry g;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;  // set first: a parse failure must not retry every call
        g.loadFromFile("data/model_overrides/models.json", "data/model_overrides");
        fprintf(stderr, "[MODOVERRIDE] registry loaded: %d override(s)\n", g.count());
        fflush(stderr);
    }
    return g;
}
```

- [ ] **Step 2: Add to mclib build + include path**

In `mclib/CMakeLists.txt`: add `model_override_registry.cpp` to the mclib source list, and ensure `${CMAKE_SOURCE_DIR}/3rdparty/include` is on the include path **for that file's target** (mclib library).

- [ ] **Step 3: Build the unit test — verify PASS**

Run: `cmake --build build --target model_override_tests && ctest --test-dir build -R model_override_tests --output-on-failure`
Expected: `ALL TESTS PASSED` (ctest passes the source-tree `fixtures/` dir as argv[1]; the binary stays in `build/`).

- [ ] **Step 4: Re-run isolation guard**

Run: `sh scripts/check-json-isolation.sh`
Expected: `json isolation OK` (only the two allowed TUs include the header).

- [ ] **Step 5: Add empty stock manifest**

Create `data/model_overrides/models.json`:
```json
{ "overrides": [] }
```

- [ ] **Step 6: Commit**

```bash
# Single GREEN commit: tests + fixtures + CMake (held back from Task 3) land
# together with the passing implementation. Never a committed red state.
git add mclib/model_override_registry.cpp mclib/CMakeLists.txt \
        data/model_overrides/models.json \
        tests/model_override/ CMakeLists.txt
git commit -m "feat(modoverride): registry parse/validate/lookup + tests + empty manifest"
```

**Slice 1 done when:** `model_override_tests` prints `ALL TESTS PASSED`, isolation guard passes, empty manifest loads to 0 overrides with no error.

---

## Task 0 — `MODEL-OVERRIDE-COLLISION-AUTHORITY-PROOF-0` (HARD GATE before Slice 2)

**Goal:** Prove, from the code, whether `cellsCovered` / collision footprint is derived from the **same `TG_TypeMultiShape` pointer** that the override hook would replace. The hook fills `bldgShape[i]` / `treeShape[i]` in place via `LoadFromFile`; if collision reads those same pointers, the override silently changes gameplay and the "render-only" premise is FALSE.

**This gate blocks Slice 2.** Do not redirect any loader until the authority is proven.

### Strong prior (must be confirmed or refuted, not assumed)
`BldgAppearance::calcCellsCovered` (`mclib/bdactor.cpp:2763-2827`) loops `bldgShape`'s vertices (`GetShapeVertexInEditor`) to mark passability; `TerrainObject::calcCellFootprint` calls it at spawn (`terrobj.cpp:1369-1415`, cached `terrobj.cpp:1442-1479`). If `calcCellsCovered` reads the **same** `bldgShape[]`/`treeShape[]` that `LoadFromFile` overwrote, collision WILL track the override mesh → **not render-only**.

### Task 0 steps

- [ ] **Step 1: Trace the collision shape pointer**

In `mclib/bdactor.cpp`, confirm which member `calcCellsCovered` (buildings ~`:2763`) and the tree equivalent read their vertices from. Confirm whether it is the identical `bldgShape[i]` / `treeShape[i]` array element that `LoadFromFile` populates in Slice 2 (the base-LOD pointers at `:211-235` / `:3389-3414`). Record the exact member + line in `docs/model-override-mvp-notes.md`.

- [ ] **Step 2: Trace timing**

Confirm `calcCellsCovered` runs at terrain-object spawn (per-instance) reading the appearance-type shape that was loaded once at type init. Confirm the cached `cellsCovered` is never re-derived after spawn (`terrobj.cpp:1442-1479`). Record.

- [ ] **Step 3: Verdict**

Write the verdict in the notes file:
  - **CASE A — same pointer (expected):** collision reads the overridden shape. **STOP. Render-only is FALSE with the single-shape hook.** Adopt the **dual-shape redesign** (below) before Slice 2.
  - **CASE B — separate source (e.g. collision reads a distinct stock member, a `.FITS` footprint, or a stock-only damage/collision shape):** single-shape hook is render-only-safe; proceed to Slice 2 as written, citing the evidence.

- [ ] **Step 4: Dual-shape redesign (only if CASE A)**

Do **not** overwrite the gameplay-authority shape. Instead:
  - Keep the stock shape as collision/gameplay authority: `calcCellsCovered` MUST continue to read a shape loaded from the **stock `.ase`** (never the override).
  - Introduce a separate render-authority shape that the override fills, consumed only by the render/cull/registry path (`render()`, `getRadius()`, static-prop registry, `OBBRadius`/`highZ`).
  - Concretely: add a `bldgRenderShape` / `treeRenderShape` (override-or-null) alongside the existing stock `bldgShape[]`/`treeShape[]`. Load stock as today (unchanged → collision untouched). After stock load, if an override resolves, load it into the render shape. Point `render()` + `getRadius()` + registry submission at the render shape when present, else the stock shape. `calcCellsCovered` stays bound to the stock shape — **never** the render shape.
  - This makes Slice 2's "redirect the base loader to `LoadFromFile`" WRONG (it would overwrite the authority shape). Replace it with: stock load unchanged + an additional render-shape override load. Update Slice 2 tasks accordingly during re-evaluation.

- [ ] **Step 5: Commit the proof (notes only; no code yet)**

```bash
git add docs/model-override-mvp-notes.md
git commit -m "docs(modoverride): Task 0 collision-authority proof + render-only verdict"
```

**Task 0 gate clears when:** the notes file states CASE A or CASE B with cited file:line evidence, and (if CASE A) the dual-shape adjustment to Slice 2 is written down before any loader code is touched.

---

## Slice 2 — `MODEL-OVERRIDE-LOADFROMFILE-HOOK-1`

> **BLOCKED by Task 0.** The single-shape redirect below is valid **only under CASE B**. Under CASE A, apply the dual-shape redesign (Task 0 Step 4): keep the stock-shape load unchanged and add a *separate* render-shape override load instead of redirecting the authority pointer.

**Goal:** Resolve overrides at `TG_TypeMultiShape::LoadFromFile`; route the static-prop/tree **render** path to it; stock path byte-identical when no override matches; warn-and-fallback on import failure; collision authority untouched (per Task 0). No `tgl.fst` mutation.

### Task 5: Extend `LoadFromFile` signature

**Files:**
- Modify: `mclib/msl.h` (declaration of `LoadFromFile`)

- [ ] **Step 1: Add optional class param to the declaration**

Find `long LoadFromFile(const char* baseName);` in `mclib/msl.h` and change to:
```cpp
// overrideClass: "staticProp" | "tree" enables model-override resolution
// (MODEL-OVERRIDE-MVP-1). nullptr (default, e.g. mech path) disables it.
long LoadFromFile(const char* baseName, const char* overrideClass = nullptr);
```

- [ ] **Step 2: Build to confirm existing mech call site still compiles (default arg)**

Run: `cmake --build build --target mclib`
Expected: PASS (existing `LoadFromFile(baseName)` callers bind the default `nullptr`).

- [ ] **Step 3: Commit**

```bash
git add mclib/msl.h
git commit -m "feat(modoverride): LoadFromFile gains optional overrideClass param"
```

### Task 6: Insert override resolution into `LoadFromFile`

**Files:**
- Modify: `mclib/msl.cpp:438-479`

- [ ] **Step 1: Add the include + resolution block at the top of the function body**

At the top of `mclib/msl.cpp` add:
```cpp
#include "model_override_registry.h"
```
Then in `LoadFromFile`, immediately after the `if (!baseName || !*baseName) return -1;` guard and the existing trace line, insert:
```cpp
    // MODEL-OVERRIDE-MVP-1: render-only model override resolution. Keyed by
    // <class>:<basename>. On a hit, import the override .glb/.gltf and return;
    // on import failure, warn and fall through to the stock probe chain below.
    // overrideClass==nullptr (mech path) skips this entirely.
    if (overrideClass) {
        const ModelOverrideRecord* ov =
            ModelOverrideRegistry::instance().resolve(overrideClass, baseName);
        if (ov) {
#ifdef ENABLE_ASSIMP_IMPORTER
            FullPathFileName ovPath;
            // sourceRelPath is relative to the manifest dir (data/model_overrides).
            ovPath.init("data" PATH_SEPARATOR "model_overrides" PATH_SEPARATOR,
                        ov->sourceRelPath.c_str(), "");
            if (fileExists(ovPath, FILE_ON_DISK)) {
                long r = ImportGeometryFromFile(ovPath, this);
                if (r == 0) {
                    fprintf(stderr, "[MODOVERRIDE] applied %s:%s <- %s\n",
                            overrideClass, baseName, (const char*)ovPath);
                    fflush(stderr);
                    return NO_ERR;
                }
                fprintf(stderr, "[MODOVERRIDE] import FAILED for %s:%s (%s); "
                                "using stock\n", overrideClass, baseName,
                                (const char*)ovPath);
            } else {
                fprintf(stderr, "[MODOVERRIDE] source MISSING for %s:%s (%s); "
                                "using stock\n", overrideClass, baseName,
                                (const char*)ovPath);
            }
            fflush(stderr);
#else
            fprintf(stderr, "[MODOVERRIDE] %s:%s override ignored "
                            "(ENABLE_ASSIMP_IMPORTER off); using stock\n",
                            overrideClass, baseName);
            fflush(stderr);
#endif
            // fall through to stock probe chain (fallback=stock invariant)
        }
    }
```
The existing `.glb→.fbx→.ase` probe chain and the ASE fallback remain unchanged below this block.

> Note on `sourceRelPath` extension: `ImportGeometryFromFile` accepts the full path *with* extension, so `ovPath` keeps the `.glb`/`.gltf` from the manifest (`FullPathFileName.init(dir, relWithExt, "")`). Verify `init` with an empty extension does not strip — if it does, build the path by concatenation instead.

- [ ] **Step 2: Build mclib**

Run: `cmake --build build --target mclib`
Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add mclib/msl.cpp
git commit -m "feat(modoverride): resolve render-only overrides in LoadFromFile"
```

### Task 7: Redirect static-prop + tree base loaders to `LoadFromFile`

**Files:**
- Modify: `mclib/bdactor.cpp` (building base: `:217` and `:235`; tree base: `:3391` and `:3414`)

- [ ] **Step 1: Redirect the building base-LOD and base call sites**

At `mclib/bdactor.cpp:217` change:
```cpp
				bldgShape[i]->LoadTGMultiShapeFromASE(bldgName);
```
to:
```cpp
				// MODEL-OVERRIDE-MVP-1: route through LoadFromFile so a staticProp
				// override (or .glb/.fbx) can substitute; falls back to this same
				// .ase on no-match. aseFileName is the basename; LoadFromFile
				// appends tglPath + extension internally.
				bldgShape[i]->LoadFromFile(aseFileName, "staticProp");
```
At `mclib/bdactor.cpp:235` change:
```cpp
		bldgShape[0]->LoadTGMultiShapeFromASE(bldgName);
```
to:
```cpp
		bldgShape[0]->LoadFromFile(aseFileName, "staticProp");
```
Leave the damage-state load at `:252` on `LoadTGMultiShapeFromASE` (damage overrides are out of MVP scope). `bldgName` becomes unused at these two sites — leave its construction (it is still used for the damage path and logging) or silence the unused warning locally; do not delete shared declarations.

- [ ] **Step 2: Redirect the tree base-LOD and base call sites**

At `mclib/bdactor.cpp:3391` change:
```cpp
				treeShape[i]->LoadTGMultiShapeFromASE(treeName);
```
to:
```cpp
				treeShape[i]->LoadFromFile(aseFileName, "tree");
```
At `mclib/bdactor.cpp:3414` change:
```cpp
		treeShape[0]->LoadTGMultiShapeFromASE(treeName);
```
to:
```cpp
		treeShape[0]->LoadFromFile(aseFileName, "tree");
```
Leave the tree damage load at `:3434` on `LoadTGMultiShapeFromASE`. (Confirm the tree base reads its basename into `aseFileName` like the building path; if the variable is named differently, pass that basename. Do **not** pass the full `treeName` path.)

- [ ] **Step 3: Build the engine**

Run: `cmake --build build --target mc2 --config RelWithDebInfo`
Expected: PASS.

- [ ] **Step 4: No-mod identity smoke (manual gate this slice; automated in Slice 3)**

Run the engine with the empty `data/model_overrides/models.json`. Expect `[MODOVERRIDE] registry loaded: 0 override(s)` on stderr and **no** `[MODOVERRIDE] applied` lines — stock props/trees load via the ASE fallback exactly as before.

- [ ] **Step 5: Commit**

```bash
git add mclib/bdactor.cpp
git commit -m "feat(modoverride): route staticProp+tree base loaders through LoadFromFile"
```

**Slice 2 done when:** engine builds; with the empty manifest, no override is applied and stock rendering is unchanged; the hook is in one function plus four redirected call sites; `tgl.fst` untouched.

---

## Slice 3 — `STATICPROP-MODEL-OVERRIDE-PROOF-1`

**Goal:** Prove one opaque static-prop replacement: render mesh changes, render bounds recompute from the new mesh, stock `cellsCovered`/gameplay unchanged, stock fallback when source missing, no GL errors, +0 destroys.

### Task 8: Confirm render-bounds recompute is mesh-driven (read + assert, no behavior change)

**Files:**
- Read: `mclib/bdactor.cpp:693-744`
- Verify: `mclib/assimp_importer.cpp` `ComputeBoundingBox` (min/max box source)

- [ ] **Step 1: Verify `OBBRadius`/`highZ` derive from the loaded shape's box**

Confirm `BldgAppearance::init` computes `OBBRadius`/`highZ` from `bldgShape->GetMinBox()/GetMaxBox()/GetRootNodeCenter()` (`mclib/bdactor.cpp:693-744`). Because the override mesh replaces `bldgShape` *before* this runs, the recompute is automatic — **no edit needed here** if the importer fills `minBox`/`maxBox` from real vertices.

- [ ] **Step 2: Verify the importer's bounding box is vertex-accurate**

Inspect `ComputeBoundingBox` in `mclib/assimp_importer.cpp`. The recon flagged it as "node-center based / loose." If it does **not** expand the box over every vertex, fix it to take the min/max over all imported vertex positions (post axis-transform) so `GetMinBox()/GetMaxBox()` are tight. Add/keep this as the only change:
```cpp
// Expand box over every transformed vertex (not just node centers) so
// downstream OBBRadius/highZ (bdactor.cpp:693-744) and frustum/HZB cull
// bounds are correct for the replacement mesh. MODEL-OVERRIDE-MVP-1.
```
If it already iterates vertices, record that in `docs/model-override-mvp-notes.md` and make no change.

- [ ] **Step 3: Commit (only if the importer box was fixed)**

```bash
git add mclib/assimp_importer.cpp docs/model-override-mvp-notes.md
git commit -m "fix(modoverride): tight vertex AABB so override render bounds recompute correctly"
```

### Task 9: Author the proof manifest + asset; manual proof

**Files:**
- Modify: `data/model_overrides/models.json` (proof override; reverted before final no-mod gate)
- Add (test mod, not stock data): `data/model_overrides/source/props/<prop>.glb`
- Append: `docs/model-override-mvp-notes.md`

- [ ] **Step 1: Pick one opaque stock static prop**

Choose a simple opaque prop appearance (no foliage alpha). Record its appearance/`FileName` basename in `docs/model-override-mvp-notes.md` as `<prop>` and the typed key `staticProp:<prop>`.

- [ ] **Step 2: Provide a replacement GLB**

Place an opaque test GLB at `data/model_overrides/source/props/<prop>.glb` (a recognizably different shape, comparable footprint per §4 of the recon). Document its triangle count + AABB.

- [ ] **Step 3: Write the proof manifest**

```json
{ "overrides": [
  { "type":"model","class":"staticProp","replaces":"staticProp:<prop>",
    "source":"source/props/<prop>.glb","renderOnly":true,"scale":1.0,"fallback":"stock" }
] }
```

- [ ] **Step 4: Run + observe (proof, not a gate)**

Launch a mission containing `<prop>`. Confirm, recording each in the notes file:
- `[MODOVERRIDE] applied staticProp:<prop>` on stderr; replacement mesh visible.
- No GL errors (`MC2_DEBUG_STATE_DUMP=1` / GL error log clean).
- Destroy count unchanged (+0 destroys) vs stock baseline.
- Pathfinding/collision unchanged: a unit routes around the prop on the same cells as stock (cellsCovered frozen). Note any clip if the GLB overhangs the stock footprint (expected, cosmetic).

- [ ] **Step 5: Fallback proof**

Rename the GLB away; relaunch. Confirm `[MODOVERRIDE] source MISSING ... using stock` and the stock prop renders. Restore the GLB.

- [ ] **Step 6: Commit the proof artifacts + notes (manifest left as proof; see Task 11 for gate reset)**

```bash
git add data/model_overrides/source/props/<prop>.glb docs/model-override-mvp-notes.md
git commit -m "test(modoverride): opaque static-prop override proof asset + notes"
```

### Task 10: Automated smoke gates

**Files:**
- Modify: `scripts/run_smoke.py`

- [ ] **Step 1: Add three smoke cases**

Add cases that (1) boot with the empty manifest and assert **byte-identical** capture vs the stock baseline + zero `[MODOVERRIDE] applied` lines; (2) boot with the proof manifest and assert one `[MODOVERRIDE] applied staticProp:<prop>` line, zero GL errors, +0 destroys; (3) boot with the proof manifest but source removed and assert the `source MISSING ... using stock` line and a clean run. Follow the existing harness's case structure and stderr-grep helpers.

- [ ] **Step 2: Run the smoke suite**

Run: `python scripts/run_smoke.py` (per the repo's smoke invocation).
Expected: all three new cases pass alongside the existing suite.

- [ ] **Step 3: Commit**

```bash
git add scripts/run_smoke.py
git commit -m "test(modoverride): smoke gates — no-mod identity, applied, missing-source fallback"
```

### Task 11: Reset default manifest to empty (no-mod identity is the shipped default)

**Files:**
- Modify: `data/model_overrides/models.json`

- [ ] **Step 1: Restore the empty stock manifest**

```json
{ "overrides": [] }
```
The proof manifest content lives only in the smoke fixtures / notes, not the shipped default.

- [ ] **Step 2: Final no-mod identity gate**

Run the no-mod smoke case again; confirm byte-identical to stock baseline.

- [ ] **Step 3: Commit**

```bash
git add data/model_overrides/models.json
git commit -m "chore(modoverride): ship empty default manifest (no-mod identity)"
```

**Slice 3 done when:** replacement prop renders under the proof manifest; render bounds track the new mesh (no pop); `cellsCovered`/destroys/gameplay unchanged; missing source falls back to stock; no GL errors; no-mod default is byte-identical to stock.

---

## Slice 4 — `TREE-MODEL-OVERRIDE-PROOF-1`

**Goal:** Apply the same path to one tree, adding alpha-mask leaves at fixed cutoff 0.5; render bounds recomputed; stock footprint/collision preserved; stock fallback; no GL errors.

### Task 12: Tree override proof with alpha-mask leaves

**Files:**
- Add (test mod): `data/model_overrides/source/trees/<tree>.glb`
- Append: `docs/model-override-mvp-notes.md`
- (No new engine code expected — the `tree` class already routes via Task 7.)

- [ ] **Step 1: Pick one stock tree; author the GLB**

Record the tree appearance basename and typed key `tree:<tree>` in the notes. Author `data/model_overrides/source/trees/<tree>.glb` with leaf materials set to glTF `alphaMode="MASK"`, `alphaCutoff` ~0.5 (engine uses fixed 0.5 — `shaders/static_prop.frag:215`). Document triangle count + AABB.

- [ ] **Step 2: Double-sided leaves decision**

The static-prop renderer's `kDoubleSided` flag is defined but not wired (recon §5). For MVP, author leaf cards as double-geometry (front+back quads) rather than relying on double-sided state. If leaves render single-sided/black-backed, **document it as known debt** in the notes — do not wire double-sided in this MVP.

- [ ] **Step 3: Proof manifest (temporary, for the gate fixture)**

```json
{ "overrides": [
  { "type":"model","class":"tree","replaces":"tree:<tree>",
    "source":"source/trees/<tree>.glb","renderOnly":true,"scale":1.0,"fallback":"stock" }
] }
```

- [ ] **Step 4: Run + observe**

Confirm and record: `[MODOVERRIDE] applied tree:<tree>`; replacement tree visible; **no culling pop** when zooming/panning (render bounds recomputed from the new mesh — watch for the tree vanishing at screen edges, which would indicate stale stock bounds); leaves alpha-cut (no opaque quads); stock fallback works with source removed; zero GL errors; stock footprint/collision unchanged.

- [ ] **Step 5: Add tree smoke case + reset default manifest**

Add a `tree:<tree>` applied-case to `scripts/run_smoke.py` (mirror Task 10 case 2). Ensure the shipped `data/model_overrides/models.json` stays `{"overrides":[]}`.

- [ ] **Step 6: Commit**

```bash
git add data/model_overrides/source/trees/<tree>.glb docs/model-override-mvp-notes.md scripts/run_smoke.py
git commit -m "test(modoverride): tree override proof — alpha-mask leaves + culling-pop + fallback gates"
```

**Slice 4 done when:** replacement tree renders with alpha-mask leaves; no culling pop from stale bounds; stock fallback works; stock collision/footprint unchanged; no GL errors; double-sided behavior documented.

---

## Risks & mitigations (plan-level)

| Risk | Mitigation in plan |
|---|---|
| **Override replaces the SAME shape pointer collision reads → silent gameplay change (render-only FALSE)** | **Task 0 HARD GATE** proves authority before any hook; CASE A forces dual-shape (stock = collision authority, override = render-only) |
| `FullPathFileName.init(dir, rel, "")` strips/garbles the `.glb` extension | Task 6 Step 1 note: verify; switch to concatenation if needed |
| Importer AABB is node-center loose → wrong cull bounds → pop | Task 8 Step 2: make the box vertex-tight before proving |
| Tree base loader uses a different basename variable than buildings | Task 7 Step 2: confirm; pass the basename, never the full path |
| Singleton load order vs path globals (if mod-profile rebinds `tglPath`) | Lazy load on first `LoadFromFile`, after boot path-binding; manifest path is fixed under `data/` |
| nlohmann double-vendor with mod-profile branch | Task 1 idempotent (same file/version); isolation guard shared |
| Damage-state shapes not overridden (asymmetry: base replaced, damage stock) | Out of MVP scope; documented; damage stays on `LoadTGMultiShapeFromASE` |

## Stop conditions (status at plan time)
- Identity ambiguous → typed `<class>:<name>` key + class-scoped lookup; class field cross-checked.
- glTF→mesh incompatible → not triggered; importer already terminates at `TG_TypeMultiShape`.
- Bounds clip replacement → Task 8 makes the box vertex-tight; recompute is automatic in `BldgAppearance::init`.
- Any mission/editor change required → none; explicitly out of scope.

## Self-review notes
- Spec coverage: Slice 1 (registry+rules+log-no-crash) ✓; Slice 2 (hook+fallback+probe preserved+no fst) ✓; Slice 3 (bounds recompute + stock collision + all four validation gates) ✓; Slice 4 (tree + alphaMode=mask 0.5 + double-sided note + bounds + fallback) ✓.
- Type consistency: `ModelOverrideRecord` fields and `resolve(overrideClass, appearanceName)` signature used identically in header, test, impl, and the `LoadFromFile` call site.
- Open item flagged for the implementer: confirm tree-loader basename variable name and `FullPathFileName.init` empty-extension behavior (both called out inline) — neither blocks planning.
