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
inline int partner(int i) { return i >= 1 && i <= 3 ? i + 7 : (i >= 8 ? i - 7 : -1); }
// Per-location limit. MC2 data has no per-location maximum (only the chassis total "MAX Armor"), so
// the limit is the larger of the tabletop rule (2 x structure, head 9) and the legacy distribution of
// a fully plated chassis (base + shares * (maxArmor - baseArmor)), which every stock loadout stays
// under. A torso's front and rear share the front's limit (rear entries are 0; `available` sums the pair).
inline Allocation maxPerLocation(const std::array<int, 8>& structure, const Allocation& base, int maxPlating) {
    const auto full = legacyDistribution(base, std::max(0, maxPlating));
    Allocation a{};
    for (int i = 0; i < 8; ++i) {
        const int legacy = full[i] + (partner(i) >= 0 ? full[partner(i)] : 0);
        a[i] = std::min(255, std::max(i == 0 ? 9 : 2 * structure[i], legacy));
    }
    return a;
}
// Most the sheet can hold: the location limits' sum, bounded by the chassis armor cap.
inline int maxTotal(const Allocation& cap, int maxArmor) { return std::min(total(cap), maxArmor); }
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
// Shared production reset adapter: explicit values never enter legacy subtraction.
struct FactoryBaseline {
    Allocation base{};
    bool valid = false, explicitApplied = false;
    template<class Armor> void capture(const Armor* armor) {
        for (int i=0; i<11; ++i) base[i] = armor[i].maxArmor;
        valid = true; explicitApplied = false;
    }
    template<class Armor, class Count> void restore(Armor* armor, Count& count) {
        if (!valid || !explicitApplied) return;
        for (int i=0; i<11; ++i) armor[i].curArmor = armor[i].maxArmor = base[i];
        count = 0; explicitApplied = false;
    }
    template<class Armor> void apply(Armor* armor, const int* values) {
        for (int i=0; i<11; ++i) armor[i].curArmor = armor[i].maxArmor = values[i];
        explicitApplied = true;
    }
};
} // namespace mc2::armor
#endif
