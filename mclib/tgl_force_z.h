// forceZ is authored in forward-Z terms (0 = front, 1 = back: spinning building parts 0.00001, sky 0.99999).
// The camera clip depth is reverse-Z (near = 1, far = 0), so it is mirrored here, once, for every path.
#pragma once
#include "math/glm_config.h"

namespace mc2_tgl {


// Post-divide clip depth for an authored forceZ.
inline float forceZClipDepth(float forceZ) { return 1.0f - forceZ; }

} // namespace mc2_tgl
