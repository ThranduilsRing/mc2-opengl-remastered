#pragma once
#include "../code/save_archive.h"

namespace mc2 {
// Hash resolved source bytes, including packed/mod inputs, rather than cache IDs.
class TerrainCacheKey {
    save::Bytes inputs;
public:
    explicit TerrainCacheKey(std::string_view recipe = "txm-composite-v1") { add(recipe, {}); }
    void add(std::string_view name, std::span<const std::uint8_t> bytes) {
        const auto path = save::sha256({reinterpret_cast<const std::uint8_t*>(name.data()), name.size()});
        const auto content = save::sha256(bytes);
        inputs.insert(inputs.end(), path.begin(), path.end());
        inputs.insert(inputs.end(), content.begin(), content.end());
    }
    std::string key() const {
        std::string result;
        for (auto byte : save::sha256(inputs)) {
            result += "0123456789abcdef"[byte >> 4];
            result += "0123456789abcdef"[byte & 15];
        }
        return result;
    }
};
}
