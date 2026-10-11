// BURNIN-DISK-JPEG: stb_image decode TU for the .burnin.jpg colormap sidecar.
//
// The terrain burn-in colormaps (<name>.burnin.tga, 5120x5120, ~75-108MB each)
// are stored as JPEG (<name>.burnin.jpg, q90, ~4MB) by tools/mc2texcook/
// burnin_jpeg.py. terrtxm2.cpp decodes them on load via the wrapper below.
//
// We own STB_IMAGE_IMPLEMENTATION here for the VENDORED 3rdparty/stb header.
// Assimp vendors its OWN copy (3rdparty/assimp/contrib/stb/stb_image.h) and
// defines STB_IMAGE_IMPLEMENTATION inside its own static archive
// (3rdparty/assimp/code/Common/Assimp.cpp), so there is no ODR/symbol clash:
// the two implementations live in different translation units / archives and
// stb's functions are file-static (stbi__*) plus the public stbi_* set, which
// the linker resolves per-archive. mclib links assimp PRIVATE.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO          // we feed bytes from memory only
#define STBI_ONLY_JPEG         // burnin sidecars are JPEG only
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "../3rdparty/stb/stb_image.h"

extern "C" {

// Decode a JPEG byte buffer into a freshly malloc'd RGB (3-channel, 8-bit)
// pixel array. Returns NULL on failure. Caller frees with
// BurninJpeg_FreePixels(). Width/height are written on success.
unsigned char* BurninJpeg_DecodeRGB(const unsigned char* data, int dataLen,
                                    int* outW, int* outH)
{
    int w = 0, h = 0, comp = 0;
    // force 3 channels (RGB) regardless of source comp count.
    unsigned char* px = stbi_load_from_memory(data, dataLen, &w, &h, &comp, 3);
    if (!px)
        return nullptr;
    if (outW) *outW = w;
    if (outH) *outH = h;
    return px;
}

void BurninJpeg_FreePixels(unsigned char* px)
{
    if (px)
        stbi_image_free(px);
}

} // extern "C"
