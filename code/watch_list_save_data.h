// Watch IDs retain their slot order and null-handle convention.
#pragma once
#include "save_sections.h"

namespace mc2::save {
inline constexpr auto watchListTag = fourcc('W', 'A', 'T', 'C');
struct SavedWatchList {
    std::vector<std::int32_t> indices;
    void serialize(Archive& a) { a.sequence(indices, maxSequenceLength); }
};
}
