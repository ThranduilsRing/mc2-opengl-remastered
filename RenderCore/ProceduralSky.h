#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <vector>

namespace RenderCore {
using SkyRgb = std::array<float, 3>;
struct SkyAtmosphereRow {
    float density, turbidity, daylight;
    SkyRgb night;
};
// Mission SkyNumber, including the dark dramatic skies 3 and 14. Colour is
// linear radiance; the mission sun supplies direction and direct-light colour.
inline constexpr SkyAtmosphereRow kSkyAtmospheres[21] = {
    {1.0f,1.5f,1.0f,{.002f,.003f,.008f}},
    {1.2f,2.8f,.65f,{.005f,.002f,.006f}},
    {1.4f,2.0f,.025f,{.003f,.002f,.008f}},
    {1.5f,3.5f,.8f,{.002f,.003f,.006f}},
    {1.0f,1.0f,0.0f,{.002f,.003f,.008f}},
    {.9f,1.1f,1.0f,{.002f,.003f,.008f}},
    {1.4f,3.2f,.8f,{.003f,.003f,.007f}},
    {.8f,.8f,0.0f,{.001f,.002f,.006f}},
    {1.2f,3.0f,.5f,{.006f,.002f,.004f}},
    {1.2f,1.8f,1.0f,{.002f,.003f,.008f}},
    {1.7f,4.0f,.7f,{.002f,.003f,.006f}},
    {1.0f,2.3f,.9f,{.003f,.003f,.007f}},
    {1.1f,2.6f,.8f,{.004f,.003f,.007f}},
    {1.5f,3.2f,.015f,{.002f,.003f,.007f}},
    {.8f,1.0f,1.0f,{.002f,.003f,.008f}},
    {.85f,1.2f,1.0f,{.002f,.003f,.008f}},
    {1.4f,3.0f,.15f,{.003f,.003f,.008f}},
    {1.3f,3.3f,.2f,{.006f,.002f,.004f}},
    {.9f,1.2f,1.0f,{.002f,.003f,.008f}},
    {.7f,.7f,0.0f,{.001f,.0015f,.004f}},
    {1.0f,1.2f,0.0f,{.003f,.002f,.008f}}
};
inline const SkyAtmosphereRow& skyAtmosphere(int number) {
    return kSkyAtmospheres[number >= 1 && number <= 21 ? number - 1 : 0];
}
inline SkyRgb skyNormalize(SkyRgb d) {
    const float length = std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    if (!(length > 0.0f)) return {0,1,0};
    for (float& c : d) c /= length;
    return d;
}
// Vulkan cube face order +X,-X,+Y,-Y,+Z,-Z, Y-up sky coordinates.
inline SkyRgb skyCubeDirection(int face, float u, float v) {
    switch (face) {
    case 0: return skyNormalize({1,-v,-u});
    case 1: return skyNormalize({-1,-v,u});
    case 2: return skyNormalize({u,1,v});
    case 3: return skyNormalize({u,-1,-v});
    case 4: return skyNormalize({u,-v,1});
    default:return skyNormalize({-u,-v,-1});
    }
}
// Matches the legacy sky yaw convention: native MC2 Z-up -> sky Y-up.
inline SkyRgb skyMissionSun(float x, float y, float z) {
    return skyNormalize({-x,z,y});
}
struct SkyCubemapBake {
    std::vector<std::vector<float>> levels; // mip-major, then six RGBA faces
    std::array<SkyRgb,9> sh{};              // raw Y-pole SH-L2 integrals
};
template<class Sample> SkyCubemapBake bakeSkyCubemap(Sample sample) {
    SkyCubemapBake out;
    out.levels.emplace_back(6*128*128*4,1.0f);
    double weightSum = 0.0;
    for (int face=0;face<6;++face) for (int y=0;y<128;++y) for (int x=0;x<128;++x) {
        const float u = (x+.5f)/64.0f-1.0f, v = (y+.5f)/64.0f-1.0f;
        const auto d = skyCubeDirection(face,u,v);
        const auto rgb = sample(d);
        const float w = 1.0f/std::pow(1.0f+u*u+v*v,1.5f);
        const float basis[9] = {.282095f,.488603f*d[1],.488603f*d[2],.488603f*d[0],
            1.092548f*d[0]*d[1],1.092548f*d[1]*d[2],.315392f*(3*d[1]*d[1]-1),
            1.092548f*d[0]*d[2],.546274f*(d[0]*d[0]-d[2]*d[2])};
        const size_t pixel = static_cast<size_t>((face*128+y)*128+x)*4;
        for (int c=0;c<3;++c) {
            out.levels[0][pixel+c] = rgb[c];
            for (int b=0;b<9;++b) out.sh[b][c] += rgb[c]*basis[b]*w;
        }
        weightSum += w;
    }
    for (auto& band : out.sh) for (float& c : band) c *= static_cast<float>(12.566370614359/weightSum);
    for (int size=128;size>1;size/=2) {
        const int next = size/2;
        const auto& src = out.levels.back();
        std::vector<float> dst(static_cast<size_t>(6*next*next*4));
        for (int face=0;face<6;++face) for (int y=0;y<next;++y) for (int x=0;x<next;++x)
            for (int c=0;c<4;++c) {
                float sum = 0;
                for (int dy=0;dy<2;++dy) for (int dx=0;dx<2;++dx)
                    sum += src[static_cast<size_t>((face*size+2*y+dy)*size+2*x+dx)*4+c];
                dst[static_cast<size_t>((face*next+y)*next+x)*4+c] = sum*.25f;
            }
        out.levels.push_back(std::move(dst));
    }
    return out;
}
inline SkyRgb sampleProceduralSky(int number, SkyRgb ray, SkyRgb sun, SkyRgb light) {
    const auto& row = skyAtmosphere(number);
    const float elevation = std::max(0.0f, ray[1]);
    const float viewAir = 1.0f / (.1f + .9f * elevation);
    const float sunAir = 1.0f / (.08f + .92f * std::max(0.0f, sun[1]));
    const float mu = std::clamp(ray[0]*sun[0]+ray[1]*sun[1]+ray[2]*sun[2],-1.0f,1.0f);
    const float rayleigh = .0596831f * (1.0f + mu*mu);
    const float mie = .0336f / std::pow(std::max(.01f,1.5776f-1.52f*mu),1.5f);
    const float daylight = row.daylight * std::clamp((sun[1]+.12f)/.24f,0.0f,1.0f);
    // A high, stretched cirrus sheet, baked with the atmosphere (no frame cost).
    const float plane = 1.0f / std::max(ray[1], .08f);
    const float x = ray[0]*plane, z = ray[2]*plane;
    const float streak = std::sin(z*24.0f + 2.0f*std::sin(x*1.7f) + .7f*std::sin(z*9.0f+x*3.0f));
    const float veil = std::clamp((streak-.45f)*1.8f,0.0f,1.0f)
        * std::clamp((std::sin(x*.9f+z*.7f)-.1f)*1.4f,0.0f,1.0f)
        * std::clamp((ray[1]-.04f)*5.0f,0.0f,1.0f) * .16f;
    const float haze = std::exp(-std::abs(ray[1])*22.0f);
    const SkyRgb beta{.055f,.13f,.32f};
    SkyRgb colour{};
    for (int c=0;c<3;++c) {
        const float extinction = beta[c]*row.density;
        const float sunlight = std::exp(-extinction*sunAir);
        const float scatter = (1.0f-std::exp(-extinction*viewAir))*rayleigh;
        const float aerosol = (1.0f-std::exp(-.04f*row.turbidity*viewAir))*mie;
        const float disc = mu > .9998f && ray[1] > 0.0f ? 8.0f : 0.0f;
        colour[c] = row.night[c]*(.4f+.6f*elevation)
            + daylight*std::max(.05f,light[c])*sunlight*(18.0f*scatter+.6f*aerosol+disc);
        const float mist = daylight*(.55f+.12f*c)*std::max(.25f,light[c]);
        colour[c] = colour[c]*(1.0f-.65f*haze) + mist*.65f*haze;
        colour[c] = colour[c]*(1.0f-veil) + daylight*(.85f+.04f*c)*veil;
        if (ray[1] < 0.0f) colour[c] *= std::max(.04f,1.0f+ray[1]);
    }
    return colour;
}
} // namespace RenderCore
