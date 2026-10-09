//-------------------------------------------------------------------------------------------------
// FoliageRender.cpp -- see FoliageRender.h.
//
// Billboards are drawn the same way the editor draws its other GL overlays
// (ScatterBrush / renderTerrainSelection): project world points to screen with
// the active camera, then submit screen-space gos_VERTEX quads. We hand-parse
// the small flat foliage JSON rather than pull in nlohmann/json -- that header is
// isolation-locked to a single engine TU (scripts/check-json-isolation.sh).
//
// v1 scope (intentionally minimal, "do not overbuild"):
//   * screen-space ground-anchored billboards, distance-scaled
//   * per-instance frustum cull via projectForScreenXY's admit return
//   * sprite texture per kind if present, else a flat colored quad fallback so
//     the preview is visible without shipping any sprite art
//   * ZCompare left OFF so sprites are always visible in the editor preview
//     (they are anchored on the terrain surface; precise depth occlusion +
//     per-superchunk culling are deferred to Phase 6)
//-------------------------------------------------------------------------------------------------
#include "stdafx.h"
#include "FoliageRender.h"

#include "Camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>

namespace
{
	enum FoliageKind { KIND_TREE = 0, KIND_ROCK = 1, KIND_BUSH = 2, KIND_COUNT = 3 };

	struct Instance
	{
		float x, y, z;     // world coords (worldUnitsPerVertex space)
		float rotation;    // degrees (unused by screen-aligned billboard, kept for later)
		float scale;
		int   kind;
	};

	std::vector<Instance> g_instances;
	bool  g_visible = true;

	// Per-kind upright world height (world units) at scale 1.0, and the flat
	// fallback color (ARGB) used when no sprite texture is available.
	const float kKindHeight[KIND_COUNT] = { 256.0f, 96.0f, 128.0f };
	const DWORD kKindColor [KIND_COUNT] = { 0xff2f7d2f, 0xff8a8a8a, 0xff6f8f3f };

	// Lazily-resolved sprite textures (0 = not loaded yet / missing -> fallback).
	DWORD g_tex[KIND_COUNT]    = { 0, 0, 0 };
	bool  g_texTried[KIND_COUNT] = { false, false, false };
	const char* kKindSprite[KIND_COUNT] = {
		"data\\textures\\foliage\\tree.tga",
		"data\\textures\\foliage\\rock.tga",
		"data\\textures\\foliage\\bush.tga",
	};

	int kindFromString( const std::string& s )
	{
		if ( s.find( "rock" ) != std::string::npos ) return KIND_ROCK;
		if ( s.find( "bush" ) != std::string::npos ) return KIND_BUSH;
		return KIND_TREE; // trees + anything else
	}

	// --- minimal JSON field scanners over a single instance-object substring ----
	bool findNum( const std::string& s, const char* key, double& out )
	{
		std::string k = std::string( "\"" ) + key + "\"";
		size_t p = s.find( k );
		if ( p == std::string::npos ) return false;
		p = s.find( ':', p );
		if ( p == std::string::npos ) return false;
		const char* start = s.c_str() + p + 1;
		char* end = NULL;
		double v = strtod( start, &end );
		if ( end == start ) return false;        // no number consumed -> treat as missing
		if ( !( v == v ) ) return false;          // NaN guard
		out = v;
		return true;
	}

	std::string findStr( const std::string& s, const char* key )
	{
		std::string k = std::string( "\"" ) + key + "\"";
		size_t p = s.find( k );
		if ( p == std::string::npos ) return std::string();
		p = s.find( ':', p );
		if ( p == std::string::npos ) return std::string();
		size_t q = s.find( '"', p );
		if ( q == std::string::npos ) return std::string();
		size_t r = s.find( '"', q + 1 );
		if ( r == std::string::npos ) return std::string();
		return s.substr( q + 1, r - q - 1 );
	}

	DWORD spriteTexFor( int kind )
	{
		if ( kind < 0 || kind >= KIND_COUNT ) return 0;
		if ( !g_texTried[kind] )
		{
			g_texTried[kind] = true;
			// gos_Texture_Detect picks keyed/alpha from the sprite's alpha channel;
			// returns 0 when the file is absent -> we fall back to a colored quad.
			g_tex[kind] = gos_NewTextureFromFile( gos_Texture_Detect, kKindSprite[kind] );
		}
		return g_tex[kind];
	}
}

namespace FoliageRender
{

bool Load( const char* jsonPath )
{
	FILE* f = fopen( jsonPath, "rb" );
	if ( !f )
		return false;
	fseek( f, 0, SEEK_END );
	long n = ftell( f );
	fseek( f, 0, SEEK_SET );
	if ( n <= 0 ) { fclose( f ); return false; }
	std::string buf;
	buf.resize( (size_t)n );
	size_t rd = fread( &buf[0], 1, (size_t)n, f );
	fclose( f );
	buf.resize( rd );

	size_t arr = buf.find( "\"instances\"" );
	if ( arr == std::string::npos )
		return false;
	size_t lb = buf.find( '[', arr );
	if ( lb == std::string::npos )
		return false;

	std::vector<Instance> parsed;
	size_t pos = lb;
	while ( true )
	{
		size_t ob = buf.find( '{', pos );
		if ( ob == std::string::npos ) break;
		size_t oe = buf.find( '}', ob );          // instance objects contain no nested {}
		if ( oe == std::string::npos ) break;
		std::string obj = buf.substr( ob, oe - ob + 1 );
		pos = oe + 1;

		double dx, dy, dz, drot = 0.0, dsc = 1.0;
		if ( findNum( obj, "x", dx ) && findNum( obj, "y", dy ) && findNum( obj, "z", dz ) )
		{
			findNum( obj, "rotation", drot );
			findNum( obj, "scale", dsc );
			Instance in;
			in.x = (float)dx; in.y = (float)dy; in.z = (float)dz;
			in.rotation = (float)drot; in.scale = (float)dsc;
			in.kind = kindFromString( findStr( obj, "kind" ) );
			parsed.push_back( in );
		}

		// Stop at the closing ']' of the instances array.
		size_t nextClose = buf.find( ']', pos );
		size_t nextOpen  = buf.find( '{', pos );
		if ( nextClose != std::string::npos && ( nextOpen == std::string::npos || nextClose < nextOpen ) )
			break;
	}

	g_instances.swap( parsed );

	// Bounds/count log -- immediately reveals swapped axes or absurd coordinates
	// (OutputDebugString so it shows in the editor's debugger/DbgView without UI).
	if ( !g_instances.empty() )
	{
		float minx = g_instances[0].x, maxx = minx;
		float miny = g_instances[0].y, maxy = miny;
		float minz = g_instances[0].z, maxz = minz;
		for ( size_t i = 1; i < g_instances.size(); ++i )
		{
			const Instance& in = g_instances[i];
			if ( in.x < minx ) minx = in.x; if ( in.x > maxx ) maxx = in.x;
			if ( in.y < miny ) miny = in.y; if ( in.y > maxy ) maxy = in.y;
			if ( in.z < minz ) minz = in.z; if ( in.z > maxz ) maxz = in.z;
		}
		char dbg[256];
		sprintf( dbg, "[Foliage] loaded %d instances  x[%.0f..%.0f] y[%.0f..%.0f] z[%.0f..%.0f]\n",
			(int)g_instances.size(), minx, maxx, miny, maxy, minz, maxz );
		OutputDebugStringA( dbg );
	}
	else
	{
		OutputDebugStringA( "[Foliage] loaded 0 instances\n" );
	}
	return true;
}

void Clear()
{
	g_instances.clear();
}

void Toggle()
{
	g_visible = !g_visible;
}

bool Visible()
{
	return g_visible;
}

int Count()
{
	return (int)g_instances.size();
}

void Render( Camera* eye )
{
	if ( !g_visible || g_instances.empty() || !eye )
		return;

	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	// AlphaTest is set PER INSTANCE below (on only when a sprite texture loaded).
	// A missing sprite -> tex==0 -> we draw a flat debug-colored quad; with alpha
	// test ON those untextured fragments get discarded and the fallback is
	// invisible (the "265 generated but nothing appears" case).
	gos_SetRenderState( gos_State_ZCompare, 0 );   // v1: always-visible preview
	gos_SetRenderState( gos_State_ZWrite,   0 );
	// Disable face culling: the terrain/scene pass leaves GL_CULL_FACE enabled, and
	// these billboard quads wind opposite to the front face, so every quad was
	// back-face culled -- submitted but never rasterized, i.e.
	// invisible. Same class as the water cull-winding bug
	// (gameos_graphics.cpp). Overlay draws must not depend on inherited cull
	// state; None makes the quad visible regardless of winding.
	gos_SetRenderState( gos_State_Culling, gos_Cull_None );

	for ( size_t i = 0; i < g_instances.size(); ++i )
	{
		const Instance& in = g_instances[i];

		Stuff::Vector3D base;
		base.x = in.x; base.y = in.y; base.z = in.z;
		Stuff::Vector4D sb;
		// NOTE: projectForScreenXY's bool return is the legacy projectZ "accepted"
		// flag, which per its own contract (camera.h) is DISCARDED by all callers --
		// only screen.xy is consumed. The editor's working overlay draws (brush
		// rings, terrain selection) call projectZ and ignore the bool likewise.
		// Honoring it here culled every on-screen instance ("265 generated, nothing
		// appears"). Project for screen.xy only; cull via the near-plane + NaN +
		// off-screen guards below, which are the real visibility tests.
		eye->projectForScreenXY( base, sb );
		const bool finite  = ( sb.x == sb.x ) && ( sb.y == sb.y ) && ( sb.w == sb.w );
		const bool inFront = ( sb.w > 1e-4f );
		const bool onScreen = finite &&
			!( sb.x < -4096.0f || sb.x > (float)Environment.screenWidth  + 4096.0f ||
			   sb.y < -4096.0f || sb.y > (float)Environment.screenHeight + 4096.0f );

		// Draw only when in front, finite, and on-screen.
		if ( !inFront )                                 // at/behind near plane
			continue;
		if ( !finite )                                 // NaN guard
			continue;
		if ( !onScreen )                               // far off-screen (no screen-filling quad)
			continue;

		Stuff::Vector3D top = base;
		top.z += kKindHeight[in.kind] * ( in.scale > 0.01f ? in.scale : 1.0f );
		Stuff::Vector4D st;
		eye->projectForScreenXY( top, st );

		float h = fabsf( sb.y - st.y );
		if ( !( h == h ) ) h = 2.0f;                   // NaN -> minimum
		if ( h < 2.0f )    h = 2.0f;
		if ( h > 1024.0f ) h = 1024.0f;
		float w = h * 0.7f;

		DWORD tex = spriteTexFor( in.kind );
		gos_SetRenderState( gos_State_Texture, (int)tex );
		// Alpha-test only the textured sprites (cuts the billboard's transparent
		// border); the untextured debug-color fallback must NOT be alpha-tested or
		// it vanishes entirely.
		gos_SetRenderState( gos_State_AlphaTest, tex ? 1 : 0 );
		DWORD argb = tex ? 0xffffffff : kKindColor[in.kind];

		const float cx = sb.x, by = sb.y;
		gos_VERTEX q[4];
		memset( q, 0, sizeof( q ) );
		// Ground-anchored at (cx, by), grows upward (screen y increases downward).
		q[0].x = cx - w * 0.5f; q[0].y = by;       q[0].u = 0.0f; q[0].v = 1.0f;
		q[1].x = cx + w * 0.5f; q[1].y = by;       q[1].u = 1.0f; q[1].v = 1.0f;
		q[2].x = cx + w * 0.5f; q[2].y = by - h;   q[2].u = 1.0f; q[2].v = 0.0f;
		q[3].x = cx - w * 0.5f; q[3].y = by - h;   q[3].u = 0.0f; q[3].v = 0.0f;
		// z = 0, NOT the projected sb.z. These are rhw=1 pre-transformed screen-space
		// verts, so z is the raw clip depth; a nonzero NDC z (e.g. ~0.99 reverse-Z, or
		// out of range) depth-clips the quad -> submitted but never rasterized ("draw>0
		// but invisible"). Every working editor overlay (brush rings,
		// renderTerrainSelection) leaves z=0. ZCompare is off anyway, so depth is
		// irrelevant once the quad is inside clip.
		for ( int k = 0; k < 4; ++k ) { q[k].z = 0.0f; q[k].rhw = 1.0f; q[k].argb = argb; }
		// Emit as TWO triangles via gos_DrawTriangles, NOT gos_DrawQuads. gos_DrawQuads
		// is gated behind the global g_disable_quads, which
		// the GAME enables only inside draw_screen but the editor's own frame loop
		// (EditorGameOS.cpp) never sets false -> every editor gos_DrawQuads silently
		// no-ops. gos_DrawTriangles has no such gate and is the proven editor-visible
		// path (HeightBrush's ring uses it in this same render() pass). Ring order
		// (v0,v1,v2)+(v0,v2,v3) matches drawQuads' own triangulation; Cull=None above
		// makes winding moot regardless.
		gos_VERTEX t[6] = { q[0], q[1], q[2], q[0], q[2], q[3] };
		gos_DrawTriangles( t, 6 );
	}

	// Restore the render state we changed so later editor draws (selection quads,
	// brush overlays) are not affected. AlphaMode is left at AlphaInvAlpha to match
	// the editor's convention -- renderTerrainSelection() and the brushes rely on
	// it being set and do not restore it either.
	gos_SetRenderState( gos_State_Texture,  0 );
	gos_SetRenderState( gos_State_AlphaTest, 0 );
	gos_SetRenderState( gos_State_ZCompare, 1 );
	gos_SetRenderState( gos_State_ZWrite,   1 );
}

} // namespace FoliageRender
