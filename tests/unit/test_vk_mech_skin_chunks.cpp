// Mech skin job chunking: every vertex of every node is covered exactly once with the node's bone base; the over-limit fallback keeps the nodes.
#include "doctest.h"

#include "vk_mech_skin_chunks.h"

#include <map>

using namespace mc2vk;

// Each dst vertex of each node is written by exactly one job, which reads the matching src vertex with the node's bone base.
static void checkCover(const std::vector<MechSkinJob>& nodes, const std::vector<MechSkinJob>& jobs) {
    std::map<uint32_t, const MechSkinJob*> owner;
    uint32_t written = 0;
    for (const MechSkinJob& j : jobs) {
        CHECK(j.count > 0);
        for (uint32_t v = 0; v < j.count; ++v) {
            CHECK(owner.emplace(j.dstFirst + v, &j).second);
            ++written;
        }
    }
    uint32_t need = 0;
    for (const MechSkinJob& n : nodes) {
        need += n.count;
        for (uint32_t v = 0; v < n.count; ++v) {
            const auto it = owner.find(n.dstFirst + v);
            REQUIRE(it != owner.end());
            const MechSkinJob& j = *it->second;
            CHECK(j.srcFirst + (n.dstFirst + v - j.dstFirst) == n.srcFirst + v);
            CHECK(j.boneBase == n.boneBase);
        }
    }
    CHECK(written == need);
}

TEST_CASE("skin chunks: one big node plus many small ones") {
    std::vector<MechSkinJob> nodes;
    uint32_t dst = 0;
    nodes.push_back({1000, 12913, dst, 7}); dst += 12913;
    for (uint32_t i = 0; i < 40; ++i) { nodes.push_back({20000 + i * 36, 36, dst, 100 + i}); dst += 36; }
    const auto jobs = chunkSkinJobs(nodes);
    CHECK(jobs.size() == 51 + 40);   // ceil(12913 / 256) = 51
    CHECK(maxSkinJobCount(jobs) == 256);
    checkCover(nodes, jobs);
}

TEST_CASE("skin chunks: zero count node and exact multiples") {
    std::vector<MechSkinJob> nodes = {{0, 0, 0, 1}, {10, 64, 0, 2}, {100, 256, 64, 3}, {400, 512, 320, 4}, {1000, 257, 832, 5}};
    const auto jobs = chunkSkinJobs(nodes);
    CHECK(jobs.size() == 1 + 1 + 2 + 2);
    CHECK(maxSkinJobCount(jobs) == 256);
    checkCover(nodes, jobs);
}

TEST_CASE("skin chunks: over the job limit falls back to the unchunked nodes") {
    std::vector<MechSkinJob> nodes = {{0, 1000, 0, 1}, {1000, 500, 1000, 2}};
    const auto jobs = chunkSkinJobs(nodes, 256, 5);   // chunked would be 4 + 2 = 6 > 5
    REQUIRE(jobs.size() == 2);
    CHECK(jobs[0].count == 1000);
    CHECK(jobs[1].count == 500);
    CHECK(chunkSkinJobs(nodes, 256, 6).size() == 6);
    CHECK(chunkSkinJobs({}).empty());
}
