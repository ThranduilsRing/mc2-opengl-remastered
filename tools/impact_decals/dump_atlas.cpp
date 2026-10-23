// Dumps the procedural impact decal atlas (RenderCore/ImpactDecalAtlas.h) as PPM/PGM images for review:
// per layer a surface image (albedo ratio shaded on mid grey, emissive in red, coverage as alpha over grey) and the normal map.
// Build: cl /std:c++20 /O2 /EHsc tools/impact_decals/dump_atlas.cpp /Fe:<out>.exe ; run: <out>.exe <outdir>
#include "../../RenderCore/ImpactDecalAtlas.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const impact::atlas::Layers a = impact::atlas::generate();
    const uint32_t e = impact::atlas::kSize;
    for (uint32_t l = 0; l < impact::kTileCount; ++l) {
        for (int kind = 0; kind < 2; ++kind) {
            const std::string path = dir + "/layer" + std::to_string(l) + (kind ? "_normal.ppm" : "_surface.ppm");
            FILE* f = std::fopen(path.c_str(), "wb");
            if (!f) return 1;
            std::fprintf(f, "P6\n%u %u\n255\n", e, e);
            for (uint32_t i = 0; i < e * e; ++i) {
                const uint8_t* s = a.surface.data() + (size_t(l) * e * e + i) * 4;
                const uint8_t* n = a.normal.data() + (size_t(l) * e * e + i) * 4;
                unsigned char rgb[3];
                if (kind) { rgb[0] = n[0]; rgb[1] = n[1]; rgb[2] = 255; }
                else {
                    // Preview over a 0.6 grey surface: colour = grey * albedoRatio * coverage-blend, plus emissive in orange.
                    const float cov = s[3] / 255.0f, alb = s[0] / 255.0f * 2.0f, em = s[1] / 255.0f;
                    const float base = 0.6f * (1.0f - cov + cov * alb);
                    const float r = base + em * 0.9f, g = base + em * 0.4f, b = base;
                    rgb[0] = static_cast<unsigned char>(std::min(1.0f, r) * 255); rgb[1] = static_cast<unsigned char>(std::min(1.0f, g) * 255);
                    rgb[2] = static_cast<unsigned char>(std::min(1.0f, b) * 255);
                }
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
        }
    }
    return 0;
}
