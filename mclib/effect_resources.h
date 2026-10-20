// Existing effect-library lifecycle, shared by game and editor scene resets.
#pragma once
#include <cstddef>
void InstallEffectStream(const void* data, size_t size);
void ReleaseEffectStream();
namespace mc2::effects {
void shutdown();
void reload();
}
