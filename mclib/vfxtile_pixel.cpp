// VFX: VFX_pixel_write and VFX_pixel_read.
// Declared in the class headers; shared names in vfxtile_internal.h.
#include "vfxtile_internal.h"


//----------------------------------------------------------------------------
//
// int cdecl VFX_pixel_write (PANE *panep, int x, int y, UBYTE color)
//
//     This function writes a single pixel.
//
// The panep parameter specifies the pane containing the pixel to be written.
// The x and y parameters specify the pixel coordinates.
// The color parameter specifies the color to write to the pixel.
//                                          
// Return values:
//
//    0..255:
//       Pixel value prior to write.
//
//   -1: Bad window.
//       The height or width of the pane's window is less than one.
//
//   -2: Bad pane.
//       The height or width of the pane is less than one.
//
//   -3: Off pane.
//       The specified pixel is off the pane.
//                                          
//----------------------------------------------------------------------------

LONG VFX_pixel_write (PANE *panep, LONG x, LONG y, ULONG color)
{
		int result = 0;
	assert(0 && "VFX_pixel_write is not implemented");
			return(result);
}


//----------------------------------------------------------------------------
//
// int cdecl VFX_pixel_read (PANE *panep, int x, int y)
//
//     This function reads a single pixel.
//
// The panep parameter specifies the pane containing the pixel to be written.
// The x and y parameters specify the pixel coordinates.
//                                          
// Return values:
//
//    0..255:
//       Pixel value.
//
//   -1: Bad window.
//       The height or width of the pane's window is less than one.
//
//   -2: Bad pane.
//       The height or width of the pane is less than one.
//
//   -3: Off pane.
//      The specified pixel is off the pane.
//                                         
//---------------------------------------------------------------------------

LONG VFX_pixel_read (PANE *panep, LONG x, LONG y)
{
		int result = 0;
	assert(0 && "VFX_line_draw is not implemented");
	return(result);
}
