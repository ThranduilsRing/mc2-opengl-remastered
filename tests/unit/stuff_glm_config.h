#pragma once

// Scalar GLM fixes the candidate implementation independently of host SIMD support.
#ifdef GLM_VERSION
#error Include stuff_glm_config.h before any GLM header
#endif
#define GLM_FORCE_PURE
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#if GLM_VERSION_MAJOR != 1 || GLM_VERSION_MINOR != 0 || GLM_VERSION_PATCH != 1
#error The identity corpus is pinned to GLM 1.0.1
#endif
