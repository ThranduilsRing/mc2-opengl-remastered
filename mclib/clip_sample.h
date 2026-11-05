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
#include <cstddef>
#include <span>

namespace mc2skel::clip {

inline constexpr int kStep = 0, kLinear = 1, kSlerp = 2, kCubic = 3;

template <class Key>
bool isCubic(std::span<const Key> k) {
    return k.size() >= 3 && k.size() % 3 == 0 && static_cast<int>(k[0].mInterpolation) == kCubic;
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
    // One scalar component: p0/p1 are the segment values, m0 the out-tangent of key i, m1 the in-tangent of key i+1.
    float at(float p0, float m0, float p1, float m1, float dts) const {
        return h00 * p0 + h10 * dts * m0 + h01 * p1 + h11 * dts * m1;
    }
};

// Where a time lands in a key track. Indices address the raw key array.
struct Segment {
    enum class Kind { Hold, Linear, Cubic } kind = Kind::Hold;
    std::size_t a = 0, b = 0;       // value keys (Hold uses a only)
    std::size_t outA = 0, inB = 0;  // Cubic: out-tangent of a, in-tangent of b
    float f = 0.0f;                 // Linear: blend factor; Cubic: clamped s
    float dts = 0.0f;               // Cubic: segment length in seconds
};

template <class Key>
Segment locate(std::span<const Key> k, double t, double secPerTick) {
    Segment r;
    const bool cubic = isCubic<Key>(k);
    const std::size_t m = cubic ? k.size() / 3 : k.size();
    auto idx = [cubic](std::size_t j) { return cubic ? 3 * j + 1 : j; };
    std::size_t i = 0;
    while (i + 1 < m && k[idx(i + 1)].mTime <= t) ++i;
    r.a = idx(i);
    if (m == 1 || i + 1 >= m || static_cast<int>(k[r.a].mInterpolation) == kStep) return r;  // Hold
    const double t0 = k[r.a].mTime, t1 = k[idx(i + 1)].mTime;
    r.b = idx(i + 1);
    if (cubic) {
        const double dt = t1 - t0;
        if (dt <= 0.0) return r;
        const double sd = (t - t0) / dt;
        r.kind = Segment::Kind::Cubic;
        r.f = static_cast<float>(sd < 0.0 ? 0.0 : (sd > 1.0 ? 1.0 : sd));
        r.dts = static_cast<float>(dt * secPerTick);
        r.outA = 3 * i + 2;
        r.inB = 3 * (i + 1);
        return r;
    }
    r.kind = Segment::Kind::Linear;
    r.f = (t1 > t0) ? static_cast<float>((t - t0) / (t1 - t0)) : 0.0f;
    return r;
}

template <class Key, class Vec>
Vec sampleVec3(std::span<const Key> k, double t, double secPerTick) {
    const Segment g = locate<Key>(k, t, secPerTick);
    const auto& p0 = k[g.a].mValue;
    if (g.kind == Segment::Kind::Hold) return p0;
    const auto& p1 = k[g.b].mValue;
    if (g.kind == Segment::Kind::Linear)
        return Vec(p0.x * (1.0f - g.f) + p1.x * g.f, p0.y * (1.0f - g.f) + p1.y * g.f, p0.z * (1.0f - g.f) + p1.z * g.f);
    const Hermite h(g.f);
    const auto& m0 = k[g.outA].mValue;
    const auto& m1 = k[g.inB].mValue;
    return Vec(h.at(p0.x, m0.x, p1.x, m1.x, g.dts), h.at(p0.y, m0.y, p1.y, m1.y, g.dts),
               h.at(p0.z, m0.z, p1.z, m1.z, g.dts));
}

// slerp(out, from, to, f): caller-supplied spherical interpolation + normalise
// (Assimp: aiQuaternion::Interpolate then Normalize).
template <class Key, class Quat, class SlerpFn>
Quat sampleQuat(std::span<const Key> k, double t, double secPerTick, SlerpFn slerp) {
    const Segment g = locate<Key>(k, t, secPerTick);
    Quat rot = k[g.a].mValue;
    if (g.kind == Segment::Kind::Hold) return rot;
    const auto& p0 = k[g.a].mValue;
    const auto& p1 = k[g.b].mValue;
    if (g.kind == Segment::Kind::Linear) {
        slerp(rot, p0, p1, g.f);
        return rot;
    }
    const Hermite h(g.f);
    const auto& m0 = k[g.outA].mValue;
    const auto& m1 = k[g.inB].mValue;
    float w = h.at(p0.w, m0.w, p1.w, m1.w, g.dts), x = h.at(p0.x, m0.x, p1.x, m1.x, g.dts);
    float y = h.at(p0.y, m0.y, p1.y, m1.y, g.dts), z = h.at(p0.z, m0.z, p1.z, m1.z, g.dts);
    const float len = std::sqrt(w * w + x * x + y * y + z * z);
    if (len > 0.0f) { w /= len; x /= len; y /= len; z /= len; }
    return Quat(w, x, y, z);
}

}  // namespace mc2skel::clip
