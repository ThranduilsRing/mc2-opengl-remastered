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

// Live/authored sun elevation: mech and vehicle light rows follow the live pitch, the ground's flat-light ratio does not.
// This gain on their directional term puts a flat surface where the ground is. Exactly 1 with no keys.
inline float pitchGain(const AuthoredSun& a, float livePitchDeg) {
    if (!a.valid || a.pitch <= 0.0f || a.pitch == livePitchDeg || livePitchDeg <= 0.0f) return 1.0f;
    const float k = 0.01745329252f;
    const float sinLive = std::sin(livePitchDeg * k);
    if (!(sinLive > 0.0f)) return 1.0f;   // live pitch in [180, 360): sun below the horizon, no ratio (a NaN here made the mission lighting compare unequal every frame)
    const float g = std::sqrt(std::sin(a.pitch * k) / sinLive);   // sqrt: movers keep an ambient share the pitch does not touch
    return std::isfinite(g) ? std::clamp(g, 0.5f, 2.0f) : 1.0f;   // bounded: a near-horizon live sun must not blow the mover light rows up
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
