#include"vfx.h"

extern char AlphaTable[256*256];
//extern enum { CPU_UNKNOWN, CPU_PENTIUM, CPU_MMX } Processor;
extern void memfill(void *Dest,int Length);
extern void memclear(void *Dest,int Length);

void CopySprite( PANE *pane, void *texture, int X, int Y, int Width, int Height, int Flip, int Shrink );
void AG_shape_fill (PANE *pane, void *shape_table,LONG shape_number, LONG hotX, LONG hotY);
void AG_shape_draw (PANE *pane, void *shape_table,LONG shape_number, LONG hotX, LONG hotY);
void AG_shape_translate_fill (PANE *pane, void *shape_table,LONG shape_number, LONG hotX, LONG hotY);
extern unsigned int lookaside;


int SqrtCount=0;


using DWORD = unsigned int;
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

static unsigned int SourceWidth,tWidth,tHeight,DestWidth;			// Used for code optimizing
//static _int64 xmask=-1;
static unsigned int tempXmax,tempXmin;
static unsigned int minX,minY,maxY,maxX,SkipLeft,NewWidth,StartofLine,StartofClip,EndofClip;
static unsigned int lines,paneX0,paneX1,paneY0,paneY1,SourcePointer,DestPointer;
static signed int X,Y,X1;

static PANE tempPANE;
static WINDOW tempWINDOW;



void AG_shape_transform( PANE *globalPane, void *shapeTable, LONG frameNum, LONG hotX, LONG hotY, void *tempBuffer, LONG reverse, LONG scaleUp )
{


    tempWINDOW.buffer=(UBYTE*)tempBuffer;
    tempWINDOW.x_max=maxX-1;
    tempWINDOW.y_max=maxY-1;
    tempPANE.window=&tempWINDOW;
    tempPANE.x0=0;
    tempPANE.y0=0;
    tempPANE.x1=maxX-1;
    tempPANE.y1=maxY-1;

    //-------------------------------------------------------------------------------------
    // Some armor here.  We have still have a bug somewhere that trashes the shape by
    // the time it gets here.  Armor against for now and make Fatal to try to track down!
#define MAX_X		360
#define MAX_Y		360

    if ((maxX * maxY) >= (MAX_Y * MAX_X))
    {
        return;
    }
    //-------------------------------------------------------------------------------------

    memclear(tempBuffer,maxX*maxY);

    AG_shape_draw ( &tempPANE, shapeTable, frameNum, -X, -Y );

    if(scaleUp)
    {
        if(reverse)
            CopySprite( globalPane, tempBuffer, hotX-X1, hotY+Y, maxX, maxY, reverse, scaleUp );
        else
            CopySprite( globalPane, tempBuffer, hotX+X, hotY+Y, maxX, maxY, reverse, scaleUp );
    }
    else
    {
        if(reverse)
            CopySprite( globalPane, tempBuffer, hotX-(X1>>1), hotY+(Y>>1), maxX, maxY, reverse, scaleUp );
        else
            CopySprite( globalPane, tempBuffer, hotX+(X>>1), hotY+(Y>>1), maxX, maxY, reverse, scaleUp );
    }
}




void AG_shape_translate_transform( PANE *globalPane, void *shapeTable, LONG frameNum, LONG hotX, LONG hotY,void *tempBuffer, LONG reverse, LONG scaleUp )
{

tempWINDOW.buffer=(UBYTE*)tempBuffer;
tempWINDOW.x_max=maxX-1;
tempWINDOW.y_max=maxY-1;
tempPANE.window=&tempWINDOW;
tempPANE.x0=0;
tempPANE.y0=0;
tempPANE.x1=maxX-1;
tempPANE.y1=maxY-1;

//-------------------------------------------------------------------------------------
// Some armor here.  We have still have a bug somewhere that trashes the shape by
// the time it gets here.  Armor against for now and make Fatal to try to track down!
#define MAX_X		360
#define MAX_Y		360

	if ((maxX * maxY) >= (MAX_Y * MAX_X))
	{
		return;
	}
//-------------------------------------------------------------------------------------

memclear(tempBuffer,maxX*maxY);

AG_shape_translate_fill ( &tempPANE, shapeTable, frameNum, -X, -Y );

	if(scaleUp)
	{
		if(reverse)
			CopySprite( globalPane, tempBuffer, hotX-X1, hotY+Y, maxX, maxY, reverse, scaleUp );
		else
			CopySprite( globalPane, tempBuffer, hotX+X, hotY+Y, maxX, maxY, reverse, scaleUp );
	}
	else
	{
		if(reverse)
			CopySprite( globalPane, tempBuffer, hotX-(X1>>1), hotY+(Y>>1), maxX, maxY, reverse, scaleUp );
		else
			CopySprite( globalPane, tempBuffer, hotX+(X>>1), hotY+(Y>>1), maxX, maxY, reverse, scaleUp );
	}
}








void CopySprite( PANE *pane, void *texture, int X, int Y, int Width, int Height, int Flip, int ScaleUp )
{

	return;
}


















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
void AG_shape_fill (PANE *pane, void *shape_table,LONG shape_number, LONG hotX, LONG hotY)
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
void AG_shape_translate_fill (PANE *pane, void *shape_table,LONG shape_number, LONG hotX, LONG hotY)
{

}





