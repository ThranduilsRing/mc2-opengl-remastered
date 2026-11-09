#include "doctest.h"
#include "../../GameOS/gameos/light_feed_sync.h"
#include <cstring>
#include <random>
#include <vector>
using namespace mc2vk;
using mc2::vkLights::RowWrite;
namespace {
// The vkLights::Packet row/generation/journal model (mclib/vk_object_lights.cpp).
struct FakePacket {
    std::vector<uint32_t> rows;
    std::vector<uint64_t> generations;
    std::vector<RowWrite> journal;
    uint64_t revision = 0, journalBase = 0;
    uint32_t count = 0;
    static constexpr size_t kJournalCap = 1024;
    void start(uint64_t& nextGen, size_t n) {
        rows.assign(n, 0);
        revision = ++nextGen;
        generations.assign(n, revision);
        journal.clear();
        journalBase = revision;
        count = 0;
    }
    void write(uint64_t& nextGen, uint32_t row, uint32_t value) {
        if (rows[row] == value) return;  // unchanged content keeps its generation
        rows[row] = value;
        const uint64_t gen = ++nextGen;
        generations[row] = revision = gen;
        if (journal.size() >= kJournalCap) {
            const size_t drop = kJournalCap / 2;
            journalBase = journal[drop - 1].gen;
            journal.erase(journal.begin(), journal.begin() + drop);
        }
        journal.push_back({gen, row});
    }
};
struct FakeSlot { std::vector<uint32_t> dst; LightFeedSyncState sync; };
} // namespace

TEST_CASE("Light feed slot sync (revision fast exit + journal) equals the source rows") {
    constexpr size_t kRows = 600;
    std::mt19937 rng(4242);
    uint64_t nextGen = 0;
    FakePacket packet;
    packet.start(nextGen, kRows);
    FakeSlot slots[3];
    for (auto& s : slots) s.dst.assign(kRows, 0xDEADu);
    LightFeedSyncStats stats;
    for (int frame = 0; frame < 6000; ++frame) {
        const int mode = int(rng() % 100);
        if (mode == 0) {
            packet.start(nextGen, kRows);           // new lineage
        } else if (mode < 3) {
            for (int i = 0; i < 1500; ++i)          // overflow the journal cap
                packet.write(nextGen, uint32_t(rng() % kRows), rng() % 7);
        } else if (mode < 50) {
            for (int i = int(rng() % 12); i > 0; --i)
                packet.write(nextGen, uint32_t(rng() % kRows), rng() % 7);
        } // else: an idle frame (no content change)
        if (rng() % 5 == 0) packet.count = uint32_t(rng() % kRows);
        if (rng() % 200 == 0)
            for (auto& s : slots) s.sync.invalidate();   // buffer reallocation (stale dst)
        FakeSlot& slot = slots[frame % 3];
        const std::span<const uint64_t> gens(packet.generations.data(), packet.count);
        SyncLightFeedRows(slot.sync, gens, std::span<const RowWrite>(packet.journal), packet.journalBase,
                          packet.revision,
                          [&](size_t first, size_t n) {
                              REQUIRE(first + n <= packet.count);
                              std::memcpy(slot.dst.data() + first, packet.rows.data() + first, n * sizeof(uint32_t));
                          }, &stats);
        REQUIRE(0 == std::memcmp(slot.dst.data(), packet.rows.data(), packet.count * sizeof(uint32_t)));
    }
    CHECK(stats.fastExits > 0);
    CHECK(stats.journalSyncs > stats.fullScans);
    CHECK(stats.fullScans > 0);
}

TEST_CASE("Light feed slot sync full-scans an older feed than its last sync") {
    uint64_t nextGen = 0;
    FakePacket older;
    older.start(nextGen, 8);
    older.count = 8;
    older.write(nextGen, 3, 5);
    FakePacket newer = older;                  // detached copy, then edited
    newer.write(nextGen, 3, 6);
    FakeSlot slot;
    slot.dst.assign(8, 0xDEADu);
    auto sync = [&](const FakePacket& p) {
        SyncLightFeedRows(slot.sync, std::span<const uint64_t>(p.generations.data(), p.count),
                          std::span<const RowWrite>(p.journal), p.journalBase, p.revision,
                          [&](size_t first, size_t n) {
                              std::memcpy(slot.dst.data() + first, p.rows.data() + first, n * sizeof(uint32_t));
                          });
        return 0 == std::memcmp(slot.dst.data(), p.rows.data(), p.count * sizeof(uint32_t));
    };
    CHECK(sync(newer));
    CHECK(sync(older));
    CHECK(sync(newer));
}
