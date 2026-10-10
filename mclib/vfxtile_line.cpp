// VFX: VFX_line_draw, the clipped line rasteriser entry point (an assert stub; the x86 asm body was removed).
// Declared in the class headers; shared names in vfxtile_internal.h.
#include "vfxtile_internal.h"



//;----------------------------------------------------------------------------
//;
//; int cdecl VFX_line_draw (PANE *panep, int x0, int y0, int x1, int y1,
//;                                                   int mode, int parm);
//;
//; This function clips and draws a line to a pane.
//;
//; The panep parameter specifies the pane.
//;
//; The x0 and y0 parameters specify the initial endpoint of the line.
//; The x1 and y1 parameters specify the final endpoint of the line.
//;
//; The mode parameter specifies the operation to perform on each point
//; in the line.
//;
//; If mode is...   parm specifies...
//;
//;   DRAW            a color
//;   TRANSLATE       the address of a color translation table
//;   EXECUTE         the address of a caller-provided function
//;
//; If mode is DRAW (0), the line is drawn in a solid color specified by 
//; parm. 
//;
//; If mode is TRANSLATE (1), the line is drawn with color translation. 
//; Each pixel along the path of the line is replaced with the correspond-
//; ing entry in the color translation table specified by parm.
//;
//; If mode is EXECUTE (2), the line is drawn with the aid of a callback 
//; function specifed by parm.  For each point on the line, VFX_line_draw() 
//; executes the callback function, passing it the coordinates of the point.
//;
//; The callback function must use cdecl parameter passing, and its para-
//; meter list must be (int x, int y).  The function's return type is not
//; important; VFX_line_draw() ignores the return value (if any).  
//;
//; VFX_line_draw() clips the line to the pane.  The locus of the clipped 
//; line is the same for all modes and is guaranteed to be identical to the 
//; intersection of the loci of the unclipped line and the pane.  Moreover, 
//; plotting always proceeds from (x0,y0) to (x1,y1) regardless of the 
//; relative orientation of the two points.
//;
//; The locus of the clipped line consists of all points in the pane whose 
//; minor-axis distance* from the ideal line is less than or equal to 1/2,
//; with the following exception.  In places where the ideal line passes 
//; exactly halfway between two pixels which share the same major-axis co-
//; ordinate, only one of the two points is plotted.  The selection method 
//; is unspecified, but is consistent throughout the line.
//;
//; * The minor-axis distance from a point P to a line L is the absolute 
//;   difference between the minor-axis coordinates of P and Q where Q is
//;   the point on L having the same major-axis coordinate as P.  (Here,
//;   major-axis and minor axis are determined by L.  The major axis is
//;   the axis in which the endpoints of L differ the most.  Likewise the
//;   minor-axis is the one in which the endpoints differ the least).
//;
//; VFX_line_draw is reentrant, so callback functions can use it.
//;
//;
//; Examples:
//;
//;    #define HELIOTROPE 147
//;    UBYTE color_negative[256];
//;    void cdecl DrawDiamond (int x, int y);
//;
//;    VFX_line_draw (pane, Px, Py, Qx, Qy, DRAW, HELIOTROPE);
//;       draws a line from (Px,Py) to (Qx,Qy) using the color HELIOTROPE.
//;
//;    VFX_line_draw (pane, Px, Py, Qx, Qy, TRANSLATE, (int) color_negative);
//;       draws a line from (Px,Py) to (Qx,Qy) replacing each pixel with its
//;       color negative (as specified by the table color_negative).
//;
//;    VFX_line_draw (pane, Px, Py, Qx, Qy, EXECUTE, (int) DrawDiamond);
//;       draws a line of diamonds from (Px,Py) to (Qx,Qy) using the
//;       caller-provided function DrawDiamond().
//;
//; Return values:
//;
//;   -2: pane was malformed or completely outside its window
//;   -1: window was malformed
//;    0: all of the line was inside the pane and was drawn without clipping
//;    1: some of the line was inside the pane and was drawn after clipping
//;    2: the line was completely outside the pane and was not drawn
//;
//;----------------------------------------------------------------------------

LONG VFX_line_draw (PANE *panep, LONG x0, LONG y0, 
                                 LONG x1, LONG y1, LONG mode, void* parm)

{

	long	lineResult = 0;
	// The x86 inline-assembly rasteriser that used to follow is not built: LINUX_BUILD is defined for every target.
	assert(0 && "VFX_line_draw is not implemented");
		return lineResult;
}
