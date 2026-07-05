// TERRAIN-CONTROLMAP-SAMPLE-1: stb_image decode TU for the authored control-map
// sidecar (data/missions/<stem>.beauty/control_map.png, RGBA8).
//
// Separate TU from mclib/burnin_jpeg_decode.cpp because that TU already defines
// STB_IMAGE_IMPLEMENTATION with STBI_ONLY_JPEG (a single stb_image implementation
// build cannot mix STBI_ONLY_* restrictions across call sites in the same TU).
//
// STB_IMAGE_STATIC: burnin_jpeg_decode.cpp and this TU both link into the SAME
// static archive (mclib.lib), unlike Assimp's vendored copy which lives in its
// own archive — two PUBLIC stbi_* definitions in the same archive is an ODR/
// LNK2005 clash (proven: initial build without STB_IMAGE_STATIC failed link
// with duplicate stbi_load_from_memory et al). STB_IMAGE_STATIC gives this
// TU's stb_image symbols internal linkage so they can coexist with
// burnin_jpeg_decode.cpp's copy in the same archive; ControlMapPng_DecodeRGBA/
// _FreePixels (this TU's own extern "C" wrappers) remain the only externally
// visible entry points.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO          // we feed bytes from memory only
#define STBI_ONLY_PNG          // control-map sidecars are PNG only
#include "../3rdparty/stb/stb_image.h"

extern "C" {

// Decode a PNG byte buffer into a freshly malloc'd RGBA (4-channel, 8-bit)
// pixel array. Returns NULL on failure. Caller frees with
// ControlMapPng_FreePixels(). Width/height are written on success.
unsigned char* ControlMapPng_DecodeRGBA(const unsigned char* data, int dataLen,
                                        int* outW, int* outH)
{
    int w = 0, h = 0, comp = 0;
    // force 4 channels (RGBA) regardless of source comp count.
    unsigned char* px = stbi_load_from_memory(data, dataLen, &w, &h, &comp, 4);
    if (!px)
        return 0;
    if (outW) *outW = w;
    if (outH) *outH = h;
    return px;
}

void ControlMapPng_FreePixels(unsigned char* px)
{
    if (px)
        stbi_image_free(px);
}

} // extern "C"
