#include "terrain_surface_cook.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <thread>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace mc2terrain {
namespace {
bool sizeOk(uint32_t w, uint32_t h) {
    return w > 0 && h > 0 && w <= SurfaceAsset::kMaxSide && h <= SurfaceAsset::kMaxSide;
}
float mix(float a, float b, float t) { return a * (1.0f - t) + b * t; }
SurfaceColor mixColor(const SurfaceColor& a, const SurfaceColor& b, float t) {
    return {mix(a[0], b[0], t), mix(a[1], b[1], t), mix(a[2], b[2], t), 1};
}
const std::array<float, 256>& srgbTable() {
    static const auto table = [] {
        std::array<float, 256> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            const float v = float(i) / 255.0f;
            values[i] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        }
        return values;
    }();
    return table;
}
SurfaceColor sourceColor(const SurfaceCookInput& in, size_t pixel) {
    if (in.rgba.empty()) return in.color[pixel];
    const auto* p = in.rgba.data() + pixel * 4;
    if (in.srgb) { const auto& table = srgbTable(); return {table[p[0]], table[p[1]], table[p[2]], 1}; }
    return {float(p[0]) / 255.0f, float(p[1]) / 255.0f, float(p[2]) / 255.0f, 1};
}
SurfaceColor colorAt(const SurfaceCookInput& in, float u, float v) {
    // Clamp texel addresses, not coordinates: matches Vulkan LINEAR/CLAMP_TO_EDGE at the borders.
    const float px = u * float(in.colorWidth) - 0.5f, py = v * float(in.colorHeight) - 0.5f;
    const int x = int(std::floor(px)), y = int(std::floor(py));
    auto at = [&](int xx, int yy) {
        return sourceColor(in, size_t(std::clamp(yy, 0, int(in.colorHeight) - 1)) * in.colorWidth +
                               size_t(std::clamp(xx, 0, int(in.colorWidth) - 1)));
    };
    return mixColor(mixColor(at(x, y), at(x + 1, y), px - float(x)),
                    mixColor(at(x, y + 1), at(x + 1, y + 1), px - float(x)), py - float(y));
}

constexpr float kBlur = 0.0020f, kDiag = kBlur * 0.707f;
constexpr float kOffsets[9][2] = {{0,0}, {kBlur,0}, {-kBlur,0}, {0,kBlur}, {0,-kBlur},
                                 {kDiag,kDiag}, {-kDiag,kDiag}, {kDiag,-kDiag}, {-kDiag,-kDiag}};
struct AxisSample { uint32_t lo, hi; float t; };
using AxisSamples = std::array<AxisSample, 9>;
AxisSamples axisSamples(float uv, uint32_t extent, size_t axis) {
    AxisSamples samples{};
    for (size_t k = 0; k < samples.size(); ++k) {
        const float p = (uv + kOffsets[k][axis]) * float(extent) - 0.5f;
        const int i = int(std::floor(p));
        samples[k] = {uint32_t(std::clamp(i, 0, int(extent) - 1)),
                      uint32_t(std::clamp(i + 1, 0, int(extent) - 1)), p - float(i)};
    }
    return samples;
}

// Raster cooks reuse axis addresses/weights across rows. SSE2 is the x64 baseline;
// each lane retains the scalar evaluator's operation order, without changing the filter.
SurfaceColor filteredColor(const SurfaceCookInput& in, const AxisSamples& xs, const AxisSamples& ys) {
#if defined(_M_X64) || defined(__SSE2__)
    const auto at = [&](size_t pixel) {
        if (in.rgba.empty()) return _mm_loadu_ps(in.color.data()[pixel].data());
        if (in.srgb) { const auto c = sourceColor(in, pixel); return _mm_loadu_ps(c.data()); }
        uint32_t rgba = 0;
        std::memcpy(&rgba, in.rgba.data() + pixel * 4, 4);
        auto bytes = _mm_cvtsi32_si128(static_cast<int>(rgba));
        bytes = _mm_unpacklo_epi8(bytes, _mm_setzero_si128());
        bytes = _mm_unpacklo_epi16(bytes, _mm_setzero_si128());
        return _mm_div_ps(_mm_cvtepi32_ps(bytes), _mm_set1_ps(255.0f));
    };
    const auto blend = [](__m128 a, __m128 b, float t) {
        return _mm_add_ps(_mm_mul_ps(a, _mm_set1_ps(1.0f - t)), _mm_mul_ps(b, _mm_set1_ps(t)));
    };
    auto sum = _mm_setzero_ps();
    for (size_t k = 0; k < xs.size(); ++k) {
        const auto& x = xs[k]; const auto& y = ys[k];
        const size_t a = size_t(y.lo) * in.colorWidth, b = size_t(y.hi) * in.colorWidth;
        const auto c = blend(blend(at(a + x.lo), at(a + x.hi), x.t),
                             blend(at(b + x.lo), at(b + x.hi), x.t), y.t);
        sum = _mm_add_ps(sum, c);
    }
    SurfaceColor result;
    _mm_storeu_ps(result.data(), _mm_div_ps(sum, _mm_set1_ps(9.0f)));
    result[3] = 1;
    return result;
#else
    SurfaceColor result{0,0,0,1};
    for (size_t k = 0; k < xs.size(); ++k) {
        const auto& x = xs[k]; const auto& y = ys[k];
        const size_t a = size_t(y.lo) * in.colorWidth, b = size_t(y.hi) * in.colorWidth;
        const auto c = mixColor(mixColor(sourceColor(in,a+x.lo),sourceColor(in,a+x.hi),x.t),
                                mixColor(sourceColor(in,b+x.lo),sourceColor(in,b+x.hi),x.t),y.t);
        for (size_t n=0;n<3;++n) result[n] += c[n];
    }
    for (size_t n=0;n<3;++n) result[n] /= 9.0f;
    return result;
#endif
}
float catmull(float t, float a, float b, float c, float d) {
    return 0.5f * ((2.0f * b) + (-a + c) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t * t +
                   (-a + 3.0f * b - 3.0f * c + d) * t * t * t);
}
float shoreAt(const SurfaceCookInput& in, float u, float v) {
    const float px = u * float(in.shoreWidth - 1), py = v * float(in.shoreHeight - 1);
    const int x = int(std::floor(px)), y = int(std::floor(py));
    float cols[4];
    for (int row = 0; row < 4; ++row) {
        float r[4];
        for (int col = 0; col < 4; ++col)
            r[col] = in.shore[size_t(std::clamp(y - 1 + row, 0, int(in.shoreHeight) - 1)) * in.shoreWidth +
                              size_t(std::clamp(x - 1 + col, 0, int(in.shoreWidth) - 1))];
        cols[row] = catmull(px - float(x), r[0], r[1], r[2], r[3]);
    }
    return catmull(py - float(y), cols[0], cols[1], cols[2], cols[3]);
}
float waterPaint(const SurfaceColor& c) {
    const float t = std::clamp((std::min(c[1], c[2]) - c[0]) / 0.08f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
void hashWord(uint64_t& hash, uint32_t word) {
    const uint8_t bytes[] = {uint8_t(word), uint8_t(word >> 8), uint8_t(word >> 16), uint8_t(word >> 24)};
    hash = surfaceHash(bytes, hash);
}
} // namespace

std::vector<SurfaceColor> surfaceColorsFromRgba(std::span<const uint8_t> rgba, bool srgb) {
    if (rgba.empty() || rgba.size() % 4 != 0 || rgba.size() / 4 > uint64_t(SurfaceAsset::kMaxSide) * SurfaceAsset::kMaxSide)
        return {};
    std::vector<SurfaceColor> colors(rgba.size() / 4);
    for (size_t i = 0; i < colors.size(); ++i) {
        for (size_t c = 0; c < 3; ++c) {
            const float v = float(rgba[i * 4 + c]) / 255.0f;
            colors[i][c] = !srgb ? v : v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        }
        colors[i][3] = 1;
    }
    return colors;
}

bool validSurfaceCookInput(const SurfaceCookInput& in) {
    const uint64_t pixels = uint64_t(in.colorWidth) * in.colorHeight;
    const bool bytes = !in.rgba.empty();
    if (in.colorWidth == 0 || in.colorHeight == 0 || in.colorWidth > SurfaceCookInput::kMaxSourceSide ||
        in.colorHeight > SurfaceCookInput::kMaxSourceSide || !sizeOk(in.outputWidth, in.outputHeight) ||
        (bytes ? !in.color.empty() || in.rgba.size() != pixels * 4 : in.color.size() != pixels) || !std::isfinite(in.topLeftX) ||
        !std::isfinite(in.topLeftY) || !std::isfinite(in.worldSpan) || in.worldSpan <= 0 ||
        !std::isfinite(in.waterElevation)) return false;
    for (const auto& color : in.color)
        for (float c : color) if (!std::isfinite(c) || c < 0 || c > 1) return false;
    if (in.followCoast) {
        if (!sizeOk(in.shoreWidth, in.shoreHeight) || in.shoreWidth < 2 || in.shoreHeight < 2 ||
            in.shore.size() != uint64_t(in.shoreWidth) * in.shoreHeight) return false;
        for (float b : in.shoreBounds) if (!std::isfinite(b)) return false;
        if (in.shoreBounds[2] <= 0 || in.shoreBounds[3] <= 0) return false;
        for (float h : in.shore) if (!std::isfinite(h)) return false;
    }
    return true;
}

uint64_t surfaceCookSourceHash(const SurfaceCookInput& in) {
    if (!validSurfaceCookInput(in)) return 0;
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t word : {SurfaceAsset::kEvaluator, in.colorWidth, in.colorHeight, in.outputWidth, in.outputHeight,
                          uint32_t(in.followCoast)}) hashWord(hash, word);
    for (float value : {in.topLeftX, in.topLeftY, in.worldSpan}) hashWord(hash, std::bit_cast<uint32_t>(value));
    if (!in.rgba.empty()) {
        hashWord(hash, 0x41424752u); // RGBA byte source, distinct from a caller's linear float source
        hashWord(hash, uint32_t(in.srgb));
        hash = surfaceHash(in.rgba, hash);
    } else {
        for (const auto& color : in.color)
            for (float value : color) hashWord(hash, std::bit_cast<uint32_t>(value));
    }
    if (in.followCoast) {
        hashWord(hash, in.shoreWidth); hashWord(hash, in.shoreHeight);
        hashWord(hash, std::bit_cast<uint32_t>(in.waterElevation));
        for (float value : in.shoreBounds) hashWord(hash, std::bit_cast<uint32_t>(value));
        for (float value : in.shore) hashWord(hash, std::bit_cast<uint32_t>(value));
    }
    return hash == 0 ? 1 : hash;
}

namespace {
SurfaceColor followCoast(const SurfaceCookInput& in, SurfaceColor base, float u, float v) {
    if (!in.followCoast) return base;
    const float wx = in.topLeftX + u * in.worldSpan, wy = in.topLeftY - v * in.worldSpan;
    const float hnX = (wx - in.shoreBounds[0]) / std::max(in.shoreBounds[2], 1e-5f);
    const float hnY = (in.shoreBounds[1] - wy) / std::max(in.shoreBounds[3], 1e-5f);
    if (hnX < 0 || hnX > 1 || hnY < 0 || hnY > 1) return base;
    const float thick = in.waterElevation - shoreAt(in, hnX, hnY);
    if (std::abs(thick) > 24.0f) return base;
    const bool water = thick > 0;
    const float paint = waterPaint(base), want = water ? 1.0f - paint : paint;
    if (want < 0.02f) return base;
    const float ex = 1.0f / float(in.shoreWidth - 1), ey = 1.0f / float(in.shoreHeight - 1);
    float gx = shoreAt(in, hnX + ex, hnY) - shoreAt(in, hnX - ex, hnY);
    float gy = shoreAt(in, hnX, hnY - ey) - shoreAt(in, hnX, hnY + ey);
    const float len2 = gx * gx + gy * gy;
    if (len2 < 1e-10f) return base;
    const float scale = (water ? -1.0f : 1.0f) / std::sqrt(len2);
    gx *= scale; gy *= scale;
    auto found = base;
    for (int k = 1; k <= 4; ++k) {
        const auto c = colorAt(in, (wx + gx * (40.0f * float(k)) - in.topLeftX) / in.worldSpan,
                                   (in.topLeftY - (wy + gy * (40.0f * float(k)))) / in.worldSpan);
        const float p = waterPaint(c);
        if (water ? p > 0.5f : p < 0.5f) { found = c; break; }
    }
    return mixColor(base, found, want);
}
} // namespace

SurfaceColor evaluateSurfaceMacro(const SurfaceCookInput& in, float u, float v) {
    return followCoast(in, filteredColor(in, axisSamples(u,in.colorWidth,0),axisSamples(v,in.colorHeight,1)),u,v);
}

std::optional<SurfaceAsset> cookSurface(const SurfaceCookInput& in) {
    const uint64_t source = surfaceCookSourceHash(in);
    if (source == 0) return {};
    SurfaceAsset candidate;
    candidate.width = in.outputWidth; candidate.height = in.outputHeight; candidate.sourceHash = source;
    candidate.macroColor.resize(size_t(candidate.width) * candidate.height);
    std::vector<AxisSamples> xs(candidate.width), ys(candidate.height);
    for (uint32_t x=0; x<candidate.width; ++x)
        xs[x]=axisSamples((float(x)+0.5f)/float(candidate.width),in.colorWidth,0);
    for (uint32_t y=0; y<candidate.height; ++y)
        ys[y]=axisSamples((float(y)+0.5f)/float(candidate.height),in.colorHeight,1);
    const auto rows = [&](uint32_t first, uint32_t end) {
        for (uint32_t y=first; y<end; ++y)
            for (uint32_t x=0; x<candidate.width; ++x)
                candidate.macroColor[size_t(y)*candidate.width+x]=followCoast(in,filteredColor(in,xs[x],ys[y]),
                    (float(x)+0.5f)/float(candidate.width),(float(y)+0.5f)/float(candidate.height));
    };
    if (candidate.macroColor.size() >= 1024u*1024u && candidate.height > 1) {
        // At most two cook workers, both within the caller's CPU reservation. Rows have disjoint outputs.
        std::jthread worker(rows, 0u, candidate.height/2);
        rows(candidate.height/2,candidate.height);
    } else rows(0,candidate.height);
    if (!validSurfaceAsset(candidate, source)) return {};
    return candidate;
}

std::optional<SurfaceAsset> resolveSurfaceAsset(const SurfaceCookInput& input, std::span<const uint8_t> candidate) {
    const auto source = surfaceCookSourceHash(input);
    if (source == 0) return {};
    if (auto cached = decodeSurfaceAsset(candidate, source)) return cached;
    return cookSurface(input);
}
} // namespace mc2terrain
