#pragma once
#include "../code/dcontact.h"

namespace mc2 {
// Home-team contact snapshot. Range/LOS is already resolved by getContactStatus;
// sensor quality alone never grants visual identification or a body/shadow.
struct ContactVisibility {
    int quality = CONTACT_NONE;
    bool model = false, bar = false, tag = false, blip = false, shadow = false;
    constexpr int sensorLevel() const { return model ? 0 : quality; }
};
constexpr ContactVisibility contactVisibility(int quality, bool visualOverride = false)
{
    if (visualOverride) quality = CONTACT_VISUAL;
    const bool visual = quality == CONTACT_VISUAL;
    const bool detected = quality >= CONTACT_SENSOR_QUALITY_1 && quality <= CONTACT_VISUAL;
    return {quality, visual, visual, detected && quality >= CONTACT_SENSOR_QUALITY_3,
            detected, visual};
}
// Presentation only: contact/LOS and saved mover state remain authoritative.
struct DisplayContactHold {
    float lastSeen = -2.0f;
    ContactVisibility update(ContactVisibility contact, float now) {
        if (now < lastSeen) lastSeen = -2.0f;
        if (contact.model) lastSeen = now;
        return now - lastSeen < 1.0f ? contactVisibility(CONTACT_VISUAL) : contact;
    }
};
} // namespace mc2
