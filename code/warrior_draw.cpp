#include "render_view.h"
// MechWarrior: waypoint and overview move-path drawing.
// Declared in the class headers; shared names in warrior_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "warrior_internal.h"


void MechWarrior::extractHudPaths(mc2::render_view::HudMover& row)
{
    if (!tacOrderQueue || !getVehicle()) return;
    row.waypointStart = row.position;
    glm::vec3 first(getCurTacOrder()->moveParams.wayPath.points[0],
        getCurTacOrder()->moveParams.wayPath.points[1], getCurTacOrder()->moveParams.wayPath.points[2]);
    row.overviewPath.push_back(row.position);
    if (tacOrderQueueLooping && numTacOrdersQueued > 0)
        row.waypointStart = tacOrderQueue[numTacOrdersQueued - 1].point;
    else if (first.x != 0 && first.y != 0) {
        first.z = TerrainRuntime::groundElevation(first);
        row.waypointFirst = true;
        row.waypoints.push_back(first);
        row.overviewPath.push_back(first);
    }
    for (int i = 0; i < numTacOrdersQueued; ++i) {
        row.waypoints.push_back(tacOrderQueue[i].point);
        row.waypointMarkers.push_back(queueMarkers[i]);
        if (row.overviewPath.size() < 64) row.overviewPath.push_back(tacOrderQueue[i].point);
    }
}

void mc2::render_view::drawHudWaypoints(const HudMover& row)
{
    glm::vec3 previous = row.waypointStart;
    for (size_t i = 0; i < row.waypoints.size(); ++i) {
        const size_t marker = i - static_cast<size_t>(row.waypointFirst);
        if (i >= static_cast<size_t>(row.waypointFirst) && row.waypointMarkers[marker])
            row.waypointMarkers[marker]->render();
        Stuff::Vector4D a, b;
        // Preserve the legacy projection direction for the preview and queued segments.
        glm::vec3 start = row.waypointFirst && i == 0 ? previous : row.waypoints[i];
        glm::vec3 end = row.waypointFirst && i == 0 ? row.waypoints[i] : previous;
        eye->projectForScreenXY(start, a);
        eye->projectForScreenXY(end, b);
        a.z = b.z = 0.1f; a.w = b.w = 0.9999f;
        LineElement line(a, b, 0xffff0000, nullptr, -1);
        line.draw();
        previous = row.waypoints[i];
    }
}

void MechWarrior::drawWaypointPath()
{
    for (const auto& row : mc2::render_view::hud().movers)
        if (row.pilot == this) { mc2::render_view::drawHudWaypoints(row); break; }
}

static void drawThickSeg( float x0, float y0, float x1, float y1, float hw, unsigned long color )
{
	float dx = x1 - x0, dy = y1 - y0;
	float len = sqrtf( dx * dx + dy * dy );
	if ( len < 0.001f ) return;
	float nx = -dy / len * hw, ny = dx / len * hw;
	gos_VERTEX q[4];
	for ( int v = 0; v < 4; ++v ) { q[v].z = 0; q[v].rhw = .5f; q[v].argb = color; q[v].frgb = 0; q[v].u = q[v].v = 0; }
	q[0].x = x0 + nx; q[0].y = y0 + ny;
	q[1].x = x1 + nx; q[1].y = y1 + ny;
	q[2].x = x1 - nx; q[2].y = y1 - ny;
	q[3].x = x0 - nx; q[3].y = y0 - ny;
	gos_DrawQuads( q, 4 );
}

static void drawArrowHead( float tx, float ty, float dx, float dy, float sz, unsigned long color )
{
	float len = sqrtf( dx * dx + dy * dy );
	if ( len < 0.001f ) return;
	float ux = dx / len, uy = dy / len;	// forward
	float px = -uy, py = ux;			// perpendicular
	gos_VERTEX t[3];
	for ( int v = 0; v < 3; ++v ) { t[v].z = 0; t[v].rhw = .5f; t[v].argb = color; t[v].frgb = 0; t[v].u = t[v].v = 0; }
	t[0].x = tx;                          t[0].y = ty;                          // tip
	t[1].x = tx - ux * sz + px * sz*0.6f; t[1].y = ty - uy * sz + py * sz*0.6f; // back-left
	t[2].x = tx - ux * sz - px * sz*0.6f; t[2].y = ty - uy * sz - py * sz*0.6f; // back-right
	gos_DrawTriangles( t, 3 );
}

void MechWarrior::drawOverviewMovePath(unsigned long color)
{
    for (const auto& row : mc2::render_view::hud().movers)
        if (row.pilot == this) { mc2::render_view::drawHudOverviewPath(row, color); break; }
}

void mc2::render_view::drawHudOverviewPath(const HudMover& row, unsigned long color)
{
    const int kMaxPts = 64;
    const auto& pts = row.overviewPath;
    const int np = static_cast<int>(pts.size());
    if (np < 2) return;

	float sx[kMaxPts], sy[kMaxPts]; bool ok[kMaxPts];
	float vmx, vmy, vax, vay;
	gos_GetViewport( &vmx, &vmy, &vax, &vay );
	for ( int i = 0; i < np; i++ )
	{
		ModernClipResult r = eye->projectModernClipGL( pts[i] );
		ok[i] = ( r.admit && r.clip.w > 0.05f );
		if ( ok[i] )
		{
			sx[i] = vax + ( r.clip.x / r.clip.w * 0.5f + 0.5f ) * vmx;
			sy[i] = vay + ( 1.0f - ( r.clip.y / r.clip.w * 0.5f + 0.5f ) ) * vmy;
		}
	}

	gos_SetRenderState( gos_State_Texture, 0 );
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_AlphaTest, 0 );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite, 0 );
	bool prevExempt = gos_GetHudScaleExempt();
	gos_SetHudScaleExempt( true );

	const float hw = 5.0f;		// 10px thick
	const float arrowSz = 18.0f;
	int lastA = -1, lastB = -1;
	for ( int i = 0; i + 1 < np; i++ )
		if ( ok[i] && ok[i+1] ) { lastA = i; lastB = i + 1; }

	for ( int i = 0; i + 1 < np; i++ )
	{
		if ( !ok[i] || !ok[i+1] ) continue;
		float ex = sx[i+1], ey = sy[i+1];
		if ( i == lastA )	// shorten the final segment so the arrow caps it cleanly
		{
			float dx = sx[i+1] - sx[i], dy = sy[i+1] - sy[i];
			float len = sqrtf( dx * dx + dy * dy );
			if ( len > arrowSz ) { ex = sx[i] + dx / len * ( len - arrowSz ); ey = sy[i] + dy / len * ( len - arrowSz ); }
		}
		drawThickSeg( sx[i], sy[i], ex, ey, hw, color );
	}
	if ( lastB >= 0 )
		drawArrowHead( sx[lastB], sy[lastB], sx[lastB] - sx[lastA], sy[lastB] - sy[lastA], arrowSz, color );

	gos_SetHudScaleExempt( prevExempt );
}

void drawOverviewArrowWorld( const glm::vec3& from, const glm::vec3& to, unsigned long color )
{
	float vmx, vmy, vax, vay;
	gos_GetViewport( &vmx, &vmy, &vax, &vay );
	ModernClipResult r0 = eye->projectModernClipGL( from );
	ModernClipResult r1 = eye->projectModernClipGL( to );
	if ( !r0.admit || r0.clip.w <= 0.05f || !r1.admit || r1.clip.w <= 0.05f ) return;
	float x0 = vax + ( r0.clip.x / r0.clip.w * 0.5f + 0.5f ) * vmx;
	float y0 = vay + ( 1.0f - ( r0.clip.y / r0.clip.w * 0.5f + 0.5f ) ) * vmy;
	float x1 = vax + ( r1.clip.x / r1.clip.w * 0.5f + 0.5f ) * vmx;
	float y1 = vay + ( 1.0f - ( r1.clip.y / r1.clip.w * 0.5f + 0.5f ) ) * vmy;

	gos_SetRenderState( gos_State_Texture, 0 );
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_AlphaTest, 0 );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite, 0 );
	bool pe = gos_GetHudScaleExempt();
	gos_SetHudScaleExempt( true );

	float dx = x1 - x0, dy = y1 - y0;
	float len = sqrtf( dx * dx + dy * dy );
	const float arrowSz = 24.0f;
	float ex = x1, ey = y1;
	if ( len > arrowSz ) { ex = x0 + dx / len * ( len - arrowSz ); ey = y0 + dy / len * ( len - arrowSz ); }
	drawThickSeg( x0, y0, ex, ey, 7.0f, color );
	drawArrowHead( x1, y1, dx, dy, arrowSz, color );

	gos_SetHudScaleExempt( pe );
}

bool MechWarrior::getMoveDestination( glm::vec3& out )
{
	if ( !tacOrderQueue )
		return false;
	if ( numTacOrdersQueued > 0 )
	{
		out = tacOrderQueue[numTacOrdersQueued - 1].point;
		return true;
	}
	glm::vec3 first;
	first.x = getCurTacOrder()->moveParams.wayPath.points[0];
	first.y = getCurTacOrder()->moveParams.wayPath.points[1];
	if ( first.x != 0.0f && first.y != 0.0f )
	{
		first.z = TerrainRuntime::groundElevation( first );
		out = first;
		return true;
	}
	return false;
}

void MechWarrior::updateDrawWaypointPath()
{
	if ( tacOrderQueue )
	{
		for ( int i = 0; i < numTacOrdersQueued; i++ )
		{
			// B7o.4: a move mode outside slow/fast/jump owns no marker (the old code dereferenced it unchecked).
			if ( queueMarkers[i] )
			{
				BldgAppearance* waypoint = queueMarkers[i].get();
				waypoint->setObjectParameters(tacOrderQueue[i].point,(static_cast<ObjectAppearance*>(waypoint))->rotation,false,0,0);
				waypoint->recalcBounds();
				waypoint->update();
			}

			float zPos = TerrainRuntime::groundElevation(tacOrderQueue[i].point);
			tacOrderQueue[i].point.z = zPos;
		}
	}
}

bool MechWarrior::isCloseToFirstTacOrder( glm::vec3& pos )
{
	if ( tacOrderQueue )
	{
		glm::vec3 tmp;
		tmp = tacOrderQueue[0].point - pos;
		if ( glm::length(tmp) < 5 ) // random amount
			return true;
	}

	return false;
}
