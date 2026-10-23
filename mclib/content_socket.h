#pragma once
#include "math/glm_config.h"
#include <string>

namespace mc2::content {
// TG object space and world units, before the appearance's instance transform.
struct Socket {
    std::string name, kind = "light", parent, type = "point", intensityClass;
    glm::vec3 position{0}, direction{0, 0, -1}, colour{1};
    float range = 0, outerConeDegrees = 0;
    bool active = true, lightsOutOnDestroy = true;
};
}
