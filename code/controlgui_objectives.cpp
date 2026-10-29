#include "render_view.h"
#include "../GameOS/gameos/ui_texture.h"
#include "../mclib/texture_assets.h"
// ControlGui: objective start, results, the ImGui objectives / timer / video panels and the legacy objective rows.
// Declared in the class headers; shared names in controlgui_internal.h.
#include "controlgui_internal.h"
#include "mc2fmt.h"



void ControlGui::startObjectives( bool bStart )
{
	if ( !renderObjectives )
		objectiveTime = 0.f;
	else if ( renderStatusInfo )		// reset timer, if we are in results land
		objectiveTime = objectiveMoveInfo[OBJECTVE_MOVE_COUNT-1].time;
	
	renderObjectives = bStart; 

	if ( MPlayer )
	{
		MPlayer->calcPlayerRanks();

		bool scoreShown[MAX_MC_PLAYERS]; // keep track of whose shown the score
		memset( scoreShown, 0, sizeof( bool ) * MAX_MC_PLAYERS );

		long playerCount = 0;
		const MC2Player* players = MPlayer->getPlayers(playerCount);
		
		const MC2Player* sorted[MAX_MC_PLAYERS];
		for ( int j = 0; j < playerCount; j++ )
		{
			sorted[j] = &players[j];
		}
		qsort( sorted, playerCount, sizeof( MC2Player* ), sortStats );

		mpStats[0].setData( nullptr, 1 ); // this is the header...
		for ( int i = 1; i < MAX_MC_PLAYERS+1; i++ )
		{
			if ( i-1 < playerCount )
			{
				mpStats[i].setData( sorted[i-1], !scoreShown[sorted[i-1]->team] );
				scoreShown[sorted[i-1]->team] = true;
				mpStats[i].showGUIWindow( true );
			}
			else
				mpStats[i].showGUIWindow( 0 );
		}
	}
};

void ControlGui::renderResults()
{
	if ( renderStatusInfo )
	{
		if ( infoWnd ) // kill info wnd stuff
			infoWnd->setUnit( nullptr );

		resultsTime += mc2::render_view::clock().dt;
		float t0, t1, p0, p1;
		t1 = p1 = t0 = p0 = 0.f;
		float delta = 0.f;
		
		// figure out where we are in animation
		for ( int j = 0; j < RESULTS_MOVE_COUNT - 1; j++ )
		{
			if ( missionResultsMoveInfo[j].time <= resultsTime && missionResultsMoveInfo[j+1].time > resultsTime )
			{
				t0 = missionResultsMoveInfo[j].time;
				t1 = missionResultsMoveInfo[j + 1].time;
				p0 = (missionResultsMoveInfo[j].position)	* (static_cast<float>(Environment.screenWidth))/640.f;
				p1 = (missionResultsMoveInfo[j + 1].position) * (static_cast<float>(Environment.screenWidth))/640.f;
				break;
			}
		}
		
		// if not done yet
		if ( t1 )
		{

			float dT = resultsTime - t0;
			float currentPosition = p0 + dT * ( (p1 - p0)/(t1 -t0) );
			delta = currentPosition - missionStatusInfos[0].location[0].y;
		}
		else
		{
			delta = missionResultsMoveInfo[OBJECTVE_MOVE_COUNT - 1].position - missionStatusInfos[0].location[0].y;
			// grey down the screen
			GUI_RECT rect = { 0, 0, Environment.screenWidth, Environment.screenHeight };
			drawEmptyRect( rect, 0x44000000 );
			if ( resultsTime < missionResultsMoveInfo[OBJECTVE_MOVE_COUNT - 1].time + .25 )
				startObjectives( 1 );
		}


		for ( int i = 0; i < missionStatusInfoCount; i++ )
		{
			missionStatusInfos[i].move( 0, delta );
			missionStatusInfos[i].render();
		}

		missionStatusRect.rect.top += delta;
		missionStatusRect.rect.bottom += delta;
			

		char text[256];
		if ( mc2::render_view::hud().objectiveStatus == OS_SUCCESSFUL )
			cLoadString( IDS_MISSION_SUCCESS, text, 256 );
		else
			cLoadString( IDS_MISSION_FAILED, text, 256 );

		if ( MPlayer )
		{

			if ( MPlayer->playerInfo[MPlayer->commanderID].winner  )
				cLoadString( IDS_MISSION_SUCCESS, text, 256 );
			else
				cLoadString( IDS_MISSION_FAILED, text, 256 );

		}

		gos_TextDrawBackground( missionStatusRect.rect.left, missionStatusRect.rect.top, missionStatusRect.rect.right,
			missionStatusRect.rect.bottom, 0xff000000 );

		missionResultsFont.render( text, missionStatusRect.rect.left, missionStatusRect.rect.top, 
			missionStatusRect.rect.right - missionStatusRect.rect.left,
			missionStatusRect.rect.bottom - missionStatusRect.rect.top,
			missionStatusRect.color,
			0, 3 );

		float fadeTime = objectiveTime - objectiveMoveInfo[OBJECTVE_MOVE_COUNT - 1].time - 3.0f 
			- mc2::render_view::hud().objectiveCount;
		
		if ( !neverEndingStory && (fadeTime > 0.f) )
		{
			long color = interpolateColor( 0x00000000, 0xff000000, fadeTime/1.0f );
			GUI_RECT rect = { 0,0, Environment.screenWidth, Environment.screenHeight };
			drawRect( rect, color );
		}


	}
}

bool playedObjectiveClick[100];

// OBJ-IMGUI-SLICE-1: gate. Default OFF -> legacy tile/font overlay, byte-identical.
bool ControlGui::imguiObjEnabled()
{
	static const bool s_on = mc2::gate::get<"MC2_UI_IMGUI_OBJ">();
	return s_on;
}

// OBJ-IMGUI-SLICE-1: shared objective-row gather. The SINGLE place that owns
// the enumeration predicate (IsActive && !IsHiddenTrigger, primary = Priority()==1,
// primaries first in team-list order then secondaries) and the status->color
// mapping incl. the activation flash. Both the legacy RenderObjectives loops
// and renderObjectivesImGui consume this, so the two paths cannot drift.
int ControlGui::gatherObjectiveRows(ObjectiveUiRow* rows, int maxRows)
{
    int n = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& row : mc2::render_view::hud().objectives) {
            if (!row.active || row.hidden || row.primary != (pass == 0)) continue;
            if (n == maxRows) return n;
            rows[n++] = {row.description, row.primary, row.color, row.status == OS_SUCCESSFUL,
                row.status == OS_FAILED, row.points};
        }
    return n;
}

// OBJ-IMGUI-SLICE-1: ImGui objectives overlay (MC2_UI_IMGUI_OBJ=1). Hybrid
// port: the tile-stitched left edge bars come from the offline-baked frame
// (tools/ui_art_reconstruct.py --panel Obj -> data/art/gui/mc2_obj_frame.png,
// authored bbox origin (0,76) size 14x236 in the 800x600 design space); the
// header / objective rows / totals are drawn procedurally from the SAME
// fit-loaded authored constants (OBJECTIVESTOP/LEFT/SKIP/TOTALRIGHT) and the
// SAME shared gatherObjectiveRows walk as the legacy path, mapped through
// gos_ComputeHudCanvasBox. Display-only (legacy overlay takes no input; it is
// dismissed via startObjectives / the pause-menu OBJECTIVES toggle).
void ControlGui::renderObjectivesImGui()
{
	// guards mirrored from the legacy path
	if ( !( renderObjectives || renderStatusInfo ) )
		return;
	if ( renderStatusInfo && resultsTime < .24f ) // don't start objectives until mission results is done
	{
		memset( playedObjectiveClick, 0, sizeof( bool ) * 100 );
		return;
	}

	objectiveTime += mc2::render_view::clock().dt;	// renderResults' end-of-mission fade reads this

	// Map the authored layout space into the 16:9 HUD canvas box (same pattern
	// as PauseWindow/InfoWindow::renderImGui). The OBJECTIVES* constants are
	// raw logical-resolution pixels from the res-matched buttonLayout fit, so
	// the denominators are the logical screen dims (800x600 in the shipped
	// config), NOT hardcoded.
	int bx = 0, by = 0, bw = 0, bh = 0;
	if ( !gos_ComputeHudCanvasBox( Environment.drawableWidth, Environment.drawableHeight,
			&bx, &by, &bw, &bh ) )
	{ bx = 0; by = 0; bw = Environment.drawableWidth; bh = Environment.drawableHeight; }
	const float lw = Environment.screenWidth  > 0 ? static_cast<float>(Environment.screenWidth)  : 800.0f;
	const float lh = Environment.screenHeight > 0 ? static_cast<float>(Environment.screenHeight) : 600.0f;
	const float sx = static_cast<float>(bw) / lw;
	const float sy = static_cast<float>(bh) / lh;
	auto AX = [&]( float ax ){ return bx + ax * sx; };
	auto AY = [&]( float ay ){ return by + ay * sy; };

	// Legacy slides the panel until objectiveInfos[0] rests at
	// objectiveMoveInfo[last].position (instant with the stock 2-key table).
	// The ImGui path never mutates the statics, so the resting shift is just
	// target - authored. Applies to the frame + header/description X only —
	// the box/check tiles and totalRight are never delta-shifted by legacy.
	const float shiftX = objectiveMoveInfo[OBJECTVE_MOVE_COUNT - 1].position
		- ( objectiveInfos ? objectiveInfos[0].location[0].x : 0.0f );

	// one-time load of the baked frame (gate-on path only). nFlush=0x1 pins the
	// node — the overlay never draws through the legacy quad path, so an
	// unpinned texture gets cached out mid-mission and its GL name recycled
	// (first capture showed unit-icon atlas garbage in the frame rect). The GL
	// name is re-resolved per frame for the same reason.
	static DWORD s_frameMcH = 0;
	static bool s_frameTried = false;
	if ( !s_frameTried )
	{
		s_frameTried = true;
		s_frameMcH = mc2::textures::load(
			"data\\art\\gui\\mc2_obj_frame.png", gos_Texture_Alpha, 0, 0, 0x1 );
	}
	uint64_t s_frameGlTex = 0;
	if ( s_frameMcH )
	{
		DWORD gosID = mc2::textures::sourceId( s_frameMcH );
		if ( gosID && gosID != 0xffffffff )
			s_frameGlTex = gos_GetUiTextureId( gosID );
	}

	ImGui::SetNextWindowPos( ImVec2( static_cast<float>(bx), static_cast<float>(by) ) );
	ImGui::SetNextWindowSize( ImVec2( static_cast<float>(bw), static_cast<float>(bh) ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
	ImGui::Begin( "##mc2_obj", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs );	// display-only overlay

	ImDrawList* dl = ImGui::GetWindowDrawList();

	auto argb2im = []( unsigned long argb ){
		return IM_COL32( (argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff, (argb >> 24) & 0xff );
	};
	// FONT-IMGUI-SLICE-1: text now draws with the SAME legacy bitmap guiFont
	// through HudFont (glyph quads from the gos font atlas), so strings pass
	// through unmodified exactly like the legacy path (the font's glyphs are
	// caps-styled; no manual uppercasing — that was the ImGui-default-font
	// approximation this slice retires).
	HGOSFONT3D objFont = guiFont.getTempHandle();
	// drawShadowText equivalent: legacy offsets the shadow pass by
	// (xOffset=-1, yOffset=+1) authored pixels (mclib/utilities.cpp:414).
	auto ShadowText = [&]( float x, float y, unsigned long argb, const char* text ){
		HudFont::Draw( dl, objFont, x - 1.0f * sx, y + 1.0f * sy,
			IM_COL32( 0, 0, 0, 255 ), text, sx, sy );
		HudFont::Draw( dl, objFont, x, y, argb2im( argb ), text, sx, sy );
	};
	auto TextWidth = [&]( const char* text ){
		return HudFont::Measure( objFont, text, sx, sy ).x;
	};
	// legacy status tiles (PrimaryObjBox/Check/X StaticInfos): AddImage from
	// their atlas with the UVs StaticInfo::init resolved into the vertices.
	auto StaticTile = [&]( StaticInfo& st, float axPos, float ayPos ){
		DWORD gosID = mc2::textures::sourceId( st.textureHandle );
		if ( !gosID || gosID == 0xffffffff )
			return;
		uint64_t glTex = gos_GetUiTextureId( gosID );
		if ( !glTex )
			return;
		dl->AddImage( static_cast<ImTextureID>(static_cast<intptr_t>(glTex)),
			ImVec2( AX( axPos ), AY( ayPos ) ),
			ImVec2( AX( axPos + st.width() ), AY( ayPos + st.height() ) ),
			ImVec2( st.location[0].u, st.location[0].v ),
			ImVec2( st.location[2].u, st.location[2].v ) );
	};

	// baked frame (the two left-edge hazard bars), authored bbox (0,76) 14x236.
	// FULL 800x600-canvas PNG sampled at the authored sub-rect (same pattern as
	// the pause frame) — a cropped 14x236 PNG gets pow2-padded by the texture
	// manager, so UV 0..1 sampled the padding and squished the bar.
	if ( s_frameGlTex )
		dl->AddImage( static_cast<ImTextureID>(static_cast<intptr_t>(s_frameGlTex)),
			ImVec2( AX( 0.0f + shiftX ), AY( 76.0f ) ),
			ImVec2( AX( 14.0f + shiftX ), AY( 76.0f + 236.0f ) ),
			ImVec2( 0.0f / 800.0f, 76.0f / 600.0f ),
			ImVec2( 14.0f / 800.0f, ( 76.0f + 236.0f ) / 600.0f ) );

	char buffer[256];

	// "Primary Objectives" header
	cLoadString( IDS_PRIMARY, buffer, 245 );
	ShadowText( AX( OBJECTIVEHEADERLEFT + shiftX ), AY( OBJECTIVESTOP ), 0xffffffff, buffer );

	ObjectiveUiRow rows[100];
	const int nRows = gatherObjectiveRows( rows, 100 );

	StaticInfo* boxTile   = objectiveInfos ? &objectiveInfos[objectiveInfoCount - 3] : nullptr;
	StaticInfo* checkTile = objectiveInfos ? &objectiveInfos[objectiveInfoCount - 2] : nullptr;
	StaticInfo* xTile     = objectiveInfos ? &objectiveInfos[objectiveInfoCount - 1] : nullptr;

	long pos = OBJECTIVESTOP;
	pos += OBJECTIVESSKIP;

	float objectiveCount = -objectiveMoveInfo[OBJECTVE_MOVE_COUNT - 1].time;
	int sum = 0;
	int objectiveNum = 0;

	// one row: status box/check/X tiles + description + (results) dots/total.
	// Same accumulation, totals reveal timing and kill-marker click as legacy.
	auto DrawRow = [&]( const ObjectiveUiRow& row, long yPos, bool bTotal ){
		char desc[256];
		strncpy( desc, row.description.c_str(), 255 );
		desc[255] = 0;

		ShadowText( AX( OBJECTIVESLEFT + shiftX ), AY( static_cast<float>(yPos) ), static_cast<unsigned long>(row.color), desc );

		if ( bTotal )
		{
			char total[64];
			int amount = row.succeeded ? row.points : 0;
			mc2::fmt::to_buf(total, "{:d}", mc2::fmt::as_i32(amount));
			const float totalW = TextWidth( total );
			ShadowText( AX( static_cast<float>(OBJECTIVESTOTALRIGHT) ) - totalW, AY( static_cast<float>(yPos) ), 0xffffffff, total );

			// leader dots between the description and the right-aligned total
			const float descW = TextWidth( desc );
			const float dotW = TextWidth( "...................." ) / 20.f;
			const float totalLength = ( AX( static_cast<float>(OBJECTIVESTOTALRIGHT) ) - totalW )
				- ( AX( OBJECTIVESLEFT + shiftX ) + descW );
			long numberOfDots = static_cast<long>(( totalLength / dotW ));
			if ( 3 > numberOfDots )
				numberOfDots = 3;
			char dots[256];
			long nd = numberOfDots - 2;
			if ( nd > 255 ) nd = 255;
			{ int i = 0; for ( ; i < nd; i++ ) dots[i] = '.'; dots[i] = 0; }
			const float dotFinalLength = dotW * static_cast<float>(nd);
			ShadowText( AX( static_cast<float>(OBJECTIVESTOTALRIGHT) ) - totalW - dotFinalLength,
				AY( static_cast<float>(yPos) ), static_cast<unsigned long>(row.color), dots );
		}

		// little box + check/X marks (authored x from the fit statics; y
		// follows the row like the legacy setLocation calls)
		if ( boxTile )
			StaticTile( *boxTile, boxTile->location[0].x, static_cast<float>(( yPos + OBJECTIVEBOXSKIP )) );
		if ( row.failed && xTile )
			StaticTile( *xTile, xTile->location[0].x, static_cast<float>(( yPos + OBJECTIVECHECKSKIP )) );
		else if ( row.succeeded && checkTile )
			StaticTile( *checkTile, checkTile->location[0].x, static_cast<float>(( yPos + OBJECTIVECHECKSKIP )) );
	};

	// primary objectives
	for ( int r = 0; r < nRows; r++ )
	{
		if ( !rows[r].primary )
			continue;
		bool bTotal = renderStatusInfo && objectiveCount < objectiveTime;	// drawing one per second
		DrawRow( rows[r], pos, bTotal );
		if ( rows[r].succeeded && bTotal )
		{
			sum += rows[r].points;
			if ( !playedObjectiveClick[objectiveNum] )
			{
				playedObjectiveClick[objectiveNum] = true;
				soundSystem->playDigitalSample( LOG_KILLMARKER );
			}
		}
		pos += OBJECTIVESSKIP;
		objectiveCount += 1.f;
		objectiveNum++;
	}

	pos -= OBJECTIVESSKIP;

	// "Secondary Objectives" header
	cLoadString( IDS_SECONDARY, buffer, 245 );
	pos += OBJEECTIVESHEADERSKIP;
	ShadowText( AX( OBJECTIVEHEADERLEFT + shiftX ), AY( static_cast<float>(pos) ), 0xffffffff, buffer );

	// secondary objectives
	for ( int r = 0; r < nRows; r++ )
	{
		if ( rows[r].primary )
			continue;
		pos += OBJECTIVESSKIP;
		bool bTotal = renderStatusInfo && objectiveCount < objectiveTime;	// drawing one per second
		DrawRow( rows[r], pos, bTotal );
		if ( rows[r].succeeded && bTotal )
		{
			sum += rows[r].points;
			if ( !playedObjectiveClick[objectiveNum] )
			{
				playedObjectiveClick[objectiveNum] = true;
				soundSystem->playDigitalSample( LOG_KILLMARKER );
			}
		}
		objectiveCount += 1.f;
		objectiveNum++;
	}

	if ( renderStatusInfo )	// draw the total
	{
		pos += OBJECTIVESSKIP;
		char total[256];
		char total2[256];
		cLoadString( IDS_TOTAL, total2, 255 );
		sprintf( total, total2, sum );
		const float width = TextWidth( total );
		ShadowText( AX( static_cast<float>(OBJECTIVESTOTALRIGHT) ) - width, AY( static_cast<float>(pos) ), 0xffffffff, total );
	}

	ImGui::End();
	ImGui::PopStyleVar( 1 );	// WindowPadding
}

// SMALLPANELS-IMGUI-SLICE-1: gates. Default OFF -> legacy draws, byte-identical.
bool ControlGui::imguiTimerEnabled()
{
	static const bool s_on = mc2::gate::get<"MC2_UI_IMGUI_TIMER">();
	return s_on;
}

bool ControlGui::imguiVideoEnabled()
{
	static const bool s_on = mc2::gate::get<"MC2_UI_IMGUI_VIDEO">();
	return s_on;
}

// SMALLPANELS-IMGUI-SLICE-1: ImGui mission-timer panel (MC2_UI_IMGUI_TIMER=1).
// Display-only. The caller (render()) keeps the legacy time/warning/blink
// computation — this fn receives the SAME formatted buffer + ARGB color the
// legacy drawShadowText call gets, so content/state logic cannot drift. Layout:
// the timer sits in the BOTTOM band (authored y=410 of 600 > 60%), so the
// legacy verts get the in-mission HUD shrink (s_hud_scale toward bottom-center)
// BEFORE the 16:9 canvas remap — mirror both, the same order flushHUDBatch
// applies them (INFO-IMGUI-SLICE-3 lesson).
void ControlGui::renderTimerImGui( const char* buffer, unsigned long color )
{
	int bx = 0, by = 0, bw = 0, bh = 0;
	if ( !gos_ComputeHudCanvasBox( Environment.drawableWidth, Environment.drawableHeight,
			&bx, &by, &bw, &bh ) )
	{ bx = 0; by = 0; bw = Environment.drawableWidth; bh = Environment.drawableHeight; }
	const float lw = Environment.screenWidth  > 0 ? static_cast<float>(Environment.screenWidth)  : 800.0f;
	const float lh = Environment.screenHeight > 0 ? static_cast<float>(Environment.screenHeight) : 600.0f;
	// same activation condition as flushHUDBatch (scale < 0.999 gate included)
	const float hud = ( gos_GetHudScaleActive() && gos_GetHudScale() < 0.999f )
		? gos_GetHudScale() : 1.0f;
	const float cx = static_cast<float>(bw) / lw;
	const float cy = static_cast<float>(bh) / lh;
	const float sx = cx * hud;
	const float sy = cy * hud;
	auto AX = [&]( float ax ){ return bx + ( lw * 0.5f + ( ax - lw * 0.5f ) * hud ) * cx; };
	auto AY = [&]( float ay ){ return by + ( lh        + ( ay - lh        ) * hud ) * cy; };

	ImGui::SetNextWindowPos( ImVec2( static_cast<float>(bx), static_cast<float>(by) ) );
	ImGui::SetNextWindowSize( ImVec2( static_cast<float>(bw), static_cast<float>(bh) ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );   // see video panel note
	ImGui::Begin( "##mc2_timer", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs );	// display-only overlay

	ImDrawList* dl = ImGui::GetWindowDrawList();

	// FONT-IMGUI-SLICE-1: same legacy bitmap timerFont through HudFont. Legacy
	// right-aligns: x = timerRect.rect.left - width. drawShadowText draws the
	// shadow pass at (-1,+1) authored px first; gos text forces alpha 255
	// (gameos_graphics.cpp fg.w = 255), so both passes draw opaque — the blink
	// state (color==0) renders opaque black exactly like the legacy path.
	HGOSFONT3D tf = timerFont.getTempHandle();
	const float wA = HudFont::Measure( tf, buffer, 1.0f, 1.0f ).x;
	const float xA = static_cast<float>(timerRect.rect.left) - wA;
	const float yA = static_cast<float>(timerRect.rect.top);
	HudFont::Draw( dl, tf, AX( xA - 1.0f ), AY( yA + 1.0f ),
		IM_COL32( 0, 0, 0, 255 ), buffer, sx, sy );
	HudFont::Draw( dl, tf, AX( xA ), AY( yA ),
		IM_COL32( (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff, 255 ),
		buffer, sx, sy );

	// timer graphic tile(s) (TimerStatic N): AddImage from the SAME runtime
	// StaticInfo state (screen rect + atlas UVs) the legacy render uses.
	for ( long i = 0; i < timerInfoCount; i++ )
	{
		StaticInfo& st = timerInfos[i];
		DWORD gosID = mc2::textures::sourceId( st.textureHandle );
		if ( !gosID || gosID == 0xffffffff )
			continue;
		uint64_t glTex = gos_GetUiTextureId( gosID );
		if ( !glTex )
			continue;
		dl->AddImage( static_cast<ImTextureID>(static_cast<intptr_t>(glTex)),
			ImVec2( AX( st.location[0].x ), AY( st.location[0].y ) ),
			ImVec2( AX( st.location[2].x ), AY( st.location[2].y ) ),
			ImVec2( st.location[0].u, st.location[0].v ),
			ImVec2( st.location[2].u, st.location[2].v ) );
	}

	ImGui::End();
	ImGui::PopStyleVar( 2 );	// WindowPadding + WindowBorderSize
}

// SMALLPANELS-IMGUI-SLICE-1: ImGui comm-video panel (MC2_UI_IMGUI_VIDEO=1).
// Display-only. Frame chrome = the SAME runtime VideoStatic StaticInfos the
// legacy loop draws; movie content = the decoder's current frame texture
// composited at the SAME aspect-fit quad + letterbox rect video_render uses
// (MC2Movie::getLayout). The panel sits in the TOP band (authored y 0..111),
// so the raw canvas box applies — no HUD shrink (PauseWindow pattern).
void ControlGui::renderVideoImGui()
{
	if ( !( moviePlaying && bMovie ) )
		return;

	int bx = 0, by = 0, bw = 0, bh = 0;
	if ( !gos_ComputeHudCanvasBox( Environment.drawableWidth, Environment.drawableHeight,
			&bx, &by, &bw, &bh ) )
	{ bx = 0; by = 0; bw = Environment.drawableWidth; bh = Environment.drawableHeight; }
	const float lw = Environment.screenWidth  > 0 ? static_cast<float>(Environment.screenWidth)  : 800.0f;
	const float lh = Environment.screenHeight > 0 ? static_cast<float>(Environment.screenHeight) : 600.0f;
	const float sx = static_cast<float>(bw) / lw;
	const float sy = static_cast<float>(bh) / lh;
	auto AX = [&]( float ax ){ return bx + ax * sx; };
	auto AY = [&]( float ay ){ return by + ay * sy; };

	ImGui::SetNextWindowPos( ImVec2( static_cast<float>(bx), static_cast<float>(by) ) );
	ImGui::SetNextWindowSize( ImVec2( static_cast<float>(bw), static_cast<float>(bh) ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
	// WindowBorderSize=0: the default 1px border insets the window inner clip
	// rect, clipping chrome texels that touch the canvas boundary (the top
	// strip's first row and the right strip's last column rasterized 1 device
	// px short vs legacy until this).
	ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
	ImGui::Begin( "##mc2_video", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs );	// display-only overlay

	ImDrawList* dl = ImGui::GetWindowDrawList();

	// frame chrome (VideoStatic N tiles: top/bottom strips, hazard left block,
	// right strip) — same runtime screen rects + atlas UVs as the legacy loop.
	for ( int i = 0; i < videoInfoCount; i++ )
	{
		StaticInfo& st = videoInfos[i];
		DWORD gosID = mc2::textures::sourceId( st.textureHandle );
		if ( !gosID || gosID == 0xffffffff )
			continue;
		uint64_t glTex = gos_GetUiTextureId( gosID );
		if ( !glTex )
			continue;
		dl->AddImage( static_cast<ImTextureID>(static_cast<intptr_t>(glTex)),
			ImVec2( AX( st.location[0].x ), AY( st.location[0].y ) ),
			ImVec2( AX( st.location[2].x ), AY( st.location[2].y ) ),
			ImVec2( st.location[0].u, st.location[0].v ),
			ImVec2( st.location[2].u, st.location[2].v ) );
	}

	// movie content. video_render early-outs entirely when no decoded frame is
	// ready — mirror that (chrome still draws, like the legacy loop above).
	unsigned gosTex = bMovie->getTextureHandle();
	if ( gosTex )
	{
		float qx0, qy0, qx1, qy1;
		long rx, ry, rw, rh;
		if ( bMovie->getLayout( qx0, qy0, qx1, qy1, rx, ry, rw, rh ) )
		{
			// letterbox fill (opaque black) when the aspect-fit quad does not
			// cover the dest rect — same condition as video_render.
			if ( qx0 > (float)rx || qy0 > (float)ry ||
				 qx1 < static_cast<float>(( rx + rw )) || qy1 < static_cast<float>(( ry + rh )) )
				dl->AddRectFilled(
					ImVec2( AX( static_cast<float>(rx) ), AY( static_cast<float>(ry) ) ),
					ImVec2( AX( static_cast<float>(( rx + rw )) ), AY( static_cast<float>(( ry + rh )) ) ),
					IM_COL32( 0, 0, 0, 255 ) );
			uint64_t glTex = gos_GetUiTextureId( gosTex );
			if ( glTex )
				dl->AddImage( static_cast<ImTextureID>(static_cast<intptr_t>(glTex)),
					ImVec2( AX( qx0 ), AY( qy0 ) ),
					ImVec2( AX( qx1 ), AY( qy1 ) ),
					ImVec2( 0.0f, 0.0f ), ImVec2( 1.0f, 1.0f ) );
		}
	}

	ImGui::End();
	ImGui::PopStyleVar( 2 );	// WindowPadding + WindowBorderSize
}

void ControlGui::RenderObjectives()
{
	// OBJ-IMGUI-SLICE-1: ImGui overlay when gated on. The MPlayer player-status
	// variant stays on the legacy path (out of slice scope).
	if ( imguiObjEnabled() &&
		!( MPlayer && MPlayer->missionSettings.missionType != MISSION_TYPE_OTHER ) )
	{
		renderObjectivesImGui();
		return;
	}

	// make sure we need to do it.
	if ( renderObjectives || renderStatusInfo )
	{
		if ( renderStatusInfo )
		{
			if ( resultsTime < .24f ) // don't start objectives until mission results is done
			{
				memset(playedObjectiveClick,0,sizeof(bool) * 100);
				return;
			}
		}
	
		objectiveTime += mc2::render_view::clock().dt;

		float t0, t1, p0, p1;

		t1 = p1 = t0 = p0 = 0.f;
		float delta = 0.f;
		
		// figure out where we are in animation
		for ( int j = 0; j < OBJECTVE_MOVE_COUNT - 1; j++ )
		{
			if ( objectiveMoveInfo[j].time <= objectiveTime && objectiveMoveInfo[j+1].time > objectiveTime )
			{
				t0 = objectiveMoveInfo[j].time;
				t1 = objectiveMoveInfo[j + 1].time;
				p0 = (objectiveMoveInfo[j].position)	* (static_cast<float>(Environment.screenWidth))/640.f;
				p1 = (objectiveMoveInfo[j + 1].position) * (static_cast<float>(Environment.screenWidth))/640.f;
				break;
			}
		}
		
		// if not done yet
		if ( t1 )
		{

			float dT = objectiveTime - t0;
			float currentPosition = p0 + dT * ( (p1 - p0)/(t1 -t0) );
			delta = currentPosition - objectiveInfos[0].location[0].x;
		}
		else
		{
			delta = objectiveMoveInfo[OBJECTVE_MOVE_COUNT - 1].position - objectiveInfos[0].location[0].x;
		}

		// draw little edge pieces
		for ( int i = 0; i < objectiveInfoCount - 3; i++ )
		{
			objectiveInfos[i].move( delta, 0 );
			objectiveInfos[i].render();
		}

		if ( delta )
		{
			OBJECTIVEHEADERLEFT += delta;
			OBJECTIVESLEFT += delta;
		}

		if ( MPlayer && MPlayer->missionSettings.missionType != MISSION_TYPE_OTHER )
		{
			renderPlayerStatus(delta);
			return;
		}


		// draw "Primary"
		char buffer[256];
		cLoadString( IDS_PRIMARY, buffer, 245 );
		drawShadowText( 0xffffffff, 0xff000000, guiFont.getTempHandle(), OBJECTIVEHEADERLEFT, OBJECTIVESTOP, 1, buffer, 0,
			guiFont.getSize() );

		// draw primary objectives
		unsigned long pos = OBJECTIVESTOP;
		pos+= OBJECTIVESSKIP;

		// OBJ-IMGUI-SLICE-1: enumeration/status/color factored into the shared
		// gatherObjectiveRows (used verbatim by the ImGui path) — same rows,
		// same order, same colors; the draw + timing side-effects stay here.
		ObjectiveUiRow objRows[100];
		const int nObjRows = gatherObjectiveRows( objRows, 100 );

		float objectiveCount = -objectiveMoveInfo[OBJECTVE_MOVE_COUNT-1].time;
		int sum = 0;
		int objectiveNum = 0;
		for ( int r = 0; r < nObjRows; r++ )
		{
			if ( objRows[r].primary )
			{
				bool bTotal =  renderStatusInfo && objectiveCount < objectiveTime;  // drawing one per second
				renderObjective( objRows[r], OBJECTIVESLEFT, pos, bTotal );
				if ( objRows[r].succeeded && bTotal )
				{
					sum += objRows[r].points;
					if (!playedObjectiveClick[objectiveNum])
					{
						playedObjectiveClick[objectiveNum] = true;
						soundSystem->playDigitalSample(LOG_KILLMARKER);
					}
				}

				pos += OBJECTIVESSKIP;
				objectiveCount += 1.f;
				objectiveNum++;
			}
		}

		pos -= OBJECTIVESSKIP;

		// draw "Secondary"
		cLoadString( IDS_SECONDARY, buffer, 245 );
		pos += OBJEECTIVESHEADERSKIP;
		drawShadowText( 0xffffffff, 0xff000000, guiFont.getTempHandle(), OBJECTIVEHEADERLEFT, pos, 1, buffer, 0,
			guiFont.getSize() );

		// draw secondary objectives
		for ( int r = 0; r < nObjRows; r++ )
		{
			if ( !objRows[r].primary )
			{
				pos += OBJECTIVESSKIP;
				bool bTotal =  renderStatusInfo && objectiveCount < objectiveTime;  // drawing one per second
				renderObjective( objRows[r], OBJECTIVESLEFT, pos, bTotal );
				if ( objRows[r].succeeded && bTotal )
				{
					sum += objRows[r].points;
					if (!playedObjectiveClick[objectiveNum])
					{
						playedObjectiveClick[objectiveNum] = true;
						soundSystem->playDigitalSample(LOG_KILLMARKER);
					}
				}

				objectiveCount += 1.f;
				objectiveNum++;
			}
		}

		if ( renderStatusInfo ) // draw the total
		{
			pos += OBJECTIVESSKIP;

			char total[256];
			char total2[256];
			cLoadString( IDS_TOTAL, total2, 255 );
			sprintf( total, total2, sum );
			unsigned long width = guiFont.width( total );
			drawShadowText( 0xffffffff, 0xff000000, guiFont.getTempHandle(), OBJECTIVESTOTALRIGHT - width, pos, 0, total,
				0, guiFont.getSize() );

		}

	}
}

void ControlGui::renderPlayerStatus(float xDelta)
{

	bool scoreShown[MAX_MC_PLAYERS]; // keep track of whose shown the score
	memset( scoreShown, 0, sizeof( bool ) * MAX_MC_PLAYERS );


	MPlayer->calcPlayerRanks();
	long playerCount = 0;
	const MC2Player* players = MPlayer->getPlayers(playerCount);
	
	const MC2Player* sorted[MAX_MC_PLAYERS];
	for ( int j = 0; j < playerCount; j++ )
	{
		sorted[j] = &players[j];
	}
	qsort( sorted, playerCount, sizeof( MC2Player* ), sortStats );

	mpStats[0].setData( nullptr, 1 ); // this is the header...
	for ( int i = 1; i < MAX_MC_PLAYERS+1; i++ )
	{
		if ( i-1 < playerCount )
		{
			mpStats[i].setData( sorted[i-1], !scoreShown[sorted[i-1]->team] );
			scoreShown[sorted[i-1]->team] = true;
			mpStats[i].showGUIWindow( true );
		}
		else
			mpStats[i].showGUIWindow( 0 );
	}

	GUI_RECT rect = { static_cast<long>((mpStats[0].left()-5+xDelta)), static_cast<long>((mpStats[0].top()-5)),
		static_cast<long>((mpStats[0].right()+5+xDelta)), static_cast<long>((mpStats[0].bottom()+5))};
	
	for (int i = 1; i < 9; i++ )
	{
		if ( mpStats[i].isShowing() )
			rect.bottom += mpStats[0].height() + 4;
	}

	drawRect( rect, 0x7f000000 );

	for (int i = 0; i < 9; i++ )
	{
		mpStats[i].render( xDelta, ((i) * (mpStats[0].height() + 4) ));
	}


/*	long xLocs[4] = {	mpStats[0].getPlayerHeadingX(),
						mpStats[0].getRankingX(),
						mpStats[0].getKillsX(),
						mpStats[0].getLossesX()
					};
	long textIDs[4] = { IDS_MP_STATS_PLAYER_NAME,
						IDS_MP_STATS_SCORE,
						IDS_MP_STATS_KILLS,
						IDS_MP_STATS_LOSSES
						};

	char text[256];
	for ( i = 0; i < 4; i++ )
	{
		cLoadString( textIDs[i], text, 255 );

		drawShadowText( 0xffffffff, 0xff000000, missionResultsFont.getTempHandle(), 
			xLocs[i] + OBJECTIVEHEADERLEFT, OBJECTIVESTOP, 1, text, 0, 1 );
	}*/


}

// OBJ-IMGUI-SLICE-1: takes the shared gather row (enumeration + status color
// live in gatherObjectiveRows now, consumed by BOTH this legacy draw and the
// ImGui overlay). Pixel output unchanged.
void ControlGui::renderObjective( const ObjectiveUiRow& row, long xPos, long yPos, bool bDrawTotal )
{
	const long color = row.color;

	drawShadowText( color, 0xff000000, guiFont.getTempHandle(), xPos, yPos, 1, row.description.c_str(), 0,
		guiFont.getSize() );
	if ( bDrawTotal )
	{
		/*kept signed: mixing signed and unsigned values
		in arithmetic expressions can be bad news if any intermediate result is negative */
		long width, height;
		long descWidth;
		long dotWidth;

		char total[64];
		int amount = row.succeeded ? row.points : 0;
		mc2::fmt::to_buf(total, "{:d}", mc2::fmt::as_i32(amount));
		width = guiFont.width( total );
		
		// draw total
		drawShadowText( 0xffffffff, 0xff000000, guiFont.getTempHandle(), OBJECTIVESTOTALRIGHT - width, yPos, 0, total, 0, 
			guiFont.getSize() );

		// draw .............
		DWORD tmpULong1, tmpULong2;
		gos_TextStringLength( &tmpULong1, &tmpULong2, row.description.c_str() );
		descWidth = static_cast<int>(tmpULong1);
		height = static_cast<int>(tmpULong2);
		gos_TextStringLength( &tmpULong1, &tmpULong2, "...................." );
		dotWidth = static_cast<int>(tmpULong1);
		height = static_cast<int>(tmpULong2);
		gosASSERT(0 < dotWidth);
		float dotRealWidth = ((static_cast<float>(dotWidth))/20.f);

		float totalLength = (OBJECTIVESTOTALRIGHT - width - xPos - descWidth);
		long numberOfDots = (totalLength)/dotRealWidth;

		if (3 > numberOfDots)
		{
			numberOfDots = 3;
		}
		
		char* dots = static_cast<char*>(_alloca( sizeof( char ) * (numberOfDots+1) ));
        int i;
		for (i = 0; i < numberOfDots - 2; i++ )
			dots[i] = '.';
		
		dots[i] = 0;

		float dotFinalLength = dotRealWidth * static_cast<float>((numberOfDots - 2));

		drawShadowText( color, 0xff000000, guiFont.getTempHandle(), OBJECTIVESTOTALRIGHT - width - dotFinalLength, yPos, 0, dots, 
			0, guiFont.getSize() );
	}

	// draw little boxes and check marks
	float height = objectiveInfos[objectiveInfoCount-3].location[2].y - objectiveInfos[objectiveInfoCount-3].location[0].y;

	objectiveInfos[objectiveInfoCount-3].location[0].y =yPos + OBJECTIVEBOXSKIP;
	objectiveInfos[objectiveInfoCount-3].location[3].y =yPos + OBJECTIVEBOXSKIP;
	objectiveInfos[objectiveInfoCount-3].location[1].y =yPos + OBJECTIVEBOXSKIP + height;
	objectiveInfos[objectiveInfoCount-3].location[2].y =yPos + OBJECTIVEBOXSKIP + height;
	int x = objectiveInfos[objectiveInfoCount-3].location[0].x;
	objectiveInfos[objectiveInfoCount-3].setLocation( x, yPos + OBJECTIVEBOXSKIP );
	objectiveInfos[objectiveInfoCount-3].render();

	if ( row.failed )
	{
		float height = objectiveInfos[objectiveInfoCount-1].location[2].y - objectiveInfos[objectiveInfoCount-1].location[0].y;
		objectiveInfos[objectiveInfoCount-1].location[0].y =yPos + OBJECTIVECHECKSKIP;
		objectiveInfos[objectiveInfoCount-1].location[3].y =yPos + OBJECTIVECHECKSKIP;
		objectiveInfos[objectiveInfoCount-1].location[1].y =yPos + OBJECTIVECHECKSKIP + height;
		objectiveInfos[objectiveInfoCount-1].location[2].y =yPos + OBJECTIVECHECKSKIP + height;
		int x = objectiveInfos[objectiveInfoCount-1].location[0].x;

		objectiveInfos[objectiveInfoCount-1].setLocation( x, yPos + OBJECTIVECHECKSKIP );
		objectiveInfos[objectiveInfoCount-1].render();

	}
	else if ( row.succeeded )
	{
		float height = objectiveInfos[objectiveInfoCount-2].location[2].y - objectiveInfos[objectiveInfoCount-2].location[0].y;
		objectiveInfos[objectiveInfoCount-2].location[0].y =yPos + OBJECTIVECHECKSKIP;
		objectiveInfos[objectiveInfoCount-2].location[3].y =yPos + OBJECTIVECHECKSKIP;
		objectiveInfos[objectiveInfoCount-2].location[1].y =yPos + OBJECTIVECHECKSKIP + height;
		objectiveInfos[objectiveInfoCount-2].location[2].y =yPos + OBJECTIVECHECKSKIP + height;
		int x = objectiveInfos[objectiveInfoCount-2].location[0].x;

		objectiveInfos[objectiveInfoCount-2].setLocation( x, yPos + OBJECTIVECHECKSKIP );
		objectiveInfos[objectiveInfoCount-2].render();
	}		
}
