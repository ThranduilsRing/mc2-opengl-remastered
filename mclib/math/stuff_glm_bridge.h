#pragma once
// Stuff:: <-> glm conversions for the step 5 port (deleted with the last Stuff:: math type). Index mapping: glm m[column][row] ==
// Stuff M(row, column). Stuff multiplies ROW vectors (p' = p * M), so the glm equivalent is glm::vec4(p, 1) * m, and a Stuff
// inverse is the glm inverse. Matrix4D and AffineMatrix4D store entries[(column << 2) + row]: the glm column-major layout.
#include "glm_config.h"
#include "mc2_math.h"

#include <stuff/stuff.hpp>

#include <cstring>

namespace mc2::math {

inline glm::vec3 toGlm(const Stuff::Vector3D& v) { return glm::vec3(v.x, v.y, v.z); }
inline Stuff::Vector3D toVector3D(const glm::vec3& v) { return Stuff::Vector3D(v.x, v.y, v.z); }
inline Stuff::Point3D toPoint3D(const glm::vec3& v) { return Stuff::Point3D(v.x, v.y, v.z); }

inline glm::mat4 toGlm(const Stuff::Matrix4D& m)
{
	glm::mat4 g;
	static_assert(sizeof(g) == sizeof(m.entries));
	std::memcpy(&g, m.entries, sizeof(g));
	return g;
}

// The implicit fourth column of an affine matrix is (0, 0, 0, 1).
inline glm::mat4 toGlm(const Stuff::AffineMatrix4D& m)
{
	glm::mat4 g(1.0f);
	static_assert(sizeof(m.entries) == 3 * sizeof(glm::vec4));
	std::memcpy(&g, m.entries, sizeof(m.entries));
	return g;
}

inline void fromGlm(Stuff::Matrix4D& out, const glm::mat4& g) { std::memcpy(out.entries, &g, sizeof(out.entries)); }

// Writes the first three columns; the fourth ((0, 0, 0, 1) for an affine source) is dropped. A LinearMatrix4D is an
// AffineMatrix4D: its entries are written as they are, with no re-orthogonalisation.
inline void fromGlm(Stuff::AffineMatrix4D& out, const glm::mat4& g) { std::memcpy(out.entries, &g, sizeof(out.entries)); }

} // namespace mc2::math
