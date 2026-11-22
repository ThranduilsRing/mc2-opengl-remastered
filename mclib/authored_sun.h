#pragma once
// The mission's AUTHORED sun colour and ambient (before the per-mission sun keys replace them) and the flat-light ratio
// derived from them. Pure and header-only so a unit test pins it. Flat light = getLight(z) = sun*z + ambient (bytes).
#include <algorithm>
#include <array>
#include <cmath>

namespace mc2::authored_sun {

struct AuthoredSun {
    bool valid = false;                         // false: night, no mission or no per-mission sun block -> ratio is exactly 1
    std::array<unsigned char, 3> sun{}, amb{};
    float pitch = 0;                            // authored sun pitch (degrees); 0 = unknown
    float yaw = 0;                              // authored sun yaw (degrees); only meaningful with hasYaw
    bool hasYaw = false;                        // an older keyed save carries no yaw: the live yaw stands in
};

// Camera::getLightRed/Green/Blue(z) for one channel set (z is the unit light z, clamped 0..1 by the caller).
inline std::array<unsigned char, 3> flatBytes(const std::array<unsigned char, 3>& sun, const std::array<unsigned char, 3>& amb, float z, long floorV) {
    std::array<unsigned char, 3> out{};
    for (int i = 0; i < 3; ++i) {
        const long v = z > 0.0f ? static_cast<long>(sun[i] * z + amb[i]) : static_cast<long>(amb[i]);
        out[i] = static_cast<unsigned char>(std::clamp(v, floorV, 255L));
    }
    return out;
}

// R = liveFlat / authoredFlat per channel. Exactly 1 when there is no authored record or the two are equal (no keys).
inline std::array<float, 3> flatRatio(const AuthoredSun& a, const std::array<unsigned char, 3>& liveSun, const std::array<unsigned char, 3>& liveAmb,
                                      float z, long floorV) {
    if (!a.valid) return {1.0f, 1.0f, 1.0f};
    const auto live = flatBytes(liveSun, liveAmb, z, floorV);
    const auto auth = flatBytes(a.sun, a.amb, z, floorV);
    std::array<float, 3> r{1.0f, 1.0f, 1.0f};
    for (int i = 0; i < 3; ++i)
        if (live[i] != auth[i] && auth[i] > 0) r[i] = static_cast<float>(live[i]) / static_cast<float>(auth[i]);
    return r;
}

// Luma of the live ambient over the authored ambient: how far the sun keys moved the sky fill. Exactly 1 with no keys.
inline float ambientRatio(const AuthoredSun& a, const std::array<unsigned char, 3>& liveAmb) {
    if (!a.valid || a.amb == liveAmb) return 1.0f;
    auto l = [](const std::array<unsigned char, 3>& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
    return l(a.amb) > 0.0f ? l(liveAmb) / l(a.amb) : 1.0f;
}

// ---- One sun (S4): the authored sun next to the live one, for every prelit-class surface. Each value is EXACTLY identity /
// 1.0f with no keys, at night, with no mission (no authored record) and in the mech lab, so the shaders need no branch.
using Vec3 = std::array<float, 3>;
using Mat3Cols = std::array<Vec3, 3>;   // column j = image of axis j (std140: three vec4 columns)

struct OneSun {
    Mat3Cols authFromLive{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};   // rotation taking the live sun direction to the authored one
    Vec3 pitchUndo{1.0f, 1.0f, 1.0f};   // authored flat light at the authored elevation / at the live elevation (encoded, per channel): undoes the live pitch in a prop's sun term
    Vec3 ambRelight{1.0f, 1.0f, 1.0f};  // R * authored ambient / live ambient: the mover ambient moves like the ground
    Vec3 sunRelight{1.0f, 1.0f, 1.0f};  // live sun colour / authored sun colour: glint and specular colour
    bool authoredValid = false;
    bool operator==(const OneSun&) const = default;
};

inline Vec3 normalized3(const Vec3& v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return l > 0.0f ? Vec3{v[0] / l, v[1] / l, v[2] / l} : Vec3{0.0f, 0.0f, 1.0f};
}

// The camera's light direction formula (Camera::init: cos/sin(yaw+45), sin(pitch)), unit length. No authored yaw: the live yaw.
inline Vec3 authoredDir(const AuthoredSun& a, float liveYawDeg) {
    const float k = 0.01745329252f, yaw = a.hasYaw ? a.yaw : liveYawDeg;
    return normalized3({std::cos((yaw + 45.0f) * k), std::sin((yaw + 45.0f) * k), std::sin(a.pitch * k)});
}

// Minimal rotation taking unit vector l to unit vector t (Rodrigues). Exact identity when they are equal (no keys) or opposite.
inline Mat3Cols rotationBetween(const Vec3& l, const Vec3& t) {
    Mat3Cols id{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    if (l == t) return id;
    const Vec3 v{l[1] * t[2] - l[2] * t[1], l[2] * t[0] - l[0] * t[2], l[0] * t[1] - l[1] * t[0]};
    const float c = l[0] * t[0] + l[1] * t[1] + l[2] * t[2];
    if (!(c > -0.9999f)) return id;
    const float f = 1.0f / (1.0f + c);
    const float vx[3][3] = {{0, -v[2], v[1]}, {v[2], 0, -v[0]}, {-v[1], v[0], 0}};   // [v]x, row-major
    Mat3Cols m{};
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row) {
            float vx2 = 0.0f;
            for (int k = 0; k < 3; ++k) vx2 += vx[row][k] * vx[k][col];
            m[col][row] = (row == col ? 1.0f : 0.0f) + vx[row][col] + vx2 * f;
        }
    return m;
}

// Everything the shaders need of the authored sun. liveDir: the camera's light direction (any length); zLive: its flat-light z
// (clamped 0..1); R: flatRatio at zLive. Every field is exactly identity/1 when there is no authored record or the live
// value equals the authored one.
inline OneSun oneSun(const AuthoredSun& a, const Vec3& liveDir, float livePitchDeg, float liveYawDeg, const std::array<unsigned char, 3>& liveSun,
                     const std::array<unsigned char, 3>& liveAmb, float zLive, const std::array<float, 3>& R, long floorV) {
    OneSun s;
    if (!a.valid) return s;
    s.authoredValid = true;
    // Compared on the angles, never on the recomputed directions: no keys must give identity to the bit.
    const bool sameAngles = a.pitch == livePitchDeg && (!a.hasYaw || a.yaw == liveYawDeg);
    if (a.pitch > 0.0f && !sameAngles) {
        s.authFromLive = rotationBetween(normalized3(liveDir), authoredDir(a, liveYawDeg));
        const float zAuth = std::clamp(std::sin(a.pitch * 0.01745329252f), 0.0f, 1.0f);
        if (a.pitch != livePitchDeg && zAuth != zLive) {
            const auto atAuth = flatBytes(a.sun, a.amb, zAuth, floorV), atLive = flatBytes(a.sun, a.amb, zLive, floorV);
            for (int i = 0; i < 3; ++i)
                if (atAuth[i] != atLive[i] && atLive[i] > 0) s.pitchUndo[i] = static_cast<float>(atAuth[i]) / static_cast<float>(atLive[i]);
        }
    }
    for (int i = 0; i < 3; ++i) {
        const float ambK = (a.amb[i] != liveAmb[i] && liveAmb[i] > 0) ? static_cast<float>(a.amb[i]) / static_cast<float>(liveAmb[i]) : 1.0f;
        s.ambRelight[i] = R[i] * ambK;
        if (a.sun[i] != liveSun[i] && a.sun[i] > 0) s.sunRelight[i] = static_cast<float>(liveSun[i]) / static_cast<float>(a.sun[i]);
    }
    return s;
}

// Camera::init, after a save's block has been read: the authored sun a keyed save carries wins over the (already keyed)
// colours the save holds; a save without one (older, or written without keys) takes the colours as read.
inline AuthoredSun resolve(const std::array<unsigned char, 3>& readSun, const std::array<unsigned char, 3>& readAmb,
                           bool savedHas, const std::array<unsigned char, 3>& savedSun, const std::array<unsigned char, 3>& savedAmb) {
    AuthoredSun a;
    a.valid = true;
    a.sun = savedHas ? savedSun : readSun;
    a.amb = savedHas ? savedAmb : readAmb;
    return a;
}

}  // namespace mc2::authored_sun
