#pragma once
// Hash of the camera inputs that move the pick screen-rect cache (Camera::calculateProjectionConstants bumps the
// view-projection revision when it changes). Coarse grid: position / altitude / shift snapped to 0.25 WU, rotation and
// projection angle to 0.05 deg, fov to 0.1 deg; stable under per-frame altitude/zoom lerp micro-drift.
#include <cstdint>

namespace mc2cam {

struct PickInputs {
    float positionX, positionY, shiftZ, rotation, projectionAngle, fov;
    float altitude;                 // zoom: part of the camera origin, not only the goal position
    float shiftX, shiftY;           // eased look-at shift
    float resX, resY;
};

inline std::uint32_t pickInputHash(const PickInputs& in) {
    auto snapI = [](float v, float step) -> std::uint32_t {
        return static_cast<std::uint32_t>(static_cast<std::int32_t>((v / step)));
    };
    return snapI(in.positionX,       0.25f) * 2654435761u
         ^ snapI(in.positionY,       0.25f) * 2246822519u
         ^ snapI(in.shiftZ,          0.25f) * 3266489917u
         ^ snapI(in.rotation,        0.05f) * 374761393u
         ^ snapI(in.projectionAngle, 0.05f) * 668265263u
         ^ snapI(in.fov,             0.1f)  * 2166136261u
         // The camera origin is built from the altitude (zoom) and the eased look-at shift, not only the goal
         // position: a zoom or an eased pan that left these out kept stale pick rects (hits on objects that
         // are not under the cursor, misses on ones that are).
         ^ snapI(in.altitude,        0.25f) * 2869860233u
         ^ snapI(in.shiftX,          0.25f) * 1597334677u
         ^ snapI(in.shiftY,          0.25f) * 3812015801u
         ^ static_cast<std::uint32_t>(static_cast<std::int32_t>(in.resX))
         ^ static_cast<std::uint32_t>(static_cast<std::int32_t>(in.resY)) * 40503u;
}

} // namespace mc2cam
