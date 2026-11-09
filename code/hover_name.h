#pragma once

#include "dcontact.h"
#include "../resource.h"

namespace mc2 {
// Contacts expose only the identification their sensor quality permits. Never
// read an appearance's cached name: it may be DEMO or a previous visual ID.
constexpr long hoverNameId(long contactStatus, float tonnage, long descId, bool vehicle = false)
{
    if (contactStatus == CONTACT_SENSOR_QUALITY_3) {
        if (vehicle) {
            if (tonnage <= 40) return IDS_SENSOR_LIGHT_VEHICLE;
            if (tonnage <= 65) return IDS_SENSOR_MEDIUM_VEHICLE;
            return IDS_SENSOR_HEAVY_VEHICLE;
        }
        if (tonnage < 40) return IDS_SENSOR_LIGHT_MECH;
        if (tonnage < 65) return IDS_SENSOR_MEDIUM_MECH;
        if (tonnage < 80) return IDS_SENSOR_HEAVY_MECH;
        return IDS_SENSOR_ASSAULT_MECH;
    }
    if (contactStatus == CONTACT_SENSOR_QUALITY_4 || contactStatus == CONTACT_VISUAL)
        return descId > 0 && descId != IDS_DEMO_MSG ? descId : 0;
    return 0;
}
} // namespace mc2
