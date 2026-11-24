#pragma once
// Imported clips omit BT's GrounderIK pass. Solve the legs against their soles;
// the pelvis/torso and the simulation's position remain exactly as authored.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "math/mc2_xform.h"

namespace mc2ground {
// These are Assimp/glTF joint globals, before the MC2 axis conjugation. Keep
// their column convention explicit when crossing the engine's row adapters.
inline glm::mat4 columnRotation(const glm::quat& q) {
    return glm::transpose(mc2::math::rotationMatrix(q));
}
inline glm::quat columnQuaternion(const glm::mat4& m) {
    return mc2::math::quatFromRotation(glm::transpose(m));
}
struct Point { int bone; glm::vec3 local; };
struct Leg {
    int thigh = -1, calf = -1, foot = -1;
    glm::mat4 restFoot{1.0f};
    std::vector<Point> sole;
    std::vector<int> children;
};
struct Rig { std::array<Leg, 2> legs; float floor = 0.0f; };

inline auto between(glm::vec3 a, glm::vec3 b) {
    a = glm::normalize(a); b = glm::normalize(b);
    const float d = glm::dot(a, b);
    if (d < -0.9999f) {
        glm::vec3 axis = glm::cross(a, glm::vec3(1, 0, 0));
        if (glm::dot(axis, axis) < 1e-8f) axis = glm::cross(a, glm::vec3(0, 1, 0));
        return glm::angleAxis(3.14159265f, glm::normalize(axis));
    }
    const glm::vec3 v = glm::cross(a, b);
    return glm::normalize(mc2::math::quatXYZW(v.x, v.y, v.z, 1.0f + d));
}

inline glm::mat4 moveBone(const glm::mat4& old, glm::vec3 pos, glm::quat rotation) {
    glm::mat4 out = columnRotation(rotation) * old;
    out[3] = glm::vec4(pos, 1.0f);
    return out;
}

inline void solve(const Rig& rig, std::vector<glm::mat4>& pose) {
    if (rig.legs[0].sole.empty() || rig.legs[1].sole.empty()) return;
    const auto original = pose;
    // Swing height is measured at the authored sole, not the ankle: a toe-down
    // (pushed-off) foot has a high ankle but a low toe, and lifting the ankle by
    // its own height and then the sole again folded the rear leg to hip height.
    std::array<float, 2> authoredSole{1e30f, 1e30f};
    for (int side = 0; side < 2; ++side)
        for (const Point& p : rig.legs[side].sole)
            authoredSole[side] = std::min(authoredSole[side],
                                          (original[p.bone] * glm::vec4(p.local, 1.0f)).y);
    const float lowSole = std::min(authoredSole[0], authoredSole[1]);
    for (int side = 0; side < 2; ++side) {
        const Leg& leg = rig.legs[side];
        const glm::mat4& foot = original[leg.foot];
        const glm::vec3 hip(original[leg.thigh][3]), knee(original[leg.calf][3]), ankle(foot[3]);
        const float upper = glm::length(knee - hip), lower = glm::length(ankle - knee);
        if (upper < 1e-6f || lower < 1e-6f) continue;
        const float swing = std::max(0.0f, authoredSole[side] - lowSole);
        // Preserve heading and the rig's foot axes; ground contact removes only tilt.
        const glm::mat4 delta = foot * glm::inverse(leg.restFoot);
        const float yaw = std::atan2(delta[2].x, delta[2].z);
        const glm::quat level = glm::angleAxis(yaw, glm::vec3(0, 1, 0)) *
                                columnQuaternion(leg.restFoot);
        const float band = std::max(1e-5f, (upper + lower) * 0.1f);
        const float blend = std::clamp(swing / band, 0.0f, 1.0f);
        const glm::quat rot = glm::slerp(level, columnQuaternion(foot), blend);
        glm::mat4 placed = columnRotation(rot);
        const glm::mat4 invFoot = glm::inverse(foot);
        float soleY = 1e30f;
        for (const Point& p : leg.sole) {
            const glm::vec4 local = invFoot * original[p.bone] * glm::vec4(p.local, 1.0f);
            soleY = std::min(soleY, (placed * local).y);
        }
        glm::vec3 target(ankle.x, rig.floor + swing - soleY, ankle.z);
        // Keep a reachable ankle on the contact plane, shortening only fore/aft reach.
        const float reach = (upper + lower) * 0.99999f;
        const float dy = target.y - hip.y;
        const float maxXZ = std::sqrt(std::max(0.0f, reach * reach - dy * dy));
        glm::vec3 xz(target.x - hip.x, 0, target.z - hip.z);
        const float xzLen = glm::length(xz);
        if (xzLen > maxXZ && xzLen > 1e-6f) {
            xz *= maxXZ / xzLen; target.x = hip.x + xz.x; target.z = hip.z + xz.z;
        }
        const glm::vec3 ray = target - hip;
        const float distance = glm::length(ray);
        if (distance < 1e-6f || distance > upper + lower) continue;
        const glm::vec3 direction = ray / distance;
        // Bend inside the authored leg plane on the authored side; the knee's offset
        // from the NEW hip-ankle line changes sign when the target moves far, which
        // flipped a reverse knee forward.
        const glm::vec3 plane = glm::cross(knee - hip, ankle - knee);
        glm::vec3 bend = glm::cross(plane, direction);
        if (glm::dot(glm::cross(plane, ankle - hip), knee - hip) < 0.0f) bend = -bend;
        if (glm::dot(plane, plane) < 1e-12f || glm::dot(bend, bend) < 1e-12f) continue;
        bend = glm::normalize(bend);
        const float along = (upper * upper - lower * lower + distance * distance) / (2 * distance);
        const glm::vec3 newKnee = hip + direction * along +
            bend * std::sqrt(std::max(0.0f, upper * upper - along * along));
        pose[leg.thigh] = moveBone(original[leg.thigh], hip, between(knee - hip, newKnee - hip));
        pose[leg.calf] = moveBone(original[leg.calf], newKnee, between(ankle - knee, target - newKnee));
        placed[3] = glm::vec4(target, 1.0f);
        pose[leg.foot] = placed;
        const glm::mat4 correction = placed * invFoot;
        for (int child : leg.children) pose[child] = correction * original[child];
    }
}
} // namespace mc2ground
