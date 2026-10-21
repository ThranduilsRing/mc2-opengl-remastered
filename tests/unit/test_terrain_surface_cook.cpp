#include "doctest.h"
#include "terrain_surface_cook.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
