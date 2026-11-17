#pragma once
#include <glm/glm.hpp>
#include <cmath>

namespace mc2 {
struct AngularSphere {
    glm::vec3 position{};
    float heightDelta = 0, radius = 0;
    bool valid = false;
};
struct LastAngularTest : AngularSphere {
    bool outside = false;
    glm::vec3 objectCenter{};
};
inline thread_local LastAngularTest lastAngularTest;

// One out-of-line instantiation for both the prefilter and appearance visits.
template<class Frame>
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
inline bool angularSphereOutside(const glm::vec3& position, float heightDelta, float radius,
    const glm::vec3& cameraOrigin, Frame& frame, float vClipConstant, float hClipConstant)
{
    glm::vec3 cameraPos;
    cameraPos.x = -cameraOrigin.x;
    cameraPos.y = cameraOrigin.z;
    cameraPos.z = cameraOrigin.y;
    glm::vec3 objectCenter;
    objectCenter = position - cameraPos;
    objectCenter.z += heightDelta;
    frame.trans_to_frame(objectCenter);
    float distanceToEye = glm::length(objectCenter);
    float clip_distance = fabs(1.0f / objectCenter.y);
    float object_angle = fabs(objectCenter.z) * clip_distance;
    float extent_angle = radius / distanceToEye;
    bool outside = false;
    if (object_angle > (vClipConstant + extent_angle))
        outside = true;
    else {
        object_angle = fabs(objectCenter.x) * clip_distance;
        if (object_angle > (hClipConstant + extent_angle)) outside = true;
    }
    lastAngularTest.position = position;
    lastAngularTest.heightDelta = heightDelta;
    lastAngularTest.radius = radius;
    lastAngularTest.valid = true;
    lastAngularTest.outside = outside;
    lastAngularTest.objectCenter = objectCenter;
    return outside;
}
}
