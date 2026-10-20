// CPU profile-name/index protocol for native mech instance metadata.
// Index 0 is the default/passthrough profile. No backend handles or texture storage.
#pragma once
#include <cstdint>
namespace gos_materials {
void init();
void shutdown();
uint32_t getProfileIndex(const char* name);
uint32_t profileCount();
}
