#define UTILITIES_CPP
/*************************************************************************************************\
Utilities.cpp			: Implementation of the Utilities component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/
#include"utilities.h"
#include "texture_assets.h"

#include"txmmgr.h"
#include"inifile.h"
#include"mclib.h"
#include"../GameOS/gameos/gos_profiler.h"
#include "mc2fmt.h"


#pragma warning(disable:4514)

// DEFS-METER-BRIDGE-1: see utilities.h. Installed by gui/asystem.cpp when the
// aObject GUI bridge is in use; null in tools without the UI stack.
bool (*g_uiBridgeRectHook)( const GUI_RECT& rect, unsigned long color, bool filled ) = 0;

void drawRect( const GUI_RECT& area, DWORD color )
{
	if ( color & 0xff000000 )
	{
		if ( g_uiBridgeRectHook && g_uiBridgeRectHook( area, color, true ) )
			return;
		GUI_RECT clientArea = area;

		if ( clientArea.left < 0 )
			clientArea.left = 0;

		if ( clientArea.right < 0 )
			clientArea.right = 0;

		if ( clientArea.top < 0 )
			clientArea.top = 0;

		if ( clientArea.bottom < 0 )
			clientArea.bottom = 0;

		if ( clientArea.left >= Environment.screenWidth )
			clientArea.left = Environment.screenWidth;

		if ( clientArea.right >= Environment.screenWidth )
			clientArea.right = Environment.screenWidth;

		if ( clientArea.top >= Environment.screenHeight )
			clientArea.top = Environment.screenHeight;

		if ( clientArea.bottom >= Environment.screenHeight )
			clientArea.bottom = Environment.screenHeight;
		
		
		gos_VERTEX v[4];

		memset( v,0,sizeof(v) );
		v[0].rhw =  v[1].rhw = v[2].rhw = v[3].rhw = 1.0;

		v[0].argb =  v[1].argb = v[2].argb = v[3].argb = color;

		v[0].x = static_cast<float>(clientArea.left);
		v[0].y = static_cast<float>(clientArea.top);

		v[1].x = static_cast<float>(clientArea.left);
		v[1].y = static_cast<float>(clientArea.bottom);

		v[2].x = static_cast<float>(clientArea.right);
		v[2].y = static_cast<float>(clientArea.bottom);

		v[3].x = static_cast<float>(clientArea.right);
		v[3].y = static_cast<float>(clientArea.top);

		gos_SetRenderState( gos_State_Texture, 0 );
		gos_SetRenderState( gos_State_ZWrite, 0 );
		gos_SetRenderState( gos_State_ZCompare, 0 );
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_DrawQuads( v, 4 );
	}
}

	
void drawEmptyRect( const GUI_RECT& area, unsigned long leftTopBorderColor,
							unsigned long rightBottomBorderColor )
{
	if ( g_uiBridgeRectHook && g_uiBridgeRectHook( area, leftTopBorderColor, false ) )
		return;

	gos_VERTEX v[4];

	memset( v,0,sizeof(v) );
	v[0].rhw =  v[1].rhw = v[2].rhw = v[3].rhw = 1.0;

	GUI_RECT clientArea = area;

	if (   (clientArea.left < 0 && clientArea.right < 0 )
		|| (clientArea.right > Environment.screenWidth && clientArea.left > Environment.screenWidth )
		|| (clientArea.top < 0 && clientArea.bottom < 0 )
		|| ( clientArea.top > Environment.screenHeight && clientArea.bottom > Environment.screenHeight ) )
		return;

	if ( clientArea.left < 0 )
		clientArea.left = 0;

	if ( clientArea.right < 0 )
		clientArea.right = 0;

	if ( clientArea.top < 0 )
		clientArea.top = 0;

	if ( clientArea.bottom < 0 )
		clientArea.bottom = 0;

	if ( clientArea.left >= Environment.screenWidth - 1 )
		clientArea.left = Environment.screenWidth - 1;

	if ( clientArea.right >= Environment.screenWidth - 1 )
		clientArea.right = Environment.screenWidth - 1;

	if ( clientArea.top >= Environment.screenHeight - 1 )
		clientArea.top = Environment.screenHeight - 1;

	if ( clientArea.bottom >= Environment.screenHeight - 1 )
		clientArea.bottom = Environment.screenHeight - 1;

	gos_SetRenderState( gos_State_Texture, 0 );
	gos_SetRenderState( gos_State_ZWrite, 0 );
	gos_SetRenderState( gos_State_ZCompare, 0 );
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );


	v[0].x		= static_cast<float>((clientArea.left));
	v[0].y		= static_cast<float>((clientArea.top));
	v[0].argb	= leftTopBorderColor;

	v[1].x		= static_cast<float>((clientArea.right));
	v[1].y		= static_cast<float>((clientArea.top));
	v[1].argb	= leftTopBorderColor;

	v[2] = v[0];

	v[3].x		= static_cast<float>((clientArea.left));
	v[3].y		= static_cast<float>((clientArea.bottom));
	v[3].argb	= leftTopBorderColor;

	gos_DrawLines( v, 4 );
	
	v[0].x		= static_cast<float>((clientArea.right));
	v[0].y		= static_cast<float>((clientArea.top));
	v[0].argb	= rightBottomBorderColor;

	v[1].x		= static_cast<float>((clientArea.right));
	v[1].y		= static_cast<float>((clientArea.bottom));
	v[1].argb	= rightBottomBorderColor;

	v[3] = v[1];

	v[3].x += 1;

	v[2].x		= static_cast<float>((clientArea.left));
	v[2].y		= static_cast<float>((clientArea.bottom)) + .1/256.f;
	v[2].argb	= rightBottomBorderColor;


	gos_DrawLines( v, 4 );

}

// STATIC STUFF


StaticInfo::~StaticInfo()
{
	if (mcTextureManager && textureHandle)
		mc2::textures::release( textureHandle );
}

void StaticInfo::render()
{
	unsigned long gosID = mc2::textures::sourceId( textureHandle );
	gos_SetRenderState( gos_State_Texture, gosID );
	gos_SetRenderState(gos_State_Filter, gos_FilterNone);
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
	gos_SetRenderState( gos_State_Clipping, 1);
	gos_DrawQuads( location, 4 );
	
}

void StaticInfo::showGUIWindow( bool bShow )
{
	int mask = bShow ? 0xff000000 : 0x00ffffff;
	for ( int i = 0; i < 4; i++ )
	{
		if ( bShow )
			location[i].argb |= mask;
		else 
			location[i].argb &= mask;
	}
}

bool StaticInfo::isInside( int mouseX, int mouseY )
{
	if ( (location[0].x)  <= mouseX && 
		 location[3].x >= mouseX && 
		 location[0].y <= mouseY &&
		 location[1].y >= mouseY )
		 return true;

	return false;
	
}

//Fills the buffer with the bitmap data.
// Used by new mouse cursor draw routines.
void StaticInfo::getData(unsigned char * buffer)
{
	if ((vHeight > 32) || (uWidth > 32))
	{
		memset(buffer,0,sizeof(DWORD) * 32 * 32);
		return;
	}
	
    memset(buffer,0,sizeof(DWORD)*32*32);
    const auto image=mc2::textures::image(textureHandle);
    if (!image || image->vkFormat!=37) return;
    const auto x0=DWORD(location[0].u*image->width), y0=DWORD(location[0].v*image->height);
    if (x0+uWidth>DWORD(image->width) || y0+vHeight>DWORD(image->height)) return;
    auto* out=reinterpret_cast<DWORD*>(buffer);
    for (long y=0;y<vHeight;++y) for (long x=0;x<uWidth;++x) {
        const auto* pixel=image->pixels.data()+4*(x0+x+(y0+y)*image->width);
        *out++=(static_cast<DWORD>(pixel[3])<<24)|(static_cast<DWORD>(pixel[0])<<16)|(static_cast<DWORD>(pixel[1])<<8)|pixel[2];
    }

}

void StaticInfo::init( FitIniFile& file, const char* blockName, long hiResOffsetX, long hiResOffsetY, DWORD neverFlush )
{
	ZoneScopedN("StaticInfo::init");
	memset( location, 0, sizeof( location ) );
	char fileName[256];
	textureHandle = 0;
	textureWidth = 0; 
	
	{
		ZoneScopedN("StaticInfo::init seekBlock");
		if ( NO_ERR != file.seekBlock( blockName ) )
		{
			char errBuffer[256];
			mc2::fmt::to_buf(errBuffer, "couldn't find static block {}", mc2::fmt::safe(blockName));
			Assert( 0, 0, errBuffer );
			return;
		}
	}

	long x, y, width, height;
	{
		ZoneScopedN("StaticInfo::init layoutRead");
		file.readIdLong( "XLocation", x );
		file.readIdLong( "YLocation", y );

		x += hiResOffsetX;
		y += hiResOffsetY;

		file.readIdLong( "Width", width );
		file.readIdLong( "Height", height );

		file.readIdString( "FileName", fileName, 32 );
	}

	if ( !textureHandle )
	{
		FullPathFileName fullPath;
		S_strlwr( fileName );
		fullPath.init( artPath, fileName, ".tga" );
		int ID;
		{
			ZoneScopedN("StaticInfo::init loadTexture");
			ID = mc2::textures::load( fullPath, gos_Texture_Alpha, 0, 0, neverFlush );
		}
		textureHandle = ID;
		DWORD logicalWidth = 0;
		DWORD logicalHeight = 0;
		if ( mc2::textures::logicalSize( ID, logicalWidth, logicalHeight ) )
			textureWidth = logicalWidth;
		else
		{
            textureWidth=RenderCore::textureService().dimensions(RenderCore::textureService().handleOfNode(ID)).logicalWidth;
		}
	}

	bool bRotated = 0;

	{
		ZoneScopedN("StaticInfo::init uvRead");
		file.readIdLong( "UNormal", u );
		file.readIdLong( "VNormal", v );
		file.readIdLong( "UWidth", uWidth );
		file.readIdLong( "VHeight", vHeight );
		file.readIdBoolean( "texturesRotated", bRotated );
	}

	{
		ZoneScopedN("StaticInfo::init setupVertices");
	for ( int k = 0; k < 4; k++ )
	{
		location[k].argb = 0xffffffff;
		location[k].frgb = 0;
		location[k].x = x;
		location[k].y = y;
		location[k].z = 0.f;
		location[k].rhw = .5;
		location[k].u = static_cast<float>(u)/static_cast<float>(textureWidth) + (.1f / static_cast<float>(textureWidth));
		location[k].v = static_cast<float>(v)/static_cast<float>(textureWidth) + (.1f / static_cast<float>(textureWidth));
	}

	location[3].x = location[2].x = x + width;
	location[2].y = location[1].y = y + height;

	location[2].u = location[3].u = (static_cast<float>((u + uWidth)))/(static_cast<float>(textureWidth)) + (.1f / static_cast<float>(textureWidth));
	location[1].v = location[2].v = (static_cast<float>((v + vHeight)))/(static_cast<float>(textureWidth)) + (.1f / static_cast<float>(textureWidth));

	if ( bRotated )
	{

		location[0].u = (u + uWidth)/static_cast<float>(textureWidth) + (.1f / static_cast<float>(textureWidth));;
		location[1].u = u/static_cast<float>(textureWidth) + (.1f / static_cast<float>(textureWidth));;
		location[2].u = u/static_cast<float>(textureWidth) + (.1f / static_cast<float>(textureWidth));
		location[3].u = (u + uWidth)/static_cast<float>(textureWidth) + (.1f / static_cast<float>(textureWidth));

		location[0].v = v/static_cast<float>(textureWidth) + (.1f / static_cast<float>(textureWidth));;
		location[1].v = v/static_cast<float>(textureWidth) + (.1f / static_cast<float>(textureWidth));;
		location[2].v = (v + vHeight)/static_cast<float>(textureWidth) + (.1f / static_cast<float>(textureWidth));;
		location[3].v = (v + vHeight)/static_cast<float>(textureWidth) + (.1f / static_cast<float>(textureWidth));;
	}
	}

}

void StaticInfo::setLocation( float  newX, float newY )
{
	float height = location[1].y - location[0].y;
	float width = location[3].x - location[0].x;

	location[0].x = location[1].x = newX;
	location[2].x = location[3].x = newX + width;
	location[0].y = location[3].y = newY;
	location[1].y = location[2].y = newY + height;
}

void StaticInfo::move( float deltaX, float deltaY )
{
	for ( int i = 0; i < 4; i++ )
	{
		location[i].x += deltaX;
		location[i].y += deltaY;
	}
}

void StaticInfo::setColor( long newColor )
{
	for ( int i = 0; i < 4; i++ )
		location[i].argb = newColor;

}

void StaticInfo::setNewUVs( float uLeft, float vTop, float uRight, float vBottom )
{
	location[0].u = location[1].u = uLeft;
	location[2].u = location[3].u = uRight;
	location[0].v = location[3].v = vTop;
	location[1].v = location[2].v = vBottom;
}

void drawShadowText( long colorTop, long colorShadow, HGOSFONT3D font, long left, long top,  
					bool proportional, const char* text, bool bold, float scale )
{
	drawShadowText( colorTop, colorShadow, font, left, top, proportional, text, bold, scale, -1, 1 );
}

void drawShadowText( long colorTop, long colorShadow, HGOSFONT3D font, 
					long left, long top, bool proportional, const char* text, bool bold, float scale,
					long xOffset, long yOffset)
{
	gos_TextSetAttributes( font, colorShadow, scale, false, proportional, bold, false, 0 );
	gos_TextSetRegion( 0, 0, Environment.screenWidth, Environment.screenHeight );
	gos_TextSetPosition( left + xOffset, top + yOffset );
	gos_TextDraw( text );
	gos_TextSetAttributes( font, colorTop, scale, false, proportional, bold, false, 0 );
	gos_TextSetPosition( left, top );
	gos_TextDraw( text );

}

void drawShadowText( long colorTop, long colorShadow, HGOSFONT3D font, 
					long left, long top, long right, long bottom, bool proportional, const char* text, bool bold, float scale,
					long xOffset, long yOffset)
{
	gos_TextSetAttributes( font, colorShadow, scale, true, proportional, bold, false, 2 );
	gos_TextSetRegion( left + xOffset, top + yOffset, right + xOffset, bottom + yOffset);
	gos_TextSetPosition( left + xOffset, top + yOffset );
	gos_TextDraw( text );
	gos_TextSetAttributes( font, colorTop, scale, true, proportional, bold, false, 2 );
	gos_TextSetRegion( left, top, right, bottom );
	gos_TextSetPosition( left, top );
	gos_TextDraw( text );
}


long interpolateColor( long color1, long color2, float percent )
{
	long color = 0xffffffff;
	if ( percent > 1.0 )
		color = color2;
	else if ( percent < 0.0 )
		color = color1;
	else
	{
		long alphaMin = (color1 >> 24) & 0xff;
		long alphaMax = (color2 >> 24)& 0xff;

		long newAlpha = static_cast<float>(alphaMin) + (static_cast<float>((alphaMax - alphaMin)))*percent;
		
		long redMin = (color1 & 0x00ff0000)>>16;
		long redMax = (color2 & 0x00ff0000)>>16;
		long newRed = static_cast<float>(redMin) + (static_cast<float>((redMax - redMin)))*percent;
		
		long greenMin = (color1 & 0x0000ff00)>>8;
		long greenMax = (color2 & 0x0000ff00)>>8;
		long newGreen = static_cast<float>(greenMin) + (static_cast<float>((greenMax - greenMin)))*percent;
		
		long blueMin = color1 & 0x000000ff;
		long blueMax = color2 & 0x000000ff;
		long newBlue = static_cast<float>(blueMin) + (static_cast<float>((blueMax - blueMin)))*percent;

		color = newBlue + (newGreen << 8) + (newRed << 16) + (newAlpha <<24);
	}

	return color;
}


