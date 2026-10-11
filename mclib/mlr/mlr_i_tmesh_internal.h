// Shared surface of the mlr_i_tmesh.cpp translation units (mlr_i_tmesh.cpp, mlr_i_tmesh_{light,cast}.cpp).
//
// mlr_i_tmesh.cpp keeps the class data, construction, copy, visibility tests, the clip include and per-vertex lighting;
// the light-map lighting and the ray cast with the indexed-triangle test shapes are split out (see DOC in each piece).
#ifndef MLR_I_TMESH_INTERNAL_H
#define MLR_I_TMESH_INTERNAL_H

#include"mlrheaders.hpp"
#define SPEW_AWAY 0

// Globals defined in mlr_i_tmesh.cpp (or elsewhere in the engine) that the split pieces share.
extern DWORD gEnableLightMaps;
extern DynamicArrayOf<Vector2DScalar> *lightMapUVs;
extern DynamicArrayOf<Scalar> *lightMapSqFalloffs;

#endif // MLR_I_TMESH_INTERNAL_H
