#include"vfx.h"
#include <cstddef> // size_t
extern char AlphaTable[];
extern char SpecialColor[];


//extern void AG_ellipse_draw(PANE *pane, LONG xc, LONG yc, LONG width, LONG height, LONG color);
//extern void AG_ellipse_fill(PANE *pane, LONG xc, LONG yc, LONG width, LONG height, LONG color);

#ifndef LINUX_BUILD
static signed int paneY0,paneY1,paneX0,paneX1,DestWidth,DestBuffer,x_top,y_top,Bsquared,TwoBsquared,Asquared,TwoAsquared,var_dx,var_dy,x_vector,line_left,line_right;
static int DrawRoutine;
#endif


void AG_ellipse_draw(PANE *pane, LONG xc, LONG yc, LONG width, LONG height, LONG color)
{
	if( width==0 || height==0 )
	{
		size_t ll = color;
		VFX_line_draw( pane, xc-width, yc-height, xc+width, yc+height, 0, reinterpret_cast<void*>(ll) );
		return;
	}


}





void AG_ellipse_fill(PANE *pane, LONG xc, LONG yc, LONG width, LONG height, LONG color)
{
	if( width==0 || height==0 )
	{
		size_t ll = color;
		VFX_line_draw( pane, xc-width, yc-height, xc+width, yc+height, 0, reinterpret_cast<void*>(ll) );
		return;
	}

}

PANE *xorgPane = 0;
unsigned char gColor = 0;

void orLineCallback (int x, int y)
{
	long result = VFX_pixel_read(xorgPane,x,y);
	result |= gColor;
	VFX_pixel_write(xorgPane,x,y,result);
}

void AG_ellipse_fillOr(PANE *pane, LONG xc, LONG yc, LONG width, LONG height, LONG color)
{
	if( width==0 || height==0 )
	{
		xorgPane = pane;
		gColor = (unsigned char)color;
		VFX_line_draw( pane, xc-width, yc-height, xc+width, yc+height,LD_EXECUTE, &orLineCallback);
		return;
	}


}

void andLineCallback (int x, int y)
{
	long result = VFX_pixel_read(xorgPane,x,y);
	result &= gColor;
	VFX_pixel_write(xorgPane,x,y,result);
}	

void AG_ellipse_fillXor(PANE *pane, LONG xc, LONG yc, LONG width, LONG height, LONG color)
{
	
	if( width==0 || height==0 )
	{
		xorgPane = pane;
		gColor = (unsigned char)color;
		VFX_line_draw( pane, xc-width, yc-height, xc+width, yc+height, LD_EXECUTE, &andLineCallback);
		return;
	}


    // sebi !NB (checkk othes _asm in this file)
}







