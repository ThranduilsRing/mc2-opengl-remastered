/*************************************************************************************************\
hudimguipanel.cpp : ImGui HUD panel helper (IN-MISSION-HUD-IMGUI-SCOPE-1 slice B).
See hudimguipanel.h for the coordinate / exemption-bracket / pin-to-legacy contract.
\*************************************************************************************************/

#include <algorithm>
#include "hudimguipanel.h"

#include <gameos.hpp>
#include "../GuiRuntime/GuiRuntime.h"
#include "imgui.h"

#include <cstdlib>
#include <cstring>

namespace HudImGui
{

static bool s_pinnedToLegacy = false;

bool pinnedToLegacy()
{
	return s_pinnedToLegacy;
}

// Bottom-band shrink, same math + gate as flushHUDBatch (gameos_graphics.cpp):
// band and anchor live in Environment.screenWidth/Height space, gate is the
// owning draw's centroid y vs 60% of screen height, anchor is bottom-center.
void hudShrinkPoint( float& hx, float& hy, float centroidHudY )
{
	if ( !gos_GetHudScaleActive() )
		return;
	const float scale = gos_GetHudScale();
	if ( scale >= 0.999f )
		return;

	const float sw = ( Environment.screenWidth  > 0 ) ? static_cast<float>(Environment.screenWidth)  : 1.f;
	const float sh = ( Environment.screenHeight > 0 ) ? static_cast<float>(Environment.screenHeight) : 1.f;
	if ( centroidHudY < sh * 0.60f )
		return;   // above the band: flushHUDBatch leaves these draws alone

	const float ax = sw * 0.5f;
	const float ay = sh;
	hx = ax + ( hx - ax ) * scale;
	hy = ay + ( hy - ay ) * scale;
}

// Authored HUD px -> display (drawable) px via the HUD canvas box, then
// drawable px -> ImGui logical coords (io.DisplaySize may differ from the
// drawable size on HiDPI).
static void hudToDrawable( float hx, float hy, float& dx, float& dy )
{
	const float sw = ( Environment.screenWidth  > 0 ) ? static_cast<float>(Environment.screenWidth)  : 1.f;
	const float sh = ( Environment.screenHeight > 0 ) ? static_cast<float>(Environment.screenHeight) : 1.f;
	const float drawW = ( Environment.drawableWidth  > 0 ) ? static_cast<float>(Environment.drawableWidth)  : sw;
	const float drawH = ( Environment.drawableHeight > 0 ) ? static_cast<float>(Environment.drawableHeight) : sh;

	int bx = 0, by = 0, bw = 0, bh = 0;
	if ( !gos_ComputeHudCanvasBox( static_cast<int>(drawW), static_cast<int>(drawH), &bx, &by, &bw, &bh ) )
	{
		bx = 0; by = 0; bw = static_cast<int>(drawW); bh = static_cast<int>(drawH);
	}

	// same transform flushHUDBatch applies to non-exempt HUD chrome,
	// composed with the logical->drawable stretch:
	dx = static_cast<float>(bx) + hx * static_cast<float>(bw) / sw;
	dy = static_cast<float>(by) + hy * static_cast<float>(bh) / sh;
}

void hudToDisplay( float hx, float hy, float& outX, float& outY )
{
	float dx, dy;
	hudToDrawable( hx, hy, dx, dy );

	float ioW = 0.f, ioH = 0.f;
	const float drawW = ( Environment.drawableWidth  > 0 ) ? static_cast<float>(Environment.drawableWidth)  : 1.f;
	const float drawH = ( Environment.drawableHeight > 0 ) ? static_cast<float>(Environment.drawableHeight) : 1.f;
	if ( GuiRuntime::GetDisplaySize( ioW, ioH ) && ioW > 0.f && ioH > 0.f )
	{
		outX = dx * ioW / drawW;
		outY = dy * ioH / drawH;
	}
	else
	{
		outX = dx;
		outY = dy;
	}
}

bool beginPanel( const char* id, float hudLeft, float hudTop,
				 float hudRight, float hudBottom, float* fontScaleOut )
{
	if ( s_pinnedToLegacy )
		return false;

	if ( !g_imguiInitialized || !ImGui::GetCurrentContext() )
	{
		// ImGui never came up — pin so we stop probing every frame.
		SPEWALWAYS(( nullptr, "HudImGui: ImGui unavailable, pinning HUD panels to legacy\n" ));
		s_pinnedToLegacy = true;
		return false;
	}

	float x0, y0, x1, y1;
	hudToDisplay( hudLeft, hudTop, x0, y0 );
	hudToDisplay( hudRight, hudBottom, x1, y1 );
	if ( x1 <= x0 || y1 <= y0 )
		return false;   // degenerate rect this frame (e.g. mid-init); legacy, no pin

	ImGui::SetNextWindowPos( ImVec2( x0, y0 ), ImGuiCond_Always );
	ImGui::SetNextWindowSize( ImVec2( x1 - x0, y1 - y0 ), ImGuiCond_Always );

	const ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

	bool open = true;
	if ( !ImGui::Begin( id, &open, flags ) )
	{
		ImGui::End();
		SPEWALWAYS(( nullptr, "HudImGui: Begin('%s') failed, pinning HUD panels to legacy\n", id ));
		s_pinnedToLegacy = true;
		return false;
	}

	if ( fontScaleOut )
	{
		// scale ImGui's ~13px base font to the panel's HUD pixel density:
		// 1 HUD px == k ImGui px; target ~16 HUD px line height.
		float ax, ay, bx2, by2;
		hudToDisplay( 0.f, 0.f, ax, ay );
		hudToDisplay( 0.f, 100.f, bx2, by2 );
		const float k = ( by2 - ay ) / 100.f;   // ImGui px per HUD px
		const ImFont* font = ImGui::GetIO().FontDefault;
		float scale = ( 16.f * k ) / ( font->FontSize * font->Scale );
		scale = std::clamp(scale, 0.5f, 4.f);
		*fontScaleOut = scale;
	}

	return true;
}

void endPanel()
{
	ImGui::End();
}

} // namespace HudImGui
