#define FORCEGROUPBAR_CPP
/*************************************************************************************************\
ForceGroupBar.cpp			: Implementation of the ForceGroupBar component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"forcegroupbar.h"
#include"mechicon.h"
#include"objmgr.h"
#include"team.h"
#include"missiongui.h"
#include"controlgui.h"
#include "../resource.h"
#include"multplyr.h"
#include"mc2movie.h"
#include"comndr.h"
#include"prefs.h"
#include"gamesound.h"
#include"gamecam.h"   // Camera::projectForScreenXY for the overview icon overlay
#include"warrior.h"   // pilot order state for the squad status indicator
#include"tacordr.h"

// Squad status: one short word for a mech's current activity, and a rank so a
// squad card can show the highest-priority member status.
static const char* moverStatusStr( Mover* m )
{
	if ( !m ) return "IDLE";
	MechWarrior* w = m->getPilot();
	if ( !w ) return "IDLE";
	if ( w->getCurrentTarget() ) return "ENGAGING";
	TacticalOrderPtr o = w->getCurTacOrder();
	if ( !o ) return "IDLE";
	switch ( o->code )
	{
		case TACTICAL_ORDER_MOVETO_POINT:
		case TACTICAL_ORDER_MOVETO_OBJECT:
		case TACTICAL_ORDER_JUMPTO_POINT:
		case TACTICAL_ORDER_JUMPTO_OBJECT:
		case TACTICAL_ORDER_TRAVERSE_PATH:
		case TACTICAL_ORDER_FOLLOW:
		case TACTICAL_ORDER_ESCORT:        return "MOVING";
		case TACTICAL_ORDER_PATROL_PATH:   return "PATROL";
		case TACTICAL_ORDER_GUARD:         return "DEFENDING";
		case TACTICAL_ORDER_ATTACK_OBJECT:
		case TACTICAL_ORDER_ATTACK_POINT:  return "ENGAGING";
		case TACTICAL_ORDER_WITHDRAW:      return "WITHDRAW";
		default:                           return "IDLE";
	}
}

static int statusRank( const char* s )
{
	if ( !strcmp( s, "ENGAGING" ) )  return 5;
	if ( !strcmp( s, "WITHDRAW" ) )  return 4;
	if ( !strcmp( s, "MOVING" ) )    return 3;
	if ( !strcmp( s, "PATROL" ) )    return 2;
	if ( !strcmp( s, "DEFENDING" ) ) return 1;
	return 0; // IDLE
}

// Color for a status word (ARGB without alpha).
static unsigned long statusColor( const char* s )
{
	if ( !strcmp( s, "ENGAGING" ) )  return 0x00ff5050;
	if ( !strcmp( s, "WITHDRAW" ) )  return 0x00ff9b4b;
	if ( !strcmp( s, "MOVING" ) )    return 0x005fd2ff;
	if ( !strcmp( s, "PATROL" ) )    return 0x00d2b04b;
	if ( !strcmp( s, "DEFENDING" ) ) return 0x009bff9b;
	return 0x00a9c3d2; // IDLE — muted
}

// Draw one source cell of the force-bar icon atlas into a screen rect, using a
// LOCAL vertex buffer (never ForceGroupIcon::bmpLocation — touching that shared
// HUD buffer makes the real force-bar icons draw in the wrong place). Replicates
// the UV math from ForceGroupIcon::renderUnitIcon / renderUnitIconBack.
static void drawOverviewIconCell( int xIndex, int yIndex, unsigned long texHandle,
                                  float uIX, float uIY, float texW, float texH,
                                  float l, float t, float r, float b, unsigned long argb )
{
	const float u  = xIndex * uIX / texW + (0.1f / 256.f);
	const float v  = yIndex * uIY / texH + (0.1f / 256.f);
	const float uD = uIX / texW + (0.1f / 256.f);
	const float vD = uIY / texH + (0.1f / 256.f);

	gos_SetRenderState( gos_State_Texture, texHandle );
	// Additive blend keys out the icon's black background (black adds nothing);
	// the mech silhouette adds onto the scene. Vertex RGB scales the texel, so
	// the caller fades by scaling argb's RGB channels.
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneOne );
	gos_SetRenderState( gos_State_Filter, gos_FilterNone );
	gos_SetRenderState( gos_State_AlphaTest, false );

	gos_VERTEX vv[5];
	for ( int k = 0; k < 5; ++k ) { vv[k].argb = argb; vv[k].frgb = 0; vv[k].z = 0; vv[k].rhw = .5f; }
	vv[0].u = vv[1].u = u;        vv[2].u = vv[3].u = u + uD;
	vv[0].v = vv[3].v = v;        vv[1].v = vv[2].v = v + vD;
	vv[0].x = vv[1].x = l;        vv[2].x = vv[3].x = r;
	vv[0].y = vv[3].y = t;        vv[1].y = vv[2].y = b;
	vv[4] = vv[0];
	gos_DrawTriangles( vv, 3 );
	gos_DrawTriangles( &vv[2], 3 );
}

void ForceGroupBar::renderOverviewIcons( Camera* eye, float alpha )
{
	if ( !eye || alpha <= 0.0f || !ForceGroupIcon::s_textureMemory )
		return;

	// Collect visible units: true projected point (tx,ty) + working chip center.
	struct OvChip { ForceGroupIcon* ic; float tx, ty, cx, cy; };
	OvChip chip[MAX_ICONS];
	int n = 0;
	for ( int i = 0; i < iconCount && n < MAX_ICONS; i++ )
	{
		ForceGroupIcon* ic = icons[i];
		if ( !ic || !ic->unit ) continue;
		if ( ic->unit->isDestroyed() || ic->unit->isDisabled() ) continue;
		Stuff::Vector3D wp = ic->unit->getPosition();
		// Project through the SAME GL clip matrix the GPU renders the mechs with
		// (worldToClipGL). The default projectForScreenXY routes through the
		// legacy projectZ/cameraToClip path, which disagrees with the GL render
		// under camera motion (D3D<->GL split-brain) and made the icons lag/tear
		// while panning. projectModernClipGL + manual viewport remap stays locked
		// to the rendered units.
		ModernClipResult r = eye->projectModernClipGL( wp );
		if ( !r.admit || r.clip.w <= 0.05f ) continue;	// behind near plane / outside frustum
		float vmx, vmy, vax, vay;
		gos_GetViewport( &vmx, &vmy, &vax, &vay );
		float ndcX = r.clip.x / r.clip.w;
		float ndcY = r.clip.y / r.clip.w;
		float sx = vax + ( ndcX * 0.5f + 0.5f ) * vmx;
		float sy = vay + ( 1.0f - ( ndcY * 0.5f + 0.5f ) ) * vmy;
		chip[n].ic = ic; chip[n].tx = chip[n].cx = sx; chip[n].ty = chip[n].cy = sy;
		n++;
	}
	if ( n == 0 ) return;

	// De-overlap: iteratively push apart chips whose rects collide. Resolve along
	// the axis of smaller penetration so chips slide minimally off their units.
	const float chipW = 36.0f, chipH = 32.0f;
	const float sepX = chipW + 6.0f, sepY = chipH + 14.0f; // extra Y for the label
	for ( int pass = 0; pass < 16; ++pass )
		for ( int a = 0; a < n; a++ )
			for ( int b = a + 1; b < n; b++ )
			{
				float dx = chip[b].cx - chip[a].cx, dy = chip[b].cy - chip[a].cy;
				float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
				float ox = sepX - adx, oy = sepY - ady;
				if ( ox > 0 && oy > 0 )
				{
					if ( ox < oy ) { float p = (ox * 0.5f + 0.5f) * (dx < 0 ? -1.f : 1.f); chip[a].cx -= p; chip[b].cx += p; }
					else           { float p = (oy * 0.5f + 0.5f) * (dy < 0 ? -1.f : 1.f); chip[a].cy -= p; chip[b].cy += p; }
				}
			}

	unsigned long aByte = (unsigned long)(alpha * 255.0f);
	if ( aByte > 255 ) aByte = 255;
	const unsigned long aBits = aByte << 24;

	// These icons are world-anchored (true screen positions from the GL
	// projection), NOT bottom-band HUD chrome — exempt them from the s_hud_scale
	// shrink that flushHUDBatch applies below 0.6*height. Without this, icons in
	// the lower screen jump/scale at that boundary while panning (same HUD-fit
	// boundary the cursor/dialogs are exempted from).
	const bool prevHudExempt = gos_GetHudScaleExempt();
	gos_SetHudScaleExempt( true );
	// UI-ASPECT-ANCHOR-1: shrink-exempt but CANVAS-REMAPPED — this bar hit-tests
	// via getMouseHudX, so it must live on the 16:9 canvas with the rest of the
	// HUD chrome.
	int prevCanvasMode = gos_GetHudCanvasExemptMode();
	gos_SetHudCanvasExemptMode( 0 );

	// Leader lines from the true unit position to displaced chips.
	gos_SetRenderState( gos_State_Texture, 0 );
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_AlphaTest, 0 );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_ZWrite, 0 );
	for ( int i = 0; i < n; i++ )
	{
		float ddx = chip[i].cx - chip[i].tx, ddy = chip[i].cy - chip[i].ty;
		if ( ddx * ddx + ddy * ddy < 16.0f ) continue;	// not displaced
		unsigned long lc = aBits | ( chip[i].ic->unit->getSelected() ? 0x004bff4b : 0x0000aa00 );
		gos_VERTEX ln[2];
		for ( int k = 0; k < 2; ++k ) { ln[k].z = 0; ln[k].rhw = .5f; ln[k].argb = lc; ln[k].frgb = 0; ln[k].u = ln[k].v = 0; }
		ln[0].x = chip[i].tx; ln[0].y = chip[i].ty;
		ln[1].x = chip[i].cx; ln[1].y = chip[i].cy + chipH * 0.5f;
		gos_DrawLines( ln, 2 );
	}

	// Icon chips (back frame + unit icon) replicated into local verts.
	const float texW = (float)ForceGroupIcon::s_textureMemory->width;
	const float texH = (float)ForceGroupIcon::s_textureMemory->height;
	const float uIX  = ForceGroupIcon::unitIconX;
	const float uIY  = ForceGroupIcon::unitIconY;
	const int perLine     = (int)( texW / uIX );
	const int iconsPerPage = (int)( texH / uIY );  // rows per page: texture HEIGHT / cell height
	// Additive: RGB scales the texel, so encode the cross-fade in RGB (not alpha).
	const unsigned long iconArgb = 0xff000000 | (aByte << 16) | (aByte << 8) | aByte;

	HGOSFONT3D font = ForceGroupIcon::gosFontHandle ? ForceGroupIcon::gosFontHandle->getTempHandle() : 0;

	for ( int i = 0; i < n; i++ )
	{
		const float l = chip[i].cx - chipW * 0.5f, r = chip[i].cx + chipW * 0.5f;
		const float t = chip[i].cy - chipH * 0.5f, b = chip[i].cy + chipH * 0.5f;

		// Back frame omitted: it is the solid black box. With additive blending
		// the black would be invisible anyway, and skipping it keeps the overlay
		// clean (just the mech silhouette over the terrain).

		// Unit icon (may live on a later atlas page).
		int di = chip[i].ic->damageIconIndex;
		int yIndex = di / perLine, texIndex = 0;
		if ( yIndex >= iconsPerPage ) { texIndex = yIndex / iconsPerPage; yIndex = yIndex % iconsPerPage; }
		drawOverviewIconCell( di % perLine, yIndex, ForceGroupIcon::s_textureHandle[texIndex],
		                      uIX, uIY, texW, texH, l, t, r, b, iconArgb );

		// Short name label under the chip (de-overlapped already).
		const char* nm = chip[i].ic->unit->getName();
		if ( font && nm && nm[0] )
		{
			const int lx = (int)( chip[i].cx - chipW * 0.5f );
			const int ly = (int)( b + 1.0f );
			gos_TextSetAttributes( font, aBits | 0x00000000, 1.0f, false, true, false, false );
			gos_TextSetPosition( lx + 1, ly + 1 );
			gos_TextDraw( nm );
			gos_TextSetAttributes( font, aBits | 0x00ffffff, 1.0f, false, true, false, false );
			gos_TextSetPosition( lx, ly );
			gos_TextDraw( nm );
		}
	}

	gos_SetHudCanvasExemptMode( prevCanvasMode );
	gos_SetHudScaleExempt( prevHudExempt );
}

int ForceGroupBar::renderOverviewSquadCards( Camera* eye, float alpha,
                                             OverviewCardHit* hitsOut, int maxHits )
{
	if ( !eye || alpha <= 0.0f || !ForceGroupIcon::s_textureMemory )
		return 0;

	const int MAXG = 10;

	// Project a unit to screen; returns false if off/behind.
	float vmx, vmy, vax, vay;
	gos_GetViewport( &vmx, &vmy, &vax, &vay );

	// One card per non-empty force group; ungrouped units each get a singleton.
	struct Card { int fg; int mem[MAX_ICONS]; int nm; float cx, cy; float w, h; };
	Card cards[MAXG + MAX_ICONS];
	int nCards = 0;

	// Group buckets.
	int gMem[MAXG][MAX_ICONS]; int gCnt[MAXG];
	for ( int g = 0; g < MAXG; ++g ) gCnt[g] = 0;
	int singles[MAX_ICONS]; int nS = 0;

	for ( int i = 0; i < iconCount; i++ )
	{
		ForceGroupIcon* ic = icons[i];
		if ( !ic || !ic->unit ) continue;
		if ( ic->unit->isDestroyed() || ic->unit->isDisabled() ) continue;
		int g = -1;
		for ( int j = 0; j < MAXG; ++j ) if ( ic->unit->isInUnitGroup( j ) ) { g = j; break; }
		if ( g >= 0 ) gMem[g][gCnt[g]++] = i;
		else singles[nS++] = i;
	}

	// Build cards (centroid of projected member positions).
	for ( int g = 0; g < MAXG; ++g )
	{
		if ( gCnt[g] == 0 ) continue;
		float sx = 0, sy = 0; int proj = 0; int nm = 0;
		Card& c = cards[nCards];
		c.fg = g;
		for ( int k = 0; k < gCnt[g]; ++k )
		{
			int ii = gMem[g][k];
			c.mem[nm++] = ii;
			ModernClipResult r = eye->projectModernClipGL( icons[ii]->unit->getPosition() );
			if ( r.admit && r.clip.w > 0.05f )
			{
				sx += vax + ( r.clip.x / r.clip.w * 0.5f + 0.5f ) * vmx;
				sy += vay + ( 1.0f - ( r.clip.y / r.clip.w * 0.5f + 0.5f ) ) * vmy;
				proj++;
			}
		}
		if ( proj == 0 ) continue;
		c.nm = nm; c.cx = sx / proj; c.cy = sy / proj;
		nCards++;
	}
	for ( int s = 0; s < nS; ++s )
	{
		int ii = singles[s];
		ModernClipResult r = eye->projectModernClipGL( icons[ii]->unit->getPosition() );
		if ( !r.admit || r.clip.w <= 0.05f ) continue;
		Card& c = cards[nCards];
		c.fg = -1; c.nm = 1; c.mem[0] = ii;
		c.cx = vax + ( r.clip.x / r.clip.w * 0.5f + 0.5f ) * vmx;
		c.cy = vay + ( 1.0f - ( r.clip.y / r.clip.w * 0.5f + 0.5f ) ) * vmy;
		nCards++;
	}
	if ( nCards == 0 ) return 0;

	// Card box sizes + de-overlap.
	const float cellW = 30.0f, cellH = 26.0f, pad = 6.0f, titleH = 12.0f;
	for ( int i = 0; i < nCards; i++ )
	{
		float iconRow = cards[i].nm * cellW + ( cards[i].nm - 1 ) * 2;
		float wNeeded = iconRow > 72.0f ? iconRow : 72.0f;	// fit "SQUAD N" + status
		cards[i].w = pad * 2 + wNeeded;
		cards[i].h = pad * 2 + titleH + cellH + titleH;		// title + icons + status row
	}
	for ( int pass = 0; pass < 16; ++pass )
		for ( int a = 0; a < nCards; a++ )
			for ( int b = a + 1; b < nCards; b++ )
			{
				float dx = cards[b].cx - cards[a].cx, dy = cards[b].cy - cards[a].cy;
				float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
				float sepX = ( cards[a].w + cards[b].w ) * 0.5f + 6.0f;
				float sepY = ( cards[a].h + cards[b].h ) * 0.5f + 6.0f;
				float ox = sepX - adx, oy = sepY - ady;
				if ( ox > 0 && oy > 0 )
				{
					if ( ox < oy ) { float p = ( ox * 0.5f + 0.5f ) * ( dx < 0 ? -1.f : 1.f ); cards[a].cx -= p; cards[b].cx += p; }
					else           { float p = ( oy * 0.5f + 0.5f ) * ( dy < 0 ? -1.f : 1.f ); cards[a].cy -= p; cards[b].cy += p; }
				}
			}

	unsigned long aB = (unsigned long)( alpha * 255.0f ); if ( aB > 255 ) aB = 255;
	const unsigned long aBits = aB << 24;
	const float texW = (float)ForceGroupIcon::s_textureMemory->width;
	const float texH = (float)ForceGroupIcon::s_textureMemory->height;
	const float uIX = ForceGroupIcon::unitIconX, uIY = ForceGroupIcon::unitIconY;
	const int perLine = (int)( texW / uIX ), iconsPerPage = (int)( texH / uIY );  // rows: texH/cellH
	HGOSFONT3D font = ForceGroupIcon::gosFontHandle ? ForceGroupIcon::gosFontHandle->getTempHandle() : 0;
	const unsigned long iconArgb = 0xff000000 | ( aB << 16 ) | ( aB << 8 ) | aB;

	bool prevExempt = gos_GetHudScaleExempt();
	gos_SetHudScaleExempt( true );
	// UI-ASPECT-ANCHOR-1: shrink-exempt but CANVAS-REMAPPED — this bar hit-tests
	// via getMouseHudX, so it must live on the 16:9 canvas with the rest of the
	// HUD chrome.
	int prevCanvasMode = gos_GetHudCanvasExemptMode();
	gos_SetHudCanvasExemptMode( 0 );

	int nHits = 0;
	for ( int i = 0; i < nCards; i++ )
	{
		float l = cards[i].cx - cards[i].w * 0.5f, r = cards[i].cx + cards[i].w * 0.5f;
		float t = cards[i].cy - cards[i].h * 0.5f, b = cards[i].cy + cards[i].h * 0.5f;

		// Translucent dark-teal panel.
		gos_SetRenderState( gos_State_Texture, 0 );
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
		gos_SetRenderState( gos_State_AlphaTest, 0 );
		gos_SetRenderState( gos_State_ZCompare, 0 );
		gos_SetRenderState( gos_State_ZWrite, 0 );
		unsigned long panel = ( (unsigned long)( alpha * 0.70f * 255.0f ) << 24 ) | 0x000c2230;
		gos_VERTEX q[4];
		for ( int v = 0; v < 4; ++v ) { q[v].z = 0; q[v].rhw = .5f; q[v].argb = panel; q[v].frgb = 0; q[v].u = q[v].v = 0; }
		q[0].x = l; q[0].y = t; q[1].x = l; q[1].y = b; q[2].x = r; q[2].y = b; q[3].x = r; q[3].y = t;
		gos_DrawQuads( q, 4 );

		// Bright border.
		unsigned long border = aBits | 0x005fa2c2;
		gos_VERTEX ln[2];
		float bx[5] = { l, r, r, l, l }, by[5] = { t, t, b, b, t };
		for ( int e = 0; e < 4; ++e )
		{
			ln[0].x = bx[e];   ln[0].y = by[e];
			ln[1].x = bx[e+1]; ln[1].y = by[e+1];
			for ( int v = 0; v < 2; ++v ) { ln[v].z = 0; ln[v].rhw = .5f; ln[v].argb = border; ln[v].frgb = 0; ln[v].u = ln[v].v = 0; }
			gos_DrawLines( ln, 2 );
		}

		// Title.
		if ( font )
		{
			char title[24];
			if ( cards[i].fg >= 0 ) sprintf( title, "SQUAD %d", cards[i].fg );
			else { const char* nm = icons[cards[i].mem[0]]->unit->getName(); strncpy( title, nm ? nm : "UNIT", 20 ); title[20] = 0; }
			gos_TextSetAttributes( font, aBits | 0x00eaf6ff, 1.0f, false, true, false, false );
			gos_TextSetPosition( (int)( l + pad ), (int)( t + 2 ) );
			gos_TextDraw( title );

			// Squad status (highest-priority member activity), bottom row.
			const char* best = "IDLE"; int bestRank = -1;
			for ( int m = 0; m < cards[i].nm; m++ )
			{
				const char* s = moverStatusStr( icons[cards[i].mem[m]]->unit );
				int rk = statusRank( s );
				if ( rk > bestRank ) { bestRank = rk; best = s; }
			}
			gos_TextSetAttributes( font, aBits | statusColor( best ), 1.0f, false, true, false, false );
			gos_TextSetPosition( (int)( l + pad ), (int)( b - titleH - 1 ) );
			gos_TextDraw( best );
		}

		// Member icons tiled (additive, keys out black bg).
		float ix = l + pad, iy = t + pad + titleH;
		for ( int m = 0; m < cards[i].nm; m++ )
		{
			ForceGroupIcon* ic = icons[cards[i].mem[m]];
			int di = ic->damageIconIndex, yIndex = di / perLine, texIndex = 0;
			if ( yIndex >= iconsPerPage ) { texIndex = yIndex / iconsPerPage; yIndex = yIndex % iconsPerPage; }
			drawOverviewIconCell( di % perLine, yIndex, ForceGroupIcon::s_textureHandle[texIndex],
			                      uIX, uIY, texW, texH, ix, iy, ix + cellW, iy + cellH, iconArgb );
			ix += cellW + 2;
		}

		if ( hitsOut && nHits < maxHits )
		{
			hitsOut[nHits].forceGroup = cards[i].fg;
			hitsOut[nHits].unit = ( cards[i].fg < 0 ) ? (void*)icons[cards[i].mem[0]]->unit : 0;
			hitsOut[nHits].l = l; hitsOut[nHits].t = t; hitsOut[nHits].r = r; hitsOut[nHits].b = b;
			nHits++;
		}
	}

	gos_SetHudCanvasExemptMode( prevCanvasMode );
	gos_SetHudScaleExempt( prevExempt );
	return nHits;
}

float ForceGroupBar::iconWidth = 48;
float ForceGroupBar::iconHeight = 42;
int	  ForceGroupBar::iconsPerRow = 8;

StaticInfo*  ForceGroupBar::s_coverIcon = NULL;

extern bool useLeftRightMouseProfile;
extern char CDInstallPath[];
void EnterWindowMode();
void EnterFullScreenMode();
void __stdcall ExitGameOS();

#define BOTTOM_OFFSET	5 * Environment.screenHeight/640.f

#define FORCEGROUP_LEFT		ForceGroupIcon::selectionRect[0].left
#define FORCEGROUP_WIDTH	(ForceGroupIcon::selectionRect[7].right - ForceGroupIcon::selectionRect[0].left)
#define FORCEGROUP_HEIGHT	(ForceGroupIcon::selectionRect[0].bottom - ForceGroupIcon::selectionRect[0].top)

extern float frameRate;
 
ForceGroupBar::ForceGroupBar()
{
	for ( int i = 0; i < MAX_ICONS; ++i )
	{
		icons[i] = 0;
	}

	iconCount = 0;

	forceNumFlashes = 0;
	forceFlashTime = 0.0f;
}

ForceGroupBar::~ForceGroupBar()
{
	removeAll();
	
	if ( ForceGroupIcon::gosFontHandle )
		delete ForceGroupIcon::gosFontHandle;

	ForceGroupIcon::gosFontHandle  = NULL;
}


bool ForceGroupBar::flashJumpers (long numFlashes)
{
	forceNumFlashes = numFlashes;
	forceFlashTime = 0.0f;

	return true;
}

bool ForceGroupBar::addMech( Mover* pMover )
{
	if ( iconCount >= MAX_ICONS )
	{
		gosASSERT( false );
		return 0;
	}
	
	MechIcon* pIcon = new MechIcon;
	bool bRetVal = pIcon->init( pMover );

	icons[iconCount++] = pIcon;
	
	return bRetVal;
}

bool ForceGroupBar::addVehicle( Mover* pMover )
{
	if ( iconCount >= MAX_ICONS )
	{
		gosASSERT( false );
		return 0;
	}
	
	VehicleIcon* pIcon = new VehicleIcon;
	bool bRetVal = pIcon->init( pMover );

	icons[iconCount++] = pIcon;

	return bRetVal;
}

void ForceGroupBar::removeMover (Mover* mover) {

	for (long i = 0; i < iconCount; i++)
		if (icons[i]->unit == mover) {
			delete icons[i];
			iconCount --;
			memmove( &icons[i], &icons[i] + 1, (iconCount - i) * sizeof (ForceGroupIcon*) );
			icons[iconCount] = 0;
			break;
		}
}

void ForceGroupBar::update( )
{
	bool bSelect = userInput->isLeftClick();
	bool bCommand = useLeftRightMouseProfile ? userInput->isRightClick() : userInput->isLeftClick();
	bool shiftDn = userInput->getKeyDown( KEY_LSHIFT ) ? true : false;
	bool bCamera = useLeftRightMouseProfile ? (userInput->isLeftDoubleClick()) : (userInput->isRightClick() && !userInput->isRightDrag());
	bool bForceGroup = useLeftRightMouseProfile ? (userInput->isLeftDoubleClick()) : userInput->isLeftDoubleClick();
	
	if ( bCamera )
		bSelect = 0;

	Stuff::Vector2DOf<long> screen;
	// Force-group icons draw shrunk by s_hud_scale -> hit-test in HUD-inverse
	// space so clicks land on the drawn-inward icons. (No-op when scale off.)
	screen.x = userInput->getMouseHudX();
	screen.y = userInput->getMouseHudY();

	 if ( screen.x > FORCEGROUP_LEFT && screen.x < FORCEGROUP_LEFT + FORCEGROUP_WIDTH
		  && screen.y > FORCEGROUP_TOP )
	 {
		 if ( ControlGui::instance->isSelectingInfoObject() )
			userInput->setMouseCursor( mState_INFO );
		 else if ( ControlGui::instance->getRepair() )
			 userInput->setMouseCursor( mState_XREPAIR );
		 else if ( MissionInterfaceManager::instance()->hotKeyIsPressed( EJECT_COMMAND_INDEX ) )
			 userInput->setMouseCursor( mState_EJECT );
		 else
			userInput->setMouseCursor( mState_NORMAL );

		helpTextID = IDS_FORCEGROUP_BAR_DESC;
		helpTextHeaderID = IDS_FORCEGROUP_BAR;
	 }



	// unselect all if appropriate
	if ( bSelect && !shiftDn && inRegion(screen.x, screen.y) 
		&& !ControlGui::instance->isSelectingInfoObject() && (!ControlGui::instance->getRepair()
		&& !MissionInterfaceManager::instance()->hotKeyIsPressed( EJECT_COMMAND_INDEX )
		&& !ControlGui::instance->getGuard()
		|| useLeftRightMouseProfile) )
	{
		Team* pTeam = Team::home;
		for ( int i = 0; i < pTeam->rosterSize; ++i )
		{
			Mover* pMover = (Mover*)pTeam->getMover( i );
			if (pMover->getCommander()->getId() == Commander::home->getId())
			{
				pMover->setSelected( false );
			}
		}
	}

	
	// remove dead mechs
	for ( int t = 0; t < iconCount; ++t )
	{
		if ( (icons[t]->unit->isDestroyed() || icons[t]->unit->isDisabled()) && !icons[t]->unit->recoverBuddyWID )
		{
			if ( !icons[t]->isAnimatingDeath() )
				icons[t]->beginDeathAnimation();
			if ( icons[t]->deathAnimationOver() || icons[t]->unit->causeOfDeath == POWER_USED_UP )
			{
				delete icons[t];
				iconCount --;
				memmove( &icons[t], &icons[t] + 1, (iconCount - t) * sizeof (ForceGroupIcon*) );
				icons[iconCount] = 0;
			}
		}
	}

	qsort( icons, iconCount, sizeof( ForceGroupIcon* ), ForceGroupIcon::sort );

	for ( int i = 0; i < iconCount; i++ )
	{
		icons[i]->setLocationIndex( i );
	}
	

	for (int i = 0; i < iconCount; ++i )
	{
		if ( icons[i]->inRegion( screen.x, screen.y ) )
		{
			icons[i]->unit->setTargeted(true); 
			if ( ControlGui::instance->getRepair() )
			{
				if ( !MissionInterfaceManager::instance()->canRepair(icons[i]->unit ) )
				{
					userInput->setMouseCursor( mState_XREPAIR );
				
					// need to go back and unselect everything
					if ( bSelect  )
					{
						if ( !shiftDn )
						{
							Team* pTeam = Team::home;
							for ( int j = 0; j < pTeam->rosterSize; ++j )
							{
								Mover* pMover = (Mover*)pTeam->getMover( j );
								if (pMover->getCommander()->getId() == Commander::home->getId())
								{
									pMover->setSelected( false );
								}
							}
						}						
					}
				}
				else
				{
					userInput->setMouseCursor( mState_REPAIR );
				}
			}
			else if ( ControlGui::instance->getGuard() )
			{
				userInput->setMouseCursor( mState_GUARD );
			}
			else
			{
				ControlGui::instance->setRolloverHelpText( IDS_UNIT_SELECT_HELP );
			}

			if ( bSelect && !ControlGui::instance->infoButtonPressed() )
			{
				if ( !(ControlGui::instance->getRepair() && MissionInterfaceManager::instance()->canRepair(icons[i]->unit ) && !useLeftRightMouseProfile) )
					icons[i]->click( shiftDn ); 

				ControlGui::instance->setInfoWndMover( icons[i]->unit );	
			}

			if ( bCommand )
			{
				 if ( MissionInterfaceManager::instance()->hotKeyIsPressed( EJECT_COMMAND_INDEX ) )
				 {
					 MissionInterfaceManager::instance()->doEject( icons[i]->unit );
				 }
				 else if ( ControlGui::instance->getGuard() )
				 {
					 MissionInterfaceManager::instance()->doGuard( icons[i]->unit );
				 }
				 else if ( ControlGui::instance->getRepair() )
				 {
					 if ( MissionInterfaceManager::instance()->canRepair(icons[i]->unit ) )
						MissionInterfaceManager::instance()->doRepair( icons[i]->unit );
				 }
				 

				 else
					 ControlGui::instance->setInfoWndMover( icons[i]->unit );	
			}

			if ( bCamera )
			{
				icons[i]->rightClick();
			}

			if ( bForceGroup )
			{
				for( int j = 0; j < 10; ++j )
				{
					if ( icons[i]->unit->isInUnitGroup( j ) )
					{
						
						MissionInterfaceManager::selectForceGroup( j, true );

					}
				}				
			}
		
		}
		else
			icons[i]->unit->setTargeted( 0 );
		
		icons[i]->update();
		
	}

}

bool ForceGroupBar::inRegion( int x, int y )
{
	for ( int i = 0; i < iconCount; ++i )
	{
		if ( icons[i]->inRegion( x, y ) )
			return true;
	}

	return false;

}

void ForceGroupBar::render()
{	
	s_coverIcon->setColor( 0 );
		
	int maxUnits = 16;

	if ( MPlayer )
	{
		long playerCount;
		MPlayer->getPlayers( playerCount );
		if (playerCount)
			maxUnits = (MAX_MULTIPLAYER_MECHS_IN_LOGISTICS/playerCount) + 4;
		else
			maxUnits = 0;


		if ( maxUnits > 16 )
			maxUnits = 16;
	}


	for ( int i = 0; i < MAX_ICONS; i++ )
	{
		if (forceNumFlashes && icons[i] && icons[i]->unit->canJump())
		{
			if ( forceFlashTime > .25f )
			{
				if ( icons[i])
					icons[i]->render();
			}
		}
		else
		{
			if ( icons[i] )
				icons[i]->render();
		}

		if ( i >= maxUnits )
		{
			if ( s_coverIcon )
			{
				s_coverIcon->setLocation( ForceGroupIcon::selectionRect[i].left, ForceGroupIcon::selectionRect[i].top );
				s_coverIcon->setColor( 0xffffffff );
				s_coverIcon->render();
				s_coverIcon->setColor( 0 );
			}
		}
	}

	if (forceNumFlashes)
	{
		forceFlashTime += frameLength;
		if ( forceFlashTime > .5f )
		{
			forceFlashTime = 0.0f;
			forceNumFlashes--;
		}
	}
}

void ForceGroupBar::removeAll()
{
	for ( int i = 0; i < iconCount; i++ )
	{
		if ( icons[i] )
			delete icons[i];

		icons[i] = NULL;
	}

	iconCount = 0;
}

void ForceGroupBar::init( FitIniFile& file, StaticInfo* pCoverIcon )
{

	if ( NO_ERR != file.seekBlock( "Fonts" ) )
		Assert( 0, 0, "couldn't find the font block" );

	if ( !ForceGroupIcon::gosFontHandle )
		ForceGroupIcon::gosFontHandle = new aFont;

	long fontID;
	file.readIdLong( "IconFont", fontID );
	ForceGroupIcon::gosFontHandle->init( fontID );

	
	swapResolutions();

	for ( int i = 0; i < 16; i++ )
		ForceGroupIcon::init( file, i );

	s_coverIcon = pCoverIcon;
}

void ForceGroupBar::swapResolutions()
{
	ForceGroupIcon::resetResolution(0);
	
	for ( int i = 0; i < iconCount; i++ )
		icons[i]->swapResolutions(0);

	
}

bool ForceGroupBar::setPilotVideo( const char* pVideo, MechWarrior* pPilot )
{
	if ( !pVideo  )
	{
		if ( ForceGroupIcon::bMovie )
		{
			delete ForceGroupIcon::bMovie;
			ForceGroupIcon::bMovie = NULL;
			
		}
		else if ( ForceGroupIcon::pilotVideoTexture )
			gos_DestroyTexture( ForceGroupIcon::pilotVideoTexture );
		
		ForceGroupIcon::pilotVideoTexture = 0;
		ForceGroupIcon::pilotVideoPilot = 0;
	}

	else if  (ForceGroupIcon::bMovie || ControlGui::instance->isMoviePlaying()
		|| ForceGroupIcon::pilotVideoTexture || !prefs.pilotVideos)
	{
		// one already playing...
		// OR we don't want them playing.
		return 0;
	}

	else
	{
		for ( int i = 0; i < iconCount; i++ )
		{
			if ( icons[i] && icons[i]->unit->getPilot() == pPilot )
			{
				ForceGroupIcon::pilotVideoPilot = pPilot;
				FullPathFileName aviPath;
				aviPath.init( moviePath, pVideo, ".bik" );

				if ( (frameRate > 15.0) && fileExists(aviPath) && prefs.pilotVideos) // This is about correct.  Slower then this and movie has hard time keeping up!
				{
					//Update the RECT every frame.  What if we shift Icons around cause someone died!!
					RECT vRect;
					vRect.left 		= icons[i]->bmpLocation[icons[i]->locationIndex][1].x;
					vRect.right 	= icons[i]->pilotLocation[icons[i]->locationIndex][2];
					vRect.top 		= icons[i]->bmpLocation[icons[i]->locationIndex][3].y;
					vRect.bottom 	= icons[i]->bmpLocation[icons[i]->locationIndex][1].y;

					ForceGroupIcon::bMovie = new MC2Movie;
					ForceGroupIcon::bMovie->init(aviPath,vRect,true);
				}
				else // make a still texture
				{
					char realPilotName[9];

					//Set everything to zero so the strncpy below doesn't go off into LALA land!!
					memset(realPilotName,0,9);
					strncpy(realPilotName,pPilot->getName(),8);

					FullPathFileName path;
					path.init( moviePath, realPilotName, ".tga" );

					if (fileExists(path))
						ForceGroupIcon::pilotVideoTexture = gos_NewTextureFromFile( gos_Texture_Solid, path, 0 );
					else
					{
						char realMovieName[256];
						char realMoviePath[1024];
						_splitpath(path,NULL,realMoviePath,realMovieName,NULL);

						//Not in main installed directory and not in fastfile.  Look on CD.
						char actualPath[2048];
						strcpy(actualPath,CDInstallPath);
						strcat(actualPath,realMoviePath);
						strcat(actualPath,realMovieName);
						strcat(actualPath,".tga");

						bool fileThere = fileExists(actualPath);
						if (fileThere)
							ForceGroupIcon::pilotVideoTexture = gos_NewTextureFromFile( gos_Texture_Solid, actualPath, 0 );

						if (!fileThere && !Environment.checkCDForFiles)
							return 0;  // missing pilot video — skip quietly (disk install)

						bool openFailed = false;
						while (!fileThere)
						{
							openFailed = true;
							EnterWindowMode();

							char data[2048];
							char msg[1024];
							char msg1[512];
							char title[256];
							cLoadString(IDS_MC2_movieMISSING,msg1,511);
							cLoadString(IDS_MC2_CDMISSING,msg,1023);
							cLoadString(IDS_MC2_MISSING_TITLE,title,255);
							sprintf(data,msg1,(const char*)path,msg);
							DWORD result = MessageBox(NULL,data,title,MB_OKCANCEL | MB_ICONWARNING);
							if (result == IDCANCEL)
							{
								ExitGameOS();
								return 1; 
							}

							fileThere = fileExists(actualPath);
							if (fileThere)
								ForceGroupIcon::pilotVideoTexture = gos_NewTextureFromFile( gos_Texture_Solid, actualPath, 0 );
						}

						if (openFailed && (Environment.fullScreen == 0) && prefs.fullScreen)
							EnterFullScreenMode();
					}
				}

				break;
			}
		}
	}

	return 1;
}

bool ForceGroupBar::isPlayingVideo()
{
	if ( ForceGroupIcon::bMovie )
		return true;

	return false;
}

//*************************************************************************************************
// end of file ( ForceGroupBar.cpp )
