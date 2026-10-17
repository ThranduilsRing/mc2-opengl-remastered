#pragma once
// The one place GLM is configured (C++23 step 5, Stuff:: math -> glm). Include it instead of any <glm/...> header.
// WHAT: scalar (pure) GLM, pinned to the vendored 1.0.1 (3rdparty/glm).
// WHY: a SIMD code path or another GLM version can round differently from the corpus that proved the ported operations
// bit-identical (tests/unit/test_stuff_glm_identity.cpp); every translation unit must see the same configuration.
#ifdef GLM_VERSION
#error Include math/glm_config.h before any GLM header
#endif
#define GLM_FORCE_PURE
// Relative to this header, so every target that compiles an mclib/gosfx/mlr source (the game, the editor, the tools, the unit
// tier) reaches the vendored copy without an include directory of its own; GLM's own includes are relative too.
#include "../../3rdparty/glm/glm/glm.hpp"
#include "../../3rdparty/glm/glm/gtc/quaternion.hpp"
#include "../../3rdparty/glm/glm/gtc/matrix_inverse.hpp"

#if GLM_VERSION_MAJOR != 1 || GLM_VERSION_MINOR != 0 || GLM_VERSION_PATCH != 1
#error The Stuff/GLM identity corpus is pinned to GLM 1.0.1
#endif
