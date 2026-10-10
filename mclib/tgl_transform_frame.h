// Per-vertex and per-face colour accumulators the MultiTransformShape lighting phases share.
#ifndef MCLIB_TGL_TRANSFORM_FRAME_H
#define MCLIB_TGL_TRANSFORM_FRAME_H

#include "tgl_internal.h"

// The colour accumulators one vertex or one face builds across the lighting phases.
struct TgXformFrame {
	DWORD redFinal;
	DWORD greenFinal;
	DWORD blueFinal;
	DWORD redAmb;
	DWORD greenAmb;
	DWORD blueAmb;
	DWORD redSpec;
	DWORD greenSpec;
	DWORD blueSpec;
	DWORD startVLight;
	bool doHotGreenTrace;
};

#endif  // MCLIB_TGL_TRANSFORM_FRAME_H
