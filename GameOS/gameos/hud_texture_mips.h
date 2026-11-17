#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <cstring>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace mc2hud {
// Power-of-two RGBA8 atlas. Keep filtering in cached CPU memory: mapped
// HOST_COHERENT storage can be uncached, making the read/modify/write pass stall.
inline std::vector<uint8_t> premultipliedMips(const std::vector<uint8_t>& rgba, size_t side) {
    const size_t baseBytes = side * side * 4;
    std::vector<uint8_t> result((baseBytes * 4 - 4) / 3);
    std::memcpy(result.data(), rgba.data(), baseBytes);
    auto* pixels = result.data();
    size_t i = 0;
#if defined(__SSE2__) || defined(_M_X64)
    const auto zero = _mm_setzero_si128();
    const auto rounding = _mm_set1_epi16(128);
    const auto alphaMask = _mm_set1_epi32(static_cast<int>(0xff000000u));
    for (; i + 16 <= baseBytes; i += 16) {
        const auto src = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels + i));
        auto premultiply = [&](auto lanes) {
            const auto alpha = _mm_shufflehi_epi16(_mm_shufflelo_epi16(lanes, 255), 255);
            auto product = _mm_add_epi16(_mm_mullo_epi16(lanes, alpha), rounding);
            // Exact rounded division by 255 for all 8-bit products.
            return _mm_srli_epi16(_mm_add_epi16(product, _mm_srli_epi16(product, 8)), 8);
        };
        const auto rgb = _mm_packus_epi16(premultiply(_mm_unpacklo_epi8(src, zero)),
                                         premultiply(_mm_unpackhi_epi8(src, zero)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(pixels + i),
            _mm_or_si128(_mm_andnot_si128(alphaMask, rgb), _mm_and_si128(alphaMask, src)));
    }
#endif
    for (; i < baseBytes; i += 4)
        for (size_t c = 0; c < 3; ++c)
            pixels[i+c] = (static_cast<uint32_t>(pixels[i+c]) * pixels[i+3] + 127u) / 255u;
    size_t offset = 0, next = baseBytes;
    for (size_t w = side; w > 1; w /= 2) {
        for (size_t y = 0; y < w/2; ++y) {
            size_t x = 0;
#if defined(__SSE2__) || defined(_M_X64)
            for (; x + 2 <= w/2; x += 2) {
                const auto* row = pixels + offset + (2*y*w + 2*x)*4;
                const auto top = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row));
                const auto bottom = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + w*4));
                auto filter = [&](auto a, auto b) {
                    auto sum = _mm_add_epi16(a, b);
                    sum = _mm_add_epi16(sum, _mm_srli_si128(sum, 8));
                    return _mm_srli_epi16(_mm_add_epi16(sum, _mm_set1_epi16(2)), 2);
                };
                const auto lo = filter(_mm_unpacklo_epi8(top, zero), _mm_unpacklo_epi8(bottom, zero));
                const auto hi = filter(_mm_unpackhi_epi8(top, zero), _mm_unpackhi_epi8(bottom, zero));
                const auto pair = _mm_packus_epi16(_mm_unpacklo_epi64(lo, hi), zero);
                _mm_storel_epi64(reinterpret_cast<__m128i*>(pixels + next + (y*(w/2)+x)*4), pair);
            }
#endif
            for (; x < w/2; ++x)
            for (size_t c = 0; c < 4; ++c) {
                const size_t p = offset + ((2*y)*w + 2*x)*4 + c;
                pixels[next + (y*(w/2)+x)*4+c] = (static_cast<uint32_t>(pixels[p]) + pixels[p+4] + pixels[p+w*4] + pixels[p+w*4+4] + 2u) / 4u;
            }
        }
        offset = next; next += (w/2)*(w/2)*4;
    }
    return result;
}
} // namespace mc2hud
