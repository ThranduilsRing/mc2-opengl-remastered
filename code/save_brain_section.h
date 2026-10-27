// Save v2 BRN1 section: native-brain runtime persistence (design 3.2, B2). Written only when a brain runtime, task queue,
// mission Var or fixed-tick state exists (gates MC2_BRAIN_*), so a default game saves no BRN1 and loads the rebuild default.
#pragma once

#include "save_archive.h"

#include <span>

namespace mc2::save {
// Empty result: nothing to persist (no section is written).
Bytes saveBrainSection();
// False leaves the live state untouched: the whole section is parsed into staging before any warrior changes.
bool restoreBrainSection(std::span<const std::uint8_t> payload);
inline constexpr std::uint32_t brainSectionVersion = 1;
}
