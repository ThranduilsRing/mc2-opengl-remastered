// tests/unit/test_shadow_mask_asset.cpp
// The static shadow mask as a cooked asset: BC4 block packing (the bit layout the GPU encoder mirrors), header and payload validation, and the disk
// cache through a cold store, a warm load, a changed key and a healed (corrupted then rewritten) entry, plus the per-kind LRU.
#include "doctest.h"
#include "gameos/cook_cache.h"
#include "gameos/shadow_mask_asset.h"

#include <cstdio>
#include <chrono>
#include <iterator>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace mc2shadow;

TEST_CASE("cook cache: large model recipe reader accepts 303 MiB and rejects over 512 MiB") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "mc2-large-model-recipe";
    const mc2cache::CacheKind kind{"model-", ".mci", 4096, 512ull << 20};
    const auto path = mc2cache::entryPath(dir.string(), kind, 1);
    REQUIRE(mc2cache::writeEntry(dir.string(), kind, 1, std::vector<uint8_t>{7}));
    fs::resize_file(path, 303ull << 20);
    {
        const auto bytes = mc2cache::readEntry(dir.string(), kind, 1);
        REQUIRE(bytes);
        CHECK(bytes->size() == 303ull << 20);
        CHECK(bytes->front() == 7);
        CHECK(bytes->back() == 0);
    }
    fs::resize_file(path, (512ull << 20) + 1);
    CHECK_FALSE(mc2cache::readEntry(dir.string(), kind, 1));
    fs::remove_all(dir);
}

namespace {
ShadowMaskAsset makeAsset(uint32_t texels, uint32_t mips, uint64_t key) {
    ShadowMaskAsset a;
    a.texels = texels;
    a.mips = mips;
    a.key = key;
    a.blocks.resize(static_cast<size_t>(bc4ChainBytes(texels, mips)));
    for (size_t i = 0; i < a.blocks.size(); ++i) a.blocks[i] = static_cast<uint8_t>(i * 131u + key);
    return a;
}
std::string tempDir(const char* leaf) {
    const auto dir = std::filesystem::temp_directory_path() / "mc2_test_shadow_mask" / leaf;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir.string();
}
}  // namespace

TEST_CASE("shadow mask: BC4 blocks reproduce flat, two-level and graded tiles within one palette step") {
    std::array<uint8_t, 16> flat{};
    flat.fill(200);
    CHECK(decodeBc4Block(encodeBc4Block(flat)) == flat);
    std::array<uint8_t, 16> edge{};
    for (int i = 0; i < 16; ++i) edge[static_cast<size_t>(i)] = (i % 4) < 2 ? 102 : 255;   // a shadow edge: 0.4 floor and lit
    CHECK(decodeBc4Block(encodeBc4Block(edge)) == edge);
    std::array<uint8_t, 16> ramp{};
    for (int i = 0; i < 16; ++i) ramp[static_cast<size_t>(i)] = static_cast<uint8_t>(100 + i * 9);
    const auto back = decodeBc4Block(encodeBc4Block(ramp));
    for (int i = 0; i < 16; ++i) CHECK(std::abs(int(back[static_cast<size_t>(i)]) - int(ramp[static_cast<size_t>(i)])) <= (135 / 7 + 1) / 2 + 1);
}

TEST_CASE("shadow mask: chain sizes count one block for levels under 4 texels") {
    CHECK(bc4ChainBytes(4, 1) == 8);
    CHECK(bc4ChainBytes(4, 3) == 24);   // 4, 2, 1 texels: one block each
    CHECK(bc4ChainBytes(8704, 1) == uint64_t(2176) * 2176 * 8);
    // The 1024-cell cap (12288^2) fits the asset bound.
    CHECK(bc4ChainBytes(12288, 14) + 48 <= ShadowMaskAsset::kMaxBytes);
}

TEST_CASE("shadow mask: the header guards key, evaluator, sizes and payload") {
    const ShadowMaskAsset a = makeAsset(16, 5, 0x1234);
    auto bytes = encodeShadowMaskAsset(a);
    REQUIRE(!bytes.empty());
    const auto ok = decodeShadowMaskAsset(bytes, 0x1234);
    REQUIRE(ok);
    CHECK(ok->blocks == a.blocks);
    CHECK_FALSE(decodeShadowMaskAsset(bytes, 0x1235));   // another bake's key
    auto truncated = bytes;
    truncated.pop_back();
    CHECK_FALSE(decodeShadowMaskAsset(truncated, 0x1234));
    auto flipped = bytes;
    flipped.back() ^= 0x40;
    CHECK_FALSE(decodeShadowMaskAsset(flipped, 0x1234));   // payload hash
    auto evaluator = bytes;
    evaluator[8] ^= 1;
    CHECK_FALSE(decodeShadowMaskAsset(evaluator, 0x1234));
    ShadowMaskAsset bad = a;
    bad.blocks.pop_back();
    CHECK(encodeShadowMaskAsset(bad).empty());
}

TEST_CASE("shadow mask cache: cold, warm, changed and healed") {
    const std::string dir = tempDir("cwch");
    const ShadowMaskAsset a = makeAsset(32, 6, 77);
    CHECK_FALSE(loadCachedShadowMask(dir, 77));   // cold
    REQUIRE(storeCachedShadowMask(dir, a));
    const auto warm = loadCachedShadowMask(dir, 77);
    REQUIRE(warm);
    CHECK(warm->blocks == a.blocks);
    CHECK_FALSE(loadCachedShadowMask(dir, 78));   // changed inputs: another key misses
    // A corrupted entry is a miss, and the next bake's store heals it.
    const auto path = mc2cache::entryPath(dir, kShadowMaskKind, 77);
    {
        std::FILE* f = std::fopen(path.string().c_str(), "r+b");
        REQUIRE(f);
        std::fseek(f, 60, SEEK_SET);
        std::fputc(0x5A, f);
        std::fclose(f);
    }
    CHECK_FALSE(loadCachedShadowMask(dir, 77));
    REQUIRE(storeCachedShadowMask(dir, a));
    CHECK(loadCachedShadowMask(dir, 77));
}

TEST_CASE("cook cache: each kind keeps its own newest entries") {
    const std::string dir = tempDir("lru");
    const mc2cache::CacheKind other{"other-", ".bin", 2, 0};
    const std::vector<uint8_t> payload(64, 7);
    for (uint64_t k = 1; k <= 3; ++k) REQUIRE(mc2cache::writeEntry(dir, other, k, payload));
    for (uint64_t k = 1; k <= 6; ++k) REQUIRE(storeCachedShadowMask(dir, makeAsset(8, 2, 100 + k)));
    size_t shadows = 0, others = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        shadows += name.rfind("shadow-", 0) == 0 ? 1 : 0;
        others += name.rfind("other-", 0) == 0 ? 1 : 0;
    }
    CHECK(shadows == kShadowMaskKind.maxFiles);
    CHECK(others == 2);
    CHECK(loadCachedShadowMask(dir, 106));   // the entry just written always survives its own sweep
}

TEST_CASE("cook cache: newest write stays within budget after a clock rollback") {
    const std::string dir = tempDir("clock-rollback");
    const mc2cache::CacheKind kind{"other-", ".bin", 2, 0};
    const std::vector<uint8_t> bytes(16, 7);
    const auto future = std::filesystem::file_time_type::clock::now() + std::chrono::hours(1);
    for (uint64_t key : {1, 2}) {
        REQUIRE(mc2cache::writeEntry(dir, kind, key, bytes));
        std::filesystem::last_write_time(mc2cache::entryPath(dir, kind, key), future);
    }
    REQUIRE(mc2cache::writeEntry(dir, kind, 3, bytes));
    CHECK(std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}) == 2);
    CHECK(mc2cache::readEntry(dir, kind, 3));
}

TEST_CASE("cook cache: a read refreshes the entry so eviction is least-recently-used") {
    namespace fs = std::filesystem;
    const std::string dir = tempDir("lru-read");
    const mc2cache::CacheKind kind{"other-", ".bin", 2, 0};
    const std::vector<uint8_t> bytes(16, 7);
    const auto past = fs::file_time_type::clock::now() - std::chrono::hours(2);
    for (uint64_t key : {1, 2}) {
        REQUIRE(mc2cache::writeEntry(dir, kind, key, bytes));
        fs::last_write_time(mc2cache::entryPath(dir, kind, key), past + std::chrono::minutes(key));  // key 1 is the oldest
    }
    CHECK(mc2cache::readEntry(dir, kind, 1));   // hit: key 1 becomes the most recently used
    REQUIRE(mc2cache::writeEntry(dir, kind, 3, bytes));
    CHECK(fs::exists(mc2cache::entryPath(dir, kind, 1)));
    CHECK_FALSE(fs::exists(mc2cache::entryPath(dir, kind, 2)));   // never read: the victim
    CHECK(fs::exists(mc2cache::entryPath(dir, kind, 3)));
}

TEST_CASE("cook cache: a read leaves a hardlinked entry's mtime alone (installs share it by link; InodeSums keys on size+mtime)") {
    namespace fs = std::filesystem;
    const std::string dir = tempDir("hardlinked-read");
    const mc2cache::CacheKind kind{"other-", ".bin", 4, 0};
    const std::vector<uint8_t> bytes(16, 5);
    const auto past = fs::file_time_type::clock::now() - std::chrono::hours(3);
    for (uint64_t key : {1, 2}) {
        REQUIRE(mc2cache::writeEntry(dir, kind, key, bytes));
        fs::last_write_time(mc2cache::entryPath(dir, kind, key), past);
    }
    std::error_code ec;
    fs::create_hard_link(mc2cache::entryPath(dir, kind, 1), fs::path(dir) / "install-link.bin", ec);
    REQUIRE_FALSE(ec);
    const auto sharedBefore = fs::last_write_time(mc2cache::entryPath(dir, kind, 1));
    CHECK(mc2cache::readEntry(dir, kind, 1));
    CHECK(fs::last_write_time(mc2cache::entryPath(dir, kind, 1)) == sharedBefore);   // shared inode: untouched
    CHECK(mc2cache::readEntry(dir, kind, 2));
    CHECK(fs::last_write_time(mc2cache::entryPath(dir, kind, 2)) > past);            // unshared entry: still LRU-touched
}

TEST_CASE("cook cache: a read-only entry still returns its bytes") {
    namespace fs = std::filesystem;
    const std::string dir = tempDir("read-only");
    const mc2cache::CacheKind kind{"other-", ".bin", 4, 0};
    const std::vector<uint8_t> bytes(16, 9);
    REQUIRE(mc2cache::writeEntry(dir, kind, 1, bytes));
    const auto path = mc2cache::entryPath(dir, kind, 1);
    fs::permissions(path, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read, fs::perm_options::replace);
    const auto got = mc2cache::readEntry(dir, kind, 1);
    REQUIRE(got);
    CHECK(*got == bytes);
    fs::permissions(path, fs::perms::owner_write, fs::perm_options::add);
}

TEST_CASE("shadow mask: cache key changes with alpha texels and compiled shader bytes") {
    const std::array<uint8_t, 4> alpha = {255, 255, 255, 0}, reskin = {255, 255, 255, 255};
    const std::array<uint32_t, 4> spv = {0x07230203, 1, 2, 3}, editedSpv = {0x07230203, 1, 2, 4};
    const auto key = [](const auto& pixels, const auto& code) {
        std::array<uint64_t, 6> parts = {11, 22, 33, 44, 55, 66};
        return shadowMaskContentKey(parts, fnv1a(pixels.data(), sizeof(pixels)), fnv1a(code.data(), sizeof(code)));
    };
    CHECK(key(alpha, spv) == key(alpha, spv));
    CHECK(key(alpha, spv) != key(reskin, spv));
    CHECK(key(alpha, spv) != key(alpha, editedSpv));
}
