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

// legLift[side] (clip units, optional): how far that leg's sole rides above the shared floor, so each foot rests on
// the surface under itself (FOOT-GROUNDING-1). Limited to what the leg reaches.
inline void solve(const Rig& rig, std::vector<glm::mat4>& pose, const float* legLift = nullptr,
                  const std::array<glm::vec3, 2>* planted = nullptr) {
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
        // Keep a reachable ankle on the contact plane, shortening only fore/aft reach.
        const float reach = (upper + lower) * 0.99999f;
        const float soleHeight = rig.floor + swing - soleY;
        const float lift = legLift ? std::clamp(legLift[side], -reach - (soleHeight - hip.y), reach - (soleHeight - hip.y)) : 0.0f;
        glm::vec3 target(ankle.x, soleHeight + lift, ankle.z);
        if (planted) { target.x = (*planted)[side].x; target.z = (*planted)[side].z; }
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

// Clip-phase targets, shared by every instance. No previous-frame foot positions.
// Build from the *grounded* pose: reach limiting can move an authored ankle.
struct PlantTrack {
    std::vector<std::array<glm::vec3, 2>> feet;
    std::vector<std::array<bool, 2>> contact;
    std::array<glm::vec3, 2> sample(float phase) const {
        const float f = (phase - std::floor(phase)) * feet.size();
        const size_t i = static_cast<size_t>(f) % feet.size(), j = (i + 1) % feet.size();
        std::array<glm::vec3, 2> out;
        for (int side = 0; side < 2; ++side) out[side] = glm::mix(feet[i][side], feet[j][side], f - std::floor(f));
        return out;
    }
};

inline PlantTrack buildPlants(const Rig& rig, const std::vector<std::vector<glm::mat4>>& poses,
                              std::vector<float>& distance, bool reverse) {
    PlantTrack track;
    if (poses.size() < 3 || poses.size() != distance.size() ||
        rig.legs[0].sole.empty() || rig.legs[1].sole.empty()) return track;
    const int n = static_cast<int>(poses.size()) - 1;
    track.feet.resize(n); track.contact.resize(n);
    std::vector<std::array<glm::vec3, 2>> original(n);
    std::vector<std::array<float, 2>> height(n);
    const float direction = reverse ? -1.0f : 1.0f;
    for (int f = 0; f < n; ++f) {
        auto pose = poses[f]; solve(rig, pose);
        for (int side = 0; side < 2; ++side) {
            original[f][side] = glm::vec3(pose[rig.legs[side].foot][3]);
            height[f][side] = 1e30f;
            for (const auto& p : rig.legs[side].sole)
                height[f][side] = std::min(height[f][side], (poses[f][p.bone] * glm::vec4(p.local, 1)).y);
        }
    }
    for (int side = 0; side < 2; ++side) {
        const auto& leg = rig.legs[side];
        const float band = glm::length(glm::vec3(poses[0][leg.thigh][3]) - glm::vec3(poses[0][leg.foot][3])) * .02f;
        // Low sole plus backward velocity identifies stance, including reverse/limp.
        for (int f = 0; f < n; ++f) {
            const int next = (f + 1) % n;
            track.contact[f][side] = height[f][side] <= height[f][1 - side] + band &&
                direction * (poses[next][leg.foot][3].z - poses[f][leg.foot][3].z) < 0;
        }
    }
    // The existing curve used ungrounded ankles; match the feet that actually draw.
    const auto authoredDistance = distance;
    distance[0] = 0;
    for (int f = 0; f < n; ++f) {
        float travel = 0; int contacts = 0;
        for (int side = 0; side < 2; ++side)
            if (track.contact[f][side] && track.contact[(f + 1) % n][side]) {
                travel += std::max(0.0f, direction * (original[f][side].z - original[(f + 1) % n][side].z));
                ++contacts;
            }
        distance[f + 1] = distance[f] + std::max(1e-5f, contacts ? travel / contacts : authoredDistance[f + 1] - authoredDistance[f]);
    }
    for (int side = 0; side < 2; ++side) {
        std::vector<glm::vec3> correction(3 * n);
        for (int f = 0; f < 3 * n;) {
            if (!track.contact[f % n][side]) { ++f; continue; }
            const int start = f;
            while (f < 3 * n && track.contact[f % n][side]) ++f;
            const int anchor = (start + f - 1) / 2;
            const auto travel = [&](int frame) { return distance[frame % n] + (frame / n) * distance.back(); };
            for (int k = start; k < f; ++k) {
                auto target = original[anchor % n][side];
                target.z += direction * (travel(anchor) - travel(k));
                correction[k] = target - original[k % n][side];
            }
        }
        for (int f = n; f < 2 * n; ++f) {
            auto delta = correction[f];
            if (!track.contact[f % n][side]) {
                int before = f, after = f;
                while (before > 0 && !track.contact[before % n][side]) --before;
                while (after < 3 * n - 1 && !track.contact[after % n][side]) ++after;
                const float t = after > before ? static_cast<float>(f - before) / static_cast<float>(after - before) : 0;
                delta = glm::mix(correction[before], correction[after], t * t * (3 - 2 * t));
            }
            track.feet[f % n][side] = original[f % n][side] + delta;
        }
    }
    return track;
}
} // namespace mc2ground
