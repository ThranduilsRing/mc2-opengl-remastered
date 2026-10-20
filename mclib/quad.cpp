#include "Gates/preferences_legacy.h"
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

#include "quad_internal.h"
#include "texture_assets.h"


// [DEPTH_TRANSITION v1] cross-TU CPU-water REAL screen-z sample (env-gated
// MC2_DEPTH_TRANSITION_PROBE; silent default). Written by TerrainQuad::drawWater
// for the water vertex nearest the fixed mid-map probe point P; READ by the
// transition dump in gos_terrain_indirect.cpp. g_cpuWaterProbeStamp is bumped
// once per CPU-water frame so the dump can detect a STALE value (CPU water and
// the GPU water fast path are mutually exclusive per frame -- on an armed
// transition frame this is the last un-armed steady sample, NOT this-frame).
float    g_cpuWaterProbeZ        = 0.0f;
double   g_cpuWaterProbeBestD2   = 0.0;
bool     g_cpuWaterProbeAny      = false;
unsigned long long g_cpuWaterProbeStamp = 0;
constexpr unsigned int HIGHLIGHT_COLOR = 0xff00ff00;

//---------------------------------------------------------------------------
float TerrainQuad::rainLightLevel = 1.0f;
DWORD TerrainQuad::lighteningLevel = 0;
DWORD TerrainQuad::mineTextureHandle = 0xffffffff;
DWORD TerrainQuad::blownTextureHandle = 0xffffffff;

void enqueueTerrainMineState(TerrainQuad& quad)
{
	// PR2c Stage 0c — bracket the entire function for the dedicated mine bucket.
	// Counter increments once per call (per-quad). User-attested 2026-05-08
	// baseline ~157 µs/frame across all per-frame calls; the cost-split timer
	// confirms.
	CostSplitMineEnqueueScope _csMineEnq;
	gos_terrain_indirect::Counters_AddLegacyMineEnqueueQuad();

	long rowCol = quad.vertices[0]->posTile;
	long tileR = rowCol>>16;
	long tileC = rowCol & 0x0000ffff;

	if (GameMap)
	{
		quad.mineResult.init();

		// Fast skip for the ~99% of quads with no mines/scorch in any cell.
		// GameMap maintains tileMineCount incrementally on setMine() and rebuilds
		// after packet readPacket(); pre-rebuild it returns true conservatively.
		if (!GameMap->tileHasMines(tileR, tileC))
			return;

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
					tex_resolve(TerrainQuad::mineTextureHandle);
					{ CostSplitDetailOverlayScope _csDetail;
					mc2::renderSubmissions().addTriangleBulk(TerrainQuad::mineTextureHandle, MC2_DRAWALPHA, 2);
					}
					quad.mineResult.setMine(cellPos,localResult);
				}
				else if (localResult == 2)
				{
					tex_resolve(TerrainQuad::blownTextureHandle);
					{ CostSplitDetailOverlayScope _csDetail;
					mc2::renderSubmissions().addTriangleBulk(TerrainQuad::blownTextureHandle, MC2_DRAWALPHA, 2);
					}
					quad.mineResult.setMine(cellPos,localResult);
				}
			}
		}
	}
}

// Terrain LOD chunk Phase 7A — standalone mine enqueue from visible quadList.
// Called from Terrain::render() when MC2_TERRAIN_LOD_CHUNK=1 is active,
// replacing the mine enqueue that was embedded in setupTextures().
// Iterates the camera-relative quadList (same population as the minePass
// drawMine loop) and runs enqueueTerrainMineState on every quad.
// The setupTextures mine-enqueue call is suppressed by a parallel guard in
// setupTextures() when MC2_TERRAIN_LOD_CHUNK=1, so no double-enqueue occurs.
// Callers must gate on !IsFrameMineArmed() — the GPU static-bake path owns
// mine state when armed and this function must not run.
void TerrainQuad::enqueueMinesFromGrid (TerrainQuadPtr quadList, long count)
{
	TerrainQuadPtr q = quadList;
	for (long i = 0; i < count; ++i, ++q)
		enqueueTerrainMineState(*q);
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
#define HUD_DEPTH		0.9999f			//HUD Objects draw over everything else (reverse-Z: near=1, far=0).
// NS3 boundary: BaseVertexColor canonical def (was code/ leak; consumers extern it).
DWORD BaseVertexColor = 0x00000000;
bool useOverlayTexture = true;
long numTerrainFaces = 0;

void TerrainQuad::initMineTextureHandles (void)
{
	// A5 / R7: ensure the mine + scorch texture handles are loaded at mission-prime
	// time so they are valid even when TerrainQuad::setupTextures never runs (Phase 8z
	// removes setupTextures; its inline lazy-load was the ONLY loader, so deleting it
	// would leave the handles at 0xffffffff -> BuildMineTextureArray bails -> mine
	// tiles render black forever). The 0xffffffff guards make this idempotent.
	if (mineTextureHandle == 0xffffffff)
	{
		FullPathFileName mineTextureName;
		mineTextureName.init(texturePath,"defaults" PATH_SEPARATOR "mine_00",".tga");
		mineTextureHandle = mc2::textures::load(mineTextureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
	}

	if (blownTextureHandle == 0xffffffff)
	{
		FullPathFileName mineTextureName;
		mineTextureName.init(texturePath,"defaults" PATH_SEPARATOR "minescorch_00",".tga");
		blownTextureHandle = mc2::textures::load(mineTextureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
	}
}
