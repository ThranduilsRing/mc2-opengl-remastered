#include "doctest.h"
#include <filesystem>
#include <cstdio>
#include "terrain_surface_cook.h"

#include <algorithm>
#include <cmath>
#include <limits>

TEST_CASE("terrain projected error: plane, interior ridge and partial edge") {
    std::vector<float> heights(21 * 21);
    for (int y = 0; y < 21; ++y) for (int x = 0; x < 21; ++x) heights[x + y * 21] = float(x + 2 * y);
    auto errors = mc2terrain::terrainPatchHeightErrors(heights, 21, 0, 0, 20, 20);
    for (float e : errors) CHECK(e < 0.00001f);
    heights[1 + 21] += 17;
    errors = mc2terrain::terrainPatchHeightErrors(heights, 21, 0, 0, 20, 20);
    CHECK(errors[0] == 0);
    for (size_t i = 1; i < errors.size(); ++i) CHECK(errors[i] >= 17);
    errors = mc2terrain::terrainPatchHeightErrors(heights, 21, 0, 0, 19, 20);
    for (size_t i = 1; i < errors.size(); ++i) CHECK(std::isinf(errors[i]));
    errors = mc2terrain::terrainPatchHeightErrors({}, 21, 0, 0, 20, 20);
    CHECK(std::isinf(errors[1]));
    // A folded TL-BR surface is exactly representable by two triangles, while
    // bilinear reconstruction would incorrectly introduce an interior error.
    const std::vector<float> folded = {0, 0, 0, 0, 2, 2, 0, 2, 4};
    errors = mc2terrain::terrainPatchHeightErrors(folded, 3, 0, 0, 2, 2);
    CHECK(errors[1] == 0);
}

TEST_CASE("surface materials: muted sand follows semantic dirt, not hue") {
    mc2terrain::SurfaceAsset surface;
    surface.width = surface.height = 2; surface.sourceHash = 1;
    surface.macroColor.assign(4, {0.16f, 0.15f, 0.13f, 0.5f});
    const std::vector<float> dirt(4, 2);
    const auto weights = mc2terrain::cookSurfaceMaterials(surface, dirt, 2, {}, 0, 2, 2, true);
    REQUIRE(weights.size() == 16);
    for (size_t i = 0; i < weights.size(); i += 4) {
        CHECK(weights[i] == 0); CHECK(weights[i+1] == 255);
        CHECK(weights[i+2] == 0); CHECK(weights[i+3] == 0);
    }
    const uint8_t authored[] = {0, 0, 0, 255};
    const auto concrete = mc2terrain::cookSurfaceMaterials(surface, dirt, 2, authored, 1, 2, 2);
    REQUIRE(concrete.size() == 16);
    CHECK(concrete[1] == 0); CHECK(concrete[3] == 255);
    CHECK(mc2terrain::cookSurfaceMaterials(surface, dirt, 3, {}, 0, 2, 2).empty());
    // A material edit must affect a new cook even when the macro-color cache is unchanged.
    const std::vector<float> grass(4, 1);
    const auto edited = mc2terrain::cookSurfaceMaterials(surface, grass, 2, {}, 0, 2, 2);
    CHECK(edited[0] == 255); CHECK(edited[1] == 0);
}

TEST_CASE("surface materials: snow and unknown mod ground retain import fallback") {
    mc2terrain::SurfaceAsset surface;
    surface.width = surface.height = 1; surface.sourceHash = 1;
    surface.macroColor = {{0.8f, 0.8f, 0.8f, 0.5f}};
    const auto snow = mc2terrain::cookSurfaceMaterials(surface, {}, 0, {}, 0, 1, 1);
    REQUIRE(snow.size() == 4);
    CHECK(snow[2] == 255);
    surface.macroColor = {{0.1f, 0.35f, 0.1f, 0.5f}};
    const auto grass = mc2terrain::cookSurfaceMaterials(surface, {}, 0, {}, 0, 1, 1);
    CHECK(grass[0] > 200);
    CHECK(unsigned(grass[0]) + grass[1] + grass[2] + grass[3] <= 255);
}

namespace {
using mc2terrain::SurfaceCookInput;
using Color = std::array<double, 3>;
// Independent scalar oracle for terrain_tier1.frag's nine samples / sfFollowCoast and
// water_shore_edge.hglsl's Catmull-Rom. Double basis weights avoid sharing the cook's interpolation code.
Color refColor(const SurfaceCookInput& in, double u, double v) {
    const double px = u * in.colorWidth - 0.5, py = v * in.colorHeight - 0.5;
    const int ix = int(std::floor(px)), iy = int(std::floor(py));
    Color c{};
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        const double w = (x ? px - ix : 1 - (px - ix)) * (y ? py - iy : 1 - (py - iy));
        const auto& p = in.color[size_t(std::clamp(iy + y, 0, int(in.colorHeight) - 1)) * in.colorWidth +
                                 size_t(std::clamp(ix + x, 0, int(in.colorWidth) - 1))];
        for (size_t k = 0; k < 3; ++k) c[k] += w * p[k];
    }
    return c;
}
double refHeight(const SurfaceCookInput& in, double u, double v) {
    const double px = u * (in.shoreWidth - 1), py = v * (in.shoreHeight - 1);
    const int ix = int(std::floor(px)), iy = int(std::floor(py));
    auto weights = [](double t) {
        return std::array<double, 4>{-0.5*t+t*t-0.5*t*t*t, 1-2.5*t*t+1.5*t*t*t,
                                      0.5*t+2*t*t-1.5*t*t*t, -0.5*t*t+0.5*t*t*t};
    };
    const auto xs = weights(px - ix), ys = weights(py - iy);
    double h = 0;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
        h += xs[x] * ys[y] * in.shore[size_t(std::clamp(iy+y-1, 0, int(in.shoreHeight)-1)) * in.shoreWidth +
                                     size_t(std::clamp(ix+x-1, 0, int(in.shoreWidth)-1))];
    return h;
}
double paint(const Color& c) {
    const double t = std::clamp((std::min(c[1], c[2]) - c[0]) / 0.08, 0.0, 1.0);
    return t*t*(3-2*t);
}
Color reference(const SurfaceCookInput& in, double u, double v) {
    constexpr double r = 0.002, d = r * 0.707;
    const double offsets[9][2] = {{0,0},{r,0},{-r,0},{0,r},{0,-r},{d,d},{-d,d},{d,-d},{-d,-d}};
    Color base{};
    for (auto& offset : offsets) {
        const auto c = refColor(in, u+offset[0], v+offset[1]);
        for (size_t k=0; k<3; ++k) base[k] += c[k] / 9;
    }
    if (!in.followCoast) return base;
    const double wx = in.topLeftX+u*in.worldSpan, wy = in.topLeftY-v*in.worldSpan;
    const double x=(wx-in.shoreBounds[0])/in.shoreBounds[2], y=(in.shoreBounds[1]-wy)/in.shoreBounds[3];
    if (x<0 || x>1 || y<0 || y>1) return base;
    const double thick=in.waterElevation-refHeight(in,x,y);
    if (std::abs(thick)>24) return base;
    const bool water=thick>0;
    const double want=water ? 1-paint(base) : paint(base);
    if (want<0.02) return base;
    const double ex=1.0/(in.shoreWidth-1), ey=1.0/(in.shoreHeight-1);
    double gx=refHeight(in,x+ex,y)-refHeight(in,x-ex,y), gy=refHeight(in,x,y-ey)-refHeight(in,x,y+ey);
    const double len2=gx*gx+gy*gy;
    if (len2<1e-10) return base;
    const double scale=(water ? -1 : 1)/std::sqrt(len2);
    for (int k=1; k<=4; ++k) {
        const auto c=refColor(in,(wx+gx*scale*40*k-in.topLeftX)/in.worldSpan,
                                 (in.topLeftY-wy-gy*scale*40*k)/in.worldSpan);
        if (water ? paint(c)>0.5 : paint(c)<0.5) {
            for (size_t n=0;n<3;++n) base[n]=base[n]*(1-want)+c[n]*want;
            break;
        }
    }
    return base;
}
}

TEST_CASE("surface cook: reference blur and coast across borders and mismatched paint") {
    std::vector<mc2terrain::SurfaceColor> colors(16*12);
    std::vector<float> heights(9*7);
    SurfaceCookInput in;
    in.color=colors; in.colorWidth=16; in.colorHeight=12;
    in.topLeftX=-400; in.topLeftY=300; in.worldSpan=800;
    in.shore=heights; in.shoreWidth=9; in.shoreHeight=7;
    in.shoreBounds={-350,280,700,650}; in.waterElevation=2.75f;
    in.outputWidth=31; in.outputHeight=23;
    for (int axis=0;axis<2;++axis) for (int flipped=0;flipped<2;++flipped) {
        for (int y=0;y<12;++y) for (int x=0;x<16;++x) {
            const bool waterPaint=((axis ? y<5 : x<7) != (flipped!=0));
            colors[size_t(y)*16+x]=waterPaint ? mc2terrain::SurfaceColor{0.12f,0.38f,0.44f,1}
                                               : mc2terrain::SurfaceColor{0.45f,0.34f+float(y)*0.003f,0.2f,1};
        }
        for (int y=0;y<7;++y) for (int x=0;x<9;++x)
            heights[size_t(y)*9+x]=float(axis ? y-3 : x-4)*9.2f+1.1f;
        for (bool follow : {false,true}) {
            in.followCoast=follow;
            const auto cooked = mc2terrain::cookSurface(in);
            REQUIRE(cooked.has_value());
            const auto& asset = *cooked;
            for (uint32_t y=0;y<asset.height;++y) for (uint32_t x=0;x<asset.width;++x) {
                const float u=(float(x)+0.5f)/float(asset.width),v=(float(y)+0.5f)/float(asset.height);
                const auto ref=reference(in,u,v);
                for (size_t c=0;c<3;++c) CHECK(asset.macroColor[size_t(y)*asset.width+x][c] == doctest::Approx(ref[c]).epsilon(0.0001));
            }
            for (float u : {-0.02f,0.0f,0.0001f,0.5f,0.9999f,1.0f,1.02f})
                for (float v : {-0.02f,0.0f,0.31f,0.9999f,1.0f,1.02f}) {
                    const auto actual=mc2terrain::evaluateSurfaceMacro(in,u,v);
                    const auto ref=reference(in,u,v);
                    for (size_t c=0;c<3;++c) CHECK(actual[c] == doctest::Approx(ref[c]).epsilon(0.0001));
                }
        }
    }
    in.followCoast=true;
    std::fill(heights.begin(),heights.end(),in.waterElevation); // zero gradient leaves paint unchanged
    const auto zero=mc2terrain::evaluateSurfaceMacro(in,0.4f,0.4f);
    in.followCoast=false;
    CHECK(zero==mc2terrain::evaluateSurfaceMacro(in,0.4f,0.4f));
}

TEST_CASE("surface cook: filtering space provenance and malformed input") {
    const std::vector<uint8_t> rgba={128,64,32,0,255,255,255,255};
    const auto unorm=mc2terrain::surfaceColorsFromRgba(rgba,false),linear=mc2terrain::surfaceColorsFromRgba(rgba,true);
    CHECK(unorm[0][0]==doctest::Approx(128.0/255));
    CHECK(linear[0][0]==doctest::Approx(0.2158605));
    CHECK(linear[1][0]==1);
    SurfaceCookInput in;
    in.color=unorm; in.colorWidth=2; in.colorHeight=1; in.outputWidth=4; in.outputHeight=2; in.worldSpan=128;
    const auto source=mc2terrain::surfaceCookSourceHash(in);
    REQUIRE(source!=0);
    in.color=linear; CHECK(mc2terrain::surfaceCookSourceHash(in)!=source);
    in.color=unorm; in.outputWidth=5; CHECK(mc2terrain::surfaceCookSourceHash(in)!=source);
    REQUIRE(mc2terrain::cookSurface(in).has_value());
    in.worldSpan=std::numeric_limits<float>::infinity();
    CHECK_FALSE(mc2terrain::cookSurface(in).has_value());
    in.worldSpan=128; in.followCoast=true;
    CHECK_FALSE(mc2terrain::cookSurface(in).has_value());
    in.followCoast=false; in.outputWidth=0xffffffffu;
    CHECK_FALSE(mc2terrain::cookSurface(in).has_value());
}

TEST_CASE("surface cook: oversized source atlas stays bounded and matches decoded reference") {
    const std::vector<uint8_t> rgba = {128,64,32,255, 22,144,201,255, 33,211,100,255, 234,21,123,255};
    for (bool srgb : {false,true}) {
        const auto colors = mc2terrain::surfaceColorsFromRgba(rgba,srgb);
        SurfaceCookInput raw;
        raw.rgba=rgba; raw.srgb=srgb; raw.colorWidth=2; raw.colorHeight=2;
        raw.outputWidth=11; raw.outputHeight=7; raw.worldSpan=256;
        auto linear=raw; linear.rgba={}; linear.color=colors;
        const auto a=mc2terrain::cookSurface(raw),b=mc2terrain::cookSurface(linear);
        REQUIRE(a); REQUIRE(b);
        CHECK(a->macroColor==b->macroColor);
        CHECK(a->sourceHash!=b->sourceHash);
    }
    std::vector<uint8_t> wide(8192*4,128);
    SurfaceCookInput raw;
    raw.rgba=wide; raw.colorWidth=8192; raw.colorHeight=1;
    raw.outputWidth=64; raw.outputHeight=1; raw.worldSpan=2048;
    const auto asset=mc2terrain::cookSurface(raw);
    REQUIRE(asset);
    CHECK(asset->macroColor.size()==64);
    CHECK(asset->macroColor[0][0]==doctest::Approx(128.0/255));
    raw.colorWidth=SurfaceCookInput::kMaxSourceSide+1;
    CHECK_FALSE(mc2terrain::cookSurface(raw));
}

TEST_CASE("surface cook: large raster publishes complete rows with the same reference filter") {
    const std::vector<mc2terrain::SurfaceColor> colors = {{0.1f,0.3f,0.5f,1},{0.8f,0.2f,0.3f,1},
                                                       {0.5f,0.7f,0.2f,1},{0.6f,0.4f,0.9f,1}};
    SurfaceCookInput in;
    in.color=colors; in.colorWidth=in.colorHeight=2; in.worldSpan=1024;
    in.outputWidth=in.outputHeight=1024;
    const auto cooked=mc2terrain::cookSurface(in);
    REQUIRE(cooked);
    CHECK(std::all_of(cooked->macroColor.begin(),cooked->macroColor.end(),[](const auto& c){return c[3]==1;}));
    for (uint32_t y : {0u,511u,512u,1023u}) for (uint32_t x : {0u,1u,300u,1023u}) {
        const auto expected=reference(in,(float(x)+0.5f)/1024.0f,(float(y)+0.5f)/1024.0f);
        for (size_t c=0;c<3;++c) CHECK(cooked->macroColor[size_t(y)*1024+x][c]==doctest::Approx(expected[c]).epsilon(0.0001));
    }
}

TEST_CASE("surface cook: filtered gradients match independent bilinear height differences including the clamp halo") {
    constexpr uint32_t side = 13;
    std::vector<float> heights(side * side);
    for (uint32_t y = 0; y < side; ++y) for (uint32_t x = 0; x < side; ++x)
        heights[y * side + x] = float((x * 143 + y * 71 + x * y * 13) % 911) - 300.0f;
    const auto gradient = mc2terrain::cookSurfaceGradients(heights, side);
    REQUIRE(gradient);
    REQUIRE(gradient->side == side + 2);
    auto height = [&](double x, double y) {
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        double value = 0;
        for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b)
            value += heights[size_t(std::clamp(iy + b, 0, int(side) - 1)) * side + size_t(std::clamp(ix + a, 0, int(side) - 1))]
                * (a ? x - ix : 1 - x + ix) * (b ? y - iy : 1 - y + iy);
        return value;
    };
    double worst = 0;
    for (int y = -20; y <= 140; ++y) for (int x = -20; x <= 140; ++x) {
        const double px = x * 0.1, py = y * 0.1;
        const int ix = int(std::floor(px)), iy = int(std::floor(py));
        double gx = 0, gy = 0;
        for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b) {
            const auto& g = gradient->texels[size_t(std::clamp(iy + b + 1, 0, int(side) + 1)) * gradient->side +
                                              size_t(std::clamp(ix + a + 1, 0, int(side) + 1))];
            const double w = (a ? px - ix : 1 - px + ix) * (b ? py - iy : 1 - py + iy);
            gx += w * g[0]; gy += w * g[1];
        }
        worst = std::max({worst, std::abs(gx + (height(px + 1, py) - height(px - 1, py)) / 256),
                                  std::abs(gy - (height(px, py + 1) - height(px, py - 1)) / 256)});
    }
    CHECK(worst < 0.000001);
    heights[4] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(mc2terrain::cookSurfaceGradients(heights, side));
    CHECK_FALSE(mc2terrain::cookSurfaceGradients({}, side));
    CHECK_FALSE(mc2terrain::cookSurfaceGradients({}, 0xffffffffu));
}

TEST_CASE("surface cook: breakup alpha is independently invalidated and editable color stays separate") {
    const std::vector<mc2terrain::SurfaceColor> colors = {{0.3f, 0.4f, 0.5f, 1}};
    SurfaceCookInput in;
    in.color = colors; in.colorWidth = in.colorHeight = 1;
    in.worldSpan = 256; in.topLeftX = -128; in.topLeftY = 128;
    in.outputWidth = in.outputHeight = 8;
    const auto color = mc2terrain::cookSurface(in);
    REQUIRE(color);
    in.staticBreakup = true;
    const auto relief = mc2terrain::resolveSurfaceAsset(in, mc2terrain::encodeSurfaceAsset(*color));
    REQUIRE(relief);
    CHECK(relief->sourceHash != color->sourceHash);
    for (size_t i = 0; i < relief->macroColor.size(); ++i) {
        for (size_t k = 0; k < 3; ++k) CHECK(relief->macroColor[i][k] == color->macroColor[i][k]);
        CHECK(relief->macroColor[i][3] >= 0);
        CHECK(relief->macroColor[i][3] <= 1);
    }
    const auto source = relief->sourceHash;
    in.topLeftX += 1;
    CHECK(mc2terrain::surfaceCookSourceHash(in) != source);
    CHECK(mc2terrain::evaluateSurfaceBreakup(0, 0) == doctest::Approx(0.5));
}

TEST_CASE("packed surface cache: exact upload and material samples, key changes and damaged files miss") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "mc2_packed_surface_test";
    fs::remove_all(dir);
    std::vector<uint8_t> sourcePixels{20, 70, 190, 255};
    SurfaceCookInput in;
    in.rgba = sourcePixels; in.colorWidth = in.colorHeight = 1; in.srgb = true;
    in.worldSpan = 256; in.outputWidth = 2051; in.outputHeight = 3; in.staticBreakup = true;
    const auto surface = mc2terrain::cookSurface(in);
    REQUIRE(surface);
    const auto key = surface->sourceHash;
    sourcePixels[0] ^= 1;
    CHECK(mc2terrain::surfaceCookSourceHash(in) != key);
    sourcePixels[0] ^= 1;
    in.srgb = false;
    CHECK(mc2terrain::surfaceCookSourceHash(in) != key);
    in.srgb = true;
    std::vector<uint8_t> upload(surface->macroColor.size() * 4);
    for (size_t i = 0; i < upload.size(); ++i) upload[i] = static_cast<uint8_t>(i);
    REQUIRE(mc2terrain::storeCachedPackedSurface(dir.string(), *surface, true, upload));
    const auto hit = mc2terrain::loadCachedPackedSurface(dir.string(), key, true);
    REQUIRE(hit);
    CHECK(hit->rgba == upload);
    CHECK(hit->width == surface->width); CHECK(hit->height == surface->height);
    CHECK(hit->materials.macroColor.size() == 1024 * 3);
    for (bool sand : {false, true})
        CHECK(mc2terrain::cookSurfaceMaterials(*surface, {}, 0, {}, 0, 1024, 3, sand) ==
              mc2terrain::cookSurfaceMaterials(hit->materials, {}, 0, {}, 0, 1024, 3, sand));
    CHECK_FALSE(mc2terrain::loadCachedPackedSurface(dir.string(), key + 1, true));
    CHECK_FALSE(mc2terrain::loadCachedPackedSurface(dir.string(), key, false));
    const auto path = fs::directory_iterator(dir)->path();
    const auto size = fs::file_size(path);
    for (long offset : {0L, 4L, 8L, 20L, 24L, 32L, 40L, 60L}) {
        REQUIRE(mc2terrain::storeCachedPackedSurface(dir.string(), *surface, true, upload));
        auto* file = std::fopen(path.string().c_str(), "r+b");
        REQUIRE(file);
        std::fseek(file, offset, SEEK_SET); const int byte = std::fgetc(file);
        std::fseek(file, offset, SEEK_SET); std::fputc(byte ^ 1, file); std::fclose(file);
        CHECK_FALSE(mc2terrain::loadCachedPackedSurface(dir.string(), key, true));
    }
    for (uintmax_t length : {size - 1, uintmax_t(48), uintmax_t(0)}) {
        REQUIRE(mc2terrain::storeCachedPackedSurface(dir.string(), *surface, true, upload));
        fs::resize_file(path, length);
        CHECK_FALSE(mc2terrain::loadCachedPackedSurface(dir.string(), key, true));
    }
    REQUIRE(mc2terrain::storeCachedPackedSurface(dir.string(), *surface, true, upload));
    REQUIRE(mc2terrain::loadCachedPackedSurface(dir.string(), key, true));
    fs::remove_all(dir);
}

TEST_CASE("surface cook cache: roundtrip, stale key, corrupt and truncated files, bit-identical output") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "mc2_terrain_cache_test";
    fs::remove_all(dir);
    const std::vector<mc2terrain::SurfaceColor> colors = {{0.3f, 0.4f, 0.5f, 1}, {0.6f, 0.2f, 0.1f, 1},
                                                          {0.1f, 0.9f, 0.4f, 1}, {0.5f, 0.5f, 0.5f, 1}};
    SurfaceCookInput in;
    in.color = colors; in.colorWidth = in.colorHeight = 2;
    in.worldSpan = 256; in.topLeftX = -128; in.topLeftY = 128;
    in.outputWidth = in.outputHeight = 16; in.staticBreakup = true;
    const std::string cache = dir.string();
    const char* origin = "";
    const auto cold = mc2terrain::resolveSurfaceAsset(in, {}, cache, &origin);
    REQUIRE(cold);
    CHECK(std::string(origin) == "cooked+stored");
    const auto warm = mc2terrain::resolveSurfaceAsset(in, {}, cache, &origin);
    REQUIRE(warm);
    CHECK(std::string(origin) == "cache");
    CHECK(mc2terrain::encodeSurfaceAsset(*warm) == mc2terrain::encodeSurfaceAsset(*cold));
    CHECK(mc2terrain::encodeSurfaceAsset(*cold) == mc2terrain::encodeSurfaceAsset(*mc2terrain::cookSurface(in)));
    // Stale key: changed source bytes hash elsewhere, never hit the old entry.
    auto changed = in; changed.topLeftX += 1;
    CHECK_FALSE(mc2terrain::loadCachedSurfaceAsset(cache, mc2terrain::surfaceCookSourceHash(changed)));
    mc2terrain::resolveSurfaceAsset(changed, {}, cache, &origin);
    CHECK(std::string(origin) == "cooked+stored");
    // A valid file renamed to another key is rejected (embedded source hash mismatch).
    const auto key = cold->sourceHash;
    char name[40], other[40];
    std::snprintf(name, sizeof name, "macro-%016llx.tsf", static_cast<unsigned long long>(key));
    std::snprintf(other, sizeof other, "macro-%016llx.tsf", static_cast<unsigned long long>(key + 7));
    fs::copy_file(dir / name, dir / other);
    CHECK_FALSE(mc2terrain::loadCachedSurfaceAsset(cache, key + 7));
    // Corrupt and truncated entries fall back to a fresh cook and heal the cache.
    {
        std::FILE* f = std::fopen((dir / name).string().c_str(), "r+b");
        REQUIRE(f);
        std::fseek(f, 100, SEEK_SET); std::fputc(0x5a, f); std::fclose(f);
    }
    CHECK_FALSE(mc2terrain::loadCachedSurfaceAsset(cache, key));
    const auto healed = mc2terrain::resolveSurfaceAsset(in, {}, cache, &origin);
    REQUIRE(healed);
    CHECK(std::string(origin) == "cooked+stored");
    CHECK(mc2terrain::encodeSurfaceAsset(*healed) == mc2terrain::encodeSurfaceAsset(*cold));
    fs::resize_file(dir / name, 60);
    CHECK_FALSE(mc2terrain::loadCachedSurfaceAsset(cache, key));
    fs::resize_file(dir / name, 0);
    CHECK_FALSE(mc2terrain::loadCachedSurfaceAsset(cache, key));
    // No temp files are left behind by successful stores.
    for (const auto& e : fs::directory_iterator(dir)) CHECK(e.path().extension() == ".tsf");
    CHECK_FALSE(mc2terrain::loadCachedSurfaceAsset("", key));
    fs::remove_all(dir);
}
