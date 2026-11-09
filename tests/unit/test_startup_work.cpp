#include "doctest.h"
#include "RenderCore/StaticPropVertexIndex.h"
#include "GameAdapters/vegetation_noise.h"
#include "tga_scratch.h"
#include "content_source_digest.h"
#include <array>
#include <cstring>

TEST_CASE("startup_work: source digest batches reads and rejects short input") {
    struct Input {
        std::vector<uint8_t> bytes;
        std::size_t at = 0, calls = 0;
        bool shortRead = false;
        std::size_t fileSize() const { return bytes.size(); }
        long read(uint8_t* to, long count) {
            ++calls;
            if (shortRead) return count - 1;
            std::memcpy(to, bytes.data() + at, count);
            at += count;
            return count;
        }
    };
    const auto hash = [](std::span<const uint8_t> bytes, uint64_t seed) {
        for (auto b : bytes) seed = (seed ^ b) * 1099511628211ull;
        return seed;
    };
    for (std::size_t size : {0u, 7u, 65536u, 1048576u, 2097165u}) {
        Input input{std::vector<uint8_t>(size)};
        for (std::size_t i = 0; i < size; ++i) input.bytes[i] = static_cast<uint8_t>(i * 71);
        auto result = mc2::content::readSourceDigest(input, hash);
        REQUIRE(result);
        CHECK(*result == hash(input.bytes, 14695981039346656037ull));
        CHECK(input.calls == (size + 1048575) / 1048576);
        input.shortRead = true; input.at = 0;
        if (size) CHECK_FALSE(mc2::content::readSourceDigest(input, hash));
    }
}

TEST_CASE("startup_work: vertex index keeps corner order without rehashing") {
    for (bool twoSided : {false, true}) {
        auto index = mc2_prop::makeVertexIndex(4096, twoSided);
        const auto buckets = index.bucket_count();
        const uint32_t count = 4096 * (twoSided ? 6 : 3);
        for (uint32_t i = 0; i < count; ++i) {
            std::string packed(40, '\0');
            std::memcpy(packed.data(), &i, sizeof(i));
            auto [it, inserted] = index.try_emplace(packed, i);
            REQUIRE(inserted);
            CHECK(it->second == i);
            CHECK_FALSE(index.try_emplace(packed, count).second);
            CHECK(index.at(packed) == i);
        }
        CHECK(index.bucket_count() == buckets);
        CHECK(index.size() == count);
    }
}

TEST_CASE("startup_work: appearance hashes preserve authored values") {
    using namespace mc2::vegetation;
    for (float x : {-5678.25f, 0.f, 913.5f})
    for (float y : {-1234.5f, 0.f, 777.25f})
    for (int i = 0; i < 12; ++i) {
        const float fi = static_cast<float>(i);
        const auto h = appearanceNoise(x, y, fi);
        CHECK(h[0] == hashf(x + fi * 199.1f, y + fi * 17.3f));
        CHECK(h[1] == hashf(y + fi * 151.3f, x + fi * 11.7f));
        CHECK(h[2] == hashf(x + fi * 37.9f, y + fi * 59.1f));
        CHECK(h[3] == hashf(y + fi * 61.3f, x + fi * 43.1f));
        CHECK(h[4] == hashf(x + fi * 113.7f, y + fi * 89.3f));
    }
}

TEST_CASE("startup_work: TGA pixel expansion reuses 32-bit storage and preserves channels") {
    std::vector<unsigned char> rgba{99, 98, 1, 2, 3, 4, 5, 6, 7, 8};
    const auto* original = rgba.data();
    REQUIRE(mc2::tga::expandPixels(rgba, 2, 2, 32));
    CHECK(rgba.data() == original);
    CHECK(rgba == std::vector<unsigned char>{99, 98, 1, 2, 3, 4, 5, 6, 7, 8});
    std::vector<unsigned char> rgb{99, 98, 1, 2, 3, 5, 6, 7};
    REQUIRE(mc2::tga::expandPixels(rgb, 2, 2, 24));
    CHECK(rgb == std::vector<unsigned char>{99, 98, 1, 2, 3, 255, 5, 6, 7, 255});
    std::vector<unsigned char> shortRead{99, 98, 1, 2};
    CHECK_FALSE(mc2::tga::expandPixels(shortRead, 2, 2, 24));
    CHECK_FALSE(mc2::tga::expandPixels(shortRead, 8, 2, 32));
}
