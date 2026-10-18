#pragma once
// Exact math on glm types (C++23 step 5). Each function reproduces the bits of the Stuff:: operation it replaces where plain
// glm does not; tests/unit/test_stuff_glm_identity.cpp asserts every one of them bit-identical over the seeded corpus.
// WHY they exist: the owner rule "the simulation migrates only bit-identical" (2026-10-11, kept per op 2026-10-17); a plain glm
// call would change these results by 1 ULP (divide, trig) or by a different branch interval (angle wrap). Every other
// operation uses plain glm: the corpus proves those identical.
#include "glm_config.h"

#include <cmath>

namespace mc2::math {

inline constexpr float Pi = static_cast<float>(3.14159265358979323846);			// == Stuff::Pi
inline constexpr float TwoPi = static_cast<float>(2.0 * 3.14159265358979323846);	// == Stuff::Two_Pi

// The engine's ROW-vector convention (Stuff p' = p * M) on glm types: a point carries the translation row, a vector does not.
inline glm::vec3 transformPoint(const glm::vec3& p, const glm::mat4& m) { return glm::vec3(glm::vec4(p, 1.0f) * m); }
inline glm::vec3 transformVector(const glm::vec3& v, const glm::mat4& m) { return glm::vec3(glm::vec4(v, 0.0f) * m); }

// Stuff::Vector3D::Divide(v, s): ONE float reciprocal, then three multiplies (glm v / s divides each component).
inline glm::vec3 divide(const glm::vec3& v, float s)
{
	const float r = 1.0f / s;
	return glm::vec3(v.x * r, v.y * r, v.z * r);
}

// Stuff::Vector3D::Lerp(a, b, t): a + t*(b-a) per component (glm::mix is a*(1-t) + b*t, which cancels differently).
inline glm::vec3 lerp(const glm::vec3& a, const glm::vec3& b, float t)
{
	return glm::vec3(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z));
}

// Stuff::Radian::Normalize(Scalar), the static one: double fmod, then into (-Pi, Pi] (-Pi itself becomes +Pi).
inline float wrapAngleStatic(float angle)
{
	float r = static_cast<float>(std::fmod(static_cast<double>(angle), static_cast<double>(TwoPi)));
	if (r > Pi)
		r -= TwoPi;
	else if (r <= -Pi)
		r += TwoPi;
	return r;
}

// Stuff::Radian::Normalize(), the member: as above but into [-Pi, Pi] (-Pi stays -Pi). The two intervals differ on purpose.
inline float wrapAngleMember(float angle)
{
	float r = static_cast<float>(std::fmod(static_cast<double>(angle), static_cast<double>(TwoPi)));
	if (r > Pi)
		r -= TwoPi;
	else if (r < -Pi)
		r += TwoPi;
	return r;
}

// Stuff::Sin/Cos/Arccos/Arctan as compiled where only <math.h> precedes stuff.hpp (the stuff, mlr and gosfx libraries, the
// editor): the DOUBLE CRT function, rounded to float. glm::sin(float) is the float CRT (sinf), 1 ULP apart on ~0.1% of inputs.
// A translation unit that saw <cmath> first (the gameos, mclib and mc2 PCH targets) binds Stuff::Sin to sinf instead, so a port
// there uses glm::sin; a port in a non-PCH library uses these.
inline float sinD(float x) { return static_cast<float>(std::sin(static_cast<double>(x))); }
inline float cosD(float x) { return static_cast<float>(std::cos(static_cast<double>(x))); }
inline float acosD(float x) { return static_cast<float>(std::acos(static_cast<double>(x))); }
inline float atan2D(float y, float x) { return static_cast<float>(std::atan2(static_cast<double>(y), static_cast<double>(x))); }

// The former Stuff::YawPitchRange (S5.13): yaw counter-clockwise about +Y from +Z, pitch (positive looks down), range. A
// polar triple with no glm counterpart; the constructor keeps the three-argument call of the old one.
struct YawPitchRange {
	float yaw = 0.0f, pitch = 0.0f, range = 0.0f;
	YawPitchRange() = default;
	YawPitchRange(float y, float p, float r) : yaw(y), pitch(p), range(r) {}
};

// Stuff::Vector3D = YawPitchRange (vector3d.cpp), verbatim: the float CRT sinf/cosf (== glm::sin/cos, corpus rows), y from
// the pitch, x and z from the horizontal length. glm::euclidean uses another axis convention.
// The former Stuff::ExtentBox (S5.14) as a glm min/max pair. unite == ExtentBox::Union bit for bit: Stuff's Min(a, b) is
// a < b ? a : b == glm::min(b, a) and Max(a, b) is a > b ? a : b == glm::max(b, a); the argument order decides equal values
// of opposite sign (+0/-0) and NaN, so it is reversed on purpose.
struct Aabb {
	glm::vec3 min{0.0f}, max{0.0f};
};
inline Aabb unite(const Aabb& a, const Aabb& b)
{
	Aabb out;
	out.min = glm::min(b.min, a.min);
	out.max = glm::max(b.max, a.max);
	return out;
}

// The former Stuff::Sphere (S5.15): a center and a radius (gosFX debris pieces, tube profile bounds). Data only.
struct Sphere {
	glm::vec3 center{0.0f};
	float radius = 0.0f;
};
static_assert(sizeof(Sphere) == 16, "Stuff::Sphere was a Point3D and a Scalar");

// Stuff::UnitVector3D = Vector3D (Vector3D::Normalize), verbatim: one float reciprocal of the length, then three multiplies.
inline glm::vec3 normalizeExact(const glm::vec3& v)
{
	const float len = 1.0f / std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	return glm::vec3(v.x * len, v.y * len, v.z * len);
}

// The former Stuff::Plane (S5.16): points p with dot(normal, p) == offset (the MLR mesh face planes). Same 16-byte layout.
struct Plane {
	glm::vec3 normal{0.0f};
	float offset = 0.0f;

	// Stuff::Plane::BuildPlane, verbatim: (p1 - p0) x (p2 - p1), normalized, offset at p0.
	void buildPlane(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2)
	{
		const glm::vec3 v1(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z), v2(p2.x - p1.x, p2.y - p1.y, p2.z - p1.z);
		const glm::vec3 axis(v1.y * v2.z - v1.z * v2.y, v1.z * v2.x - v1.x * v2.z, v1.x * v2.y - v1.y * v2.x);
		normal = normalizeExact(axis);
		offset = normal.x * p0.x + normal.y * p0.y + normal.z * p0.z;
	}
	float distanceTo(const glm::vec3& p) const { return normal.x * p.x + normal.y * p.y + normal.z * p.z - offset; }
	bool isSeenBy(const glm::vec3& p) const { return normal.x * p.x + normal.y * p.y + normal.z * p.z > offset; }
};
static_assert(sizeof(Plane) == 16, "Stuff::Plane was a UnitVector3D and a Scalar");

inline glm::vec3 polarToVector(const YawPitchRange& polar)
{
	const float azimuthSine = sinf(polar.yaw), azimuthCosine = cosf(polar.yaw), altitudeSine = sinf(polar.pitch),
		altitudeCosine = cosf(polar.pitch);
	const float y = -polar.range * altitudeSine;
	const float len = polar.range * altitudeCosine;
	return glm::vec3(len * azimuthSine, y, len * azimuthCosine);
}

} // namespace mc2::math
