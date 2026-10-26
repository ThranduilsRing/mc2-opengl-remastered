#pragma once
#include "Gates/gate_cold.h"
#include "Gates/render_options_cfg.h"
#include "Gates/render_options_legacy_env.h"
#include "../3rdparty/include/nlohmann/json.hpp"
#include <charconv>
#include <cctype>

namespace PlayerFrontEnd {
// Drop retired/unknown environment names even when their old value is invalid.
inline void migrateLegacySettings(nlohmann::json& settings, std::string& cfg) {
    for (const auto& key : mc2::options::legacy::kLegacyKeys) {
        const auto it = settings.find(key.envName);
        if (it == settings.end()) continue;
        const auto* option = mc2::options::find(key.option);
        const auto named = settings.find(key.option);
        const auto& source = named != settings.end() ? *named : *it;
        if (source.is_string()) {
            const auto value = mc2::options::legacy::translate(*option, source.get<std::string>());
            if (value) cfg = mc2::options::cfg::set(std::move(cfg), *option, *value);
        }
        settings.erase(key.envName);
    }
    for (const auto& option : mc2::options::kOptions) {
        const auto it = settings.find(option.key);
        if (it == settings.end()) continue;
        if (it->is_string()) {
            auto value = it->get<std::string>();
            if (value.empty() && option.kind == mc2::options::Kind::Bool) value = "FALSE";
            const auto translated = mc2::options::legacy::translate(option, value);
            if (translated) cfg = mc2::options::cfg::set(std::move(cfg), option, *translated);
        }
        settings.erase(option.key);
    }
    for (auto it = settings.begin(); it != settings.end();) {
        if (it.key().starts_with("MC2_") && !mc2::gate::lookupName(it.key())) it = settings.erase(it);
        else ++it;
    }
}

inline bool hasHighObjectNumber(std::string_view source) {
    for (std::size_t at = source.find("objectnumber"); at != std::string_view::npos;
         at = source.find("objectnumber", at + 12)) {
        const auto lineStart = source.rfind('\n', at);
        const auto start = lineStart == std::string_view::npos ? 0 : lineStart + 1;
        const auto comment = source.find_first_of(";/", start);
        if (comment < at || (at > start && (std::isalnum(static_cast<unsigned char>(source[at - 1])) || source[at - 1] == '_'))) continue;
        auto value = source.substr(at + 12, source.find_first_of("\r\n", at) - (at + 12));
        const auto equal = value.find_first_not_of(" \t");
        if (equal == std::string_view::npos || value[equal] != '=') continue;
        value.remove_prefix(equal + 1);
        const auto number = value.find_first_not_of(" \t");
        if (number == std::string_view::npos) continue;
        value.remove_prefix(number);
        int id = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
        if (parsed.ec == std::errc{} && id > 1188) return true;
    }
    return false;
}
}
