#pragma once
#include <algorithm>
#include <string>
#include <string_view>

namespace mc2prop {
// Stock names already contain ObjectTextureSize (usually data/tgl/128).
inline std::string tieredKtxPath(std::string_view source, int tier) {
    std::string path(source);
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) path.erase(dot);
    const auto last = path.find_last_of('/');
    std::string parent = last == std::string::npos ? "" : path.substr(0, last + 1);
    const std::string file = path.substr(last == std::string::npos ? 0 : last + 1);
    if (!parent.empty()) {
        const auto previous = parent.find_last_of('/', parent.size() - 2);
        const auto start = previous == std::string::npos ? 0 : previous + 1;
        const auto component = parent.substr(start, parent.size() - start - 1);
        if (component == "32" || component == "64" || component == "128" || component == "256" ||
            component == "512" || component == "1024") parent.erase(start);
    }
    return parent + std::to_string(tier) + "/" + file + ".ktx2";
}
}
