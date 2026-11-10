// Keyed per-node hierarchy evaluation for TG_MultiShape::TransformMultiShape_BuildRecipe. A node's local matrix is a pure
// function of its sampled inputs and its parent's local matrix, and its world/inverse a pure function of the local matrix and
// the shape origin. evalNode keeps the inputs it last evaluated and redoes only what an edit invalidated, so the result is
// bit for bit the full rebuild (tests/unit/test_msl_node_walk.cpp). Engine-free: the walk and the test both call it.
#pragma once
#include "math/mc2_math.h"
#include "math/mc2_xform.h"
#include "sim_pose_kernel.h"
#include <cstdint>
#include <cstring>

namespace mc2::msl {

struct NodeInput {
    bool animated = false;
    glm::quat base = mc2::math::quatXYZW(0.0f, 0.0f, 0.0f, 1.0f);
    glm::quat animation = mc2::math::quatXYZW(0.0f, 0.0f, 0.0f, 1.0f);
    glm::vec3 translation{};
};

// What the matrices of a node were last evaluated from. Generations are shape-local counters; valid=false forces a rebuild.
struct NodeKey {
    NodeInput input;
    std::uint32_t parentGen = 0;
    std::uint32_t localGen = 0;
    std::uint32_t originGen = 0;
    bool valid = false;
};

inline bool sameInput(const NodeInput& a, const NodeInput& b) {
    return a.animated == b.animated && std::memcmp(&a.base, &b.base, sizeof(a.base)) == 0 &&
           std::memcmp(&a.animation, &b.animation, sizeof(a.animation)) == 0 &&
           std::memcmp(&a.translation, &b.translation, sizeof(a.translation)) == 0;
}

// parentLocal null for a root. Returns true when the local matrix was rebuilt (children must then rebuild theirs).
inline bool evalNode(const NodeInput& in, const glm::mat4* parentLocal, std::uint32_t parentGen, const glm::mat4& origin,
                     std::uint32_t originGen, std::uint32_t& genCounter, NodeKey& key, glm::mat4& local, glm::mat4& world,
                     glm::mat4& inverse) {
    const bool reuse = key.valid && key.parentGen == parentGen && sameInput(key.input, in);
    if (!reuse) {
        mc2::sim::PoseNodeInput p;
        p.parent = parentLocal ? 0 : -1;
        p.animated = in.animated;
        p.base = in.base;
        p.animation = in.animation;
        p.translation = in.translation;
        local = parentLocal ? mc2::sim::poseLocal(p) * *parentLocal : mc2::sim::poseLocal(p);
        key.input = in;
        key.parentGen = parentGen;
        key.localGen = ++genCounter;
        key.valid = true;
    }
    if (!reuse || key.originGen != originGen) {
        world = local * origin;
        inverse = glm::inverse(world);
        key.originGen = originGen;
    }
    return !reuse;
}

} // namespace mc2::msl
