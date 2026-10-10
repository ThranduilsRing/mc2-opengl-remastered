#include"vfx.h"

//extern enum { CPU_UNKNOWN, CPU_PENTIUM, CPU_MMX, CPU_KATMAI } Processor;

extern char AlphaTable[];
static unsigned int SourceWidth,tWidth,tHeight,DestWidth;			// Used for code optimizing
//static _int64 xmask=-1;



//
//
// Draws a status bar
//
//
void AG_StatusBar( PANE *pane, int X0, int Y0, int X1, int Y1, int Color, int Width )
{
	static long TopY,BottomY;

	DestWidth = pane->window->x_max+1;

	long paneX0 = (pane->x0 < 0) ? 0 : pane->x0;
	long paneY0 = (pane->y0 < 0) ? 0 : pane->y0;
	long paneX1 = (pane->x1 >= (long)DestWidth) ? pane->window->x_max : pane->x1;
	long paneY1 = (pane->y1 >= (pane->window->y_max+1)) ? pane->window->y_max : pane->y1;

	if (X0>X1)
	{
		Width=-Width;
		TopY=X0;
		X0=X1;
		X1=TopY;
	}
	if (Y0>Y1)
	{
		TopY=Y0;
		Y0=Y1;
		Y1=TopY;
	}

	TopY=Y0;
	BottomY=Y1;

	if ( ( X0 >= paneX1 ) ||
		 ( Y0 >= paneY1 ) ||
		 ( X1 <= paneX0) ||
		 ( Y1 <= paneY0))
		return;

	if( X0<paneX0 )
	{
		Width-=paneX0-X0;
		X0=paneX0;
	}

	if( Y0<paneY0 )
		Y0=paneY0;
		
	if( X1>paneX1 )
		X1=paneX1;

	if( Y1>paneY1 )
		Y1=paneY1;

	if( (X0+Width) >= X1 )
		Width=X1-X0-2;

	if( Width<0 )
		Width=0;

	if( X1-X0 < 3)
		return;

	UBYTE* DestPointer = pane->window->buffer + X0 + Y0*DestWidth;
}





//
//
// Writes a single pixel
//
//
void AG_pixel_write (PANE *pane, LONG x, LONG y, ULONG color)
{
	int X=x+pane->x0;
	int Y=y+pane->y0;

	if( X>pane->x0 && X<pane->x1 && Y>pane->y0 && Y<pane->y1 )
		*(pane->window->buffer + X + Y*(pane->window->x_max+1))=(unsigned char)color;
}



//
//
// Draws a sprite - any colors marked 255 will be drawn transparent
//
//
//
long DrawTransparent( PANE *pane, WINDOW *texture, int X, int Y, int Width, int Height )
{
	DestWidth = pane->window->x_max+1;

	long paneX0 = (pane->x0 < 0) ? 0 : pane->x0;
	long paneY0 = (pane->y0 < 0) ? 0 : pane->y0;
	long paneX1 = (pane->x1 >= (long)DestWidth) ? pane->window->x_max : pane->x1;
	long paneY1 = (pane->y1 >= (pane->window->y_max+1)) ? pane->window->y_max : pane->y1;

	X+=paneX0;
	Y+=paneY0;
	
	if ( ( X >= paneX1 ) ||
		 ( Y >= paneY1 ) ||
		 ( X <= (paneX0 - Width)) ||
		 ( Y <= (paneY0 - Height)))
		return(1);

	UBYTE* SourcePointer = texture->buffer;
	
	if( X<paneX0 )
	{
		Width-=paneX0-X;
		SourcePointer+=paneX0-X;
		X=paneX0;
	}

	if( Y<paneY0 )
	{
		Height-=paneY0-Y;
		SourcePointer+=(paneY0-Y)*(texture->x_max+1);
		Y=paneY0;
	}
		
	if( X+Width > (paneX1+1) )
		Width= paneX1+1-X;

	if( Y+Height > (paneY1+1) )
		Height= paneY1+1-Y;

	UBYTE* DestPointer = pane->window->buffer + X + Y*DestWidth;

	SourceWidth=texture->x_max+1;
	tWidth=Width;
	tHeight=Height;

	return 0;
}



