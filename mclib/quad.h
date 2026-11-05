//---------------------------------------------------------------------------
//
// Quad.h -- File contains class definitions for the Terrain Quads
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef QUAD_H
#define QUAD_H
//---------------------------------------------------------------------------
// Include Files
#ifndef DQUAD_H
#include"dquad.h"
#endif

#ifndef DVERTEX_H
#include"dvertex.h"
#endif

#ifndef DSTD_H
#include"dstd.h"
#endif

//---------------------------------------------------------------------------
struct _TerrainUVData
{
	float	minU;
	float	minV;
	float	maxU;
	float 	maxV;
}; using TerrainUVData = _TerrainUVData;

struct _MineResult
{
	DWORD mineData;
	
	void init (void)
	{
		mineData = 0;
	}
	
	void setMine (DWORD pos, unsigned char data)
	{
		DWORD result = (data << (pos << 1));
		mineData |= result;
	}
	
	DWORD getMine(DWORD pos)
	{
		DWORD result = (mineData >> (pos << 1)) & (0x3);
		return result;
	}
	
}; using MineResult = _MineResult;

// Phase-helper state of TerrainQuad::draw (defined in quad_draw.cpp).
struct QuadDrawFrame;
struct QuadFastFrame;

class TerrainQuad
{
	//Data Members
	//-------------
	public:

		VertexPtr			vertices[4];			//Pointers to vertices defining this tile.

		DWORD				terrainHandle;			//Handle to texture to draw.
		DWORD				terrainDetailHandle;	//Handle to detail texture to draw.
		DWORD				waterHandle;			//Handle to water texture to draw.
		DWORD				waterDetailHandle;		//Handle to Water Detail texture to draw.
		DWORD				overlayHandle;			//Handle to overlay texture to draw.

		DWORD				uvMode;					//Is this a top or bottom triangle?
		MineResult			mineResult;				//Is there a mine or exploded mine in this cell on this tile?
			
		TerrainUVData		uvData;					//Stores the min and max UVs for this face.
			
		bool				isCement;				//Need to know if this tile is cement for a number of reasons
	
		static float		rainLightLevel;			//How much to darken terrain based on rain
		static DWORD		lighteningLevel;		//How much to lighten terrain based on lightening.
		static DWORD 		mineTextureHandle;		//Handle to the mine textures.
		static DWORD 		blownTextureHandle;

		
#ifdef _DEBUG
		bool				selected;			//Debug to show which triangle I am on.
#endif

	//Member Functions
	//-----------------
	public:

		void init (void)
		{
			for (auto & vertex : vertices)
				vertex = nullptr;
			
			terrainHandle = 0xffffffff;
			terrainDetailHandle = 0xffffffff;
			waterHandle = 0xffffffff;
			waterDetailHandle = 0xffffffff;
			overlayHandle = 0xffffffff;

			uvMode = 0;
			
			uvData.maxU = uvData.minU = uvData.minV = uvData.maxV = 0.0f;
			
			mineResult.init();
			
			isCement = false;
		}

		TerrainQuad (void)
		{
			init();
		}

		void destroy (void)
		{
		}

		~TerrainQuad (void)
		{
			destroy();
		}

		long init (VertexPtr v0, VertexPtr v1, VertexPtr v2, VertexPtr v3);
		void setupTextures (void);

		void draw (void);
		void drawMine (void);
		void drawLine (void);
		void drawDebugCellLine (void);
		void drawLOSLine (void);
		void drawWater (void);

	// TerrainQuad::draw phases (quad_draw.cpp); callers use draw().
	private:
		void drawSetup(QuadDrawFrame& f);
		void drawPathSelect(QuadDrawFrame& f);
		void fastPzGate(QuadFastFrame& fp);
		void fastOverlayEmit(QuadDrawFrame& f, QuadFastFrame& fp);
		void drawLegacyBottomRight(QuadDrawFrame& f);
		void drawLegacyBottomLeft(QuadDrawFrame& f);
	public:

		// Terrain LOD chunk Phase 7A — mine enqueue from visible tile grid.
		// Replaces the per-quad mine enqueue that was embedded in setupTextures().
		// Called from Terrain::render() when MC2_TERRAIN_LOD_CHUNK=1 and
		// !IsFrameMineArmed(). Writes quad.mineResult and reserves addTriangleBulk
		// slots for all mine-bearing tiles in the visible quadList.
		static void enqueueMinesFromGrid (TerrainQuadPtr quadList, long count);

		// A5 / R7 fix: load the mine + scorch texture handles (mineTextureHandle /
		// blownTextureHandle) at mission-prime time, independent of setupTextures()
		// which Phase 8z removes. Idempotent (0xffffffff-guarded). Called once from
		// Terrain::primeMissionTerrainCache.
		static void initMineTextureHandles (void);
};

//---------------------------------------------------------------------------
#endif
