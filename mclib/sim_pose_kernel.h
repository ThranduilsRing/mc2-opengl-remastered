// Immutable, sampled TG pose inputs. No engine objects or animation pointers
// reach the kernel; publication stays at the original call site.
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
    bool inverseRequired = true; // false leaves the output inverse untouched
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

struct PoseEvaluationObserver {
    void operator()(int, bool) const {}
};

// Scratch and results belong to this evaluation. The observer is compiled out
// in production; tests count the actual local/inverse evaluations through it.
template<class Observer = PoseEvaluationObserver>
inline void poseNodes(std::span<const PoseNodeInput> nodes,
                      const glm::mat4& origin, std::span<PoseMatrices> results,
                      std::span<unsigned char> evaluated, Observer observe = {}) {
    for (size_t i = 0; i < nodes.size(); ++i) evaluated[i] = 0;
    const auto evaluate = [&](auto&& self, int index) -> void {
        if (evaluated[index]) return;
        const auto& node = nodes[index];
        auto& result = results[index];
        if (node.current) {
            result = node.cached;
        } else {
            if (node.parent >= 0) self(self, node.parent);
            result.local = poseLocal(node);
            observe(index, false);
            // Preserve local * (parent * (...)); never reassociate with origin.
            if (node.parent >= 0) result.local = result.local * results[node.parent].local;
            result.world = result.local * origin;
            if (node.inverseRequired) {
                result.inverse = glm::inverse(result.world);
                observe(index, true);
            }
        }
        evaluated[index] = 1;
    };
    // Node storage need not be topologically sorted (including imported shapes).
    for (size_t i = 0; i < nodes.size(); ++i) evaluate(evaluate, static_cast<int>(i));
}
} // namespace mc2::sim
