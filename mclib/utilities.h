//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

// stuff I need all over the place
#ifndef UTILITIES_H
#define UTILITIES_H

#include<gameos.hpp>
#include<string.h>
#include<string>

//#include<stdio.h>

class FitIniFile;


class StaticInfo
{
public:

	void init( FitIniFile& file, const char* blockName, long hiResOffsetX = 0, long hiResOffsetY = 0, DWORD neverFlush = 0x2 );
	void render();
	bool isInside( int mouseX, int mouseY );

	void setLocation( float  newX, float newY );
	void move( float deltaX, float deltaY );

	void setNewUVs( float uLeft, float vTop, float uRight, float vBottom );

	float width(){ return location[2].x - location[0].x; }
	float height(){ return location[2].y - location[0].y; }
	
	void getData(unsigned char * buffer);

	void showGUIWindow( bool bShow );

	void setColor( long newColor );

	StaticInfo(){}
	~StaticInfo();

	unsigned long textureHandle;
	gos_VERTEX	location[4];
	long u, v, uWidth, vHeight;

	unsigned long textureWidth; // textures are square
};



typedef struct _GUI_RECTd
{
	long left;
	long top;
	long right;
	long bottom;
} GUI_RECT;


void drawEmptyRect( const GUI_RECT& rect, unsigned long leftBorderColor = 0xffffffff,
	 unsigned long rightBorderColor = 0xff000000 );


void drawRect( const GUI_RECT& rect, DWORD color );

// DEFS-METER-BRIDGE-1: optional redirect for drawRect/drawEmptyRect installed by
// the gui layer (aObject GUI bridge). When set and it returns true, the legacy
// gos quad draw is skipped (the rect was drawn on the ImGui HUD layer instead).
// filled=true => drawRect; filled=false => drawEmptyRect (border color passed).
// Kept as a function pointer so mclib does not link against gui/GuiRuntime
// (tools that use mclib without the game's UI stack are unaffected).
extern bool (*g_uiBridgeRectHook)( const GUI_RECT& rect, unsigned long color, bool filled );

void drawShadowText( long colorTop, long colorShadow, HGOSFONT3D font, 
					long left, long top, bool proportional, const char* text, bool bBold, float scale );

void drawShadowText( long colorTop, long colorShadow, HGOSFONT3D font, 
					long left, long top, bool proportional, const char* text, bool bold, float scale,
					long xOffset, long yOffset);

void drawShadowText( long colorTop, long colorShadow, HGOSFONT3D font, 
					long left, long top, long right, long bottom, bool proportional, const char* text, bool bold, float scale,
					long xOffset, long yOffset);



long interpolateColor( long color1, long color2, float percent );

inline long reverseRGB( long oldVal ) 
{
	return ( (oldVal & 0xff000000) | ((oldVal & 0xff0000) >> 16) | (oldVal & 0xff00) | ((oldVal & 0xff) << 16) );
}


// The resource subsystem owns this handle; explicit handles select other tables.
extern HSTRRES gosResourceHandle;




// Owns the complete provider value, including valid empty strings and missing-ID
// markers. The provider interface exposes no missing/valid-empty status.
// A null handle or null provider result returns empty with one rejection line.
// No fixed content limit, cache, or retained provider pointer.
std::string loadResourceString(unsigned int id, HSTRRES handle = gosResourceHandle);

// Capacity includes the terminator. Returns the complete byte length or 0 on
// rejection, so a prefix cannot be mistaken for a valid resource value.
// Content length >= capacity is rejected with one line naming the ID, source
// length and capacity. A writable positive-capacity destination is empty and
// NUL-terminated on rejection; capacity 1 accepts only empty content.
// Null destinations/nonpositive capacities, null handles and null provider
// results return 0 with one rejection line.
// The caller must supply capacity writable bytes; unused tail bytes are zeroed.
int cLoadString(unsigned int id, char* destination, int capacity,
                HSTRRES handle = gosResourceHandle);

#endif
