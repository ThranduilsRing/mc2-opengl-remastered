// CPU profile-name indices used by native mech instance metadata.
#include "mc2_czstring.h"
#include "gos_materials.h"
#include "Gates/gate.h"
#include "mc2fmt.h"
#include <string>
#include <unordered_map>

namespace gos_materials {
namespace {
bool initialized = false;
std::unordered_map<std::string, uint32_t> profiles;
void add(mc2::czstring name) { profiles.emplace(name, static_cast<uint32_t>(profiles.size())); }
}
void init() {
    if (initialized) return;
    initialized = true;
    if (!mc2::gate::get<"MC2_MATERIAL_GPU">()) return;
    add("default");
    if (mc2::gate::is<"MC2_MECH_SURFACE_MATERIAL", "metal061b">()) {
        add("metal061b"); add("paintedmetal003");
    } else if (mc2::gate::is<"MC2_MECH_SURFACE_MATERIAL", "painted_subtle">()) {
        add("painted_subtle");
    }
    mc2::fmt::print_err("[GOS_MATERIALS] init complete: {:d} CPU profile indices\n",
                       mc2::fmt::as_size(profiles.size()));
}
void shutdown() { profiles.clear(); initialized = false; }
uint32_t getProfileIndex(mc2::czstring name) {
    if (!name) return 0;
    const auto it = profiles.find(name);
    return it == profiles.end() ? 0 : it->second;
}
uint32_t profileCount() { return static_cast<uint32_t>(profiles.size()); }
}
