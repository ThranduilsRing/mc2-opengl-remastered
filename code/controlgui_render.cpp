#include "render_view.h"
// ControlGui: the sensor / overview / weapon-view overlays and ControlGui::render.
// Declared in the class headers; shared names in controlgui_internal.h.
#include <algorithm>
#include "sim_overlap.h"
#include "controlgui_internal.h"
#include "mc2fmt.h"


// Sensor view: standalone toggle (F7), works at all zooms. Draws sensor coverage
// rings on the ground — green = friendly, red = enemy — with the range labeled
// on the rim.
bool g_sensorViewOn = false;   // default OFF (toggle with F7)

static void drawSensorRing( Camera* eye, const glm::vec3& center, float radius,
                            unsigned long rgb, unsigned long aBits )
{
	if ( radius < 1.0f ) return;
	const int N = 48;
	float px[N + 1], py[N + 1];
	bool  ok[N + 1];
	float vmx, vmy, vax, vay;
	gos_GetViewport( &vmx, &vmy, &vax, &vay );

	int firstOk = -1;
	for ( int i = 0; i <= N; i++ )
	{
		float a = ( 2.0f * 3.14159265f ) * static_cast<float>(( i % N )) / static_cast<float>(N);
		glm::vec3 p;
		p.x = center.x + cosf( a ) * radius;
		p.y = center.y + sinf( a ) * radius;
		p.z = land ? land->getTerrainElevation( p ) : 0.0f;
		ModernClipResult r = eye->projectModernClipGL( p );
		// Only require "in front of camera" (w>0), NOT frustum admission — a
		// large ring is bigger than the view, so most points are off-screen but
		// the on-screen arc must still draw (the GPU clips the line segments).
		ok[i] = ( r.clip.w > 0.05f );
		if ( ok[i] )
		{
			px[i] = vax + ( r.clip.x / r.clip.w * 0.5f + 0.5f ) * vmx;
			py[i] = vay + ( 1.0f - ( r.clip.y / r.clip.w * 0.5f + 0.5f ) ) * vmy;
			if ( firstOk < 0 ) firstOk = i;
		}
	}
	if ( firstOk < 0 ) return;

	unsigned long col = aBits | rgb;
	gos_SetRenderState( gos_State_Texture, 0 );
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_AlphaTest, 0 );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite, 0 );
	for ( int i = 0; i < N; i++ )
	{
		if ( !ok[i] || !ok[i + 1] ) continue;
		gos_VERTEX ln[2];
		for (auto & v : ln) { v.z = 0; v.rhw = .5f; v.argb = col; v.frgb = 0; v.u = v.v = 0; }
		ln[0].x = px[i];     ln[0].y = py[i];
		ln[1].x = px[i + 1]; ln[1].y = py[i + 1];
		gos_DrawLines( ln, 2 );
	}

	// Range label on the rim (top of the circle, ring index N/4 = +y).
	int li = N / 4;
	if ( ok[li] && gosFontHandle )
	{
		char buf[16];
		mc2::fmt::to_buf(buf, "{:.0f}m", radius / worldUnitsPerMeter);	// world units -> meters
		gos_TextSetAttributes( gosFontHandle, aBits | 0x00000000, 1.0f, false, true, false, false );
		gos_TextSetPosition( static_cast<int>(px[li]) + 1, static_cast<int>(py[li]) + 1 );
		gos_TextDraw( buf );
		gos_TextSetAttributes( gosFontHandle, aBits | rgb, 1.0f, false, true, false, false );
		gos_TextSetPosition( static_cast<int>(px[li]), static_cast<int>(py[li]) );
		gos_TextDraw( buf );
	}
}

// Tiny radio-tower glyph: A-frame mast + crossbar, apex up, base on the spot.
static void drawTowerGlyph( Camera* eye, const glm::vec3& wp, unsigned long col )
{
	ModernClipResult r = eye->projectModernClipGL( wp );
	if ( r.clip.w <= 0.05f ) return;
	float vmx, vmy, vax, vay;
	gos_GetViewport( &vmx, &vmy, &vax, &vay );
	float cx = vax + ( r.clip.x / r.clip.w * 0.5f + 0.5f ) * vmx;
	float cy = vay + ( 1.0f - ( r.clip.y / r.clip.w * 0.5f + 0.5f ) ) * vmy;
	gos_VERTEX ln[2];
	for (auto & v : ln) { v.z = 0; v.rhw = .5f; v.argb = col; v.frgb = 0; v.u = v.v = 0; }
	// left leg
	ln[0].x = cx - 5.0f; ln[0].y = cy;        ln[1].x = cx; ln[1].y = cy - 12.0f; gos_DrawLines( ln, 2 );
	// right leg
	ln[0].x = cx + 5.0f; ln[0].y = cy;        ln[1].x = cx; ln[1].y = cy - 12.0f; gos_DrawLines( ln, 2 );
	// crossbar
	ln[0].x = cx - 2.5f; ln[0].y = cy - 6.0f; ln[1].x = cx + 2.5f; ln[1].y = cy - 6.0f; gos_DrawLines( ln, 2 );
	// signal tick above the apex
	ln[0].x = cx; ln[0].y = cy - 13.0f;       ln[1].x = cx; ln[1].y = cy - 16.0f; gos_DrawLines( ln, 2 );
}

static void renderSensorView( Camera* eye )
{
	if (!eye || !mc2::render_view::hud().home) return;
	const unsigned long aBits = 0xcc000000;	// ~80% alpha
	bool prevExempt = gos_GetHudScaleExempt();
	gos_SetHudScaleExempt( true );

    for (const auto& row : mc2::render_view::hud().movers) {
        if (!row.active || row.sensorRange < 10 || row.sensorBroken || row.shutdown) continue;
        if (!row.friendly && !row.detected) continue;
        drawSensorRing(eye, row.position, row.sensorRange * worldUnitsPerMeter,
            row.friendly ? 0x0000cc00 : 0x00ff2020, aBits);
    }
    for (const auto& row : mc2::render_view::hud().sensors) {
        drawSensorRing(eye, row.position, row.range * worldUnitsPerMeter, 0x0033ddff, aBits);
        drawTowerGlyph(eye, row.position, aBits | 0x0033ddff);
    }

	gos_SetHudScaleExempt( prevExempt );
}

// Tactical Overview friendly-coverage tint: darken the whole battlefield
// slightly EXCEPT where friendly sensors have line of sight (inverted fog).
// Screen-cell grid; each cell maps to a ground point via the O(1)
// screenToGroundPlaneApprox, tested in WORLD space against friendly sensor
// ranges. Reliable (plain dark quads), no stencil / RTT.
static void renderOverviewCoverageTint( Camera* eye, float alpha )
{
	if (!eye || alpha <= 0.0f || !mc2::render_view::hud().home)
		return;

	struct Cov { float x, y, r; };
	Cov cov[256];
	int nc = 0;
    for (const auto& row : mc2::render_view::hud().coverage) {
        cov[nc].x = row.position.x; cov[nc].y = row.position.y; cov[nc].r = row.range; ++nc;
    }

	const float D    = 0.30f * alpha;	// max darkness in unsighted areas
	const int   sw   = Environment.screenWidth;
	const int   sh   = Environment.screenHeight;
	const int   cell = 28;

	gos_SetRenderState( gos_State_Texture, 0 );
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_AlphaTest, 0 );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite, 0 );

	bool prevExempt = gos_GetHudScaleExempt();
	gos_SetHudScaleExempt( true );

	for ( int gy = 0; gy < sh; gy += cell )
		for ( int gx = 0; gx < sw; gx += cell )
		{
			glm::vec3 w;
			if ( !eye->screenToGroundPlaneApprox( gx + cell / 2, gy + cell / 2, w ) )
				continue;	// not ground (sky) — leave untouched

			float coverage = 0.0f;	// 0 = unsighted, 1 = fully inside friendly LOS
			for ( int k = 0; k < nc; ++k )
			{
				float dx = w.x - cov[k].x, dy = w.y - cov[k].y;
				float dist = sqrtf( dx * dx + dy * dy );
				float feather = cov[k].r * 0.18f + 1.0f;
				float c = ( cov[k].r - dist ) / feather;	// soft edge
				coverage = std::max(coverage, c);
				if ( coverage >= 1.0f ) break;
			}
			coverage = std::clamp(coverage, 0.0f, 1.0f);

			float a = D * ( 1.0f - coverage );
			if ( a <= 0.003f ) continue;	// fully sighted: full brightness
			unsigned long aB = static_cast<unsigned long>(( a * 255.0f )); aB = std::min<unsigned long>(aB, 255);
			unsigned long col = ( aB << 24 );	// black veil

			gos_VERTEX q[4];
			float l = static_cast<float>(gx), r = static_cast<float>(( gx + cell )), t = static_cast<float>(gy), b = static_cast<float>(( gy + cell ));
			for (auto & v : q) { v.z = 0; v.rhw = .5f; v.argb = col; v.frgb = 0; v.u = v.v = 0; }
			q[0].x = l; q[0].y = t; q[1].x = l; q[1].y = b; q[2].x = r; q[2].y = b; q[3].x = r; q[3].y = t;
			gos_DrawQuads( q, 4 );
		}

	gos_SetHudScaleExempt( prevExempt );
}

// Tactical Overview: red chevron markers for DETECTED enemy/neutral units
// (friendlies are drawn as icons by ForceGroupBar). Honors the same fog/ECM/
// signature/shutdown gating the tac map uses.
static void renderOverviewEnemyMarkers( Camera* eye, float alpha )
{
	if (!eye || alpha <= 0.0f || !mc2::render_view::hud().home)
		return;

	unsigned long aB = static_cast<unsigned long>(( alpha * 255.0f ));
	aB = std::min<unsigned long>(aB, 255);
	const unsigned long aBits = aB << 24;
	const float w = 8.0f, h = 12.0f;

	gos_SetRenderState( gos_State_Texture, 0 );
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_AlphaTest, 0 );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite, 0 );

	bool prevExempt = gos_GetHudScaleExempt();
	gos_SetHudScaleExempt( true );

    for (const auto& mover : mc2::render_view::hud().movers)
    {
        if (!mover.active || mover.friendly || !mover.enemyMarker) continue;
        unsigned long rgb = mover.neutral ? 0x000000ff : 0x00ff0000;
        if (mover.selected) rgb |= 0x003f3f3f;
        unsigned long col = aBits | rgb;
        glm::vec3 wp = mover.position;
		ModernClipResult r = eye->projectModernClipGL( wp );
		if ( !r.admit || r.clip.w <= 0.05f )
			continue;
		float vmx, vmy, vax, vay;
		gos_GetViewport( &vmx, &vmy, &vax, &vay );
		float cx = vax + ( r.clip.x / r.clip.w * 0.5f + 0.5f ) * vmx;
		float cy = vay + ( 1.0f - ( r.clip.y / r.clip.w * 0.5f + 0.5f ) ) * vmy;

		// Downward chevron, apex on the unit.
		gos_VERTEX tri[3];
		for (auto & k : tri) { k.z = 0; k.rhw = .5f; k.argb = col; k.frgb = 0; k.u = k.v = 0; }
		tri[0].x = cx;     tri[0].y = cy;
		tri[1].x = cx - w; tri[1].y = cy - h;
		tri[2].x = cx + w; tri[2].y = cy - h;
		gos_DrawTriangles( tri, 3 );
	}

	gos_SetHudScaleExempt( prevExempt );
}

// Weapon view: standalone toggle (F8). Per friendly unit, one ring per weapon
// range BAND present (short/medium/long) — so duplicate weapons share a ring.
bool g_weaponViewOn = false;
extern float WeaponRanges[5][2];	// [SHORT/MEDIUM/LONG/...][min,max], from game data

// Thick screen-space line segment (a quad), half-width hw.
static void csgThickSeg( float x0, float y0, float x1, float y1, float hw, unsigned long c )
{
	float dx = x1 - x0, dy = y1 - y0, len = sqrtf( dx * dx + dy * dy );
	if ( len < 0.001f ) return;
	float nx = -dy / len * hw, ny = dx / len * hw;
	gos_VERTEX q[4];
	for (auto & v : q) { v.z = 0; v.rhw = .5f; v.argb = c; v.frgb = 0; v.u = v.v = 0; }
	q[0].x = x0 + nx; q[0].y = y0 + ny; q[1].x = x1 + nx; q[1].y = y1 + ny;
	q[2].x = x1 - nx; q[2].y = y1 - ny; q[3].x = x0 - nx; q[3].y = y0 - ny;
	gos_DrawQuads( q, 4 );
}

// Draw the UNION outline of N equal-radius ground circles (centers/sel arrays):
// a circle-point is on the boundary only if it is NOT inside any OTHER circle,
// so overlapping interior arcs are culled (figure-eight). Selected units' arcs
// are bolded.
static void drawWeaponBandUnion( Camera* eye, const glm::vec3* centers,
                                 const bool* sel, int n, float R,
                                 unsigned long rgb, unsigned long aBits )
{
	if ( n <= 0 || R < 1.0f ) return;
	const unsigned long col = aBits | rgb;
	const int N = 72;
	const float R2 = R * R * 0.997f;	// epsilon so shared boundaries aren't double-culled
	float vmx, vmy, vax, vay;
	gos_GetViewport( &vmx, &vmy, &vax, &vay );

	for ( int i = 0; i < n; i++ )
	{
		bool havePrev = false; float ppx = 0, ppy = 0;
		// Hairline arcs like the sensor rings for everyone; the selected
		// mech's arc is brightened instead of thickened.
		const unsigned long arcCol = sel[i] ? ( col | 0x003f3f3f ) : col;
		for ( int k = 0; k <= N; k++ )
		{
			float a = ( 2.0f * 3.14159265f ) * static_cast<float>(( k % N )) / static_cast<float>(N);
			glm::vec3 p;
			p.x = centers[i].x + cosf( a ) * R;
			p.y = centers[i].y + sinf( a ) * R;
			p.z = land ? land->getTerrainElevation( p ) : 0.0f;

			bool interior = false;	// inside another circle of this band?
			for ( int j = 0; j < n; j++ )
			{
				if ( j == i ) continue;
				float ddx = p.x - centers[j].x, ddy = p.y - centers[j].y;
				if ( ddx * ddx + ddy * ddy < R2 ) { interior = true; break; }
			}
			if ( interior ) { havePrev = false; continue; }

			ModernClipResult r = eye->projectModernClipGL( p );
			if ( r.clip.w <= 0.05f ) { havePrev = false; continue; }
			float sx = vax + ( r.clip.x / r.clip.w * 0.5f + 0.5f ) * vmx;
			float sy = vay + ( 1.0f - ( r.clip.y / r.clip.w * 0.5f + 0.5f ) ) * vmy;
			if ( havePrev )
			{
				gos_VERTEX ln[2];
				for (auto & v : ln) { v.z = 0; v.rhw = .5f; v.argb = arcCol; v.frgb = 0; v.u = v.v = 0; }
				ln[0].x = ppx; ln[0].y = ppy;
				ln[1].x = sx;  ln[1].y = sy;
				gos_DrawLines( ln, 2 );
			}
			ppx = sx; ppy = sy; havePrev = true;
		}
	}

	// One range label per BAND (first boundary rim point found), not per circle.
	for ( int i = 0; i < n; i++ )
	{
		glm::vec3 p;
		p.x = centers[i].x; p.y = centers[i].y + R;
		p.z = land ? land->getTerrainElevation( p ) : 0.0f;
		bool interior = false;
		for ( int j = 0; j < n; j++ )
		{
			if ( j == i ) continue;
			float ddx = p.x - centers[j].x, ddy = p.y - centers[j].y;
			if ( ddx * ddx + ddy * ddy < R2 ) { interior = true; break; }
		}
		if ( interior ) continue;
		ModernClipResult r = eye->projectModernClipGL( p );
		if ( r.clip.w <= 0.05f ) continue;
		float sx = vax + ( r.clip.x / r.clip.w * 0.5f + 0.5f ) * vmx;
		float sy = vay + ( 1.0f - ( r.clip.y / r.clip.w * 0.5f + 0.5f ) ) * vmy;
		if ( gosFontHandle )
		{
			char buf[16];
			mc2::fmt::to_buf(buf, "{:.0f}m", R / worldUnitsPerMeter);
			gos_TextSetAttributes( gosFontHandle, aBits | 0x00000000, 1.0f, false, true, false, false );
			gos_TextSetPosition( static_cast<int>(sx) + 1, static_cast<int>(sy) + 1 ); gos_TextDraw( buf );
			gos_TextSetAttributes( gosFontHandle, aBits | rgb, 1.0f, false, true, false, false );
			gos_TextSetPosition( static_cast<int>(sx), static_cast<int>(sy) ); gos_TextDraw( buf );
			break;	// one label per band is plenty
		}
	}
}

static void renderWeaponView( Camera* eye )
{
	if (!eye || !mc2::render_view::hud().home) return;
	static const unsigned long kBandColor[3] = { 0x00ffe24b, 0x00ff9b30, 0x00ff5a3a }; // S/M/L
	static const int kBandWR[3] = { 0 /*SHORT*/, 1 /*MEDIUM*/, 2 /*LONG*/ };
	const unsigned long aBits = 0xb0000000;

	// Bucket friendly units into the 3 weapon bands they have.
	const int kMax = 64;
	glm::vec3 centers[3][kMax];
	bool sel[3][kMax];
	int n[3] = { 0, 0, 0 };
    for (const auto& m : mc2::render_view::hud().movers) {
        if (!m.active || !m.friendly || !m.own) continue;
        for (int b = 0; b < 3; ++b)
            if (m.weaponBands[b] && n[b] < kMax) {
                centers[b][n[b]] = m.position; sel[b][n[b]] = m.selected; ++n[b];
            }
    }

	bool prevExempt = gos_GetHudScaleExempt();
	gos_SetHudScaleExempt( true );
	gos_SetRenderState( gos_State_Texture, 0 );
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_AlphaTest, 0 );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite, 0 );
	for ( int b = 0; b < 3; b++ )
	{
		float R = WeaponRanges[kBandWR[b]][1] * worldUnitsPerMeter;
		drawWeaponBandUnion( eye, centers[b], sel[b], n[b], R, kBandColor[b], aBits );
	}
	gos_SetHudScaleExempt( prevExempt );
}

// Formation line (MC2_TACMAP_FORMATION_LINE): RENDER ONLY — ghost line, slot
// pips, armed cursor hint. Input/state advance + order issuance live in
// MissionInterfaceManager::update (missiongui.cpp), BEFORE the world mouse
// handlers, so the press/release cannot leak into select/move.
static void updateAndRenderFormationLine( Camera* eye )
{
	if ( !eye || !Team::home || !TacticalOverview::formationLineEnabled() )
		return;
	TacticalOverview::FormationLineState st = g_tacticalOverview.flState();
	if ( st == TacticalOverview::FL_IDLE )
		return;

	long mx = userInput->getMouseX();
	long my = userInput->getMouseY();

	// --- render ---
	bool prevExempt = gos_GetHudScaleExempt();
	gos_SetHudScaleExempt( true );
	gos_SetRenderState( gos_State_Texture, 0 );
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_AlphaTest, 0 );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite, 0 );

	const unsigned long aBits = 0xcc000000;
	const unsigned long kGreen = 0x0000cc00;

	if ( st == TacticalOverview::FL_ARMED )
	{
		// Cursor hint while armed.
		if ( gosFontHandle )
		{
			gos_TextSetAttributes( gosFontHandle, aBits | kGreen, 1.0f, false, true, false, false );
			gos_TextSetPosition( static_cast<int>(mx) + 14, static_cast<int>(my) + 14 );
			gos_TextDraw( "DRAW FORMATION LINE" );
		}
	}
	else if ( st == TacticalOverview::FL_DRAGGING )
	{
		float vmx, vmy, vax, vay;
		gos_GetViewport( &vmx, &vmy, &vax, &vay );

		// Compute the slots first (they already reflect the spacing factor);
		// drop each onto the terrain surface so it does not float over dips.
		glm::vec3 slots[32];
		int ns = g_tacticalOverview.flComputeSlots( slots, 32 );
		for ( int s = 0; s < ns; s++ )
			if ( land )
				slots[s].z = land->getTerrainElevation( slots[s] );

		// Project all slots to screen once.
		float sxArr[32], syArr[32];
		bool  okArr[32];
		for ( int s = 0; s < ns; s++ )
		{
			ModernClipResult r = eye->projectModernClipGL( slots[s] );
			okArr[s] = ( r.clip.w > 0.05f );
			if ( okArr[s] )
			{
				sxArr[s] = vax + ( r.clip.x / r.clip.w * 0.5f + 0.5f ) * vmx;
				syArr[s] = vay + ( 1.0f - ( r.clip.y / r.clip.w * 0.5f + 0.5f ) ) * vmy;
			}
		}

		// Ghost line spans the OUTERMOST slots (WYSIWYG: the line = where the
		// squad lands, already scaled by the spacing wheel).
		if ( ns >= 2 && okArr[0] && okArr[ns - 1] )
			csgThickSeg( sxArr[0], syArr[0], sxArr[ns - 1], syArr[ns - 1], 1.5f, aBits | kGreen );

		// Slot pips.
		for ( int s = 0; s < ns; s++ )
		{
			if ( !okArr[s] ) continue;
			const float hp = 3.0f;
			gos_VERTEX q[4];
			for (auto & v : q) { v.z = 0; v.rhw = .5f; v.argb = aBits | kGreen; v.frgb = 0; v.u = v.v = 0; }
			q[0].x = sxArr[s] - hp; q[0].y = syArr[s] - hp; q[1].x = sxArr[s] - hp; q[1].y = syArr[s] + hp;
			q[2].x = sxArr[s] + hp; q[2].y = syArr[s] + hp; q[3].x = sxArr[s] + hp; q[3].y = syArr[s] - hp;
			gos_DrawQuads( q, 4 );
		}

		// Unit count + spacing percent at the cursor.
		if ( gosFontHandle )
		{
			char buf[32];
			mc2::fmt::to_buf(buf, "{:d}  {:.0f}%", mc2::fmt::as_i32(g_tacticalOverview.flMoverCount()), g_tacticalOverview.flSpacing() * 100.0f);
			gos_TextSetAttributes( gosFontHandle, aBits | kGreen, 1.0f, false, true, false, false );
			gos_TextSetPosition( static_cast<int>(mx) + 14, static_cast<int>(my) + 14 );
			gos_TextDraw( buf );
		}
	}

	gos_SetHudScaleExempt( prevExempt );
}

extern const char* extractMoverHudStatus(Mover* mover);

void ControlGui::extractHudSnapshot()
{
    latchResults();   // the last draw finished and no tick is in flight: resultsDone on the worker reads this
    auto& hud = mc2::render_view::stagingHud();
    hud.movers.clear(); hud.roster.clear(); hud.sensors.clear(); hud.coverage.clear(); hud.objectives.clear();
    hud.home = Team::home != nullptr;
    hud.timeLimit = mission ? static_cast<float>(mission->m_timeLimit) : 0;
    hud.resourcePoints = LogisticsData::instance ? LogisticsData::instance->getResourcePoints() : 0;
    hud.ended = mission && mission->terminationCounterStarted;
    hud.objectiveStatus = OS_SUCCESSFUL;
    hud.objectiveCount = Team::home ? Team::home->objectives.Count() : 0;
    if (!Team::home || !ObjectManager) return;
    for (int i = 0; i < ObjectManager->numMovers; ++i) {
        Mover* m = ObjectManager->getMover(i);
        if (!m) continue;
        auto& row = hud.movers.emplace_back();
        row.object = m; row.position = m->getPosition();
        row.active = m->getExists() && !m->isDestroyed() && !m->isDisabled();
        row.friendly = m->getTeamId() == Team::home->id;
        row.own = m->getCommander() && commanderIsHome(m->getCommander());
        row.neutral = m->getTeam() && m->getTeam()->isNeutral(Team::home);
        row.selected = m->getSelected(); row.onGui = m->isOnGUI();
        row.shutdown = m->getStatus() == OBJECT_STATUS_SHUTDOWN;
        row.canJump = m->canJump();
        row.name = m->getName();
        row.health = m->getAppearance() ? m->getAppearance()->barStatus : 0;
        // Object draws used to set barStatus before the HUD. Read that same pending value at extract.
        for (const auto& view : mc2::render_view::list().movers)
            if (view.obj == m && (view.mode == mc2::render_view::Mode::Full || view.mode == mc2::render_view::Mode::Own)) {
                row.health = view.barStatus; break;
            }
        for (int g = 0; g < 10; ++g) if (m->isInUnitGroup(g)) row.groups |= 1u << g;
        auto* sensor = m->getSensorSystem();
        row.sensorRange = sensor ? sensor->getRange() : 0;
        row.sensorBroken = !sensor || sensor->broken;
        row.detected = m->contactView.blip;
        row.enemyMarker = row.detected && (m->contactView.model
            || (!row.shutdown && !m->hasNullSignature() && m->getEcmRange() <= 0));
        m->getWeaponBandsPresent(row.weaponBands);
        glm::vec3 destination;
        row.moving = m->getMoveDestination(destination);
        if (auto* pilot = m->getPilot()) {
            row.pilot = pilot; row.pilotName = pilot->getName();
            row.messagePlaying = pilot->getMessagePlaying();
            pilot->extractHudPaths(row);
            row.orderStatus = extractMoverHudStatus(m);
        }
    }
    for (long i = 0; i < Team::home->getRosterSize(); ++i) {
        const auto* object = Team::home->getMover(i);
        for (size_t j = 0; j < hud.movers.size(); ++j)
            if (hud.movers[j].object == object) { hud.roster.push_back(j); break; }
    }
    if (auto* sys = SensorManager ? SensorManager->getTeamSensor(Team::home->getId()) : nullptr)
        for (int i = 0; i < sys->numSensors; ++i) {
            auto* sensor = sys->sensors[i];
            if (!sensor || sensor->broken || !sensor->owner || sensor->owner->isDestroyed()) continue;
            const float range = sensor->getRange();
            if (range >= 1 && hud.coverage.size() < 256)
                hud.coverage.push_back({sensor->owner->getPosition(), range, false});
            if (!sensor->owner->isMover() && range >= 10)
                hud.sensors.push_back({sensor->owner->getPosition(), range, true});
        }
    for (CObjective* objective : Team::home->objectives) {
        auto& row = hud.objectives.emplace_back();
        row.object = objective;
        row.position = {objective->MarkerX(), objective->MarkerY(), 0};
        row.marker = objective->DisplayMarker(); row.markerText = objective->MarkerText();
        row.visible = !objective->IsResolved() && row.marker && objective->IsActive();
        row.active = objective->IsActive(); row.hidden = objective->IsHiddenTrigger();
        row.primary = objective->Priority() == 1;
        row.status = objective->observedStatus();
        row.points = objective->ResourcePoints(); row.description = objective->LocalizedDescription();
        row.color = row.status == OS_FAILED ? 0xffc61c00 : row.status == OS_SUCCESSFUL ? 0xff41c700 : 0xffffc600;
        if (objective->ActivateOnFlag() && mission->actualTime - objective->ActivationTime() < 20
            && static_cast<int>(2 * (mission->actualTime - objective->ActivationTime())) % 2 == 0)
            row.color = 0xffffffff;
    }
    // Display the status resolved by the tick; Status() evaluates conditions and dispatches actions.
    for (const auto& row : hud.objectives) {
        if (!row.primary) continue;
        const int status = row.active ? row.status : OS_UNDETERMINED;
        if (status == OS_FAILED) { hud.objectiveStatus = OS_FAILED; break; }
        if (status == OS_UNDETERMINED) hud.objectiveStatus = OS_UNDETERMINED;
    }
    tacMap.extractHudBlips();
    if (infoWnd) infoWnd->extractHudSnapshot();
}

void ControlGui::render( bool bPaused )
{
	ZoneScopedN("ControlGui::render");
	if (drawGUIOn)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
		gos_SetRenderState( gos_State_Specular,FALSE );
		gos_SetRenderState( gos_State_AlphaTest, TRUE );
				
		gos_SetRenderState( gos_State_Filter, gos_FilterNone );
		gos_SetRenderState( gos_State_ZWrite, 0 );
		gos_SetRenderState( gos_State_ZCompare, 0 );

		for ( int i = 0; i < rectCount; i++ )
		{
			drawRect( rectInfos[i].rect, rectInfos[i].color );
		}

		// Tactical Overview: draw each unit's HUD icon over its world position on
		// the main screen whenever the overview is active, plus objective markers.
		// Sensor view (F7) + weapon view (F8): standalone overlays, any zoom.
		if ( g_sensorViewOn )
			renderSensorView( eye );
		if ( g_weaponViewOn )
			renderWeaponView( eye );

		if ( g_tacticalOverview.active() )
		{
			// Formation line ghost + pips + order issuance (env-gated, no-op IDLE).
			updateAndRenderFormationLine( eye );

			// Altitude-driven overlay stages (independent of the abstract blend t):
			//   - icons roll in hard over alt 6000 -> 6500 (just past gameplay cap)
			//   - squad cards take over above alt 12000 (full by 14000)
			const float alt = eye->getCameraAltitude();
			float iconViz = std::clamp( ( alt - 6000.0f ) / 500.0f, 0.0f, 1.0f );
			float cardViz = std::clamp( ( alt - 12000.0f ) / 2000.0f, 0.0f, 1.0f );
			float iconW = iconViz * ( 1.0f - cardViz );
			float markerA = iconViz;

			// Friendly-coverage tint first (under the icons/markers).
			if ( TacticalOverview::tintEnabled() )
				renderOverviewCoverageTint( eye, markerA );

			// Move-path arrows, colored by squad. At icon zoom: one per mech.
			// At squad-card zoom: a single arrow per squad (centroid -> avg dest).
			static const unsigned long kSquadColors[10] = {
				0x004bff4b, 0x004b9bff, 0x00ffd24b, 0x00ff7b4b, 0x00c77bff,
				0x004bffe0, 0x00ff4b9b, 0x009bff4b, 0x00b0b0ff, 0x00ffffff };
			if ( mc2::render_view::hud().home )
			{
				// Per-mech arrows fade with the icon stage.
				if ( iconW > 0.01f )
				{
					unsigned long pa = static_cast<unsigned long>(( iconW * 255.0f )) << 24;
					for (size_t index : mc2::render_view::hud().roster)
					{
						const auto& m = mc2::render_view::hud().movers[index];
						if ( !m.onGui || !m.active ) continue;
						if ( !m.own ) continue;
						int g = -1;
						for ( int j = 0; j < 10; ++j ) if ( (m.groups & (1u << j)) ) { g = j; break; }
						mc2::render_view::drawHudOverviewPath(m,  pa | ( ( g >= 0 ) ? kSquadColors[g] : 0x0000cc00 ) );
					}
				}
				// Single squad arrow per force group, fading with the card stage:
				// trace the LEADER's (first moving member's) real path. Averaging
				// curved routes gets mushy, so pick one representative.
				if ( cardViz > 0.01f )
				{
					unsigned long pa = static_cast<unsigned long>(( cardViz * 255.0f )) << 24;
					for ( int g = 0; g < 10; ++g )
					{
						for (size_t index : mc2::render_view::hud().roster)
						{
							const auto& m = mc2::render_view::hud().movers[index];
							if ( !m.onGui || !m.active ) continue;
							if ( !m.own ) continue;
							if ( !(m.groups & (1u << g)) ) continue;
							glm::vec3 d;
							if ( m.moving )	// this member has a move
							{
								mc2::render_view::drawHudOverviewPath(m,  pa | kSquadColors[g] );
								break;	// only the leader's path for the squad
							}
						}
					}
				}
			}

			renderOverviewEnemyMarkers( eye, markerA );
			if ( iconW > 0.01f )
				forceGroupBar.renderOverviewIcons( eye, iconW );
			ForceGroupBar::OverviewCardHit raw[32];
			int nh = ( cardViz > 0.01f )
			         ? forceGroupBar.renderOverviewSquadCards( eye, cardViz, raw, 32 ) : 0;
			TacticalOverview::CardHit hits[32];
			for ( int hI = 0; hI < nh; ++hI )
			{
				hits[hI].forceGroup = raw[hI].forceGroup;
				hits[hI].unit = raw[hI].unit;
				hits[hI].l = raw[hI].l; hits[hI].t = raw[hI].t;
				hits[hI].r = raw[hI].r; hits[hI].b = raw[hI].b;
			}
			g_tacticalOverview.setCardHits( std::span(hits).first(nh) );
			if ( markerA > 0.01f && mc2::render_view::hud().home )
				for (const auto& row : mc2::render_view::hud().objectives)
                    CObjective::RenderOverviewMarker(row, eye, markerA);
		}

		// slice C site 1 (render dispatch): registry under MC2_HUD_IMGUI_TABS,
		// legacy if/else chain byte-for-byte otherwise.
		if ( hudTabsGateOn() && hudTabRegistry.activeTab() )
		{
			const HudTabs::Tab* activeT = hudTabRegistry.activeTab();
			if ( activeT->render )
				activeT->render( this );
		}
		else if ( getButton( TACMAP_TAB )->state & ControlButton::PRESSED )
			tacMap.render();
		else if ( getButton( VEHICLE_TAB )->state & ControlButton::PRESSED )
			renderVehicleTab();
		else
			renderInfoTab();

		{
			ZoneScopedN("ControlGui::frame");
			if (frameScale != gos_GetHudScale() || frameWidth != Environment.screenWidth ||
			    frameHeight != Environment.screenHeight) bakeFrame();
			if (!frameAtlas) {
				for (int i = 0; i < staticCount; ++i) staticInfos[i].render();
			} else {
				const auto texture = mc2::textures::sourceId(frameAtlas->textureHandle);
				gos_SetRenderState(gos_State_Texture, texture);
				gos_SetRenderState(gos_State_Filter, gos_FilterNone);
				gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
				gos_SetRenderState(gos_State_Clipping, 1);
				if (frameSplit) gos_DrawQuads(frameVertices.data(), frameSplit);
				// The force-group overlay changes alpha at runtime; never bake it.
				const auto* cover = ForceGroupBar::s_coverIcon;
				if (cover && std::any_of(std::begin(cover->location),
				    std::end(cover->location), [](const auto& v) { return (v.argb >> 24) != 0; }))
					ForceGroupBar::s_coverIcon->render();
				gos_SetRenderState(gos_State_Texture, texture);
				if (static_cast<size_t>(frameSplit) < frameVertices.size())
					gos_DrawQuads(frameVertices.data() + frameSplit, static_cast<int>(frameVertices.size()) - frameSplit);
			}
		}

		forceGroupBar.render();



		renderHelpText();
		renderHoverTip();
		
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero );
		
		for (int i = 0; i < LAST_COMMAND; ++i )
			buttons[i].render();

		// want to draw the tab on top last
		// slice C site 2 (top-draw): under the gate, iterate the registry
		// (legacy-button tabs re-render their pressed button; buttonless tabs
		// get ImGui chrome). Legacy hardcoded +3 loop otherwise.
		if ( hudTabsGateOn() )
		{
			for ( int i = 0; i < hudTabRegistry.count(); i++ )
			{
				const int cmd = hudTabRegistry.at( i )->buttonCommand;
				if ( cmd >= 0 && ( getButton( cmd )->state & ControlButton::PRESSED ) )
					buttons[cmd].render();
			}
			renderHudTabChromeImGui();
		}
		else
		for (int i = TACMAP_TAB; i < TACMAP_TAB + 3; i++ )
		{
			if ( (getButton( i )->state) & ControlButton::PRESSED )
			{
				buttons[i].render();
			}
		}

		getButton( LAYMINES )->render();


		RenderObjectives();
	}

	if ( moviePlaying && bMovie)
	{
		// SMALLPANELS-IMGUI-SLICE-1: ImGui comm-video panel when gated on.
		// Gate OFF -> legacy below, byte-identical.
		if ( imguiVideoEnabled() )
			renderVideoImGui();
		else
		{
			for ( int i = 0; i < videoInfoCount; i++ )
				videoInfos[i].render();
			bMovie->render();
		}
	}

	if (drawGUIOn)
	{
		// draw the mission timer if we have one
		if ( mc2::render_view::hud().timeLimit > 0 )
		{
	
			float fTime = static_cast<float>(mc2::render_view::hud().timeLimit) - mc2::render_view::clock().scenarioTime;
			int time = static_cast<int>(fTime);
	
			if ( time <= 120  && !twoMinWarningPlayed )
			{
				twoMinWarningPlayed = true;
				soundSystem->playBettySample( BETTY_TWO_MIN_LEFT );
			}
			else if ( time <= 30 && !thirtySecondWarningPlayed )
			{
				thirtySecondWarningPlayed = true;
				soundSystem->playBettySample( BETTY_THIRTY_LEFT );
			}
			else if ( time < 0)
			{
				time = 0;
			}
		
			char buffer[32];
			unsigned long minutes = time/60;
			unsigned long seconds = time%60;
			mc2::fmt::to_buf(buffer, "{:03d} : {:02d}", mc2::fmt::as_i32(minutes), mc2::fmt::as_i32(seconds));
	
			unsigned long color = timerRect.color;
			if ( minutes < 5 )
			{
				color = 0xffc61c00;
				if ( minutes < 2 )
				{
					if ( seconds % 2 )
					{
						color = 0;
					}
				}
			}
	
			
			// SMALLPANELS-IMGUI-SLICE-1: ImGui timer panel when gated on. The
			// time/warning/blink computation above is shared -- only the draw
			// forks. Gate OFF -> legacy below, byte-identical.
			if ( imguiTimerEnabled() )
				renderTimerImGui( buffer, color );
			else
			{
				long width = timerFont.width( buffer );
				drawShadowText( color, 0x00000000, timerFont.getTempHandle(),
					timerRect.rect.left - width, timerRect.rect.top, true,
					buffer, 0, guiFont.getSize() );
	
				for (long i = 0; i < timerInfoCount; i++ )
					timerInfos[i].render();
			}
		}
	
		if ( bPaused )
			pauseWnd->render();
	
	
		if ((!MPlayer && (mc2::render_view::hud().objectiveCount != 0) && (mc2::render_view::hud().objectiveStatus != OS_UNDETERMINED))
			|| mc2::render_view::hud().ended)
		{
			if ( !resultsTime )
				resultsTime = .001f;
			renderStatusInfo = 1;
		}
	
		renderResults();
		renderChatText();
	
		//l XLocation = 80
		//l YLocation = 505
		//l Width = 41
		//l Height = 34
		//l Color = 0xff000000
		//b outline = FALSE
		
		// flash the rp tab if we just got kudos
		if ( LogisticsData::instance->rpJustAdded )
		{
			tabFlashTime += frameLength;
			if ( tabFlashTime > .5 )
			{
				tabFlashTime = 0;
			}
			else if ( tabFlashTime >  .25 )
			{
				float scaleX = Environment.screenWidth <= 1024 ? Environment.screenWidth : 1024;
				float scaleY = Environment.screenHeight <= 768 ? Environment.screenHeight : 768;
				GUI_RECT rect = { static_cast<long>((51 * scaleX/640.f + ControlGui::hiResOffsetX)), static_cast<long>((317 * scaleY/480.f + ControlGui::hiResOffsetY)),
								static_cast<long>((74 *scaleX/640.f + ControlGui::hiResOffsetX)), static_cast<long>((337 * scaleY/480.f  + ControlGui::hiResOffsetY))};
				drawRect( rect, 0xff000000 );
			}
		}
	}
}
