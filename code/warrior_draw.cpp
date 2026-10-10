// MechWarrior: waypoint and overview move-path drawing.
// Declared in the class headers; shared names in warrior_internal.h.
#include "warrior_internal.h"


void MechWarrior::drawWaypointPath()
{
	if ( tacOrderQueue )
	{
		Stuff::Vector3D tmpPos = this->getVehicle()->getPosition();
		
		{
			Stuff::Vector4D screenPos1;
			Stuff::Vector4D screenPos2;

			Stuff::Vector3D pos;
			pos.x = getCurTacOrder()->moveParams.wayPath.points[0];
			pos.y = getCurTacOrder()->moveParams.wayPath.points[1];
			pos.z = getCurTacOrder()->moveParams.wayPath.points[2];

			if ( tacOrderQueueLooping )
			{
				tmpPos = tacOrderQueue[numTacOrdersQueued - 1].point;
			}

			else if ( pos.x != 0 && pos.y != 0  )
			{

				pos.z = TerrainRuntime::groundElevation( pos );
				// [PROJECTZ:ScreenXYOracle id=warrior_tac_order_preview_start]
				eye->projectForScreenXY( tmpPos, screenPos1 );
				// [PROJECTZ:ScreenXYOracle id=warrior_tac_order_preview_end]
				eye->projectForScreenXY( pos, screenPos2 );

				Stuff::Vector4D vertices[2];
				vertices[0].x = screenPos1.x;
				vertices[0].y = screenPos1.y;
				vertices[1].x = screenPos2.x;
				vertices[1].y = screenPos2.y;

				vertices[0].z = vertices[1].z = 0.1f;
				vertices[0].w = vertices[1].w = 0.9999f;

				LineElement line( vertices[0], vertices[1], 0xffff0000, 0, -1 );
				line.draw();
				
				tmpPos = pos;
			}
		}

		for ( int i = 0; i < numTacOrdersQueued; i++ )
		{
			tacOrderQueue[i].marker->render();
			
			Stuff::Vector4D screenPos1;
			Stuff::Vector4D screenPos2;

			// [PROJECTZ:ScreenXYOracle id=warrior_tac_queue_segment_start]
			eye->projectForScreenXY( tacOrderQueue[i].point, screenPos1 );
			// [PROJECTZ:ScreenXYOracle id=warrior_tac_queue_segment_end]
			eye->projectForScreenXY( tmpPos, screenPos2 );

   			Stuff::Vector4D vertices[2];
   			vertices[0].x = screenPos1.x;
   			vertices[0].y = screenPos1.y;
   			vertices[1].x = screenPos2.x;
   			vertices[1].y = screenPos2.y;

   			vertices[0].z = vertices[1].z = 0.1f;
   			vertices[0].w = vertices[1].w = 0.9999f;

   			LineElement line( vertices[0], vertices[1], 0xffff0000, 0, -1 );
   			line.draw();

			tmpPos = tacOrderQueue[i].point;
		}

	}

}

// Thick line segment (a quad) from (x0,y0) to (x1,y1), half-width hw, in color.
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

// Filled arrowhead at tip (tx,ty) pointing along (dx,dy), size sz, in color.
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

// Tactical Overview: draw this mech's queued move path as a thick arrowed line
// in the given squad color (GL-consistent projection, HUD-scale exempt).
void MechWarrior::drawOverviewMovePath( unsigned long color )
{
	if ( !tacOrderQueue || !getVehicle() )
		return;

	// Straight line through the move waypoints: start at the mech, the current
	// move target, then any queued waypoints. (Full A* route tracing was tried
	// but the path data isn't reliable here; revisit after the pathfinding work.)
	const int kMaxPts = 64;
	Stuff::Vector3D pts[kMaxPts];
	int np = 0;
	pts[np++] = getVehicle()->getPosition();

	Stuff::Vector3D first;
	first.x = getCurTacOrder()->moveParams.wayPath.points[0];
	first.y = getCurTacOrder()->moveParams.wayPath.points[1];
	first.z = getCurTacOrder()->moveParams.wayPath.points[2];
	if ( !tacOrderQueueLooping && first.x != 0.0f && first.y != 0.0f )
	{
		first.z = TerrainRuntime::groundElevation( first );
		pts[np++] = first;
	}
	for ( int i = 0; i < numTacOrdersQueued && np < kMaxPts; i++ )
		pts[np++] = tacOrderQueue[i].point;
	if ( np < 2 )
		return;

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

// Free function: draw a single thick arrow between two WORLD points (used for
// the squad-level overview arrow). GL-consistent projection, HUD-scale exempt.
void drawOverviewArrowWorld( const Stuff::Vector3D& from, const Stuff::Vector3D& to, unsigned long color )
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

bool MechWarrior::getMoveDestination( Stuff::Vector3D& out )
{
	if ( !tacOrderQueue )
		return false;
	if ( numTacOrdersQueued > 0 )
	{
		out = tacOrderQueue[numTacOrdersQueued - 1].point;
		return true;
	}
	Stuff::Vector3D first;
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
			tacOrderQueue[i].marker->setObjectParameters(tacOrderQueue[i].point,((ObjectAppearance*)tacOrderQueue[i].marker)->rotation,false,0,0);
			tacOrderQueue[i].marker->recalcBounds();
	
			tacOrderQueue[i].marker->update();
			
			float zPos = TerrainRuntime::groundElevation(tacOrderQueue[i].point);
			tacOrderQueue[i].point.z = zPos;
		}
	}
}

bool MechWarrior::isCloseToFirstTacOrder( Stuff::Vector3D& pos )
{
	if ( tacOrderQueue )
	{
		Stuff::Vector3D tmp;
		tmp.Subtract(tacOrderQueue[0].point, pos );
		if ( tmp.GetLength() < 5 ) // random amount
			return true;
	}

	return false;
}
