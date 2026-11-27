#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "RenderCore/IblHdriRegistry.h"
#include "RenderCore/IblShRegistry.h"
#include "RenderCore/ProceduralSky.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"

namespace fs = std::filesystem;

TEST_CASE("procedural_sky/cubemap mips and solid-angle SH preserve a uniform probe") {
    const auto cube = RenderCore::bakeSkyCubemap([](RenderCore::SkyRgb) { return RenderCore::SkyRgb{1,2,3}; });
    REQUIRE(cube.levels.size() == 8);
    for (int mip=0;mip<8;++mip) {
        const int size = 128>>mip;
        CHECK(cube.levels[mip].size() == static_cast<size_t>(6*size*size*4));
        for (int face=0;face<6;++face) for (int c=0;c<4;++c)
            CHECK(cube.levels[mip][static_cast<size_t>(face*size*size*4+c)] == (c==3 ? 1.0f : float(c+1)));
    }
    for (int c=0;c<3;++c) {
        CHECK(cube.sh[0][c] == doctest::Approx(3.54491f*(c+1)).epsilon(.0003));
        for (int b=1;b<9;++b) CHECK(cube.sh[b][c] == doctest::Approx(0).epsilon(.0003));
    }
}

TEST_CASE("procedural_sky/rows remain finite and dark missions cannot get daylight photos") {
    const auto sun = RenderCore::skyMissionSun(1,0,1);
    for (int sky=1;sky<=21;++sky) for (int face=0;face<6;++face) {
        const auto ray = RenderCore::skyCubeDirection(face,.25f,-.75f);
        const auto rgb = RenderCore::sampleProceduralSky(sky,ray,sun,{1,1,1});
        for (float c : rgb) { CHECK(std::isfinite(c)); CHECK(c >= 0); }
    }
    for (int sky : {3,5,8,14,20,21})
        CHECK(RenderCore::skyAtmosphere(sky).daylight < .03f);
    CHECK(&RenderCore::skyAtmosphere(0) == &RenderCore::skyAtmosphere(1));
}
TEST_CASE("procedural_sky/sun azimuth and cube axes share the native camera frame") {
    const auto sun = RenderCore::skyMissionSun(1,2,3);
    CHECK(std::atan2(sun[2],sun[0]) == doctest::Approx(std::atan2(2.0f,-1.0f)));
    CHECK(RenderCore::skyCubeDirection(0,0,0)[0] == 1);
    CHECK(RenderCore::skyCubeDirection(1,0,0)[0] == -1);
    CHECK(RenderCore::skyCubeDirection(2,0,0)[1] == 1);
    CHECK(RenderCore::skyCubeDirection(3,0,0)[1] == -1);
    CHECK(RenderCore::skyCubeDirection(4,0,0)[2] == 1);
    CHECK(RenderCore::skyCubeDirection(5,0,0)[2] == -1);
    const auto row = RenderCore::sampleProceduralSky(1,sun,sun,{1,1,1});
    const auto away = RenderCore::sampleProceduralSky(1,{-sun[0],sun[1],-sun[2]},sun,{1,1,1});
    CHECK(row[0] > away[0]);
}

namespace {
namespace rc = RenderCore;

#ifndef MC2_REPO_ROOT
#define MC2_REPO_ROOT "."
#endif

static fs::path repoRoot() {
    if (const std::string e = mc2::gate::str<"MC2_CONTRACT_REPO_ROOT">(); !e.empty())
        return fs::path(e);
    return fs::path(MC2_REPO_ROOT);
}

static bool assetStrictEnv() {
    return mc2::gate::get<"MC2_IBL_ASSET_STRICT">();
}

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

static const char* kManifestRel = "docs/testing/ibl_hdri_external_pack.txt";

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

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

TEST_CASE("ibl_registry_contract_harness/index0_is_default") {
    CHECK((rc::kIblHdriSetCount > 0));
    CHECK((rc::kIblHdriSets[0].name != nullptr &&
                std::string(rc::kIblHdriSets[0].name) == "default"));
    CHECK((rc::kIblShSetCount > 0));
    CHECK((rc::kIblShSets[0].name != nullptr &&
                std::string(rc::kIblShSets[0].name) == "default"));
    return;
}

TEST_CASE("ibl_registry_contract_harness/every_skymap_name_resolves") {
    for (size_t i = 0; i < rc::kSkyNumberHdriMapCount; ++i) {
        const auto& e = rc::kSkyNumberHdriMap[i];
        if (rc::findHdriSetByName(e.hdriSetName) == nullptr)
            FAIL_CHECK((std::string("sky ") + std::to_string(e.skyNumber) +
                   " names unresolved HDRI set '" +
                   (e.hdriSetName ? e.hdriSetName : "(null)") + "'"));
    }
    return;
}

TEST_CASE("ibl_registry_contract_harness/all_sky_1_to_21_mapped") {
    for (int sky = 1; sky <= 21; ++sky) {
        bool found = false;
        for (size_t i = 0; i < rc::kSkyNumberHdriMapCount; ++i)
            if (rc::kSkyNumberHdriMap[i].skyNumber == sky) { found = true; break; }
        if (!found) FAIL_CHECK(("sky number " + std::to_string(sky) + " has no map entry"));
    }
    return;
}

TEST_CASE("ibl_registry_contract_harness/out_of_range_returns_default") {
    const rc::IblHdriSet& d = rc::kIblHdriSets[0];
    for (int sky : {0, 22, -1, INT_MAX, INT_MIN, 1000}) {
        const rc::IblHdriSet& got = rc::lookupHdriForSkyNumber(sky);
        CHECK((&got == &d));
    }
    return;
}

TEST_CASE("ibl_registry_contract_harness/no_duplicate_hdri_set_names") {
    std::set<std::string> seen;
    for (size_t i = 0; i < rc::kIblHdriSetCount; ++i) {
        std::string n = rc::kIblHdriSets[i].name ? rc::kIblHdriSets[i].name : "(null)";
        if (!seen.insert(n).second)
            FAIL_CHECK(("duplicate HDRI set name (shadows lookup-by-name): " + n));
    }
    return;
}

TEST_CASE("ibl_registry_contract_harness/no_duplicate_sh_set_names") {
    std::set<std::string> seen;
    for (size_t i = 0; i < rc::kIblShSetCount; ++i) {
        std::string n = rc::kIblShSets[i].name ? rc::kIblShSets[i].name : "(null)";
        if (!seen.insert(n).second)
            FAIL_CHECK(("duplicate SH set name: " + n));
    }
    return;
}

TEST_CASE("ibl_registry_contract_harness/sh_registry_names_resolve_if_referenced") {
    int checked = 0;
    for (size_t i = 0; i < rc::kMissionShMapCount; ++i) {
        const auto& e = rc::kMissionShMap[i];
        if (e.missionName == nullptr && e.shSetName == nullptr) continue;
        ++checked;
        if (rc::findShSetByName(e.shSetName) == nullptr)
            FAIL_CHECK((std::string("mission '") + (e.missionName ? e.missionName : "(null)") +
                   "' names unresolved SH set '" + (e.shSetName ? e.shSetName : "(null)") + "'"));
    }
    std::fprintf(stderr, "    SH mission mappings checked: %d\n", checked);
    return;
}

static void collectHdriMissing(const fs::path& root, std::vector<std::string>& missing) {
    for (size_t i = 0; i < rc::kIblHdriSetCount; ++i) {
        std::string foundAs;
        if (!hdriAssetPresent(root, rc::kIblHdriSets[i].exrPath, foundAs))
            missing.push_back(std::string(rc::kIblHdriSets[i].name) + " -> " +
                              rc::kIblHdriSets[i].exrPath);
    }
}

TEST_CASE("ibl_registry_contract_harness/hdri_assets_inventory") {

    std::vector<std::string> missing;
    collectHdriMissing(repoRoot(), missing);
    std::fprintf(stderr, "    HDRI assets: %zu sets, %zu missing (informational)\n",
                 rc::kIblHdriSetCount, missing.size());
    for (const auto& m : missing)
        std::fprintf(stderr, "      MISSING %s\n", m.c_str());
    if (!missing.empty())
        std::fprintf(stderr, "    (set MC2_IBL_ASSET_STRICT=1 to require local assets)\n");
    return;
}

// Strict mode requires local assets; the normal corpus also accepts declared external packs.
TEST_CASE("ibl_registry_contract_harness/hdri_assets_exist_strict") {
    const fs::path root = repoRoot();
    bool manifestOk = false;
    const auto declared = loadExternalManifest(root, manifestOk);
    REQUIRE(manifestOk);
    const bool strict = assetStrictEnv();
    for (size_t i = 0; i < rc::kIblHdriSetCount; ++i) {
        std::string foundAs, path = rc::kIblHdriSets[i].exrPath;
        for (char& c : path) if (c == '\\') c = '/';
        CAPTURE(path);
        CAPTURE(strict);
        CHECK((hdriAssetPresent(root, path, foundAs) || (!strict && declared.count(path))));
    }
}

TEST_CASE("ibl_registry_contract_harness/every_hdri_tracked_or_declared") {
    const fs::path root = repoRoot();
    bool manifestOk = false;
    const std::set<std::string> declared = loadExternalManifest(root, manifestOk);
    if (!manifestOk)
        FAIL_CHECK((std::string("external-pack manifest unreadable: ") + kManifestRel));
    int present = 0, ext = 0;
    for (size_t i = 0; i < rc::kIblHdriSetCount; ++i) {
        std::string foundAs, path = rc::kIblHdriSets[i].exrPath;
        for (char& c : path) if (c == '\\') c = '/';
        if (hdriAssetPresent(root, path, foundAs)) { ++present; continue; }
        if (declared.count(path)) { ++ext; continue; }
        FAIL_CHECK((std::string("HDRI '") + rc::kIblHdriSets[i].name +
               "' -> " + path + " is neither tracked-on-disk nor declared external"
               " (add the asset or a manifest line)"));
    }
    std::fprintf(stderr, "    HDRI accounting: %d present, %d declared-external, "
                 "%zu manifest entries\n", present, ext, declared.size());
    return;
}

TEST_CASE("ibl_registry_contract_harness/manifest_entries_match_registry") {
    bool manifestOk = false;
    const std::set<std::string> declared = loadExternalManifest(repoRoot(), manifestOk);
    if (!manifestOk) { FAIL_CHECK(("external-pack manifest unreadable")); FAIL_CHECK(("test returned false")); return; }
    std::set<std::string> registryPaths;
    for (size_t i = 0; i < rc::kIblHdriSetCount; ++i) {
        std::string p = rc::kIblHdriSets[i].exrPath;
        for (char& c : p) if (c == '\\') c = '/';
        registryPaths.insert(p);
    }
    for (const auto& d : declared)
        if (!registryPaths.count(d))
            FAIL_CHECK(("stale manifest entry (no registry HDRI references it): " + d));
    return;
}

}
