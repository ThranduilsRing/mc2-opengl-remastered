#pragma once

// Ray misses retain the last position without invoking the expensive quad walk.
template<class CameraType, class Position>
bool pickCursorWorld(CameraType* cam, long mx, long my, Position& out, const Position& prev, bool prevValid)
{
	Position hit = out;
	if (cam->pickTerrainRay(mx, my, hit)) {
		out = hit;
		return true;
	}
	if (prevValid)
		out = prev;
	return false;
}
