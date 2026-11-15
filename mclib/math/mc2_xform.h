#pragma once
// The engine's transform math on glm types (C++23 step 5, the matrices / quaternion / Euler / OBB / vector tail).
// WHAT: the matrix types are glm::mat4 holding the STUFF matrix: entry (row r, column c) is m[c][r], points are ROW vectors
// (p' = p * M), the translation is ROW 3 (m[0][3], m[1][3], m[2][3]) and column 3 is (0, 0, 0, 1) for an affine matrix.
// WHY these functions: glm's own helpers assume the column convention (glm::translate writes m[3], glm::mat4_cast is the
// transpose of the engine's rotation, m * v is the transposed action), so every operation whose meaning depends on the
// convention is here, and the lint row glm_convention (scripts/check-glm-convention.py) keeps the column-convention helpers
// out of engine code. Bodies that had a Stuff original are that body (a convention adapter costs the same written verbatim).
// Everything not here is plain glm (owner ruling 2026-10-18: P/R by default, exact only for byte formats).
#include "glm_config.h"
#include "mc2_math.h"
#include "sim_math.h"

#include <cmath>
#include <cstddef>
#include <utility>

namespace mc2::math {

// Stuff::Axes: the row / column numbers of the engine matrices.
enum class Axes : int { X_Axis = 0, Y_Axis = 1, Z_Axis = 2, W_Axis = 3 };
// Matrix row / column indices: the engine uses these as plain integers (array subscripts, -1 sentinels).
inline constexpr int X_Axis = std::to_underlying(Axes::X_Axis);
inline constexpr int Y_Axis = std::to_underlying(Axes::Y_Axis);
inline constexpr int Z_Axis = std::to_underlying(Axes::Z_Axis);
inline constexpr int W_Axis = std::to_underlying(Axes::W_Axis);

// The engine's base coordinate system (Stuff vector3d.hpp macros): forward +Z, right -X, up +Y.
inline constexpr glm::vec3 Forward{0.0f, 0.0f, 1.0f};
inline constexpr glm::vec3 Backward{0.0f, 0.0f, -1.0f};
inline constexpr glm::vec3 Right{-1.0f, 0.0f, 0.0f};
inline constexpr glm::vec3 Left{1.0f, 0.0f, 0.0f};
inline constexpr glm::vec3 Up{0.0f, 1.0f, 0.0f};
inline constexpr glm::vec3 Down{0.0f, -1.0f, 0.0f};

inline constexpr float SmallValue = 1e-4f;	// == Stuff SMALL
inline bool smallEnough(float x, float e = SmallValue) { return std::fabs(x) <= e; }
inline bool closeEnough(float a, float b, float e = SmallValue) { return std::fabs(a - b) <= e; }
inline bool smallEnough(const glm::vec3& v, float e = SmallValue)
{
	return smallEnough(v.x, e) && smallEnough(v.y, e) && smallEnough(v.z, e);
}
inline bool closeEnough(const glm::vec3& a, const glm::vec3& b, float e = SmallValue) { return smallEnough(a - b, e); }
inline float lengthSquared(const glm::vec3& v) { return glm::dot(v, v); }
inline bool closeEnough(const glm::mat4& a, const glm::mat4& b, float e = SmallValue)
{
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			if (!closeEnough(a[c][r], b[c][r], e))
				return false;
	return true;
}
inline bool closeEnough(const glm::quat& a, const glm::quat& b, float e = SmallValue)
{
	return closeEnough(a.x, b.x, e) && closeEnough(a.y, b.y, e) && closeEnough(a.z, b.z, e) && closeEnough(a.w, b.w, e);
}

// The former Stuff::EulerAngles: radians, in this member order. glm has no Euler type with this order.
struct Euler {
	float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
	Euler() = default;
	Euler(float p, float y, float r) : pitch(p), yaw(y), roll(r) {}
	float& operator[](size_t i) { return i == 0 ? pitch : (i == 1 ? yaw : roll); }
	float operator[](size_t i) const { return i == 0 ? pitch : (i == 1 ? yaw : roll); }
};
// Stuff EulerAngles::Normalize: every angle into [-Pi, Pi] (wrapAngleMember, the member interval).
inline Euler& normalizeAngles(Euler& e)
{
	e.pitch = wrapAngleMember(e.pitch);
	e.yaw = wrapAngleMember(e.yaw);
	e.roll = wrapAngleMember(e.roll);
	return e;
}
inline bool smallEnough(const Euler& e, float eps = SmallValue)
{
	return smallEnough(e.pitch, eps) && smallEnough(e.yaw, eps) && smallEnough(e.roll, eps);
}
inline bool closeEnough(const Euler& a, const Euler& b, float eps = SmallValue)
{
	return closeEnough(a.pitch, b.pitch, eps) && closeEnough(a.yaw, b.yaw, eps) && closeEnough(a.roll, b.roll, eps);
}

// Quaternions are glm::quat (storage x, y, z, w: the byte order of the anim cache, see glm_config.h). glm's constructor takes
// w first; engine code builds them through quatXYZW so no call site swaps the arguments.
inline glm::quat quatXYZW(float x, float y, float z, float w) { return glm::quat(w, x, y, z); }
inline const glm::quat QuatIdentity{1.0f, 0.0f, 0.0f, 0.0f};

// Stuff::UnitQuaternion::Multiply(a, b): the rotation a then b, which is glm b * a.
inline glm::quat composeQuat(const glm::quat& a, const glm::quat& b) { return b * a; }

// ---- matrix element access in the engine's (row, column) order
inline float& at(glm::mat4& m, int row, int column) { return m[column][row]; }
inline float at(const glm::mat4& m, int row, int column) { return m[column][row]; }

inline glm::vec3 getTranslation(const glm::mat4& m) { return glm::vec3(m[0][3], m[1][3], m[2][3]); }
// Stuff BuildTranslation: writes row 3 only.
inline glm::mat4& buildTranslation(glm::mat4& m, const glm::vec3& p)
{
	m[0][3] = p.x;
	m[1][3] = p.y;
	m[2][3] = p.z;
	return m;
}
inline glm::mat4 translationMatrix(const glm::vec3& p)
{
	glm::mat4 m(1.0f);
	return buildTranslation(m, p);
}

// Stuff BuildRotation(EulerAngles): writes the 3x3 only (row 3 and column 3 are kept).
template<bool Sim = false>
inline glm::mat4& buildRotation(glm::mat4& m, const Euler& angles)
{
	const auto sine = [](float x) { if constexpr (Sim) return mc2::sim::math::sin(x); else return sinf(x); };
	const auto cosine = [](float x) { if constexpr (Sim) return mc2::sim::math::cos(x); else return cosf(x); };
	const float xSine = sine(angles.pitch), xCosine = cosine(angles.pitch), ySine = sine(angles.yaw), yCosine = cosine(angles.yaw),
				zSine = sine(angles.roll), zCosine = cosine(angles.roll);
	at(m, 0, 0) = yCosine * zCosine;
	at(m, 0, 1) = yCosine * zSine;
	at(m, 0, 2) = -ySine;
	at(m, 1, 0) = xSine * ySine * zCosine - xCosine * zSine;
	at(m, 1, 1) = xSine * ySine * zSine + xCosine * zCosine;
	at(m, 1, 2) = xSine * yCosine;
	at(m, 2, 0) = xCosine * ySine * zCosine + xSine * zSine;
	at(m, 2, 1) = xCosine * ySine * zSine - xSine * zCosine;
	at(m, 2, 2) = xCosine * yCosine;
	return m;
}

// Stuff BuildRotation(UnitQuaternion): the 3x3 only. It is the TRANSPOSE of glm::mat4_cast(q) (row convention).
inline glm::mat4& buildRotation(glm::mat4& m, const glm::quat& q)
{
	const float a = q.x * q.y, b = q.y * q.z, c = q.z * q.x, d = q.w * q.x, e = q.w * q.y, f = q.w * q.z, g = q.w * q.w,
				h = q.x * q.x, i = q.y * q.y, j = q.z * q.z;
	at(m, 0, 0) = g + h - i - j;
	at(m, 1, 0) = 2.0f * (a - f);
	at(m, 2, 0) = 2.0f * (c + e);
	at(m, 0, 1) = 2.0f * (f + a);
	at(m, 1, 1) = g - h + i - j;
	at(m, 2, 1) = 2.0f * (b - d);
	at(m, 0, 2) = 2.0f * (c - e);
	at(m, 1, 2) = 2.0f * (b + d);
	at(m, 2, 2) = g - h - i + j;
	return m;
}

inline glm::mat4 rotationMatrix(const Euler& e)
{
	glm::mat4 m(1.0f);
	return buildRotation(m, e);
}
inline glm::mat4 rotationMatrix(const glm::quat& q)
{
	glm::mat4 m(1.0f);
	return buildRotation(m, q);
}

// Stuff UnitQuaternion = LinearMatrix4D: the rotation of the 3x3 (transposed into glm's convention).
inline glm::quat quatFromRotation(const glm::mat4& m) { return glm::normalize(glm::quat_cast(glm::transpose(glm::mat3(m)))); }
// Stuff UnitQuaternion = EulerAngles (it went through the matrix too).
inline glm::quat quatFromEuler(const Euler& e) { return quatFromRotation(rotationMatrix(e)); }
inline glm::quat quatFromEulerSim(const Euler& e) {
	glm::mat4 m(1.0f);
	return quatFromRotation(buildRotation<true>(m, e));
}

// Stuff Matrix4D = AffineMatrix4D / an affine matrix as a full one: column 3 becomes (0, 0, 0, 1).
inline glm::mat4 asAffine(glm::mat4 m)
{
	m[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	return m;
}

// Stuff LinearMatrix4D::Invert: a rigid inverse (transpose the rotation, run the offsets through it).
inline glm::mat4 invertRigid(const glm::mat4& m)
{
	glm::mat4 r(1.0f);
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			at(r, i, j) = at(m, j, i);
	at(r, 3, 0) = -at(m, 3, 0) * at(m, 0, 0) - at(m, 3, 1) * at(m, 0, 1) - at(m, 3, 2) * at(m, 0, 2);
	at(r, 3, 1) = -at(m, 3, 0) * at(m, 1, 0) - at(m, 3, 1) * at(m, 1, 1) - at(m, 3, 2) * at(m, 1, 2);
	at(r, 3, 2) = -at(m, 3, 0) * at(m, 2, 0) - at(m, 3, 1) * at(m, 2, 1) - at(m, 3, 2) * at(m, 2, 2);
	return r;
}

// Stuff LinearMatrix4D::Normalize: rebuild row 2 from rows 0 and 1 (right-handed).
inline glm::mat4& reorthogonalize(glm::mat4& m)
{
	at(m, 0, 2) = at(m, 1, 0) * at(m, 2, 1) - at(m, 1, 1) * at(m, 2, 0);
	at(m, 1, 2) = at(m, 2, 0) * at(m, 0, 1) - at(m, 2, 1) * at(m, 0, 0);
	at(m, 2, 2) = at(m, 0, 0) * at(m, 1, 1) - at(m, 0, 1) * at(m, 1, 0);
	return m;
}

// Stuff AffineMatrix4D::Multiply(m, Point3D): row 3 of m plus p; the other rows of the destination are KEPT (as Stuff did).
inline glm::mat4& multiplyTranslation(glm::mat4& dst, const glm::mat4& m, const glm::vec3& p)
{
	dst[0][3] = m[0][3] + p.x;
	dst[1][3] = m[1][3] + p.y;
	dst[2][3] = m[2][3] + p.z;
	return dst;
}
// Stuff AffineMatrix4D::Multiply(m, Vector3D): scale column c by v[c] (rows 0-3).
inline glm::mat4 scaleColumns(const glm::mat4& m, const glm::vec3& v)
{
	glm::mat4 r = m;
	r[0] = glm::vec4(glm::vec3(m[0]) * v.x, m[0][3] * v.x);
	r[1] = glm::vec4(glm::vec3(m[1]) * v.y, m[1][3] * v.y);
	r[2] = glm::vec4(glm::vec3(m[2]) * v.z, m[2][3] * v.z);
	return r;
}
// Stuff AffineMatrix4D::Multiply(m, UnitQuaternion): m then the rotation q.
inline glm::mat4 multiply(const glm::mat4& m, const glm::quat& q) { return m * rotationMatrix(q); }
// Stuff UnitQuaternion::Multiply(q, LinearMatrix4D).
inline glm::quat multiply(const glm::quat& q, const glm::mat4& m) { return quatFromRotation(rotationMatrix(q) * m); }

// Stuff Get{Local,World}{Forward,...}InWorld: a row (local axis in world) or a column (world axis in local) with the sign macros.
inline glm::vec3 rowOf(const glm::mat4& m, int r) { return glm::vec3(m[0][r], m[1][r], m[2][r]); }
inline glm::vec3 columnOf(const glm::mat4& m, int c) { return glm::vec3(m[c]); }
inline glm::vec3 localForwardInWorld(const glm::mat4& m) { return rowOf(m, Z_Axis); }
inline glm::vec3 localBackwardInWorld(const glm::mat4& m) { return -rowOf(m, Z_Axis); }
inline glm::vec3 localRightInWorld(const glm::mat4& m) { return -rowOf(m, X_Axis); }
inline glm::vec3 localLeftInWorld(const glm::mat4& m) { return rowOf(m, X_Axis); }
inline glm::vec3 localUpInWorld(const glm::mat4& m) { return rowOf(m, Y_Axis); }
inline glm::vec3 localDownInWorld(const glm::mat4& m) { return -rowOf(m, Y_Axis); }
inline glm::vec3 worldForwardInLocal(const glm::mat4& m) { return columnOf(m, Z_Axis); }
inline glm::vec3 worldBackwardInLocal(const glm::mat4& m) { return -columnOf(m, Z_Axis); }
inline glm::vec3 worldRightInLocal(const glm::mat4& m) { return -columnOf(m, X_Axis); }
inline glm::vec3 worldLeftInLocal(const glm::mat4& m) { return columnOf(m, X_Axis); }
inline glm::vec3 worldUpInLocal(const glm::mat4& m) { return columnOf(m, Y_Axis); }
inline glm::vec3 worldDownInLocal(const glm::mat4& m) { return -columnOf(m, Y_Axis); }

// Stuff LinearMatrix4D::AlignLocalAxisToWorldVector, verbatim on glm (cards facing the camera, effect orientation).
inline void alignLocalAxisToWorldVector(glm::mat4& m, const glm::vec3& world_target, int pointing_axis, int rotating_axis,
	int minor_axis = -1)
{
	if (smallEnough(lengthSquared(world_target)))
		return;
	glm::vec3 rotation_vector = rowOf(m, rotating_axis), pointing_vector, minor_vector;
	const glm::vec3 temp = glm::cross(rotation_vector, world_target);
	if (minor_axis == -1) {
		minor_axis = 3 - pointing_axis - rotating_axis;
		minor_vector = glm::normalize(temp);
		if ((rotating_axis + 1) % 3 == pointing_axis)
			pointing_vector = glm::cross(minor_vector, rotation_vector);
		else {
			minor_vector = -minor_vector;
			pointing_vector = glm::cross(rotation_vector, minor_vector);
		}
	} else {
		if (smallEnough(lengthSquared(temp))) {
			if (glm::dot(world_target, rotation_vector) > 0.0f) {
				pointing_vector = rowOf(m, rotating_axis);
				rotation_vector = -rowOf(m, pointing_axis);
			} else {
				pointing_vector = -rowOf(m, rotating_axis);
				rotation_vector = rowOf(m, pointing_axis);
			}
			minor_vector = rowOf(m, minor_axis);
		} else {
			pointing_vector = glm::normalize(world_target);
			minor_vector = glm::normalize(temp);
			if ((rotating_axis + 1) % 3 == pointing_axis)
				rotation_vector = glm::cross(pointing_vector, minor_vector);
			else {
				minor_vector = -minor_vector;
				rotation_vector = glm::cross(minor_vector, pointing_vector);
			}
		}
	}
	for (int c = 0; c < 3; ++c) {
		m[c][pointing_axis] = pointing_vector[c];
		m[c][rotating_axis] = rotation_vector[c];
		m[c][minor_axis] = minor_vector[c];
	}
}

// Stuff Matrix4D::SetPerspective / GetPerspective, verbatim (the engine's clip convention; glm::perspective is another one).
// LEFT_AXIS = X, UP_AXIS = Y, FORWARD_AXIS = Z; APPLY_LEFT/UP/FORWARD_SIGN are +.
inline void setPerspective(glm::mat4& m, float near_clip, float far_clip, float left_clip, float right_clip, float top_clip,
	float bottom_clip)
{
	const float horizontal_range = 1.0f / (left_clip - right_clip), vertical_range = 1.0f / (top_clip - bottom_clip),
				depth_range = 1.0f / (far_clip - near_clip);
	at(m, X_Axis, X_Axis) = near_clip * horizontal_range;
	at(m, X_Axis, Y_Axis) = 0.0f;
	at(m, X_Axis, Z_Axis) = 0.0f;
	at(m, X_Axis, 3) = 0.0f;
	at(m, Y_Axis, X_Axis) = 0.0f;
	at(m, Y_Axis, Y_Axis) = near_clip * vertical_range;
	at(m, Y_Axis, Z_Axis) = 0.0f;
	at(m, Y_Axis, 3) = 0.0f;
	at(m, Z_Axis, X_Axis) = -right_clip * horizontal_range;
	at(m, Z_Axis, Y_Axis) = -bottom_clip * vertical_range;
	at(m, Z_Axis, Z_Axis) = far_clip * depth_range;
	at(m, Z_Axis, 3) = 1.0f;
	at(m, 3, X_Axis) = 0.0f;
	at(m, 3, Y_Axis) = 0.0f;
	at(m, 3, Z_Axis) = -far_clip * near_clip * depth_range;
	at(m, 3, 3) = 0.0f;
}
inline void getPerspective(const glm::mat4& m, float* near_clip, float* far_clip, float* left_clip, float* right_clip,
	float* top_clip, float* bottom_clip)
{
	const float m3z = at(m, 3, Z_Axis), mzz = at(m, Z_Axis, Z_Axis), mzx = at(m, Z_Axis, X_Axis), mzy = at(m, Z_Axis, Y_Axis),
				mxx = at(m, X_Axis, X_Axis), myy = at(m, Y_Axis, Y_Axis);
	if (near_clip)
		*near_clip = -m3z / mzz;
	if (far_clip)
		*far_clip = m3z / (1.0f - mzz);
	if (left_clip)
		*left_clip = ((mzx - 1.0f) * m3z) / (mzz * mxx);
	if (right_clip)
		*right_clip = (mzx * m3z) / (mzz * mxx);
	if (top_clip)
		*top_clip = ((mzy - 1.0f) * m3z) / (myy * mzz);
	if (bottom_clip)
		*bottom_clip = (mzy * m3z) / (myy * mzz);
}
inline void setPerspective(glm::mat4& m, float near_clip, float far_clip, float horizontal_fov, float height_to_width)
{
	const float width = static_cast<float>(near_clip * tan(horizontal_fov * 0.5f));
	const float height = width * height_to_width;
	setPerspective(m, near_clip, far_clip, width, -width, height, -height);
}
inline void getPerspective(const glm::mat4& m, float* nearClip, float* farClip, float* horizontal_fov, float* height_to_width)
{
	float near_clip, far_clip, left_clip, right_clip, top_clip, bottom_clip;
	getPerspective(m, &near_clip, &far_clip, &left_clip, &right_clip, &top_clip, &bottom_clip);
	if (nearClip)
		*nearClip = near_clip;
	if (farClip)
		*farClip = far_clip;
	if (horizontal_fov)
		*horizontal_fov = 2.0f * static_cast<float>(atan(left_clip / near_clip));
	if (height_to_width)
		*height_to_width = top_clip / left_clip;
}

// Stuff EulerAngles = LinearMatrix4D, verbatim (both gimbal branches); EulerAngles = UnitQuaternion goes through the matrix.
inline Euler eulerFromRotation(const glm::mat4& m)
{
	constexpr float HalfPi = Pi * 0.5f;
	const float ySine = -at(m, 0, 2);
	if (closeEnough(ySine, 1.0f, 0.0001f))
		return Euler(static_cast<float>(atan2(at(m, 1, 0), at(m, 2, 0))), HalfPi, 0.0f);
	if (closeEnough(ySine, -1.0f, 0.0001f))
		return Euler(static_cast<float>(atan2(-at(m, 1, 0), -at(m, 2, 0))), -HalfPi, 0.0f);
	float yCosine = std::sqrt(1.0f - ySine * ySine);
	float one_y_cosine = 1.0f / yCosine;
	float pCosine = at(m, 2, 2) * one_y_cosine;
	if (pCosine < 0.0f) {
		pCosine = -pCosine;
		yCosine = -yCosine;
		one_y_cosine = -one_y_cosine;
	}
	const float pSine = at(m, 1, 2) * one_y_cosine, rSine = at(m, 0, 1) * one_y_cosine, rCosine = at(m, 0, 0) * one_y_cosine;
	return Euler(static_cast<float>(atan2(pSine, pCosine)), static_cast<float>(atan2(ySine, yCosine)),
		static_cast<float>(atan2(rSine, rCosine)));
}
inline Euler eulerFromQuat(const glm::quat& q) { return eulerFromRotation(rotationMatrix(q)); }

// Stuff Vector3D = UnitQuaternion (rotation vector: angle * axis) and UnitQuaternion = Vector3D.
inline glm::vec3 rotationVectorFromQuat(const glm::quat& q)
{
	const float sine_of_half = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
	if (smallEnough(sine_of_half))
		return glm::vec3(0.0f);
	const float len = static_cast<float>(atan2(sine_of_half, q.w)) * 2.0f / sine_of_half;
	return glm::vec3(q.x * len, q.y * len, q.z * len);
}
inline glm::quat quatFromRotationVector(const glm::vec3& v)
{
	float rotation = glm::length(v);
	if (smallEnough(rotation))
		return QuatIdentity;
	const float half = 0.5f * wrapAngleStatic(rotation);
	rotation = sinf(half) / rotation;
	return quatXYZW(v.x * rotation, v.y * rotation, v.z * rotation, cosf(half));
}

// The former Stuff::OBB: an oriented box with its bounding-sphere radius (gosFX effect bounds). 80 bytes; streamed through
// mclib/stuff/glmstream.hpp in the old 64-byte record.
struct Obb {
	glm::mat4 localToParent{1.0f};
	glm::vec3 axisExtents{0.0f};
	float sphereRadius = 0.0f;
	Obb() = default;
	Obb(const glm::mat4& origin, const glm::vec3& extents, float radius)
		: localToParent(origin), axisExtents(extents), sphereRadius(radius) {}
	Obb(const glm::mat4& origin, const glm::vec3& extents)
		: localToParent(origin), axisExtents(extents), sphereRadius(glm::length(extents)) {}
};
// Stuff OBB::Multiply(obb, m).
inline Obb multiply(const Obb& obb, const glm::mat4& m) { return Obb(obb.localToParent * m, obb.axisExtents, obb.sphereRadius); }
// Stuff OBB::MultiplySphereOnly: only the center moves.
inline Obb multiplySphereOnly(const Obb& obb, const glm::mat4& m)
{
	Obb r = obb;
	buildTranslation(r.localToParent, glm::vec3(glm::vec4(getTranslation(obb.localToParent), 1.0f) * m));
	return r;
}
// Stuff OBB::Union: the smallest sphere around both (the box part becomes an identity box).
inline Obb unite(const Obb& first, const Obb& second)
{
	const glm::vec3 c1 = getTranslation(first.localToParent), c2 = getTranslation(second.localToParent);
	const float len = glm::length(c2 - c1);
	if (len + first.sphereRadius <= second.sphereRadius || !first.sphereRadius)
		return second;
	if (len + second.sphereRadius <= first.sphereRadius || !second.sphereRadius)
		return first;
	Obb r;
	r.axisExtents = glm::vec3(0.0f);
	buildTranslation(r.localToParent, glm::mix(c1, c2, (len + second.sphereRadius - first.sphereRadius) / (2.0f * len)));
	r.sphereRadius = 0.5f * (len + first.sphereRadius + second.sphereRadius);
	return r;
}

} // namespace mc2::math
