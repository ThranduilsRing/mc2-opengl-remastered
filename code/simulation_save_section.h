#pragma once
#include "save_archive.h"

namespace mc2::sim {
// A mission's animation section exceeds the generic vector element limit.
// Its byte payload uses the archive's file and decoded-allocation budgets.
inline void serializeAnimationBlob(save::Archive& archive, save::Bytes& bytes)
{
    if (!archive.reading() && bytes.size() > save::maxFileBytes) { archive.reject("animation_bytes"); return; }
    auto length = static_cast<std::uint32_t>(bytes.size());
    archive(length);
    if (!archive.good()) return;
    if (length > save::maxFileBytes) { archive.reject("animation_bytes"); return; }
    if (archive.reading()) {
        if (length > archive.remaining()) { archive.reject("animation_truncated"); return; }
        if (!archive.allocate(length, 1)) return;
        bytes.resize(length);
    }
    archive.raw(bytes);
}
}
