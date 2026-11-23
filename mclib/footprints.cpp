// Stock footprint atlas and persistent ground quads; weapon marks belong to impact_presentation.
#include <algorithm>
#include <cmath>
#include "footprints.h"
#include "Gates/preferences_legacy.h"
#include "texture_assets.h"
#include "camera.h"
#include "terrain.h"
#include "quad.h"
#include "terrain_runtime.h"
#include "paths.h"
#include "cident.h"
#include "mathfunc.h"
#include "../RenderCore/axis_convert.h"
#include <array>

extern bool useFog;

namespace Footprints {
namespace {
struct Print {
    long shape;
    std::array<glm::vec3, 4> corners;
};
std::array<Print, 1000> prints;
size_t count = 0, next = 0;
DWORD atlas = 0xffffffffu;
}

void destroy()
{
    count = next = 0;
    if (atlas != 0xffffffffu)
        mc2::textures::release(atlas);
    atlas = 0xffffffffu;
}

void init()
{
    destroy();
    FullPathFileName path;
    path.init(texturePath, "defaults" PATH_SEPARATOR "feet0001", ".tga");
    atlas = mc2::textures::load(path, gos_Texture_Alpha, gosHint_DisableMipmap);
}

void add(long shape, const glm::vec3& position, float rotation)
{
    if (!useNonWeaponEffects || atlas == 0xffffffffu || shape < 0 || shape >= 64)
        return;
    auto& print = prints[next];
    print.shape = shape;
    print.corners = {glm::vec3(-16, -16, 0), glm::vec3(16, -16, 0),
                     glm::vec3(16, 16, 0), glm::vec3(-16, 16, 0)};
    for (auto& corner : print.corners) {
        OppRotate(corner, rotation);
        corner += position;
        corner.z = TerrainRuntime::decalElevation(corner);
    }
    next = (next + 1) % prints.size();
    if (count < prints.size()) ++count;
}

void render()
{
    if (!useNonWeaponEffects || !eye || atlas == 0xffffffffu)
        return;
    // Resolve the live source each frame: GPU residency may change after init.
    const DWORD texture = mc2::textures::sourceId(atlas);
    for (size_t i = 0; i < count; ++i) {
        auto& print = prints[i];
        bool visible = false;
        for (auto& corner : print.corners) {
            Stuff::Vector4D screen;
            // [PROJECTZ:Both id=footprint_corner]
            PROJECTZ_SITE("footprint_corner", "Both");
            visible |= eye->projectForEffectAdmission(corner, screen);
        }
        if (!visible) continue;
        float haze = 0.0f;
        if (eye->usePerspective) {
            glm::vec3 renderPos;
            mc2ToRender(renderPos, print.corners[0]);
            const float distance = glm::length(renderPos - glm::vec3(eye->getCameraOrigin()));
            if (distance > Camera::MaxClipDistance) continue;
            if (distance > Camera::MinHazeDistance)
                haze = Camera::fogHaze(distance);
        }
        const auto lit = eye->currentLighting();
        unsigned char r = lit.amb[0], g = lit.amb[1], b = lit.amb[2];
        if (Terrain::terrainTextures2) {
            if (TerrainQuad::rainLightLevel < 1.0f) {
                r = static_cast<float>(r) * TerrainQuad::rainLightLevel;
                g = static_cast<float>(g) * TerrainQuad::rainLightLevel;
                b = static_cast<float>(b) * TerrainQuad::rainLightLevel;
            }
        }
        float fog = 1.0f;
        if (useFog) {
            DWORD fogValue = static_cast<DWORD>(mc2FogHeightAlpha(print.corners[0].z, lit.fogStart, lit.fogFull));
            fog = static_cast<float>(fogValue) / 255.0f;
            if (haze != 0.0f) {
                const DWORD distanceFog = float2long((1.0f - haze) * 255.0f);
                fogValue = std::min(fogValue, distanceFog);
                fog = static_cast<float>(fogValue) / 255.0f;
            }
        }
        const DWORD color = 0xff000000u | (DWORD(r) << 16) | (DWORD(g) << 8) | b;
        const float u = static_cast<float>(print.shape % 8) * 0.125f;
        const float v = static_cast<float>(print.shape / 8) * 0.125f;
        WorldOverlayVert quad[4];
        for (int c = 0; c < 4; ++c) {
            const auto& p = print.corners[c];
            quad[c] = {p.x, p.y, p.z, u + ((c == 1 || c == 2) ? 0.125f : 0),
                       v + ((c >= 2) ? 0.125f : 0), fog, color};
        }
        WorldOverlayVert a[3] = {quad[0], quad[1], quad[2]};
        WorldOverlayVert btri[3] = {quad[0], quad[2], quad[3]};
        gos_PushDecal(a, texture);
        gos_PushDecal(btri, texture);
    }
}
}
