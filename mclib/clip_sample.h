// clip_sample.h -- glTF animation key sampling (STEP / LINEAR / CUBICSPLINE), header-only.
//
// Templated over the key/vector/quaternion types so the same code serves Assimp
// (aiVectorKey / aiQuatKey) and the Assimp-free unit test. A key type needs
// mTime (double), mValue and mInterpolation (int-convertible: 0 step, 1 linear,
// 2 spherical-linear, 3 cubic spline -- the aiAnimInterpolation values).
// Vec needs Vec(x,y,z) and .x/.y/.z; Quat needs Quat(w,x,y,z) and .w/.x/.y/.z.
//
// CUBICSPLINE keys are stored by Assimp as triplets [in-tangent, value, out-tangent]
// per logical key, all at the same mTime. Tangents are per second, so a segment's
// tangents are scaled by its length in seconds (secPerTick).
//
// LINEAR / SLERP segments keep the pre-existing behaviour bit-for-bit (stock BT2018
// clips are LINEAR); the quaternion slerp (and its normalise) is supplied by the caller.
#pragma once
#include <cmath>

namespace mc2skel::clip {

inline constexpr int kStep = 0, kLinear = 1, kSlerp = 2, kCubic = 3;

template <class Key>
bool isCubic(const Key* k, unsigned n) {
    return n >= 3 && n % 3 == 0 && static_cast<int>(k[0].mInterpolation) == kCubic;
}

// Hermite basis for s in [0,1].
struct Hermite {
    float h00, h10, h01, h11;
    explicit Hermite(float s) {
        const float s2 = s * s, s3 = s2 * s;
        h00 = 2 * s3 - 3 * s2 + 1;
        h10 = s3 - 2 * s2 + s;
        h01 = -2 * s3 + 3 * s2;
        h11 = s3 - s2;
    }
};

template <class Key, class Vec>
Vec sampleVec3(const Key* k, unsigned n, double t, double secPerTick) {
    const bool cubic = isCubic(k, n);
    const unsigned m = cubic ? n / 3 : n;
    auto V = [&](unsigned j) -> const Key& { return k[cubic ? 3 * j + 1 : j]; };
    if (m == 1) return V(0).mValue;
    unsigned i = 0;
    while (i + 1 < m && V(i + 1).mTime <= t) ++i;
    if (i + 1 >= m) return V(m - 1).mValue;
    const double t0 = V(i).mTime, t1 = V(i + 1).mTime;
    const int mode = static_cast<int>(V(i).mInterpolation);
    if (mode == kStep) return V(i).mValue;
    if (cubic) {
        const double dt = t1 - t0;
        if (dt <= 0.0) return V(i).mValue;
        double sd = (t - t0) / dt;
        sd = sd < 0.0 ? 0.0 : (sd > 1.0 ? 1.0 : sd);
        const Hermite h(static_cast<float>(sd));
        const float dts = static_cast<float>(dt * secPerTick);
        const auto& p0 = V(i).mValue;
        const auto& p1 = V(i + 1).mValue;
        const auto& m0 = k[3 * i + 2].mValue;        // out-tangent of key i
        const auto& m1 = k[3 * (i + 1)].mValue;      // in-tangent of key i+1
        return Vec(h.h00 * p0.x + h.h10 * dts * m0.x + h.h01 * p1.x + h.h11 * dts * m1.x,
                   h.h00 * p0.y + h.h10 * dts * m0.y + h.h01 * p1.y + h.h11 * dts * m1.y,
                   h.h00 * p0.z + h.h10 * dts * m0.z + h.h01 * p1.z + h.h11 * dts * m1.z);
    }
    const float f = (t1 > t0) ? static_cast<float>(((t - t0) / (t1 - t0))) : 0.0f;
    const auto& a = V(i).mValue;
    const auto& b = V(i + 1).mValue;
    return Vec(a.x * (1.0f - f) + b.x * f, a.y * (1.0f - f) + b.y * f, a.z * (1.0f - f) + b.z * f);
}

// slerp(out, from, to, f): caller-supplied spherical interpolation + normalise
// (Assimp: aiQuaternion::Interpolate then Normalize).
template <class Key, class Quat, class SlerpFn>
Quat sampleQuat(const Key* k, unsigned n, double t, double secPerTick, SlerpFn slerp) {
    const bool cubic = isCubic(k, n);
    const unsigned m = cubic ? n / 3 : n;
    auto V = [&](unsigned j) -> const Key& { return k[cubic ? 3 * j + 1 : j]; };
    if (m == 1) return V(0).mValue;
    unsigned i = 0;
    while (i + 1 < m && V(i + 1).mTime <= t) ++i;
    if (i + 1 >= m) return V(m - 1).mValue;
    const double t0 = V(i).mTime, t1 = V(i + 1).mTime;
    const int mode = static_cast<int>(V(i).mInterpolation);
    if (mode == kStep) return V(i).mValue;
    if (cubic) {
        const double dt = t1 - t0;
        if (dt <= 0.0) return V(i).mValue;
        double sd = (t - t0) / dt;
        sd = sd < 0.0 ? 0.0 : (sd > 1.0 ? 1.0 : sd);
        const Hermite h(static_cast<float>(sd));
        const float dts = static_cast<float>(dt * secPerTick);
        const auto& p0 = V(i).mValue;
        const auto& p1 = V(i + 1).mValue;
        const auto& m0 = k[3 * i + 2].mValue;
        const auto& m1 = k[3 * (i + 1)].mValue;
        float w = h.h00 * p0.w + h.h10 * dts * m0.w + h.h01 * p1.w + h.h11 * dts * m1.w;
        float x = h.h00 * p0.x + h.h10 * dts * m0.x + h.h01 * p1.x + h.h11 * dts * m1.x;
        float y = h.h00 * p0.y + h.h10 * dts * m0.y + h.h01 * p1.y + h.h11 * dts * m1.y;
        float z = h.h00 * p0.z + h.h10 * dts * m0.z + h.h01 * p1.z + h.h11 * dts * m1.z;
        const float len = std::sqrt(w * w + x * x + y * y + z * z);
        if (len > 0.0f) { w /= len; x /= len; y /= len; z /= len; }
        return Quat(w, x, y, z);
    }
    const float f = (t1 > t0) ? static_cast<float>(((t - t0) / (t1 - t0))) : 0.0f;
    Quat rot = V(i).mValue;
    slerp(rot, V(i).mValue, V(i + 1).mValue, f);
    return rot;
}

}  // namespace mc2skel::clip
