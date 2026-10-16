/*************************************************************************************************\
HeightBrush.cpp : see HeightBrush.h
\*************************************************************************************************/

#include "HeightBrush.h"

#ifndef CAMERA_H
#include "Camera.h"
#endif

#include "../GameOS/gameos/gos_terrain_indirect.h"
#include "../GameOS/gameos/gos_terrain_water_stream.h"
#include "mapdata.h"

#include <math.h>

//-------------------------------------------------------------------------------------------------
// Modern GL forward projection (projectModernClipGL + viewport remap) -- the same
// zoom-correct transform object selection uses. Legacy projectZ diverges/rejects
// when zoomed in (X-collapse). Returns false (skip primitive) behind the near plane.
static bool projectPtGL_brush( Camera* eye, const Stuff::Vector3D& wp, float& sx, float& sy )
{
	ModernClipResult r = eye->projectModernClipGL( wp );
	if ( r.clip.w <= 1e-4f ) return false;
	float vmx = 0.f, vmy = 0.f, vax = 0.f, vay = 0.f;
	gos_GetViewport( &vmx, &vmy, &vax, &vay );
	const float ndcX = r.clip.x / r.clip.w;
	const float ndcY = r.clip.y / r.clip.w;
	sx = vax + ( ndcX * 0.5f + 0.5f ) * vmx;
	sy = vay + ( 1.0f - ( ndcY * 0.5f + 0.5f ) ) * vmy;
	if ( !( sx == sx ) || !( sy == sy ) ) return false;
	return true;
}

//-------------------------------------------------------------------------------------------------
HeightBrush::HeightBrush( Mode m, float r, float s )
	: mode( m ), radius( r ), strength( s ), pCurAction( nullptr )
{
}

HeightBrush::~HeightBrush()
{
}

//-------------------------------------------------------------------------------------------------
bool HeightBrush::beginPaint()
{
	pCurAction = new ActionPaintTile();
	touchedThisStroke.clear();
	return true;
}

Action* HeightBrush::endPaint()
{
	ActionPaintTile* pRet = pCurAction;
	if ( pCurAction && pCurAction->vertexInfoList.empty() )
	{
		delete pCurAction;
		pRet = nullptr;
	}
	if ( land )
	{
		land->recalcWater();

		// Water surfaces must reflect the sculpted terrain heights.
		WaterStream::Reset();
		WaterStream::Build();

		// Sculpted terrain needs fresh normals and lighting in its render recipes.
		if ( Terrain::mapData )
			Terrain::mapData->calcLight();

		const int side = land->realVerticesMapSide;
		if ( side > 0 )
		{
			for ( std::set<int>::const_iterator it = touchedThisStroke.begin();
			      it != touchedThisStroke.end(); ++it )
			{
				const int vn = *it;
				invalidateQuadsAround( vn / side, vn % side );
			}
		}
	}
	pCurAction = nullptr;
	touchedThisStroke.clear();
	return pRet;
}

//-------------------------------------------------------------------------------------------------
bool HeightBrush::paint( Stuff::Vector3D& worldPos, int screenX, int screenY )
{
	if ( !land || !pCurAction )
		return false;

	const float wupv = land->worldUnitsPerVertex;
	const int   side = land->realVerticesMapSide;
	if ( wupv <= 0.0f || side <= 0 )
		return false;

	// Vertex nearest the cursor, and the vertex-radius covering the brush radius.
	const int cx = (int)floor( ( worldPos.x - land->mapTopLeft3d.x ) / wupv + 0.5f );
	const int cy = (int)floor( ( land->mapTopLeft3d.y - worldPos.y ) / wupv + 0.5f );
	const int vr = (int)ceil( radius / wupv ) + 1;

	for ( int j = cy - vr; j <= cy + vr; ++j )
	{
		if ( j < 0 || j >= side ) continue;
		for ( int i = cx - vr; i <= cx + vr; ++i )
		{
			if ( i < 0 || i >= side ) continue;

			// World position of this vertex (mirror FlattenBrush index math).
			const float vwx = land->mapTopLeft3d.x + (float)i * wupv;
			const float vwy = land->mapTopLeft3d.y - (float)j * wupv;
			const float dx  = vwx - worldPos.x;
			const float dy  = vwy - worldPos.y;
			const float dist = sqrtf( dx * dx + dy * dy );
			if ( dist > radius ) continue;

			const int key = j * side + i;
			if ( touchedThisStroke.count( key ) )   // one step per stroke -> passes layer
				continue;
			touchedThisStroke.insert( key );

			applyToVertex( j, i, dist );
		}
	}

	return true;
}

//-------------------------------------------------------------------------------------------------
void HeightBrush::applyToVertex( int row, int col, float worldDist )
{
	const int side = land->realVerticesMapSide;
	const int vn   = row * side + col;

	// Smoothstep falloff: full at the centre, 0 at the radius edge.
	float t = ( radius > 0.0f ) ? ( 1.0f - worldDist / radius ) : 1.0f;
	if ( t < 0.0f ) t = 0.0f;
	if ( t > 1.0f ) t = 1.0f;
	const float falloff = t * t * ( 3.0f - 2.0f * t );

	pCurAction->addChangedVertexInfo( row, col );   // undo

	const float cur = land->getTerrainElevation( row, col );

	if ( mode == SMOOTH )
	{
		float sum = 0.0f; int n = 0;
		const int nb[4][2] = { { row - 1, col }, { row + 1, col }, { row, col - 1 }, { row, col + 1 } };
		for ( int k = 0; k < 4; ++k )
		{
			const int r = nb[k][0], c = nb[k][1];
			if ( r >= 0 && r < side && c >= 0 && c < side )
			{
				sum += land->getTerrainElevation( r, c );
				++n;
			}
		}
		if ( n > 0 )
		{
			const float avg = sum / (float)n;
			land->setVertexHeight( vn, cur + ( avg - cur ) * falloff );
		}
	}
	else
	{
		const float dir = ( mode == RAISE ) ? 1.0f : -1.0f;
		land->setVertexHeight( vn, cur + strength * falloff * dir );
	}

	invalidateQuadsAround( row, col );
}

//-------------------------------------------------------------------------------------------------
// A moved vertex belongs to up to four terrain quads; invalidate each quad's GPU
// recipe (top-left vertexNum) so the GPU-direct terrain re-bakes the new elevation.
// Height edits do NOT null the Shape-C face cache (only setTerrain/setOverlay do),
// so the recipe re-bake gets a valid texture nodeId and the quad does not go black.
void HeightBrush::invalidateQuadsAround( int row, int col )
{
	const int side = land->realVerticesMapSide;
	const int q[4][2] = { { row, col }, { row, col - 1 }, { row - 1, col }, { row - 1, col - 1 } };
	for ( int k = 0; k < 4; ++k )
	{
		const int r = q[k][0], c = q[k][1];
		if ( r >= 0 && r < side - 1 && c >= 0 && c < side - 1 )
			gos_terrain_indirect::InvalidateRecipeForVertexNum( r * side + c );
	}
}

//-------------------------------------------------------------------------------------------------
// Draw the brush footprint as a magenta ring on the terrain around the cursor.
void HeightBrush::render( int screenX, int screenY )
{
	if ( !eye || !land )
		return;

	Stuff::Vector3D center;
	Stuff::Vector2DOf<long> sp;
	sp.x = screenX; sp.y = screenY;
	eye->inverseProject( sp, center );
	center.z = land->getTerrainElevation( center );

	float centerSx = 0.f, centerSy = 0.f;
	const bool centerOk = projectPtGL_brush( eye, center, centerSx, centerSy );

	// Project the rim of the brush footprint onto the terrain.
	const int N = 48;
	float rimX[N + 1], rimY[N + 1];
	bool  rimOk[N + 1];
	for ( int k = 0; k <= N; ++k )
	{
		const float ang = (float)k / (float)N * 6.2831853f;
		Stuff::Vector3D wp = center;
		wp.x += cosf( ang ) * radius;
		wp.y += sinf( ang ) * radius;
		wp.z = land->getTerrainElevation( wp );
		rimOk[k] = projectPtGL_brush( eye, wp, rimX[k], rimY[k] );
	}

	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	// Always-visible cursor overlay: disable depth test/write so the footprint
	// draws on top regardless of the depth state the (reverse-Z chunk) terrain
	// pass left enabled. These rhw=1 verts have z=0, so with ZCompare on they fail
	// the terrain depth test and vanish. Mirrors FoliageRender; restored below.
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite,   0 );

	// Filled translucent magenta disc (triangle fan from the centre to the rim).
	if ( centerOk )
	for ( int k = 0; k < N; ++k )
	{
		if ( !rimOk[k] || !rimOk[k+1] )
			continue;  // skip tris touching a near-plane-rejected rim point
		gos_VERTEX t[3];
		memset( t, 0, sizeof( t ) );
		t[0].x = centerSx;  t[0].y = centerSy;  t[0].rhw = 1.0f; t[0].argb = 0x40ff00ff;
		t[1].x = rimX[k];   t[1].y = rimY[k];   t[1].rhw = 1.0f; t[1].argb = 0x40ff00ff;
		t[2].x = rimX[k+1]; t[2].y = rimY[k+1]; t[2].rhw = 1.0f; t[2].argb = 0x40ff00ff;
		gos_DrawTriangles( t, 3 );
	}

	// Bright magenta outline ring on top.
	for ( int k = 0; k < N; ++k )
	{
		if ( !rimOk[k] || !rimOk[k+1] )
			continue;  // skip segments touching a near-plane-rejected rim point
		gos_VERTEX v[2];
		memset( v, 0, sizeof( v ) );
		v[0].x = rimX[k];   v[0].y = rimY[k];   v[0].rhw = 1.0f; v[0].argb = 0xffff00ff;
		v[1].x = rimX[k+1]; v[1].y = rimY[k+1]; v[1].rhw = 1.0f; v[1].argb = 0xffff00ff;
		gos_DrawLines( v, 2 );
	}

	gos_SetRenderState( gos_State_ZCompare, 1 );
	gos_SetRenderState( gos_State_ZWrite,   1 );
}

//*************************************************************************************************
// end of file ( HeightBrush.cpp )
