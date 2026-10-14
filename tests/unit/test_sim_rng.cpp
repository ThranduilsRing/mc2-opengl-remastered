#include "doctest.h"
#include "sim_rng.h"

#include <algorithm>

namespace {
using namespace mc2::sim;
using mc2::save::Bytes;
Bytes rngWire(std::uint32_t root, std::vector<RngStream> records) {
    mc2::save::Archive a;
    a(root, records);
    REQUIRE(a.finish());
    return a.bytes();
}
Bytes rngWire(SimRng rng) {
    mc2::save::Archive a;
    a(rng);
    REQUIRE(a.finish());
    return a.bytes();
}
} // namespace

TEST_SUITE("SimRng") {
TEST_CASE("fixed stream IDs XOR seeds and independent LCG15 golden vectors") {
    constexpr std::array<std::uint32_t, 8> ids{
        0x41424c52,0x41494445,0x434f4d42,0x454e5652,0x494e4954,0x50415448,0x50485953,0x50494c54};
    constexpr std::array<std::uint32_t, 8> seeds{
        0x53761a2a,0x537d123d,0x517b1b3a,0x577a002a,0x5b7a1f2c,0x42750230,0x427c0f2b,0x427d1a2c};
    // Evaluated independently with arbitrary-width integer arithmetic and a 32-bit mask.
    constexpr std::array<std::array<std::uint16_t, 5>, 8> golden{{
        {2810,10351,5956,11562,12529}, {2945,26595,6489,18957,27885},
        {2713,15622,15239,26583,19594}, {9957,22284,8421,28041,31678},
        {19117,10721,26194,31368,27165}, {14583,32491,16598,20084,29623},
        {17148,30600,2670,31136,9833}, {449,26487,30999,4255,8788}
    }};
    SimRng rng(0x12345678), replay(0x12345678), different(0x12345679);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        CHECK(rng.streams()[i].streamId == ids[i]);
        CHECK(rng.streams()[i].initialSeed == seeds[i]);
        CHECK_FALSE(simStreamTable[i].owner.empty());
        for (auto expected : golden[i]) {
            const auto draw = rng.draw(static_cast<StreamId>(ids[i]));
            REQUIRE(draw);
            CHECK(*draw == expected);
        }
        CHECK(rng.streams()[i].drawCount == 5);
    }
    // A different interleaving cannot move another owner's stream.
    for (int n = 0; n < 5; ++n)
        for (std::size_t i = ids.size(); i-- > 0;) REQUIRE(replay.draw(static_cast<StreamId>(ids[i])));
    CHECK(rng == replay);
    CHECK(rng.stream(StreamId::Combat)->initialSeed != different.stream(StreamId::Combat)->initialSeed);
}

TEST_CASE("frozen wire field order widths and exact continuation after restore") {
    SimRng rng(0x12345678);
    auto wire = rngWire(rng);
    REQUIRE(wire.size() == 232);
    const Bytes prefix{
        0x78,0x56,0x34,0x12, 8,0,0,0,
        0x52,0x4c,0x42,0x41, 1,0,0,0, 1,0,0,0,
        0x2a,0x1a,0x76,0x53, 0x2a,0x1a,0x76,0x53, 0,0,0,0,0,0,0,0
    };
    CHECK(std::equal(prefix.begin(), prefix.end(), wire.begin()));
    auto records = std::vector<RngStream>(rng.streams().begin(), rng.streams().end());
    records[0].state = 0xaabbccddu;
    records[0].drawCount = 0x0102030405060708ull;
    const auto distinctFields = rngWire(0x12345678, records);
    const Bytes stateAndCount{0xdd,0xcc,0xbb,0xaa,8,7,6,5,4,3,2,1};
    CHECK(std::equal(stateAndCount.begin(), stateAndCount.end(), distinctFields.begin() + 24));
    for (int i = 0; i < 7; ++i) REQUIRE(rng.draw(StreamId::Combat));
    REQUIRE(rng.draw(StreamId::ABL));
    SimRng restored(999);
    REQUIRE(mc2::save::decodeRecord(rngWire(rng), restored));
    CHECK(restored == rng);
    for (int i = 0; i < 100; ++i) {
        const auto left = rng.draw(StreamId::Combat), right = restored.draw(StreamId::Combat);
        REQUIRE(left); REQUIRE(right); CHECK(*left == *right);
    }
    CHECK(restored == rng);
}

TEST_CASE("ABL-only reseeds preserve initial seed lifetime draws and saved history") {
    SimRng rng(0x12345678);
    AblReseedState history;
    REQUIRE(rng.draw(StreamId::ABL));
    REQUIRE(rng.draw(StreamId::Combat));
    const auto combat = *rng.stream(StreamId::Combat);
    const auto initial = rng.stream(StreamId::ABL)->initialSeed;
    REQUIRE(rng.reseedAbl(history, 7, 42, 19));
    CHECK(rng.stream(StreamId::ABL)->state == 19);
    CHECK(rng.stream(StreamId::ABL)->drawCount == 1);
    REQUIRE(rng.draw(StreamId::ABL));
    auto automatic = rng.reseedAbl(history, 7, 42, -1);
    REQUIRE(automatic);
    CHECK(automatic->resolvedSeed == 0x0ca8a25a);
    CHECK(automatic->ordinal == 2);
    CHECK(rng.stream(StreamId::ABL)->initialSeed == initial);
    CHECK(rng.stream(StreamId::ABL)->drawCount == 2);
    CHECK(*rng.stream(StreamId::Combat) == combat);
    mc2::save::Archive a;
    a(history);
    REQUIRE(a.finish());
    AblReseedState saved;
    SimRng restored;
    REQUIRE(mc2::save::decodeRecord(a.bytes(), saved));
    REQUIRE(mc2::save::decodeRecord(rngWire(rng), restored));
    CHECK(history == saved);
    auto next = rng.reseedAbl(history, 9, 50, -1), resumed = restored.reseedAbl(saved, 9, 50, -1);
    REQUIRE(next); REQUIRE(resumed); CHECK(*next == *resumed);
    CHECK(restored == rng);
    const auto before = rng;
    const auto beforeHistory = history;
    int diagnostics = 0;
    CHECK_FALSE(rng.reseedAbl(history, 7, 51, -2, [&](auto) { ++diagnostics; }));
    CHECK(diagnostics == 1);
    CHECK(rng == before); CHECK(history == beforeHistory);
}

TEST_CASE("missing duplicate unknown IDs algorithm versions and malformed bytes roll back once") {
    SimRng original(45);
    REQUIRE(original.draw(StreamId::AI));
    const auto records = std::vector<RngStream>(original.streams().begin(), original.streams().end());
    std::vector<Bytes> malformed;
    auto changed = records; changed.pop_back(); malformed.push_back(rngWire(45, changed));
    changed = records; changed[1].streamId = changed[0].streamId; malformed.push_back(rngWire(45, changed));
    changed = records; changed[0].streamId = 0x41505052; malformed.push_back(rngWire(45, changed));
    changed = records; changed[0].algorithmId = 2; malformed.push_back(rngWire(45, changed));
    changed = records; changed[0].algorithmVersion = 2; malformed.push_back(rngWire(45, changed));
    changed = records; changed[0].initialSeed ^= 1; malformed.push_back(rngWire(45, changed));
    auto truncated = rngWire(original); truncated.pop_back(); malformed.push_back(truncated);
    auto trailing = rngWire(original); trailing.push_back(1); malformed.push_back(trailing);
    for (const auto& bytes : malformed) {
        auto destination = original;
        std::vector<std::string> logs;
        CHECK_FALSE(mc2::save::decodeRecord(bytes, destination, [&](auto line) { logs.emplace_back(line); }));
        REQUIRE(logs.size() == 1);
        CHECK(logs[0].starts_with("[SAVE v1] reject reason="));
        CHECK(destination == original);
    }
}

TEST_CASE("draw counter u64 boundary rejects without advancing state") {
    SimRng rng(0);
    auto records = std::vector<RngStream>(rng.streams().begin(), rng.streams().end());
    records[0].drawCount = std::numeric_limits<std::uint64_t>::max() - 1;
    REQUIRE(mc2::save::decodeRecord(rngWire(0, records), rng));
    REQUIRE(rng.draw(StreamId::ABL));
    const auto before = rng;
    int logs = 0;
    auto overflow = rng.draw(StreamId::ABL, [&](auto) { ++logs; });
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error() == RngError::DrawCountOverflow);
    CHECK(logs == 1); CHECK(rng == before);
    CHECK_FALSE(rng.draw(static_cast<StreamId>(0x5241494e), [&](auto) { ++logs; }));
    CHECK(logs == 2); CHECK(rng == before);
}

TEST_CASE("ABL history malformed decode rolls back and capacity rejects without reseeding") {
    AblReseedState destination{1,{{1,7,42,19,19}}};
    const auto original = destination;
    std::vector<AblReseedState> malformed{
        {2,{{1,7,42,19,19}}}, {1,{{2,7,42,19,19}}}, {1,{{1,7,0,19,19}}},
        {1,{{1,7,42,-2,19}}}, {1,{{1,7,42,19,20}}}, {2,{{1,7,42,0,0},{2,7,41,0,0}}}
    };
    for (auto candidate : malformed) {
        mc2::save::Archive a;
        candidate.serialize(a);
        REQUIRE(a.finish());
        int logs = 0;
        CHECK_FALSE(mc2::save::decodeRecord(a.bytes(),destination,[&](auto) { ++logs; }));
        CHECK(logs == 1); CHECK(destination == original);
    }
    AblReseedState full;
    full.reseedOrdinal = mc2::save::maxSequenceLength;
    full.reseedHistory.reserve(mc2::save::maxSequenceLength);
    for (std::uint64_t i = 1; i <= mc2::save::maxSequenceLength; ++i)
        full.reseedHistory.push_back({i,7,42,0,0});
    SimRng rng(19);
    const auto before = rng;
    int logs = 0;
    auto rejected = rng.reseedAbl(full,7,42,-1,[&](auto) { ++logs; });
    REQUIRE_FALSE(rejected); CHECK(rejected.error() == RngError::HistoryCapacity);
    CHECK(logs == 1); CHECK(rng == before);
    CHECK(full.reseedOrdinal == mc2::save::maxSequenceLength);
    CHECK(full.reseedHistory.size() == mc2::save::maxSequenceLength);
}
} // TEST_SUITE
