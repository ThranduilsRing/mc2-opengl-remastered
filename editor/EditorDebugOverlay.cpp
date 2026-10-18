//-------------------------------------------------------------------------------------------------
// EditorDebugOverlay.cpp -- see EditorDebugOverlay.h.
//
// World overlays are drawn as screen-space gos line strips: sample world points along
// each grid line, project them with Camera::projectForScreenXY, and connect successive
// admitted points with gos_DrawLines (the exact primitive the editor's other overlays
// -- ScatterBrush/HeightBrush/selection -- use).  A line breaks (no segment) wherever a
// sample point fails projection (off-frustum / behind near plane) so a single bad point
// never spawns a screen-spanning streak.
//-------------------------------------------------------------------------------------------------
#include "stdafx.h"
#include "EditorDebugOverlay.h"
#include "GuiRuntime.h"   // GuiRuntime::AutoDockActive (skip SetNextWindowPos when docking)

#include "Camera.h"
#include "Terrain.h"
#include "BeautySidecarPreview.h"   // B7b delta heatmap overlay
#include "FoliageRender.h"
#include "vertex.h"                                   // PostcompVertex (.elevation/.water)
#include "../GameOS/gameos/gos_terrain_indirect.h"    // IsDenseRecipeReady()
#include "EditorObjectMgr.h"                          // object ID overlay
#include "EditorObjects.h"                            // Unit + waypoints (patrol path overlay)
#include "UnitBrainPanel.h"                           // IsOpen() gate for the patrol overlay

#ifdef MC2_IMGUI
#include "imgui.h"
#endif

#include <math.h>
#include <float.h>
#include <stdio.h>
#include <vector>

// The editor's terrain instance (same global ScatterBrush/StampBrush use).
extern Terrain* land;

// Render-side state accessors (GLuint == unsigned int; avoid pulling in glew here).
extern unsigned int gos_terrain_indirect_getAtlasGLTex();  // colormap atlas the chunk shader samples
bool mc2TerrainLodChunkEnabled();                          // GPU-direct chunk terrain path active?

namespace
{
	// --- overlay state (file static; pure UI, never saved) -----------------------
	bool  s_showChunkGrid      = false;
	bool  s_showSuperchunkGrid = false;
	bool  s_showWaterDebug     = false;
	bool  s_showFoliageDebug   = false;   // ImGui stats only; no world draw
	bool  s_showObjectIds      = false;   // draw id+name labels over every placed object

	int   s_superchunkChunks   = 3;       // 1..8 chunks per superchunk
	float s_lineOpacity        = 0.65f;   // 0..1
	float s_heightBias         = 8.0f;    // world units lifted above the sample plane
	bool  s_gridOnWaterPlane   = true;    // true: draw on water plane (water diag);
	                                      // false: follow terrain elevation

	const int   kChunkCells          = 20;     // chunk = 20 terrain cells (= 20 verts)
	const float kMaxSamplesPerLine   = 4096.f; // safety cap

	bool terrainLoaded()
	{
		return ( land != nullptr && Terrain::realVerticesMapSide > 1 );
	}

	// World height for a grid sample at (wx, wy).
	float sampleZ( float wx, float wy )
	{
		if ( s_gridOnWaterPlane )
			return Terrain::waterElevation + s_heightBias;

		glm::vec3 p;
		p.x = wx; p.y = wy; p.z = 0.0f;
		return land->getTerrainElevation( p ) + s_heightBias;
	}

	// Project a world point; returns false if it should break the line strip.
	bool projectPt( Camera* eye, float wx, float wy, float wz, float& sx, float& sy )
	{
		glm::vec3 world;  world.x = wx; world.y = wy; world.z = wz;
		Stuff::Vector4D scr;
		if ( !eye->projectForScreenXY( world, scr ) )
			return false;
		if ( scr.w <= 1e-4f )                       return false;   // behind near plane
		if ( !( scr.x == scr.x ) || !( scr.y == scr.y ) ) return false;   // NaN
		sx = scr.x; sy = scr.y;
		return true;
	}

	// Modern GL forward projection (projectModernClipGL + viewport remap) — the same
	// zoom-correct transform object selection uses. The legacy projectForScreenXY
	// (projectZ) above diverges/rejects when zoomed in, which made the patrol overlay
	// vanish on zoom. Used by the patrol/guard overlay.
	bool projectPtGL( Camera* eye, float wx, float wy, float wz, float& sx, float& sy )
	{
		glm::vec3 world; world.x = wx; world.y = wy; world.z = wz;
		ModernClipResult r = eye->projectModernClipGL( world );
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

	// Draw a connected world polyline (list of world XY samples at a given plane).
	// horizontal=true -> line runs along +X at fixed row world-y; else along -Y.
	void drawGridLine( Camera* eye, bool horizontal, float fixedWorld,
	                   float start, float end, float stepWorld, DWORD argb )
	{
		gos_VERTEX seg[2];
		memset( seg, 0, sizeof( seg ) );
		seg[0].rhw = seg[1].rhw = 1.0f;
		seg[0].argb = seg[1].argb = argb;

		bool havePrev = false;
		float px = 0.f, py = 0.f;

		int   n = 0;
		float t = start;
		const float dir = ( end >= start ) ? 1.0f : -1.0f;
		for ( ; ( dir > 0 ? t <= end : t >= end ) && n < kMaxSamplesPerLine; t += stepWorld * dir, ++n )
		{
			float wx, wy;
			if ( horizontal ) { wx = t;          wy = fixedWorld; }
			else              { wx = fixedWorld;  wy = t;          }

			float sx, sy;
			bool ok = projectPt( eye, wx, wy, sampleZ( wx, wy ), sx, sy );
			if ( ok && havePrev )
			{
				seg[0].x = px; seg[0].y = py;
				seg[1].x = sx; seg[1].y = sy;
				gos_DrawLines( seg, 2 );
			}
			havePrev = ok;
			px = sx; py = sy;
		}
	}

	DWORD packARGB( int r, int g, int b )
	{
		int a = (int)( s_lineOpacity * 255.0f );
		if ( a < 0 ) a = 0; if ( a > 255 ) a = 255;
		return ( (DWORD)a << 24 ) | ( (DWORD)r << 16 ) | ( (DWORD)g << 8 ) | (DWORD)b;
	}

	// Draw a square grid every `stepCells` vertices across the whole map.
	void drawGrid( Camera* eye, int stepCells, DWORD argb )
	{
		const long  side = Terrain::realVerticesMapSide;
		const float wupv = Terrain::worldUnitsPerVertex;
		const float tlx  = Terrain::mapTopLeft3d.x;
		const float tly  = Terrain::mapTopLeft3d.y;

		const float minX = tlx;
		const float maxX = tlx + (float)( side - 1 ) * wupv;
		const float minY = tly - (float)( side - 1 ) * wupv;   // bottom (largest row)
		const float maxY = tly;                                 // top (row 0)

		// Sample along each line at chunk resolution so terrain-follow mode tracks the
		// surface; on the water plane this is a straight line anyway.
		const float sampleStep = wupv * (float)kChunkCells;

		// Vertical lines (fixed column index c -> fixed world x), run top->bottom.
		for ( long c = 0; c < side; c += stepCells )
		{
			float wx = tlx + (float)c * wupv;
			drawGridLine( eye, /*horizontal=*/false, wx, maxY, minY, sampleStep, argb );
		}
		// Horizontal lines (fixed row index r -> fixed world y), run left->right.
		for ( long r = 0; r < side; r += stepCells )
		{
			float wy = tly - (float)r * wupv;
			drawGridLine( eye, /*horizontal=*/true, wy, minX, maxX, sampleStep, argb );
		}
	}

	// --- Terrain Probe (read-only instrumentation) ------------------------------
	// Proves WHERE generated elevations are lost by comparing, in the SAME running
	// editor session, three independent height sources + the water classification.
	struct HeightStats { bool valid; long count; float mn, mx, mean; };
	struct ProbeData
	{
		bool        scanned       = false;
		long        side          = 0;       // realVerticesMapSide
		long        expectVerts   = 0;       // side*side
		// (1) the generator's elevation file (if present on disk)
		bool        fileFound     = false;
		HeightStats file          = { false, 0, 0, 0, 0 };
		// (2) terrain vertex storage written by setVertexHeight (land->getVertexHeight)
		HeightStats vert          = { false, 0, 0, 0, 0 };
		// (3) the postcomp render/water mesh the renderer + recalcWater read
		HeightStats mesh          = { false, 0, 0, 0, 0 };
		long        waterVerts    = 0;
		long        dryVerts      = 0;
		float       waterElev     = 0.0f;
		// readiness flags
		bool        hasColormap   = false;   // land->terrainTextures2 != NULL
		bool        recipeReady   = false;   // gos_terrain_indirect::IsDenseRecipeReady()
		// render-side diagnosis
		unsigned    atlasTex      = 0;       // chunk colormap atlas GL tex (0 -> samples black)
		bool        chunkPath     = false;   // mc2TerrainLodChunkEnabled()
		long        lightBlack    = 0;       // verts whose baked localRGBLight RGB == 0
		unsigned    lightSample   = 0;       // first vert's localRGBLight (aRGB)
	};
	ProbeData s_probe;
	const char* kProbeElevPath = "terrain_gen_out\\genmap.elev.r32";

	void statsAccum( HeightStats& s, float v )
	{
		if ( !s.valid ) { s.valid = true; s.mn = s.mx = v; s.mean = 0.f; s.count = 0; }
		if ( v < s.mn ) s.mn = v;
		if ( v > s.mx ) s.mx = v;
		s.mean += v;          // sum; divided at the end
		++s.count;
	}
	void statsFinish( HeightStats& s ) { if ( s.valid && s.count > 0 ) s.mean /= (float)s.count; }

	void rescanTerrainHeights()
	{
		s_probe = ProbeData();
		s_probe.scanned = true;
		if ( !terrainLoaded() )
			return;

		const long side = Terrain::realVerticesMapSide;
		const long n    = side * side;
		s_probe.side        = side;
		s_probe.expectVerts = n;
		s_probe.waterElev   = Terrain::waterElevation;
		s_probe.hasColormap = ( land->terrainTextures2 != nullptr );
		s_probe.recipeReady = gos_terrain_indirect::IsDenseRecipeReady();
		s_probe.atlasTex    = gos_terrain_indirect_getAtlasGLTex();
		s_probe.chunkPath   = mc2TerrainLodChunkEnabled();

		// (1) elevation file on disk (the last generated one, if present).
		if ( FILE* ef = fopen( kProbeElevPath, "rb" ) )
		{
			s_probe.fileFound = true;
			float buf;
			while ( fread( &buf, sizeof(float), 1, ef ) == 1 )
				if ( buf == buf )                       // NaN guard
					statsAccum( s_probe.file, buf );
			statsFinish( s_probe.file );
			fclose( ef );
		}

		// (2) terrain vertex storage (what setVertexHeight wrote).
		for ( long i = 0; i < n; ++i )
		{
			float h = land->getVertexHeight( (int)i );
			if ( h == h ) statsAccum( s_probe.vert, h );
		}
		statsFinish( s_probe.vert );

		// (3) postcomp render/water mesh (what the renderer + recalcWater read).
		if ( Terrain::mapData )
		{
			PostcompVertexPtr verts = Terrain::mapData->getData();
			if ( verts )
			{
				for ( long i = 0; i < n; ++i )
				{
					const PostcompVertex& v = verts[i];
					if ( v.elevation == v.elevation ) statsAccum( s_probe.mesh, v.elevation );
					// PostcompVertex.water is a BITFIELD: bit0 = actually wet (the test
					// the renderer uses, quad.cpp `water & 1`); bits 0x40/0x80 are random
					// dither/edge MARKERS set by recalcWater. Counting `if (water)` (the
					// original trace's mistake) counts markers as wet -> false ~70%.
					if ( v.water & 0x01 ) ++s_probe.waterVerts; else ++s_probe.dryVerts;
					if ( ( v.localRGBLight & 0x00FFFFFF ) == 0 ) ++s_probe.lightBlack;
					if ( i == 0 ) s_probe.lightSample = (unsigned)v.localRGBLight;
				}
				statsFinish( s_probe.mesh );
			}
		}

		// Machine-readable line (smoke/DbgView). One per rescan; cheap.
		char line[512];
		snprintf( line, sizeof(line),
			"TERRAIN_PROBE side=%ld expect=%ld "
			"file_found=%d file_min=%.1f file_max=%.1f file_mean=%.1f file_n=%ld "
			"vert_min=%.1f vert_max=%.1f vert_mean=%.1f "
			"mesh_min=%.1f mesh_max=%.1f mesh_mean=%.1f "
			"water=%ld dry=%ld waterElev=%.1f colormap=%d recipeReady=%d "
			"chunkPath=%d atlasTex=%u lightBlack=%ld lightSample=0x%06X\n",
			s_probe.side, s_probe.expectVerts,
			s_probe.fileFound ? 1 : 0,
			s_probe.file.valid ? s_probe.file.mn : 0.f, s_probe.file.valid ? s_probe.file.mx : 0.f,
			s_probe.file.valid ? s_probe.file.mean : 0.f, s_probe.file.count,
			s_probe.vert.valid ? s_probe.vert.mn : 0.f, s_probe.vert.valid ? s_probe.vert.mx : 0.f,
			s_probe.vert.valid ? s_probe.vert.mean : 0.f,
			s_probe.mesh.valid ? s_probe.mesh.mn : 0.f, s_probe.mesh.valid ? s_probe.mesh.mx : 0.f,
			s_probe.mesh.valid ? s_probe.mesh.mean : 0.f,
			s_probe.waterVerts, s_probe.dryVerts, s_probe.waterElev,
			s_probe.hasColormap ? 1 : 0, s_probe.recipeReady ? 1 : 0,
			s_probe.chunkPath ? 1 : 0, s_probe.atlasTex,
			s_probe.lightBlack, s_probe.lightSample & 0x00FFFFFF );
		printf( "%s", line ); fflush( stdout );
		OutputDebugStringA( line );
	}

	// Water-plane bounds rectangle (4 edges) at exactly waterElevation.
	void drawWaterBounds( Camera* eye )
	{
		const long  side = Terrain::realVerticesMapSide;
		const float wupv = Terrain::worldUnitsPerVertex;
		const float tlx  = Terrain::mapTopLeft3d.x;
		const float tly  = Terrain::mapTopLeft3d.y;
		const float minX = tlx, maxX = tlx + (float)( side - 1 ) * wupv;
		const float minY = tly - (float)( side - 1 ) * wupv, maxY = tly;

		const bool savedMode = s_gridOnWaterPlane;
		s_gridOnWaterPlane = true;                 // force exact water plane for the box
		const float savedBias = s_heightBias;
		s_heightBias = 0.0f;
		const DWORD argb = packARGB( 60, 140, 255 );
		const float sampleStep = wupv * (float)kChunkCells;
		drawGridLine( eye, true,  maxY, minX, maxX, sampleStep, argb );  // top
		drawGridLine( eye, true,  minY, minX, maxX, sampleStep, argb );  // bottom
		drawGridLine( eye, false, minX, maxY, minY, sampleStep, argb );  // left
		drawGridLine( eye, false, maxX, maxY, minY, sampleStep, argb );  // right
		s_gridOnWaterPlane = savedMode;
		s_heightBias = savedBias;
	}

#ifdef MC2_IMGUI
	// Draw "id:NNN name" labels over every placed object in the scene.
	// Uses the ImGui foreground draw list so no GL state is disturbed.
	void drawObjectIdLabels( Camera* eye )
	{
		EditorObjectMgr* mgr = EditorObjectMgr::instance();
		if ( !mgr )
			return;

		ImDrawList* dl = ImGui::GetForegroundDrawList();
		if ( !dl )
			return;

		const ImU32 colText   = IM_COL32( 255, 255,  80, 230 );  // bright yellow, mostly opaque
		const ImU32 colShadow = IM_COL32(   0,   0,   0, 160 );  // drop-shadow for readability

		auto drawLabel = [&]( EditorObject* obj )
		{
			if ( !obj || !obj->appearance() )
				return;
			glm::vec3 wpos = obj->getPosition();

			// Modern zoom-correct projection (projectPtGL) — legacy projectForScreenXY
			// diverges/rejects when zoomed in (X-collapse), making labels jump/vanish.
			float scrX = 0.f, scrY = 0.f;
			if ( !projectPtGL( eye, wpos.x, wpos.y, wpos.z, scrX, scrY ) )
				return;

			char buf[64];
			const char* name = obj->getDisplayName();
			snprintf( buf, sizeof(buf), "id:%ld %s", obj->getID(), name ? name : "?" );

			const ImVec2 pos( scrX + 2.f, scrY - 14.f );
			// Drop shadow one pixel offset for contrast over bright terrain.
			dl->AddText( ImVec2( pos.x + 1.f, pos.y + 1.f ), colShadow, buf );
			dl->AddText( pos, colText, buf );
		};

		EditorObjectMgr::BUILDING_LIST blds = mgr->getBuildings();
		for ( auto it = blds.begin(); it != blds.end(); it++ )
			drawLabel( *it );

		EditorObjectMgr::UNIT_LIST units = mgr->getUnits();
		for ( auto it = units.begin(); it != units.end(); it++ )
			drawLabel( *it );

		EditorObjectMgr::DROPZONE_LIST dzs = mgr->getDropZones();
		for ( auto it = dzs.begin(); it != dzs.end(); it++ )
			drawLabel( *it );
	}
#endif // MC2_IMGUI

}

namespace EditorDebugOverlay
{

// --- Patrol/Move path overlay (UnitBrainPanel) -----------------------------
// All segments for the whole overlay are accumulated into one vertex list and
// drawn with a SINGLE gos_DrawLines per frame. The old one-gos_DrawLines-per-dash
// approach produced hundreds of draw calls when zoomed in (long on-screen paths ->
// many dashes) and lagged hard. Batched, it is one (or a few chunked) draw calls.
static std::vector<gos_VERTEX> s_patrolVerts;

static void patrolSeg( float x0, float y0, float x1, float y1, DWORD argb )
{
	gos_VERTEX a; memset( &a, 0, sizeof( a ) );
	a.rhw = 1.0f; a.argb = argb;
	gos_VERTEX b = a;
	a.x = x0; a.y = y0;
	b.x = x1; b.y = y1;
	s_patrolVerts.push_back( a );
	s_patrolVerts.push_back( b );
}

static void patrolFlush()
{
	// Chunk to stay well within any gos_DrawLines batch limit.
	const size_t kChunk = 4000;   // even (line list = pairs)
	for ( size_t off = 0; off < s_patrolVerts.size(); off += kChunk )
	{
		size_t n = s_patrolVerts.size() - off; if ( n > kChunk ) n = kChunk;
		gos_DrawLines( &s_patrolVerts[off], (int)n );
	}
	s_patrolVerts.clear();
}

// Dashed line between two screen points.
static void patrolDashed( float x0, float y0, float x1, float y1, DWORD argb )
{
	const float dx = x1 - x0, dy = y1 - y0;
	const float len = sqrtf( dx * dx + dy * dy );
	if ( len < 1.0f ) { patrolSeg( x0, y0, x1, y1, argb ); return; }
	const float ux = dx / len, uy = dy / len;
	float step = 17.0f;                              // dash(10) + gap(7)
	// Cap dashes per segment (huge zoomed-in on-screen lines) — widen the step so a
	// segment never emits more than ~256 dashes regardless of on-screen length.
	if ( len / step > 256.0f ) step = len / 256.0f;
	const float dash = step * 0.6f;
	for ( float s = 0.f; s < len; s += step )
	{
		float a = s, b = s + dash; if ( b > len ) b = len;
		patrolSeg( x0 + ux * a, y0 + uy * a, x0 + ux * b, y0 + uy * b, argb );
	}
}

// Small diamond marker at a screen point.
static void patrolDot( float x, float y, float r, DWORD argb )
{
	patrolSeg( x - r, y, x, y - r, argb );
	patrolSeg( x, y - r, x + r, y, argb );
	patrolSeg( x + r, y, x, y + r, argb );
	patrolSeg( x, y + r, x - r, y, argb );
}

// Arrowhead at (mx,my) pointing along unit dir (ux,uy) — two back-swept barbs.
static void patrolArrow( float mx, float my, float ux, float uy, DWORD argb )
{
	const float h = 10.0f;
	const float bx = -ux, by = -uy;   // backwards along the segment
	const float px = -uy, py = ux;    // screen-perpendicular
	patrolSeg( mx, my, mx + bx * h + px * h * 0.5f, my + by * h + py * h * 0.5f, argb );
	patrolSeg( mx, my, mx + bx * h - px * h * 0.5f, my + by * h - py * h * 0.5f, argb );
}

// Draw one unit's patrol/move path: team-color dashed line + dots at points +
// direction arrows. Patrol draws the closing loop segment; Move stops at the end.
static void drawOnePatrol( Camera* eye, Unit* unit )
{
	if ( !unit )
		return;
	unit->importPatrolFromBrainIfNeeded();   // show existing brain-.abl patrols too

	// Selected unit's path is emphasized (full alpha + bold + bright connector) so
	// you can tell which unit owns which path; others are dimmer.
	const bool sel = unit->isSelected();
	DWORD base = (DWORD)unit->getColor();
	if ( base == 0 ) base = 0x00ffffff;             // team "none" -> white
	base &= 0x00ffffff;
	const DWORD col  = base | ( sel ? 0xff000000u : 0xa0000000u );
	const DWORD ccol = base | ( sel ? 0xff000000u : 0x60000000u );   // connector

	// Unit's own screen position (connector start + guard ring center).
	const glm::vec3& up = unit->getPosition();
	float uX, uY;
	const bool uok = projectPtGL( eye, up.x, up.y, land->getTerrainElevation( up ) + 12.0f, uX, uY );

	const std::vector<glm::vec3>& wps = unit->getWaypoints();
	if ( wps.empty() )
	{
		// Guard brains hold their spawn position -> draw a guard ring at the unit.
		if ( unit->getBrainBehavior() == Unit::BRAIN_GUARD && uok )
		{
			const float r = 14.0f;
			float px = 0.f, py = 0.f; bool have = false;
			for ( int k = 0; k <= 8; ++k )
			{
				float a = (float)k * 0.78539816f;
				float x = uX + r * cosf( a ), y = uY + r * sinf( a );
				if ( have ) patrolSeg( px, py, x, y, col );
				px = x; py = y; have = true;
			}
			patrolDot( uX, uY, sel ? 5.0f : 4.0f, col );
		}
		return;
	}

	const bool loop = ( unit->getOrderType() == Unit::ORDER_PATROL );
	const float lift = 12.0f;

	std::vector<unsigned char> ok( wps.size(), 0 );
	std::vector<float> sx( wps.size(), 0.f ), sy( wps.size(), 0.f );
	for ( size_t i = 0; i < wps.size(); ++i )
	{
		float z = land->getTerrainElevation( wps[i] ) + lift;
		ok[i] = projectPtGL( eye, wps[i].x, wps[i].y, z, sx[i], sy[i] ) ? 1 : 0;
	}

	const size_t segCount = loop ? wps.size() : ( wps.size() ? wps.size() - 1 : 0 );
	for ( size_t i = 0; i < segCount; ++i )
	{
		size_t a = i, b = ( i + 1 ) % wps.size();
		if ( !loop && b == 0 ) break;
		if ( !ok[a] || !ok[b] ) continue;
		patrolDashed( sx[a], sy[a], sx[b], sy[b], col );
		float dx = sx[b] - sx[a], dy = sy[b] - sy[a];
		float len = sqrtf( dx * dx + dy * dy );
		// Bold the selected unit's path with +/- perpendicular offset copies.
		if ( sel && len > 1e-3f )
		{
			float ox = -dy / len * 1.5f, oy = dx / len * 1.5f;
			patrolDashed( sx[a] + ox, sy[a] + oy, sx[b] + ox, sy[b] + oy, col );
			patrolDashed( sx[a] - ox, sy[a] - oy, sx[b] - ox, sy[b] - oy, col );
		}
		if ( len > 1e-3f )
			patrolArrow( ( sx[a] + sx[b] ) * 0.5f, ( sy[a] + sy[b] ) * 0.5f, dx / len, dy / len, col );
	}

	for ( size_t i = 0; i < wps.size(); ++i )
		if ( ok[i] )
			patrolDot( sx[i], sy[i], sel ? 6.0f : 5.0f, col );

	// Connector: tie the path to its unit (unit position -> first waypoint), so it
	// is obvious which unit owns which path even with every path drawn at once.
	if ( uok && ok[0] )
	{
		patrolDashed( uX, uY, sx[0], sy[0], ccol );
		patrolDot( uX, uY, 3.0f, ccol );
	}
}

// Draw EVERY unit's patrol/move path (each in its own team color), only while the
// AI/Brain/Orders panel is open. Units with no waypoints are skipped inside.
void RenderPatrolPaths( Camera* eye )
{
	if ( !eye || !UnitBrainPanel::IsOpen() || !terrainLoaded() )
		return;
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if ( !mgr )
		return;

	// Same overlay render state as RenderWorldOverlay — alpha-blended, no texture,
	// always-on-top (ZCompare/ZWrite off). Without this the gos_DrawLines for the
	// path inherit arbitrary state (textured / z-tested) and never show.
	gos_SetRenderState( gos_State_Texture,   0 );
	gos_SetRenderState( gos_State_AlphaMode,  gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_AlphaTest,  0 );
	gos_SetRenderState( gos_State_ZCompare,   0 );
	gos_SetRenderState( gos_State_ZWrite,     0 );

	// Accumulate every unit's path into one vertex list, drawn in a single
	// gos_DrawLines (chunked). Non-selected first, selected last so the emphasized
	// path renders on top. patrolFlush() issues the batched draw + clears.
	s_patrolVerts.clear();
	EditorObjectMgr::UNIT_LIST units = mgr->getUnits();
	for ( int pass = 0; pass < 2; ++pass )
		for ( auto it = units.begin(); it != units.end(); it++ )
		{
			Unit* u = *it;
			if ( !u )
				continue;
			const bool wantSel = ( pass == 1 );
			if ( u->isSelected() != wantSel )
				continue;
			drawOnePatrol( eye, u );
		}
	patrolFlush();
}

// B7b: draw the loaded beauty-sidecar delta as a terrain heatmap — a small cross
// per changed cell, RED where elevation is raised, BLUE where lowered, sized by
// |delta|/max. Reuses the same screen-projection (projectPt) + render state the
// grid overlay uses. Grid side must match the live terrain.
void drawBeautyHeatmap( Camera* eye )
{
	const int    side  = BeautySidecarPreview::DeltaSide();
	const float* delta = BeautySidecarPreview::DeltaData();
	const int    terrSide = (int)Terrain::realVerticesMapSide;
	if ( !delta || side <= 0 || side != terrSide )
	{
		static int s_warned = 0;
		if ( s_warned < 3 ) { ++s_warned;
			fprintf( stderr, "[BEAUTY_HEATMAP] skip: deltaSide=%d terrainSide=%d delta=%p\n",
			         side, terrSide, (void*)delta ); fflush( stderr ); }
		return;
	}
	const float maxAbs = BeautySidecarPreview::DeltaMaxAbs();
	const float invMax = ( maxAbs > 1e-4f ) ? 1.0f / maxAbs : 0.0f;
	const float wupv = Terrain::worldUnitsPerVertex;
	const float tlx  = Terrain::mapTopLeft3d.x;
	const float tly  = Terrain::mapTopLeft3d.y;

	gos_VERTEX seg[2];
	memset( seg, 0, sizeof( seg ) );
	seg[0].rhw = seg[1].rhw = 1.0f;

	int drawn = 0, changed = 0, projfail = 0;
	const int kMaxMarkers = 4000;   // safety cap on draw count
	for ( int r = 0; r < side && drawn < kMaxMarkers; ++r )
	{
		for ( int c = 0; c < side && drawn < kMaxMarkers; ++c )
		{
			const float d = delta[ r * side + c ];
			if ( d == 0.0f )
				continue;
			++changed;
			const float wx = tlx + (float)c * wupv;
			const float wy = tly - (float)r * wupv;
			float sx, sy;
			if ( !projectPt( eye, wx, wy, sampleZ( wx, wy ), sx, sy ) )
				{ ++projfail; continue; }
			const float t    = ( ( d < 0 ? -d : d ) * invMax );   // 0..1
			const float half = 5.0f + 9.0f * t;                   // marker size px
			const DWORD argb = ( d > 0.0f ) ? packARGB( 255, 40, 40 )    // raised = red
			                                : packARGB( 60, 120, 255 );  // lowered = blue
			seg[0].argb = seg[1].argb = argb;
			seg[0].x = sx - half; seg[0].y = sy; seg[1].x = sx + half; seg[1].y = sy;
			gos_DrawLines( seg, 2 );
			seg[0].x = sx; seg[0].y = sy - half; seg[1].x = sx; seg[1].y = sy + half;
			gos_DrawLines( seg, 2 );
			++drawn;
		}
	}
	static int s_logged = 0;
	if ( s_logged < 3 ) { ++s_logged;
		fprintf( stderr, "[BEAUTY_HEATMAP] side=%d changed=%d projected_fail=%d drawn=%d maxAbs=%.2f\n",
		         side, changed, projfail, drawn, maxAbs ); fflush( stderr ); }
}

// B7c: draw the protected-zone mask — a marker per protected cell, RED for
// structural (roads/buildings, "do not touch"), BLUE for water. Water is
// downsampled to keep the draw count bounded. Grid must match the live terrain.
void drawProtectedOverlay( Camera* eye )
{
	const int            side = BeautySidecarPreview::ProtectedSide();
	const unsigned char* prot = BeautySidecarPreview::ProtectedData();
	if ( !prot || side <= 0 || side != (int)Terrain::realVerticesMapSide )
		return;
	const float wupv = Terrain::worldUnitsPerVertex;
	const float tlx  = Terrain::mapTopLeft3d.x;
	const float tly  = Terrain::mapTopLeft3d.y;

	gos_VERTEX seg[2];
	memset( seg, 0, sizeof( seg ) );
	seg[0].rhw = seg[1].rhw = 1.0f;

	int drawn = 0;
	const int kMaxMarkers = 6000;
	for ( int r = 0; r < side && drawn < kMaxMarkers; ++r )
	{
		for ( int c = 0; c < side && drawn < kMaxMarkers; ++c )
		{
			const unsigned char lv = prot[ r * side + c ];
			if ( lv == 0 ) continue;
			if ( lv == 1 && ( ( r ^ c ) & 1 ) ) continue;   // downsample water
			const float wx = tlx + (float)c * wupv;
			const float wy = tly - (float)r * wupv;
			float sx, sy;
			if ( !projectPt( eye, wx, wy, sampleZ( wx, wy ), sx, sy ) ) continue;
			const DWORD argb = ( lv == 2 ) ? packARGB( 255, 40, 40 )    // structural = red
			                               : packARGB( 40, 120, 255 );  // water = blue
			const float half = ( lv == 2 ) ? 4.0f : 2.5f;
			seg[0].argb = seg[1].argb = argb;
			seg[0].x = sx - half; seg[0].y = sy; seg[1].x = sx + half; seg[1].y = sy;
			gos_DrawLines( seg, 2 );
			seg[0].x = sx; seg[0].y = sy - half; seg[1].x = sx; seg[1].y = sy + half;
			gos_DrawLines( seg, 2 );
			++drawn;
		}
	}
}

void RenderWorldOverlay( Camera* eye )
{
	if ( !eye )
		return;

#ifdef MC2_IMGUI
	if ( s_showObjectIds )
		drawObjectIdLabels( eye );
#endif

	if ( !terrainLoaded() )
		return;
	if ( !s_showChunkGrid && !s_showSuperchunkGrid && !s_showWaterDebug
	     && !BeautySidecarPreview::ShowHeatmap() && !BeautySidecarPreview::ShowProtected() )
		return;

	// Overlay render state -- match FoliageRender: alpha-blended, no texture, drawn
	// always-on-top (ZCompare off) so the grid is readable over water/terrain.
	gos_SetRenderState( gos_State_Texture,   0 );
	gos_SetRenderState( gos_State_AlphaMode,  gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_AlphaTest,  0 );
	gos_SetRenderState( gos_State_ZCompare,   0 );
	gos_SetRenderState( gos_State_ZWrite,     0 );

	if ( s_showChunkGrid )
		drawGrid( eye, kChunkCells, packARGB( 0, 200, 255 ) );          // cyan

	if ( s_showSuperchunkGrid )
	{
		int chunks = s_superchunkChunks; if ( chunks < 1 ) chunks = 1; if ( chunks > 8 ) chunks = 8;
		drawGrid( eye, kChunkCells * chunks, packARGB( 255, 225, 40 ) ); // bright yellow
	}

	if ( s_showWaterDebug )
		drawWaterBounds( eye );

	if ( BeautySidecarPreview::ShowProtected() )
		drawProtectedOverlay( eye );

	if ( BeautySidecarPreview::ShowHeatmap() )
		drawBeautyHeatmap( eye );

	// Restore state for subsequent editor overlays (selection/brush quads).
	gos_SetRenderState( gos_State_ZCompare, 1 );
	gos_SetRenderState( gos_State_ZWrite,   1 );
}

void RunProbeOnce()
{
	rescanTerrainHeights();   // computes stats + emits the TERRAIN_PROBE log line
}

#ifdef MC2_IMGUI

void RenderImGui()
{
	ImGui::SetNextWindowSize( ImVec2( 300.f, 0.f ), ImGuiCond_Once );
	// Skip explicit pos under autodock (it would float the window out of the dock).
	if ( !GuiRuntime::AutoDockActive() )
		ImGui::SetNextWindowPos ( ImVec2( 16.f, 540.f ), ImGuiCond_Once );
	if ( !ImGui::Begin( "Debug Overlays" ) ) { ImGui::End(); return; }

	ImGui::Checkbox( "Show Chunk Grid",      &s_showChunkGrid );
	ImGui::Checkbox( "Show Superchunk Grid", &s_showSuperchunkGrid );
	ImGui::Checkbox( "Show Water Debug",     &s_showWaterDebug );
	ImGui::Checkbox( "Show Foliage Debug",   &s_showFoliageDebug );
	ImGui::Checkbox( "Object IDs",           &s_showObjectIds );

	ImGui::Separator();
	ImGui::SetNextItemWidth( 160.f );
	ImGui::SliderInt( "Superchunk = N chunks", &s_superchunkChunks, 1, 8 );
	ImGui::SetNextItemWidth( 160.f );
	ImGui::SliderFloat( "Line opacity", &s_lineOpacity, 0.05f, 1.0f, "%.2f" );
	ImGui::Checkbox( "Grid on water plane (vs follow terrain)", &s_gridOnWaterPlane );
	ImGui::SetNextItemWidth( 160.f );
	ImGui::SliderFloat( "Height bias", &s_heightBias, 0.0f, 64.0f, "%.0f" );

	ImGui::Separator();
	if ( terrainLoaded() )
	{
		const long  side  = Terrain::realVerticesMapSide;
		const int   chunks = ( s_superchunkChunks < 1 ) ? 1 : s_superchunkChunks;
		const int   chunksAcross = (int)( ( side + kChunkCells - 1 ) / kChunkCells );
		const int   superAcross  = ( chunksAcross + chunks - 1 ) / chunks;
		ImGui::Text( "Terrain: %ld x %ld verts", side, side );
		ImGui::Text( "Chunks across: %d  (chunk = %d cells)", chunksAcross, kChunkCells );
		ImGui::Text( "Superchunks across: %d  (= %d cells)", superAcross, kChunkCells * chunks );
		ImGui::Text( "Water elevation: %.1f", Terrain::waterElevation );
	}
	else
	{
		ImGui::TextDisabled( "(no terrain loaded)" );
	}

	if ( s_showFoliageDebug )
	{
		ImGui::Separator();
		ImGui::Text( "Foliage instances: %d", FoliageRender::Count() );
		ImGui::Text( "Foliage preview: %s", FoliageRender::Visible() ? "visible" : "hidden" );
	}

	// --- Terrain Probe ----------------------------------------------------------
	// Read-only: compares the three height sources in the SAME session so we can see
	// exactly where generated elevations are lost (file -> vertex store -> render mesh).
	ImGui::Separator();
	if ( ImGui::CollapsingHeader( "Terrain Probe" ) )
	{
		bool doScan = ImGui::Button( "Rescan Terrain Heights" );
		static double s_lastScan = -1.0;
		double now = ImGui::GetTime();
		if ( !s_probe.scanned || now - s_lastScan > 1.0 ) { doScan = true; }
		if ( doScan ) { rescanTerrainHeights(); s_lastScan = now; }

		auto row = []( const char* label, const HeightStats& s ) {
			if ( s.valid )
				ImGui::Text( "%-18s min=%.1f  max=%.1f  mean=%.1f  n=%ld",
					label, s.mn, s.mx, s.mean, s.count );
			else
				ImGui::TextDisabled( "%-18s (n/a)", label );
		};

		if ( !s_probe.scanned || s_probe.side == 0 )
		{
			ImGui::TextDisabled( "(no terrain loaded)" );
		}
		else
		{
			ImGui::Text( "Recipe/grid side: %ld   expect verts: %ld",
				s_probe.side, s_probe.expectVerts );
			// Map-identity guard (adversarial finding): the elev file is only the
			// CURRENT map's if its vertex count matches. A leftover file from a prior
			// generate would otherwise be silently compared against a loaded .pak.
			const bool fileCurrent =
				s_probe.fileFound && s_probe.file.valid && ( s_probe.file.count == s_probe.expectVerts );
			ImGui::Text( "Elev file: %s%s", kProbeElevPath,
				!s_probe.fileFound ? "  (NOT FOUND)"
				: fileCurrent      ? ""
				:                    "  (STALE: vert-count != this map)" );
			row( "1 file elev",   s_probe.file );
			// Sources 2 and 3 ALIAS the same MapData::blocks[] (get/setVertexHeight and
			// getData().elevation are the same array; recalcWater reads it too). Shown
			// separately only as a self-check -- they are EXPECTED to be identical.
			row( "2 vert store",   s_probe.vert );
			row( "3 mesh(==2)",    s_probe.mesh );
			ImGui::Text( "Water elev: %.1f   water verts: %ld   dry verts: %ld",
				s_probe.waterElev, s_probe.waterVerts, s_probe.dryVerts );
			ImGui::Text( "colormap set: %s   recipe ready: %s",
				s_probe.hasColormap ? "yes" : "NO",
				s_probe.recipeReady ? "yes" : "NO" );
			ImGui::Text( "chunk path: %s   colormap atlas tex: %s (id=%u)",
				s_probe.chunkPath ? "ON" : "off",
				s_probe.atlasTex ? "YES" : "MISSING", s_probe.atlasTex );
			ImGui::Text( "baked light: black %ld/%ld   sample=0x%06X",
				s_probe.lightBlack, s_probe.expectVerts, s_probe.lightSample & 0x00FFFFFF );
			if ( s_probe.chunkPath && s_probe.atlasTex == 0 )
				ImGui::TextColored( ImVec4(1,0.4f,0.4f,1),
					"-> colormap ATLAS missing (id=0) -> chunk terrain samples black" );
			else if ( s_probe.expectVerts > 0 && s_probe.lightBlack == s_probe.expectVerts )
				ImGui::TextColored( ImVec4(1,0.6f,0.3f,1),
					"-> baked light all-black -> terrain unlit" );

			// Decisive verdict: the only independent comparison is disk file vs the
			// blocks[] mesh (what the renderer + recalcWater consume).
			const HeightStats& M = s_probe.mesh;   // == vert store
			if ( s_probe.fileFound && !fileCurrent )
				ImGui::TextColored( ImVec4(1,0.5f,0.5f,1),
					"-> source 1 is a DIFFERENT map (count mismatch) -- ignore it" );
			else if ( fileCurrent && M.valid &&
			          ( fabsf( s_probe.file.mx - M.mx ) > 2.0f ||
			            fabsf( s_probe.file.mn - M.mn ) > 2.0f ) )
				ImGui::TextColored( ImVec4(1,0.4f,0.4f,1),
					"-> heights LOST in apply: file != mesh blocks[] (setVertexHeight path)" );
			else if ( M.valid && s_probe.waterVerts > 0 &&
			          M.mn > s_probe.waterElev + 1.0f )
				ImGui::TextColored( ImVec4(1,0.6f,0.3f,1),
					"-> heights OK but %ld verts flagged wet though all elev > waterElev"
					" (recalcWater ran with stale waterDepth / before apply)", s_probe.waterVerts );
			else if ( M.valid )
				ImGui::TextColored( ImVec4(0.5f,0.9f,0.5f,1),
					"-> file == mesh, water consistent (heights+water OK)" );
		}
	}

	ImGui::End();
}

#endif // MC2_IMGUI

} // namespace EditorDebugOverlay
