// Shared surface of the vfxtile.cpp translation units (vfxtile.cpp, vfxtile_{line,pixel}.cpp): the include bundle only.
//
// vfxtile.cpp keeps the tile blit, its file-static scratch and the shape queries; the line drawer and the pixel read / write
// share nothing with it but the VFX headers.
#ifndef VFXTILE_INTERNAL_H
#define VFXTILE_INTERNAL_H

#ifndef DSTD_H
#include"dstd.h"
#endif

#ifndef VFX_H
#include"vfx.h"
#endif

#include<cassert>

// Globals defined in vfxtile.cpp (or elsewhere in the engine) that the split pieces share.

#endif // VFXTILE_INTERNAL_H
