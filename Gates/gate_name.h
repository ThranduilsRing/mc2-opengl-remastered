// Registered names for tools that write child-process gate settings.
#pragma once
#include "gate.h"

namespace mc2::gate {
template <Name n>
[[nodiscard]] consteval const char* name() noexcept {
    constexpr std::size_t idx = detail::indexOf(n.sv());
    static_assert(idx != detail::kNpos, "gate::name<>: name is not in the gate table");
    return detail::kIndex[idx].name.data();
}
}
