// Immutable, sampled TG pose inputs. No engine objects or animation pointers
// cross the fork/join barrier; publication stays at the original call site.
#pragma once
#include "math/mc2_math.h"
#include "math/mc2_xform.h"
#include <span>

namespace mc2::sim {
struct PoseMatrices {
    glm::mat4 local, world, inverse;
};
struct PoseNodeInput {
    int parent = -1;
    bool current = false;
    bool animated = false;
    glm::quat base = mc2::math::quatXYZW(0.0f, 0.0f, 0.0f, 1.0f), animation = mc2::math::quatXYZW(0.0f, 0.0f, 0.0f, 1.0f);
    glm::vec3 translation{};
    PoseMatrices cached;
};

// Preserve the legacy root/child distinction and multiplication grouping.
inline glm::mat4 poseLocal(const PoseNodeInput& n) {
    glm::mat4 local;
    if (n.animated) {
        auto rotation = n.animation;
        if (!(n.base.w == 1.0f && n.base.x == 0.0f && n.base.y == 0.0f && n.base.z == 0.0f)) {
            rotation = mc2::math::composeQuat(rotation, n.base);
            rotation = glm::normalize(rotation);
        }
        mc2::math::buildRotation(local, rotation);
    } else if (n.parent < 0) {
        mc2::math::buildRotation(local, mc2::math::Euler(0.0f, 0.0f, 0.0f));
    } else {
        mc2::math::buildRotation(local, n.base);
    }
    mc2::math::buildTranslation(local, n.translation);
    return local;
}

inline glm::mat4 poseChain(std::span<const PoseNodeInput> nodes, int index) {
    // Each job walks its immutable ancestry. Cached parents retain precisely
    // the matrices the old calcedThisFrame test would have reused.
    if (nodes[index].current) return nodes[index].cached.local;
    const auto& node = nodes[index];
    auto local = poseLocal(node);
    if (node.parent >= 0)
        local = local * poseChain(nodes, node.parent);
    return local;
}

inline PoseMatrices poseNode(std::span<const PoseNodeInput> nodes,
                            const glm::mat4& origin, int index) {
    if (nodes[index].current) return nodes[index].cached;
    PoseMatrices result;
    result.local = poseChain(nodes, index);
    result.world = result.local * origin;
    result.inverse = glm::inverse(result.world);
    return result;
}
} // namespace mc2::sim
