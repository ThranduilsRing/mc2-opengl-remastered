#include "doctest.h"
#include "terrain_field_store.h"
#include "terrain_surface_cook.h"
#include "file.h"
#include "ffile.h"
#include "fastfile.h"

#include <bit>
#include <limits>
#include <cstdio>

namespace {
mc2terrain::SurfaceAsset sampleSurface() {
    return {2, 1, 0x123456789abcdef0ull, {{0.1f, 0.2f, 0.3f, 1}, {0.9f, 0.8f, 0.7f, 1}}};
}
void putWord(std::vector<uint8_t>& bytes, size_t at, uint64_t value, unsigned count = 4) {
    for (unsigned i = 0; i < count; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}
}

TEST_CASE("surface asset: explicit wire roundtrip and unique store versions") {
    const auto asset = sampleSurface();
    const auto bytes = mc2terrain::encodeSurfaceAsset(asset);
    REQUIRE(bytes.size() == 48 + 2 * 16);
    CHECK(bytes[0] == 'T'); CHECK(bytes[1] == 'S'); CHECK(bytes[2] == 'F'); CHECK(bytes[3] == '1');
    mc2terrain::TerrainFieldStore first, second;
    REQUIRE(first.loadSurfaceAsset(bytes, asset.sourceHash));
    CHECK(first.surfaceAsset().macroColor == asset.macroColor);
    CHECK(first.surfaceAsset().width == 2);
    REQUIRE(second.loadSurfaceAsset(bytes, asset.sourceHash));
    CHECK(second.surfaceVersion() > first.surfaceVersion());
    const auto version = first.surfaceVersion();
    first.clearSurfaceAsset();
    CHECK(first.surfaceAsset().macroColor.empty());
    CHECK(first.surfaceVersion() > version);
}

TEST_CASE("surface asset: stale truncated oversized corrupt replacements preserve active bytes and version") {
    const auto asset = sampleSurface();
    const auto good = mc2terrain::encodeSurfaceAsset(asset);
    mc2terrain::TerrainFieldStore store;
    REQUIRE(store.loadSurfaceAsset(good, asset.sourceHash));
    const auto version = store.surfaceVersion();
    auto rejects = [&](const auto& bytes, uint64_t source) {
        CHECK_FALSE(store.loadSurfaceAsset(bytes, source));
        CHECK(store.surfaceVersion() == version);
        CHECK(mc2terrain::encodeSurfaceAsset(store.surfaceAsset()) == good);
    };
    rejects(good, asset.sourceHash + 1);
    for (size_t size = 0; size < good.size(); ++size)
        rejects(std::span<const uint8_t>(good).first(size), asset.sourceHash);
    for (const size_t offset : {size_t(0), size_t(4), size_t(8), size_t(20), size_t(32), size_t(48)}) {
        auto bytes = good; bytes[offset] ^= 0x40; rejects(bytes, asset.sourceHash);
    }
    for (uint32_t width : {0u, mc2terrain::SurfaceAsset::kMaxSide + 1, 0xffffffffu}) {
        auto bytes = good; putWord(bytes, 12, width); rejects(bytes, asset.sourceHash);
    }
    auto bytes = good; bytes.push_back(0); rejects(bytes, asset.sourceHash);
    bytes = good; putWord(bytes, 40, 0xffffffffffffffffull, 8); rejects(bytes, asset.sourceHash);
    for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -0.1f, 1.1f}) {
        bytes = good; putWord(bytes, 48, std::bit_cast<uint32_t>(bad));
        putWord(bytes, 32, mc2terrain::surfaceHash(std::span<const uint8_t>(bytes).subspan(48)), 8);
        rejects(bytes, asset.sourceHash);
        auto candidate = asset; candidate.macroColor[0][0] = bad;
        CHECK_FALSE(store.setSurfaceAsset(candidate, asset.sourceHash));
        CHECK(mc2terrain::encodeSurfaceAsset(candidate).empty());
        CHECK(store.surfaceVersion() == version);
    }
}

TEST_CASE("surface asset: resolved stock FST and loose inputs give the same complete cook") {
    const char* sourcePath = "mvh_scratch_surface_source.rgba";
    const char* fstPath = "mvh_scratch_surface_providers.fst";
    const std::vector<uint8_t> stock = {22,45,78,255, 100,80,40,255, 33,99,44,255, 88,77,66,255};
    auto overrideBytes = stock; overrideBytes[0] = 244;
    auto write = [&](const auto& bytes) {
        FILE* f = std::fopen(sourcePath,"wb"); REQUIRE(f != nullptr);
        REQUIRE(std::fwrite(bytes.data(),1,bytes.size(),f)==bytes.size());
        REQUIRE(std::fclose(f)==0);
    };
    auto read = [&](bool disk) {
        File f; REQUIRE(f.open(sourcePath)==NO_ERR);
        CHECK(f.isLoadedFromDisk()==disk);
        std::vector<uint8_t> bytes(f.getLength());
        REQUIRE(f.read(bytes.data(),bytes.size())==static_cast<long>(bytes.size()));
        return bytes;
    };
    auto resolve = [&](const auto& bytes, const auto& hint) {
        const auto colors = mc2terrain::surfaceColorsFromRgba(bytes,false);
        mc2terrain::SurfaceCookInput input;
        input.color=colors; input.colorWidth=2; input.colorHeight=2; input.worldSpan=256;
        input.outputWidth=4; input.outputHeight=4;
        auto asset = mc2terrain::resolveSurfaceAsset(input,hint);
        REQUIRE(asset.has_value());
        CHECK(asset->macroColor.size()==16); // complete coverage even when no cook matches
        return mc2terrain::encodeSurfaceAsset(*asset);
    };
    const std::vector<uint8_t> missing;
    write(stock);
    const auto stockResolved=read(true);
    const auto stockCook=resolve(stockResolved,missing);
    REQUIRE(std::remove(sourcePath)==0);
    {
        FastFile archive;
        REQUIRE(archive.create(fstPath,true)==NO_ERR);
        REQUIRE(archive.reserve(1)==NO_ERR);
        auto packed=stock;
        REQUIRE(archive.writeFast(sourcePath,packed.data(),packed.size())==NO_ERR);
        archive.close();
    }
    struct Registry {
        Registry() {
            maxFastFiles=4; numFastFiles=0;
            fastFiles=static_cast<FastFile**>(std::calloc(maxFastFiles,sizeof(FastFile*)));
        }
        ~Registry() { FastFileFini(); }
    };
    {
        Registry registry;
        REQUIRE(FastFileInit(fstPath));
        CHECK(fileExists(sourcePath)==FILE_ON_FST);
        const auto packedResolved=read(false);
        CHECK(packedResolved==stockResolved);
        CHECK(resolve(packedResolved,missing)==stockCook);
        CHECK(resolve(packedResolved,stockCook)==stockCook);
        write(stock);
        CHECK(read(true)==packedResolved);
        CHECK(resolve(read(true),stockCook)==stockCook);
        write(overrideBytes);
        const auto looseResolved=read(true);
        CHECK(looseResolved==overrideBytes);
        const auto looseCook=resolve(looseResolved,missing);
        CHECK(looseCook!=stockCook);
        CHECK(resolve(looseResolved,stockCook)==looseCook); // stale stock cook cannot hide a mod override
        auto truncated=stockCook; truncated.resize(21);
        CHECK(resolve(looseResolved,truncated)==looseCook);
        REQUIRE(std::remove(sourcePath)==0);
        CHECK(resolve(read(false),looseCook)==stockCook); // removing override restores the FST provider
    }
    REQUIRE(std::remove(fstPath)==0);
    mc2terrain::setSurfaceMissionCook(stockCook);
    CHECK(std::vector<uint8_t>(mc2terrain::surfaceMissionCook().begin(),mc2terrain::surfaceMissionCook().end())==stockCook);
    mc2terrain::setSurfaceMissionCook({});
    CHECK(mc2terrain::surfaceMissionCook().empty());
}
