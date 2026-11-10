// MSL-NODE-WALK-1: the keyed per-node hierarchy evaluation (mclib/msl_node_walk.h) used by
// TG_MultiShape::TransformMultiShape_BuildRecipe. Rebuilding only the nodes an edit touched must equal a full rebuild bit for bit.
#include "doctest.h"
#include "msl_node_walk.h"
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace {
using namespace mc2::msl;

struct Walk {
    std::vector<int> parent;                // parents precede children
    std::vector<NodeInput> input;
    std::vector<glm::mat4> local, world, inverse;
    std::vector<NodeKey> key;
    std::uint32_t genCounter = 0;
    std::uint32_t originGen = 0;
    std::uint32_t evals = 0;
    explicit Walk(const std::vector<int>& p)
        : parent(p), input(p.size()), local(p.size(), glm::mat4(1.0f)), world(p.size(), glm::mat4(1.0f)),
          inverse(p.size(), glm::mat4(1.0f)), key(p.size()) {}

    void run(const glm::mat4& origin) {
        for (std::size_t i = 0; i < parent.size(); ++i) {
            const int p = parent[i];
            const std::uint32_t before = key[i].localGen;
            evalNode(input[i], p >= 0 ? &local[p] : nullptr, p >= 0 ? key[p].localGen : 0u, origin, originGen, genCounter,
                     key[i], local[i], world[i], inverse[i]);
            if (key[i].localGen != before) ++evals;
        }
    }
    void invalidate() { for (auto& k : key) k.valid = false; }
};

bool bitEqual(const glm::mat4& a, const glm::mat4& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
}

TEST_CASE("msl_node_walk/changed-node rebuild equals a full rebuild bit for bit over random edits")
{
    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    const std::vector<int> topo{-1, 0, 1, 1, 0, 4, 5, 4, -1, 8, 9};
    Walk inc(topo), full(topo);
    glm::mat4 origin(1.0f);
    for (int step = 0; step < 400; ++step) {
        const int edits = static_cast<int>(rng() % 4);
        for (int e = 0; e < edits; ++e) {
            const std::size_t n = rng() % topo.size();
            NodeInput& in = inc.input[n];
            switch (rng() % 4) {
            case 0: in.animated = !in.animated; break;
            case 1: in.animation = glm::normalize(glm::quat(unit(rng) + 2.0f, unit(rng), unit(rng), unit(rng))); break;
            case 2: in.base = glm::normalize(glm::quat(unit(rng) + 2.0f, unit(rng), unit(rng), unit(rng))); break;
            default: in.translation = glm::vec3(unit(rng), unit(rng), unit(rng)); break;
            }
        }
        if (rng() % 3 == 0) {
            mc2::math::buildRotation(origin, glm::normalize(glm::quat(unit(rng) + 2.0f, unit(rng), unit(rng), unit(rng))));
            mc2::math::buildTranslation(origin, glm::vec3(unit(rng), unit(rng), unit(rng)));
            ++inc.originGen;
            ++full.originGen;
        }
        full.input = inc.input;
        full.invalidate();
        inc.run(origin);
        full.run(origin);
        for (std::size_t n = 0; n < topo.size(); ++n) {
            REQUIRE(bitEqual(inc.local[n], full.local[n]));
            REQUIRE(bitEqual(inc.world[n], full.world[n]));
            REQUIRE(bitEqual(inc.inverse[n], full.inverse[n]));
        }
    }
}

TEST_CASE("msl_node_walk/an unchanged pose rebuilds nothing; an edit rebuilds only the node and its descendants")
{
    const std::vector<int> topo{-1, 0, 1, 0, 3};
    Walk w(topo);
    const glm::mat4 origin(1.0f);
    w.run(origin);
    CHECK(w.evals == topo.size());
    w.evals = 0;
    w.run(origin);
    CHECK(w.evals == 0);
    w.input[1].translation = glm::vec3(1.0f, 2.0f, 3.0f);
    w.run(origin);
    CHECK(w.evals == 2); // node 1 and its child 2; the sibling chain 3,4 is untouched
}
