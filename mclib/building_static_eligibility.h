#pragma once

namespace mc2::building_static {

// A capture flash still needs updates during its dark intervals and final clear.
constexpr bool flashNeedsUpdate(bool lit, float remaining)
{
    return lit || remaining > 0.0f;
}

// stopActivity() kills effects but keeps their handles for startActivity() to reuse.
// Only a running effect requires the full appearance update's Execute() pass.
constexpr bool activityNeedsUpdate(bool running, bool primary, bool secondary)
{
    return running && (primary || secondary);
}

} // namespace mc2::building_static
