#pragma once
#include "platform_windows.h"
// BT2018-FIRST-CLASS P6: package chassis HUD icons.
//
// A package chassis names a strip base in its chassisdef.fit ([Chassis] HudIcon); the pack ships
// <base>_<W>x<H>.tga for each HUD size (one column, row 0 front, row 1 rear, exact region palette;
// tools/bt2018_mechs/render_icons.py). Stock units keep resolving through the name-keyed
// unit_icons registry; a package chassis with a strip bypasses it and blits its own strip.

class Mover;

namespace mechicon_package_hud {
// True and `dest` filled (width x height, row stride `stride`) when `unit` is a package chassis with
// a valid strip for this HUD size; false leaves `dest` untouched so the caller uses the registry.
bool blit(Mover* unit, float iconX, float iconY, bool back, DWORD* dest, int stride, int width, int height);
}
