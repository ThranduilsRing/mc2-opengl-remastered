// Camera view export for the editor's HZB replay button.
#include "gamecam.h"
#include "Gates/gate_cold.h"
#include <cstdio>
#include <string>
#include "mc2fmt.h"

extern "C" int mc2_hzb_dump_camera_view(void)
{
	const std::string envPath = mc2::gate::str<"MC2_HZB_VIEW_FILE">();
	const char* dumpPath = !envPath.empty() ? envPath.c_str() : "A:/Games/mc2-hzb-cull/saved_view.txt";
	if (!eye) {
		mc2::fmt::print_err("[HZB_VIEW_DUMP v1] ERROR: eye is null\n");
		fflush(stderr);
		return 0;
	}
	glm::vec3 pos = eye->getPosition();
	float rot   = eye->getCameraRotation();
	float angle = eye->getProjectionAngle();
	float alt   = eye->getCameraAltitude();
	FILE* df = fopen(dumpPath, "w");
	if (df) {
		mc2::fmt::print(df, "X={:.4f} Y={:.4f} Z={:.4f} ROT={:.4f} ANGLE={:.4f} ALT={:.4f}\n",
		        pos.x, pos.y, pos.z, rot, angle, alt);
		fclose(df);
		mc2::fmt::print_err("[HZB_VIEW_DUMP v1] saved X={:.4f} Y={:.4f} Z={:.4f} ROT={:.4f} ANGLE={:.4f} ALT={:.4f} -> '{}'\n",
		        pos.x, pos.y, pos.z, rot, angle, alt, dumpPath);
		fflush(stderr);
		return 1;
	}
	mc2::fmt::print_err("[HZB_VIEW_DUMP v1] ERROR: cannot open '{}' for write\n", dumpPath);
	fflush(stderr);
	return 0;
}
