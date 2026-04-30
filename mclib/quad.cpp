//---------------------------------------------------------------------------
//
// Quad.cpp -- File contains class code for the Terrain Quads
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef QUAD_H
#include"quad.h"
#endif

#ifndef VERTEX_H
#include"vertex.h"
#endif

#ifndef TERRAIN_H
#include"terrain.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#ifndef CELINE_H
#include"celine.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#include"../GameOS/gameos/gos_profiler.h"
#include"../GameOS/gameos/gos_terrain_patch_stream.h"
#include"projectz_trace.h"
#include"projectz_overlay.h"
#include"tex_resolve_table.h"

// Per-quad scratch for the four BoolAdmission per-vertex projectZ calls in
// TerrainQuad::setupTextures. After each `eye->projectZ()` call we copy
// g_pzLastPredicates into the matching slot, so pz_emit_terrain_tris (called
// later for the same quad) can hand both screen-state AND predicate-state to
// the overlay without ever recomputing predicates. Single-threaded terrain
// submission makes the file-static safe.
static ProjectZPredicates s_pzVertPreds[4] = {};
static inline void pz_capture_vert_preds(int slot) {
    if (g_pzTrace) s_pzVertPreds[slot] = g_pzLastPredicates;
}
static const bool s_shapeCParityCheck = (getenv("MC2_SHAPE_C_PARITY_CHECK") != nullptr);
static const bool s_shapeCEnabled = ([] {
	const char* env = getenv("MC2_MODERN_TERRAIN_PATCHES");
	return (env != nullptr) && (env[0] == '1');
})();

#define SELECTION_COLOR 0xffff7fff
#define HIGHLIGHT_COLOR	0xff00ff00

// Map MC2 terrain type enum (0-20) to PBR material index (0-3)
// 0=Rock, 1=Grass, 2=Dirt, 3=Concrete
static BYTE terrainTypeToMaterial(DWORD terrainType)
{
    switch (terrainType)
    {
        case 3:  // Moss
        case 8:  // ForestFloor
        case 9:  // Grass
        case 12: // Slimy
            return 1; // Grass
        case 2:  // Mud
        case 4:  // Dirt
            return 2; // Dirt
        case 10: // Concrete
        case 13: case 14: case 15: case 16: // Cement 2-5
        case 17: case 18: case 19:          // Cement 6-8
            return 3; // Concrete
        default:
            return 0; // Rock (Mountain, Cliff, Ash, Tundra, Water, None)
    }
}

//---------------------------------------------------------------------------
float TerrainQuad::rainLightLevel = 1.0f;
DWORD TerrainQuad::lighteningLevel = 0;
DWORD TerrainQuad::mineTextureHandle = 0xffffffff;
DWORD TerrainQuad::blownTextureHandle = 0xffffffff;

extern bool drawTerrainGrid;
extern bool drawLOSGrid;

// Fill terrain extra data for tessellation — routed to per-node storage via texture manager
static void fillTerrainExtra(DWORD texHandle, DWORD flags, VertexPtr v0, VertexPtr v1, VertexPtr v2) {
    gos_TERRAIN_EXTRA textra[3];
    textra[0].wx = v0->vx; textra[0].wy = v0->vy; textra[0].wz = v0->pVertex->elevation;
    textra[0].nx = v0->pVertex->vertexNormal.x; textra[0].ny = v0->pVertex->vertexNormal.y; textra[0].nz = v0->pVertex->vertexNormal.z;
    textra[1].wx = v1->vx; textra[1].wy = v1->vy; textra[1].wz = v1->pVertex->elevation;
    textra[1].nx = v1->pVertex->vertexNormal.x; textra[1].ny = v1->pVertex->vertexNormal.y; textra[1].nz = v1->pVertex->vertexNormal.z;
    textra[2].wx = v2->vx; textra[2].wy = v2->vy; textra[2].wz = v2->pVertex->elevation;
    textra[2].nx = v2->pVertex->vertexNormal.x; textra[2].ny = v2->pVertex->vertexNormal.y; textra[2].nz = v2->pVertex->vertexNormal.z;
    mcTextureManager->addTerrainExtra(texHandle, textra, flags);
}

// Builds the same 3-element gos_TERRAIN_EXTRA[3] that fillTerrainExtra
// would push to addTerrainExtra, but returns it by value so the modern
// patch stream can mirror-write the bytes without re-reading the verts.
// Keeping the math identical to fillTerrainExtra is the BR-byte-parity
// guarantee for shadow / grass behavior.
static inline void buildTerrainExtraTriple(VertexPtr v0, VertexPtr v1, VertexPtr v2,
                                           gos_TERRAIN_EXTRA out[3])
{
    VertexPtr vs[3] = { v0, v1, v2 };
    for (int k = 0; k < 3; ++k) {
        out[k].wx = vs[k]->vx;
        out[k].wy = vs[k]->vy;
        out[k].wz = vs[k]->pVertex->elevation;
        out[k].nx = vs[k]->pVertex->vertexNormal.x;
        out[k].ny = vs[k]->pVertex->vertexNormal.y;
        out[k].nz = vs[k]->pVertex->vertexNormal.z;
    }
}

// Per-triangle diagnostic hook for terrain addTriangle sites.
// Observation-only; must be called AFTER addTriangle so it cannot affect submission.
// Active only when g_pzTrace is true (any PROJECTZ env var set).
static void pz_emit_terrain_tris(
    VertexPtr*  verts,     // array of 4
    int         uvMode,
    const char* callsiteId,
    const char* file,
    int         line)
{
    if (!g_pzTrace) return;
    long rowCol = verts[0]->posTile;
    int tileR = rowCol >> 16;
    int tileC = rowCol & 0x0000ffff;
    ProjectZTriVert pzv[4];
    for (int i = 0; i < 4; i++) {
        pzv[i].legacyAccepted = (verts[i]->clipInfo != 0);
        pzv[i].wx = verts[i]->px;
        pzv[i].wy = verts[i]->py;
        pzv[i].wz = verts[i]->pz;
        pzv[i].vx = verts[i]->vx;
        pzv[i].vy = verts[i]->vy;
        pzv[i].vz = verts[i]->pVertex->elevation;
    }
    // `eye` is the global CameraPtr declared in camera.h — the same camera the
    // BoolAdmission projectZ calls in setupTextures used.
    float resX = eye ? eye->fgetScreenResX() : 1920.0f;
    float resY = eye ? eye->fgetScreenResY() : 1080.0f;
    if (uvMode == BOTTOMRIGHT) {
        static const int c012[] = {0,1,2};
        static const int c023[] = {0,2,3};
        projectz_emit_tri(callsiteId, tileR, tileC, c012, pzv, true, file, line);
        projectz_emit_tri(callsiteId, tileR, tileC, c023, pzv, true, file, line);
        projectz_overlay_record_tri(pzv, s_pzVertPreds, c012, resX, resY);
        projectz_overlay_record_tri(pzv, s_pzVertPreds, c023, resX, resY);
    } else {
        static const int c013[] = {0,1,3};
        static const int c123[] = {1,2,3};
        projectz_emit_tri(callsiteId, tileR, tileC, c013, pzv, true, file, line);
        projectz_emit_tri(callsiteId, tileR, tileC, c123, pzv, true, file, line);
        projectz_overlay_record_tri(pzv, s_pzVertPreds, c013, resX, resY);
        projectz_overlay_record_tri(pzv, s_pzVertPreds, c123, resX, resY);
    }
}

static bool isTerrainQuadVisible(const TerrainQuad& quad)
{
	if (quad.uvMode == BOTTOMRIGHT)
	{
		long clipped1 = quad.vertices[0]->clipInfo + quad.vertices[1]->clipInfo + quad.vertices[2]->clipInfo;
		long clipped2 = quad.vertices[0]->clipInfo + quad.vertices[2]->clipInfo + quad.vertices[3]->clipInfo;
		return (clipped1 || clipped2) != 0;
	}

	long clipped1 = quad.vertices[0]->clipInfo + quad.vertices[1]->clipInfo + quad.vertices[3]->clipInfo;
	long clipped2 = quad.vertices[1]->clipInfo + quad.vertices[2]->clipInfo + quad.vertices[3]->clipInfo;
	return (clipped1 || clipped2) != 0;
}

static void enqueueTerrainMineState(TerrainQuad& quad)
{
	long rowCol = quad.vertices[0]->posTile;
	long tileR = rowCol>>16;
	long tileC = rowCol & 0x0000ffff;
			
	if (GameMap)
	{
		long cellPos = 0;
		quad.mineResult.init();
		for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
		{
			for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
			{
				long actualCellRow = tileR * MAPCELL_DIM + cellR;
				long actualCellCol = tileC * MAPCELL_DIM + cellC;
				
				DWORD localResult = 0;
				if (GameMap->inBounds(actualCellRow, actualCellCol))
					localResult = GameMap->getMine(actualCellRow, actualCellCol);
					
				if (localResult == 1)
				{
					tex_resolve(TerrainQuad::mineTextureHandle);
					mcTextureManager->addTriangle(TerrainQuad::mineTextureHandle,MC2_DRAWALPHA);
					mcTextureManager->addTriangle(TerrainQuad::mineTextureHandle,MC2_DRAWALPHA);
					
					quad.mineResult.setMine(cellPos,localResult);
				}
				else if (localResult == 2)
				{
					tex_resolve(TerrainQuad::blownTextureHandle);
					mcTextureManager->addTriangle(TerrainQuad::blownTextureHandle,MC2_DRAWALPHA);
					mcTextureManager->addTriangle(TerrainQuad::blownTextureHandle,MC2_DRAWALPHA);
					
					quad.mineResult.setMine(cellPos,localResult);
				}
			}
		}
	}
}

static void enqueueCachedTerrainTriangles(const MapData::WorldQuadTerrainCacheEntry& entry)
{
	if(entry.terrainHandle!=0 && entry.terrainHandle != 0xffffffff) {
		mcTextureManager->addTriangle(entry.terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
		mcTextureManager->addTriangle(entry.terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
		if (entry.terrainDetailHandle != 0xffffffff && (!entry.isCement() || entry.isAlpha()))
		{
			mcTextureManager->addTriangle(entry.terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
			mcTextureManager->addTriangle(entry.terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
		}
	}
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// Class TerrainQuad
long TerrainQuad::init (VertexPtr v0, VertexPtr v1, VertexPtr v2, VertexPtr v3)
{
	vertices[0] = v0;
	vertices[1] = v1;
	vertices[2] = v2;
	vertices[3] = v3;

	return(NO_ERR);
}

float twoFiveFive = 255.0;
#define HUD_DEPTH		0.0001f			//HUD Objects draw over everything else.
extern float cosineEyeHalfFOV; 
extern DWORD BaseVertexColor;
extern bool useShadows;
extern bool useFog;
extern long sprayFrame;
bool useWaterInterestTexture = true;
bool useOverlayTexture = true;
long numTerrainFaces = 0;
extern float MaxMinUV;

extern float leastZ;
extern float leastW;
extern float mostZ; 
extern float mostW;
extern float leastWY; 
extern float mostWY;

//---------------------------------------------------------------------------
struct TerrainRecipe {
	DWORD terrainHandle       = 0xffffffff;
	DWORD terrainDetailHandle = 0xffffffff;
	DWORD overlayHandle       = 0xffffffff;
	TerrainUVData uvData      = {};
	bool isCement             = false;
	bool isAlpha              = false;
};

static void shapeC_checkParity(
	const TerrainRecipe& inlineR,
	const MapData::WorldQuadTerrainCacheEntry& e,
	long tileR, long tileC, long uvMode)
{
	static int s_mismatches = 0;
	static int s_checks     = 0;
	++s_checks;

	const char* siteTag =
		!inlineR.isCement ? (uvMode == BOTTOMRIGHT ? "BR_no_cement" : "TL_no_cement")
	  : inlineR.isAlpha   ? (uvMode == BOTTOMRIGHT ? "BR_alpha"     : "TL_alpha")
						  : (uvMode == BOTTOMRIGHT ? "BR_cement"    : "TL_cement");

	bool ok = true;

#define SHAPE_C_CHECK_DWORD(field) \
	if (inlineR.field != e.field) { \
		if (s_mismatches < 10) \
			printf("[SHAPE_C] MISMATCH " #field " site=%s tileR=%ld tileC=%ld" \
				   " inline=%u cached=%u\n", \
				   siteTag, tileR, tileC, inlineR.field, e.field); \
		ok = false; ++s_mismatches; }

#define SHAPE_C_CHECK_BOOL(method) \
	if (inlineR.method != e.method()) { \
		if (s_mismatches < 10) \
			printf("[SHAPE_C] MISMATCH " #method " site=%s tileR=%ld tileC=%ld" \
				   " inline=%d cached=%d\n", \
				   siteTag, tileR, tileC, (int)inlineR.method, (int)e.method()); \
		ok = false; ++s_mismatches; }

	SHAPE_C_CHECK_DWORD(terrainHandle)
	SHAPE_C_CHECK_DWORD(terrainDetailHandle)
	SHAPE_C_CHECK_DWORD(overlayHandle)
	SHAPE_C_CHECK_BOOL(isCement)
	SHAPE_C_CHECK_BOOL(isAlpha)

#undef SHAPE_C_CHECK_DWORD
#undef SHAPE_C_CHECK_BOOL

	if (fabsf(inlineR.uvData.minU - e.uvData.minU) > 1e-5f ||
		fabsf(inlineR.uvData.minV - e.uvData.minV) > 1e-5f ||
		fabsf(inlineR.uvData.maxU - e.uvData.maxU) > 1e-5f ||
		fabsf(inlineR.uvData.maxV - e.uvData.maxV) > 1e-5f)
	{
		if (s_mismatches < 10)
			printf("[SHAPE_C] MISMATCH uvData site=%s tileR=%ld tileC=%ld"
				   " inline=(%.5f,%.5f,%.5f,%.5f) cached=(%.5f,%.5f,%.5f,%.5f)\n",
				   siteTag, tileR, tileC,
				   inlineR.uvData.minU, inlineR.uvData.minV,
				   inlineR.uvData.maxU, inlineR.uvData.maxV,
				   e.uvData.minU, e.uvData.minV, e.uvData.maxU, e.uvData.maxV);
		ok = false; ++s_mismatches;
	}

	// Summary every 10000 checks and on first check of session.
	if (s_checks == 1 || s_checks % 10000 == 0)
		printf("[SHAPE_C] parity checks=%d mismatches=%d\n", s_checks, s_mismatches);
	(void)ok;
}

// Fills recipe from inline getTextureHandle() calls. Always correct; never stale.
static void buildTerrainRecipeInline(VertexPtr* vertices, long uvMode, TerrainRecipe& r)
{
	r.isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
	r.isAlpha  = Terrain::terrainTextures->isAlpha (vertices[0]->pVertex->textureData & 0x0000ffff);

	if (!r.isCement)
	{
		VertexPtr vA = (uvMode == BOTTOMRIGHT) ? vertices[0] : vertices[1];
		VertexPtr vB = (uvMode == BOTTOMRIGHT) ? vertices[2] : vertices[3];
		r.terrainHandle       = Terrain::terrainTextures2->getTextureHandle(vA, vB, &r.uvData);
		r.terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();
		r.overlayHandle       = 0xffffffff;
	}
	else if (r.isAlpha)
	{
		r.overlayHandle       = Terrain::terrainTextures->getTextureHandle(vertices[0]->pVertex->textureData & 0x0000ffff);
		r.terrainHandle       = Terrain::terrainTextures2->getTextureHandle(vertices[1], vertices[3], &r.uvData);
		r.terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();
	}
	else // pure cement
	{
		r.terrainHandle       = Terrain::terrainTextures->getTextureHandle(vertices[0]->pVertex->textureData & 0x0000ffff);
		r.terrainDetailHandle = 0xffffffff;
		r.overlayHandle       = 0xffffffff;
	}
}

// Fills recipe from cache when s_shapeCEnabled and entry is valid.
// Returns false if cache unavailable; recipe is unmodified.
static bool tryGetCachedTerrainRecipe(
	const MapData::WorldQuadTerrainCacheEntry* entry, TerrainRecipe& r)
{
	if (!s_shapeCEnabled || !entry || !entry->isValid())
		return false;
	r.isCement            = entry->isCement();
	r.isAlpha             = entry->isAlpha();
	r.terrainHandle       = entry->terrainHandle;
	r.terrainDetailHandle = entry->terrainDetailHandle;
	r.overlayHandle       = entry->overlayHandle;
	r.uvData              = entry->uvData;
	return true;
}

// Calls mcTextureManager->addTriangle() for the recipe's handles.
// Does NOT call pz_emit_terrain_tris -- that stays at the setupTextures() callsite
// to preserve projectZ callsite identity.
static void addTerrainTriangles(const TerrainRecipe& r)
{
	if (!r.isCement)
	{
		if (r.terrainHandle != 0)
		{
			mcTextureManager->addTriangle(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
			mcTextureManager->addTriangle(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
			if (r.terrainDetailHandle != 0xffffffff)
			{
				mcTextureManager->addTriangle(r.terrainDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA);
				mcTextureManager->addTriangle(r.terrainDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA);
			}
		}
	}
	else if (r.isAlpha)
	{
		if (r.terrainHandle != 0)
		{
			mcTextureManager->addTriangle(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
			mcTextureManager->addTriangle(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
		}
		if (r.terrainDetailHandle != 0xffffffff)
		{
			mcTextureManager->addTriangle(r.terrainDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA);
			mcTextureManager->addTriangle(r.terrainDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA);
		}
	}
	else // pure cement
	{
		if (r.terrainHandle != 0)
		{
			mcTextureManager->addTriangle(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
			mcTextureManager->addTriangle(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
		}
	}
}

void TerrainQuad::setupTextures (void)
{
	{
		ZoneScopedN("quadSetupTextures admission / early guards");
	if (mineTextureHandle == 0xffffffff)
	{
		FullPathFileName mineTextureName;
		mineTextureName.init(texturePath,"defaults" PATH_SEPARATOR "mine_00",".tga");
		mineTextureHandle = mcTextureManager->loadTexture(mineTextureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
	}
	
	if (blownTextureHandle == 0xffffffff)
	{
		FullPathFileName mineTextureName;
		mineTextureName.init(texturePath,"defaults" PATH_SEPARATOR "minescorch_00",".tga");
		blownTextureHandle = mcTextureManager->loadTexture(mineTextureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
	}
	}
	
 	if (!Terrain::terrainTextures2)
	{
		if (uvMode == BOTTOMRIGHT)
		{
			long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
			long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

			if (clipped1 || clipped2)
			{
				{
					terrainHandle = Terrain::terrainTextures->getTextureHandle((vertices[0]->pVertex->textureData & 0x0000ffff));
					DWORD terrainDetailData = Terrain::terrainTextures->setDetail(1,0);
					if (terrainDetailData != 0xfffffff)
						terrainDetailHandle = Terrain::terrainTextures->getTextureHandle(terrainDetailData);
					else
						terrainDetailHandle = 0xffffffff;
					overlayHandle = 0xffffffff;

					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
					mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
					pz_emit_terrain_tris(vertices, uvMode, "terrain_quad_cluster_a", __FILE__, __LINE__);
				}

				//--------------------------------------------------------------------
				//Mine Information
				long rowCol = vertices[0]->posTile;
				long tileR = rowCol>>16;
				long tileC = rowCol & 0x0000ffff;
						
				if (GameMap)
				{
					mineResult.init();
					long cellPos = 0;
					for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
					{
						for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
						{
							long actualCellRow = tileR * MAPCELL_DIM + cellR;
							long actualCellCol = tileC * MAPCELL_DIM + cellC;
							
							DWORD localResult = 0;
							if (GameMap->inBounds(actualCellRow, actualCellCol))
								localResult = GameMap->getMine(actualCellRow, actualCellCol);
								
							if (localResult == 1)
							{
								tex_resolve(mineTextureHandle);
								mcTextureManager->addTriangle(mineTextureHandle, MC2_DRAWALPHA);
								mcTextureManager->addTriangle(mineTextureHandle, MC2_DRAWALPHA);
								
								mineResult.setMine(cellPos,localResult);
							}
							else if (localResult == 2)
							{
								tex_resolve(blownTextureHandle);
								mcTextureManager->addTriangle(blownTextureHandle, MC2_DRAWALPHA);
								mcTextureManager->addTriangle(blownTextureHandle, MC2_DRAWALPHA);
								
								mineResult.setMine(cellPos,localResult);
							}
						}
					}
				}
			}
			else
			{
				terrainHandle = 0xffffffff; 
				waterHandle = 0xffffffff; 
				waterDetailHandle = 0xffffffff;
				terrainDetailHandle = 0xffffffff;
				overlayHandle = 0xffffffff;
			}
		}
		else
		{
			long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
			long clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

			if (clipped1 || clipped2)
			{
				{
					terrainHandle = Terrain::terrainTextures->getTextureHandle((vertices[0]->pVertex->textureData & 0x0000ffff));
					DWORD terrainDetailData = Terrain::terrainTextures->setDetail(1,0);
					if (terrainDetailData != 0xfffffff)
						terrainDetailHandle = Terrain::terrainTextures->getTextureHandle(terrainDetailData);
					else
						terrainDetailHandle = 0xffffffff;
					overlayHandle = 0xffffffff;

					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainHandle,MC2_ISTERRAIN | MC2_DRAWSOLID);
					mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
					mcTextureManager->addTriangle(terrainDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA);
					pz_emit_terrain_tris(vertices, uvMode, "terrain_quad_cluster_c", __FILE__, __LINE__);
				}

				//--------------------------------------------------------------------
				//Mine Information
				long rowCol = vertices[0]->posTile;
				long tileR = rowCol>>16;
				long tileC = rowCol & 0x0000ffff;
						
				if (GameMap)
				{
					long cellPos = 0;
					mineResult.init();
					for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
					{
						for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
						{
							long actualCellRow = tileR * MAPCELL_DIM + cellR;
							long actualCellCol = tileC * MAPCELL_DIM + cellC;
							
							DWORD localResult = 0;
							if (GameMap->inBounds(actualCellRow, actualCellCol))
								localResult = GameMap->getMine(actualCellRow, actualCellCol);
								
							if (localResult == 1)
							{
								tex_resolve(mineTextureHandle);
								mcTextureManager->addTriangle(mineTextureHandle,MC2_DRAWALPHA);
								mcTextureManager->addTriangle(mineTextureHandle,MC2_DRAWALPHA);
								
								mineResult.setMine(cellPos,localResult);
							}
							else if (localResult == 2)
							{
								tex_resolve(blownTextureHandle);
								mcTextureManager->addTriangle(blownTextureHandle,MC2_DRAWALPHA);
								mcTextureManager->addTriangle(blownTextureHandle,MC2_DRAWALPHA);
								
								mineResult.setMine(cellPos,localResult);
							}
						}
					}
				}
			}
			else
			{
				terrainHandle = 0xffffffff; 
				waterHandle = 0xffffffff; 
				waterDetailHandle = 0xffffffff;
				terrainDetailHandle = 0xffffffff;
				overlayHandle = 0xffffffff;
			}
		}
	}
	else		//New single bitmap on the terrain.
	{
		if (!isTerrainQuadVisible(*this))
		{
			overlayHandle = 0xffffffff;
			terrainHandle = 0xffffffff; 
			waterHandle = 0xffffffff; 
			waterDetailHandle = 0xffffffff;
			terrainDetailHandle = 0xffffffff;
		}
		else
		{
			long rowCol = vertices[0]->posTile;
			long tileR = rowCol >> 16;
			long tileC = rowCol & 0x0000ffff;
			const MapData::WorldQuadTerrainCacheEntry* cachedEntry = Terrain::mapData ? Terrain::mapData->getTerrainFaceCacheEntry(tileR, tileC) : NULL;
			if (cachedEntry && cachedEntry->isValid())
			{
				ZoneScopedN("TerrainQuad::setupTextures cachedVisibleSubmission");
				Terrain::mapData->ensureTerrainFaceCacheEntryResident(*cachedEntry, false);
			}

			{
				ZoneScopedN("TerrainQuad::setupTextures resolveFallback");
				{
				ZoneScopedN("quadSetupTextures diagonal branch / triangle selection");
				TerrainRecipe recipe;
				TerrainRecipe inlineRecipe;

				// Always build inline recipe when parity check is on -- keeps comparison
				// meaningful even after the cache path is enabled in Task 5.
				if (s_shapeCParityCheck)
					buildTerrainRecipeInline(vertices, uvMode, inlineRecipe);

				if (!tryGetCachedTerrainRecipe(cachedEntry, recipe))
					buildTerrainRecipeInline(vertices, uvMode, recipe);

				// Compare inline vs cache entry (not recipe -- recipe may now be cache-sourced).
				if (s_shapeCParityCheck && cachedEntry && cachedEntry->isValid())
					shapeC_checkParity(inlineRecipe, *cachedEntry, tileR, tileC, uvMode);

				// Assign member vars inside the member function.
				isCement            = recipe.isCement;
				terrainHandle       = recipe.terrainHandle;
				terrainDetailHandle = recipe.terrainDetailHandle;
				overlayHandle       = recipe.overlayHandle;
				uvData              = recipe.uvData;

				// Register triangle batches (no pz_emit here).
				addTerrainTriangles(recipe);

				// pz_emit stays at this callsite to preserve projectZ callsite identity.
				if (!recipe.isCement && recipe.terrainHandle != 0)
					pz_emit_terrain_tris(vertices, uvMode,
						(uvMode == BOTTOMRIGHT) ? "terrain_quad_cluster_a" : "terrain_quad_cluster_c",
						__FILE__, __LINE__);
				}
			}

			{
				ZoneScopedN("quadSetupTextures mine/scorch checks");
				enqueueTerrainMineState(*this);
			}
		}
	}

	//-----------------------------------------
	// NEW(tm) water texture code here.
	if ((vertices[0]->pVertex->water & 1) ||
		(vertices[1]->pVertex->water & 1) ||
		(vertices[2]->pVertex->water & 1) ||
		(vertices[3]->pVertex->water & 1))
	{
		Stuff::Vector3D vertex3D(vertices[0]->vx,vertices[0]->vy,Terrain::waterElevation);
		Stuff::Vector4D screenPos;

		long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
		long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

		if (uvMode != BOTTOMRIGHT)
		{
			clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
			clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
		}

 		float negCos = Terrain::frameCos * -1.0;
		float ourCos = Terrain::frameCos;
		if (!(vertices[0]->calcThisFrame & 2))
		{
			if (clipped1 || clipped2)
			{
				if (vertices[0]->pVertex->water & 128)
				{
					vertices[0]->wAlpha = -Terrain::frameCosAlpha;
					ourCos = negCos;
				}
				else if (vertices[0]->pVertex->water & 64)
				{
					vertices[0]->wAlpha = Terrain::frameCosAlpha;
					ourCos = Terrain::frameCos;
				}
	
				vertex3D.z = ourCos + Terrain::waterElevation;

				bool clipData = false;
				// [PROJECTZ:BoolAdmission id=terrain_quad_vert0_admit]
				PROJECTZ_SITE("terrain_quad_vert0_admit", "BoolAdmission");
				clipData = eye->projectForTerrainAdmission(vertex3D,screenPos);
				pz_capture_vert_preds(0);
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
				if (!isVisible)
				{
					clipData = false;
					vertices[0]->hazeFactor = 1.0f;
				}

				if (eye->usePerspective)
					vertices[0]->clipInfo = clipData;
				else
					vertices[0]->clipInfo = clipData;
		
				vertices[0]->wx = screenPos.x;
				vertices[0]->wy = screenPos.y;
				vertices[0]->wz = screenPos.z;
				vertices[0]->ww = screenPos.w;
	
				vertices[0]->calcThisFrame |= 2;

				if (clipData)
				{
					if (screenPos.z < leastZ)
					{
						leastZ = screenPos.z;
					}

					if (screenPos.z > mostZ)
					{
						mostZ = screenPos.z;
					}

					if (screenPos.w < leastW)
					{
						leastW = screenPos.w;
						leastWY = screenPos.y;
					}

					if (screenPos.w > mostW)
					{
						mostW = screenPos.w;
						mostWY = screenPos.y;
					}
				}
			}
		}
		
		if (!(vertices[1]->calcThisFrame & 2))
		{
			if (clipped1 || clipped2)
			{
				if (vertices[1]->pVertex->water & 128)
				{
					vertices[1]->wAlpha = -Terrain::frameCosAlpha;
					ourCos = negCos;
				}
				else if (vertices[1]->pVertex->water & 64)
				{
					vertices[1]->wAlpha = Terrain::frameCosAlpha;
					ourCos = Terrain::frameCos;
				}
	
				vertex3D.z = ourCos + Terrain::waterElevation;
				vertex3D.x = vertices[1]->vx;
				vertex3D.y = vertices[1]->vy;

				bool clipData = false;
				// [PROJECTZ:BoolAdmission id=terrain_quad_vert1_admit]
				PROJECTZ_SITE("terrain_quad_vert1_admit", "BoolAdmission");
				clipData = eye->projectForTerrainAdmission(vertex3D,screenPos);
				pz_capture_vert_preds(1);
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
				if (!isVisible)
				{
					clipData = false;
					vertices[1]->hazeFactor = 1.0f;
				}

				if (eye->usePerspective)
					vertices[1]->clipInfo = clipData; //onScreen;
				else
					vertices[1]->clipInfo = clipData;
 
				vertices[1]->wx = screenPos.x;
				vertices[1]->wy = screenPos.y;
				vertices[1]->wz = screenPos.z;
				vertices[1]->ww = screenPos.w;
	
				vertices[1]->calcThisFrame |= 2;

				if (clipData)
				{
					if (screenPos.z < leastZ)
					{
						leastZ = screenPos.z;
					}

					if (screenPos.z > mostZ)
					{
						mostZ = screenPos.z;
					}

					if (screenPos.w < leastW)
					{
						leastW = screenPos.w;
						leastWY = screenPos.y;
					}

					if (screenPos.w > mostW)
					{
						mostW = screenPos.w;
						mostWY = screenPos.y;
					}
				}
			}
		}

		if (!(vertices[2]->calcThisFrame & 2))
		{
			if (clipped1 || clipped2)
			{
				if (vertices[2]->pVertex->water & 128)
				{
					vertices[2]->wAlpha = -Terrain::frameCosAlpha;
					ourCos = negCos;
				}
				else if (vertices[2]->pVertex->water & 64)
				{
					vertices[2]->wAlpha = Terrain::frameCosAlpha;
					ourCos = Terrain::frameCos;
				}
	
				vertex3D.z = ourCos + Terrain::waterElevation;
				vertex3D.x = vertices[2]->vx;
				vertex3D.y = vertices[2]->vy;

				bool clipData = false;
				// [PROJECTZ:BoolAdmission id=terrain_quad_vert2_admit]
				PROJECTZ_SITE("terrain_quad_vert2_admit", "BoolAdmission");
				clipData = eye->projectForTerrainAdmission(vertex3D,screenPos);
				pz_capture_vert_preds(2);
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
				if (!isVisible)
				{
					clipData = false;
					vertices[2]->hazeFactor = 1.0f;
				}

				if (eye->usePerspective)
					vertices[2]->clipInfo = clipData; //onScreen;
				else
					vertices[2]->clipInfo = clipData;
					
				vertices[2]->wx = screenPos.x;
				vertices[2]->wy = screenPos.y;
				vertices[2]->wz = screenPos.z;
				vertices[2]->ww = screenPos.w;
	
				vertices[2]->calcThisFrame |= 2;

				if (clipData)
				{
					if (screenPos.z < leastZ)
					{
						leastZ = screenPos.z;
					}

					if (screenPos.z > mostZ)
					{
						mostZ = screenPos.z;
					}

					if (screenPos.w < leastW)
					{
						leastW = screenPos.w;
						leastWY = screenPos.y;
					}

					if (screenPos.w > mostW)
					{
						mostW = screenPos.w;
						mostWY = screenPos.y;
					}
				}
			}
		}

		if (!(vertices[3]->calcThisFrame & 2))
		{
			if (clipped1 || clipped2)
			{
				if (vertices[3]->pVertex->water & 128)
				{
					vertices[3]->wAlpha = -Terrain::frameCosAlpha;
					ourCos = negCos;
				}
				else if (vertices[3]->pVertex->water & 64)
				{
					vertices[3]->wAlpha = Terrain::frameCosAlpha;
					ourCos = Terrain::frameCos;
				}
	
				vertex3D.z = ourCos + Terrain::waterElevation;
				vertex3D.x = vertices[3]->vx;
				vertex3D.y = vertices[3]->vy;

				bool clipData = false;
				// [PROJECTZ:BoolAdmission id=terrain_quad_vert3_admit]
				PROJECTZ_SITE("terrain_quad_vert3_admit", "BoolAdmission");
				clipData = eye->projectForTerrainAdmission(vertex3D,screenPos);
				pz_capture_vert_preds(3);
				bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
				if (!isVisible)
				{
					clipData = false;
					vertices[3]->hazeFactor = 1.0f;
				}

				if (eye->usePerspective)
					vertices[3]->clipInfo = clipData; //onScreen;
				else
					vertices[3]->clipInfo = clipData;
				 
				vertices[3]->wx = screenPos.x;
				vertices[3]->wy = screenPos.y;
				vertices[3]->wz = screenPos.z;
				vertices[3]->ww = screenPos.w;
	
				vertices[3]->calcThisFrame |= 2;

				if (clipData)
				{
					if (screenPos.z < leastZ)
					{
						leastZ = screenPos.z;
					}

					if (screenPos.z > mostZ)
					{
						mostZ = screenPos.z;
					}

					if (screenPos.w < leastW)
					{
						leastW = screenPos.w;
						leastWY = screenPos.y;
					}

					if (screenPos.w > mostW)
					{
						mostW = screenPos.w;
						mostWY = screenPos.y;
					}
				}
			}
		}

		if (clipped1 || clipped2)
		{
			if (!Terrain::terrainTextures2)
			{
				DWORD waterDetailData = Terrain::terrainTextures->setDetail(0,sprayFrame);
				waterHandle = Terrain::terrainTextures->getTextureHandle(MapData::WaterTXMData & 0x0000ffff);
				waterDetailHandle = Terrain::terrainTextures->getDetailHandle(waterDetailData & 0x0000ffff); 
			}
			else
			{
				waterHandle = Terrain::terrainTextures2->getWaterTextureHandle();
				waterDetailHandle = Terrain::terrainTextures2->getWaterDetailHandle(sprayFrame);
			}
			
			mcTextureManager->addTriangle(waterHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
			mcTextureManager->addTriangle(waterHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
			mcTextureManager->addTriangle(waterDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
			mcTextureManager->addTriangle(waterDetailHandle,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
		}
		else
		{
			waterHandle = 0xffffffff;
			waterDetailHandle = 0xffffffff;
		}
	}
	else
	{
		waterHandle = 0xffffffff;
		waterDetailHandle = 0xffffffff;
	}

	if (terrainHandle != 0xffffffff)
	{
		//-----------------------------------------------------
		// FOG time.
		float fogStart = eye->fogStart;
		float fogFull = eye->fogFull;

		//-----------------------------------------------------
		// Process Vertex 0 if not already done
		if (!(vertices[0]->calcThisFrame & 1)) 
		{
			DWORD specR=0, specG=0, specB=0;
			DWORD lightr=0xff,lightg=0xff,lightb=0xff;
			if (Environment.Renderer != 3)
			{
				//------------------------------------------------------------
				float lightIntensity = vertices[0]->pVertex->vertexNormal * eye->lightDirection;

				unsigned char shadow = vertices[0]->pVertex->shadow;
				if (shadow && lightIntensity > 0.2f)
				{
					lightIntensity = 0.2f;
				}

				lightr = eye->getLightRed(lightIntensity);
				lightg = eye->getLightGreen(lightIntensity);
				lightb = eye->getLightBlue(lightIntensity);

				if (BaseVertexColor)
				{
					lightr += ((BaseVertexColor>>16) & 0x000000ff);
					if (lightr > 0xff)
						lightr = 0xff;

					lightg += ((BaseVertexColor>>8) & 0x000000ff);
					if (lightg > 0xff)
						lightg = 0xff;

					lightb += (BaseVertexColor & 0x000000ff);
					if (lightb > 0xff)
						lightb = 0xff;
				}

				if (rainLightLevel < 1.0f)
				{
					lightr = (float)lightr * rainLightLevel;
					lightb = (float)lightb * rainLightLevel;
					lightg = (float)lightg * rainLightLevel;
				}

				if (lighteningLevel > 0x0)
				{
					specR = specG = specB = lighteningLevel;
				}

				vertices[0]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);

				//First two light are already factored into the above equations!
				for (long i=2;i<eye->getNumTerrainLights();i++)
				{
					TG_LightPtr thisLight = eye->getTerrainLight(i);
					if (thisLight)
					{
	//					if (useShadows)
						{
							if ((thisLight->lightType == TG_LIGHT_POINT) || 
								(thisLight->lightType == TG_LIGHT_SPOT) ||
								(thisLight->lightType == TG_LIGHT_TERRAIN))
							{
								Stuff::Point3D vertexToLight;

								vertexToLight.x = vertices[0]->vx;
								vertexToLight.y = vertices[0]->vy;
								vertexToLight.z = vertices[0]->pVertex->elevation;

								vertexToLight -= thisLight->position;

								float length = vertexToLight.GetApproximateLength();
								float falloff = 1.0f;

								if (thisLight->GetFalloff(length, falloff))
								{
									float red,green,blue;
									red = float((thisLight->GetaRGB()>>16) & 0x000000ff) * falloff;
									green = float((thisLight->GetaRGB()>>8) & 0x000000ff) * falloff;
									blue = float((thisLight->GetaRGB()) & 0x000000ff) * falloff;

									specR += (DWORD)red;
									specG += (DWORD)green;
									specB += (DWORD)blue;

									if (specR > 255)
										specR = 255;

									if (specG > 255)
										specG = 255;

									if (specB > 255)
										specB = 255;
								}
							}
						}
					}
				}
			}
			
			vertices[0]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);
			vertices[0]->fogRGB = (0xff<<24) + (specR<<16) + (specG << 8) + (specB);
				
			//Fog
			DWORD fogResult = 0xff;
			if (!(vertices[0]->calcThisFrame & 1)) 
			{
				if (useFog)
				{
					if (vertices[0]->pVertex->elevation < fogStart)
					{
						float fogFactor = fogStart - vertices[0]->pVertex->elevation;
						if (fogFactor < 0.0)
						{
							fogResult = 0xff;
						}
						else
						{
							fogFactor /= (fogStart - fogFull);
							if (fogFactor <= 1.0)
							{
								fogFactor *= fogFactor;
								fogFactor = 1.0 - fogFactor;
								fogFactor *= 256.0;
							}
							else
							{
								fogFactor = 256.0;
							}
							
							fogResult = float2long(fogFactor);
						}
					}
					else
					{
						fogResult = 0xff;
					}
					
					vertices[0]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
				}
			}

			//-------------------
			// Distance FOG now.
			if (vertices[0]->hazeFactor != 0.0f)
			{
				float fogFactor = 1.0 - vertices[0]->hazeFactor;
				DWORD distFog = float2long(fogFactor * 255.0f);
				
				if (distFog < fogResult)
				   fogResult = distFog;
				
				vertices[0]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
			}

			vertices[0]->calcThisFrame |= 1;
		}

		//-----------------------------------------------------
		// Process Vertex 1 if not already done
		if (!(vertices[1]->calcThisFrame & 1))
		{
			DWORD specR=0, specG=0, specB=0;
			DWORD lightr=0xff,lightg=0xff,lightb=0xff;
			if (Environment.Renderer != 3)
			{
				float lightIntensity = vertices[1]->pVertex->vertexNormal * eye->lightDirection;

				unsigned char shadow = vertices[1]->pVertex->shadow;
				if (shadow && lightIntensity > 0.2f)
				{
					lightIntensity = 0.2f;
				}

				lightr = eye->getLightRed(lightIntensity);
				lightg = eye->getLightGreen(lightIntensity);
				lightb = eye->getLightBlue(lightIntensity);

				if (BaseVertexColor)
				{
					lightr += ((BaseVertexColor>>16) & 0x000000ff);
					if (lightr > 0xff)
						lightr = 0xff;

					lightg += ((BaseVertexColor>>8) & 0x000000ff);
					if (lightg > 0xff)
						lightg = 0xff;

					lightb += (BaseVertexColor & 0x000000ff);
					if (lightb > 0xff)
						lightb = 0xff;
				}
				if (rainLightLevel < 1.0f)
				{
					lightr = (float)lightr * rainLightLevel;
					lightb = (float)lightb * rainLightLevel;
					lightg = (float)lightg * rainLightLevel;
				}

				if (lighteningLevel > 0x0)
				{
					specR = specG = specB = lighteningLevel;
				}
				vertices[1]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);

				//First two light are already factored into the above equations!
				for (long i=2;i<eye->getNumTerrainLights();i++)
				{
					TG_LightPtr thisLight = eye->getTerrainLight(i);
					if (thisLight)
					{
	//					if (useShadows)
						{
							if ((thisLight->lightType == TG_LIGHT_POINT) || 
								(thisLight->lightType == TG_LIGHT_SPOT) ||
								(thisLight->lightType == TG_LIGHT_TERRAIN))
							{
								Stuff::Point3D vertexToLight;
								vertexToLight.x = vertices[1]->vx;
								vertexToLight.y = vertices[1]->vy;
								vertexToLight.z = vertices[1]->pVertex->elevation;

								vertexToLight -= thisLight->position;

								float length = vertexToLight.GetApproximateLength();
								float falloff = 1.0f;

								if (thisLight->GetFalloff(length, falloff))
								{
									float red,green,blue;

									red = float((thisLight->GetaRGB()>>16) & 0x000000ff) * falloff;
									green = float((thisLight->GetaRGB()>>8) & 0x000000ff) * falloff;
									blue = float((thisLight->GetaRGB()) & 0x000000ff) * falloff;

									specR += (DWORD)red;
									specG += (DWORD)green;
									specB += (DWORD)blue;

									if (specR > 255)
										specR = 255;

									if (specG > 255)
										specG = 255;

									if (specB > 255)
										specB = 255;
								}
							}
						}
					}
				}
			}
			
			vertices[1]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);
			vertices[1]->fogRGB = (0xff<<24) + (specR<<16) + (specG << 8) + (specB);

			//Fog
			DWORD fogResult = 0xff;
			if (Environment.Renderer != 3)
			{
				if (useFog)
				{
					if (vertices[1]->pVertex->elevation < fogStart)
					{
					   float fogFactor = fogStart - vertices[1]->pVertex->elevation;
					   if (fogFactor < 0.0)
					   {
						   fogResult = 0xff;
					   }
					   else
					   {
						   fogFactor /= (fogStart - fogFull);
						   if (fogFactor <= 1.0)
						   {
							   fogFactor *= fogFactor;
							   fogFactor = 1.0 - fogFactor;
							   fogFactor *= 256.0;
						   }
						   else
						   {
							   fogFactor = 256.0;
						   }
						   
						   fogResult = float2long(fogFactor);
					   }
					}
					else
					{
					   fogResult = 0xff;
					}
					
					vertices[1]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
				}
			}

			//-------------------
			// Distance FOG now.
			if (vertices[1]->hazeFactor != 0.0f)
			{
				float fogFactor = 1.0 - vertices[1]->hazeFactor;
				DWORD distFog = float2long(fogFactor * 255.0);
				
				if (distFog < fogResult)
				   fogResult = distFog;
				   
				vertices[1]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
			}

			vertices[1]->calcThisFrame |= 1;
		}

		//-----------------------------------------------------
		// Process Vertex 2 if not already done
		if (!(vertices[2]->calcThisFrame & 1))
		{
			DWORD specR=0, specG=0, specB=0;
			DWORD lightr=0xff,lightg=0xff,lightb=0xff;
			if (Environment.Renderer != 3)
			{
				float lightIntensity = vertices[2]->pVertex->vertexNormal * eye->lightDirection;
	
				unsigned char shadow = vertices[2]->pVertex->shadow;
				if (shadow && lightIntensity > 0.2f)
				{
					lightIntensity = 0.2f;
				}
							  
				lightr = eye->getLightRed(lightIntensity);
				lightg = eye->getLightGreen(lightIntensity);
				lightb = eye->getLightBlue(lightIntensity);
					
				if (BaseVertexColor)
				{
					lightr += ((BaseVertexColor>>16) & 0x000000ff);
					if (lightr > 0xff)
						lightr = 0xff;
						
					lightg += ((BaseVertexColor>>8) & 0x000000ff);
					if (lightg > 0xff)
						lightg = 0xff;
						
					lightb += (BaseVertexColor & 0x000000ff);
					if (lightb > 0xff)
						lightb = 0xff;
				}
				if (rainLightLevel < 1.0f)
				{
					lightr = (float)lightr * rainLightLevel;
					lightb = (float)lightb * rainLightLevel;
					lightg = (float)lightg * rainLightLevel;
				}

				if (lighteningLevel > 0x0)
				{
					specR = specG = specB = lighteningLevel;
				}
				vertices[2]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);

				//First two light are already factored into the above equations!
				for (long i=2;i<eye->getNumTerrainLights();i++)
				{
					TG_LightPtr thisLight = eye->getTerrainLight(i);
					if (thisLight)
					{
	//					if (useShadows)
						{
							if ((thisLight->lightType == TG_LIGHT_POINT) || 
								(thisLight->lightType == TG_LIGHT_SPOT) ||
								(thisLight->lightType == TG_LIGHT_TERRAIN))
							{
								Stuff::Point3D vertexToLight;
								vertexToLight.x = vertices[2]->vx;
								vertexToLight.y = vertices[2]->vy;
								vertexToLight.z = vertices[2]->pVertex->elevation;

								vertexToLight -= thisLight->position;

								float length = vertexToLight.GetApproximateLength();
								float falloff = 1.0f;

								if (thisLight->GetFalloff(length, falloff))
								{
									float red,green,blue;

									red = float((thisLight->GetaRGB()>>16) & 0x000000ff) * falloff;
									green = float((thisLight->GetaRGB()>>8) & 0x000000ff) * falloff;
									blue = float((thisLight->GetaRGB()) & 0x000000ff) * falloff;

									specR += (DWORD)red;
									specG += (DWORD)green;
									specB += (DWORD)blue;

									if (specR > 255)
										specR = 255;

									if (specG > 255)
										specG = 255;

									if (specB > 255)
										specB = 255;
								}
							}
						}
					}
				}
			}
			
			vertices[2]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);
			vertices[2]->fogRGB = (0xff<<24) + (specR<<16) + (specG << 8) + (specB);

			//Fog
			DWORD fogResult = 0xff;
			if (Environment.Renderer != 3)
			{
				if (useFog)
				{
				   if (vertices[2]->pVertex->elevation < fogStart)
				   {
					   float fogFactor = fogStart - vertices[2]->pVertex->elevation;
					   if ((fogFactor < 0.0) || (0.0 == (fogStart - fogFull)))
					   {
						   fogResult = 0xff;
					   }
					   else
					   {
						   fogFactor /= (fogStart - fogFull);
						   if (fogFactor <= 1.0)
						   {
							   fogFactor *= fogFactor;
							   fogFactor = 1.0 - fogFactor;
							   fogFactor *= 256.0;
						   }
						   else
						   {
							   fogFactor = 256.0;
						   }
						   
						   fogResult = float2long(fogFactor);
					   }
				   }
				   else
				   {
					   fogResult = 0xff;
				   }
					
					vertices[2]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
				}
			}

			//-------------------
			// Distance FOG now.
			if (vertices[2]->hazeFactor != 0.0f)
			{
				float fogFactor = 1.0 - vertices[2]->hazeFactor;
				DWORD distFog = float2long(fogFactor * 255.0f);
				
				if (distFog < fogResult)
				   fogResult = distFog;
				   
				vertices[2]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
			}

			vertices[2]->calcThisFrame |= 1;
		}

		//-----------------------------------------------------
		// Process Vertex 3 if not already done
		if (!(vertices[3]->calcThisFrame & 1))
		{
			DWORD specR=0, specG=0, specB=0;
			DWORD lightr=0xff,lightg=0xff,lightb=0xff;
			if (Environment.Renderer != 3)
			{
				float lightIntensity = vertices[3]->pVertex->vertexNormal * eye->lightDirection;
	
				unsigned char shadow = vertices[3]->pVertex->shadow;
				if (shadow && lightIntensity > 0.2f)
				{
					lightIntensity = 0.2f;
				}
							  
				lightr = eye->getLightRed(lightIntensity);
				lightg = eye->getLightGreen(lightIntensity);
				lightb = eye->getLightBlue(lightIntensity);
					
				if (BaseVertexColor)
				{
					lightr += ((BaseVertexColor>>16) & 0x000000ff);
					if (lightr > 0xff)
						lightr = 0xff;
						
					lightg += ((BaseVertexColor>>8) & 0x000000ff);
					if (lightg > 0xff)
						lightg = 0xff;
						
					lightb += (BaseVertexColor & 0x000000ff);
					if (lightb > 0xff)
						lightb = 0xff;
				}
				if (rainLightLevel < 1.0f)
				{
					lightr = (float)lightr * rainLightLevel;
					lightb = (float)lightb * rainLightLevel;
					lightg = (float)lightg * rainLightLevel;
				}

				if (lighteningLevel > 0x0)
				{
					specR = specG = specB = lighteningLevel;
				}
				vertices[3]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);

				//First two light are already factored into the above equations!
				for (long i=2;i<eye->getNumTerrainLights();i++)
				{
					TG_LightPtr thisLight = eye->getTerrainLight(i);
					if (thisLight)
					{
	//					if (useShadows)
						{
							if ((thisLight->lightType == TG_LIGHT_POINT) || 
								(thisLight->lightType == TG_LIGHT_SPOT) ||
								(thisLight->lightType == TG_LIGHT_TERRAIN))
							{
								Stuff::Point3D vertexToLight;
								vertexToLight.x = vertices[3]->vx;
								vertexToLight.y = vertices[3]->vy;
								vertexToLight.z = vertices[3]->pVertex->elevation;

								vertexToLight -= thisLight->position;

								float length = vertexToLight.GetApproximateLength();
								float falloff = 1.0f;

								if (thisLight->GetFalloff(length, falloff))
								{
									float red,green,blue;

									red = float((thisLight->GetaRGB()>>16) & 0x000000ff) * falloff;
									green = float((thisLight->GetaRGB()>>8) & 0x000000ff) * falloff;
									blue = float((thisLight->GetaRGB()) & 0x000000ff) * falloff;

									specR += (DWORD)red;
									specG += (DWORD)green;
									specB += (DWORD)blue;

									if (specR > 255)
										specR = 255;

									if (specG > 255)
										specG = 255;

									if (specB > 255)
										specB = 255;
								}
							}
						}
					}
				}
			}
			
			vertices[3]->lightRGB = lightb + (lightr<<16) + (lightg << 8) + (0xff << 24);
			vertices[3]->fogRGB = (0xff<<24) + (specR<<16) + (specG << 8) + (specB);
			
			//Fog
			DWORD fogResult = 0xff;
			if (Environment.Renderer != 3)
			{
				if (useFog)
				{
					if (vertices[3]->pVertex->elevation < fogStart)
					{
					   float fogFactor = fogStart - vertices[3]->pVertex->elevation;
					   if (fogFactor < 0.0)
					   {
						   fogResult = 0xff;
					   }
					   else
					   {
						   fogFactor /= (fogStart - fogFull);
						   if (fogFactor <= 1.0)
						   {
							   fogFactor *= fogFactor;
							   fogFactor = 1.0 - fogFactor;
							   fogFactor *= 256.0;
						   }
						   else
						   {
							   fogFactor = 256.0;
						   }
						   
						   fogResult = float2long(fogFactor);
					   }
					}
					else
					{
					   fogResult = 0xff;
					}
				}
			}

			//-------------------
			// Distance FOG now.
			if (vertices[3]->hazeFactor != 0.0f)
			{
				float fogFactor = 1.0 - vertices[3]->hazeFactor;
				DWORD distFog = float2long(fogFactor * 255.0f);
				
				if (distFog < fogResult)
				   fogResult = distFog;
				   
				vertices[3]->fogRGB = (fogResult<<24) + (specR<<16) + (specG << 8) + (specB);
			}

			vertices[3]->calcThisFrame |= 1;
		}
	}
}

#define TERRAIN_DEPTH_FUDGE		0.001f
#define OVERLAY_ELEV_OFFSET		0.15f

// GPU projection: pack MC2 world coords into overlay vertex instead of screen-space
static inline void setOverlayWorldCoords(gos_VERTEX& v, const VertexPtr src) {
	v.x = src->vx;
	v.y = src->vy;
	v.z = src->pVertex->elevation + OVERLAY_ELEV_OFFSET;
	v.rhw = 1.0f;
}

//---------------------------------------------------------------------------
void TerrainQuad::draw (void)
{
	if (terrainHandle != 0xffffffff)
	{
		numTerrainFaces++;

		// M2b: pure-water early-exit. Quads with no base terrain texture, no overlay,
		// and no water-interest detail have nothing to emit here — addVertices, appendQuad,
		// the M2 fast-path emit, and overlay/detail draws are all gated on these
		// conditions. Water surface is rendered separately by Terrain::renderWater().
		// On water-heavy maps (mc2_01 ~75% water) this skips ~28K wasted iterations/frame.
		// Most pure-water quads are loop-hoisted in Terrain::render (terrain.cpp); this
		// is the in-function fallback for when the global useOverlayTexture /
		// useWaterInterestTexture flags get toggled and the loop check no longer matches.
		if (terrainHandle == 0
		    && !(useOverlayTexture && overlayHandle != 0xffffffff)
		    && !(useWaterInterestTexture && terrainDetailHandle != 0xffffffff))
		{
		    return;
		}

		//---------------------------------------
		// GOS 3D draw Calls now!

		// M2b instrumentation: pre-branch setup zone. Wraps gVertex decl, minU/maxU
		// compute, optional uvData read, camPosition load, fastPathEligible 7-flag
		// computation (4 PatchStream method calls), and the path_mix counter. This
		// runs for every quad that didn't pure-water early-exit. Closes at end of
		// the outer-if scope, so it INCLUDES the fast-path and legacy bodies — but
		// FP.* and Quad.legacy zones are children, so Tracy will show preBranch self
		// time = (preBranch total) - (FP.* + Quad.legacy + Legacy.*).
		gos_VERTEX gVertex[3];

        // sebi half pixel, hope this is correct :-)
		float minU = 0.5f / TERRAIN_TXM_SIZE;
		float maxU = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
		float minV = 0.5f / TERRAIN_TXM_SIZE;
		float maxV = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
		float oldminU = 0.0078125f;
		float oldmaxU = 0.9921875f;
		float oldminV = 0.0078125f;
		float oldmaxV = 0.9921875f;

		if (Terrain::terrainTextures2 && !(overlayHandle == 0xffffffff && isCement))
		{
			minU = uvData.minU;
			minV = uvData.minV;
			maxU = uvData.maxU;
			maxV = uvData.maxV;
		}

		Stuff::Point3D camPosition;
		camPosition = *TG_Shape::s_cameraOrigin;

		// === M2 fast-path eligibility ===
		// Quads enter the fast path when:
		//   (a) terrainHandle != 0           → emit thin record + optional detail
		//   (b) terrainHandle == 0 + detail  → emit detail only, no thin record
		// Quads with overlayHandle != 0xffffffff still fall to legacy (M2d-overlay
		// would absorb them; ~4-5K quads/frame on splatmap-heavy scenes).
		// Per-gate flags retained so the path_mix diagnostic counter below can
		// attribute fastPathEligible failures to the specific gate.
		const bool g_active   = TerrainPatchStream::isFastPathActive();
		const bool g_thin     = TerrainPatchStream::isThinRecordsActive();
		const bool g_ready    = TerrainPatchStream::isReady();
		const bool g_notOver  = !TerrainPatchStream::isOverflowed();
		const bool g_handle   = (terrainHandle != 0);
		const bool g_noOverlay= !(useOverlayTexture && overlayHandle != 0xffffffff);
		const bool g_noWater  = !(useWaterInterestTexture && terrainDetailHandle != 0xffffffff);
		const bool fastPathEligible =
		    g_active && g_thin && g_ready && g_notOver && g_noOverlay
		    && (g_handle || !g_noWater);

		// MC2_THIN_DEBUG=1: per-gate fail counts to pinpoint which condition is culling
		// most quads from the fast path. Demoted to silent-by-default per CLAUDE.md
		// debug-instrumentation rule. Holds off until ~frame 1200 to skip warmup load
		// and capture steady-state. Prints 5 frames then stops; per-quad cost is one
		// branch on s_framesPrinted after that (effectively zero).
		{
		    static const bool s_pathMixOn = (getenv("MC2_THIN_DEBUG") != nullptr);
		    static uint32_t s_fast = 0, s_legacy = 0;
		    static uint32_t s_failActive = 0, s_failThin = 0, s_failReady = 0, s_failOver = 0;
		    static uint32_t s_failHandle = 0, s_failOverlay = 0, s_failWater = 0;
		    static uint32_t s_framesPrinted = 0;
		    static uint32_t s_quadsThisFrame = 0;
		    static uint32_t s_frameCounter = 0;
		    constexpr uint32_t kWarmupHoldoffFrames = 1200;  // ~15s @ 80fps
		    if (s_pathMixOn && s_framesPrinted < 5) {
		        const bool warmedUp = (s_frameCounter >= kWarmupHoldoffFrames);
		        if (warmedUp) {
		            if (fastPathEligible) ++s_fast;
		            else {
		                ++s_legacy;
		                if      (!g_active)    ++s_failActive;
		                else if (!g_thin)      ++s_failThin;
		                else if (!g_ready)     ++s_failReady;
		                else if (!g_notOver)   ++s_failOver;
		                else if (!g_handle)    ++s_failHandle;
		                else if (!g_noOverlay) ++s_failOverlay;
		                else if (!g_noWater)   ++s_failWater;
		            }
		        }
		        if (++s_quadsThisFrame >= 14000) {
		            ++s_frameCounter;
		            if (warmedUp) {
		                fprintf(stderr,
		                    "[THIN_DEBUG v1] event=path_mix frame=%u (post-warmup) fast=%u legacy=%u "
		                    "fail_active=%u fail_thin=%u fail_ready=%u fail_over=%u "
		                    "fail_handle0=%u fail_overlay=%u fail_water=%u\n",
		                    s_framesPrinted, s_fast, s_legacy,
		                    s_failActive, s_failThin, s_failReady, s_failOver,
		                    s_failHandle, s_failOverlay, s_failWater);
		                fflush(stderr);
		                s_fast = s_legacy = 0;
		                s_failActive = s_failThin = s_failReady = s_failOver = 0;
		                s_failHandle = s_failOverlay = s_failWater = 0;
		                ++s_framesPrinted;
		            }
		            s_quadsThisFrame = 0;
		        }
		    }
		}

		if (fastPathEligible)
		{
		    // pz validity — vertices[c]->pz is pre-projected by the camera transform pass.
		    // Range [0,1) is in-clip; outside is behind-camera or far-clipped.
		    bool pzc[4];
		    for (int c = 0; c < 4; c++) {
		        float pz_adj = vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
		        pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
		    }

		    bool pzTri1, pzTri2;
		    if (uvMode == BOTTOMLEFT) {
		        // tri1 = corners [0,1,3], tri2 = corners [1,2,3]
		        pzTri1 = pzc[0] && pzc[1] && pzc[3];
		        pzTri2 = pzc[1] && pzc[2] && pzc[3];
		    } else {
		        // BOTTOMRIGHT (= TOPRIGHT diagonal): tri1 = [0,1,2], tri2 = [0,2,3]
		        pzTri1 = pzc[0] && pzc[1] && pzc[2];
		        pzTri2 = pzc[0] && pzc[2] && pzc[3];
		    }

		    if (!pzTri1 && !pzTri2) return;  // both culled — skip entirely

		    // === Thin-record emit path (terrainHandle != 0 only) ===
		    // Detail-only quads (terrainHandle == 0 + has detail) skip this entire block
		    // and go straight to the M2c detail emit below. The thin record is a base-
		    // texture submission; quads without a base texture have nothing to put in it.
		    if (terrainHandle != 0)
		    {
		        const bool isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
		        const bool isAlpha  = Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff);
		        const bool alphaOverride = Terrain::terrainTextures2 && (!isCement || isAlpha);

		        // effectiveLightRGB: alphaOverride first, then selected (matching legacy priority)
		        auto lightRGBc = [&](int c) -> uint32_t {
		            DWORD lc = vertices[c]->lightRGB;
		            if (alphaOverride) lc = 0xffffffffu;
		            if (vertices[c]->pVertex->selected) lc = static_cast<DWORD>(SELECTION_COLOR);
		            return static_cast<uint32_t>(lc);
		        };

		        // Lazy recipe build: peek the cache first using only corner-0 (wx0, wy0).
		        // ~99% of quads hit the cache after warmup; building the full recipe (~20
		        // cold reads through vertices[c]->pVertex->{elevation, vertexNormal,
		        // terrainType}, ~30 stack stores, 4× terrainTypeToMaterial, bit-pack +
		        // memcpy) on a hit is pure waste — ensureRecipeForQuad discards `recipe`
		        // on hit. Build it only on miss.
		        const float wx0 = vertices[0]->vx;
		        const float wy0 = vertices[0]->vy;
		        const uint64_t recipeKey = TerrainPatchStream::makeRecipeKey(wx0, wy0);
		        uint32_t recipeIdx = TerrainPatchStream::tryGetRecipeIdx(recipeKey);
		        if (recipeIdx == UINT32_MAX)
		        {
		            TerrainQuadRecipe recipe;
		            recipe.wx0=wx0;             recipe.wy0=wy0;             recipe.wz0=vertices[0]->pVertex->elevation; recipe._wp0=0.f;
		            recipe.wx1=vertices[1]->vx; recipe.wy1=vertices[1]->vy; recipe.wz1=vertices[1]->pVertex->elevation; recipe._wp1=0.f;
		            recipe.wx2=vertices[2]->vx; recipe.wy2=vertices[2]->vy; recipe.wz2=vertices[2]->pVertex->elevation; recipe._wp2=0.f;
		            recipe.wx3=vertices[3]->vx; recipe.wy3=vertices[3]->vy; recipe.wz3=vertices[3]->pVertex->elevation; recipe._wp3=0.f;
		            recipe.nx0=vertices[0]->pVertex->vertexNormal.x; recipe.ny0=vertices[0]->pVertex->vertexNormal.y; recipe.nz0=vertices[0]->pVertex->vertexNormal.z; recipe._np0=0.f;
		            recipe.nx1=vertices[1]->pVertex->vertexNormal.x; recipe.ny1=vertices[1]->pVertex->vertexNormal.y; recipe.nz1=vertices[1]->pVertex->vertexNormal.z; recipe._np1=0.f;
		            recipe.nx2=vertices[2]->pVertex->vertexNormal.x; recipe.ny2=vertices[2]->pVertex->vertexNormal.y; recipe.nz2=vertices[2]->pVertex->vertexNormal.z; recipe._np2=0.f;
		            recipe.nx3=vertices[3]->pVertex->vertexNormal.x; recipe.ny3=vertices[3]->pVertex->vertexNormal.y; recipe.nz3=vertices[3]->pVertex->vertexNormal.z; recipe._np3=0.f;
		            recipe.minU=minU; recipe.minV=minV; recipe.maxU=maxU; recipe.maxV=maxV;

		            // Pack 4 corner material types into _wp0 — bit-preserving write; shader reads via floatBitsToUint
		            {
		                uint32_t m0 = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
		                uint32_t m1 = terrainTypeToMaterial(vertices[1]->pVertex->terrainType);
		                uint32_t m2 = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
		                uint32_t m3 = terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
		                uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
		                memcpy(&recipe._wp0, &tpacked, 4);
		            }

		            recipeIdx = TerrainPatchStream::ensureRecipeForQuad(recipeKey, recipe);
		            if (recipeIdx == UINT32_MAX) goto fp_detail_only;  // SSBO full — try detail still
		        }

		        {
		            TerrainQuadThinRecord tr;
		            tr.recipeIdx     = recipeIdx;
		            tr.terrainHandle = static_cast<uint32_t>(terrainHandle);
		            tr.flags         = (uvMode == BOTTOMLEFT ? 1u : 0u)
		                             | (pzTri1 ? 2u : 0u)
		                             | (pzTri2 ? 4u : 0u);
		            tr._pad0         = 0u;
		            tr.lightRGB0     = lightRGBc(0);
		            tr.lightRGB1     = lightRGBc(1);
		            tr.lightRGB2     = lightRGBc(2);
		            tr.lightRGB3     = lightRGBc(3);
		            TerrainPatchStream::appendThinRecordDirect(tr);
		        }
		    }
		    fp_detail_only:;

		    // M2c: water-interest detail emit. Replaces the legacy detail-draw blocks
		    // at quad.cpp:1972/2115/2288/2429 — same output, but builds sVertex straight
		    // from vertices[c] instead of memcpy'ing gVertex and overriding most fields.
		    // Skipped for non-water-interest quads (the && short-circuits).
		    // ARGB matches legacy Option A: terrainTextures2 → 0xFFFFFFFFu, else
		    // vertices[c]->lightRGB (NO `selected` override on detail tiles, by design).
		    if (useWaterInterestTexture && terrainDetailHandle != 0xffffffff)
		    {
		        const float tilingFactor = Terrain::terrainTextures2
		            ? Terrain::terrainTextures2->getDetailTilingFactor()
		            : Terrain::terrainTextures->getDetailTilingFactor(1);
		        const float oneOverTf = tilingFactor / Terrain::worldUnitsMapSide;
		        const bool whitenArgb = (Terrain::terrainTextures2 != nullptr);

		        gos_VERTEX corner[4];
		        for (int c = 0; c < 4; c++) {
		            corner[c].x    = vertices[c]->px;
		            corner[c].y    = vertices[c]->py;
		            corner[c].z    = vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
		            corner[c].rhw  = vertices[c]->pw;
		            corner[c].u    = (vertices[c]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
		            corner[c].v    = (Terrain::mapTopLeft3d.y - vertices[c]->vy) * oneOverTf;
		            corner[c].argb = whitenArgb ? 0xFFFFFFFFu : (DWORD)vertices[c]->lightRGB;
		            corner[c].frgb = (vertices[c]->fogRGB & 0xFFFFFF00u)
		                           | terrainTypeToMaterial(vertices[c]->pVertex->terrainType);
		        }

		        auto clampUVs = [](gos_VERTEX* tri) {
		            if (tri[0].u > MaxMinUV || tri[0].v > MaxMinUV ||
		                tri[1].u > MaxMinUV || tri[1].v > MaxMinUV ||
		                tri[2].u > MaxMinUV || tri[2].v > MaxMinUV) {
		                float maxU = fmax(tri[0].u, fmax(tri[1].u, tri[2].u));
		                maxU = floor(maxU - (MaxMinUV - 1.0f));
		                float maxV = fmax(tri[0].v, fmax(tri[1].v, tri[2].v));
		                maxV = floor(maxV - (MaxMinUV - 1.0f));
		                tri[0].u -= maxU; tri[1].u -= maxU; tri[2].u -= maxU;
		                tri[0].v -= maxV; tri[1].v -= maxV; tri[2].v -= maxV;
		            }
		        };

		        // Triangle assembly mirrors the thin-record uvMode/pzTri rules.
		        if (uvMode == BOTTOMLEFT) {
		            if (pzTri1) {
		                gos_VERTEX tri[3] = { corner[0], corner[1], corner[3] };
		                clampUVs(tri);
		                mcTextureManager->addVertices(terrainDetailHandle, tri,
		                                              MC2_ISTERRAIN | MC2_DRAWALPHA);
		            }
		            if (pzTri2) {
		                gos_VERTEX tri[3] = { corner[1], corner[2], corner[3] };
		                clampUVs(tri);
		                mcTextureManager->addVertices(terrainDetailHandle, tri,
		                                              MC2_ISTERRAIN | MC2_DRAWALPHA);
		            }
		        } else {
		            // BOTTOMRIGHT (= TOPRIGHT diagonal)
		            if (pzTri1) {
		                gos_VERTEX tri[3] = { corner[0], corner[1], corner[2] };
		                clampUVs(tri);
		                mcTextureManager->addVertices(terrainDetailHandle, tri,
		                                              MC2_ISTERRAIN | MC2_DRAWALPHA);
		            }
		            if (pzTri2) {
		                gos_VERTEX tri[3] = { corner[0], corner[2], corner[3] };
		                clampUVs(tri);
		                mcTextureManager->addVertices(terrainDetailHandle, tri,
		                                              MC2_ISTERRAIN | MC2_DRAWALPHA);
		            }
		        }
		    }

		    return;
		}
		// === end M2 branch — legacy path continues below ===

		if (uvMode == BOTTOMRIGHT)
		{
			//--------------------------
			// Top Triangle
			DWORD lightRGB0 = vertices[0]->lightRGB; 
			DWORD lightRGB1 = vertices[1]->lightRGB; 
			DWORD lightRGB2 = vertices[2]->lightRGB; 
			bool isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
			bool isAlpha = Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff);

			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
				lightRGB0 = lightRGB1 = lightRGB2 = 0xffffffff;

			lightRGB0 = vertices[0]->pVertex->selected ? SELECTION_COLOR : lightRGB0;
			lightRGB1 = vertices[1]->pVertex->selected ? SELECTION_COLOR : lightRGB1;
			lightRGB2 = vertices[2]->pVertex->selected ? SELECTION_COLOR : lightRGB2;

			gVertex[0].x		= vertices[0]->px;
			gVertex[0].y		= vertices[0]->py;
			gVertex[0].z		= vertices[0]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->pw;
			gVertex[0].u		= minU;
			gVertex[0].v		= minV;
			gVertex[0].argb		= lightRGB0;
			gVertex[0].frgb		= vertices[0]->fogRGB;
			gVertex[0].frgb		= (gVertex[0].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[0]->pVertex->terrainType);

			gVertex[1].x		= vertices[1]->px;
			gVertex[1].y		= vertices[1]->py;
			gVertex[1].z		= vertices[1]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->pw;
			gVertex[1].u		= maxU;
			gVertex[1].v		= minV;
			gVertex[1].argb		= lightRGB1;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

			gVertex[2].x		= vertices[2]->px;
			gVertex[2].y		= vertices[2]->py;
			gVertex[2].z		= vertices[2]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[2]->pw;
			gVertex[2].u		= maxU;
			gVertex[2].v		= maxV;
			gVertex[2].argb		= lightRGB2;
			gVertex[2].frgb		= vertices[2]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

			// Capture tri1 pz result and vertex data before the destructive
			// gVertex shuffle for tri2. appendQuad (below) uses both.
			const bool pzTri1 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
			const gos_VERTEX gvTri1[3] = {gVertex[0], gVertex[1], gVertex[2]};
			if (pzTri1)
			{
				{
					// sebi: beware this will be drawn with alpha blending, so need to make sure that alpha is not zero, because this is a base terrain layer!
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[2]);
						// PatchStream append moved to appendQuad after both pz gates.
					}

					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldminU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldminV;
						oVertex[2].u		= oldmaxU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[0]->lightRGB;
						oVertex[1].argb		= vertices[1]->lightRGB;
						oVertex[2].argb		= vertices[2]->lightRGB;

						// GPU projection: MC2 world coords into new typed batch
						setOverlayWorldCoords(oVertex[0], vertices[0]);
						setOverlayWorldCoords(oVertex[1], vertices[1]);
						setOverlayWorldCoords(oVertex[2], vertices[2]);

						{
							WorldOverlayVert wov[3];
							for (int _k = 0; _k < 3; ++_k) {
								wov[_k].wx   = oVertex[_k].x;
								wov[_k].wy   = oVertex[_k].y;
								wov[_k].wz   = oVertex[_k].z;
								wov[_k].u    = oVertex[_k].u;
								wov[_k].v    = oVertex[_k].v;
								wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
								wov[_k].argb = oVertex[_k].argb;
							}
							const DWORD overlayTexId = tex_resolve(overlayHandle);
							if (overlayTexId != 0)
								gos_PushTerrainOverlay(wov, overlayTexId);
						}
					}

					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);

						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();

						float oneOverTf		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTf;
		
						sVertex[1].u		= (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTf;
		
						sVertex[2].u		= (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTf;
						
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}

						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}

						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);	
					}
				}
			}

			//--------------------------
			//Bottom Triangle
			//
			// gVertex[0] same as above gVertex[0].
			// gVertex[1] is same as above gVertex[2].
			// gVertex[2] is calced from vertex[3].
			DWORD lightRGB3 = vertices[3]->lightRGB;
			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
				lightRGB3 = 0xffffffff;

			lightRGB3 = vertices[3]->pVertex->selected ? SELECTION_COLOR : lightRGB3;

			gVertex[1].x		= gVertex[2].x;	
			gVertex[1].y		= gVertex[2].y;	
			gVertex[1].z		= gVertex[2].z;	
			gVertex[1].rhw		= gVertex[2].rhw;
			gVertex[1].u		= gVertex[2].u;
			gVertex[1].v		= gVertex[2].v;	
			gVertex[1].argb		= gVertex[2].argb;
			gVertex[1].frgb		= gVertex[2].frgb;

			gVertex[2].x		= vertices[3]->px;
			gVertex[2].y		= vertices[3]->py;
			gVertex[2].z		= vertices[3]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->pw;
			gVertex[2].u		= minU;
			gVertex[2].v		= maxV;
			gVertex[2].argb		= lightRGB3;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

			const bool pzTri2 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
			if (pzTri2)
			{
				{
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[2], vertices[3]);
						// PatchStream append moved to appendQuad below.
					}

					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldminU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldmaxV;
						oVertex[2].u		= oldminU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[0]->lightRGB;
						oVertex[1].argb		= vertices[2]->lightRGB;
						oVertex[2].argb		= vertices[3]->lightRGB;

						// GPU projection: MC2 world coords into new typed batch
						setOverlayWorldCoords(oVertex[0], vertices[0]);
						setOverlayWorldCoords(oVertex[1], vertices[2]);
						setOverlayWorldCoords(oVertex[2], vertices[3]);

						{
							WorldOverlayVert wov[3];
							for (int _k = 0; _k < 3; ++_k) {
								wov[_k].wx   = oVertex[_k].x;
								wov[_k].wy   = oVertex[_k].y;
								wov[_k].wz   = oVertex[_k].z;
								wov[_k].u    = oVertex[_k].u;
								wov[_k].v    = oVertex[_k].v;
								wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
								wov[_k].argb = oVertex[_k].argb;
							}
							const DWORD overlayTexId = tex_resolve(overlayHandle);
							if (overlayTexId != 0)
								gos_PushTerrainOverlay(wov, overlayTexId);
						}
					}

					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);

						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();

 						float oneOverTF		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF;
		
						sVertex[1].u		= (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF;
		
						sVertex[2].u		= (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF;
		
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}

						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}

						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
				}
			}

			// PatchStream: one bucket lookup for both triangles of this quad.
			if (terrainHandle != 0 && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
				gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
				if (!TerrainPatchStream::isThinRecordsActive()) {
					if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[2], tx1);
					if (pzTri2) buildTerrainExtraTriple(vertices[0], vertices[2], vertices[3], tx2);
				}
				TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
				// M1 compact record — TOPRIGHT diagonal.
				// gvTri1[0..2] = corners 0,1,2 (saved before shuffle).
				// gVertex[2]   = corner 3 (constructed by shuffle from vertices[3]).
				TerrainQuadRecord rec;
				rec.wx0=vertices[0]->vx; rec.wy0=vertices[0]->vy; rec.wz0=vertices[0]->pVertex->elevation; rec._wp0=0.f;
				rec.wx1=vertices[1]->vx; rec.wy1=vertices[1]->vy; rec.wz1=vertices[1]->pVertex->elevation; rec._wp1=0.f;
				rec.wx2=vertices[2]->vx; rec.wy2=vertices[2]->vy; rec.wz2=vertices[2]->pVertex->elevation; rec._wp2=0.f;
				rec.wx3=vertices[3]->vx; rec.wy3=vertices[3]->vy; rec.wz3=vertices[3]->pVertex->elevation; rec._wp3=0.f;
				rec.nx0=vertices[0]->pVertex->vertexNormal.x; rec.ny0=vertices[0]->pVertex->vertexNormal.y; rec.nz0=vertices[0]->pVertex->vertexNormal.z; rec._np0=0.f;
				rec.nx1=vertices[1]->pVertex->vertexNormal.x; rec.ny1=vertices[1]->pVertex->vertexNormal.y; rec.nz1=vertices[1]->pVertex->vertexNormal.z; rec._np1=0.f;
				rec.nx2=vertices[2]->pVertex->vertexNormal.x; rec.ny2=vertices[2]->pVertex->vertexNormal.y; rec.nz2=vertices[2]->pVertex->vertexNormal.z; rec._np2=0.f;
				rec.nx3=vertices[3]->pVertex->vertexNormal.x; rec.ny3=vertices[3]->pVertex->vertexNormal.y; rec.nz3=vertices[3]->pVertex->vertexNormal.z; rec._np3=0.f;
				rec.minU=minU; rec.minV=minV; rec.maxU=maxU; rec.maxV=maxV;
				rec.lightRGB0=gvTri1[0].argb; rec.lightRGB1=gvTri1[1].argb; rec.lightRGB2=gvTri1[2].argb; rec.lightRGB3=gVertex[2].argb;
				rec.fogRGB0  =gvTri1[0].frgb; rec.fogRGB1  =gvTri1[1].frgb; rec.fogRGB2  =gvTri1[2].frgb; rec.fogRGB3  =gVertex[2].frgb;
				rec.terrainHandle = (uint32_t)terrainHandle;
				rec.flags = 0u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=0 → TOPRIGHT
				rec._ctrl2 = 0u; rec._ctrl3 = 0u;
				TerrainPatchStream::appendQuadRecord(rec);
				TerrainPatchStream::addRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				{
					TerrainQuadRecipe recipe;
					recipe.wx0=vertices[0]->vx; recipe.wy0=vertices[0]->vy; recipe.wz0=vertices[0]->pVertex->elevation; recipe._wp0=0.f;
					recipe.wx1=vertices[1]->vx; recipe.wy1=vertices[1]->vy; recipe.wz1=vertices[1]->pVertex->elevation; recipe._wp1=0.f;
					recipe.wx2=vertices[2]->vx; recipe.wy2=vertices[2]->vy; recipe.wz2=vertices[2]->pVertex->elevation; recipe._wp2=0.f;
					recipe.wx3=vertices[3]->vx; recipe.wy3=vertices[3]->vy; recipe.wz3=vertices[3]->pVertex->elevation; recipe._wp3=0.f;
					recipe.nx0=vertices[0]->pVertex->vertexNormal.x; recipe.ny0=vertices[0]->pVertex->vertexNormal.y; recipe.nz0=vertices[0]->pVertex->vertexNormal.z; recipe._np0=0.f;
					recipe.nx1=vertices[1]->pVertex->vertexNormal.x; recipe.ny1=vertices[1]->pVertex->vertexNormal.y; recipe.nz1=vertices[1]->pVertex->vertexNormal.z; recipe._np1=0.f;
					recipe.nx2=vertices[2]->pVertex->vertexNormal.x; recipe.ny2=vertices[2]->pVertex->vertexNormal.y; recipe.nz2=vertices[2]->pVertex->vertexNormal.z; recipe._np2=0.f;
					recipe.nx3=vertices[3]->pVertex->vertexNormal.x; recipe.ny3=vertices[3]->pVertex->vertexNormal.y; recipe.nz3=vertices[3]->pVertex->vertexNormal.z; recipe._np3=0.f;
					recipe.minU=minU; recipe.minV=minV; recipe.maxU=maxU; recipe.maxV=maxV;
					const uint32_t tFlags = 0u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=0 → TOPRIGHT
					{
						uint32_t m0 = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
						uint32_t m1 = terrainTypeToMaterial(vertices[1]->pVertex->terrainType);
						uint32_t m2 = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
						uint32_t m3 = terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
						uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
						memcpy(&recipe._wp0, &tpacked, 4);
					}
					TerrainPatchStream::appendThinRecord(terrainHandle, recipe, tFlags,
						gvTri1[0].argb, gvTri1[1].argb, gvTri1[2].argb, gVertex[2].argb);
					TerrainPatchStream::addThinRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				}
			}
		}
		else if (uvMode == BOTTOMLEFT)
		{
			//------------------------------
			// Top Triangle.
			DWORD lightRGB0 = vertices[0]->lightRGB;
			DWORD lightRGB1 = vertices[1]->lightRGB;
			DWORD lightRGB3 = vertices[3]->lightRGB;
			bool isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
			bool isAlpha = Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff);
			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
				lightRGB0 = lightRGB1 = lightRGB3 = 0xffffffff;

			lightRGB0 = vertices[0]->pVertex->selected ? SELECTION_COLOR : lightRGB0;
			lightRGB1 = vertices[1]->pVertex->selected ? SELECTION_COLOR : lightRGB1;
			lightRGB3 = vertices[3]->pVertex->selected ? SELECTION_COLOR : lightRGB3;

			gVertex[0].x		= vertices[0]->px;
			gVertex[0].y		= vertices[0]->py;
			gVertex[0].z		= vertices[0]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->pw;
			gVertex[0].u		= minU;
			gVertex[0].v		= minV;
			gVertex[0].argb		= lightRGB0;
			gVertex[0].frgb		= vertices[0]->fogRGB;
			gVertex[0].frgb		= (gVertex[0].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[0]->pVertex->terrainType);

			gVertex[1].x		= vertices[1]->px;
			gVertex[1].y		= vertices[1]->py;
			gVertex[1].z		= vertices[1]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->pw;
			gVertex[1].u		= maxU;
			gVertex[1].v		= minV;
			gVertex[1].argb		= lightRGB1;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

			gVertex[2].x		= vertices[3]->px;
			gVertex[2].y		= vertices[3]->py;
			gVertex[2].z		= vertices[3]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->pw;
			gVertex[2].u		= minU;
			gVertex[2].v		= maxV;
			gVertex[2].argb		= lightRGB3;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

			// Capture tri1 pz result and vertex data before the destructive
			// gVertex shuffle for tri2. appendQuad (below) uses both.
			const bool pzTri1 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
			const gos_VERTEX gvTri1[3] = {gVertex[0], gVertex[1], gVertex[2]};
			if (pzTri1)
			{
				{
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[3]);
						// PatchStream append moved to appendQuad after both pz gates.
					}

					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();
							
 						float oneOverTF		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF;
		
						sVertex[1].u		= (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF;
		
						sVertex[2].u		= (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF;
		
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}
						
						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}
						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
					
					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldminU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldminV;
						oVertex[2].u		= oldminU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[0]->lightRGB;
						oVertex[1].argb		= vertices[1]->lightRGB;
						oVertex[2].argb		= vertices[3]->lightRGB;

						// GPU projection: MC2 world coords into new typed batch
						setOverlayWorldCoords(oVertex[0], vertices[0]);
						setOverlayWorldCoords(oVertex[1], vertices[1]);
						setOverlayWorldCoords(oVertex[2], vertices[3]);

						{
							WorldOverlayVert wov[3];
							for (int _k = 0; _k < 3; ++_k) {
								wov[_k].wx   = oVertex[_k].x;
								wov[_k].wy   = oVertex[_k].y;
								wov[_k].wz   = oVertex[_k].z;
								wov[_k].u    = oVertex[_k].u;
								wov[_k].v    = oVertex[_k].v;
								wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
								wov[_k].argb = oVertex[_k].argb;
							}
							const DWORD overlayTexId = tex_resolve(overlayHandle);
							if (overlayTexId != 0)
								gos_PushTerrainOverlay(wov, overlayTexId);
						}
					}
				}
			}

			//---------------------------------------
			// Bottom Triangle.
			// gVertex[0] is same as above gVertex[1]
			// gVertex[1] is new and calced from vertex[2].
			// gVertex[2] is same as above.
			DWORD lightRGB2 = vertices[2]->lightRGB;
			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
				lightRGB2 = 0xffffffff;

			lightRGB2 = vertices[2]->pVertex->selected ? SELECTION_COLOR : lightRGB2;

			gVertex[0].x		= gVertex[1].x;	
			gVertex[0].y		= gVertex[1].y;	
			gVertex[0].z		= gVertex[1].z;	
			gVertex[0].rhw		= gVertex[1].rhw;	
			gVertex[0].u		= gVertex[1].u;	
			gVertex[0].v		= gVertex[1].v;	
			gVertex[0].argb		= gVertex[1].argb;
			gVertex[0].frgb		= gVertex[1].frgb;

			gVertex[1].x		= vertices[2]->px;
			gVertex[1].y		= vertices[2]->py;
			gVertex[1].z		= vertices[2]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[2]->pw;
			gVertex[1].u		= maxU;
			gVertex[1].v		= maxV;
			gVertex[1].argb		= lightRGB2;
			gVertex[1].frgb		= vertices[2]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

			const bool pzTri2 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
			if (pzTri2)
			{
				{
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[1], vertices[2], vertices[3]);
						// PatchStream append moved to appendQuad below.
					}

					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();
							
 						float oneOverTf		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTf;
		
						sVertex[1].u		= (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTf;
		
						sVertex[2].u		= (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTf ;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTf ;
		
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}
						
						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}
						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
					
					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldmaxU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldmaxV;
						oVertex[2].u		= oldminU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[1]->lightRGB;
						oVertex[1].argb		= vertices[2]->lightRGB;
						oVertex[2].argb		= vertices[3]->lightRGB;

						// GPU projection: MC2 world coords into new typed batch
						setOverlayWorldCoords(oVertex[0], vertices[1]);
						setOverlayWorldCoords(oVertex[1], vertices[2]);
						setOverlayWorldCoords(oVertex[2], vertices[3]);

						{
							WorldOverlayVert wov[3];
							for (int _k = 0; _k < 3; ++_k) {
								wov[_k].wx   = oVertex[_k].x;
								wov[_k].wy   = oVertex[_k].y;
								wov[_k].wz   = oVertex[_k].z;
								wov[_k].u    = oVertex[_k].u;
								wov[_k].v    = oVertex[_k].v;
								wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
								wov[_k].argb = oVertex[_k].argb;
							}
							const DWORD overlayTexId = tex_resolve(overlayHandle);
							if (overlayTexId != 0)
								gos_PushTerrainOverlay(wov, overlayTexId);
						}
					}
				}
			}

			// PatchStream: one bucket lookup for both triangles of this quad.
			if (terrainHandle != 0 && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
				gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
				if (!TerrainPatchStream::isThinRecordsActive()) {
					if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[3], tx1);
					if (pzTri2) buildTerrainExtraTriple(vertices[1], vertices[2], vertices[3], tx2);
				}
				TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
				// M1 compact record — BOTTOMLEFT diagonal.
				// gvTri1 = {corner0, corner1, corner3} (saved before shuffle).
				// After shuffle: gVertex[0]=corner1, gVertex[1]=corner2, gVertex[2]=corner3.
				// corner2 (vertices[2]) is only in gVertex[1] after the shuffle.
				TerrainQuadRecord rec;
				rec.wx0=vertices[0]->vx; rec.wy0=vertices[0]->vy; rec.wz0=vertices[0]->pVertex->elevation; rec._wp0=0.f;
				rec.wx1=vertices[1]->vx; rec.wy1=vertices[1]->vy; rec.wz1=vertices[1]->pVertex->elevation; rec._wp1=0.f;
				rec.wx2=vertices[2]->vx; rec.wy2=vertices[2]->vy; rec.wz2=vertices[2]->pVertex->elevation; rec._wp2=0.f;
				rec.wx3=vertices[3]->vx; rec.wy3=vertices[3]->vy; rec.wz3=vertices[3]->pVertex->elevation; rec._wp3=0.f;
				rec.nx0=vertices[0]->pVertex->vertexNormal.x; rec.ny0=vertices[0]->pVertex->vertexNormal.y; rec.nz0=vertices[0]->pVertex->vertexNormal.z; rec._np0=0.f;
				rec.nx1=vertices[1]->pVertex->vertexNormal.x; rec.ny1=vertices[1]->pVertex->vertexNormal.y; rec.nz1=vertices[1]->pVertex->vertexNormal.z; rec._np1=0.f;
				rec.nx2=vertices[2]->pVertex->vertexNormal.x; rec.ny2=vertices[2]->pVertex->vertexNormal.y; rec.nz2=vertices[2]->pVertex->vertexNormal.z; rec._np2=0.f;
				rec.nx3=vertices[3]->pVertex->vertexNormal.x; rec.ny3=vertices[3]->pVertex->vertexNormal.y; rec.nz3=vertices[3]->pVertex->vertexNormal.z; rec._np3=0.f;
				rec.minU=minU; rec.minV=minV; rec.maxU=maxU; rec.maxV=maxV;
				// BOTTOMLEFT: gvTri1[0]=corner0, gvTri1[1]=corner1, gvTri1[2]=corner3
				//             gVertex[1] after shuffle = corner2
				rec.lightRGB0=gvTri1[0].argb; rec.lightRGB1=gvTri1[1].argb; rec.lightRGB2=gVertex[1].argb; rec.lightRGB3=gvTri1[2].argb;
				rec.fogRGB0  =gvTri1[0].frgb; rec.fogRGB1  =gvTri1[1].frgb; rec.fogRGB2  =gVertex[1].frgb; rec.fogRGB3  =gvTri1[2].frgb;
				rec.terrainHandle = (uint32_t)terrainHandle;
				rec.flags = 1u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=1 → BOTTOMLEFT
				rec._ctrl2 = 0u; rec._ctrl3 = 0u;
				TerrainPatchStream::appendQuadRecord(rec);
				TerrainPatchStream::addRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				{
					TerrainQuadRecipe recipe;
					recipe.wx0=vertices[0]->vx; recipe.wy0=vertices[0]->vy; recipe.wz0=vertices[0]->pVertex->elevation; recipe._wp0=0.f;
					recipe.wx1=vertices[1]->vx; recipe.wy1=vertices[1]->vy; recipe.wz1=vertices[1]->pVertex->elevation; recipe._wp1=0.f;
					recipe.wx2=vertices[2]->vx; recipe.wy2=vertices[2]->vy; recipe.wz2=vertices[2]->pVertex->elevation; recipe._wp2=0.f;
					recipe.wx3=vertices[3]->vx; recipe.wy3=vertices[3]->vy; recipe.wz3=vertices[3]->pVertex->elevation; recipe._wp3=0.f;
					recipe.nx0=vertices[0]->pVertex->vertexNormal.x; recipe.ny0=vertices[0]->pVertex->vertexNormal.y; recipe.nz0=vertices[0]->pVertex->vertexNormal.z; recipe._np0=0.f;
					recipe.nx1=vertices[1]->pVertex->vertexNormal.x; recipe.ny1=vertices[1]->pVertex->vertexNormal.y; recipe.nz1=vertices[1]->pVertex->vertexNormal.z; recipe._np1=0.f;
					recipe.nx2=vertices[2]->pVertex->vertexNormal.x; recipe.ny2=vertices[2]->pVertex->vertexNormal.y; recipe.nz2=vertices[2]->pVertex->vertexNormal.z; recipe._np2=0.f;
					recipe.nx3=vertices[3]->pVertex->vertexNormal.x; recipe.ny3=vertices[3]->pVertex->vertexNormal.y; recipe.nz3=vertices[3]->pVertex->vertexNormal.z; recipe._np3=0.f;
					recipe.minU=minU; recipe.minV=minV; recipe.maxU=maxU; recipe.maxV=maxV;
					// BOTTOMLEFT: same corner order as fat record above.
					// gvTri1[0]=corner0, gvTri1[1]=corner1, gvTri1[2]=corner3; gVertex[1]=corner2
					const uint32_t tFlags = 1u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=1 → BOTTOMLEFT
					{
						uint32_t m0 = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
						uint32_t m1 = terrainTypeToMaterial(vertices[1]->pVertex->terrainType);
						uint32_t m2 = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
						uint32_t m3 = terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
						uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
						memcpy(&recipe._wp0, &tpacked, 4);
					}
					TerrainPatchStream::appendThinRecord(terrainHandle, recipe, tFlags,
						gvTri1[0].argb, gvTri1[1].argb, gVertex[1].argb, gvTri1[2].argb);
					TerrainPatchStream::addThinRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				}
			}
		}
	}

#ifdef _DEBUG
	if (selected )
	{
		drawLine();
		selected = FALSE;		
	}
#endif

}

extern float elevationAnimation;
#define SKY_FUDGE			(1.0f / 60000.0f)
#define WATER_ALPHA			0x1fffffff
extern float cloudScrollX;
extern float cloudScrollY;

//---------------------------------------------------------------------------
void TerrainQuad::drawWater (void)
{
	float cloudOffsetX = cos(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollX) * 0.1f;
	float cloudOffsetY = sin(360.0f * DEGREES_TO_RADS * 32.0f * cloudScrollY) * 0.1f;
	
	float sprayOffsetX = cloudScrollX * 10.0f;
	float sprayOffsetY = cloudScrollY * 10.0f;

	//Gotta be able to run the untextured maps!!!
	float oneOverWaterTF = 1.0f / 64.0f;
	float oneOverTF = 1.0f / 64.0f;

	if (Terrain::terrainTextures2)
	{
		oneOverWaterTF = (Terrain::terrainTextures2->getWaterDetailTilingFactor() / Terrain::worldUnitsMapSide);
		oneOverTF = (Terrain::terrainTextures2->getWaterTextureTilingFactor() / Terrain::worldUnitsMapSide);
	}

	if (waterHandle != 0xffffffff)
	{
		numTerrainFaces++;

		//---------------------------------------
		// GOS 3D draw Calls now!

		float minU = 0.0f;
		float maxU = 0.9999999f;

		float minV = 0.0f;
		float maxV = 0.9999999f;

		//-----------------------------------------------------
		// FOG time.  Set Render state to FOG on!
		if (useFog)
		{
			DWORD fogColor = eye->fogColor;
			//gos_SetRenderState( gos_State_Fog, (int)&fogColor);
			gos_SetRenderState( gos_State_Fog, fogColor);
		}
		else
		{
			gos_SetRenderState( gos_State_Fog, 0);
		}

		gos_VERTEX gVertex[3];
		if (uvMode == BOTTOMRIGHT)
		{
			//--------------------------
			// Top Triangle
			gVertex[0].x		= vertices[0]->wx;
			gVertex[0].y		= vertices[0]->wy;
			gVertex[0].z		= vertices[0]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->ww;
			gVertex[0].u		= minU + cloudOffsetX;
			gVertex[0].v		= minV + cloudOffsetY;
			gVertex[0].argb		= vertices[0]->pVertex->selected ? SELECTION_COLOR : vertices[0]->lightRGB;
			gVertex[0].frgb		= vertices[0]->fogRGB;
			gVertex[0].frgb		= (gVertex[0].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[0]->pVertex->terrainType);

			gVertex[1].x		= vertices[1]->wx;
			gVertex[1].y		= vertices[1]->wy;
			gVertex[1].z		= vertices[1]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->ww;
			gVertex[1].u		= maxU + cloudOffsetX;
			gVertex[1].v		= minV + cloudOffsetY;
			gVertex[1].argb		= vertices[1]->pVertex->selected ? SELECTION_COLOR : vertices[1]->lightRGB;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

			gVertex[2].x		= vertices[2]->wx;
			gVertex[2].y		= vertices[2]->wy;
			gVertex[2].z		= vertices[2]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[2]->ww;
			gVertex[2].u		= maxU + cloudOffsetX;
			gVertex[2].v		= maxV + cloudOffsetY;
			gVertex[2].argb		= vertices[2]->pVertex->selected ? SELECTION_COLOR : vertices[2]->lightRGB;
			gVertex[2].frgb		= vertices[2]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

			gVertex[0].u = (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[1].u = (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF + cloudOffsetY; 

			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{
						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
		
						if (vertices[0]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[1]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[2]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
		
						if (vertices[0]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[1]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[2]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[0]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[1]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[2]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
						
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mcTextureManager->addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mcTextureManager->addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
					}
				}
 			}
   
			//--------------------------
			//Bottom Triangle
			//
			// gVertex[0] same as above gVertex[0].
			// gVertex[1] is same as above gVertex[2].
			// gVertex[2] is calced from vertex[3].
			gVertex[1].x		= gVertex[2].x;	
			gVertex[1].y		= gVertex[2].y;	
			gVertex[1].z		= gVertex[2].z;	
			gVertex[1].rhw		= gVertex[2].rhw;
			gVertex[1].u		= gVertex[2].u;
			gVertex[1].v		= gVertex[2].v;	
			gVertex[1].argb		= gVertex[2].argb;
			gVertex[1].frgb		= gVertex[2].frgb;

			gVertex[2].x		= vertices[3]->wx;
			gVertex[2].y		= vertices[3]->wy;
			gVertex[2].z		= vertices[3]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->ww;
			gVertex[2].u		= minU + cloudOffsetX;
			gVertex[2].v		= maxV + cloudOffsetY;
			gVertex[2].argb		= vertices[3]->pVertex->selected ? SELECTION_COLOR : vertices[3]->lightRGB;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

			gVertex[0].u = (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF + cloudOffsetY;

			gVertex[1].u = (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF + cloudOffsetY;

			gVertex[2].u = (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF + cloudOffsetY;

			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{
	
						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
		
						if (vertices[0]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[2]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[3]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
		
						if (vertices[0]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[2]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[3]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[0]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[2]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[3]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
						
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mcTextureManager->addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mcTextureManager->addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
					}
				}
			}
		}
		else if (uvMode == BOTTOMLEFT)
		{
			//------------------------------
			// Top Triangle.
			gVertex[0].x		= vertices[0]->wx;
			gVertex[0].y		= vertices[0]->wy;
			gVertex[0].z		= vertices[0]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->ww;
			gVertex[0].u		= minU + cloudOffsetX;;
			gVertex[0].v		= minV + cloudOffsetY;;
			gVertex[0].argb		= vertices[0]->pVertex->selected ? SELECTION_COLOR : vertices[0]->lightRGB;
			gVertex[0].frgb		= vertices[0]->fogRGB;
			gVertex[0].frgb		= (gVertex[0].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[0]->pVertex->terrainType);

			gVertex[1].x		= vertices[1]->wx;
			gVertex[1].y		= vertices[1]->wy;
			gVertex[1].z		= vertices[1]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->ww;
			gVertex[1].u		= maxU + cloudOffsetX;;
			gVertex[1].v		= minV + cloudOffsetY;;
			gVertex[1].argb		= vertices[1]->pVertex->selected ? SELECTION_COLOR : vertices[1]->lightRGB;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

			gVertex[2].x		= vertices[3]->wx;
			gVertex[2].y		= vertices[3]->wy;
			gVertex[2].z		= vertices[3]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->ww;
			gVertex[2].u		= minU + cloudOffsetX;;
			gVertex[2].v		= maxV + cloudOffsetY;;
			gVertex[2].argb		= vertices[3]->pVertex->selected ? SELECTION_COLOR : vertices[3]->lightRGB;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

			gVertex[0].u = (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF + cloudOffsetY;
																   
			gVertex[1].u = (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF + cloudOffsetY; 

			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{

						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
		
						if (vertices[0]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[1]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[3]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
		
						if (vertices[0]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[1]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[3]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[0]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[1]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[3]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
						
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mcTextureManager->addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mcTextureManager->addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
					}
				}
			}

			//---------------------------------------
			// Bottom Triangle.
			// gVertex[0] is same as above gVertex[1]
			// gVertex[1] is new and calced from vertex[2].
			// gVertex[2] is same as above.
			gVertex[0].x		= gVertex[1].x;	
			gVertex[0].y		= gVertex[1].y;	
			gVertex[0].z		= gVertex[1].z;	
			gVertex[0].rhw		= gVertex[1].rhw;	
			gVertex[0].u		= gVertex[1].u;	
			gVertex[0].v		= gVertex[1].v;	
			gVertex[0].argb		= gVertex[1].argb;
			gVertex[0].frgb		= gVertex[1].frgb;

			gVertex[1].x		= vertices[2]->wx;
			gVertex[1].y		= vertices[2]->wy;
			gVertex[1].z		= vertices[2]->wz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[2]->ww;
			gVertex[1].u		= maxU + cloudOffsetX;;
			gVertex[1].v		= maxV + cloudOffsetY;;
			gVertex[1].argb		= vertices[2]->pVertex->selected ? SELECTION_COLOR : vertices[2]->lightRGB;
			gVertex[1].frgb		= vertices[2]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

			gVertex[0].u = (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX;
			gVertex[0].v = (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF + cloudOffsetY;
																   
			gVertex[1].u = (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[1].v = (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF + cloudOffsetY; 
																   
			gVertex[2].u = (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF + cloudOffsetX; 
			gVertex[2].v = (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF + cloudOffsetY; 
																   
			if ((gVertex[0].z >= 0.0f) &&
				(gVertex[0].z < 1.0f) &&
				(gVertex[1].z >= 0.0f) &&  
				(gVertex[1].z < 1.0f) && 
				(gVertex[2].z >= 0.0f) &&  
				(gVertex[2].z < 1.0f))
			{
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					if (waterHandle != 0xffffffff)
					{
	
						DWORD alphaMode0 = Terrain::alphaMiddle;
						DWORD alphaMode1 = Terrain::alphaMiddle;
						DWORD alphaMode2 = Terrain::alphaMiddle;
						
						if (vertices[1]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode0 = Terrain::alphaEdge;
						}
		
						if (vertices[2]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{									 
							alphaMode1 = Terrain::alphaEdge;
						}
		
						if (vertices[3]->pVertex->elevation >= (Terrain::waterElevation - MapData::alphaDepth) )
						{
							alphaMode2 = Terrain::alphaEdge;
						}
						
						if (vertices[1]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode0 = Terrain::alphaDeep;
						}
		
						if (vertices[2]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode1 = Terrain::alphaDeep;
						}
		
						if (vertices[3]->pVertex->elevation <= (Terrain::waterElevation - (MapData::alphaDepth * 3.0f)) )
						{
							alphaMode2 = Terrain::alphaDeep;
						}
		
						gVertex[0].argb = (vertices[1]->lightRGB & 0x00ffffff) + alphaMode0;
						gVertex[1].argb = (vertices[2]->lightRGB & 0x00ffffff) + alphaMode1;
						gVertex[2].argb = (vertices[3]->lightRGB & 0x00ffffff) + alphaMode2;
		
						if ((gVertex[0].u > MaxMinUV) || 
							(gVertex[0].v > MaxMinUV) ||
							(gVertex[1].u > MaxMinUV) || 
							(gVertex[1].v > MaxMinUV) ||
							(gVertex[2].u > MaxMinUV) || 
							(gVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(gVertex[0].u,fmax(gVertex[1].u,gVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(gVertex[0].v,fmax(gVertex[1].v,gVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							gVertex[0].u -= maxU;
							gVertex[1].u -= maxU;
							gVertex[2].u -= maxU;
							
							gVertex[0].v -= maxV;
							gVertex[1].v -= maxV;
							gVertex[2].v -= maxV;
						}
		 
						if (alphaMode0 + alphaMode1 + alphaMode2)
						{
							mcTextureManager->addVertices(waterHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATER);
						}
					}
						
					//----------------------------------------------------
					// Draw the sky reflection on the water.
					if (useWaterInterestTexture && (waterDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						sVertex[0].u		= ((vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[0].v		= ((Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[1].u		= ((vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[1].v		= ((Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[2].u		= ((vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverWaterTF) + sprayOffsetX;
						sVertex[2].v		= ((Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverWaterTF) + sprayOffsetY;
		
						sVertex[0].argb = (sVertex[0].argb & 0xff000000) + 0xffffff;
						sVertex[1].argb = (sVertex[1].argb & 0xff000000) + 0xffffff; 
						sVertex[2].argb = (sVertex[2].argb & 0xff000000) + 0xffffff; 

						mcTextureManager->addVertices(waterDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISWATERDETAIL);
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------
long DrawDebugCells = 0;

void TerrainQuad::drawLine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw the Tile block lines at depth just above base tiles.
	long color = XP_WHITE;

	if (uvMode == BOTTOMRIGHT)
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,vertices[0]->pz-0.002f,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,vertices[1]->pz-0.002f,1.0f / vertices[1]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;

			pos2.x = vertices[2]->px;
			pos2.y = vertices[2]->py;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;
			pos1.z = vertices[2]->pz - 0.002f;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;
			pos2.z = vertices[0]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,vertices[0]->pz-0.002f,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,vertices[2]->pz-0.002f,1.0f / vertices[2]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;
			pos1.z = vertices[2]->pz - 0.002f;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;
			pos2.z = vertices[3]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;
			pos1.z = vertices[3]->pz - 0.002f;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;
			pos2.z = vertices[0]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
		}
	}
	else
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,vertices[0]->pz-0.002f,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,vertices[1]->pz-0.002f,1.0f / vertices[1]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;
			pos1.z = vertices[1]->pz - 0.002f;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;
			pos2.z = vertices[3]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;
			pos1.z = vertices[3]->pz - 0.002f;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;
			pos2.z = vertices[0]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[1]->px,vertices[1]->py,vertices[1]->pz-0.002f,1.0f / vertices[1]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,vertices[2]->pz-0.002f,1.0f / vertices[2]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;
			pos1.z = vertices[2]->pz - 0.002f;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;
			pos2.z = vertices[3]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;
			pos1.z = vertices[3]->pz - 0.002f;

			pos2.x = vertices[1]->px;
			pos2.y = vertices[1]->py;
			pos2.z = vertices[1]->pz - 0.002f;

			{
				LineElement newElement(pos1,pos2,color,NULL);
				newElement.draw();
			}
		}
	}

	//------------------------------------------------------------
	// Draw Movement Map Grid.
	//		Once movement is split up, turn this back on for editor -fs
	// I need ALL cells drawn to check elevation Code
	if (clipped1 != 0)
	{
		//--------------------------------------------------------------------
		// Display the ScenarioMap cell grid, as well, displaying open\blocked
		// states...
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		//cellWidth -= 5.0;
		
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
				
		if (GameMap)
		{
			for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
			{
				for (long cellC = 0; cellC < MAPCELL_DIM; cellC++) 
				{
					long actualCellRow = tileR * MAPCELL_DIM + cellR;
					long actualCellCol = tileC * MAPCELL_DIM + cellC;
					
					MapCellPtr curCell = NULL;
					if (GameMap->inBounds(actualCellRow, actualCellCol))
						curCell = GameMap->getCell(actualCellRow, actualCellCol);
					
 					if (!curCell || 
						curCell->getDebug() || 
						!curCell->getPassable() || 
						curCell->getPathlock(0) || 
						curCell->getDeepWater() || 
						curCell->getShallowWater() ||
						curCell->getForest())
					{
						Stuff::Vector4D pos1;
						Stuff::Vector4D pos2;
						Stuff::Vector4D pos3;
						Stuff::Vector4D pos4;
						
						Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);
						
						thePoint.x += (cellC) * cellWidth;
						thePoint.y -= (cellR) * cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_passability_0]
						eye->projectForDebugOverlay(thePoint,pos4);

						thePoint.x += cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_passability_1]
						eye->projectForDebugOverlay(thePoint,pos1);

						thePoint.y -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_passability_2]
						eye->projectForDebugOverlay(thePoint,pos2);

						thePoint.x -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_passability_3]
						eye->projectForDebugOverlay(thePoint,pos3);

						pos1.z -= 0.002f;
						pos2.z -= 0.002f;
						pos3.z -= 0.002f;
						pos4.z -= 0.002f;

						DWORD color = XP_RED;
						
						if (!curCell)
						{
							color = XP_GREEN;
						}
						else if (curCell->getDebug() && DrawDebugCells)
						{
							color = XP_YELLOW;
						}
						else if (curCell->getPathlock(0))
						{
							color = XP_YELLOW;
						}
						else if (curCell->getForest())
						{
							color = SB_ORANGE;
						}
						else if (!curCell->getPassable())
						{
							color = SB_RED;
						}
						else if (curCell->getDeepWater())
						{
							color = SB_ORANGE;
						}
						else if (curCell->getShallowWater())
						{
							color = XP_BLUE;
						}

						{
							LineElement newElement(pos1,pos2,color,NULL);
							newElement.draw();
						}	
						
						{
							LineElement newElement(pos2,pos3,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos3,pos4,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos1,pos4,color,NULL);
							newElement.draw();
						}
					}
				}
			}
		}
	}

	if (GlobalMoveMap[0]->badLoad)
		return;

	if (clipped1 != 0)
	{
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		cellWidth -= 5.0;
				
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
		
		long cellR = tileR * MAPCELL_DIM;
		long cellC = tileC * MAPCELL_DIM;
		
		for (long currentDoor = 0;currentDoor < GlobalMoveMap[0]->numDoors;currentDoor++)
		{
			if ((GlobalMoveMap[0]->doors[currentDoor].row >= cellR) && 
				(GlobalMoveMap[0]->doors[currentDoor].row < (cellR + MAPCELL_DIM)) &&
				(GlobalMoveMap[0]->doors[currentDoor].col >= cellC) &&
				(GlobalMoveMap[0]->doors[currentDoor].col < (cellC + MAPCELL_DIM)))
			{
				Stuff::Vector4D pos1;
				Stuff::Vector4D pos2;
				Stuff::Vector4D pos3;
				Stuff::Vector4D pos4;
					
				long xLength = 1;
				long yLength = 1;
				
				if (GlobalMoveMap[0]->doors[currentDoor].direction[0] == 1)
				{
					yLength = GlobalMoveMap[0]->doors[currentDoor].length;
				}
				
				if (GlobalMoveMap[0]->doors[currentDoor].direction[0] == 2)
				{
					xLength = GlobalMoveMap[0]->doors[currentDoor].length;
				}

				Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);
				
				thePoint.x += (GlobalMoveMap[0]->doors[currentDoor].col - cellC) * cellWidth;
				thePoint.y -= (GlobalMoveMap[0]->doors[currentDoor].row - cellR) * cellWidth;

				thePoint.z = land->getTerrainElevation(thePoint);
				// [PROJECTZ:DebugOnly id=debug_door_outline_0]
				eye->projectForDebugOverlay(thePoint,pos4);

				thePoint.x += (xLength) * cellWidth;
				thePoint.z = land->getTerrainElevation(thePoint);
				// [PROJECTZ:DebugOnly id=debug_door_outline_1]
				eye->projectForDebugOverlay(thePoint,pos1);

				thePoint.y -= (yLength) * cellWidth;
				thePoint.z = land->getTerrainElevation(thePoint);
				// [PROJECTZ:DebugOnly id=debug_door_outline_2]
				eye->projectForDebugOverlay(thePoint,pos2);

				thePoint.x -= (xLength) * cellWidth;
				thePoint.z = land->getTerrainElevation(thePoint);
				// [PROJECTZ:DebugOnly id=debug_door_outline_3]
				eye->projectForDebugOverlay(thePoint,pos3);

				pos1.z -= 0.002f;
				pos2.z -= 0.002f;
				pos3.z -= 0.002f;
				pos4.z -= 0.002f;
				{
					LineElement newElement(pos1,pos2,XP_GREEN,NULL);
					newElement.draw();
				}	
				
				{
					LineElement newElement(pos2,pos3,XP_GREEN,NULL);
					newElement.draw();
				}
				
				{
					LineElement newElement(pos3,pos4,XP_GREEN,NULL);
					newElement.draw();
				}
				
				{
					LineElement newElement(pos1,pos4,XP_GREEN,NULL);
					newElement.draw();
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------------------------
void TerrainQuad::drawLOSLine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw the Tile block lines at depth just above base tiles.

	//------------------------------------------------------------
	// Draw LOS Map Grid.
	// Draw a color for LOS cell height data based on height.  Draw NOTHING if cell height is ZERO!!
	if (clipped1 != 0)
	{
		//--------------------------------------------------------------------
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
				
		if (GameMap)
		{
			for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
			{
				for (long cellC = 0; cellC < MAPCELL_DIM; cellC++) 
				{
					long actualCellRow = tileR * MAPCELL_DIM + cellR;
					long actualCellCol = tileC * MAPCELL_DIM + cellC;
					
					MapCellPtr curCell = NULL;
					if (GameMap->inBounds(actualCellRow, actualCellCol))
						curCell = GameMap->getCell(actualCellRow, actualCellCol);
					
 					if (curCell && curCell->getLocalHeight())
					{
						Stuff::Vector4D pos1;
						Stuff::Vector4D pos2;
						Stuff::Vector4D pos3;
						Stuff::Vector4D pos4;

						Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);

						thePoint.x += (cellC) * cellWidth;
						thePoint.y -= (cellR) * cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_los_cell_height_0]
						eye->projectForDebugOverlay(thePoint,pos4);

						thePoint.x += cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_los_cell_height_1]
						eye->projectForDebugOverlay(thePoint,pos1);

						thePoint.y -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_los_cell_height_2]
						eye->projectForDebugOverlay(thePoint,pos2);

						thePoint.x -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_los_cell_height_3]
						eye->projectForDebugOverlay(thePoint,pos3);
						
						pos1.z -= 0.002f;
						pos2.z -= 0.002f;
						pos3.z -= 0.002f;
						pos4.z -= 0.002f;

						DWORD color = XP_BLACK;
						
						if (curCell->getLocalHeight() < 2)
						{
							color = XP_BLACK;
						}
						else if (curCell->getLocalHeight() < 4)
						{
							color = XP_GRAY;
						}
						else if (curCell->getLocalHeight() < 6)
						{
							color = XP_RED;
						}
						else if (curCell->getLocalHeight() < 8)
						{
							color = XP_ORANGE;
						}
						else if (curCell->getLocalHeight() < 10)
						{
							color = XP_YELLOW;
						}
						else if (curCell->getLocalHeight() < 12)
						{
							color = XP_GREEN;
						}
						else if (curCell->getLocalHeight() < 14)
						{
							color = XP_BLUE;
						}
						else if (curCell->getLocalHeight() <= 16)
						{
							color = XP_WHITE;
						}

						{
							LineElement newElement(pos1,pos2,color,NULL);
							newElement.draw();
						}	
						
						{
							LineElement newElement(pos2,pos3,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos3,pos4,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos1,pos4,color,NULL);
							newElement.draw();
						}
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------
void TerrainQuad::drawDebugCellLine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw the Tile block lines at depth just above base tiles.

	if (uvMode == BOTTOMRIGHT)
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,HUD_DEPTH,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,HUD_DEPTH,1.0f / vertices[1]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;

			pos2.x = vertices[2]->px;
			pos2.y = vertices[2]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,HUD_DEPTH,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,HUD_DEPTH,1.0f / vertices[2]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
	}
	else
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,HUD_DEPTH,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,HUD_DEPTH,1.0f / vertices[1]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[1]->px,vertices[1]->py,HUD_DEPTH,1.0f / vertices[1]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,HUD_DEPTH,1.0f / vertices[2]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;

			pos2.x = vertices[1]->px;
			pos2.y = vertices[1]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
	}

	//------------------------------------------------------------
	// Draw Movement Map Grid.
	//		Once movement is split up, turn this back on for editor -fs
	// I need ALL cells drawn to check elevation Code
	if (clipped1 != 0)
	{
		//--------------------------------------------------------------------
		// Display the ScenarioMap cell grid, as well, displaying open\blocked
		// states...
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		//cellWidth -= 5.0;
		
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
				
		if (GameMap)
		{
			for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
			{
				for (long cellC = 0; cellC < MAPCELL_DIM; cellC++) 
				{
					long actualCellRow = tileR * MAPCELL_DIM + cellR;
					long actualCellCol = tileC * MAPCELL_DIM + cellC;
					
					MapCellPtr curCell = NULL;
					if (GameMap->inBounds(actualCellRow, actualCellCol))
						curCell = GameMap->getCell(actualCellRow, actualCellCol);
					
 					if (!curCell || curCell->getDebug())
					{
						Stuff::Vector4D pos1;
						Stuff::Vector4D pos2;
						Stuff::Vector4D pos3;
						Stuff::Vector4D pos4;

						Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);

						thePoint.x += (cellC) * cellWidth;
						thePoint.y -= (cellR) * cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_state_0]
						eye->projectForDebugOverlay(thePoint,pos4);

						thePoint.x += cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_state_1]
						eye->projectForDebugOverlay(thePoint,pos1);

						thePoint.y -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_state_2]
						eye->projectForDebugOverlay(thePoint,pos2);

						thePoint.x -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_state_3]
						eye->projectForDebugOverlay(thePoint,pos3);

						pos1.z = pos2.z = pos3.z = pos4.z = HUD_DEPTH;

						DWORD color = XP_RED;
						if (!curCell)
						{
							color = XP_GREEN;
						}
						else {
							static DWORD debugColors[4] = {0, XP_RED, XP_WHITE, XP_BLUE};
							DWORD cellDebugValue = curCell->getDebug();
							if (cellDebugValue)
								color = debugColors[cellDebugValue];
						}
/*						else if (curCell->getPathlock())
						{
								color = XP_YELLOW;
						}
						else if (!curCell->getLineOfSight())
						{
					 		color = XP_BLUE;
						}
						else if (!curCell->getPassable())
						{
							color = SB_RED;
						}
*/
						{
							LineElement newElement(pos1,pos2,color,NULL);
							newElement.draw();
						}	
						
						{
							LineElement newElement(pos2,pos3,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos3,pos4,color,NULL);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos1,pos4,color,NULL);
							newElement.draw();
						}
					}
				}
			}
		}
	}


}

//---------------------------------------------------------------------------
void TerrainQuad::drawMine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw Mines.
	// All mines are visible all the time!
	if ((clipped1 != 0) || (clipped2 != 0))
	{
		long cellPos = 0;
		float cellWidth = Terrain::worldUnitsPerCell;
		for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
		{
			for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
			{
				//--------------------------------------------------------------------
				bool drawMine = false;
				bool drawBlownMine = false;
				
				if (mineResult.getMine(cellPos) == 1)
					drawMine = true;
								
				if (mineResult.getMine(cellPos) == 2)
					drawBlownMine = true;
								
				if (drawMine || drawBlownMine)
				{
					Stuff::Vector4D pos1;
					Stuff::Vector4D pos2;
					Stuff::Vector4D pos3;
					Stuff::Vector4D pos4;

					//------------------------------------------------------------------------------------
					// Dig the actual Vertex information out of the projected vertices already done.
					// In this way, the draw requires only interpolation and not Giant Matrix multiplies.
					Stuff::Vector3D thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);

					thePoint.x += (cellC) * cellWidth;
					thePoint.y -= (cellR) * cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					// [PROJECTZ:ScreenXYOracle id=mine_cell_corner0]
					eye->projectForScreenXY(thePoint,pos4);

					thePoint.x += cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					// [PROJECTZ:ScreenXYOracle id=mine_cell_corner1]
					eye->projectForScreenXY(thePoint,pos1);
					
					thePoint.y -= cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					// [PROJECTZ:ScreenXYOracle id=mine_cell_corner2]
					eye->projectForScreenXY(thePoint,pos2);

					thePoint.x -= cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					// [PROJECTZ:ScreenXYOracle id=mine_cell_corner3]
					eye->projectForScreenXY(thePoint,pos3);

					//------------------------------------
					// Replace with New RIA code
					gos_VERTEX gVertex[3];
					gos_VERTEX sVertex[3];
		
					gVertex[0].x        = sVertex[0].x		= pos1.x;
					gVertex[0].y        = sVertex[0].y		= pos1.y;
					gVertex[0].z        = sVertex[0].z		= pos1.z;
					gVertex[0].rhw      = sVertex[0].rhw	= pos1.w;
					gVertex[0].u        = sVertex[0].u		= 0.0f;
					gVertex[0].v        = sVertex[0].v		= 0.0f;
					gVertex[0].argb     = sVertex[0].argb	= vertices[0]->lightRGB;
					gVertex[0].frgb     = sVertex[0].frgb	= vertices[0]->fogRGB;
					{
						BYTE matIdx = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
						gVertex[0].frgb = (gVertex[0].frgb & 0xFFFFFF00) | matIdx;
						sVertex[0].frgb = (sVertex[0].frgb & 0xFFFFFF00) | matIdx;
					}

					gVertex[1].x		= pos2.x;
					gVertex[1].y		= pos2.y;
					gVertex[1].z		= pos2.z;
					gVertex[1].rhw		= pos2.w;
					gVertex[1].u		= 0.999999999f;
					gVertex[1].v		= 0.0f;
					gVertex[1].argb		= vertices[1]->lightRGB;
					gVertex[1].frgb		= vertices[1]->fogRGB;
					gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

					gVertex[2].x        = sVertex[1].x		= pos3.x;
					gVertex[2].y        = sVertex[1].y		= pos3.y;
					gVertex[2].z        = sVertex[1].z		= pos3.z;
					gVertex[2].rhw      = sVertex[1].rhw	= pos3.w;
					gVertex[2].u        = sVertex[1].u		= 0.999999999f;
					gVertex[2].v        = sVertex[1].v		= 0.999999999f;
					gVertex[2].argb     = sVertex[1].argb	= vertices[2]->lightRGB;
					gVertex[2].frgb     = sVertex[1].frgb	= vertices[2]->fogRGB;
					{
						BYTE matIdx = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
						gVertex[2].frgb = (gVertex[2].frgb & 0xFFFFFF00) | matIdx;
						sVertex[1].frgb = (sVertex[1].frgb & 0xFFFFFF00) | matIdx;
					}

					sVertex[2].x		= pos4.x;
					sVertex[2].y		= pos4.y;
					sVertex[2].z		= pos4.z;
					sVertex[2].rhw		= pos4.w;
					sVertex[2].u		= 0.0f;
					sVertex[2].v		= 0.999999999f;
					sVertex[2].argb		= vertices[3]->lightRGB;
					sVertex[2].frgb		= vertices[3]->fogRGB;
					sVertex[2].frgb		= (sVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
		
					if ((gVertex[0].z >= 0.0f) &&
						(gVertex[0].z < 1.0f) &&
						(gVertex[1].z >= 0.0f) &&  
						(gVertex[1].z < 1.0f) && 
						(gVertex[2].z >= 0.0f) &&  
						(gVertex[2].z < 1.0f) &&
						(sVertex[0].z >= 0.0f) &&
						(sVertex[0].z < 1.0f) &&
						(sVertex[1].z >= 0.0f) &&  
						(sVertex[1].z < 1.0f) && 
						(sVertex[2].z >= 0.0f) &&  
						(sVertex[2].z < 1.0f))
					{
						if (drawBlownMine)
						{
							mcTextureManager->addVertices(blownTextureHandle,gVertex,MC2_DRAWALPHA);
							mcTextureManager->addVertices(blownTextureHandle,sVertex,MC2_DRAWALPHA);
						}
						else
						{
							mcTextureManager->addVertices(mineTextureHandle,gVertex,MC2_DRAWALPHA);
							mcTextureManager->addVertices(mineTextureHandle,sVertex,MC2_DRAWALPHA);
						}
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------

