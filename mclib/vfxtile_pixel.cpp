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
#ifdef LINUX_BUILD
	assert(0 && "VFX_pixel_write is not implemented");
#else
		long		_L;	//Leftmost pixel in Window coord.
		long		_T;	//Top
		long		_R;	//Right
		long		_B;	//Bottom
	              
		MemoryPtr	_A;	//Base address of Clipped Pane
		long		_W;	//Width of underlying window (bytes)
	
		long		_CX;	//Window x coord. = Pane x coord. + CP_CX
		long		_CY;	//Window y coord. = Pane x coord. + CP_CY

		
		__asm
			{
				cld
			}

	//Clip Pane to Window Routine

		// get panep (esi)
        // windowp (ebx) = panep->win

		//ASSUME  esi:PPANE
		//ASSUME  ebx:PWIN

		__asm
			{
                mov     esi,panep
                mov     ebx,[esi]		//This is the Window Pointer

                // _W = windowp->wnd_x1 + 1
                // if _W <= 0, return bad window
    
                mov     eax,[ebx+4]		//X1 Window Coord
                inc     eax
                mov     _W,eax
                jle     ReturnBadWindow
    
                // ecx = Ysize = windowp->wnd_y1 + 1
                // if <= 0, return bad window

                mov     eax,[ebx+8]		//y1 Window Coord
                inc     eax
                mov     ecx,eax
                jle     ReturnBadWindow

                // clip pane to window:
                //  pane_x0 = max (pane->x0, 0)
                //  pane_y0 = max (pane->y0, 0)
                //  pane_x1 = min (pane->x1, _W - 1)
                //  pane_y1 = min (pane->x1, (Ysize=ecx) - 1)

                mov     eax,[esi+4]		//x0 in Pane Coord
                mov     _CX,eax

	            cmp     eax,0
	            jg      around1
		        mov     eax,0
around1:
                mov     _L,eax
    
                mov     eax,[esi+8]		//y0 in Pane Coord
                mov     _CY,eax

	            cmp     eax,0
	            jg      around2
		        mov     eax,0
around2:
                mov     _T,eax
    
                mov     eax,[esi+12]	//X1 in Pane Coord
                mov     edx,_W
                dec     edx

				cmp     eax,edx
				jl      around3
				mov     eax,edx
around3:
                mov     _R,eax
    
                mov     eax,[esi+16]	//y1 in Pane Coord
                mov     edx,ecx
                dec     edx
				cmp     eax,edx
				jl      around4
				mov     eax,edx
around4:
                mov     _B,eax

                // exit if pane is malformed or completely off window:
                //  if _B < &vname&_T, return bad pane
                //  if _R < &vname&_L, return bad pane

                mov     eax,_R
                cmp     eax,_L
                jl      ReturnBadPane
    
                mov     eax,_B
                cmp     eax,_T
                jl      ReturnBadPane

                mov     eax,[ebx]		//Buffer in Window
                mov     _A,eax

                jmp     exit1

ReturnBadWindow:
                mov     eax,-1

				jmp		exit1
			}

			__asm
			{

ReturnBadPane:
                mov     eax,-2
			}

exit1:

			__asm
			{
				// transform x & y to window coord's

                mov     ecx,x
                mov     ebx,y

                //CONVERT_REG_PAIR_PANE_TO_WINDOW ecx, ebx
				
                add     ecx,_CX
                add     ebx,_CY

        		// clip pixel to pane
    
                cmp     ecx,_L
                jl      ReturnOffPane
                cmp     ecx,_R
                jg      ReturnOffPane
                cmp     ebx,_T
                jl      ReturnOffPane
                cmp     ebx,_B
                jg      ReturnOffPane

		        // adr (ebx) = window->buffer + CP_W * y + x

				//GET_WINDOW_ADDRESS  x0_, y0_

				mov     eax,ebx
				imul    _W
				add     eax,_A
				add     eax,ecx

        	    mov     edi,eax

				mov     ebx,eax

        		// write the pixel

                mov     dl,BYTE PTR color
                mov     [ebx],dl

        		// return prior_value

                ret

        		// error returns

ReturnOffPane:
                mov     eax,-3
                ret
				
			}
#endif // LINUX_BUILD			
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
#ifdef LINUX_BUILD
	assert(0 && "VFX_line_draw is not implemented");
#else
		long		_L;	//Leftmost pixel in Window coord.
		long		_T;	//Top
		long		_R;	//Right
		long		_B;	//Bottom
	              
		MemoryPtr	_A;	//Base address of Clipped Pane
		long		_W;	//Width of underlying window (bytes)
	
		long		_CX;	//Window x coord. = Pane x coord. + CP_CX
		long		_CY;	//Window y coord. = Pane x coord. + CP_CY

		
		__asm
			{
				cld
			}

	//Clip Pane to Window Routine

		// get panep (esi)
        // windowp (ebx) = panep->win

		//ASSUME  esi:PPANE
		//ASSUME  ebx:PWIN

		__asm
			{
                mov     esi,panep
                mov     ebx,[esi]		//This is the Window Pointer

                // _W = windowp->wnd_x1 + 1
                // if _W <= 0, return bad window
    
                mov     eax,[ebx+4]		//X1 Window Coord
                inc     eax
                mov     _W,eax
                jle     ReturnBadWindow
    
                // ecx = Ysize = windowp->wnd_y1 + 1
                // if <= 0, return bad window

                mov     eax,[ebx+8]		//y1 Window Coord
                inc     eax
                mov     ecx,eax
                jle     ReturnBadWindow

                // clip pane to window:
                //  pane_x0 = max (pane->x0, 0)
                //  pane_y0 = max (pane->y0, 0)
                //  pane_x1 = min (pane->x1, _W - 1)
                //  pane_y1 = min (pane->x1, (Ysize=ecx) - 1)

                mov     eax,[esi+4]		//x0 in Pane Coord
                mov     _CX,eax

	            cmp     eax,0
	            jg      around1
		        mov     eax,0
around1:
                mov     _L,eax
    
                mov     eax,[esi+8]		//y0 in Pane Coord
                mov     _CY,eax

	            cmp     eax,0
	            jg      around2
		        mov     eax,0
around2:
                mov     _T,eax
    
                mov     eax,[esi+12]	//X1 in Pane Coord
                mov     edx,_W
                dec     edx

				cmp     eax,edx
				jl      around3
				mov     eax,edx
around3:
                mov     _R,eax
    
                mov     eax,[esi+16]	//y1 in Pane Coord
                mov     edx,ecx
                dec     edx
				cmp     eax,edx
				jl      around4
				mov     eax,edx
around4:
                mov     _B,eax

                // exit if pane is malformed or completely off window:
                //  if _B < &vname&_T, return bad pane
                //  if _R < &vname&_L, return bad pane

                mov     eax,_R
                cmp     eax,_L
                jl      ReturnBadPane
    
                mov     eax,_B
                cmp     eax,_T
                jl      ReturnBadPane

                mov     eax,[ebx]		//Buffer in Window
                mov     _A,eax

                jmp     exit1

ReturnBadWindow:
                mov     eax,-1

				jmp		exit1
			}

			__asm
			{

ReturnBadPane:
                mov     eax,-2
			}

exit1:

			__asm
			{
				// transform x & y to window coord's

                mov     ecx,x
                mov     ebx,y

                //CONVERT_REG_PAIR_PANE_TO_WINDOW ecx, ebx
				
                add     ecx,_CX
                add     ebx,_CY

        		// clip pixel to pane
    
                cmp     ecx,_L
                jl      ReturnOffPane
                cmp     ecx,_R
                jg      ReturnOffPane
                cmp     ebx,_T
                jl      ReturnOffPane
                cmp     ebx,_B
                jg      ReturnOffPane

		        // adr (ebx) = window->buffer + CP_W * y + x

				//GET_WINDOW_ADDRESS  x0_, y0_

				mov     eax,ebx
				imul    _W
				add     eax,_A
				add     eax,ecx

        	    mov     edi,eax

				mov     ebx,eax

        		// read and return the pixel
    
                xor     eax,eax
                mov     al,[ebx]
                ret

				// error returns

ReturnOffPane:
                mov     eax,-3
				mov 	result, eax
                ret
			}
#endif // LINUX_BUILD	
	return(result);
}
