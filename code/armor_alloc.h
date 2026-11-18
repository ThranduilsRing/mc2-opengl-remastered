#ifndef MC2_ARMOR_ALLOC_H
#define MC2_ARMOR_ALLOC_H
#include <algorithm>
#include <array>
#include <numeric>

namespace mc2::armor {
using Allocation = std::array<int, 11>;
inline constexpr std::array<float, 11> shares{0.0f, .17f, .125f, .125f, .12f, .12f, .13f, .13f, .03f, .02f, .02f};
inline int total(const Allocation& a) { return std::accumulate(a.begin(), a.end(), 0); }
inline Allocation legacyDistribution(const Allocation& base, int plating) {
    auto a = base;
    for (int i = 0; i < 11; ++i) a[i] += static_cast<int>(shares[i] * plating);
    return a;
}
inline Allocation maxPerLocation(const Allocation& base, int baseTotal, int maxArmor) {
    auto a = base;
    const int d = std::max(0, maxArmor - baseTotal);
    for (int i = 0; i < 11; ++i) a[i] = std::min(255, base[i] + 2 * static_cast<int>(shares[i] * d));
    return a;
}
inline int partner(int i) { return i >= 1 && i <= 3 ? i + 7 : (i >= 8 ? i - 7 : -1); }
inline int available(const Allocation& a, int i, const Allocation& cap, int pool) {
    const int other = partner(i);
    const int room = other < 0 ? cap[i] - a[i] : cap[i] + cap[other] - a[i] - a[other];
    return std::max(0, std::min({pool - total(a), 255 - a[i], room}));
}
inline Allocation normalise(Allocation a, const Allocation& base, const Allocation& cap, int pool) {
    for (int i = 0; i < 11; ++i) a[i] = std::clamp(a[i], 0, partner(i) < 0 ? cap[i] : 255);
    // Largest surplus first; iteration order breaks ties deterministically.
    auto reduce = [&](int first, int last) {
        int pick = first;
        for (int i = first; i <= last; ++i)
            if (a[i] > 0 && (a[pick] == 0 || a[i] - base[i] > a[pick] - base[pick])) pick = i;
        if (a[pick] > 0) --a[pick];
    };
    for (int front = 1; front <= 3; ++front) {
        const int rear = front + 7;
        while (a[front] + a[rear] > cap[front] + cap[rear]) {
            const int pick = a[front] > 0 && (a[rear] == 0 || a[front] - base[front] >= a[rear] - base[rear]) ? front : rear;
            --a[pick];
        }
    }
    while (total(a) > std::max(0, pool)) reduce(0, 10);
    return a;
}
inline bool step(Allocation& a, int i, int delta, const Allocation& cap, int pool) {
    if (i < 0 || i >= 11 || (delta != 1 && delta != -1)) return false;
    if (delta > 0 ? available(a, i, cap, pool) == 0 : a[i] == 0) return false;
    a[i] += delta;
    return true;
}
} // namespace mc2::armor
#endif
