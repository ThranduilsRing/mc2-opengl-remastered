#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include <algorithm>
#include <cmath>

namespace shoreline_animation_test {
using std::abs;
using std::sin;
static float smoothstep(float lo, float hi, float x) {
    const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
#include "../../shaders/include/water_shore_animation.hglsl"
}

TEST_CASE("shoreline_animation/depth_and_motion") {
    using namespace shoreline_animation_test;
    // Exposed rock/land and deep water never receive the animated material.
    CHECK(wsa_depthFade(-20.0f) == 0.0f);
    CHECK(wsa_depthFade(0.0f) == 0.0f);
    CHECK(wsa_depthFade(3.0f) == 1.0f);
    CHECK(wsa_depthFade(24.0f) < wsa_depthFade(12.0f));
    CHECK(wsa_depthFade(32.0f) == 0.0f);
    constexpr float pi = 3.14159265f;
    // Crest advances and retreats over one five-second period, without state.
    CHECK(wsa_reach(pi * 0.5f) == doctest::Approx(38.0f));
    CHECK(wsa_reach(pi * 1.5f) == doctest::Approx(6.0f));
    CHECK(wsa_reach(2.0f * pi) == doctest::Approx(wsa_reach(0.0f)));
    CHECK(wsa_crest(38.0f, wsa_reach(pi * 0.5f), 1.0f) == 1.0f);
    CHECK(wsa_crest(38.0f, wsa_reach(pi * 1.5f), 1.0f) == 0.0f);
    CHECK(wsa_wash(12.0f, wsa_reach(pi * 0.5f), 1.0f) > 0.0f);
    CHECK(wsa_wash(12.0f, wsa_reach(pi * 1.5f), 1.0f) == 0.0f);
    // Coarse screen footprints stay finite and cannot amplify coverage.
    CHECK(wsa_crest(38.0f, 22.0f, 64.0f) >= 0.0f);
    CHECK(wsa_crest(38.0f, 22.0f, 64.0f) <= 1.0f);
}

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {

#ifndef MC2_REPO_ROOT
#define MC2_REPO_ROOT "."
#endif

static fs::path repoRoot() {
    if (const std::string e = mc2::gate::str<"MC2_CONTRACT_REPO_ROOT">(); !e.empty())
        return fs::path(e);
    return fs::path(MC2_REPO_ROOT);
}

static const char* kSourceDirs[] = {
    "GameOS", "mclib", "RenderCore", "RenderWorld", "GameAdapters", "code"
};

static const char* kShaderExts[] = {
    ".vert", ".frag", ".geom", ".comp", ".tesc", ".tese"
};

static const char* kRemovedShaderFiles[] = {
    "bloom_threshold.frag", "bloom_blur.frag", "bloom.frag", "godray.frag"
};

static bool hasShaderExt(const std::string& s) {
    for (const char* e : kShaderExts) {
        const std::string ext = e;
        if (s.size() >= ext.size() &&
            s.compare(s.size() - ext.size(), ext.size(), ext) == 0)
            return true;
    }
    return false;
}

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void collectShaderRefs(const std::string& text, std::set<std::string>& out) {
    size_t pos = 0;
    while ((pos = text.find('"', pos)) != std::string::npos) {
        size_t end = text.find('"', pos + 1);
        if (end == std::string::npos) break;
        std::string lit = text.substr(pos + 1, end - pos - 1);
        pos = end + 1;

        for (char& c : lit) if (c == '\\') c = '/';
        if (lit.rfind("shaders/", 0) == 0 && hasShaderExt(lit) &&
            lit.find('%') == std::string::npos)
            out.insert(lit);
    }
}

static std::set<std::string> scanSourceShaderRefs(const fs::path& root) {
    std::set<std::string> refs;
    for (const char* d : kSourceDirs) {
        fs::path dir = root / d;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = it->path().extension().string();
            if (ext == ".cpp" || ext == ".h" || ext == ".hpp" ||
                ext == ".cc"  || ext == ".cxx")
                collectShaderRefs(readFile(it->path()), refs);
        }
    }
    return refs;
}

TEST_CASE("shader_contract_harness/referenced_shaders_exist") {
    const fs::path root = repoRoot();
    const auto refs = scanSourceShaderRefs(root);
    // GL end (lane2 s68): no source file loads a GLSL path any more, so an empty set is correct.
    // Any shaders/*.{vert,frag,...} literal that reappears must still exist on disk.
    int missing = 0;
    for (const auto& rel : refs) {
        std::error_code ec;
        if (!fs::is_regular_file(root / rel, ec)) {
            ++missing;
            FAIL_CHECK(("referenced shader missing on disk: " + rel));
        }
    }
    std::fprintf(stderr, "    referenced shaders: %zu, missing: %d\n", refs.size(), missing);
    CHECK((missing == 0));
}

TEST_CASE("shader_contract_harness/removed_postfx_absent") {
    const fs::path root = repoRoot();
    const auto refs = scanSourceShaderRefs(root);
    for (const char* removed : kRemovedShaderFiles) {
        const std::string base = removed;

        for (const auto& rel : refs) {
            const std::string fname = fs::path(rel).filename().string();
            if (fname == base)
                FAIL_CHECK((std::string("removed post-FX shader still referenced: ") + base));
        }

        std::error_code ec;
        if (fs::is_regular_file(root / "shaders" / base, ec))
            FAIL_CHECK((std::string("removed post-FX shader still on disk: ") + base));
    }
    return;
}

TEST_CASE("shader_contract_harness/shader_inventory") {
    const fs::path shaders = repoRoot() / "shaders";
    std::error_code ec;
    CHECK((fs::is_directory(shaders, ec)));
    std::map<std::string, int> byExt;
    int total = 0;
    for (auto it = fs::recursive_directory_iterator(shaders, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;

        if (it->path().string().find("/fixtures/") != std::string::npos ||
            it->path().string().find("\\fixtures\\") != std::string::npos)
            continue;
        const std::string ext = it->path().extension().string();
        if (hasShaderExt(ext)) { ++byExt[ext]; ++total; }
    }
    for (const auto& kv : byExt)
        std::fprintf(stderr, "    %-6s %d\n", kv.first.c_str(), kv.second);
    std::fprintf(stderr, "    total runtime shaders: %d\n", total);
    CHECK((total > 0));
    return;
}

TEST_CASE("shader_contract_harness/ifdef_symbols") {

    const fs::path shaders = repoRoot() / "shaders";
    std::error_code ec;
    std::map<std::string, int> symbols;
    int directives = 0;
    for (auto it = fs::recursive_directory_iterator(shaders, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (!hasShaderExt(it->path().extension().string())) continue;
        std::istringstream src(readFile(it->path()));
        std::string line;
        while (std::getline(src, line)) {
            const size_t h = line.find_first_not_of(" \t");
            if (h == std::string::npos || line[h] != '#') continue;
            std::string rest = line.substr(h + 1);
            const char* kw[] = {"ifdef", "ifndef"};
            for (const char* k : kw) {
                if (rest.rfind(k, 0) == 0) {
                    std::istringstream ls(rest.substr(std::string(k).size()));
                    std::string sym; ls >> sym;
                    if (!sym.empty()) { ++symbols[sym]; ++directives; }
                }
            }
        }
    }
    std::fprintf(stderr, "    #ifdef/#ifndef directives: %d across %zu distinct symbols\n",
                directives, symbols.size());
    for (const auto& kv : symbols)
        std::fprintf(stderr, "    %-28s %d\n", kv.first.c_str(), kv.second);
    return;
}

TEST_CASE("shader_contract_harness/missing_fixture" * doctest::should_fail()) {
    const fs::path bogus = repoRoot() / "shaders" / "__intentionally_absent__.frag";
    std::error_code ec;
    CHECK((fs::is_regular_file(bogus, ec)));
    return;
}

}
