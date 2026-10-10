#include"dstd.h"
#include"vfx.h"
#include<stdio.h>

extern char AlphaTable[256*256];

using SHAPEHEADER = struct
{

	DWORD	bounds;
	DWORD	origin;
	DWORD	xmin;
	DWORD	ymin;
	DWORD	xmax;
	DWORD	ymax;

};

//
// Sprite format
//
//
// Marker.
// 0			End of line
// 1			Skip next bytes
// Bit 0 = 0	Repeat next byte [7654321] times
// Bit 0 = 1	String packet [7654321] bytes
//

unsigned int lookaside;
static unsigned int tempXmax,tempXmin;
static unsigned int minX,minY,maxY,SkipLeft,NewWidth,StartofLine,StartofClip,EndofClip;
static unsigned int lines,DestWidth,paneX0,paneX1,paneY0,paneY1;




/*
;
; int cdecl VFX_shape_draw (PANE *panep, void *shape_table,
;                           long shape_number,int hotX, int hotY)
;
; This function clips and draws a shape to a pane.
; 
; The panep parameter specifies the pane.
;
; The shape parameter specifies the shape, which must be in VFX Shape format.
;
; The hotX and hotY parameters specify the location where the shape is to be
; drawn.  The shape's hot spot will end up at the specified location.
;
*/
void AG_shape_draw (PANE *pane, void *shape_table,LONG shape_number, LONG hotX, LONG hotY)
{
}






































/*
;----------------------------------------------------------------------------
;
; void cdecl VFX_shape_lookaside (unsigned char *table)
;
; Establishes a color translation lookaside table for use by future calls
; to VFX_shape_translate_draw().
; 
; table points to a 256-byte table specifying remap values for each of
; the 256 possible palette indices.  The table is copied to static local 
; memory, and need not remain valid after the call.
;
;----------------------------------------------------------------------------
*/
void AG_shape_lookaside( UBYTE *table )
{
}

/*
;----------------------------------------------------------------------------
;
; int cdecl VFX_shape_translate_draw (PANE *panep, void *shape_table,
;                           long shape_number,int hotX, int hotY)
;
; This function clips and draws a shape to a pane.  It is identical to 
; VFX_shape_draw(), except that each pixel written is translated through a
; 256-byte table which was specified by a prior call to VFX_shape_lookaside().
; 
; The panep parameter specifies the pane.
;
; The shape parameter specifies the shape, which must be in VFX Shape format.
;
; The hotX and hotY parameters specify the location where the shape is to be
; drawn.  The shape's hot spot will end up at the specified location.
;
; For more information, see the "VFX Shape Format Description".
;
; Return values:
;
;    0: OK
;   -1: Bad window
;   -2: Bad pane
;   -3: Shape off pane
;   -4: Null shape
;
;----------------------------------------------------------------------------
*/
void AG_shape_translate_draw (PANE *pane, void *shape_table,LONG shape_number, LONG hotX, LONG hotY)
{
}





