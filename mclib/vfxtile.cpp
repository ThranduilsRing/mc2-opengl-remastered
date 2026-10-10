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

#define FULLY_CLIPPED		0xCDCF0001
#define FORMAT_SPEC			0xFEEDABBA

#define BLACK				0x10101010

//-----------------------------------------------------------
// This one is called if the cameraScale is 100.  No drop
// of any pixels.  Straight BLT to screen.
long VFX_nTile_draw (PANE* pane, void *tile, LONG hotX, LONG hotY, MemoryPtr fadeTable)
{

#ifdef LINUX_BUILD
#else
	//-----------------------------------------
	// Format of tile shape data is NEW!!
	// Byte -- Hot Spot X.
	// Byte -- Hot Spot Y.
	// Byte -- NumScanLines.  (height)
	// Byte -- NumScanColumns. (width)
	// Word -- One word for each scanline indicating offset into main data for that scanline.
	// Bytes -- Main Shape data.  Each scanline pointed to by above table.
	// The main difference between .fsx and .gsx files are in the main data.
	// Because there is no end scanline marker, the length of each Blt can be
	// determined and a MUCH faster rep movsd can be used to Blt the data.
	//------------------------------------------------------

	//-------------------------------------------------------
	// Create Important Data from tile data.
	MemoryPtr tileData = (MemoryPtr)tile + sizeof(tileStruct);
	tileStruct *tileInfo = (tileStruct *)tile;
	
	unsigned long *yOffsetTable = (unsigned long *)(tileData);

	pwXMax = pane->window->x_max+1;

	long paneX0 = (pane->x0 < 0) ? 0 : pane->x0;
	long paneY0 = (pane->y0 < 0) ? 0 : pane->y0;
	long paneX1 = (pane->x1 >= (long)pwXMax) ? pane->window->x_max : pane->x1;
	long paneY1 = (pane->y1 >= (pane->window->y_max+1)) ? pane->window->y_max : pane->y1;

	long scanLines = tileInfo->numScanLines;
	long scanCol = tileInfo->numScanColumns;

	long topLeftX = (hotX + paneX0) - tileInfo->tileHotSpotX;
	long topLeftY = (hotY + paneY0) - tileInfo->tileHotSpotY;  //In theory, tileHotSpotY is always zero!
	
	if ( (topLeftX >= paneX1) ||
		 (topLeftY >= paneY1) ||
		 (topLeftX <= (paneX0 - scanCol)) ||
		 (topLeftY <= (paneY0 - scanLines)))
		return(FULLY_CLIPPED);

	MemoryPtr screenBuffer = pane->window->buffer + ((topLeftY > paneY0) ? topLeftY*pwXMax : paneY0*pwXMax);

	long firstScanOffset = (topLeftY < paneY0) ? paneY0 - topLeftY : 0;
	
	long lastScanOffset = ((topLeftY + scanLines) > paneY1) ? paneY1 - topLeftY + 1 : scanLines;

	yOffsetTable += firstScanOffset;

	if( (topLeftX>paneX0) && ((topLeftX+scanCol) < paneX1) )
	{
		FadeTable=fadeTable;
		screenBuffer+=topLeftX;
		count=lastScanOffset-firstScanOffset;
//
// No clipping, whole tile is on screen
//
		_asm{
			push ebp
			mov edx,screenBuffer

			mov esi,tile
			mov ebp,yOffsetTable

			mov eax,FadeTable
			add esi,[ebp]

			cmp eax,-1
			jz DoBlack

			test eax,eax
			jnz DoFade

//
// Normal
//
DisplayTile:
			xor ebx,ebx
			mov eax,[ebp+4]

			mov ecx,[ebp]
			lea ebp,[ebp+4]

			sub eax,ecx
			mov bl,[esi]

			mov ecx,0
			jz DisplayTile1

			lea edi,[ebx+edx]
			dec eax

			sub ecx,edi
			inc esi

			and ecx,3
			sub eax,ecx
			jle DisplayTile2
			rep movsb
			mov ecx,eax
			and eax,3				; Main loop, copying an unclipped tile
			shr ecx,2
			jz DisplayTile2
DisplayTile3:
			mov ebx,[esi]
			add esi,4
			mov [edi],ebx
			add edi,4
			dec ecx
			jnz DisplayTile3
DisplayTile2:
			add eax,ecx
			jz DisplayTile1
DisplayTile4:
			mov bl,[esi]
			inc esi
			mov [edi],bl
			inc edi
			dec eax
			jnz DisplayTile4

DisplayTile1:
			mov ebx,pwXMax
			mov eax,count

			add edx,ebx
			dec eax

			mov count,eax
			jnz DisplayTile

			jmp done

//
// Black
//
DoBlack:	xor ebx,ebx
			mov eax,[ebp+4]

			mov ecx,[ebp]
			lea ebp,[ebp+4]

			sub eax,ecx
			mov bl,[esi]

			mov ecx,0
			jz DoBlack1

			lea edi,[ebx+edx]
			dec eax

			sub ecx,edi
			lea esi,[esi+eax+1]

			and ecx,3
			mov ebx,eax

			mov eax,BLACK
			sub ebx,ecx
			jle DoBlack2
			rep stosb
			mov ecx,ebx
			and ebx,3				; Main loop, blacking out an unclipped tile
			shr ecx,2
			rep stosd
DoBlack2:	add ecx,ebx
			rep stosb

DoBlack1:	mov ebx,pwXMax
			mov eax,count

			add edx,ebx
			dec eax

			mov count,eax
			jnz DoBlack

			jmp done

//
// Fade
//
DoFade:		xor ebx,ebx
			mov ecx,[ebp+4]

			mov eax,[ebp]
			lea ebp,[ebp+4]

			sub ecx,eax
			mov bl,[esi]

			lea ecx,[ecx-1]
			jz DoFade1

			lea edi,[ebx+edx]
			inc esi

// Fade inner loop
			push edx
			mov edx,FadeTable

			mov bl,[esi]
			inc esi

DoFade0:	inc edi
			dec ecx

			mov al,[edx+ebx]
			mov bl,[esi]

			mov [edi-1],al
			lea esi,[esi+1]
				
			jnz DoFade0
			pop edx
			dec esi

DoFade1:	mov ebx,pwXMax
			mov eax,count

			add edx,ebx
			dec eax

			mov count,eax
			jnz Dofade
done:
			pop ebp
		}

	}
	else
//
// This tile needs to be clipped
//
	{
		unsigned long currentOffset = *yOffsetTable++;
		unsigned long nextOffset=*yOffsetTable++;

		for (long scanLine = firstScanOffset; scanLine<lastScanOffset; scanLine++)
		{
			__asm
			{
				mov esi, tile
				xor ebx, ebx

				add esi, currentOffset
				xor eax, eax

				mov	al, BYTE PTR [esi]
				inc esi

				add eax, topLeftX
				mov edi, screenBuffer

				add edi, eax
				mov ecx, nextOffset

				sub ecx, currentOffset
				xor edx, edx

				dec ecx
				mov edx, eax

				add edx, ecx
				cmp eax, paneX0

				jge SHORT LINE207
				
				mov ebx, paneX0
				sub ebx, eax
				
				add esi, ebx
				add edi, ebx
LINE207:
				cmp edx, paneX1
				jle SHORT LINE214
				
				sub edx, paneX1
				dec edx
				jmp SHORT LEAVE_EDX
			
LINE214:
				xor edx, edx

LEAVE_EDX:
				sub ecx, ebx
				sub ecx, edx
				
				cmp ecx, 0
				jle SHORT NO_DRAW
			
				mov eax,fadeTable
				cmp eax,-1
				jz SHORT BLT_BLACK
				
				test eax,eax
				jz BLT_FAST
			
//FADE_BLT_TOP:
				xor edx, edx
				mov ebx,fadeTable

				mov dl, [esi]
				inc esi
Blt_fade1:
				inc edi
				dec ecx

				mov al, [edx+ebx]
				mov dl, [esi]

				mov BYTE PTR [edi-1], al
				lea esi,[esi+1]
					
				jnz Blt_fade1
				jmp short NO_DRAW

BLT_BLACK:
				mov ebx,ecx				; DWORD align edi when possible
				sub ecx,edi
				mov eax,BLACK
				sub ecx,ebx
				and ecx,3
				sub ebx,ecx
				jle blt_black1
				rep stosb
				mov ecx,ebx
				and ebx,3
				shr ecx,2
				rep stosd
blt_black1:		add ecx,ebx
				rep stosb
				jmp SHORT NO_DRAW

BLT_FAST:
				mov eax,ecx				; DWORD align edi when possible
				sub ecx,edi
				sub ecx,eax
				and ecx,3
				sub eax,ecx
				jle blt_fast1
				rep movsb
				mov ecx,eax
				and eax,3
				shr ecx,2
				rep movsd
blt_fast1:		add ecx,eax
				rep movsb
NO_DRAW:
			}

			screenBuffer += pwXMax;
			currentOffset = nextOffset;
			nextOffset = *yOffsetTable++;
		}
	}
	//-----------------------------------------------------------
#endif // LINUX_BUILD
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

#ifdef LINUX_BUILD
#else
	__asm
	{
                mov esi,shape_table
                add esi,8                       //skip to offsets
                
                mov eax,shape_number            //point to shape offset ptr 
                shl eax,3                       //mul eax by sizeof 2 longs
                add esi,eax                     

                mov esi,[esi]                   //get shape offset ptr
                add esi,shape_table				//add base address

                mov eax,[esi+4]					//SHAPEHEADER.origin

				mov	result,eax
	}
#endif 
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

#ifdef LINUX_BUILD
#else
	__asm
	{
				mov esi,shape_table
                add esi,8                       //skip to offsets

                
                mov eax,shape_number            //point to shape offset ptr 
                shl eax,3                       //mul eax by sizeof 2 longs
                add esi,eax                     

                mov esi,[esi]                   //get shape offset ptr
                add esi,shape_table	            //add base address

                mov eax,[esi+16]				//SHAPEHEADER.xmax  ;eax = xmax - xmin
                sub eax,[esi+8]					//SHAPEHEADER.xmin
                inc eax                         //      + 1

                mov ebx,[esi+20]				//SHAPEHEADER.ymax  ;ebx = ymax - ymin
                sub ebx,[esi+12]				//SHAPEHEADER.ymin
                inc ebx                         //      + 1

                shl eax,16
				and eax,0xffff0000
                add eax,ebx

				mov	result,eax
	}
#endif // LINUX_BUILD
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

#ifdef LINUX_BUILD
#else
	__asm
	{
                mov esi,shape_table
                add esi,8                       //skip to offsets
                
                mov eax,shape_number            //point to shape offset ptr 
                shl eax,3                       //mul eax by sizeof 2 longs
                add esi,eax                     

                mov esi,[esi]						//get shape offset ptr
                add esi,shape_table				//add base address

                mov eax,[esi+8]					//SHAPEHEADER.xmin
                shl eax,16

				mov ebx,[esi+12]				//SHAPEHEADER.ymin
				and eax,0xffff0000
				add eax,ebx

				mov result, eax
	}
#endif // LINUX_BUILD
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

#ifdef LINUX_BUILD
    assert(0);
#else
	__asm
	{
		mov esi,shape_table
		add esi,8                       //skip to offsets
                
		mov eax,shape_number            //point to shape offset ptr 
		shl eax,3                       //mul eax by sizeof 2 longs
		add esi,eax                     

		mov esi,[esi]                   //get shape offset ptr
		add esi,shape_table				//add base address

		mov eax,[esi]					//Bounds is First Member of struct
		mov boundsResult, eax
	}
#endif // LINUX_BUILD
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

#ifdef LINUX_BUILD
#else
	__asm
	{
		mov esi,shape_table
		mov eax,[esi+4]
		mov countResult,eax
	}
#endif // LINUX_BUILD
	return countResult;
}
