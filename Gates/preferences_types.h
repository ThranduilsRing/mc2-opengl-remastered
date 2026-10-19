#pragma once
#include "preferences_values.h"
#include "render_options.h"
namespace mc2::options {
struct PreferenceValue {
    std::string key, text;
    char type;
    bool operator==(const PreferenceValue&) const = default;
};
} // namespace mc2::options
