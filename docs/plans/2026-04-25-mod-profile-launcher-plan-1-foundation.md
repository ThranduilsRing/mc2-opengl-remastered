# Mod Profile Launcher — Plan 1: Foundation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the engine-side profile-loading machinery so `mc2.exe --profile <id>` resolves a profile + base chain and rewrites MC2's content-path globals before any subsystem reads them, while leaving stock behavior unchanged.

**Architecture:** A new isolated `profile_manager` translation unit owns JSON parsing (via vendored `nlohmann/json`), profile validation, base-chain resolution, and path-global binding. Engine init calls one entry point during `ParseCommandLine`/early boot. ABL policy, campaign slots, saves, graphics declarations, and launcher UI are NOT in this plan — `profile.json` fields for those are accepted by the parser but produce no behavior in Plan 1.

**Deferred from Plan 1:** the design doc mentions a `ProfileId=` key in `mc2.cfg` as a config fallback for `--profile`. Plan 1 implements **CLI only** (and the implicit default `stock`). Config-file fallback is deferred to either the launcher-UI plan (Plan 5, where the launcher writes `mc2.cfg` for the user) or a small follow-up task — whichever lands first. Documenting this here so the implementer doesn't surprise themselves looking for it.

**Tech Stack:** C++ (engine), `nlohmann/json` (vendored single-header), CMake 3.10, MSVC `RelWithDebInfo`, Python 3 + `tests/smoke/run_smoke.py` for integration gates.

**Language standard discipline:** The engine builds at MSVC default (C++14) and `-std=c++0x` (C++11) on non-MSVC. **Production `profile_manager.cpp` MUST stay within C++14** — no `std::filesystem`, `std::optional`, `std::variant`, `std::string_view`, `if constexpr`. Use POSIX `stat()` / `<sys/stat.h>` (works on MSVC + MSYS2) for directory-existence checks. **C++17 is opted into ONLY for the `profile_tests` target** via `target_compile_features(profile_tests PRIVATE cxx_std_17)` — that's where `std::filesystem` lives. nlohmann/json itself is C++11-compatible, so the production TU stays clean.

**Reads:**
- Spec: `docs/plans/2026-04-25-mod-profile-launcher-scope-additions.md`
- Companion: `docs/plans/2026-04-23-mod-profile-launcher-design.md`, `docs/plans/2026-04-23-mod-profile-launcher-prep-notes.md`
- ABL precedence rule: `docs/observations/2026-04-25-abl-library-shadow-rule.md`

---

## File Structure

**Created:**
- `3rdparty/include/nlohmann/json.hpp` — vendored single-header JSON library (~900 KB).
- `THIRD_PARTY_NOTICES.md` — license attribution registry (created if missing).
- `code/profile_manager.h` — public API for profile resolution + path binding.
- `code/profile_manager.cpp` — JSON parsing, validation, chain resolution, path binding. **Only TU that includes `nlohmann/json.hpp`.**
- `tests/profile/test_profile_manager.cpp` — standalone C++ test runner.
- `tests/profile/CMakeLists.txt` — `profile_tests` target.
- `tests/profile/fixtures/` — small JSON fixtures used by the unit tests (created per-test as needed; `.gitignore` for any temp output).
- `profiles/stock/profile.json` — built-in stock profile descriptor.
- `profiles/magic_corebrain_only/profile.json` — minimal `base: "stock"` smoke profile.

**Modified:**
- `CMakeLists.txt` — add `tests/profile` subdirectory (gated behind a `MC2_BUILD_TESTS` option, default ON).
- `code/mechcmd2.cpp` — extend `ParseCommandLine` to recognize `--profile <id>` / `-profile <id>`; call `profile_manager::bindActive()` after parsing, before any subsystem touches path globals.
- `mclib/paths.cpp` (and/or callers) — no signature changes; the existing `char[80]` externs `missionPath`, `objectPath`, `artPath`, `tglPath`, `texturePath`, `campaignPath`, `moviePath`, `effectsPath`, `soundPath`, `interfacePath`, `warriorPath` are rewritten in place by the binder (the 11-global advisor scope). `terrainPath`, `tilePath`, `shapesPath`, `spritePath`, `fontPath` keep their compile-time defaults in Plan 1 — no v1 profile use case rebinds them. `savePath`, `saveTempPath`, `profilePath`, CD mirrors, and `transcriptsPath` also untouched (saves come in Plan 3; CD mirrors are dead code paths).
- `GameOS/gameos/gos_smoke.cpp` — rename smoke harness flag `--profile` to `--smoke-profile`. Treat the old bare `--profile` as a pass-through/no-op so the engine-level mod-profile parser owns the flag.
- `tests/smoke/run_smoke.py` — emit `--smoke-profile` instead of `--profile` for the smoke harness; add three new smoke cases that exercise mod-profile boot (stock, valid profile, missing profile).

**File-isolation invariant:** `nlohmann/json.hpp` is `#include`d ONLY from `code/profile_manager.cpp` and `tests/profile/test_profile_manager.cpp`. No other engine TU may include it. Enforced by an in-repo grep check added in Task 1.

---

## Task 1: Vendor `nlohmann/json` + license note

**Files:**
- Create: `3rdparty/include/nlohmann/json.hpp`
- Create: `THIRD_PARTY_NOTICES.md`
- Create: `scripts/check-json-isolation.sh`

- [ ] **Step 1: Download `nlohmann/json` v3.11.3 single-header release**

Run from worktree root:

```bash
mkdir -p 3rdparty/include/nlohmann
curl -fsSL -o 3rdparty/include/nlohmann/json.hpp \
  https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
```

Verify size:

```bash
wc -c 3rdparty/include/nlohmann/json.hpp
```

Expected: ~900,000 bytes (≥ 800 KB and ≤ 1 MB).

- [ ] **Step 2: Verify the include resolves under existing `THIRDPARTY_INCLUDE_DIRS`**

The CMake `include_directories(${THIRDPARTY_INCLUDE_DIRS})` line already covers `3rdparty/include/`. Confirm by reading [CMakeLists.txt:185](CMakeLists.txt:185) — no CMake change required for the header to be found.

- [ ] **Step 3: Create `THIRD_PARTY_NOTICES.md`**

```markdown
# Third-Party Notices

This project vendors third-party code under the licenses below.

## nlohmann/json (v3.11.3)

- Source: https://github.com/nlohmann/json
- License: MIT
- Vendored at: `3rdparty/include/nlohmann/json.hpp`
- Copyright (c) 2013-2025 Niels Lohmann

The MIT license text is reproduced at the top of `json.hpp`. Permission
is granted to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the software, subject to the conditions in that
license header.

## tracy

- Source: https://github.com/wolfpld/tracy
- License: BSD 3-Clause
- Vendored at: `3rdparty/tracy/`
```

- [ ] **Step 4: Create the JSON-isolation check script**

`scripts/check-json-isolation.sh`:

```bash
#!/usr/bin/env bash
# Fails if any TU outside the allowed list includes nlohmann/json.hpp.
# Plan 1 invariant: JSON parsing stays behind profile_manager.

set -e

ALLOWED=(
  "code/profile_manager.cpp"
  "tests/profile/test_profile_manager.cpp"
)

# Build a regex of allowed paths.
allowed_regex=""
for p in "${ALLOWED[@]}"; do
  if [ -n "$allowed_regex" ]; then allowed_regex="$allowed_regex|"; fi
  allowed_regex="$allowed_regex$p"
done

# Find offenders. Search C/C++ sources only.
offenders=$(grep -rln \
  --include='*.cpp' --include='*.h' --include='*.hpp' --include='*.cc' \
  --exclude-dir=3rdparty \
  --exclude-dir=.claude \
  --exclude-dir=out \
  --exclude-dir=build \
  -E '#[[:space:]]*include[[:space:]]+["<]nlohmann/json\.hpp[">]' \
  . 2>/dev/null | sed 's|^\./||' | grep -Ev "^($allowed_regex)$" || true)

if [ -n "$offenders" ]; then
  echo "ERROR: nlohmann/json.hpp included from non-allowed TU:" >&2
  echo "$offenders" >&2
  echo "" >&2
  echo "Plan 1 invariant: only profile_manager.cpp and its test may include the JSON header." >&2
  exit 1
fi

echo "OK: nlohmann/json.hpp isolation invariant holds."
exit 0
```

Make executable:

```bash
chmod +x scripts/check-json-isolation.sh
```

- [ ] **Step 5: Run the isolation check (must pass with empty allowed-set state)**

```bash
sh scripts/check-json-isolation.sh
```

Expected output: `OK: nlohmann/json.hpp isolation invariant holds.`
Expected exit: 0.

- [ ] **Step 6: Commit**

```bash
git add 3rdparty/include/nlohmann/json.hpp THIRD_PARTY_NOTICES.md scripts/check-json-isolation.sh
git commit -m "build: vendor nlohmann/json v3.11.3 + isolation guard

Single-header JSON parser used only by profile_manager (Plan 1
of mod profile launcher). MIT-licensed; attribution in
THIRD_PARTY_NOTICES.md. scripts/check-json-isolation.sh enforces
that no engine TU outside profile_manager and its test pulls in
the header.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Rename smoke harness flag `--profile` → `--smoke-profile`

**Files:**
- Modify: `GameOS/gameos/gos_smoke.cpp:73-74`
- Modify: `tests/smoke/run_smoke.py`

- [ ] **Step 1: Update `gos_smoke.cpp` flag name + add transition error**

Replace the existing `--profile` branch in [GameOS/gameos/gos_smoke.cpp:73-76](GameOS/gameos/gos_smoke.cpp:73) with:

```cpp
        } else if (std::strcmp(a, "--smoke-profile") == 0) {
            if (i + 1 >= argc) failMissingValue("--smoke-profile");
            g_state.profile = argv[++i];
        } else if (std::strcmp(a, "--profile") == 0) {
            // Transition window: smoke harness used to own --profile.
            // The mod profile launcher (Plan 1) now owns that name.
            // Pass through to mc2's own ParseCommandLine; do NOT
            // consume the value.
        }
```

Note the transition branch consumes only the flag itself, NOT the next argv. This lets `mc2.exe --profile foo --smoke-profile passive` work cleanly even with old scripts mid-migration.

- [ ] **Step 2: Update Python smoke harness**

Find every occurrence of `"--profile"` in `tests/smoke/run_smoke.py` that targets the smoke harness, and replace with `"--smoke-profile"`. Verify with:

```bash
grep -n '"--profile"' tests/smoke/run_smoke.py
```

Expected: no remaining hits, OR remaining hits are intentional Plan 1 mod-profile invocations added in Task 9.

- [ ] **Step 3: Build sanity check**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build out --config RelWithDebInfo --target mc2 -- /m
```

Expected: clean build.

- [ ] **Step 4: Smoke regression — old menu canary + tier1 pass with the renamed flag**

```bash
py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 8 --fail-fast
```

Expected exit: 0.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_smoke.cpp tests/smoke/run_smoke.py
git commit -m "smoke: rename harness flag --profile to --smoke-profile

Frees the --profile name for the upcoming mod profile launcher.
The smoke parser still recognises bare --profile as a transition
no-op so mid-migration command lines do not consume the next
argv. tests/smoke/run_smoke.py emits the new flag.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Profile data model + JSON parsing (TDD)

**Files:**
- Create: `code/profile_manager.h`
- Create: `code/profile_manager.cpp`
- Create: `tests/profile/test_profile_manager.cpp`
- Create: `tests/profile/CMakeLists.txt`

- [ ] **Step 1: Write the header**

`code/profile_manager.h`:

```cpp
#pragma once

#include <map>
#include <string>
#include <vector>

namespace profile_manager {

struct ProfileInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string base;             // empty == no base
    std::string description;
    std::string requiresEngine;
    std::string defaultMission;

    std::map<std::string, bool> overrides;

    // Plan 1 parses these blocks for forward compatibility but
    // applies no behavior. Plans 2-4 own them.
    bool hasAblBlock = false;
    bool hasCampaignsBlock = false;
    bool hasGraphicsBlock = false;
};

enum class ParseStatus {
    Ok,
    FileNotFound,
    InvalidJson,
    SchemaError,
};

struct ParseResult {
    ParseStatus status = ParseStatus::Ok;
    std::string errorDetail;     // human-readable; empty on Ok
    ProfileInfo profile;
};

// Parse a profile.json on disk. The id field is required; if the file's
// "id" disagrees with the parent directory name, returns SchemaError.
// Caller passes the directory (e.g. "profiles/magic_corebrain_only");
// parser appends "/profile.json".
ParseResult parseProfileFile(const std::string& profileDir);

// Parse a JSON document already loaded into memory. Used by tests and
// by the built-in stock fallback. expectedId, when non-empty, is
// checked against the document's id field.
ParseResult parseProfileFromString(const std::string& jsonText,
                                   const std::string& expectedId);

}  // namespace profile_manager
```

- [ ] **Step 2: Write the failing tests**

`tests/profile/test_profile_manager.cpp`:

```cpp
#include "profile_manager.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace pm = profile_manager;

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

#define CHECK_EQ_STR(a, b) do { \
    const std::string _a = (a); \
    const std::string _b = (b); \
    if (_a != _b) { \
        std::fprintf(stderr, "FAIL %s:%d expected \"%s\" got \"%s\"\n", \
                     __FILE__, __LINE__, _b.c_str(), _a.c_str()); \
        ++g_failures; \
    } \
} while (0)

static void testParseStockMinimal() {
    const std::string json = R"({
        "id": "stock",
        "name": "Stock Carver V",
        "base": null
    })";
    auto r = pm::parseProfileFromString(json, "stock");
    CHECK(r.status == pm::ParseStatus::Ok);
    CHECK_EQ_STR(r.profile.id, "stock");
    CHECK_EQ_STR(r.profile.name, "Stock Carver V");
    CHECK_EQ_STR(r.profile.base, "");
    CHECK(!r.profile.hasAblBlock);
    CHECK(!r.profile.hasCampaignsBlock);
    CHECK(!r.profile.hasGraphicsBlock);
}

static void testParseFullSchema() {
    const std::string json = R"({
        "id": "magic_carver_v",
        "name": "Magic's Unofficial Patch",
        "version": "1.45",
        "author": "Magic",
        "base": "stock",
        "description": "smarter AI",
        "default_mission": "mc2_01.fit",
        "requires_engine": ">=0.1.1",
        "overrides": {"missions": true, "art": false},
        "abl": {"abx_libraries": []},
        "campaigns": [],
        "graphics": {"preset": "profile-default"}
    })";
    auto r = pm::parseProfileFromString(json, "magic_carver_v");
    CHECK(r.status == pm::ParseStatus::Ok);
    CHECK_EQ_STR(r.profile.id, "magic_carver_v");
    CHECK_EQ_STR(r.profile.base, "stock");
    CHECK_EQ_STR(r.profile.defaultMission, "mc2_01.fit");
    CHECK_EQ_STR(r.profile.requiresEngine, ">=0.1.1");
    CHECK(r.profile.overrides.size() == 2);
    CHECK(r.profile.overrides["missions"] == true);
    CHECK(r.profile.overrides["art"] == false);
    CHECK(r.profile.hasAblBlock);
    CHECK(r.profile.hasCampaignsBlock);
    CHECK(r.profile.hasGraphicsBlock);
}

static void testIdMismatch() {
    const std::string json = R"({"id":"foo","base":null})";
    auto r = pm::parseProfileFromString(json, "bar");
    CHECK(r.status == pm::ParseStatus::SchemaError);
    CHECK(r.errorDetail.find("id mismatch") != std::string::npos);
}

static void testInvalidJson() {
    auto r = pm::parseProfileFromString("{not json", "any");
    CHECK(r.status == pm::ParseStatus::InvalidJson);
}

static void testMissingId() {
    auto r = pm::parseProfileFromString(R"({"name":"x"})", "");
    CHECK(r.status == pm::ParseStatus::SchemaError);
    CHECK(r.errorDetail.find("missing required field: id") != std::string::npos);
}

static void testNullBaseMeansEmpty() {
    auto r = pm::parseProfileFromString(R"({"id":"x","base":null})", "x");
    CHECK(r.status == pm::ParseStatus::Ok);
    CHECK_EQ_STR(r.profile.base, "");
}

static void testMissingBaseMeansEmpty() {
    auto r = pm::parseProfileFromString(R"({"id":"x"})", "x");
    CHECK(r.status == pm::ParseStatus::Ok);
    CHECK_EQ_STR(r.profile.base, "");
}

int main() {
    testParseStockMinimal();
    testParseFullSchema();
    testIdMismatch();
    testInvalidJson();
    testMissingId();
    testNullBaseMeansEmpty();
    testMissingBaseMeansEmpty();

    if (g_failures > 0) {
        std::fprintf(stderr, "FAIL: %d assertion(s) failed\n", g_failures);
        return 1;
    }
    std::printf("OK: all profile_manager parsing tests passed\n");
    return 0;
}
```

- [ ] **Step 3: Write the CMake target**

`tests/profile/CMakeLists.txt`:

```cmake
add_executable(profile_tests
    test_profile_manager.cpp
    ${CMAKE_SOURCE_DIR}/code/profile_manager.cpp
)

target_include_directories(profile_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/code
    ${CMAKE_SOURCE_DIR}/3rdparty/include
)

# C++17 is required ONLY here because the test fixtures use
# std::filesystem to build temp dirs. Production profile_manager.cpp
# stays C++14-compatible -- it sees the same compile setting because
# CMake applies cxx_std_17 to the whole target, but it must not USE
# any C++17-only features. The engine target (mc2) does not get this
# bump.
target_compile_features(profile_tests PRIVATE cxx_std_17)

# Discoverable via ctest if the project ever wires it up.
add_test(NAME profile_tests COMMAND profile_tests)
```

Append to root [CMakeLists.txt](CMakeLists.txt) after line 203:

```cmake
option(MC2_BUILD_TESTS "Build engine unit tests (profile_tests, ...)" ON)
if(MC2_BUILD_TESTS)
    enable_testing()
    add_subdirectory("./tests/profile" "./out/tests/profile")
endif()
```

- [ ] **Step 4: Stub `profile_manager.cpp` so the test target links and FAILS**

`code/profile_manager.cpp` (initial stub — intentionally returns `SchemaError` for everything so the tests fail loudly):

```cpp
#include "profile_manager.h"

namespace profile_manager {

ParseResult parseProfileFromString(const std::string& jsonText,
                                   const std::string& expectedId) {
    (void)jsonText;
    (void)expectedId;
    ParseResult r;
    r.status = ParseStatus::SchemaError;
    r.errorDetail = "stub: parsing not implemented yet";
    return r;
}

ParseResult parseProfileFile(const std::string& profileDir) {
    (void)profileDir;
    ParseResult r;
    r.status = ParseStatus::SchemaError;
    r.errorDetail = "stub: parseProfileFile not implemented yet";
    return r;
}

}  // namespace profile_manager
```

- [ ] **Step 5: Configure + build the test target, run, expect FAIL**

```bash
cmake --build out --config RelWithDebInfo --target profile_tests -- /m
out/tests/profile/RelWithDebInfo/profile_tests.exe
```

Expected: nonzero exit. Multiple `FAIL` lines on stderr (every test fails because every call returns `SchemaError`).

- [ ] **Step 6: Implement `parseProfileFromString` against `nlohmann/json`**

Replace the body of `code/profile_manager.cpp` with the full implementation:

```cpp
#include "profile_manager.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace profile_manager {

namespace {

using nlohmann::json;

void readStringField(const json& obj, const char* key, std::string& out) {
    if (!obj.contains(key)) return;
    const auto& v = obj.at(key);
    if (v.is_string()) {
        out = v.get<std::string>();
    } else if (v.is_null()) {
        // null treated as empty
    }
}

void readBoolMap(const json& obj, const char* key,
                 std::map<std::string, bool>& out) {
    if (!obj.contains(key)) return;
    const auto& v = obj.at(key);
    if (!v.is_object()) return;
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it.value().is_boolean()) {
            out[it.key()] = it.value().get<bool>();
        }
    }
}

}  // namespace

ParseResult parseProfileFromString(const std::string& jsonText,
                                   const std::string& expectedId) {
    ParseResult r;
    json doc;

    try {
        doc = json::parse(jsonText);
    } catch (const json::parse_error& e) {
        r.status = ParseStatus::InvalidJson;
        r.errorDetail = e.what();
        return r;
    }

    if (!doc.is_object()) {
        r.status = ParseStatus::SchemaError;
        r.errorDetail = "root must be an object";
        return r;
    }

    if (!doc.contains("id") || !doc.at("id").is_string()) {
        r.status = ParseStatus::SchemaError;
        r.errorDetail = "missing required field: id";
        return r;
    }

    r.profile.id = doc.at("id").get<std::string>();

    if (!expectedId.empty() && r.profile.id != expectedId) {
        r.status = ParseStatus::SchemaError;
        r.errorDetail = "id mismatch: file says \"" + r.profile.id +
                        "\", expected \"" + expectedId + "\"";
        return r;
    }

    readStringField(doc, "name",            r.profile.name);
    readStringField(doc, "version",         r.profile.version);
    readStringField(doc, "author",          r.profile.author);
    readStringField(doc, "base",            r.profile.base);
    readStringField(doc, "description",     r.profile.description);
    readStringField(doc, "requires_engine", r.profile.requiresEngine);
    readStringField(doc, "default_mission", r.profile.defaultMission);
    readBoolMap   (doc, "overrides",        r.profile.overrides);

    r.profile.hasAblBlock       = doc.contains("abl")       && doc.at("abl").is_object();
    r.profile.hasCampaignsBlock = doc.contains("campaigns") && doc.at("campaigns").is_array();
    r.profile.hasGraphicsBlock  = doc.contains("graphics")  && doc.at("graphics").is_object();

    r.status = ParseStatus::Ok;
    return r;
}

ParseResult parseProfileFile(const std::string& profileDir) {
    ParseResult r;
    const std::string path = profileDir + "/profile.json";

    // C++14: use stdio file I/O, not std::ifstream-only streams.
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        r.status = ParseStatus::FileNotFound;
        r.errorDetail = "could not open " + path;
        return r;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();

    // Derive expectedId from the directory name.
    std::string expectedId = profileDir;
    const std::string::size_type slash = expectedId.find_last_of("/\\");
    if (slash != std::string::npos) expectedId = expectedId.substr(slash + 1);

    return parseProfileFromString(text, expectedId);
}

}  // namespace profile_manager
```

- [ ] **Step 7: Rebuild + run tests, expect PASS**

```bash
cmake --build out --config RelWithDebInfo --target profile_tests -- /m
out/tests/profile/RelWithDebInfo/profile_tests.exe
```

Expected output: `OK: all profile_manager parsing tests passed`
Expected exit: 0.

- [ ] **Step 8: Run the JSON isolation guard (must still pass)**

```bash
sh scripts/check-json-isolation.sh
```

Expected: `OK: nlohmann/json.hpp isolation invariant holds.`

- [ ] **Step 9: Commit**

```bash
git add code/profile_manager.h code/profile_manager.cpp \
        tests/profile/test_profile_manager.cpp tests/profile/CMakeLists.txt \
        CMakeLists.txt
git commit -m "feat(profile): JSON schema parsing + unit tests

profile_manager::parseProfileFromString accepts the v1 schema
(id, name, version, base, overrides, default_mission,
requires_engine) and records presence of the abl/campaigns/
graphics blocks for later plans without acting on them. Hidden
behind profile_manager translation unit -- only TU that includes
nlohmann/json.hpp; isolation guard enforces it.

Tests cover stock minimal, full schema, id mismatch, malformed
JSON, missing id, null/missing base.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Profile chain resolution (TDD)

**Files:**
- Modify: `code/profile_manager.h` — add resolution API
- Modify: `code/profile_manager.cpp`
- Modify: `tests/profile/test_profile_manager.cpp`

- [ ] **Step 1: Extend the header**

Append to `code/profile_manager.h` inside the namespace:

```cpp
struct ResolvedProfile {
    std::string activeId;
    std::vector<ProfileInfo> chain;       // active first, base last
    std::vector<std::string> dataRoots;   // active data first, stock last
};

enum class ResolveStatus {
    Ok,
    ProfileNotFound,
    BaseNotFound,
    BaseCycle,
    InvalidProfile,
};

struct ResolveResult {
    ResolveStatus status = ResolveStatus::Ok;
    std::string errorDetail;
    ResolvedProfile resolved;
};

// Resolve a profile and its base chain by reading profile.json files
// from disk under `profilesDir` (typically "profiles"). Walks
// `base` until a null/empty base is reached or a cycle is detected.
// `stockDataDir` is the engine-relative path used as the terminal
// data root (typically "data"). The chain always ends with the stock
// data root regardless of whether a "stock" profile descriptor is
// loaded — stock is special.
ResolveResult resolveActiveProfile(const std::string& activeId,
                                   const std::string& profilesDir,
                                   const std::string& stockDataDir);
```

- [ ] **Step 2: Add the failing tests**

Append to `tests/profile/test_profile_manager.cpp` before `main()`:

```cpp
namespace fs = std::filesystem;

static std::string makeTempDir(const char* tag) {
    auto base = fs::temp_directory_path() / "mc2_profile_test";
    fs::create_directories(base);
    auto dir = base / tag;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir.string();
}

static void writeFile(const std::string& path, const std::string& content) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

static void testResolveStockOnly() {
    const std::string root = makeTempDir("stock_only");
    writeFile(root + "/profiles/stock/profile.json",
              R"({"id":"stock","base":null})");
    fs::create_directories(root + "/data");

    auto r = pm::resolveActiveProfile("stock", root + "/profiles", root + "/data");
    CHECK(r.status == pm::ResolveStatus::Ok);
    CHECK(r.resolved.chain.size() == 1);
    CHECK_EQ_STR(r.resolved.chain[0].id, "stock");
    // dataRoots: stock data only
    CHECK(r.resolved.dataRoots.size() == 1);
    CHECK_EQ_STR(r.resolved.dataRoots[0], root + "/data");
}

static void testResolveTwoLevelChain() {
    const std::string root = makeTempDir("two_level");
    writeFile(root + "/profiles/stock/profile.json",
              R"({"id":"stock","base":null})");
    writeFile(root + "/profiles/magic_co/profile.json",
              R"({"id":"magic_co","base":"stock"})");
    fs::create_directories(root + "/profiles/magic_co/data");
    fs::create_directories(root + "/data");

    auto r = pm::resolveActiveProfile("magic_co",
                                      root + "/profiles",
                                      root + "/data");
    CHECK(r.status == pm::ResolveStatus::Ok);
    CHECK(r.resolved.chain.size() == 2);
    CHECK_EQ_STR(r.resolved.chain[0].id, "magic_co");
    CHECK_EQ_STR(r.resolved.chain[1].id, "stock");
    CHECK(r.resolved.dataRoots.size() == 2);
    CHECK_EQ_STR(r.resolved.dataRoots[0], root + "/profiles/magic_co/data");
    CHECK_EQ_STR(r.resolved.dataRoots[1], root + "/data");
}

static void testResolveProfileNotFound() {
    const std::string root = makeTempDir("not_found");
    fs::create_directories(root + "/profiles");
    auto r = pm::resolveActiveProfile("ghost",
                                      root + "/profiles",
                                      root + "/data");
    CHECK(r.status == pm::ResolveStatus::ProfileNotFound);
    CHECK(r.errorDetail.find("ghost") != std::string::npos);
}

static void testResolveBaseNotFound() {
    const std::string root = makeTempDir("base_missing");
    writeFile(root + "/profiles/orphan/profile.json",
              R"({"id":"orphan","base":"missing_base"})");
    auto r = pm::resolveActiveProfile("orphan",
                                      root + "/profiles",
                                      root + "/data");
    CHECK(r.status == pm::ResolveStatus::BaseNotFound);
    CHECK(r.errorDetail.find("missing_base") != std::string::npos);
}

static void testResolveBaseCycle() {
    const std::string root = makeTempDir("cycle");
    writeFile(root + "/profiles/a/profile.json",
              R"({"id":"a","base":"b"})");
    writeFile(root + "/profiles/b/profile.json",
              R"({"id":"b","base":"a"})");
    auto r = pm::resolveActiveProfile("a",
                                      root + "/profiles",
                                      root + "/data");
    CHECK(r.status == pm::ResolveStatus::BaseCycle);
}

static void testResolveBuiltinStockFallback() {
    // No profiles/stock/profile.json on disk. The resolver must
    // synthesize a stock entry rather than failing.
    const std::string root = makeTempDir("builtin_stock");
    fs::create_directories(root + "/profiles");
    fs::create_directories(root + "/data");
    auto r = pm::resolveActiveProfile("stock",
                                      root + "/profiles",
                                      root + "/data");
    CHECK(r.status == pm::ResolveStatus::Ok);
    CHECK(r.resolved.chain.size() == 1);
    CHECK_EQ_STR(r.resolved.chain[0].id, "stock");
}
```

Add these calls to `main()`:

```cpp
    testResolveStockOnly();
    testResolveTwoLevelChain();
    testResolveProfileNotFound();
    testResolveBaseNotFound();
    testResolveBaseCycle();
    testResolveBuiltinStockFallback();
```

- [ ] **Step 3: Run the tests; expect FAIL**

```bash
cmake --build out --config RelWithDebInfo --target profile_tests -- /m
out/tests/profile/RelWithDebInfo/profile_tests.exe
```

Expected: link errors (`resolveActiveProfile` undefined) OR — once a stub is added — multiple FAIL lines.

- [ ] **Step 4: Implement `resolveActiveProfile`**

Append to `code/profile_manager.cpp`:

```cpp
#include <set>
#include <sys/stat.h>

namespace profile_manager {

namespace {

// C++14-compatible directory existence check. POSIX stat() works on
// MSVC (via sys/stat.h), MSYS2, and Linux.
bool dirExists(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return (st.st_mode & S_IFDIR) != 0;
}

bool fileExists(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return (st.st_mode & S_IFREG) != 0;
}

ProfileInfo makeBuiltinStock() {
    ProfileInfo p;
    p.id = "stock";
    p.name = "Stock Carver V";
    p.author = "FASA / Microsoft";
    p.base = "";
    return p;
}

}  // namespace

ResolveResult resolveActiveProfile(const std::string& activeId,
                                   const std::string& profilesDir,
                                   const std::string& stockDataDir) {
    ResolveResult r;
    r.resolved.activeId = activeId;

    std::set<std::string> visited;
    std::string current = activeId;

    while (!current.empty()) {
        if (visited.count(current)) {
            r.status = ResolveStatus::BaseCycle;
            r.errorDetail = "base cycle detected at \"" + current + "\"";
            return r;
        }
        visited.insert(current);

        ProfileInfo info;

        const std::string dir = profilesDir + "/" + current;
        const std::string jsonPath = dir + "/profile.json";

        if (fileExists(jsonPath)) {
            ParseResult pr = parseProfileFile(dir);
            if (pr.status != ParseStatus::Ok) {
                r.status = ResolveStatus::InvalidProfile;
                r.errorDetail = "profile \"" + current + "\": " + pr.errorDetail;
                return r;
            }
            info = pr.profile;
        } else if (current == "stock") {
            // Built-in fallback: stock works even without a JSON file.
            info = makeBuiltinStock();
        } else {
            const bool isInitial = (visited.size() == 1);
            r.status = isInitial ? ResolveStatus::ProfileNotFound
                                 : ResolveStatus::BaseNotFound;
            r.errorDetail = std::string(isInitial ? "profile" : "base profile")
                          + " not found: \"" + current + "\"";
            return r;
        }

        // Push this entry onto the chain.
        r.resolved.chain.push_back(info);

        // Record its data root (active profile's data/, then each base's data/,
        // and finally the stock data dir).
        if (current == "stock") {
            // Stock terminal — ignore profiles/stock/data, use stockDataDir.
            // Note: we DO NOT check dirExists(stockDataDir) here. Stock
            // installs may run FST-only with no loose data/ dir; the path
            // global default points there anyway, and resolveSubdir will
            // soft-skip subdirs not on disk.
            r.resolved.dataRoots.push_back(stockDataDir);
        } else {
            const std::string profileDataDir = dir + "/data";
            if (dirExists(profileDataDir)) {
                r.resolved.dataRoots.push_back(profileDataDir);
            }
            // If profile has no data/ directory yet, its content is fully
            // inherited from base — that's fine, do not insert an empty root.
        }

        // Advance to base.
        current = info.base;
    }

    // If we exited without ever hitting "stock", append stock data terminal.
    if (r.resolved.dataRoots.empty() ||
        r.resolved.dataRoots.back() != stockDataDir) {
        r.resolved.dataRoots.push_back(stockDataDir);
    }

    r.status = ResolveStatus::Ok;
    return r;
}

}  // namespace profile_manager
```

- [ ] **Step 5: Rebuild + run; expect PASS**

```bash
cmake --build out --config RelWithDebInfo --target profile_tests -- /m
out/tests/profile/RelWithDebInfo/profile_tests.exe
```

Expected: `OK: all profile_manager parsing tests passed` (or whatever the final summary line says — all asserts pass).

- [ ] **Step 6: Commit**

```bash
git add code/profile_manager.h code/profile_manager.cpp \
        tests/profile/test_profile_manager.cpp
git commit -m "feat(profile): base-chain resolution + cycle detection

resolveActiveProfile walks profile.json -> base -> base ... until
it terminates at empty base. Built-in stock fallback synthesises a
ProfileInfo when no profile.json exists, so --profile stock works
on a fresh checkout. Distinguishes 'profile not found' (top of
chain) from 'base profile not found' (mid-chain) so error messages
point at the right culprit. Cycles are detected via visited set.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Path subdir resolver (TDD)

**Files:**
- Modify: `code/profile_manager.h`
- Modify: `code/profile_manager.cpp`
- Modify: `tests/profile/test_profile_manager.cpp`

The resolver picks the topmost data root that contains a given subdir (e.g. `missions/`) and returns it as a path string. Plan 1 binds path globals only when the *topmost* root has the subdir; otherwise it walks the chain to find a defining root, falling back to stock as a last resort.

- [ ] **Step 1: Extend the header**

```cpp
enum class SubdirResolveStatus {
    Ok,
    NotFoundAnywhere,
    PathTooLongForBuffer,
};

struct SubdirResolveResult {
    SubdirResolveStatus status = SubdirResolveStatus::Ok;
    std::string resolvedPath;     // e.g. "profiles/magic_co/data/missions/"
    std::string sourceRoot;       // e.g. "profiles/magic_co/data" (which root won)
    std::string errorDetail;
};

// Search the resolved data roots in order for a subdir. Returns the
// first hit. The trailing path separator is platform-appropriate
// ('/' or '\\') and matches MC2's PATH_SEPARATOR convention.
//
// `bufferSize` is the destination char[] capacity (e.g. 80 for the
// stock content-path globals). If the resolved path including null
// terminator exceeds the buffer, returns PathTooLongForBuffer.
SubdirResolveResult resolveSubdir(const ResolvedProfile& resolved,
                                  const std::string& subdir,
                                  size_t bufferSize);
```

- [ ] **Step 2: Add the failing tests**

Append:

```cpp
static void testResolveSubdirActiveWins() {
    const std::string root = makeTempDir("subdir_active");
    writeFile(root + "/profiles/stock/profile.json",
              R"({"id":"stock","base":null})");
    writeFile(root + "/profiles/magic_co/profile.json",
              R"({"id":"magic_co","base":"stock"})");
    fs::create_directories(root + "/profiles/magic_co/data/missions");
    fs::create_directories(root + "/data/missions");

    auto rr = pm::resolveActiveProfile("magic_co",
                                       root + "/profiles",
                                       root + "/data");
    auto sr = pm::resolveSubdir(rr.resolved, "missions", 256);
    CHECK(sr.status == pm::SubdirResolveStatus::Ok);
    CHECK(sr.resolvedPath.find("magic_co") != std::string::npos);
    CHECK(sr.resolvedPath.find("missions") != std::string::npos);
}

static void testResolveSubdirFallsThroughToStock() {
    const std::string root = makeTempDir("subdir_fallthrough");
    writeFile(root + "/profiles/magic_co/profile.json",
              R"({"id":"magic_co","base":"stock"})");
    fs::create_directories(root + "/profiles/magic_co/data");  // no missions
    fs::create_directories(root + "/data/missions");

    auto rr = pm::resolveActiveProfile("magic_co",
                                       root + "/profiles",
                                       root + "/data");
    auto sr = pm::resolveSubdir(rr.resolved, "missions", 256);
    CHECK(sr.status == pm::SubdirResolveStatus::Ok);
    CHECK(sr.resolvedPath.find("magic_co") == std::string::npos);
    CHECK(sr.resolvedPath.find(root) != std::string::npos);
}

static void testResolveSubdirNotFound() {
    const std::string root = makeTempDir("subdir_missing");
    writeFile(root + "/profiles/magic_co/profile.json",
              R"({"id":"magic_co","base":"stock"})");
    fs::create_directories(root + "/data");  // no missions anywhere

    auto rr = pm::resolveActiveProfile("magic_co",
                                       root + "/profiles",
                                       root + "/data");
    auto sr = pm::resolveSubdir(rr.resolved, "missions", 256);
    CHECK(sr.status == pm::SubdirResolveStatus::NotFoundAnywhere);
}

static void testResolveSubdirBufferTooSmall() {
    const std::string root = makeTempDir("subdir_overflow");
    writeFile(root + "/profiles/magic_co/profile.json",
              R"({"id":"magic_co","base":"stock"})");
    fs::create_directories(root + "/profiles/magic_co/data/missions");
    fs::create_directories(root + "/data/missions");

    auto rr = pm::resolveActiveProfile("magic_co",
                                       root + "/profiles",
                                       root + "/data");
    auto sr = pm::resolveSubdir(rr.resolved, "missions", 8);
    CHECK(sr.status == pm::SubdirResolveStatus::PathTooLongForBuffer);
}
```

Wire them into `main()`. Also wire `testBindPathGlobalsFstOnlyStockSoftSkips` (added later in Task 6).

- [ ] **Step 3: Run; expect FAIL**

```bash
cmake --build out --config RelWithDebInfo --target profile_tests -- /m
out/tests/profile/RelWithDebInfo/profile_tests.exe
```

Expected: link error or FAILs.

- [ ] **Step 4: Implement `resolveSubdir`**

Append to `code/profile_manager.cpp`:

```cpp
SubdirResolveResult resolveSubdir(const ResolvedProfile& resolved,
                                  const std::string& subdir,
                                  size_t bufferSize) {
    SubdirResolveResult r;

#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif

    for (const std::string& root : resolved.dataRoots) {
        const std::string candidate = root + "/" + subdir;
        if (!dirExists(candidate)) continue;

        // Build with platform separator + trailing separator.
        std::string out = root + sep + subdir + sep;
        // Normalize forward slashes to platform separator.
        for (char& c : out) if (c == '/') c = sep;

        // bufferSize must hold the string + null terminator.
        if (out.size() + 1 > bufferSize) {
            r.status = SubdirResolveStatus::PathTooLongForBuffer;
            r.errorDetail = "resolved path \"" + out + "\" exceeds buffer ("
                          + std::to_string(out.size() + 1) + " > "
                          + std::to_string(bufferSize) + ")";
            return r;
        }

        r.status = SubdirResolveStatus::Ok;
        r.resolvedPath = out;
        r.sourceRoot = root;
        return r;
    }

    r.status = SubdirResolveStatus::NotFoundAnywhere;
    r.errorDetail = "subdir \"" + subdir + "\" not found in any data root";
    return r;
}
```

- [ ] **Step 5: Rebuild + run; expect PASS**

```bash
cmake --build out --config RelWithDebInfo --target profile_tests -- /m
out/tests/profile/RelWithDebInfo/profile_tests.exe
```

Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add code/profile_manager.h code/profile_manager.cpp \
        tests/profile/test_profile_manager.cpp
git commit -m "feat(profile): subdir resolver with buffer-overflow guard

resolveSubdir walks the resolved data-roots list and returns the
first root containing the requested subdir. Buffer-size argument
matches MC2's char[80] / char[256] path globals: overflow is
returned as a hard error, never silently truncated. Tests cover
active-wins, fall-through to stock, missing-everywhere, overflow.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Path-globals binding (TDD against fakes, then real wiring)

**Files:**
- Modify: `code/profile_manager.h`
- Modify: `code/profile_manager.cpp`
- Modify: `tests/profile/test_profile_manager.cpp`

The binder writes resolved paths into a caller-provided `PathBindingTargets` struct (so the unit test can pass fakes; production code passes pointers to the real `mclib/paths.cpp` externs).

- [ ] **Step 1: Extend the header**

```cpp
struct PathBindingTargets {
    // Plan 1 binds these 11 content-path globals. terrainPath, tilePath,
    // spritePath, fontPath, profilePath, savePath, saveTempPath, and CD
    // mirrors are deliberately not bound here (see plan file structure).
    char* missionPath;     size_t missionPathSize;
    char* objectPath;      size_t objectPathSize;
    char* artPath;         size_t artPathSize;
    char* tglPath;         size_t tglPathSize;
    char* texturePath;     size_t texturePathSize;
    char* campaignPath;    size_t campaignPathSize;
    char* moviePath;       size_t moviePathSize;
    char* effectsPath;     size_t effectsPathSize;
    char* soundPath;       size_t soundPathSize;
    char* interfacePath;   size_t interfacePathSize;
    char* warriorPath;     size_t warriorPathSize;
};

enum class BindStatus {
    Ok,
    PathOverflow,
    // Note: a "subdir not found anywhere" condition is a soft warning,
    // not a status, in Plan 1 -- the binder leaves the buffer at its
    // compile-time default and logs. This keeps corebrain-only profiles
    // and FST-only stock installs bootable. Plan 2/3/4 may revisit
    // if a profile declares a subdir as required.
};

struct BindResult {
    BindStatus status = BindStatus::Ok;
    std::string errorDetail;
    // Per-global resolved paths, for diagnostics. Empty string means
    // the global was not bound (e.g. profile didn't override and stock
    // was already the source — Plan 1 still binds in that case so the
    // log is uniform).
    std::map<std::string, std::string> bound;
};

BindResult bindPathGlobals(const ResolvedProfile& resolved,
                           PathBindingTargets& targets);

// Diagnostic flag: when true, bindPathGlobals emits [PROFILE] log lines.
// Set from the env var MC2_PROFILE_TRACE at startup.
extern bool g_profileTraceEnabled;
```

- [ ] **Step 2: Failing test**

Append to `tests/profile/test_profile_manager.cpp`:

```cpp
static void testBindPathGlobalsHappyPath() {
    const std::string root = makeTempDir("bind_happy");
    writeFile(root + "/profiles/magic_co/profile.json",
              R"({"id":"magic_co","base":"stock"})");
    fs::create_directories(root + "/profiles/magic_co/data/missions");
    fs::create_directories(root + "/profiles/magic_co/data/art");
    // No /objects in profile -> falls through to stock.
    fs::create_directories(root + "/data/missions");
    fs::create_directories(root + "/data/objects");
    fs::create_directories(root + "/data/art");
    fs::create_directories(root + "/data/tgl");
    fs::create_directories(root + "/data/textures");
    fs::create_directories(root + "/data/campaign");
    fs::create_directories(root + "/data/movies");
    fs::create_directories(root + "/data/effects");
    fs::create_directories(root + "/data/sound");
    fs::create_directories(root + "/data/interface");
    fs::create_directories(root + "/data/missions/profiles");

    char missionBuf[80] = {0};
    char objectBuf[80] = {0};
    char artBuf[80] = {0};
    char tglBuf[80] = {0}, texBuf[80] = {0}, campBuf[80] = {0},
         movBuf[80] = {0}, effBuf[80] = {0}, sndBuf[80] = {0},
         ifaceBuf[80] = {0}, warBuf[80] = {0};

    pm::PathBindingTargets t{
        missionBuf, sizeof(missionBuf),
        objectBuf,  sizeof(objectBuf),
        artBuf,     sizeof(artBuf),
        tglBuf,     sizeof(tglBuf),
        texBuf,     sizeof(texBuf),
        campBuf,    sizeof(campBuf),
        movBuf,     sizeof(movBuf),
        effBuf,     sizeof(effBuf),
        sndBuf,     sizeof(sndBuf),
        ifaceBuf,   sizeof(ifaceBuf),
        warBuf,     sizeof(warBuf),
    };

    auto rr = pm::resolveActiveProfile("magic_co",
                                       root + "/profiles",
                                       root + "/data");
    auto br = pm::bindPathGlobals(rr.resolved, t);

    CHECK(br.status == pm::BindStatus::Ok);
    CHECK(std::string(missionBuf).find("magic_co") != std::string::npos);
    CHECK(std::string(objectBuf).find("magic_co") == std::string::npos);
    CHECK(std::string(artBuf).find("magic_co") != std::string::npos);
}

static void testBindPathGlobalsFstOnlyStockSoftSkips() {
    // Stock install where data/ exists but data/missions/ does NOT
    // (FST-only mode). The binder must leave the missionPath buffer
    // at its compile-time default rather than aborting -- the engine
    // FST fallback will serve missions just fine.
    const std::string root = makeTempDir("bind_fst_only");
    fs::create_directories(root + "/data");  // no subdirs at all

    char missionBuf[80] = "data\\missions\\";  // simulate compile-time default
    char normalBuf[80] = {0};

    pm::PathBindingTargets t{
        missionBuf, sizeof(missionBuf),
        normalBuf,  sizeof(normalBuf), normalBuf, sizeof(normalBuf),
        normalBuf,  sizeof(normalBuf), normalBuf, sizeof(normalBuf),
        normalBuf,  sizeof(normalBuf), normalBuf, sizeof(normalBuf),
        normalBuf,  sizeof(normalBuf), normalBuf, sizeof(normalBuf),
        normalBuf,  sizeof(normalBuf), normalBuf, sizeof(normalBuf),
    };

    auto rr = pm::resolveActiveProfile("stock",
                                       root + "/profiles",
                                       root + "/data");
    auto br = pm::bindPathGlobals(rr.resolved, t);
    CHECK(br.status == pm::BindStatus::Ok);
    // Buffer should still hold its initial value -- binder did not
    // overwrite when no candidate root contained data/missions/.
    CHECK_EQ_STR(std::string(missionBuf), "data\\missions\\");
}

static void testBindPathGlobalsOverflowAborts() {
    // Construct an absurdly long temp path (depends on temp_directory,
    // but we can simulate by passing a tiny buffer).
    const std::string root = makeTempDir("bind_overflow");
    writeFile(root + "/profiles/x/profile.json",
              R"({"id":"x","base":"stock"})");
    fs::create_directories(root + "/profiles/x/data/missions");
    fs::create_directories(root + "/data/missions");
    fs::create_directories(root + "/data/objects");

    char tinyBuf[16] = {0};
    char normalBuf[80] = {0};

    pm::PathBindingTargets t{
        tinyBuf,    sizeof(tinyBuf),    // missionPath: too small
        normalBuf,  sizeof(normalBuf),
        normalBuf,  sizeof(normalBuf),
        normalBuf,  sizeof(normalBuf), normalBuf, sizeof(normalBuf),
        normalBuf,  sizeof(normalBuf), normalBuf, sizeof(normalBuf),
        normalBuf,  sizeof(normalBuf), normalBuf, sizeof(normalBuf),
        normalBuf,  sizeof(normalBuf), normalBuf, sizeof(normalBuf),
    };

    auto rr = pm::resolveActiveProfile("x",
                                       root + "/profiles",
                                       root + "/data");
    auto br = pm::bindPathGlobals(rr.resolved, t);
    CHECK(br.status == pm::BindStatus::PathOverflow);
}
```

Wire into `main()`. Run, expect FAIL/link errors.

- [ ] **Step 3: Implement `bindPathGlobals`**

Append to `code/profile_manager.cpp`:

```cpp
#include <cstdio>
#include <cstring>

namespace profile_manager {

bool g_profileTraceEnabled = false;

namespace {

struct SubdirBinding {
    const char* subdirName;
    const char* globalName;          // for diagnostics
    char**      bufferPtr;           // pointer to PathBindingTargets buffer
    size_t*     bufferSizePtr;
};

bool bindOne(const ResolvedProfile& resolved,
             const SubdirBinding& b,
             BindResult& outResult) {
    auto sr = resolveSubdir(resolved, b.subdirName, *b.bufferSizePtr);

    if (sr.status == SubdirResolveStatus::PathTooLongForBuffer) {
        outResult.status = BindStatus::PathOverflow;
        outResult.errorDetail = std::string(b.globalName) + ": " + sr.errorDetail;
        return false;
    }
    if (sr.status == SubdirResolveStatus::NotFoundAnywhere) {
        // Plan 1 policy: missing required subdir is a soft warning,
        // not an abort, because some path globals (e.g. campaignPath)
        // may be unused by certain profiles. Leave the buffer at its
        // compile-time default.
        if (g_profileTraceEnabled) {
            std::printf("[PROFILE] subdir not found, leaving default: %s (%s)\n",
                        b.globalName, b.subdirName);
            std::fflush(stdout);
        }
        return true;
    }

    // Success: copy into buffer.
    std::strncpy(*b.bufferPtr, sr.resolvedPath.c_str(), *b.bufferSizePtr);
    (*b.bufferPtr)[*b.bufferSizePtr - 1] = '\0';
    outResult.bound[b.globalName] = sr.resolvedPath;

    if (g_profileTraceEnabled) {
        std::printf("[PROFILE] bind %s=%s\n",
                    b.globalName, sr.resolvedPath.c_str());
        std::fflush(stdout);
    }
    return true;
}

}  // namespace

BindResult bindPathGlobals(const ResolvedProfile& resolved,
                           PathBindingTargets& t) {
    BindResult r;

    SubdirBinding bindings[] = {
        {"missions",  "missionPath",   &t.missionPath,   &t.missionPathSize},
        {"objects",   "objectPath",    &t.objectPath,    &t.objectPathSize},
        {"art",       "artPath",       &t.artPath,       &t.artPathSize},
        {"tgl",       "tglPath",       &t.tglPath,       &t.tglPathSize},
        {"textures",  "texturePath",   &t.texturePath,   &t.texturePathSize},
        {"campaign",  "campaignPath",  &t.campaignPath,  &t.campaignPathSize},
        {"movies",    "moviePath",     &t.moviePath,     &t.moviePathSize},
        {"effects",   "effectsPath",   &t.effectsPath,   &t.effectsPathSize},
        {"sound",     "soundPath",     &t.soundPath,     &t.soundPathSize},
        {"interface", "interfacePath", &t.interfacePath, &t.interfacePathSize},
        // warriorPath defaults to "data/missions/profiles/" in stock.
        // For Plan 1 we keep that default behavior: profiles can opt
        // in by providing data/missions/profiles/. Plan 3 will revisit.
        {"missions/profiles", "warriorPath", &t.warriorPath, &t.warriorPathSize},
    };

    for (const auto& b : bindings) {
        if (!bindOne(resolved, b, r)) return r;
    }

    r.status = BindStatus::Ok;
    return r;
}

}  // namespace profile_manager
```

- [ ] **Step 4: Rebuild + run; expect PASS**

```bash
cmake --build out --config RelWithDebInfo --target profile_tests -- /m
out/tests/profile/RelWithDebInfo/profile_tests.exe
```

Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add code/profile_manager.h code/profile_manager.cpp \
        tests/profile/test_profile_manager.cpp
git commit -m "feat(profile): bind resolved subdirs into path-global targets

bindPathGlobals walks the Plan 1 11-content-subdir set and writes
the first matching profile-or-base data root into each caller-
provided char[] buffer. PathOverflow is fatal; missing subdirs
leave the buffer at its compile-time default so corebrain-only
profiles and FST-only stock installs still boot. Diagnostics
gated on g_profileTraceEnabled.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Wire `--profile` CLI + engine init

**Files:**
- Modify: `code/mechcmd2.cpp` (extend `ParseCommandLine` and call binding)
- Modify: `code/profile_manager.cpp` (add a thin `bindActive` entry point)
- Modify: `code/profile_manager.h`

The entry point reads a profile id from CLI/config, calls `resolveActiveProfile` with `"profiles"` and `"data"`, and calls `bindPathGlobals` against the real `mclib/paths.cpp` externs.

- [ ] **Step 1: Add the public boot entry to the header**

Append to `code/profile_manager.h`:

```cpp
// One-shot init called from mechcmd2 after CLI parsing. Returns 0 on
// success, nonzero on fatal binding failure. profileId may be empty
// or "stock" -- both behave identically (no profile dir required).
//
// Side effects:
//   - rewrites the path globals in mclib/paths.cpp
//   - emits [PROFILE] log lines (always for the active=/chain= summary
//     line; gated on MC2_PROFILE_TRACE for per-bind detail)
//
// Reads the env var MC2_PROFILE_TRACE.
int bindActive(const std::string& profileId);
```

- [ ] **Step 2: Implement `bindActive`**

Append to `code/profile_manager.cpp`:

```cpp
// Path globals are declared in mclib/paths.h with C++ linkage. Include
// the header rather than redeclaring -- the linkage must match the
// definitions in mclib/paths.cpp, and an extern "C" wrapper would
// cause an unresolved-external mismatch.
#include "paths.h"

namespace profile_manager {

int bindActive(const std::string& profileIdIn) {
    if (std::getenv("MC2_PROFILE_TRACE") != nullptr) {
        g_profileTraceEnabled = true;
    }

    std::string profileId = profileIdIn.empty() ? "stock" : profileIdIn;

    std::printf("[PROFILE] requested=%s source=%s\n",
                profileId.c_str(),
                profileIdIn.empty() ? "default" : "cli");
    std::fflush(stdout);

    auto rr = resolveActiveProfile(profileId, "profiles", "data");
    if (rr.status != ResolveStatus::Ok) {
        std::printf("[PROFILE] error: %s\n", rr.errorDetail.c_str());
        std::fflush(stdout);
        return 1;
    }

    std::printf("[PROFILE] active=%s\n", rr.resolved.activeId.c_str());
    // chain[i] = profile id (the inheritance chain).
    for (size_t i = 0; i < rr.resolved.chain.size(); ++i) {
        std::printf("[PROFILE] chain[%zu]=%s\n",
                    i, rr.resolved.chain[i].id.c_str());
    }
    // root[i] = data directory bound for that chain entry. A profile
    // with no data/ on disk has no entry here; stock terminal is the
    // last root regardless. Splitting these two views avoids the
    // misleading "chain[0] root=data" pattern when the active profile
    // has no data of its own.
    for (size_t i = 0; i < rr.resolved.dataRoots.size(); ++i) {
        std::printf("[PROFILE] root[%zu]=%s\n",
                    i, rr.resolved.dataRoots[i].c_str());
    }
    std::fflush(stdout);

    // paths.h declares these as char foo[] (unsized), so sizeof() does
    // not work here. The actual definitions in mclib/paths.cpp use
    // [80] for content paths. The constant is asserted by the
    // path-overflow guard inside resolveSubdir; if mclib/paths.cpp
    // ever changes these widths, update this constant and the
    // PathBindingTargets test fixtures.
    static const size_t kPathGlobalSize = 80;

    PathBindingTargets t{
        missionPath,   kPathGlobalSize,
        objectPath,    kPathGlobalSize,
        artPath,       kPathGlobalSize,
        tglPath,       kPathGlobalSize,
        texturePath,   kPathGlobalSize,
        campaignPath,  kPathGlobalSize,
        moviePath,     kPathGlobalSize,
        effectsPath,   kPathGlobalSize,
        soundPath,     kPathGlobalSize,
        interfacePath, kPathGlobalSize,
        warriorPath,   kPathGlobalSize,
    };
    auto br = bindPathGlobals(rr.resolved, t);
    if (br.status != BindStatus::Ok) {
        std::printf("[PROFILE] error: bind failed: %s\n",
                    br.errorDetail.c_str());
        std::fflush(stdout);
        return 1;
    }

    return 0;
}

}  // namespace profile_manager
```

- [ ] **Step 3: Wire into `mechcmd2.cpp`**

Add include at the top of [code/mechcmd2.cpp](code/mechcmd2.cpp) (alongside existing includes):

```cpp
#include "profile_manager.h"
```

Add a static for capturing the parsed id near the top of the file (next to other CLI state — e.g. near `gNoDialogs`):

```cpp
static std::string g_pendingProfileId;
```

Inside `ParseCommandLine` at [code/mechcmd2.cpp:2547](code/mechcmd2.cpp:2547), insert a new branch alongside the existing flag handlers (after the `-debugwins` branch is fine):

```cpp
        else if (S_stricmp(argv[i], "--profile") == 0 ||
                 S_stricmp(argv[i], "-profile") == 0) {
            i++;
            if (i < n_args) {
                g_pendingProfileId = argv[i];
            }
        }
```

Immediately after the `ParseCommandLine(CommandLine);` call at [code/mechcmd2.cpp:2643](code/mechcmd2.cpp:2643), add:

```cpp
    if (profile_manager::bindActive(g_pendingProfileId) != 0) {
        // Fatal: profile resolution failed. Refuse to boot.
        std::fprintf(stderr,
                     "[PROFILE] fatal: profile resolution failed; aborting.\n");
        std::fflush(stderr);
        std::exit(2);
    }
```

- [ ] **Step 4: Add `profile_manager.cpp` to the engine build**

In [CMakeLists.txt](CMakeLists.txt), find the source list that already contains `code/mechcmd2.cpp` (around line 100-180) and add:

```
    "code/profile_manager.cpp"
```

- [ ] **Step 5: Build**

```bash
cmake --build out --config RelWithDebInfo --target mc2 -- /m
```

Expected: clean build.

- [ ] **Step 6: Manual smoke — boot with `--profile stock`**

```bash
cd /a/Games/mc2-opengl/mc2-win64-v0.2
cp -f /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/out/RelWithDebInfo/mc2.exe ./mc2.exe
MC2_PROFILE_TRACE=1 ./mc2.exe --profile stock 2>&1 | head -30
```

Expected (early in stdout):
```
[PROFILE] requested=stock source=cli
[PROFILE] active=stock
[PROFILE] chain[0]=stock
[PROFILE] root[0]=data
[PROFILE] bind missionPath=data\missions\
...
```

Close the game window after main menu reaches.

- [ ] **Step 7: Manual smoke — boot with no flag**

```bash
./mc2.exe 2>&1 | head -10
```

Expected:
```
[PROFILE] requested=stock source=default
[PROFILE] active=stock
...
```

- [ ] **Step 8: Manual smoke — `--profile does_not_exist` aborts**

```bash
./mc2.exe --profile does_not_exist 2>&1 | head -10
echo "exit=$?"
```

Expected:
```
[PROFILE] requested=does_not_exist source=cli
[PROFILE] error: profile not found: "does_not_exist"
[PROFILE] fatal: profile resolution failed; aborting.
exit=2
```

- [ ] **Step 9: Run JSON-isolation guard + `profile_tests`**

```bash
sh scripts/check-json-isolation.sh
out/tests/profile/RelWithDebInfo/profile_tests.exe
```

Expected: both pass.

- [ ] **Step 10: Commit**

```bash
git add code/profile_manager.h code/profile_manager.cpp \
        code/mechcmd2.cpp CMakeLists.txt
git commit -m "feat(profile): wire --profile CLI + path-global binding

mc2.exe accepts --profile <id> (and -profile for symmetry with
existing single-dash flags). profile_manager::bindActive runs
immediately after ParseCommandLine, before any subsystem reads
path globals. Failure aborts boot with exit code 2 -- silent
profile failures would have been a Plan 1 footgun.

MC2_PROFILE_TRACE=1 enables per-bind log detail. The
[PROFILE] active=/chain= summary lines are always-on so a fresh
operator sees the resolved chain on every boot.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Ship the two on-disk profile descriptors

**Files:**
- Create: `profiles/stock/profile.json`
- Create: `profiles/magic_corebrain_only/profile.json`
- Create: `profiles/.gitignore`

- [ ] **Step 1: Create `profiles/stock/profile.json`**

```json
{
  "id": "stock",
  "name": "Stock Carver V",
  "version": "1.0",
  "author": "FASA / Microsoft",
  "base": null,
  "description": "Stock MechCommander 2 content. Resolves directly to data/."
}
```

- [ ] **Step 2: Create `profiles/magic_corebrain_only/profile.json`**

```json
{
  "id": "magic_corebrain_only",
  "name": "Magic Corebrain Only",
  "version": "1.45",
  "author": "Magic",
  "base": "stock",
  "description": "Skeleton profile. Inherits all content from stock. ABL policy comes in Plan 2.",
  "overrides": {
    "missions": false
  }
}
```

(Note: `overrides` is informational in Plan 1; setting `"missions": false` here is honest — this profile doesn't yet ship overriding mission content.)

- [ ] **Step 3: Add `profiles/.gitignore`**

```
# Per-(profile, campaign) save dirs land here in Plan 3. Keep them
# out of source control while still committing profile descriptors.
*/saves/
```

- [ ] **Step 4: Manual smoke — boot the corebrain_only profile**

```bash
cd /a/Games/mc2-opengl/mc2-win64-v0.2
mkdir -p profiles/stock profiles/magic_corebrain_only
cp /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/profiles/stock/profile.json profiles/stock/
cp /a/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/profiles/magic_corebrain_only/profile.json profiles/magic_corebrain_only/
MC2_PROFILE_TRACE=1 ./mc2.exe --profile magic_corebrain_only 2>&1 | head -30
```

Expected (truncated):
```
[PROFILE] requested=magic_corebrain_only source=cli
[PROFILE] active=magic_corebrain_only
[PROFILE] chain[0]=magic_corebrain_only
[PROFILE] chain[1]=stock
[PROFILE] root[0]=data
[PROFILE] bind missionPath=data\missions\
...
```

(The chain has two entries — the active profile and its base — but only one root, because `profiles/magic_corebrain_only/data/` doesn't exist on disk and content falls through to stock immediately. The `chain[i]` vs `root[i]` separation makes that visible at a glance.)

- [ ] **Step 5: Commit**

```bash
git add profiles/stock/profile.json profiles/magic_corebrain_only/profile.json \
        profiles/.gitignore
git commit -m "feat(profile): ship stock + magic_corebrain_only descriptors

Stock profile descriptor exists for symmetry only -- the engine has
a built-in stock fallback that works without it. magic_corebrain_only
is a base:'stock' skeleton: Plan 1 has nothing for it to override
yet, but it proves the chain resolves end-to-end.

profiles/*/saves/ is gitignored ahead of Plan 3.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: Smoke-harness profile cases

**Files:**
- Modify: `tests/smoke/run_smoke.py`

- [ ] **Step 1: Add a profile-boot case helper**

Identify in `tests/smoke/run_smoke.py` where existing tier1 cases are constructed and add a parallel "profile boot" sub-mode that runs each of:

| Case | Args | Expected exit | Expected stdout substring |
|---|---|---|---|
| profile_default | (no `--profile`) | engine boots through main menu, clean shutdown | `[PROFILE] requested=stock source=default` |
| profile_stock | `--profile stock` | clean | `[PROFILE] requested=stock source=cli` |
| profile_corebrain_only | `--profile magic_corebrain_only` | clean | `[PROFILE] active=magic_corebrain_only` |
| profile_missing | `--profile does_not_exist` | exit=2, no window | `[PROFILE] fatal: profile resolution failed` |

Implementation pattern (sketch — match the existing harness structure):

```python
def run_profile_boot_case(name, args, expect_substr, expect_exit=0,
                          run_full_menu_canary=False):
    cmd = [str(MC2_EXE), *args, "--smoke-active",
           # menu-canary args here if run_full_menu_canary
          ]
    env = {**os.environ, "MC2_PROFILE_TRACE": "1"}
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          env=env, timeout=45)
    log = proc.stdout + proc.stderr
    if proc.returncode != expect_exit:
        return Fail(f"{name}: exit={proc.returncode} expected={expect_exit}")
    if expect_substr not in log:
        return Fail(f"{name}: missing expected substring {expect_substr!r}")
    return Pass(name)
```

- [ ] **Step 2: Wire the four cases into the runner**

Add a `--profile-cases` flag to `run_smoke.py` that triggers the four cases above. Keep them disabled by default so tier1 baselines don't shift.

- [ ] **Step 3: Run the new cases manually**

```bash
py -3 scripts/run_smoke.py --profile-cases --kill-existing
```

Expected: all 4 pass.

- [ ] **Step 4: Run baseline tier1 + menu canary**

```bash
py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected exit: 0 (no regression from any of Plans 1's wiring).

- [ ] **Step 5: Commit**

```bash
git add tests/smoke/run_smoke.py
git commit -m "test(smoke): add --profile-cases for Plan 1 boot cases

Four cases: default no-arg boot, --profile stock, --profile
magic_corebrain_only, and --profile does_not_exist. The first
three must reach a clean exit with the expected [PROFILE] log
line; the fourth must exit 2 with the fatal-resolution diagnostic.
Gated behind --profile-cases so tier1 baselines stay stable.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 10: Plan 1 exit-criteria gate

- [ ] **Step 1: Run all gates in sequence**

```bash
sh scripts/check-json-isolation.sh && \
out/tests/profile/RelWithDebInfo/profile_tests.exe && \
py -3 scripts/run_smoke.py --profile-cases --kill-existing && \
py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected exit: 0 from each. If any fails, fix before declaring Plan 1 complete.

- [ ] **Step 2: Verify the plan-level invariants**

Manually confirm:

- [ ] `--profile stock` and bare `mc2.exe` produce identical post-boot behavior. The smoke baseline numbers from `--profile stock` boot should match the no-arg baseline within tier1 noise (~5%).
- [ ] No `[PROFILE]` log lines appear in `MC2_PROFILE_TRACE`-disabled runs except the `requested=` / `active=` / `chain[i]=` summary trio.
- [ ] No code outside `code/profile_manager.cpp` and `tests/profile/test_profile_manager.cpp` includes `nlohmann/json.hpp`.
- [ ] No ABL behavior changed (sanity-check by booting `mc2_01` and confirming enemies still engage on contact — the bug fixed in `5b5b248` did not regress).
- [ ] No save-game behavior changed (saves still land in `data/savegame/` exactly as before — Plan 3 will move them).
- [ ] No renderer changes.

- [ ] **Step 3: Tag Plan 1 complete in the spec**

Append to `docs/plans/2026-04-25-mod-profile-launcher-scope-additions.md` at the end of the v1 exit-criteria section:

```markdown
**Plan 1 (foundation) complete:** <commit-sha>. Engine boots through
profile_manager; --profile stock is identical to no-arg boot;
profile chain + path-globals binding tested; smoke gates green.
```

- [ ] **Step 4: Commit the marker**

```bash
git add docs/plans/2026-04-25-mod-profile-launcher-scope-additions.md
git commit -m "docs(profile): mark Plan 1 (foundation) complete

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage:**

| Spec requirement | Plan task |
|---|---|
| `--profile <id>` CLI + default `stock` | Task 7 |
| Profile dir layout (`profiles/<id>/profile.json`, `data/`) | Tasks 3, 8 |
| Linear `base` chain | Task 4 |
| Cycle/missing-base detection | Task 4 |
| `char[80]` overflow detection | Tasks 5, 6 |
| Path-globals rewrite at bind | Task 6, 7 |
| `[PROFILE]` diagnostic log | Tasks 6, 7 |
| `MC2_PROFILE_TRACE` env gate | Task 7 |
| `nlohmann/json` isolation invariant | Task 1 (script) + Tasks 3-7 (enforced) |
| Stock built-in fallback | Task 4 |
| Profile-JSON parser tolerates unknown fields (forward compat for ABL/campaigns/graphics) | Task 3 |
| Profile-JSON records presence of abl/campaigns/graphics blocks for Plans 2-4 | Task 3 |
| Smoke-harness profile cases | Task 9 |
| Renamed smoke `--profile` → `--smoke-profile` | Task 2 |
| THIRD_PARTY_NOTICES.md for nlohmann/json MIT | Task 1 |

**Out-of-scope checks (must remain unchanged in Plan 1):**

- ABL policy / `native_extern_policy` / `abx_libraries` — parsed only, no behavior. Verified in Task 10 step 2.
- Campaign slots — parsed only. Verified in Task 10 step 2.
- Graphics block — parsed only. Verified in Task 10 step 2.
- Saves — `savePath` not touched. Verified in Task 10 step 2.
- Renderer — not touched. Verified by tier1 smoke.
- FST packer — not touched.

**Placeholder scan:** no TBD/TODO/"implement later" remaining. Every code step has full code. Every command step has the exact command + expected output.

**Type consistency:** `ProfileInfo`, `ResolvedProfile`, `ResolveResult`, `BindResult`, `PathBindingTargets`, and the `ParseStatus`/`ResolveStatus`/`BindStatus`/`SubdirResolveStatus` enums are defined once in Task 3/4/5/6 and referenced consistently afterward. The path-global extern names match `mclib/paths.cpp` exactly.
