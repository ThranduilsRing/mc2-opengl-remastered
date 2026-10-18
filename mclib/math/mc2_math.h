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

} // namespace mc2::math
