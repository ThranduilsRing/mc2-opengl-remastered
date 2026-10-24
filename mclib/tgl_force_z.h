// forceZ is authored in forward-Z terms (0 = front, 1 = back: spinning building parts 0.00001, sky 0.99999).
// The camera clip depth is reverse-Z (near = 1, far = 0), so it is mirrored here, once, for every path.
#pragma once
#include <glm/glm.hpp>

namespace mc2_tgl {

inline bool forceZActive(float forceZ) { return forceZ >= 0.0f && forceZ < 1.0f; }

// Post-divide clip depth for an authored forceZ.
inline float forceZClipDepth(float forceZ) { return 1.0f - forceZ; }

// Matrix M (column-major, applied as clip = shape2clip * M * p) giving clip.z = clip.w * forceZClipDepth.
// Output z comes from input w, so the term sits at column 3, row 2: m[3][2].
inline glm::mat4 forceZMatrix(float forceZ)
{
    glm::mat4 m(1.0f);
    m[2][2] = 0.0f;
    m[3][2] = forceZClipDepth(forceZ);
    return m;
}

} // namespace mc2_tgl
