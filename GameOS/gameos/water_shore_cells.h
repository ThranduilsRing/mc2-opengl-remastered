#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace mc2terrain {

// Conservative intervals only: every height reaching a pixel is still evaluated
// by the shader. Two floats per origin, including an unused last row/column.
inline std::vector<float> bakeWaterShoreCells(const float* fine, int side) {
    const float inf = (std::numeric_limits<float>::max)();
    const size_t count = static_cast<size_t>(side) * side;
    std::vector<float> records(count * 2, 0.0f);
    struct Vertex { float lo, hi; bool flat, interior; };
    std::vector<Vertex> vertices(count);
    const auto raw = [&](int x, int y) {
        return fine[std::clamp(x, 0, side - 1) + std::clamp(y, 0, side - 1) * side];
    };
    for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
        const float c = raw(x, y);
        const float rise = (std::max)({std::abs(raw(x - 2, y) - c), std::abs(raw(x + 2, y) - c),
                                      std::abs(raw(x, y - 2) - c), std::abs(raw(x, y + 2) - c)});
        Vertex v{c, c, rise <= 23.99f, (std::min)({x, y, side - 1 - x, side - 1 - y}) >= 8};
        if (!v.flat) for (int dy = -2; dy <= 2; dy += 2) for (int dx = -2; dx <= 2; dx += 2) {
            v.lo = (std::min)(v.lo, raw(x + dx, y + dy));
            v.hi = (std::max)(v.hi, raw(x + dx, y + dy));
        }
        vertices[x + y * side] = v;
    }
    for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
        const size_t index = 2 * (static_cast<size_t>(x) + static_cast<size_t>(y) * side);
        records[index] = inf;
        if (x == side - 1 || y == side - 1) continue;
        Vertex cache[4][4];
        bool flat = true, interior = true;
        for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) {
            const int vx = (std::min)(x + i, side - 1), vy = (std::min)(y + j, side - 1);
            cache[j][i] = vertices[vx + vy * side];
            flat = flat && cache[j][i].flat && cache[j][i].interior;
            interior = interior && cache[j][i].interior;
        }
        records[index + 1] = flat ? 1.0f : 0.0f;
        if (!interior) continue; // coarse blend has no fine-only bound
        float lo[3][3], hi[3][3];
        constexpr int first[3] = {0, 0, 1}, last[3] = {1, 3, 3};
        for (int j = 0; j < 3; ++j) for (int i = 0; i < 3; ++i) {
            lo[j][i] = inf; hi[j][i] = -inf;
            for (int v = first[j]; v <= last[j]; ++v) for (int u = first[i]; u <= last[i]; ++u) {
                lo[j][i] = (std::min)(lo[j][i], cache[v][u].lo);
                hi[j][i] = (std::max)(hi[j][i], cache[v][u].hi);
            }
            lo[j][i] -= 0.01f; hi[j][i] += 0.01f;
        }
        float upper = 0.0f;
        constexpr float weights[3] = {1.0f, 2.0f, 1.0f};
        for (int j = 0; j < 3; ++j) for (int i = 0; i < 3; ++i)
            upper += weights[j] * weights[i] * hi[j][i];
        float gxHi = 0, gxLo = 0, gyHi = 0, gyLo = 0;
        for (int i = 0; i < 3; ++i) {
            gxHi += weights[i] * (hi[i][2] - lo[i][0]);
            gxLo += weights[i] * (lo[i][2] - hi[i][0]);
            gyHi += weights[i] * (hi[2][i] - lo[0][i]);
            gyLo += weights[i] * (lo[2][i] - hi[0][i]);
        }
        const float gx = (std::max)(std::abs(gxHi), std::abs(gxLo)) / 256.0f;
        const float gy = (std::max)(std::abs(gyHi), std::abs(gyLo)) / 256.0f;
        const float gradient = std::sqrt(gx * gx + gy * gy) * 1.001f;
        records[index] = gradient <= 0.0399f ? -inf
            : upper / 16.0f + 100.0f * (std::max)(gradient, 0.08f) * 1.001f + 0.05f;
    }
    return records;
}

} // namespace mc2terrain
