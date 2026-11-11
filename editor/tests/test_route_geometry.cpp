#include "../../tests/unit/doctest.h"
#include "../EditorRouteGeometry.h"

namespace
{
struct P { float x, y, z; };
}

TEST_CASE("route geometry: nearestWaypoint picks the closest point inside the radius")
{
	const std::vector<P> wps{ {0, 0, 0}, {100, 0, 0}, {100, 100, 0} };
	CHECK(EditorRouteGeometry::nearestWaypoint(wps, 98, 3, 10) == 1);
	CHECK(EditorRouteGeometry::nearestWaypoint(wps, 50, 50, 10) == -1);
	CHECK(EditorRouteGeometry::nearestWaypoint(std::vector<P>{}, 0, 0, 10) == -1);
	// two candidates inside the radius: the nearer one wins
	const std::vector<P> close{ {0, 0, 0}, {4, 0, 0} };
	CHECK(EditorRouteGeometry::nearestWaypoint(close, 3, 0, 10) == 1);
}

TEST_CASE("route geometry: insertIndexFor splits the nearest segment, loop adds the closing one")
{
	const std::vector<P> wps{ {0, 0, 0}, {100, 0, 0}, {100, 100, 0} };
	CHECK(EditorRouteGeometry::insertIndexFor(wps, 50, 2, false) == 1);    // between 0 and 1
	CHECK(EditorRouteGeometry::insertIndexFor(wps, 98, 50, false) == 2);   // between 1 and 2
	// near the closing segment (2 -> 0): only a loop route inserts there
	CHECK(EditorRouteGeometry::insertIndexFor(wps, 40, 40, true) == 3);
	CHECK(EditorRouteGeometry::insertIndexFor(wps, 40, 40, false) != 3);
	// empty and single-point routes append
	CHECK(EditorRouteGeometry::insertIndexFor(std::vector<P>{}, 5, 5, false) == 0);
	CHECK(EditorRouteGeometry::insertIndexFor(std::vector<P>{ {1, 1, 0} }, 5, 5, true) == 1);
}
