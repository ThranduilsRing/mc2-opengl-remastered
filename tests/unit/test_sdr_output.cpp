// H0: frozen CPU reference for postprocess_visual.frag (encoded display domain).
#include "doctest.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace {
using V = std::array<double, 3>;
double clip(double x) { return std::clamp(x, 0.0, 1.0); }
double smooth(double a, double b, double x) {
    const double t = clip((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}
double fract(double x) { return x - std::floor(x); }
V matrix(const V& v, const std::array<V, 3>& columns) {
    V out{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i] += columns[j][i] * v[j];
    return out;
}
V grade(V c, double x, double y) {
    const V gain{1.04, 0.98, 1.02}, cool{0.93, 0.98, 1.06};
    const V warm{1.06, 1.01, 0.94}, top{1.05, 1.01, 0.95};
    for (int i = 0; i < 3; ++i) c[i] *= 1.1 * gain[i];
    const double t = smooth(0.10, 0.75, c[0]*0.299 + c[1]*0.587 + c[2]*0.114);
    const double vignette = 1.0 - smooth(0.25, 0.85, std::hypot(x-0.5, (y-0.5)*0.6));
    const double sun = std::pow(smooth(0.0, 1.0, 1.0-y), 1.5) * 0.2;
    for (int i = 0; i < 3; ++i)
        c[i] *= (cool[i] + (warm[i]-cool[i])*t) * (0.8+0.2*vignette) * (1.0+(top[i]-1.0)*sun);
    return c;
}
double hable(double x) {
    return ((x*(0.15*x+0.05)+0.004)/(x*(0.15*x+0.5)+0.06))-0.02/0.3;
}
V tonemap(V c, int mode) {
    if (mode == 1) {
        for (auto& v : c) v = std::max(v, 0.0);
        c = matrix(c, {{{0.842479062253094, 0.0423282422610123, 0.0423756549057051},
                        {0.0784335999999992, 0.878468636469772, 0.0784336},
                        {0.0792237451477643, 0.0791661274605434, 0.879142973793104}}});
        for (auto& v : c) {
            v = (std::clamp(std::log2(std::max(v, 1e-10)), -12.47393, 4.026069)+12.47393)/16.499999;
            const double v2=v*v, v4=v2*v2;
            v = 15.5*v4*v2-40.14*v4*v+31.96*v4-6.868*v2*v+0.4298*v2+0.1191*v-0.00232;
        }
        c = matrix(c, {{{1.19687900512017, -0.0528968517574562, -0.0529716355144438},
                        {-0.0980208811401368, 1.15190312990417, -0.0980434501171241},
                        {-0.0990297440797205, -0.0989611768448433, 1.15107367264116}}});
        for (auto& v : c) v = clip(v);
    } else if (mode == 2) {
        for (auto& v : c) v = clip(hable(v*2.0)/hable(11.2));
    } else if (mode == 3) {
        for (auto& v : c) v /= 1.0+v;
    }
    return c;
}
double noise(double x, double y) {
    const auto hash = [](double a, double b) { return fract(52.9829189*fract(a*0.06711056+b*0.00583715)); };
    return hash(x,y)+hash(x+37.0,y+17.0)-1.0;
}
V output(V c, int mode, int x, int y, bool reconstruct = false) {
    if (!reconstruct) {
        c = tonemap(c, mode);
        for (auto& v : c) v += noise(x,y)/255.0;
    }
    for (auto& v : c) v = clip(v);
    return c;
}
}

TEST_CASE("SDR output/reference grid pins grade curves dither and clamp") {
    // Aggregate each channel separately: catches matrix transposition and grade order.
    std::array<V, 4> sums{};
    for (int mode = 0; mode < 4; ++mode)
        for (double value : {0.0, 0.01, 0.18, 0.5, 1.0, 2.0, 16.0})
            for (int y : {0, 17, 2159})
                for (int x : {0, 37, 3839}) {
                    const V c = grade({value, value*0.7, value*1.3}, x/3839.0, y/2159.0);
                    const V out = output(c, mode, x, y);
                    for (int i = 0; i < 3; ++i) sums[mode][i] += out[i];
                    CHECK(output(c, mode, x, y, true) == output(c, 0, 0, 0, true));
                }
    const std::array<V, 4> golden{{
        {33.283352230741, 28.233293388027, 35.029148041562},
        {35.566676518268, 32.922411813658, 36.986422537193},
        {24.288422039995, 20.539903622127, 25.803253863758},
        {23.829999911048, 20.335175804147, 25.243085044322}
    }};
    for (int mode = 0; mode < 4; ++mode)
        for (int i = 0; i < 3; ++i) {
            CAPTURE(mode);
            CAPTURE(i);
            CHECK(sums[mode][i] == doctest::Approx(golden[mode][i]).epsilon(1e-9));
        }
    CHECK(output({-1.0, 1.0, 4.0}, 0, 0, 0, true) == V{0.0, 1.0, 1.0});
}
