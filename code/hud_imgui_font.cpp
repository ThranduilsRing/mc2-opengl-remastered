/*===========================================================================
FONT-IMGUI-SLICE-1: implementation. See hud_imgui_font.h for the contract.
Only called from gate-on ImGui panel paths; the legacy draw never calls
into this file, so gate-off stays byte-identical.

No per-font caching on purpose: gosFont handles are destroyed/reloaded
across missions and a pointer-keyed cache could serve a stale GL texture
name. gos_FontGetAtlasDesc is a cheap CPU query plus a handful of GL
texture-parameter calls per STRING (not per glyph) — noise next to the
frame's draw-call count.
 ===========================================================================*/

#include"hud_imgui_font.h"

#include<cstdint>
#include<cstring>

namespace
{

int glyphAdvance( HGOSFONT3D font, const gos_FontAtlasDesc& ad, char c )
{
	gos_FontGlyphDesc gd;
	if ( gos_FontGetGlyphDesc( font, static_cast<unsigned char>(c), &gd ) )
		return gd.advance;
	return ad.maxAdvance;   // legacy getCharAdvance fallback
}

// Single-line width in FONT pixels (sum of advances), stopping at '\n' / NUL.
int lineWidthPx( HGOSFONT3D font, const gos_FontAtlasDesc& ad,
	const char* text, int count )
{
	int w = 0;
	for ( int i = 0; i < count && text[i] && text[i] != '\n'; ++i )
		w += glyphAdvance( font, ad, text[i] );
	return w;
}

// Legacy findTextBreak (gameos_graphics.cpp): consume words until the next
// word would overflow regionWidth (font pixels). Returns chars consumed
// (including a trailing '\n' / the space that joined the last word).
int findBreak( HGOSFONT3D font, const gos_FontAtlasDesc& ad,
	const char* text, int regionWidthPx )
{
	int width = 0;
	int pos = 0;
	const int spaceAdv = glyphAdvance( font, ad, ' ' );

	while ( text[pos] )
	{
		// next possible break: run of non-space, non-'\n' chars
		int breakLen = 0;
		while ( text[pos + breakLen] && text[pos + breakLen] != ' ' &&
				text[pos + breakLen] != '\n' )
			breakLen++;

		int curWidth = 0;
		for ( int j = 0; j < breakLen; ++j )
			curWidth += glyphAdvance( font, ad, text[pos + j] );

		if ( width + curWidth >= regionWidthPx )
		{
			if ( pos == 0 )   // single word wider than region: draw it whole
				pos = breakLen;
			break;
		}

		width += curWidth;
		pos += breakLen;

		if ( text[pos] == '\n' ) { pos++; break; }
		if ( text[pos] == ' ' )  { width += spaceAdv; pos++; }
	}
	return pos;
}

// Draw one line's glyph quads. (x, y) display pixels = legacy pen origin.
void drawLine( ImDrawList* dl, HGOSFONT3D font, const gos_FontAtlasDesc& ad,
	float x, float y, ImU32 col, const char* text, int count,
	float sx, float sy )
{
	const float ooW = 1.0f / (float)ad.texWidth;
	const float ooH = 1.0f / (float)ad.texHeight;
	const ImTextureID tex = (ImTextureID)(intptr_t)ad.textureId;

	float penX = x;
	for ( int i = 0; i < count && text[i] && text[i] != '\n'; ++i )
	{
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if ( c < 0x20 )
			continue;   // control chars draw nothing (legacy drawText rule)

		gos_FontGlyphDesc gd;
		if ( !gos_FontGetGlyphDesc( font, c, &gd ) )
		{
			penX += ad.maxAdvance * sx;
			continue;
		}
		if ( gd.width > 0 && gd.height > 0 )
		{
			const float x0 = penX + gd.offX * sx;
			const float y0 = y    + gd.offY * sy;
			const float x1 = x0 + gd.width  * sx;
			const float y1 = y0 + gd.height * sy;
			const ImVec2 uv0( static_cast<float>(gd.u) * ooW, static_cast<float>(gd.v) * ooH );
			const ImVec2 uv1( static_cast<float>(( gd.u + gd.width ))  * ooW,
							  static_cast<float>(( gd.v + gd.height )) * ooH );
			dl->AddImage( tex, ImVec2( x0, y0 ), ImVec2( x1, y1 ), uv0, uv1, col );
		}
		penX += gd.advance * sx;
	}
}

} // namespace

namespace HudFont
{

float LineHeight( HGOSFONT3D font, float sy )
{
	gos_FontAtlasDesc ad;
	if ( !gos_FontGetAtlasDesc( font, &ad ) )
		return ImGui::GetTextLineHeight();
	return ad.lineSkip * sy;
}

ImVec2 Measure( HGOSFONT3D font, const char* text, float sx, float sy )
{
	if ( !text )
		return ImVec2( 0, 0 );
	gos_FontAtlasDesc ad;
	if ( !gos_FontGetAtlasDesc( font, &ad ) )
		return ImGui::CalcTextSize( text );
	const int w = lineWidthPx( font, ad, text, static_cast<int>(strlen( text )) );
	return ImVec2( w * sx, ad.lineSkip * sy );
}

void Draw( ImDrawList* dl, HGOSFONT3D font, float x, float y,
	ImU32 col, const char* text, float sx, float sy )
{
	if ( !dl || !text )
		return;
	gos_FontAtlasDesc ad;
	if ( !gos_FontGetAtlasDesc( font, &ad ) )
	{
		dl->AddText( ImVec2( x, y ), col, text );   // fallback: default font
		return;
	}
	drawLine( dl, font, ad, x, y, col, text, static_cast<int>(strlen( text )), sx, sy );
}

float DrawWrapped( ImDrawList* dl, HGOSFONT3D font, float x, float y,
	float wrapWidth, ImU32 col, const char* text, float sx, float sy )
{
	if ( !dl || !text )
		return 0.0f;
	gos_FontAtlasDesc ad;
	if ( !gos_FontGetAtlasDesc( font, &ad ) )
	{
		dl->AddText( nullptr, 0.0f, ImVec2( x, y ), col, text,
			nullptr, wrapWidth );
		return ImGui::CalcTextSize( text, nullptr, false, wrapWidth ).y;
	}

	const int regionPx = sx > 0.0f ? static_cast<int>(( wrapWidth / sx )) : 0x7fffffff;
	const int count = static_cast<int>(strlen( text ));
	int pos = 0;
	float curY = y;
	while ( pos < count )
	{
		const int numChars = findBreak( font, ad, text + pos, regionPx );
		if ( numChars <= 0 )
			break;
		drawLine( dl, font, ad, x, curY, col, text + pos, numChars, sx, sy );
		pos += numChars;
		curY += ad.lineSkip * sy;
	}
	return curY - y;
}

float MeasureWrappedHeight( HGOSFONT3D font, const char* text,
	float wrapWidth, float sx, float sy )
{
	if ( !text )
		return 0.0f;
	gos_FontAtlasDesc ad;
	if ( !gos_FontGetAtlasDesc( font, &ad ) )
		return ImGui::CalcTextSize( text, nullptr, false, wrapWidth ).y;

	const int regionPx = sx > 0.0f ? static_cast<int>(( wrapWidth / sx )) : 0x7fffffff;
	const int count = static_cast<int>(strlen( text ));
	int pos = 0;
	int lines = 0;
	while ( pos < count )
	{
		const int numChars = findBreak( font, ad, text + pos, regionPx );
		if ( numChars <= 0 )
			break;
		pos += numChars;
		lines++;
	}
	return lines * ad.lineSkip * sy;
}

void DrawWrappedCentered( ImDrawList* dl, HGOSFONT3D font,
	float x0, float y0, float x1, float y1,
	ImU32 col, const char* text, float sx, float sy )
{
	if ( !dl || !text )
		return;
	gos_FontAtlasDesc ad;
	if ( !gos_FontGetAtlasDesc( font, &ad ) )
	{
		const ImVec2 ts = ImGui::CalcTextSize( text );
		dl->AddText( ImVec2( ( x0 + x1 - ts.x ) * 0.5f,
			( y0 + y1 - ts.y ) * 0.5f ), col, text );
		return;
	}

	const float wrapWidth = x1 - x0;
	const int regionPx = sx > 0.0f ? static_cast<int>(( wrapWidth / sx )) : 0x7fffffff;
	const int count = static_cast<int>(strlen( text ));

	// first pass: line count (legacy calcTextHeight)
	int lines = 0;
	{
		int pos = 0;
		while ( pos < count )
		{
			const int n = findBreak( font, ad, text + pos, regionPx );
			if ( n <= 0 ) break;
			pos += n;
			lines++;
		}
	}
	if ( lines <= 0 )
		return;

	// legacy WrapType-3: y += (region_height - num_lines*font_height)/2,
	// each line x += (region_width - str_width)/2
	float curY = y0 + ( ( y1 - y0 ) - lines * ad.lineSkip * sy ) * 0.5f;
	int pos = 0;
	while ( pos < count )
	{
		const int n = findBreak( font, ad, text + pos, regionPx );
		if ( n <= 0 ) break;
		// width of this line (stop at '\n' inside the segment)
		int lw = 0;
		for ( int i = 0; i < n && text[pos + i] && text[pos + i] != '\n'; ++i )
			lw += glyphAdvance( font, ad, text[pos + i] );
		const float lx = x0 + ( wrapWidth - lw * sx ) * 0.5f;
		drawLine( dl, font, ad, lx, curY, col, text + pos, n, sx, sy );
		pos += n;
		curY += ad.lineSkip * sy;
	}
}

} // namespace HudFont
