// Counted navigation value packets. Asset packets retain their native format.
#pragma once
#include "save_sections.h"

namespace mc2::save {
struct NavigationFormat {
    bool cells = false;
    bool areas = false;
    bool doors = false;
    bool links = false;
};

// The already-validated map dimensions/counts bound decoding, including grids
// larger than Archive's ordinary 65536-element sequence limit.
template<class T> Bytes encodeNavigationRecord(std::span<T> values, std::uint32_t tag) {
    Archive a;
    std::uint32_t version = 1, count = static_cast<std::uint32_t>(values.size());
    if (values.size() > maxFileBytes / sizeof(T)) return {};
    a(tag, version, count);
    for (auto& value : values) a(value);
    return a.good() ? a.bytes() : Bytes{};
}

template<class T> bool decodeNavigationRecord(std::span<const std::uint8_t> bytes,
                                             std::span<T> values, std::uint32_t expected) {
    Archive a(bytes);
    std::uint32_t tag = 0, version = 0, count = 0;
    a(tag, version, count);
    if (!a.good() || tag != expected || version != 1 || count != values.size() ||
        !a.allocate(count, sizeof(T))) return false;
    std::vector<T> staged(count);
    for (auto& value : staged) a(value);
    if (!a.finish()) return false;
    std::copy(staged.begin(), staged.end(), values.begin());
    return true;
}
}
