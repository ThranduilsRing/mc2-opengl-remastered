#ifndef HUD_IMGUI_FONT_H
#define HUD_IMGUI_FONT_H
/*===========================================================================
FONT-IMGUI-SLICE-1: shared legacy-bitmap-font text for the ImGui HUD panels
(pause / info / obj). Draws glyph quads straight from the gos font atlas
through the ImGui draw list using the exact quad math the legacy
gosRenderer::drawText path emits (offX=minx, offY=ascent-maxy, advance),
so gate-on text pixel-matches the legacy panels. Panels pass the SAME
aFont handles the legacy draw uses (aFont::getTempHandle()) plus the HUD
canvas scale (sx, sy) they already compute from gos_ComputeHudCanvasBox.

The atlas sample is nearest/clamp (pinned by gos_FontGetAtlasDesc) and the
font BMPs are binary white-on-transparent, so ImGui's vertexColor*texture
modulate reproduces the legacy gos_text shader (Foreground * mask.xxxx)
exactly.
===========================================================================*/

#include"gameos.hpp"
#include"imgui.h"

namespace HudFont
{
	// Line advance (lineSkip) in display pixels at vertical scale sy.
	float LineHeight( HGOSFONT3D font, float sy );

	// Single-line size (width = sum of advances, height = lineSkip) in
	// display pixels. Mirrors gos_TextStringLength for one line.
	ImVec2 Measure( HGOSFONT3D font, const char* text, float sx, float sy );

	// Draw one line at display position (x, y) = the legacy pen origin
	// (top of the line box, like gos_TextSetPosition).
	void Draw( ImDrawList* dl, HGOSFONT3D font, float x, float y,
		ImU32 col, const char* text, float sx, float sy );

	// Word-wrapped draw inside wrapWidth display pixels (same break rule as
	// the legacy findTextBreak: break at spaces / '\n'; a single word wider
	// than the region draws whole). Returns total height in display pixels.
	float DrawWrapped( ImDrawList* dl, HGOSFONT3D font, float x, float y,
		float wrapWidth, ImU32 col, const char* text, float sx, float sy );

	// Height DrawWrapped would use, without drawing.
	float MeasureWrappedHeight( HGOSFONT3D font, const char* text,
		float wrapWidth, float sx, float sy );

	// Legacy WrapType-3 (gos_TextSetAttributes WrapType 3): word-wrap into
	// the given display rect, centering the line block vertically and each
	// line horizontally — the math gosRenderer::drawText applies for
	// alignment-3 aFont::render calls (e.g. the info-window unit name).
	void DrawWrappedCentered( ImDrawList* dl, HGOSFONT3D font,
		float x0, float y0, float x1, float y1,
		ImU32 col, const char* text, float sx, float sy );
}

#endif // HUD_IMGUI_FONT_H
