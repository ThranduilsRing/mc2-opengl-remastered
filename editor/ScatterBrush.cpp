/*************************************************************************************************\
ScatterBrush.cpp : see ScatterBrush.h
\*************************************************************************************************/

#include "stdafx.h"
#include "ScatterBrush.h"

#ifndef BUILDINGBRUSH_H
#include "BuildingBrush.h"      // reuse BuildingBrush::BuildingAction for undo
#endif
#ifndef EDITOROBJECTMGR_H
#include "EditorObjectMgr.h"
#endif
#ifndef TERRAIN_H
#include "Terrain.h"
#endif
#ifndef CAMERA_H
#include "Camera.h"
#endif

#include <math.h>

extern CameraPtr eye;
extern Terrain*  land;

//-------------------------------------------------------------------------------------------------
// Modern GL forward projection (projectModernClipGL + viewport remap). Legacy
// projectZ diverges/rejects when zoomed in (X-collapse). False = behind near plane.
static bool projectPtGL_brush( Camera* eye, const glm::vec3& wp, float& sx, float& sy )
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

// Scatter control defaults.
float ScatterBrush::s_radius        = 400.0f;
int   ScatterBrush::s_density       = 4;
float ScatterBrush::s_minSpacing    = 96.0f;
float ScatterBrush::s_maxSlopeRise  = 0.0f;    // 0 = no slope filter
bool  ScatterBrush::s_randomRotation = true;
float ScatterBrush::s_scaleJitter   = 0.15f;

// Small deterministic RNG (LCG) so a given seed+click is repeatable.
static inline unsigned lcgNext( unsigned& s ) { s = s * 1664525u + 1013904223u; return s; }
static inline float    frand01( unsigned& s ) { return (float)( lcgNext( s ) >> 8 ) / (float)( 1u << 24 ); }

ScatterBrush::ScatterBrush( int group, int indexInGroup, int alignment )
	: m_group( group ), m_indexInGroup( indexInGroup ), m_alignment( alignment ),
	  m_pAction( nullptr ), m_rng( 0x12345u ), m_haveLast( false ), m_lastX( 0.0f ), m_lastY( 0.0f )
{
}

ScatterBrush::~ScatterBrush()
{
}

bool ScatterBrush::beginPaint()
{
	m_pAction = new BuildingBrush::BuildingAction;
	m_haveLast = false;
	return true;
}

Action* ScatterBrush::endPaint()
{
	BuildingBrush::BuildingAction* pAct = (BuildingBrush::BuildingAction*)m_pAction;
	if ( pAct && pAct->objInfoPtrList.empty() )
	{
		delete pAct;
		pAct = nullptr;
	}
	m_pAction = nullptr;
	return pAct;
}

bool ScatterBrush::paint( glm::vec3& worldPos, int /*screenX*/, int /*screenY*/ )
{
	if ( !land )
		return false;

	// Throttle: paint() fires every frame while the button is held, so without
	// this a single click (or a slow drag) dumps the full density many times over.
	// Only scatter again once the cursor has moved ~half a radius, giving an even
	// drag-painted spread and roughly one density-batch per click.
	if ( m_haveLast )
	{
		const float mvx = worldPos.x - m_lastX;
		const float mvy = worldPos.y - m_lastY;
		const float moveThresh = s_radius * 0.5f;
		if ( mvx * mvx + mvy * mvy < moveThresh * moveThresh )
			return false;
	}
	m_lastX = worldPos.x;
	m_lastY = worldPos.y;
	m_haveLast = true;

	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	BuildingBrush::BuildingAction* pAct = (BuildingBrush::BuildingAction*)m_pAction;

	const float wupv = land->worldUnitsPerVertex;

	// Seed the per-stroke RNG from the click location so repeated paints in the
	// same spot reproduce, but different spots differ.
	m_rng ^= (unsigned)( worldPos.x * 7.0f ) * 2654435761u
	       ^ (unsigned)( worldPos.y * 13.0f ) * 40503u;

	int placed = 0;
	for ( int i = 0; i < s_density; ++i )
	{
		// Uniform random point in the brush disc (sqrt for area-uniform).
		const float ang = frand01( m_rng ) * 6.2831853f;
		const float rr  = sqrtf( frand01( m_rng ) ) * s_radius;
		glm::vec3 p = worldPos;
		p.x += cosf( ang ) * rr;
		p.y += sinf( ang ) * rr;
		p.z = land->getTerrainElevation( p );

		// Slope filter: reject if the terrain rises more than s_maxSlopeRise over
		// one vertex spacing in either axis.
		if ( s_maxSlopeRise > 0.0f && wupv > 0.0f )
		{
			glm::vec3 px = p, py = p;
			px.x += wupv; py.y += wupv;
			const float dzx = fabsf( land->getTerrainElevation( px ) - p.z );
			const float dzy = fabsf( land->getTerrainElevation( py ) - p.z );
			if ( dzx > s_maxSlopeRise || dzy > s_maxSlopeRise )
				continue;
		}

		// Spacing: skip if something is already very close.
		if ( s_minSpacing > 0.0f )
		{
			EditorObject* nearObj = mgr->getObjectAtLocation( p.x, p.y );
			if ( nearObj && nearObj->appearance() )
			{
				const float ddx = nearObj->getPosition().x - p.x;
				const float ddy = nearObj->getPosition().y - p.y;
				if ( ddx * ddx + ddy * ddy < s_minSpacing * s_minSpacing )
					continue;
			}
		}

		const float rot   = s_randomRotation ? frand01( m_rng ) * 360.0f : 0.0f;
		const float scale = 1.0f + ( frand01( m_rng ) * 2.0f - 1.0f ) * s_scaleJitter;

		// Engine refuses overlapping props; canAddBuilding gates that.
		if ( !mgr->canAddBuilding( p, rot, m_group, m_indexInGroup ) )
			continue;

		EditorObject* o = mgr->addBuilding( p, m_group, m_indexInGroup, m_alignment, rot, scale, false /*bSnapToCell*/ );
		if ( o )
		{
			if ( pAct )
				pAct->addBuildingInfo( *o );
			++placed;
		}
	}

	return placed > 0;
}

void ScatterBrush::render( int screenX, int screenY )
{
	if ( !eye || !land )
		return;

	glm::vec3 center;
	Stuff::Vector2DOf<long> sp;
	sp.x = screenX; sp.y = screenY;
	eye->inverseProject( sp, center );
	center.z = land->getTerrainElevation( center );

	float centerSx = 0.f, centerSy = 0.f;
	const bool centerOk = projectPtGL_brush( eye, center, centerSx, centerSy );

	const int N = 48;
	float rimX[N + 1], rimY[N + 1];
	bool  rimOk[N + 1];
	for ( int k = 0; k <= N; ++k )
	{
		const float ang = (float)k / (float)N * 6.2831853f;
		glm::vec3 wp = center;
		wp.x += cosf( ang ) * s_radius;
		wp.y += sinf( ang ) * s_radius;
		wp.z = land->getTerrainElevation( wp );
		rimOk[k] = projectPtGL_brush( eye, wp, rimX[k], rimY[k] );
	}

	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	// Always-visible cursor overlay: disable depth test/write (rhw=1 z=0 verts
	// otherwise fail the reverse-Z chunk-terrain depth test and vanish). Mirrors
	// FoliageRender; restored below.
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite,   0 );

	// Translucent green disc (scatter footprint).
	if ( centerOk )
	for ( int k = 0; k < N; ++k )
	{
		if ( !rimOk[k] || !rimOk[k+1] )
			continue;  // skip tris touching a near-plane-rejected rim point
		gos_VERTEX t[3];
		memset( t, 0, sizeof( t ) );
		t[0].x = centerSx;  t[0].y = centerSy;  t[0].rhw = 1.0f; t[0].argb = 0x3000ff40;
		t[1].x = rimX[k];   t[1].y = rimY[k];   t[1].rhw = 1.0f; t[1].argb = 0x3000ff40;
		t[2].x = rimX[k+1]; t[2].y = rimY[k+1]; t[2].rhw = 1.0f; t[2].argb = 0x3000ff40;
		gos_DrawTriangles( t, 3 );
	}
	for ( int k = 0; k < N; ++k )
	{
		if ( !rimOk[k] || !rimOk[k+1] )
			continue;  // skip segments touching a near-plane-rejected rim point
		gos_VERTEX v[2];
		memset( v, 0, sizeof( v ) );
		v[0].x = rimX[k];   v[0].y = rimY[k];   v[0].rhw = 1.0f; v[0].argb = 0xff00ff40;
		v[1].x = rimX[k+1]; v[1].y = rimY[k+1]; v[1].rhw = 1.0f; v[1].argb = 0xff00ff40;
		gos_DrawLines( v, 2 );
	}

	gos_SetRenderState( gos_State_ZCompare, 1 );
	gos_SetRenderState( gos_State_ZWrite,   1 );
}
