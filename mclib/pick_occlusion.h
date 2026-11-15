#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include "../RenderCore/axis_convert.h"

class Camera;

namespace mc2 {
// Engine adapter shared with the terrain picker, without changing Camera's header.
bool pickScreenRay(Camera& camera, long x, long y, double (&origin)[3], double (&direction)[3]);

// Mech type bounds are already MC2 XYZ (mech3d_init); static/vehicle bounds are render XYZ.
template<class V>
V pickBoundsInMc2(bool mech, const V& bound) {
    return mech ? bound : mc2ToRender(bound);
}

// Ray and bounds are in the object's unrotated MC2 frame. A projected rect
// alone does not say where along the cursor ray the object is encountered.
inline bool pickBoxHit(const double (&origin)[3], const double (&direction)[3],
                       const double (&lo)[3], const double (&hi)[3], double& hit)
{
    double first = 0, last = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(origin[i]) || !std::isfinite(direction[i])
            || !std::isfinite(lo[i]) || !std::isfinite(hi[i])) return false;
        const double lower = std::min(lo[i], hi[i]), upper = std::max(lo[i], hi[i]);
        if (std::abs(direction[i]) < 1e-12) {
            if (origin[i] < lower || origin[i] > upper) return false;
        } else {
            double a = (lower-origin[i])/direction[i], b = (upper-origin[i])/direction[i];
            if (a > b) std::swap(a, b);
            first = std::max(first, a); last = std::min(last, b);
        }
        if (first > last) return false;
    }
    if (!std::isfinite(first) || last <= 0) return false;
    hit = first;
    return true;
}

// Nonzero slack requires the caller to establish a local terrain-height mismatch.
inline bool pickBeforeSurface(double objectT, double surfaceT, double slack = 0)
{
    return std::isfinite(objectT) && objectT >= 0 && objectT <= surfaceT + 1e-3 + slack;
}

// A visual terrain hit inside the mover's rotated XY footprint can disagree with its gameplay ground placement.
// A ridge in front of that footprint is an occluder, even when closer than one bounds diagonal.
inline bool pickMoverBeforeSurface(double objectT, double surfaceT, const double (&surface)[3],
                                   const double (&lo)[3], const double (&hi)[3]) {
    double size2 = 0;
    bool footprint = true;
    for (int i = 0; i < 3; ++i) {
        size2 += (hi[i] - lo[i]) * (hi[i] - lo[i]);
        const auto [lower, upper] = std::minmax(lo[i], hi[i]);
        if (i < 2) footprint = footprint && surface[i] == std::clamp(surface[i], lower, upper);
    }
    return pickBeforeSurface(objectT, surfaceT, footprint ? std::sqrt(size2) : 0);
}

// The caller has already tested the stock screen rectangle. Static scenery
// requires an exact hit; movers may recover via a ray through their bounds.
template<class ExactHit, class MoverHit>
std::optional<double> pickDepthWithForgiveness(bool mover, ExactHit exactHit, MoverHit moverHit)
{
    double depth;
    if (exactHit(depth) || (mover && moverHit(depth))) return depth;
    return std::nullopt;
}

// Apply only after terrain rejection. Visible units win overlapping scenery;
// depth still resolves two units (or two static objects).
inline bool pickCandidateBefore(bool mover, double depth, int priority,
                                bool otherMover, double otherDepth, int otherPriority)
{
    if (mover != otherMover) return mover;
    if (depth != otherDepth) return depth < otherDepth;
    return priority < otherPriority;
}

// Total order of the gameplay pick stack: an eligible (visible, targetable) mover always precedes scenery
// (trees, props, buildings, even when nearer along the ray); then depth, priority, handle.
// forgiven: the cursor ray missed the unit's bounds and the hit comes from a ray through its centre (padded-rect recovery);
// a unit the cursor actually hits outranks one that was only forgiven, however far behind it stands (grazing views overlap rects).
struct PickRank { bool eligibleMover; double depth; int priority; long handle; bool forgiven = false; };
inline bool pickStackBefore(const PickRank& a, const PickRank& b)
{
    if (a.eligibleMover != b.eligibleMover) return a.eligibleMover;
    if (a.eligibleMover && a.forgiven != b.forgiven) return !a.forgiven;
    if (a.depth != b.depth) return a.depth < b.depth;
    if (a.priority != b.priority) return a.priority < b.priority;
    return a.handle < b.handle;
}
} // namespace mc2
