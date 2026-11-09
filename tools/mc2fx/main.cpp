// tools/mc2fx/main.cpp
//
// mc2fx — standalone gosFX effect-blob (.fx) inspector / round-trip tool.
//
// Thin arg dispatcher over mc2fx_core (initEngineHeadless/loadBlob/
// dumpCatalogJson/saveBlob). Subcommands:
//   mc2fx dump    <mc2.fx> [out.json]   dump effect catalog to JSON
//   mc2fx rebuild <in.fx>  <out.fx>     Load -> Save round-trip + byte-compare
//
// `rebuild` is the Save-fidelity gate: it re-emits the blob via SpecLibrary::
// Save and byte-compares against the input, reporting the first divergence.

#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mc2fx_core.h"
#include "particles/spec_library.h"   // SpecLibrary (for the returned handle)

namespace {

bool readFileBytes(const char* path, std::vector<unsigned char>& out)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(sz));
    size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

bool writeFileBytes(const char* path, const unsigned char* data, size_t n)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    size_t put = std::fwrite(data, 1, n, f);
    std::fclose(f);
    return put == n;
}

int cmdDump(const char* inPath, const char* outPath, bool full)
{
    std::vector<unsigned char> bytes;
    if (!readFileBytes(inPath, bytes)) {
        std::fprintf(stderr, "mc2fx: cannot read '%s'\n", inPath);
        return 2;
    }
    std::string error;
    const auto catalog = mc2::fx::read_catalog(bytes, error);
    if (!catalog) { std::fprintf(stderr, "mc2fx: %s\n", error.c_str()); return 4; }
    std::string out = mc2fx::dumpCatalogJson(*catalog, full);

    if (outPath) {
        if (!writeFileBytes(outPath, reinterpret_cast<const unsigned char*>(out.data()), out.size())) {
            std::fprintf(stderr, "mc2fx: cannot write '%s'\n", outPath); return 3;
        }
        std::fprintf(stderr, "mc2fx: dumped %zu effects -> %s\n", catalog->effects().size(), outPath);
    } else {
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
    return 0;
}

int cmdRebuild(const char* inPath, const char* outPath)
{
    std::vector<unsigned char> inBytes;
    if (!readFileBytes(inPath, inBytes)) {
        std::fprintf(stderr, "mc2fx: cannot read '%s'\n", inPath);
        return 2;
    }
    std::string error;
    const auto catalog = mc2::fx::read_catalog(inBytes, error);
    if (!catalog) { std::fprintf(stderr, "mc2fx: %s\n", error.c_str()); return 4; }
    const auto outBytes = catalog->rebuild();
    if (!writeFileBytes(outPath, outBytes.data(), outBytes.size())) {
        std::fprintf(stderr, "mc2fx: cannot write '%s'\n", outPath); return 3;
    }

    // --- byte-compare ---
    std::fprintf(stderr, "mc2fx rebuild: in=%zu bytes  out=%zu bytes\n",
                 inBytes.size(), outBytes.size());
    size_t n = inBytes.size() < outBytes.size() ? inBytes.size() : outBytes.size();
    size_t firstDiff = (size_t)-1;
    for (size_t i = 0; i < n; ++i) {
        if (inBytes[i] != outBytes[i]) { firstDiff = i; break; }
    }
    if (firstDiff == (size_t)-1 && inBytes.size() == outBytes.size()) {
        std::fprintf(stderr, "mc2fx rebuild: IDENTICAL (byte-exact round-trip)\n");
        std::printf("IDENTICAL\n");
        return 0;
    }
    if (firstDiff == (size_t)-1) {
        // common prefix matches but lengths differ
        firstDiff = n;
    }
    std::fprintf(stderr, "mc2fx rebuild: DIFFER at offset %zu\n", firstDiff);
    auto hexDump = [](const char* label, const std::vector<unsigned char>& b, size_t off) {
        std::fprintf(stderr, "  %s:", label);
        for (size_t i = off; i < off + 16 && i < b.size(); ++i)
            std::fprintf(stderr, " %02X", b[i]);
        std::fprintf(stderr, "\n");
    };
    size_t ctx = firstDiff >= 4 ? firstDiff - 4 : 0;
    hexDump("in ", inBytes, ctx);
    hexDump("out", outBytes, ctx);
    std::printf("DIFFER@%zu in=%zu out=%zu\n", firstDiff, inBytes.size(), outBytes.size());
    return 6;
}

int cmdBuild(const char* basePath, const char* patchPath, const char* outPath)
{
    std::vector<unsigned char> baseBytes;
    if (!readFileBytes(basePath, baseBytes)) {
        std::fprintf(stderr, "mc2fx: cannot read base '%s'\n", basePath); return 2;
    }
    std::vector<unsigned char> patchBytes;
    if (!readFileBytes(patchPath, patchBytes)) {
        std::fprintf(stderr, "mc2fx: cannot read patch '%s'\n", patchPath); return 2;
    }
    std::string patchText(reinterpret_cast<const char*>(patchBytes.data()), patchBytes.size());

    std::vector<mc2fx::PatchEdit> edits;
    std::string err;
    if (!mc2fx::parsePatchJson(patchText, edits, err)) {
        std::fprintf(stderr, "mc2fx: patch parse error: %s\n", err.c_str()); return 6;
    }

    mc2fx::initEngineHeadless();
    mc2::particles::SpecLibrary* lib = mc2fx::loadBlob(baseBytes.data(), baseBytes.size());
    if (!lib) { std::fprintf(stderr, "mc2fx: load failed\n"); return 4; }

    std::string report;
    unsigned applied = mc2fx::applyPatch(lib, edits, report);
    std::fprintf(stderr, "mc2fx build: %u edit(s) parsed, %u applied:\n%s",
                 (unsigned)edits.size(), applied, report.c_str());

    std::vector<unsigned char> outBytes;
    size_t reserveHint = baseBytes.size() * 4 + (1u << 20);
    if (!mc2fx::saveBlob(lib, outBytes, reserveHint)) {
        std::fprintf(stderr, "mc2fx: save failed\n"); return 5;
    }
    if (!writeFileBytes(outPath, outBytes.data(), outBytes.size())) {
        std::fprintf(stderr, "mc2fx: cannot write '%s'\n", outPath); return 3;
    }
    std::fprintf(stderr, "mc2fx build: wrote %zu bytes -> %s\n", outBytes.size(), outPath);
    return 0;
}

// Shared tail for clone/new: base already loaded, `record` holds the new spec's
// serialized bytes and `newCount` the post-splice count. Saves base, splices,
// writes out.
int spliceAndWrite(mc2::particles::SpecLibrary* lib,
                   const std::vector<unsigned char>& baseBytes,
                   const std::vector<unsigned char>& record,
                   const char* outPath)
{
    std::vector<unsigned char> baseBlob;
    size_t reserveHint = baseBytes.size() * 4 + (1u << 20);
    if (!mc2fx::saveBlob(lib, baseBlob, reserveHint)) {
        std::fprintf(stderr, "mc2fx: base save failed\n"); return 5;
    }
    // Header = [int fourcc][int version][uint32 count]; count at byte offset 8.
    const size_t kCountOffset = 8;
    unsigned newCount = lib->Count() + 1u;
    std::vector<unsigned char> blob =
        mc2fx::spliceSpec(baseBlob, record, kCountOffset, newCount);
    if (!writeFileBytes(outPath, blob.data(), blob.size())) {
        std::fprintf(stderr, "mc2fx: cannot write '%s'\n", outPath); return 3;
    }
    std::fprintf(stderr, "mc2fx: wrote %zu specs (%zu bytes) -> %s\n",
                 (size_t)newCount, blob.size(), outPath);
    return 0;
}

int cmdClone(const char* basePath, const char* srcName, const char* newName,
             const char* outPath)
{
    std::vector<unsigned char> baseBytes;
    if (!readFileBytes(basePath, baseBytes)) {
        std::fprintf(stderr, "mc2fx: cannot read base '%s'\n", basePath); return 2;
    }
    mc2fx::initEngineHeadless();
    mc2::particles::SpecLibrary* lib = mc2fx::loadBlob(baseBytes.data(), baseBytes.size());
    if (!lib) { std::fprintf(stderr, "mc2fx: load failed\n"); return 4; }

    std::vector<unsigned char> record;
    int rc = mc2fx::cloneSpecBytes(lib, srcName, newName, record);
    if (rc == 1) { std::fprintf(stderr, "mc2fx clone: src '%s' not found\n", srcName); return 7; }
    if (rc == 2) { std::fprintf(stderr, "mc2fx clone: name '%s' already exists (abort)\n", newName); return 7; }
    if (rc != 0) { std::fprintf(stderr, "mc2fx clone: serialize/reparse failed\n"); return 5; }
    std::fprintf(stderr, "mc2fx clone: '%s' -> '%s' (record %zu bytes, route A object-reparse)\n",
                 srcName, newName, record.size());
    return spliceAndWrite(lib, baseBytes, record, outPath);
}

int cmdNew(const char* basePath, const char* type, const char* newName,
           const char* outPath)
{
    std::vector<unsigned char> baseBytes;
    if (!readFileBytes(basePath, baseBytes)) {
        std::fprintf(stderr, "mc2fx: cannot read base '%s'\n", basePath); return 2;
    }
    mc2fx::initEngineHeadless();
    mc2::particles::SpecLibrary* lib = mc2fx::loadBlob(baseBytes.data(), baseBytes.size());
    if (!lib) { std::fprintf(stderr, "mc2fx: load failed\n"); return 4; }

    std::vector<unsigned char> record;
    std::string err;
    int rc = mc2fx::newSpecBytes(lib, type, newName, record, err);
    if (rc == 1) { std::fprintf(stderr, "mc2fx new: name '%s' already exists (abort)\n", newName); return 7; }
    if (rc == 2) { std::fprintf(stderr, "mc2fx new: %s\n", err.c_str()); return 7; }
    if (rc != 0) { std::fprintf(stderr, "mc2fx new: %s\n", err.c_str()); return 5; }
    std::fprintf(stderr, "mc2fx new: type '%s' name '%s' (defaults built, record %zu bytes)\n",
                 type, newName, record.size());
    return spliceAndWrite(lib, baseBytes, record, outPath);
}

void usage()
{
    std::fprintf(stderr,
        "mc2fx — gosFX effect blob inspector\n"
        "  mc2fx dump    [--full] <mc2.fx> [out.json]   dump catalog (--full = per-curve values)\n"
        "  mc2fx rebuild <in.fx>  <out.fx>              Load->Save round-trip + byte-compare\n"
        "  mc2fx compare <in.fx>                       offline native/legacy semantic comparison\n"
        "  mc2fx build   <base.fx> <patch.json> <out.fx> apply JSON edits + re-emit\n"
        "  mc2fx clone   <base.fx> <srcName> <newName> <out.fx>  duplicate+rename an effect\n"
        "  mc2fx new     <base.fx> <type> <newName> <out.fx>     blank effect from a type\n"
        "    concrete types: PointCloud ShardCloud CardCloud EffectCloud\n"
        "                    Card Tube DebrisCloud PointLight\n"
        "    (ParticleCloud/SpinningCloud/Singleton are abstract bases; Shape/\n"
        "     ShapeCloud/PertCloud need an asset/arg -> not authorable blank headless)\n"
        "\n"
        "  patch.json schema:\n"
        "    { \"edits\": [ { \"effect\": \"PPC_Trail\",\n"
        "                     \"set\": { \"m_lifeSpan\": { \"type\": \"constant\", \"value\": 7.5 } } } ] }\n");
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { usage(); return 1; }
    std::string cmd = argv[1];
    if (cmd == "compare") {
        std::vector<unsigned char> bytes;
        if (!readFileBytes(argv[2], bytes)) return 2;
        std::string report;
        const auto catalog = mc2::fx::read_catalog(bytes, report);
        if (!catalog) { std::fprintf(stderr, "%s\n", report.c_str()); return 4; }
        mc2fx::initEngineHeadless();
        const bool equal = mc2fx::compareLegacy(*catalog, bytes, report);
        std::printf("%s\n", report.c_str());
        return equal ? 0 : 5;
    }
    if (cmd == "dump") {
        // dump [--full] <in.fx> [out.json]
        bool full = false;
        int ai = 2;
        if (std::string(argv[ai]) == "--full") { full = true; ++ai; }
        if (ai >= argc) { usage(); return 1; }
        const char* inPath = argv[ai++];
        const char* outPath = (ai < argc) ? argv[ai] : nullptr;
        return cmdDump(inPath, outPath, full);
    }
    if (cmd == "rebuild") {
        if (argc < 4) { usage(); return 1; }
        return cmdRebuild(argv[2], argv[3]);
    }
    if (cmd == "build") {
        if (argc < 5) { usage(); return 1; }
        return cmdBuild(argv[2], argv[3], argv[4]);
    }
    if (cmd == "clone") {
        if (argc < 6) { usage(); return 1; }
        return cmdClone(argv[2], argv[3], argv[4], argv[5]);
    }
    if (cmd == "new") {
        if (argc < 6) { usage(); return 1; }
        return cmdNew(argv[2], argv[3], argv[4], argv[5]);
    }
    usage();
    return 1;
}
