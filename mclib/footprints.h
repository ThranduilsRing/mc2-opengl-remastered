// Stock mech footfalls, retained from the former crater manager.
#pragma once
#include "math/glm_config.h"

namespace Footprints {
void init();
void destroy();
void add(long shape, const glm::vec3& position, float rotation);
void render();
}
