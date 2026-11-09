// Immutable, sampled TG pose inputs. No engine objects or animation pointers
// reach the kernel; publication stays at the original call site.
#pragma once
#include "math/mc2_math.h"
#include "math/mc2_xform.h"
#include <cstdint>
#include <span>
#include <vector>

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

// One shape's captured pose: TG_MultiShape::CaptureSimPose fills it on the sim thread, evaluate() is pure (any thread,
// alloc-free once sized), TG_MultiShape::PublishSimPose writes the matrices back on the sim thread.
struct PoseJob {
    std::vector<PoseNodeInput> inputs;
    std::vector<PoseMatrices> outputs;
    std::vector<unsigned char> evaluated;
    glm::mat4 origin{1.0f};
    int32_t revision = 0;
    bool done = false;
    void evaluate() {
        if (done) return;
        poseNodes(inputs, origin, outputs, evaluated);
        done = true;
    }
};

// Every input of a published mech simulation pose. An equal key reuses the published pose; any change (shape or
// binding, gesture, frame, torso, root, status, arm loss), a pose cleared by load/reset (hadPose false) or any
// imported clip (which advances its own clock) recomputes it.
struct SimPoseKey {
    std::uint64_t shapeId = 0, bindRevision = 0;
    const void* type = nullptr;
    long gesture = 0, status = 0;
    float frame = 0.0f, torso = 0.0f, rotation = 0.0f;
    glm::vec3 position{};
    bool leftArmOff = false, rightArmOff = false;
    bool operator==(const SimPoseKey&) const = default;
};
inline bool reuseSimPose(bool hadPose, bool keyValid, const SimPoseKey& now, const SimPoseKey& published,
                         bool anyImportedAnim) {
    return hadPose && keyValid && now == published && !anyImportedAnim;
}
} // namespace mc2::sim
