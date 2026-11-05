#include "terrain_surface_cook.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <mdspan>
#include <limits>
#include <thread>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace mc2terrain {
std::array<float, 6> terrainPatchHeightErrors(std::span<const float> heights, uint32_t side,
    uint32_t originX, uint32_t originY, uint32_t quadsX, uint32_t quadsY) {
    std::array<float, 6> errors;
    errors.fill(std::numeric_limits<float>::infinity());
    errors[0] = 0;
    if (!side || heights.size() != static_cast<size_t>(side) * side || originX >= side || originY >= side
        || !quadsX || !quadsY || quadsX >= side - originX || quadsY >= side - originY) return errors;
    constexpr uint32_t strides[] = {1, 2, 4, 5, 10, 20};
    const auto h = [&](uint32_t x, uint32_t y) { return heights[originX + x + static_cast<size_t>(originY + y) * side]; };
    for (size_t level = 1; level < errors.size(); ++level) {
        const auto step = strides[level];
        if (quadsX % step || quadsY % step) continue;
        float error = 0;
        for (uint32_t y = 0; y <= quadsY; ++y) for (uint32_t x = 0; x <= quadsX; ++x) {
            const uint32_t x0 = std::min(x / step * step, quadsX - step);
            const uint32_t y0 = std::min(y / step * step, quadsY - step);
            const float u = static_cast<float>(x - x0) / step, v = static_cast<float>(y - y0) / step;
            const float tl = h(x0, y0), br = h(x0 + step, y0 + step);
            const float coarse = u >= v ? tl + u * (h(x0 + step, y0) - tl) + v * (br - h(x0 + step, y0))
                : tl + v * (h(x0, y0 + step) - tl) + u * (br - h(x0, y0 + step));
            error = std::max(error, std::abs(h(x, y) - coarse));
        }
        // Both meshes use the same diagonal slope; their overlay vertices are grid
        // vertices, where the extrema of their piecewise linear difference occur.
        errors[level] = error;
    }
    return errors;
}
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
            const float v = static_cast<float>(i) / 255.0f;
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
    return {static_cast<float>(p[0]) / 255.0f, static_cast<float>(p[1]) / 255.0f, static_cast<float>(p[2]) / 255.0f, 1};
}
SurfaceColor colorAt(const SurfaceCookInput& in, float u, float v) {
    // Clamp texel addresses, not coordinates: matches Vulkan LINEAR/CLAMP_TO_EDGE at the borders.
    const float px = u * static_cast<float>(in.colorWidth) - 0.5f, py = v * static_cast<float>(in.colorHeight) - 0.5f;
    const int x = static_cast<int>(std::floor(px)), y = static_cast<int>(std::floor(py));
    auto at = [&](int xx, int yy) {
        return sourceColor(in, static_cast<size_t>(std::clamp(yy, 0, static_cast<int>(in.colorHeight) - 1)) * in.colorWidth +
                               static_cast<size_t>(std::clamp(xx, 0, static_cast<int>(in.colorWidth) - 1)));
    };
    return mixColor(mixColor(at(x, y), at(x + 1, y), px - static_cast<float>(x)),
                    mixColor(at(x, y + 1), at(x + 1, y + 1), px - static_cast<float>(x)), py - static_cast<float>(y));
}

constexpr float kBlur = 0.0020f, kDiag = kBlur * 0.707f;
constexpr float kOffsets[9][2] = {{0,0}, {kBlur,0}, {-kBlur,0}, {0,kBlur}, {0,-kBlur},
                                 {kDiag,kDiag}, {-kDiag,kDiag}, {kDiag,-kDiag}, {-kDiag,-kDiag}};
struct AxisSample { uint32_t lo, hi; float t; };
using AxisSamples = std::array<AxisSample, 9>;
AxisSamples axisSamples(float uv, uint32_t extent, size_t axis) {
    AxisSamples samples{};
    for (size_t k = 0; k < samples.size(); ++k) {
        const float p = (uv + kOffsets[k][axis]) * static_cast<float>(extent) - 0.5f;
        const int i = static_cast<int>(std::floor(p));
        samples[k] = {static_cast<uint32_t>(std::clamp(i, 0, static_cast<int>(extent) - 1)),
                      static_cast<uint32_t>(std::clamp(i + 1, 0, static_cast<int>(extent) - 1)), p - static_cast<float>(i)};
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
        const size_t a = static_cast<size_t>(y.lo) * in.colorWidth, b = static_cast<size_t>(y.hi) * in.colorWidth;
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
    const float px = u * static_cast<float>(in.shoreWidth - 1), py = v * static_cast<float>(in.shoreHeight - 1);
    const int x = static_cast<int>(std::floor(px)), y = static_cast<int>(std::floor(py));
    float cols[4];
    for (int row = 0; row < 4; ++row) {
        float r[4];
        for (int col = 0; col < 4; ++col)
            r[col] = in.shore[static_cast<size_t>(std::clamp(y - 1 + row, 0, static_cast<int>(in.shoreHeight) - 1)) * in.shoreWidth +
                              static_cast<size_t>(std::clamp(x - 1 + col, 0, static_cast<int>(in.shoreWidth) - 1))];
        cols[row] = catmull(px - static_cast<float>(x), r[0], r[1], r[2], r[3]);
    }
    return catmull(py - static_cast<float>(y), cols[0], cols[1], cols[2], cols[3]);
}
float waterPaint(const SurfaceColor& c) {
    const float t = std::clamp((std::min(c[1], c[2]) - c[0]) / 0.08f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
void hashWord(uint64_t& hash, uint32_t word) {
    const uint8_t bytes[] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};
    hash = surfaceHash(bytes, hash);
}
} // namespace

std::vector<SurfaceColor> surfaceColorsFromRgba(std::span<const uint8_t> rgba, bool srgb) {
    if (rgba.empty() || rgba.size() % 4 != 0 || rgba.size() / 4 > static_cast<uint64_t>(SurfaceAsset::kMaxSide) * SurfaceAsset::kMaxSide)
        return {};
    std::vector<SurfaceColor> colors(rgba.size() / 4);
    for (size_t i = 0; i < colors.size(); ++i) {
        for (size_t c = 0; c < 3; ++c) {
            const float v = static_cast<float>(rgba[i * 4 + c]) / 255.0f;
            colors[i][c] = !srgb ? v : v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        }
        colors[i][3] = 1;
    }
    return colors;
}

bool validSurfaceCookInput(const SurfaceCookInput& in) {
    const uint64_t pixels = static_cast<uint64_t>(in.colorWidth) * in.colorHeight;
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
            in.shore.size() != static_cast<uint64_t>(in.shoreWidth) * in.shoreHeight) return false;
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
                          static_cast<uint32_t>(in.followCoast), static_cast<uint32_t>(in.staticBreakup)}) hashWord(hash, word);
    for (float value : {in.topLeftX, in.topLeftY, in.worldSpan}) hashWord(hash, std::bit_cast<uint32_t>(value));
    if (!in.rgba.empty()) {
        hashWord(hash, 0x41424752u); // RGBA byte source, distinct from a caller's linear float source
        hashWord(hash, static_cast<uint32_t>(in.srgb));
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
    const float ex = 1.0f / static_cast<float>(in.shoreWidth - 1), ey = 1.0f / static_cast<float>(in.shoreHeight - 1);
    float gx = shoreAt(in, hnX + ex, hnY) - shoreAt(in, hnX - ex, hnY);
    float gy = shoreAt(in, hnX, hnY - ey) - shoreAt(in, hnX, hnY + ey);
    const float len2 = gx * gx + gy * gy;
    if (len2 < 1e-10f) return base;
    const float scale = (water ? -1.0f : 1.0f) / std::sqrt(len2);
    gx *= scale; gy *= scale;
    auto found = base;
    for (int k = 1; k <= 4; ++k) {
        const auto c = colorAt(in, (wx + gx * (40.0f * static_cast<float>(k)) - in.topLeftX) / in.worldSpan,
                                   (in.topLeftY - (wy + gy * (40.0f * static_cast<float>(k)))) / in.worldSpan);
        const float p = waterPaint(c);
        if (water ? p > 0.5f : p < 0.5f) { found = c; break; }
    }
    return mixColor(base, found, want);
}
} // namespace

SurfaceColor evaluateSurfaceMacro(const SurfaceCookInput& in, float u, float v) {
    auto color = followCoast(in, filteredColor(in, axisSamples(u,in.colorWidth,0),axisSamples(v,in.colorHeight,1)),u,v);
    if (in.staticBreakup) color[3] = evaluateSurfaceBreakup(in.topLeftX + u * in.worldSpan, in.topLeftY - v * in.worldSpan);
    return color;
}

namespace {
// Ashima simplex response from noise.hglsl. The encoded blend fits the existing alpha channel.
float simplex(float vx, float vy) {
    constexpr float c0 = 0.211324865405187f, c1 = 0.366025403784439f;
    constexpr float c2 = -0.577350269189626f, c3 = 0.024390243902439f;
    const float skew = (vx + vy) * c1;
    float ix = std::floor(vx + skew), iy = std::floor(vy + skew);
    const float unskew = (ix + iy) * c0;
    const float x = vx - ix + unskew, y = vy - iy + unskew;
    const float i1x = x > y ? 1.0f : 0.0f, i1y = x > y ? 0.0f : 1.0f;
    auto mod289 = [](float a) { return a - std::floor(a * (1.0f / 289.0f)) * 289.0f; };
    auto permute = [&](float a) { return mod289(((a * 34.0f) + 1.0f) * a); };
    ix = mod289(ix); iy = mod289(iy);
    const float xs[3] = {x, x + c0 - i1x, x + c2};
    const float ys[3] = {y, y + c0 - i1y, y + c2};
    const float oxs[3] = {0, i1x, 1}, oys[3] = {0, i1y, 1};
    float value = 0;
    for (int k = 0; k < 3; ++k) {
        const float p = permute(permute(iy + oys[k]) + ix + oxs[k]);
        float m = std::max(0.5f - (xs[k] * xs[k] + ys[k] * ys[k]), 0.0f);
        m *= m; m *= m;
        const float a = 2.0f * (p * c3 - std::floor(p * c3)) - 1.0f;
        const float h = std::abs(a) - 0.5f;
        const float a0 = a - std::floor(a + 0.5f);
        m *= 1.79284291400159f - 0.85373472095314f * (a0 * a0 + h * h);
        value += m * (a0 * xs[k] + h * ys[k]);
    }
    return 130.0f * value;
}
float surfaceFbm(float x, float y, int octaves) {
    float value = 0, amplitude = 0.5f, frequency = 1;
    for (int i = 0; i < octaves; ++i) {
        value += amplitude * simplex(x * frequency, y * frequency);
        frequency *= 2; amplitude *= 0.5f;
    }
    return value;
}
} // namespace

float evaluateSurfaceBreakup(float worldX, float worldY) {
    const float low = surfaceFbm(worldX * 0.0035f, worldY * 0.0035f, 3) * 0.5f + 0.5f;
    const float high = surfaceFbm(worldX * 0.018f, worldY * 0.018f, 2) * 0.5f + 0.5f;
    return mix(low, high, 0.55f);
}

std::optional<SurfaceGradients> cookSurfaceGradients(std::span<const float> heights, uint32_t side) {
    if (side < 2 || side > 4097 || heights.size() != static_cast<uint64_t>(side) * side) return {};
    for (float h : heights) if (!std::isfinite(h)) return {};
    SurfaceGradients result;
    result.side = side + 2;
    result.texels.resize(static_cast<size_t>(result.side) * result.side);
    const auto height = [&](int x, int y) {
        return heights[static_cast<size_t>(std::clamp(y, 0, static_cast<int>(side) - 1)) * side + static_cast<size_t>(std::clamp(x, 0, static_cast<int>(side) - 1))];
    };
    for (int y = -1; y <= static_cast<int>(side); ++y) for (int x = -1; x <= static_cast<int>(side); ++x)
        result.texels[static_cast<size_t>(y + 1) * result.side + static_cast<size_t>(x + 1)] = {
            -(height(x + 1, y) - height(x - 1, y)) / 256.0f,
             (height(x, y + 1) - height(x, y - 1)) / 256.0f};
    return result;
}

std::optional<SurfaceAsset> cookSurface(const SurfaceCookInput& in) {
    const uint64_t source = surfaceCookSourceHash(in);
    if (source == 0) return {};
    SurfaceAsset candidate;
    candidate.width = in.outputWidth; candidate.height = in.outputHeight; candidate.sourceHash = source;
    candidate.macroColor.resize(static_cast<size_t>(candidate.width) * candidate.height);
    std::vector<AxisSamples> xs(candidate.width), ys(candidate.height);
    for (uint32_t x=0; x<candidate.width; ++x)
        xs[x]=axisSamples((static_cast<float>(x)+0.5f)/static_cast<float>(candidate.width),in.colorWidth,0);
    for (uint32_t y=0; y<candidate.height; ++y)
        ys[y]=axisSamples((static_cast<float>(y)+0.5f)/static_cast<float>(candidate.height),in.colorHeight,1);
    const auto rows = [&](uint32_t first, uint32_t end) {
        for (uint32_t y=first; y<end; ++y)
            for (uint32_t x=0; x<candidate.width; ++x) {
                candidate.macroColor[static_cast<size_t>(y)*candidate.width+x]=followCoast(in,filteredColor(in,xs[x],ys[y]),
                    (static_cast<float>(x)+0.5f)/static_cast<float>(candidate.width),(static_cast<float>(y)+0.5f)/static_cast<float>(candidate.height));
                if (in.staticBreakup) candidate.macroColor[static_cast<size_t>(y)*candidate.width+x][3] = evaluateSurfaceBreakup(
                    in.topLeftX + (static_cast<float>(x)+0.5f)/static_cast<float>(candidate.width)*in.worldSpan,
                    in.topLeftY - (static_cast<float>(y)+0.5f)/static_cast<float>(candidate.height)*in.worldSpan);
            }
    };
    if (candidate.macroColor.size() >= 1024u*1024u && candidate.height > 1) {
        // At most two cook workers, both within the caller's CPU reservation. Rows have disjoint outputs.
        std::jthread worker(rows, 0u, candidate.height/2);
        rows(candidate.height/2,candidate.height);
    } else rows(0,candidate.height);
    if (!validSurfaceAsset(candidate, source)) return {};
    return candidate;
}

std::optional<SurfaceAsset> resolveSurfaceAsset(const SurfaceCookInput& input, std::span<const uint8_t> candidate,
                                                const std::string& cacheDir, const char** origin) {
    const auto source = surfaceCookSourceHash(input);
    if (source == 0) return {};
    if (origin) *origin = "precooked";
    if (auto cached = decodeSurfaceAsset(candidate, source)) return cached;
    if (origin) *origin = "cache";
    if (auto cached = loadCachedSurfaceAsset(cacheDir, source)) return cached;
    auto cooked = cookSurface(input);
    if (origin) *origin = "cooked";
    if (cooked && !cacheDir.empty() && storeCachedSurfaceAsset(cacheDir, *cooked) && origin) *origin = "cooked+stored";
    return cooked;
}

std::vector<uint8_t> cookSurfaceMaterials(const SurfaceAsset& surface,
    std::span<const float> materials, uint32_t side, std::span<const uint8_t> control, uint32_t controlSide,
    uint32_t width, uint32_t height, bool sandProfile) {
    if (!validSurfaceAsset(surface, surface.sourceHash) || !sizeOk(width, height) ||
        (!materials.empty() && (side < 2 || materials.size() != static_cast<uint64_t>(side) * side)) ||
        (!control.empty() && (controlSide < 1 || control.size() != static_cast<uint64_t>(controlSide) * controlSide * 4))) return {};
    const auto smooth = [](float lo, float hi, float v) {
        const float t = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    std::vector<uint8_t> result(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / width, v = (static_cast<float>(y) + 0.5f) / height;
        const auto& c = surface.macroColor[static_cast<size_t>(std::min(static_cast<uint32_t>(v * surface.height), surface.height - 1)) * surface.width +
                                           std::min(static_cast<uint32_t>(u * surface.width), surface.width - 1)];
        const float bright = std::max({c[0], c[1], c[2]});
        const float darkest = std::min({c[0], c[1], c[2]});
        const float saturation = (bright - darkest) / std::max(bright, 1e-10f);
        float snow = smooth(0.25f, 0.55f, smooth(0.15f, 0.03f, saturation) * smooth(0.42f, 0.62f, bright));
        // Color is an import fallback for older maps, never fragment work.
        float grass = smooth(-0.02f, 0.06f, c[1] - c[0]) * smooth(0.22f, 0.40f, c[1]);
        float dirt = smooth(-0.02f, 0.06f, c[0] - c[1]) * smooth(0.22f, 0.45f, c[0]);
        float rock = 1.0f - std::max(grass, dirt);
        const float water = smooth(0.0f, 0.08f, std::min(c[1], c[2]) - c[0]);
        rock += water; grass *= 1.0f - water; dirt *= 1.0f - water;
        const float total = rock + grass + dirt;
        grass /= total; dirt /= total;
        if (!materials.empty()) {
            const float px = u * (side - 1), py = v * (side - 1);
            const uint32_t ix = static_cast<uint32_t>(px), iy = static_cast<uint32_t>(py);
            float semanticGrass = 0, semanticDirt = 0;
            const std::mdspan<const float, std::dextents<size_t, 2>> grid(materials.data(), side, side);
            for (uint32_t dy = 0; dy < 2; ++dy) for (uint32_t dx = 0; dx < 2; ++dx) {
                const float w = (dx ? px - ix : 1.0f - (px - ix)) * (dy ? py - iy : 1.0f - (py - iy));
                const size_t row = std::min(iy + dy, side - 1);
                const size_t col = std::min(ix + dx, side - 1);
                const float m = grid[row, col];
                semanticGrass += m == 1 ? w : 0;
                semanticDirt += sandProfile && m == 2 ? w : 0;
            }
            const float semantic = semanticGrass + semanticDirt;
            grass = grass * (1.0f - semantic) + semanticGrass;
            dirt = dirt * (1.0f - semantic) + semanticDirt;
            snow *= 1.0f - semantic;
        }
        float concrete = 0;
        if (!control.empty()) {
            const size_t i = (static_cast<size_t>(std::min(static_cast<uint32_t>(v * controlSide), controlSide - 1)) * controlSide +
                                     std::min(static_cast<uint32_t>(u * controlSide), controlSide - 1)) * 4;
            const float sum = static_cast<float>(control[i]) + control[i+1] + control[i+2] + control[i+3];
            if (sum > 0) {
                grass = control[i+1] / sum; dirt = control[i+2] / sum; concrete = control[i+3] / sum;
                snow = 0;
            }
        }
        // Snow is additive over the (1 - snow) remainder, as the runtime classifier did.
        grass *= 1.0f - snow; dirt *= 1.0f - snow;
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        const float w[] = {grass, dirt, snow, concrete};
        // Quantization must not turn a convex material blend into negative rock.
        unsigned remaining = 255;
        for (size_t k = 0; k < 4; ++k) {
            const unsigned q = std::min(k == 2 ? 255u : remaining, static_cast<unsigned>(std::lround(std::clamp(w[k], 0.0f, 1.0f) * 255)));
            result[i+k] = static_cast<uint8_t>(q); if (k != 2) remaining -= q;
        }
    }
    return result;
}
} // namespace mc2terrain
