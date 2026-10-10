// tools/ibl_registry_contract_harness/ibl_registry_contract_harness.cpp
// SUBSYSTEM-HARNESS-ARC / IBL-REGISTRY-CONTRACT-HARNESS-1
//
// Registry-integrity harness for the IBL lighting tables. Links ONLY the real
// header-only constexpr registries (RenderCore/IblHdriRegistry.h +
// IblShRegistry.h) — no .cpp, no GL, no game, no 3rdparty. It tests the actual
// production tables (no duplication), guarding the dangerous silent class:
//
//   kSkyNumberHdriMap names a set "qwantani_night", a typo says "qwantani_nite",
//   lookup falls back to "default" with no error, and a 30s smoke still passes.
//
// Asset existence is handled separately from registry integrity (the HDRIs are
// too large for normal Git tracking and may be deployed out-of-band):
//   * default suite: hdri_assets_inventory — INFORMATIONAL, reports missing
//     assets to stderr but PASSES if registry names are valid.
//   * strict:        hdri_assets_exist_strict — runnable via --test, and also
//     run in the default suite when MC2_IBL_ASSET_STRICT=1. FAILS on any absent
//     referenced asset. Lets a clean checkout stay green while still catching
//     drift on demand.
//
// Repo root: compile-time MC2_REPO_ROOT (set by CMake), env override
// MC2_CONTRACT_REPO_ROOT. See docs/testing/lighting-staticprop-harness-recon-1.md.

#include "contract_harness.h"

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "RenderCore/IblHdriRegistry.h"
#include "RenderCore/IblShRegistry.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

namespace fs = std::filesystem;
using namespace contract_harness;
namespace rc = RenderCore;

#ifndef MC2_REPO_ROOT
#define MC2_REPO_ROOT "."
#endif

static fs::path repoRoot() {
    if (const std::string e = mc2::gate::str<"MC2_CONTRACT_REPO_ROOT">(); !e.empty())   // "" when unset or empty
        return fs::path(e);
    return fs::path(MC2_REPO_ROOT);
}

static bool assetStrictEnv() {
    return mc2::gate::get<"MC2_IBL_ASSET_STRICT">();
}

// An HDRI is "present" if the source .exr OR its cooked .ktx2 sidecar exists.
static bool hdriAssetPresent(const fs::path& root, const std::string& exrPath, std::string& foundAs) {
    std::error_code ec;
    if (fs::is_regular_file(root / exrPath, ec)) { foundAs = exrPath; return true; }
    std::string ktx = exrPath;
    const std::string ext = ".exr";
    if (ktx.size() >= ext.size() && ktx.compare(ktx.size() - ext.size(), ext.size(), ext) == 0)
        ktx.replace(ktx.size() - ext.size(), ext.size(), ".ktx2");
    else
        ktx += ".ktx2";
    if (fs::is_regular_file(root / ktx, ec)) { foundAs = ktx; return true; }
    return false;
}

// External-pack manifest: declares HDRIs too large for Git (installed locally).
// Format: "<repo-relative-path> | size | sha | source"; '#' comments. Only the
// first pipe field (the path) is contract-significant. See
// docs/assets/ibl_hdri_external_pack.txt.
static const char* kManifestRel = "docs/testing/ibl_hdri_external_pack.txt";

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Returns the set of declared external paths; sets ok=false if the manifest is
// missing or unreadable (an undeclared-but-required contract still needs it).
static std::set<std::string> loadExternalManifest(const fs::path& root, bool& ok) {
    std::set<std::string> decl;
    std::ifstream in(root / kManifestRel);
    ok = in.good();
    if (!ok) return decl;
    std::string line;
    while (std::getline(in, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        size_t bar = s.find('|');
        std::string path = trim(bar == std::string::npos ? s : s.substr(0, bar));
        for (char& c : path) if (c == '\\') c = '/';
        if (!path.empty()) decl.insert(path);
    }
    return decl;
}

// ---- registry-integrity tests (pure, no filesystem) ------------------------

static bool test_index0_is_default(TestCtx& t) {
    CH_CHECK(t, rc::kIblHdriSetCount > 0);
    CH_CHECK(t, rc::kIblHdriSets[0].name != nullptr &&
                std::string(rc::kIblHdriSets[0].name) == "default");
    CH_CHECK(t, rc::kIblShSetCount > 0);
    CH_CHECK(t, rc::kIblShSets[0].name != nullptr &&
                std::string(rc::kIblShSets[0].name) == "default");
    return t.failures == 0;
}

// THE typo guard: every kSkyNumberHdriMap entry's set name must resolve.
static bool test_every_skymap_name_resolves(TestCtx& t) {
    for (size_t i = 0; i < rc::kSkyNumberHdriMapCount; ++i) {
        const auto& e = rc::kSkyNumberHdriMap[i];
        if (rc::findHdriSetByName(e.hdriSetName) == nullptr)
            t.fail(std::string("sky ") + std::to_string(e.skyNumber) +
                   " names unresolved HDRI set '" +
                   (e.hdriSetName ? e.hdriSetName : "(null)") + "'");
    }
    return t.failures == 0;
}

// Every sky number 1..21 has an explicit map entry (catches a dropped sky;
// an intentional default would be an explicit entry naming "default").
static bool test_all_sky_1_to_21_mapped(TestCtx& t) {
    for (int sky = 1; sky <= 21; ++sky) {
        bool found = false;
        for (size_t i = 0; i < rc::kSkyNumberHdriMapCount; ++i)
            if (rc::kSkyNumberHdriMap[i].skyNumber == sky) { found = true; break; }
        if (!found) t.fail("sky number " + std::to_string(sky) + " has no map entry");
    }
    return t.failures == 0;
}

static bool test_out_of_range_returns_default(TestCtx& t) {
    const rc::IblHdriSet& d = rc::kIblHdriSets[0];
    for (int sky : {0, 22, -1, INT_MAX, INT_MIN, 1000}) {
        const rc::IblHdriSet& got = rc::lookupHdriForSkyNumber(sky);
        CH_CHECK(t, &got == &d);
    }
    return t.failures == 0;
}

static bool test_no_duplicate_hdri_set_names(TestCtx& t) {
    std::set<std::string> seen;
    for (size_t i = 0; i < rc::kIblHdriSetCount; ++i) {
        std::string n = rc::kIblHdriSets[i].name ? rc::kIblHdriSets[i].name : "(null)";
        if (!seen.insert(n).second)
            t.fail("duplicate HDRI set name (shadows lookup-by-name): " + n);
    }
    return t.failures == 0;
}

static bool test_no_duplicate_sh_set_names(TestCtx& t) {
    std::set<std::string> seen;
    for (size_t i = 0; i < rc::kIblShSetCount; ++i) {
        std::string n = rc::kIblShSets[i].name ? rc::kIblShSets[i].name : "(null)";
        if (!seen.insert(n).second)
            t.fail("duplicate SH set name: " + n);
    }
    return t.failures == 0;
}

// Every non-null mission->SH mapping must name a set that exists.
static bool test_sh_registry_names_resolve_if_referenced(TestCtx& t) {
    int checked = 0;
    for (size_t i = 0; i < rc::kMissionShMapCount; ++i) {
        const auto& e = rc::kMissionShMap[i];
        if (e.missionName == nullptr && e.shSetName == nullptr) continue;  // placeholder
        ++checked;
        if (rc::findShSetByName(e.shSetName) == nullptr)
            t.fail(std::string("mission '") + (e.missionName ? e.missionName : "(null)") +
                   "' names unresolved SH set '" + (e.shSetName ? e.shSetName : "(null)") + "'");
    }
    std::fprintf(stderr, "    SH mission mappings checked: %d\n", checked);
    return t.failures == 0;
}

// ---- asset existence (separated from integrity) ----------------------------

static void collectHdriMissing(const fs::path& root, std::vector<std::string>& missing) {
    for (size_t i = 0; i < rc::kIblHdriSetCount; ++i) {
        std::string foundAs;
        if (!hdriAssetPresent(root, rc::kIblHdriSets[i].exrPath, foundAs))
            missing.push_back(std::string(rc::kIblHdriSets[i].name) + " -> " +
                              rc::kIblHdriSets[i].exrPath);
    }
}

// Default: INFORMATIONAL inventory — reports missing assets, never fails.
static bool test_hdri_assets_inventory(TestCtx& t) {
    (void)t;
    std::vector<std::string> missing;
    collectHdriMissing(repoRoot(), missing);
    std::fprintf(stderr, "    HDRI assets: %zu sets, %zu missing (informational)\n",
                 rc::kIblHdriSetCount, missing.size());
    for (const auto& m : missing)
        std::fprintf(stderr, "      MISSING %s\n", m.c_str());
    if (!missing.empty())
        std::fprintf(stderr, "    (set MC2_IBL_ASSET_STRICT=1 or run --test "
                             "hdri_assets_exist_strict to make this fail)\n");
    return true;  // informational only
}

// Strict: FAIL on any absent referenced asset. Default-excluded unless
// MC2_IBL_ASSET_STRICT=1; always runnable via --test. An external-pack asset
// counts as satisfied ONLY when actually installed locally (strict = local).
static bool test_hdri_assets_exist_strict(TestCtx& t) {
    std::vector<std::string> missing;
    collectHdriMissing(repoRoot(), missing);
    for (const auto& m : missing)
        t.fail("referenced HDRI asset absent (.exr and .ktx2): " + m);
    return t.failures == 0;
}

// DEFAULT CONTRACT: every registry HDRI must be EITHER present on disk
// (tracked-in-repo / installed) OR declared in the external-pack manifest.
// A reference that is neither is a real dangling reference (typo / new sky with
// no asset and no manifest line) and FAILS. The known 16K external pack passes
// because it is declared; clean checkouts stay green.
static bool test_every_hdri_tracked_or_declared(TestCtx& t) {
    const fs::path root = repoRoot();
    bool manifestOk = false;
    const std::set<std::string> declared = loadExternalManifest(root, manifestOk);
    if (!manifestOk)
        t.fail(std::string("external-pack manifest unreadable: ") + kManifestRel);
    int present = 0, ext = 0;
    for (size_t i = 0; i < rc::kIblHdriSetCount; ++i) {
        std::string foundAs, path = rc::kIblHdriSets[i].exrPath;
        for (char& c : path) if (c == '\\') c = '/';
        if (hdriAssetPresent(root, path, foundAs)) { ++present; continue; }
        if (declared.count(path)) { ++ext; continue; }
        t.fail(std::string("HDRI '") + rc::kIblHdriSets[i].name +
               "' -> " + path + " is neither tracked-on-disk nor declared external"
               " (add the asset or a manifest line)");
    }
    std::fprintf(stderr, "    HDRI accounting: %d present, %d declared-external, "
                 "%zu manifest entries\n", present, ext, declared.size());
    return t.failures == 0;
}

// Hygiene: every manifest entry must correspond to a real registry exrPath
// (catches a stale external declaration left behind after a registry edit).
static bool test_manifest_entries_match_registry(TestCtx& t) {
    bool manifestOk = false;
    const std::set<std::string> declared = loadExternalManifest(repoRoot(), manifestOk);
    if (!manifestOk) { t.fail("external-pack manifest unreadable"); return false; }
    std::set<std::string> registryPaths;
    for (size_t i = 0; i < rc::kIblHdriSetCount; ++i) {
        std::string p = rc::kIblHdriSets[i].exrPath;
        for (char& c : p) if (c == '\\') c = '/';
        registryPaths.insert(p);
    }
    for (const auto& d : declared)
        if (!registryPaths.count(d))
            t.fail("stale manifest entry (no registry HDRI references it): " + d);
    return t.failures == 0;
}

int main(int argc, char** argv) {
    Harness h("ibl_registry_contract_harness");
    // registry integrity (always default)
    h.add("index0_is_default",                    test_index0_is_default);
    h.add("every_skymap_name_resolves",           test_every_skymap_name_resolves);
    h.add("all_sky_1_to_21_mapped",               test_all_sky_1_to_21_mapped);
    h.add("out_of_range_returns_default",         test_out_of_range_returns_default);
    h.add("no_duplicate_hdri_set_names",          test_no_duplicate_hdri_set_names);
    h.add("no_duplicate_sh_set_names",            test_no_duplicate_sh_set_names);
    h.add("sh_registry_names_resolve_if_referenced", test_sh_registry_names_resolve_if_referenced);
    // asset accounting (separated from integrity)
    h.add("every_hdri_tracked_or_declared",       test_every_hdri_tracked_or_declared);
    h.add("manifest_entries_match_registry",      test_manifest_entries_match_registry);
    h.add("hdri_assets_inventory",                test_hdri_assets_inventory);
    // strict runs in the default suite only when MC2_IBL_ASSET_STRICT=1
    h.add("hdri_assets_exist_strict",             test_hdri_assets_exist_strict,
          /*inDefault=*/assetStrictEnv());
    return h.run(argc, argv);
}
