#pragma once
// The one place GLM is configured (C++23 step 5, Stuff:: math -> glm). Include it instead of any <glm/...> header.
// WHAT: scalar (pure) GLM, pinned to the vendored 1.0.1 (3rdparty/glm).
// WHY: a SIMD code path or another GLM version can round differently from the corpus that proved the ported operations
// bit-identical (tests/unit/test_stuff_glm_identity.cpp); every translation unit must see the same configuration.
#ifdef GLM_VERSION
#error Include math/glm_config.h before any GLM header
#endif
#define GLM_FORCE_PURE
// Default constructors initialize (vectors to 0, matrices and quaternions to identity). The Stuff types left members
// uninitialized; an ex-AffineMatrix4D glm::mat4 has a column 3 Stuff never had, which must be (0, 0, 0, 1) before a product.
#define GLM_FORCE_CTOR_INIT
#if defined(GLM_FORCE_QUAT_DATA_WXYZ) || defined(GLM_FORCE_QUAT_DATA_XYZW)
#error glm::quat must keep the x, y, z, w storage: the anim cache (mclib/msl_anim.cpp) stores quaternions as those bytes
#endif
// Relative to this header, so every target that compiles an mclib/gosfx/mlr source (the game, the editor, the tools, the unit
// tier) reaches the vendored copy without an include directory of its own; GLM's own includes are relative too.
#include "../../3rdparty/glm/glm/glm.hpp"
#include "../../3rdparty/glm/glm/gtc/quaternion.hpp"
#include "../../3rdparty/glm/glm/gtc/matrix_inverse.hpp"
#include "../../3rdparty/glm/glm/gtc/type_ptr.hpp"

#if GLM_VERSION_MAJOR != 1 || GLM_VERSION_MINOR != 0 || GLM_VERSION_PATCH != 1
#error The Stuff/GLM identity corpus is pinned to GLM 1.0.1
#endif

// The sim-math numerical contract (pinned here so a build change fails to compile instead of drifting the SIMID chain):
// scalar GLM (no SIMD, no aligned gentypes), and /fp:precise with no FMA contraction and no /fp:fast. What
// "bit-identical" promises: the same exe repeats, across worker counts and across save/load; NOT across compilers or platforms.
static_assert(GLM_CONFIG_SIMD == GLM_DISABLE, "sim math needs scalar GLM (GLM_FORCE_PURE)");
static_assert(GLM_CONFIG_ALIGNED_GENTYPES == GLM_DISABLE, "aligned GLM gentypes change layout and code paths");
#if defined(GLM_FORCE_INTRINSICS) || defined(GLM_FORCE_DEFAULT_ALIGNED_GENTYPES) || defined(GLM_FORCE_FMA)
#error GLM must not force intrinsics, aligned gentypes or FMA (sim-math contract)
#endif
// The floating-point flags are enforced per command line, for every TU whether or not it includes GLM, under MSVC and clang-cl:
// cmake/Mc2SimFpContract.cmake sets them, scripts/check-sim-fp-contract.py checks the compile database.

// Byte formats old files read back (owner ruling 2026-10-18: the only exact rows of step 5): E1 the quaternion of the anim
// cache (x, y, z, w), E2 the 48-byte matrix records (mclib/stuff/glmstream.hpp), E3 the 12-byte vectors of the save *Data
// structs. The byte order is asserted by the corpus (tests/unit/test_glm_math_identity.cpp).
static_assert(sizeof(glm::quat) == 16 && sizeof(glm::vec3) == 12 && sizeof(glm::mat4) == 64, "E1-E3 byte formats");
