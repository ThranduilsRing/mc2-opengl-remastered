// gos_screenshot.cpp - Shared TGA capture helper.
// Body lifted from gos_validate.cpp (writeScreenshotTGA, was static at :73-99
// before extraction). No behavior change; only linkage + return-type widened
// from void to bool so future callers can detect fopen failure.
#include "gos_screenshot.h"

#include <cstdio>
#include <GL/glew.h>

namespace gos { namespace screenshot {

bool writeTGA(const char* path, int w, int h) {
    unsigned char* pixels = new unsigned char[w * h * 3];
    // FREE-RESIZE-1: GL_PACK_ALIGNMENT defaults to 4; a 3-byte-per-pixel read
    // at a width whose row size is not 4-aligned (any free-resized window)
    // gets per-row padding the tight TGA write then misinterprets — the
    // classic diagonal-shear screenshot. Force tight packing, restore after.
    GLint prevPack = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &prevPack);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_PACK_ALIGNMENT, prevPack);

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "VALIDATE: Failed to write screenshot to %s\n", path);
        delete[] pixels;
        return false;
    }

    // TGA header: uncompressed true-color
    unsigned char header[18] = {};
    header[2] = 2;
    header[12] = w & 0xFF;
    header[13] = (w >> 8) & 0xFF;
    header[14] = h & 0xFF;
    header[15] = (h >> 8) & 0xFF;
    header[16] = 24;

    fwrite(header, 1, 18, f);
    fwrite(pixels, 1, w * h * 3, f);
    fclose(f);
    delete[] pixels;

    fprintf(stderr, "VALIDATE: Screenshot saved to %s (%dx%d)\n", path, w, h);
    return true;
}

}}  // namespace gos::screenshot
