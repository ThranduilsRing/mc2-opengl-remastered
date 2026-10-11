// Shared surface of the cevfx.cpp translation units (cevfx.cpp, cevfx_polys.cpp).
//
// cevfx.cpp keeps the VFX shape element (VFXElement, VFXShapeElement) and the palette lookup tables; the textured and
// untextured polygon / texture elements are split out (see DOC in the piece).
#ifndef CEVFX_INTERNAL_H
#define CEVFX_INTERNAL_H


#ifndef CEVFX_H
#include"cevfx.h"
#endif

#ifndef SCALE_H
#include"scale.h"
#endif

#ifndef VPORT_H
#include"vport.h"
#endif

#ifndef VFX_H
#include"vfx.h"
#endif

#ifndef HEAP_H
#include"heap.h"
#endif

#ifndef TGL_H
#include"tgl.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef CLIP_H
#include"clip.h"
#endif

// Globals defined in cevfx.cpp (or elsewhere in the engine) that the split pieces share.
extern void AG_shape_translate_transform(PANE *globalPane, void *shapeTable,LONG frameNum, LONG hotX, LONG hotY,void *tempBuffer,LONG reverse, LONG scaleUp);
extern void AG_shape_transform(PANE *globalPane, void *shapeTable,LONG frameNum, LONG hotX, LONG hotY, void *tempBuffer,LONG reverse, LONG scaleUp);
extern void AG_shape_lookaside( UBYTE *table );
extern void AG_shape_draw (PANE *pane, void *shape_table,LONG shape_number, LONG hotX, LONG hotY);
extern void AG_shape_translate_draw (PANE *pane, void *shape_table,LONG shape_number, LONG hotX, LONG hotY);
extern void memclear(void *Dest,int Length);
extern MemoryPtr shapeBuffer16;
extern MemoryPtr shapeBuffer32;
extern MemoryPtr shapeBuffer64;
extern MemoryPtr shapeBuffer128;
extern MemoryPtr shapeBuffer256;

#endif // CEVFX_INTERNAL_H
