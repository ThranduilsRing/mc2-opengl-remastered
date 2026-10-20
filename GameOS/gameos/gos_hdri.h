#ifndef GAMEOS_GOS_HDRI_H
#define GAMEOS_GOS_HDRI_H


#include <cstdint>
#include <utility>
#include <vector>

// Retain decoded HDRI bytes and the authored sun azimuth; no context or GPU object is required.
bool decodeHdriSource(const char* path, float* outSunAzGL, bool* outSunValid, struct HdriCpuImage* outCpu);

// The decoded HDRI as supplied to native residency, mip 0 first.
struct HdriCpuImage {
    enum class Format { Rgba16f, Bc6hUfloat, Bc6hSfloat };
    Format format = Format::Rgba16f;
    std::vector<std::vector<uint8_t>> levels;   // RGBA16F: w*h*8 bytes per level; BC6H: the compressed mip 0 only
    std::vector<std::pair<int, int>> dims;       // width, height per level
    uint64_t generation = 0;                     // set by the owner, bumps on every (re)load; 0 = empty
};

// The CPU copy of the current sky HDRI (owned by the post-process object); null when there is none (no HDRI, or the
// copy was not requested because no Vulkan consumer is armed).
const HdriCpuImage* gos_GetHdriCpuImage();

#endif  // GAMEOS_GOS_HDRI_H
