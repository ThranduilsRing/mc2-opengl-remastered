#pragma once
// LightingState: the one producer of the sun, ambient and fog the camera publishes (dynamic time-of-day, slice T1).
// Pure and header-only so a unit test pins it. The shape of the future clock/shader work is in the plan
// (dynamic-tod-plan-1.md, section 2A); only the fields needed now exist.
//
//   authored  = what the mission and its sun keys say today (Fixed mode, the only mode stock missions have)
//   evaluate  = authored + an optional TodCurve at time t. With NO curve the result IS the authored state, field for field.
//
// Reserved, comments only (T2/T7, no storage yet): sky{hdriA, hdriB, blend, sh[9]}, exposure, cloud keys, nightFactor as a
// continuous curve, lampsOn. The clock (t advancing on the sim tick, save/load) is T3; the UBO tail the shaders read is T2.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "dstd.h"

namespace mc2::lighting {

enum class Mode : uint8_t { Fixed, Live };

struct LightingState {
	float sunDir[3] = {0.0f, 0.0f, 0.0f};   // MC2 world space (x, y, elevation): Camera::lightDirection
	float pitch = 0.0f, yaw = 0.0f;         // degrees
	uint8_t sun[3] = {0, 0, 0};             // the camera's encoded sun colour bytes
	uint8_t amb[3] = {0, 0, 0};             // the camera's encoded ambient bytes
	uint32_t fogColor = 0;                  // 0xAARRGGBB as Camera::fogColor
	float fogStart = 0.0f, fogFull = 0.0f;
	Mode mode = Mode::Fixed;
	float todWeight = 0.0f;                 // 0 Fixed, 1 Live (the plan's relight weight; nothing reads it until T2/T4)
	bool operator==(const LightingState&) const = default;
};

// The camera's own direction expression (mclib/camera.cpp:301, 388, 430). Unqualified sin/cos exactly as there.
inline void sunDirFromAngles(float pitch, float yaw, float out[3])
{
	out[0] = cos((yaw + 45.0f) * DEGREES_TO_RADS);
	out[1] = sin((yaw + 45.0f) * DEGREES_TO_RADS);
	out[2] = sin(pitch * DEGREES_TO_RADS);
}

// ---- Per-mission sun keys (visual_tuning sidecar: sunPitch/sunYaw/sunColorR/G/B/sunAmbientScale): Fixed-mode authoring ----
struct SunKeys {
	std::optional<float> pitch, yaw, colorR, colorG, colorB, ambientScale;
};
inline constexpr float kMinSunPitch = 26.0f;   // the keys' clamp (camera.cpp:428)

inline uint8_t scaleAmbientByte(uint8_t v, float scale) { return static_cast<uint8_t>(std::min(255.0f, v * scale + 0.5f)); }

// The authored state after the keys, exactly the arithmetic camera.cpp did inline. ambientScaledInSave: a save already holds
// scaled ambient bytes (apply no second scale). Returns the new state; the base is unchanged.
inline LightingState applySunKeys(LightingState s, const SunKeys& k, bool ambientScaledInSave)
{
	if (k.pitch) s.pitch = std::max(*k.pitch, kMinSunPitch);
	if (k.yaw) s.yaw = *k.yaw;
	sunDirFromAngles(s.pitch, s.yaw, s.sunDir);
	if (k.colorR && k.colorG && k.colorB) {
		s.sun[0] = static_cast<uint8_t>(*k.colorR + 0.5f);
		s.sun[1] = static_cast<uint8_t>(*k.colorG + 0.5f);
		s.sun[2] = static_cast<uint8_t>(*k.colorB + 0.5f);
	}
	if (k.ambientScale && !ambientScaledInSave) {
		for (auto& v : s.amb) v = scaleAmbientByte(v, *k.ambientScale);
	}
	return s;
}

// ---- Time-of-day curve ----
inline constexpr uint32_t kPitch = 1u << 0, kYaw = 1u << 1, kSun = 1u << 2, kAmb = 1u << 3, kFogColor = 1u << 4, kFogStart = 1u << 5, kFogFull = 1u << 6;   // TodKey::mask bits

struct TodKey {
	float t = 0.0f;                 // normalized time
	uint32_t mask = 0;              // TodChannel bits this key sets; other channels fall through to the authored value
	float pitch = 0.0f, yaw = 0.0f;
	uint8_t sun[3] = {0, 0, 0}, amb[3] = {0, 0, 0};
	uint32_t fogColor = 0;          // 0xRRGGBB (the alpha byte stays the authored one)
	float fogStart = 0.0f, fogFull = 0.0f;
};

inline constexpr float kMinCurvePitch = -10.0f, kMaxCurvePitch = 89.0f;   // a curve's sun pitch range (low for dawn/dusk; the keys' 26 floor does not apply)
inline constexpr size_t kMaxTodKeys = 64;   // one cap for every channel (parseTodCurve drops the rest and counts them)

struct TodCurve {
	std::vector<TodKey> keys;       // sorted by t, strictly increasing (parseTodCurve guarantees it)
	float start = 0.0f;             // normalized time a mission begins at
	float rate = 1.0f / 86400.0f;   // normalized time per sim second; default is one real-time day
	uint32_t hash = 0;             // stable identity of the accepted sidecar block
	size_t droppedKeys = 0;         // keys past kMaxTodKeys that parseTodCurve discarded (the camera logs a [TOD] warning)
};

inline double advanceTod(const TodCurve& curve, double t, float rate)
{
	// Double accumulation avoids float rounding changing the real-time rate near noon.
	return std::min(static_cast<double>(curve.keys.back().t), t + static_cast<double>(rate) / 30.0);
}

inline float restoreTod(const TodCurve& curve, uint32_t hash, float t)
{
	return hash == curve.hash && std::isfinite(t)
		? std::clamp(t, curve.keys.front().t, curve.keys.back().t) : curve.start;
}

template<class Save>
inline void saveTod(Save& file, const TodCurve* curve, double t, float rate)
{
	if (!curve) return;
	file.writeIdFloat("TimeOfDay", static_cast<float>(t));
	file.writeIdFloat("TimeOfDayRate", rate);
	file.writeIdULong("TodCurveHash", curve->hash);
}

template<class Publish>
inline void tickTod(const TodCurve* curve, double& t, float rate, Publish publish)
{
	if (!curve) return;
	const double next = advanceTod(*curve, t, rate);
	if (next == t) return;
	t = next;
	publish();
}

namespace detail {

// Fritsch-Carlson monotone cubic (PCHIP) through (x[i], y[i]); no overshoot between keys. x strictly increasing, n >= 2.
inline float pchip(std::span<const float> x, std::span<const float> y, float t)
{
	const size_t n = x.size();
	if (n == 1 || t <= x[0]) return y[0];
	if (t >= x[n - 1]) return y[n - 1];
	size_t i = 0;
	while (i + 2 < n && t > x[i + 1]) ++i;
	auto slope = [&](size_t j) { return (y[j + 1] - y[j]) / (x[j + 1] - x[j]); };
	auto tangent = [&](size_t j) {
		if (n == 2) return slope(0);
		if (j == 0 || j == n - 1) {
			const size_t a = j == 0 ? 0 : n - 2;
			return slope(a);   // one-sided end tangent; the clamp below keeps it monotone
		}
		const float d0 = slope(j - 1), d1 = slope(j);
		if (d0 * d1 <= 0.0f) return 0.0f;
		const float h0 = x[j] - x[j - 1], h1 = x[j + 1] - x[j];
		const float w1 = 2.0f * h1 + h0, w2 = h1 + 2.0f * h0;
		return (w1 + w2) / (w1 / d0 + w2 / d1);
	};
	const float h = x[i + 1] - x[i], d = slope(i);
	float m0 = tangent(i), m1 = tangent(i + 1);
	if (d == 0.0f) { m0 = m1 = 0.0f; }
	else {
		if (m0 * d < 0.0f) m0 = 0.0f;
		if (m1 * d < 0.0f) m1 = 0.0f;
		const float a = m0 / d, b = m1 / d, s = a * a + b * b;
		if (s > 9.0f) { const float k = 3.0f / std::sqrt(s); m0 *= k; m1 *= k; }
	}
	const float u = (t - x[i]) / h, u2 = u * u, u3 = u2 * u;
	return (2 * u3 - 3 * u2 + 1) * y[i] + (u3 - 2 * u2 + u) * h * m0 + (-2 * u3 + 3 * u2) * y[i + 1] + (u3 - u2) * h * m1;
}

inline uint8_t lerpByte(uint8_t a, uint8_t b, float u)
{
	if (u <= 0.0f) return a;
	if (u >= 1.0f) return b;
	return static_cast<uint8_t>(std::clamp(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * u + 0.5f, 0.0f, 255.0f));
}

// Keys carrying `bit`, in order; the bracketing pair for t.
struct Span { const TodKey* a = nullptr; const TodKey* b = nullptr; float u = 0.0f; };
inline Span bracket(const TodCurve& c, uint32_t bit, float t)
{
	Span s;
	const TodKey* prev = nullptr;
	for (const auto& k : c.keys) {
		if (!(k.mask & bit)) continue;
		if (k.t >= t) { s.b = &k; s.a = prev ? prev : &k; break; }
		prev = &k;
	}
	if (!s.b) { s.a = s.b = prev; }   // after the last key (or no keys with this channel: both null)
	if (s.a && s.b && s.b->t > s.a->t) s.u = std::clamp((t - s.a->t) / (s.b->t - s.a->t), 0.0f, 1.0f);
	return s;
}

inline float scalar(const TodCurve& c, uint32_t bit, float t, float TodKey::*member, float authored)
{
	float x[kMaxTodKeys], y[kMaxTodKeys];
	size_t n = 0;
	for (const auto& k : c.keys)
		if ((k.mask & bit) && n < kMaxTodKeys) { x[n] = k.t; y[n] = k.*member; ++n; }
	return n ? pchip(std::span<const float>(x, n), std::span<const float>(y, n), t) : authored;
}

} // namespace detail

// The evaluated state. With a null curve (or no keys) this returns `authored` unchanged: same bytes, same floats, mode Fixed.
// t is clamped to the keyed range (a curve does not extrapolate).
inline LightingState evaluate(const TodCurve* curve, float t, const LightingState& authored)
{
	if (!curve || curve->keys.empty()) return authored;
	LightingState s = authored;
	s.mode = Mode::Live;
	s.todWeight = 1.0f;
	uint32_t present = 0;
	for (const auto& k : curve->keys) present |= k.mask;
	const float tc = std::clamp(t, curve->keys.front().t, curve->keys.back().t);
	if (present & kPitch) s.pitch = detail::scalar(*curve, kPitch, tc, &TodKey::pitch, s.pitch);
	if (present & kYaw) s.yaw = detail::scalar(*curve, kYaw, tc, &TodKey::yaw, s.yaw);
	if (present & (kPitch | kYaw)) sunDirFromAngles(s.pitch, s.yaw, s.sunDir);
	if (present & kSun) {
		const auto sp = detail::bracket(*curve, kSun, tc);
		for (int i = 0; i < 3; ++i) s.sun[i] = detail::lerpByte(sp.a->sun[i], sp.b->sun[i], sp.u);
	}
	if (present & kAmb) {
		const auto sp = detail::bracket(*curve, kAmb, tc);
		for (int i = 0; i < 3; ++i) s.amb[i] = detail::lerpByte(sp.a->amb[i], sp.b->amb[i], sp.u);
	}
	if (present & kFogColor) {
		const auto sp = detail::bracket(*curve, kFogColor, tc);
		uint32_t rgb = 0;
		for (int sh = 16; sh >= 0; sh -= 8)
			rgb |= static_cast<uint32_t>(detail::lerpByte((sp.a->fogColor >> sh) & 255u, (sp.b->fogColor >> sh) & 255u, sp.u)) << sh;
		s.fogColor = (authored.fogColor & 0xff000000u) | rgb;
	}
	if (present & kFogStart) s.fogStart = detail::scalar(*curve, kFogStart, tc, &TodKey::fogStart, s.fogStart);
	if (present & kFogFull) s.fogFull = detail::scalar(*curve, kFogFull, tc, &TodKey::fogFull, s.fogFull);
	return s;
}

// ---- Modulators: the systems that write the camera's light on top of its base (weather, KillAmbientLight, the runtime sun control) ----
// The base is today's day members (no curve) or evaluate() (a curve). The composition is this ONE function, so the two cases cannot
// drift: rain dims sun and ambient by dividing the base bytes (the old weather.cpp expression), kill forces ambient to white, a sun
// override replaces the pitch/yaw. All default to "none", which returns the base untouched.
struct LightMods {
	std::optional<float> rainDivisor;
	bool killAmbient = false;
	std::optional<float> sunPitch, sunYaw;
};

inline LightingState modulate(LightingState s, const LightMods& m)
{
	if (m.sunPitch || m.sunYaw) {
		if (m.sunPitch) s.pitch = *m.sunPitch;
		if (m.sunYaw) s.yaw = *m.sunYaw;
		sunDirFromAngles(s.pitch, s.yaw, s.sunDir);
	}
	if (m.rainDivisor) {
		const float d = *m.rainDivisor;
		for (auto& v : s.sun) v = static_cast<uint8_t>(v / d);
		for (auto& v : s.amb) v = static_cast<uint8_t>(v / d);
	}
	if (m.killAmbient) s.amb[0] = s.amb[1] = s.amb[2] = 0xFF;
	return s;
}

// The camera's published state with a curve: the curve over the authored base at t, then the modulators. Camera::currentLighting,
// publishLight and the bake reference all call this, so there is exactly one composition. A null curve gives modulate(base).
inline LightingState compose(const TodCurve* curve, float t, const LightingState& base, const LightMods& mods)
{
	return modulate(evaluate(curve, t, base), mods);
}

// The load-time bakes (ground burn-in, forest light, scene-lighting capture) bake against the AUTHORED state, never a live one:
// a bake is a fixed reference (the plan's "bake reference = authored"; T3/T6 decide how a live mission re-bakes). Identity today;
// it exists so every bake site names the intent and the doctest can pin it.
inline const LightingState& bakeReference(const LightingState& authored) { return authored; }

// Camera::getLightRed/Green/Blue's arithmetic for one channel (truncation, the night-ambient floor, clamp to 255).
inline unsigned char flatLightByte(unsigned char sun, unsigned char amb, float intensity, long floorV)
{
	if (intensity > 0.0f) {
		long r = static_cast<long>(sun * intensity + amb);
		r = std::max(r, floorV);
		return r < 0xff ? static_cast<unsigned char>(r) : static_cast<unsigned char>(0xff);
	}
	return amb < floorV ? static_cast<unsigned char>(floorV) : amb;
}

} // namespace mc2::lighting
