// Logistics-safe ground-vehicle detection from a chassis CSV (VEHICLE-AUTO-PILOT-1).
// Item rows are ItemNMasterId,Load#,X,Y,CompBas#,Name,... so the component NAME is 1-based
// column 6 (column 5 is the numeric CompBas#). Ground vehicles list a "Vehicle ..." component
// (e.g. "Vehicle Sensors") in Item0..Item22 (rows 26..48); mechs never do.
#ifndef CHASSIS_VEHICLE_DETECT_H
#define CHASSIS_VEHICLE_DETECT_H

#include "csvfile.h"
#include "platform_str.h"

inline bool chassisCsvHasVehicleComponent(CSVFile& file)
{
	char comp[256];
	for (int r = 26; r <= 48; ++r)
	{
		if (NO_ERR == file.readString(r, 6, comp, 256))
		{
			S_strlwr(comp);
			if (0 == S_strncmp(comp, "vehicle", 7))
				return true;
		}
	}
	return false;
}

#endif // CHASSIS_VEHICLE_DETECT_H
