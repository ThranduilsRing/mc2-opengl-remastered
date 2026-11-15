#pragma once

namespace graphics {
// Legacy callers clear both flags when a canvas change should keep the mode.
constexpr bool requested_fullscreen(bool current, bool enter, bool leave) {
    return enter || (current && !leave);
}
}
