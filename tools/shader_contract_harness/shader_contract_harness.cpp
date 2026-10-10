// tools/shader_contract_harness/shader_contract_harness.cpp
// SUBSYSTEM-HARNESS-ARC / SHADER-CONTRACT-HARNESS-1
//
// Validates the shader subsystem's *file contract* without a GL context, without
// mission data, and without linking any production code. It scans the source
// tree (as text) and the shaders/ directory (as files) and asserts invariants:
//
//   * referenced_shaders_exist  — every "shaders/<name>.<ext>" literal in engine
//     source resolves to a file on disk (catches a runtime ref to a deleted /
//     renamed shader).
//   * removed_postfx_absent     — deleted post-FX shaders (bloom/godray) are
//     neither referenced in source nor present on disk.
//   * shader_inventory          — shaders/ enumerates by extension, count > 0.
//   * ifdef_symbols             — informational: collect #ifdef/#if defined
//     symbols across shaders for the future SPIR-V seam (never fails).
//   * missing_fixture (demo)    — inDefault=false; fails when invoked directly,
//     proving the failure path on a real file check.
//
// Repo root: compile-time MC2_REPO_ROOT (set by CMake), overridable at runtime
// via env MC2_CONTRACT_REPO_ROOT. No GL, no production link, no shader compile.
//
// Build (standalone):
//   cmake -S tools/shader_contract_harness -B build64-shader -G "Visual Studio 17 2022" -A x64
//   cmake --build build64-shader --config RelWithDebInfo --target shader_contract_harness

#include "contract_harness.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace contract_harness;

#ifndef MC2_REPO_ROOT
#define MC2_REPO_ROOT "."
#endif

// ---- helpers ---------------------------------------------------------------

static fs::path repoRoot() {
    if (const std::string e = mc2::gate::str<"MC2_CONTRACT_REPO_ROOT">(); !e.empty())   // "" when unset or empty
        return fs::path(e);
    return fs::path(MC2_REPO_ROOT);
}

// Engine source dirs scanned for shader-path string literals.
static const char* kSourceDirs[] = {
    "GameOS", "mclib", "RenderCore", "RenderWorld", "GameAdapters", "code"
};

static const char* kShaderExts[] = {
    ".vert", ".frag", ".geom", ".comp", ".tesc", ".tese"
};

// Post-FX shaders deleted from the engine (see MEMORY: DEAD-POST-FX-CLEANUP).
// A surviving reference or on-disk file is a cleanup regression.
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

// Extract every "shaders/<...>.<shaderext>" double-quoted literal from text.
// Returns the path RELATIVE to repo root (e.g. "shaders/mech.vert").
static void collectShaderRefs(const std::string& text, std::set<std::string>& out) {
    size_t pos = 0;
    while ((pos = text.find('"', pos)) != std::string::npos) {
        size_t end = text.find('"', pos + 1);
        if (end == std::string::npos) break;
        std::string lit = text.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        // Normalize backslashes; require it to start with shaders/ and end .<ext>.
        for (char& c : lit) if (c == '\\') c = '/';
        if (lit.rfind("shaders/", 0) == 0 && hasShaderExt(lit) &&
            lit.find('%') == std::string::npos)   // skip printf-style format strings
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

// ---- tests -----------------------------------------------------------------

// Every "shaders/X" literal in engine source must resolve to a file on disk.
static bool test_referenced_shaders_exist(TestCtx& t) {
    const fs::path root = repoRoot();
    const auto refs = scanSourceShaderRefs(root);
    CH_CHECK(t, !refs.empty());   // we must have found *some* refs, else scan is broken
    int missing = 0;
    for (const auto& rel : refs) {
        std::error_code ec;
        if (!fs::is_regular_file(root / rel, ec)) {
            ++missing;
            t.fail("referenced shader missing on disk: " + rel);
        }
    }
    std::fprintf(stderr, "    referenced shaders: %zu, missing: %d\n", refs.size(), missing);
    return missing == 0;
}

// Deleted post-FX shaders must not be referenced in source nor present on disk.
static bool test_removed_postfx_absent(TestCtx& t) {
    const fs::path root = repoRoot();
    const auto refs = scanSourceShaderRefs(root);
    for (const char* removed : kRemovedShaderFiles) {
        const std::string base = removed;
        // (a) not referenced in source
        for (const auto& rel : refs) {
            const std::string fname = fs::path(rel).filename().string();
            if (fname == base)
                t.fail(std::string("removed post-FX shader still referenced: ") + base);
        }
        // (b) not present on disk under shaders/
        std::error_code ec;
        if (fs::is_regular_file(root / "shaders" / base, ec))
            t.fail(std::string("removed post-FX shader still on disk: ") + base);
    }
    return t.failures == 0;
}

// shaders/ enumerates by extension and is non-empty (excludes fixtures/).
static bool test_shader_inventory(TestCtx& t) {
    const fs::path shaders = repoRoot() / "shaders";
    std::error_code ec;
    CH_CHECK(t, fs::is_directory(shaders, ec));
    std::map<std::string, int> byExt;
    int total = 0;
    for (auto it = fs::recursive_directory_iterator(shaders, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        // Skip the reflection fixtures — not runtime shaders.
        if (it->path().string().find("/fixtures/") != std::string::npos ||
            it->path().string().find("\\fixtures\\") != std::string::npos)
            continue;
        const std::string ext = it->path().extension().string();
        if (hasShaderExt(ext)) { ++byExt[ext]; ++total; }
    }
    for (const auto& kv : byExt)
        std::fprintf(stderr, "    %-6s %d\n", kv.first.c_str(), kv.second);
    std::fprintf(stderr, "    total runtime shaders: %d\n", total);
    CH_CHECK(t, total > 0);
    return t.failures == 0;
}

// Informational: classify #ifdef / #if defined symbols across shaders for the
// future SPIR-V permutation seam. Never fails (data-only).
static bool test_ifdef_symbols(TestCtx& t) {
    (void)t;
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
    return true;  // informational only
}

// Demo failure (inDefault=false): asserts a deliberately-absent file exists.
// Proves the failure path on a real filesystem check; never runs in the green
// default suite.
static bool test_missing_fixture(TestCtx& t) {
    const fs::path bogus = repoRoot() / "shaders" / "__intentionally_absent__.frag";
    std::error_code ec;
    CH_CHECK(t, fs::is_regular_file(bogus, ec));  // intentionally false
    return t.failures == 0;
}

int main(int argc, char** argv) {
    Harness h("shader_contract_harness");
    h.add("referenced_shaders_exist", test_referenced_shaders_exist);
    h.add("removed_postfx_absent",    test_removed_postfx_absent);
    h.add("shader_inventory",         test_shader_inventory);
    h.add("ifdef_symbols",            test_ifdef_symbols);
    h.add("missing_fixture",          test_missing_fixture, /*inDefault=*/false);
    return h.run(argc, argv);
}
