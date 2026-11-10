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

// ---- Order cache + keyed walk vs a cache-free reference, on a shape whose node set changes ----
namespace {
struct CachedShape {
    std::vector<int> parent;                // parent index per node; a parent may have a HIGHER index than its child
    std::vector<NodeInput> input;
    std::vector<glm::mat4> local, world, inverse;
    std::vector<NodeKey> key;
    std::vector<int> order, snapshot;
    std::uint32_t genCounter = 0, originGen = 0;
    glm::mat4 lastOrigin{1.0f};
    bool originValid = false;
    int orderRebuilds = 0;

    void setNodes(const std::vector<int>& p, const std::vector<NodeInput>& in) {   // node set changed (compaction / reload)
        parent = p; input = in;
        local.assign(p.size(), glm::mat4(1.0f)); world = local; inverse = local;
        key.assign(p.size(), NodeKey{});
    }
    // Same glue as TG_MultiShape::TransformMultiShape_BuildRecipe: refreshWalkOrder, origin generation, evalNode in order.
    void walk(const glm::mat4& origin) {
        const long n = static_cast<long>(parent.size());
        if (refreshWalkOrder(order, snapshot, n, [&](long i) { return parent[i]; }, 256)) ++orderRebuilds;
        if (!originValid || std::memcmp(&lastOrigin, &origin, sizeof(origin)) != 0) { lastOrigin = origin; originValid = true; ++originGen; }
        for (const int i : order) {
            const int p = parent[i];
            evalNode(input[i], p >= 0 ? &local[p] : nullptr, p >= 0 ? key[p].localGen : 0u, origin, originGen, genCounter,
                     key[i], local[i], world[i], inverse[i]);
        }
    }
};

// Cache-free: recompute every node from raw inputs by walking up the parent chain.
glm::mat4 refLocal(const std::vector<int>& parent, const std::vector<NodeInput>& input, int i) {
    mc2::sim::PoseNodeInput p;
    p.parent = parent[i] >= 0 ? 0 : -1;
    p.animated = input[i].animated; p.base = input[i].base; p.animation = input[i].animation; p.translation = input[i].translation;
    return parent[i] >= 0 ? mc2::sim::poseLocal(p) * refLocal(parent, input, parent[i]) : mc2::sim::poseLocal(p);
}

NodeInput randomInput(std::mt19937& rng) {
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    NodeInput in;
    in.animated = (rng() & 1u) != 0;
    in.base = glm::normalize(glm::quat(unit(rng) + 2.0f, unit(rng), unit(rng), unit(rng)));
    in.animation = glm::normalize(glm::quat(unit(rng) + 2.0f, unit(rng), unit(rng), unit(rng)));
    in.translation = glm::vec3(unit(rng), unit(rng), unit(rng));
    return in;
}

void checkAgainstReference(const CachedShape& s, const glm::mat4& origin) {
    for (std::size_t i = 0; i < s.parent.size(); ++i) {
        const glm::mat4 l = refLocal(s.parent, s.input, static_cast<int>(i));
        const glm::mat4 w = l * origin;
        REQUIRE(bitEqual(s.local[i], l));
        REQUIRE(bitEqual(s.world[i], w));
        REQUIRE(bitEqual(s.inverse[i], glm::inverse(w)));
    }
}
}

TEST_CASE("msl_node_walk/cached order walk equals a cache-free reference on a multi-node shape whose node set changes")
{
    std::mt19937 rng(777u);
    CachedShape s;
    glm::mat4 origin(1.0f);
    // Parents after children in index order in the second topology: the cached order must still put parents first.
    const std::vector<std::vector<int>> topologies{
        {-1, 0, 1, 1, 0, 4, 5, 4, -1, 8, 9},
        {7, 3, 3, -1, 3, 9, 4, 0, -1, -1, 8, 3},   // node set changes size and every link
        {-1, 0, 0, 1, 2},
        {-1, 0, 1, 2, 3, 4, 5, 6},                 // a chain
    };
    for (const auto& topo : topologies) {
        std::vector<NodeInput> in(topo.size());
        for (auto& x : in) x = randomInput(rng);
        s.setNodes(topo, in);
        const int rebuildsBefore = s.orderRebuilds;
        for (int step = 0; step < 150; ++step) {
            if (rng() % 3 == 0) s.input[rng() % topo.size()] = randomInput(rng);
            if (rng() % 5 == 0) {
                std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
                mc2::math::buildRotation(origin, glm::normalize(glm::quat(unit(rng) + 2.0f, unit(rng), unit(rng), unit(rng))));
                mc2::math::buildTranslation(origin, glm::vec3(unit(rng), unit(rng), unit(rng)));
            }
            s.walk(origin);
            checkAgainstReference(s, origin);
        }
        CHECK(s.orderRebuilds == rebuildsBefore + 1);   // one rebuild per node-set change, none per frame
        std::vector<char> seen(topo.size(), 0);
        for (const int i : s.order) { if (topo[i] >= 0) CHECK(seen[topo[i]]); seen[i] = 1; }
        CHECK(s.order.size() == topo.size());
    }
}

TEST_CASE("msl_node_walk/a parent link change at the same node count rebuilds the order and the matrices")
{
    std::mt19937 rng(4242u);
    CachedShape s;
    const glm::mat4 origin(1.0f);
    std::vector<int> topo{-1, 0, 1, 2};
    std::vector<NodeInput> in(topo.size());
    for (auto& x : in) x = randomInput(rng);
    s.setNodes(topo, in);
    s.walk(origin);
    checkAgainstReference(s, origin);
    s.parent = {-1, 0, 0, 1};                        // relink in place; keys are stale-valid, the parent gen check must catch it
    s.walk(origin);
    CHECK(s.orderRebuilds == 2);
    checkAgainstReference(s, origin);
}
