// Small save-side helpers kept header-only so the unit tier can exercise the real code.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>
#include "mc2fmt.h"

namespace mc2::save {
// Nine significant digits round-trip a float exactly; atof reads them back as before (legacy "{:f}" saves load unchanged).
inline void exactFloatLine(char (&line)[255], std::string_view name, float value) {
    mc2::fmt::to_buf(line, "f {} = {:.9g}\r\n", name, value);
}

// Reorders a typed pool to the saved order when the saved handles name exactly the objects it holds; otherwise leaves it alone.
template<class Pool, class Obj>
void applyPoolOrder(std::span<Pool> pool, const std::vector<int32_t>& handles, Obj** objList, long maxObjects) {
    if (!pool.data() || pool.empty() || handles.size() != pool.size()) return;
    std::vector<Pool> next(pool.size());
    for (size_t i = 0; i < pool.size(); ++i) {
        const int32_t h = handles[i];
        if (h < 1 || h > maxObjects || !objList[h]) return;
        next[i] = static_cast<Pool>(objList[h]);
    }
    std::vector<Pool> sortedNext = next, current(pool.begin(), pool.end());
    std::sort(sortedNext.begin(), sortedNext.end());
    std::sort(current.begin(), current.end());
    if (sortedNext != current) return;
    std::copy(next.begin(), next.end(), pool.begin());
}
}
