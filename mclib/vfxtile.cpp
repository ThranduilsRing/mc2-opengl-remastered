//-----------------------------------------------------------
// 
// FASA Interactive Technologies
//
// VFX Tile Draw Routine
//
// Started April 30, 1996
// FFS
//
//-----------------------------------------------------------

//-----------------------------------------------------------
// Include Files

#include "vfxtile_internal.h"


static unsigned long pwXMax,count;		// Must be static for assembly optimizations
static unsigned char *FadeTable;		// Must be static for assembly optimizations

//-----------------------------------------------------------
struct tileStruct
{
	byte tileHotSpotX;
	byte tileHotSpotY;
	byte numScanLines;
	byte numScanColumns;
};

//-----------------------------------------------------------
struct newShapeStruct
{
	long header;
	short tileHotSpotX;
	short tileHotSpotY;
	short numScanLines;
	short numScanColumns;
};

constexpr unsigned int FULLY_CLIPPED = 0xCDCF0001;
constexpr unsigned int FORMAT_SPEC = 0xFEEDABBA;

#define BLACK				0x10101010

//-----------------------------------------------------------
// This one is called if the cameraScale is 100.  No drop
// of any pixels.  Straight BLT to screen.
long VFX_nTile_draw (PANE* pane, void *tile, LONG hotX, LONG hotY, MemoryPtr fadeTable)
{

	return(0x00);
}

//----------------------------------------------------------------------------
//
// long VFX_shape_origin(void *shape_table, LONG shape_number);
//
// Returns hotspot of the shape (in pixels) relative to the upper left bounds).
// (E)AX=x E(AX)=y.
//
//----------------------------------------------------------------------------

long VFX_shape_origin(void *shape_table, LONG shape_number)
{
	long result = -1;

	return result;
}

//----------------------------------------------------------------------------
//
// long VFX_shape_resolution(void *shape_table, LONG shape_number);
//
// Returns x,y resolution (image size) in pixels.  (E)AX=x E(AX)=y.
//
//----------------------------------------------------------------------------

long VFX_shape_resolution(void *shape_table, LONG shape_number)
{
	long result = 0;

	return result;
}

//----------------------------------------------------------------------------
//
// long VFX_shape_minxy(void *shape_table, LONG shape_number);
//
// Returns min x,min y in pixels.  (E)AX=x E(AX)=y.
//
//---------------------------------------------------------------------------

long VFX_shape_minxy(void *shape_table, LONG shape_number)
{
	long result = 0;

	return result;
}

//;----------------------------------------------------------------------------
//;
//; long VFX_shape_bounds(void *shape_table, LONG shape_number);
//;
//; Returns width,height of the shape (including transparent areas) in pixels.  (E)AX=x E(AX)=y.
//; (E)AX=x E(AX)=y.
//;
//;----------------------------------------------------------------------------

int VFX_shape_bounds (void *shape_table, long shape_number)
{
	int boundsResult = -1;

    assert(0);
	return boundsResult;
}

//;----------------------------------------------------------------------------
//;
//; long VFX_shape_count(void *shape_table);
//;
//; Returns number of shapes references in the specified shape table. Each
//; shape may have one or more references.
//;
//;----------------------------------------------------------------------------

long VFX_shape_count (void *shape_table)
{
	long countResult = 0;

	return countResult;
}
