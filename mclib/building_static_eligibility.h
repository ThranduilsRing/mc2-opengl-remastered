#pragma once

namespace mc2::building_static {

// stopActivity() kills effects but keeps their handles for startActivity() to reuse.
// Only a running effect requires the full appearance update's Execute() pass.
constexpr bool activityNeedsUpdate(bool running, bool primary, bool secondary)
{
    return running && (primary || secondary);
}

} // namespace mc2::building_static
