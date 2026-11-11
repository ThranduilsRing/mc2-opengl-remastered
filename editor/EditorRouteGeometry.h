#ifndef EDITOR_ROUTE_GEOMETRY_H
#define EDITOR_ROUTE_GEOMETRY_H
// Pure route geometry for the viewport route tool (no engine, no undo). Templates over any
// point type with .x/.y so editor_document_tests can cover them without the engine headers.

#include <cstddef>
#include <vector>

namespace EditorRouteGeometry
{
	// Index of the waypoint within radius of (x,y), nearest first; -1 if none.
	template <class P>
	int nearestWaypoint( const std::vector<P>& wps, float x, float y, float radius )
	{
		int best = -1;
		float bestD2 = radius * radius;
		for ( size_t i = 0; i < wps.size(); ++i )
		{
			const float dx = wps[i].x - x, dy = wps[i].y - y, d2 = dx * dx + dy * dy;
			if ( d2 <= bestD2 )
			{
				bestD2 = d2;
				best = (int)i;
			}
		}
		return best;
	}

	// Index at which a point at (x,y) splits the nearest segment (loop adds the closing
	// segment). Empty or single-point routes append.
	template <class P>
	int insertIndexFor( const std::vector<P>& wps, float x, float y, bool loop )
	{
		if ( wps.size() < 2 )
			return (int)wps.size();
		const size_t segs = loop ? wps.size() : wps.size() - 1;
		int best = (int)wps.size();
		float bestD2 = 1e30f;
		for ( size_t i = 0; i < segs; ++i )
		{
			const P& a = wps[i];
			const P& b = wps[( i + 1 ) % wps.size()];
			const float ex = b.x - a.x, ey = b.y - a.y, len2 = ex * ex + ey * ey;
			float t = len2 > 1e-6f ? ( ( x - a.x ) * ex + ( y - a.y ) * ey ) / len2 : 0.0f;
			t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
			const float dx = a.x + t * ex - x, dy = a.y + t * ey - y, d2 = dx * dx + dy * dy;
			if ( d2 < bestD2 )
			{
				bestD2 = d2;
				best = (int)i + 1;
			}
		}
		return best;
	}
}

#endif // EDITOR_ROUTE_GEOMETRY_H
