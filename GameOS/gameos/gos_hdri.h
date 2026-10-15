#ifndef GAMEOS_GOS_HDRI_H
#define GAMEOS_GOS_HDRI_H

#include <GL/glew.h>

#include <cstdint>
#include <utility>
#include <vector>

// Load an OpenEXR (.exr) equirectangular image as a GL_RGBA16F 2D texture.
// Returns the GL texture name on success, or 0 on any failure
// (file missing, decode error, GL upload error).
//
// Caller owns the returned texture and must glDeleteTextures() it.
//
// Failure is logged to stderr using the project's [HDRI_SKY v1]
// schema via std::fprintf(stderr, ...). No printf, no PAUSE macros.
//
// Sampler state is configured on the returned texture:
//   GL_TEXTURE_WRAP_S/T = GL_CLAMP_TO_EDGE
//   GL_TEXTURE_MIN_FILTER = GL_LINEAR (no mips)
//   GL_TEXTURE_MAG_FILTER = GL_LINEAR
//
// HDRI-SKY Item 2 (sun scan): if outSunAzGL / outSunValid are non-null, the
// loader scans the decoded equirect (before freeing the pixel buffer) for the
// dominant above-horizon light source and writes its GL-equirect azimuth in
// radians (the same atan(worldDir.z, worldDir.x) convention used by
// hdri_skybox.frag) to *outSunAzGL, setting *outSunValid=true. On any failure
// or empty scan *outSunValid is set false and *outSunAzGL left untouched. The
// scan is read-only on GL state and adds no GL calls.
//
// outCpu: when non-null, also receives the uploaded bytes as CPU data (cleared on failure), so a backend that uploads the
// HDRI itself (the Vulkan sky and water islands) never reads the GL texture. The RGBA16F path builds the mip chain on the
// CPU with a 2x2 box filter, the filter glGenerateMipmap applies to a power-of-two image.
GLuint loadHdriTexture(const char* path,
                       float* outSunAzGL = nullptr,
                       bool*  outSunValid = nullptr,
                       struct HdriCpuImage* outCpu = nullptr);

// The decoded HDRI as the GL texture holds it, mip 0 first. No GL here.
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
