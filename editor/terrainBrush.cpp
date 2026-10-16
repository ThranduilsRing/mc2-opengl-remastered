#define TERRAINBRUSH_CPP
/*************************************************************************************************\
terrainBrush.cpp			: Implementation of the terrainBrush component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "terrainBrush.h"
#include "EditorData.h"

#ifndef CAMERA_H
#include "Camera.h"
#endif
#ifndef TERRAIN_H
#include "Terrain.h"
#endif

#include "mapdata.h"
#include <math.h>

extern CameraPtr eye;
extern Terrain*  land;

int   TerrainBrush::s_lastType   = 0;
float TerrainBrush::s_lastRadius = 0.0f;   // 0 => single-vertex (legacy) until a size is set

//-------------------------------------------------------------------------------------------------
// Modern GL forward projection (projectModernClipGL + viewport remap) -- the same
// zoom-correct transform the sculpt cursor uses. Legacy projectZ diverges when
// zoomed in. Returns false (skip primitive) behind the near plane.
static bool projectPtGL_terrainbrush( Camera* eye, const Stuff::Vector3D& wp, float& sx, float& sy )
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
bool TerrainBrush::beginPaint()
{
	if ( pAction )
	{
		gosASSERT( false );
	}
	pAction = new ActionPaintTile;
	gosASSERT( pAction );
	touchedThisStroke.clear();
	return true;
}

//-------------------------------------------------------------------------------------------------
Action* TerrainBrush::endPaint()
{
	Action* pRetAction = pAction;
	pAction = nullptr;
	touchedThisStroke.clear();

	// Rebuild the legacy/indirect face cache + decal VBO for the painted cells
	// (so they don't go black on the legacy path), AND re-upload the per-vertex
	// type SSBO consumed by the live LOD-chunk frag so painted material appears
	// immediately instead of only after a mission reload.
	EditorData::refreshTerrainAfterEdit();
	if ( land )
		land->refreshTerrainTypeSSBO();

	return pRetAction;
}

//-------------------------------------------------------------------------------------------------
// Paint EVERY vertex within the world-space brush radius (mirrors HeightBrush's
// disk loop). radius<=0 falls back to the single closest vertex (legacy behaviour).
bool TerrainBrush::paint( Stuff::Vector3D& worldPos, int screenX, int screenY )
{
	if ( !pAction )
		return false;

	// Legacy single-vertex path when no radius has been set.
	if ( radius <= 0.0f || !land )
	{
		long tileC, tileR;
		Stuff::Vector2DOf<long> screenPos( screenX, screenY );
		eye->getClosestVertex( screenPos, tileR, tileC );

		if ( tileR < Terrain::realVerticesMapSide && tileR > -1
			&& tileC < Terrain::realVerticesMapSide && tileC > -1 )
		{
			pAction->addChangedVertexInfo( tileR, tileC );	// for undo
			land->setTerrain( tileR, tileC, terrainType );
			return true;
		}
		return false;
	}

	const float wupv = land->worldUnitsPerVertex;
	const int   side = land->realVerticesMapSide;
	if ( wupv <= 0.0f || side <= 0 )
		return false;

	// Vertex nearest the cursor, and the vertex-radius covering the brush radius.
	const int cx = (int)floor( ( worldPos.x - land->mapTopLeft3d.x ) / wupv + 0.5f );
	const int cy = (int)floor( ( land->mapTopLeft3d.y - worldPos.y ) / wupv + 0.5f );
	const int vr = (int)ceil( radius / wupv ) + 1;

	bool any = false;
	for ( int j = cy - vr; j <= cy + vr; ++j )
	{
		if ( j < 0 || j >= side ) continue;
		for ( int i = cx - vr; i <= cx + vr; ++i )
		{
			if ( i < 0 || i >= side ) continue;

			// World position of this vertex (mirror HeightBrush index math).
			const float vwx = land->mapTopLeft3d.x + (float)i * wupv;
			const float vwy = land->mapTopLeft3d.y - (float)j * wupv;
			const float dx  = vwx - worldPos.x;
			const float dy  = vwy - worldPos.y;
			if ( sqrtf( dx * dx + dy * dy ) > radius ) continue;

			const int key = j * side + i;
			if ( touchedThisStroke.count( key ) )   // one apply per vertex per stroke
				continue;
			touchedThisStroke.insert( key );

			pAction->addChangedVertexInfo( j, i );   // undo
			land->setTerrain( j, i, terrainType );
			any = true;
		}
	}

	return any;
}

//-------------------------------------------------------------------------------------------------
// Draw the brush footprint as a magenta ring on the terrain (mirrors HeightBrush::render).
void TerrainBrush::render( int screenX, int screenY )
{
	if ( !eye || !land || radius <= 0.0f )
		return;

	Stuff::Vector3D center;
	Stuff::Vector2DOf<long> sp;
	sp.x = screenX; sp.y = screenY;
	eye->inverseProject( sp, center );
	center.z = land->getTerrainElevation( center );

	float centerSx = 0.f, centerSy = 0.f;
	const bool centerOk = projectPtGL_terrainbrush( eye, center, centerSx, centerSy );

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
		rimOk[k] = projectPtGL_terrainbrush( eye, wp, rimX[k], rimY[k] );
	}

	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite,   0 );

	if ( centerOk )
	for ( int k = 0; k < N; ++k )
	{
		if ( !rimOk[k] || !rimOk[k+1] )
			continue;
		gos_VERTEX t[3];
		memset( t, 0, sizeof( t ) );
		t[0].x = centerSx;  t[0].y = centerSy;  t[0].rhw = 1.0f; t[0].argb = 0x40ff00ff;
		t[1].x = rimX[k];   t[1].y = rimY[k];   t[1].rhw = 1.0f; t[1].argb = 0x40ff00ff;
		t[2].x = rimX[k+1]; t[2].y = rimY[k+1]; t[2].rhw = 1.0f; t[2].argb = 0x40ff00ff;
		gos_DrawTriangles( t, 3 );
	}

	for ( int k = 0; k < N; ++k )
	{
		if ( !rimOk[k] || !rimOk[k+1] )
			continue;
		gos_VERTEX v[2];
		memset( v, 0, sizeof( v ) );
		v[0].x = rimX[k];   v[0].y = rimY[k];   v[0].rhw = 1.0f; v[0].argb = 0xffff00ff;
		v[1].x = rimX[k+1]; v[1].y = rimY[k+1]; v[1].rhw = 1.0f; v[1].argb = 0xffff00ff;
		gos_DrawLines( v, 2 );
	}

	gos_SetRenderState( gos_State_ZCompare, 1 );
	gos_SetRenderState( gos_State_ZWrite,   1 );
}

//-------------------------------------------------------------------------------------------------
Action* TerrainBrush::applyToSelection()
{
	ActionPaintTile* pRetAction = new ActionPaintTile();
	for ( int i = 0; i < land->realVerticesMapSide; ++i )
	{
		for ( int j = 0; j < land->realVerticesMapSide; ++j )
		{
			if ( land->isVertexSelected( j, i ) )
			{
				pRetAction->addChangedVertexInfo( j, i );
				land->setTerrain( j, i, terrainType );
			}
		}
	}

	EditorData::refreshTerrainAfterEdit();   // rebuild face cache so edited cells aren't black
	if ( land )
		land->refreshTerrainTypeSSBO();
	return pRetAction;
}
//*************************************************************************************************
// end of file ( terrainBrush.cpp )
