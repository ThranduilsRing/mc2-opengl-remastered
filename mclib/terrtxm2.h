//---------------------------------------------------------------------------
//
// TerrTxm2.h -- File contains class definitions for the Terrain Textures
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef TERRTXM2_H
#define TERRTXM2_H
//---------------------------------------------------------------------------
// Include Files
#ifndef QUAD_H
#include"quad.h"
#endif

#ifndef TXMMGR_H
#include"txmmgr.h"
#endif
#include"tex_resolve_table.h"

#ifndef INIFILE_H
#include"inifile.h"
#endif

//---------------------------------------------------------------------------
// Macro Definitions
#ifndef NO_ERR
#define NO_ERR		0
#endif

#define MAX_WATER_DETAIL_TEXTURES	256

#define TOTAL_COLORMAP_TYPES		5
//---------------------------------------------------------------------------
// Class Definitions
typedef struct _ColorMapTextures
{
	DWORD 		mcTextureNodeIndex;
} ColorMapTextures;

typedef struct _ColorMapRAM
{
	MemoryPtr 	ourRAM;
} ColorMapRAM;

class TerrainColorMap
{
	//Data Members
	//-------------
	protected:
		MemoryPtr				ColorMap;
		
		DWORD					numTextures;
		
		float					numTexturesAcross;
		float					fractionPerTexture;
		
		ColorMapTextures		*textures;
		ColorMapRAM				*txmRAM;
		
		UserHeapPtr				colorMapHeap;
		UserHeapPtr				colorMapRAMHeap;
		
		MemoryPtr				detailTextureRAM;
		DWORD					detailTextureNodeIndex;
		float					detailTextureTilingFactor;
		
		MemoryPtr				waterTextureRAM;
		DWORD					waterTextureNodeIndex;
		float					waterTextureTilingFactor;
		
		DWORD					numWaterDetailFrames;
		DWORD					waterDetailNodeIndex[MAX_WATER_DETAIL_TEXTURES];
		float					waterDetailFrameRate;
		float					waterDetailTilingFactor;

		// Normal map for terrain lighting
		ColorMapTextures		*normalMapTextures;
		DWORD					numNormalMapTextures;
		DWORD					detailNormalNodeIndex;
		bool					hasNormalMap;
		long					lastResultTexture; // tile index from last getTextureHandle call

		static DWORD			terrainTypeIDs[ TOTAL_COLORMAP_TYPES ];

	public:
		// CPU-side displacement data for terrain elevation correction
		unsigned char*  cpuDispAlpha;       // matNormal2 alpha channel (dirt displacement)
		int             cpuDispAlphaSize;   // width=height of the square texture
		unsigned char*  cpuColorMap;        // full colormap RGBA for HSV classification
		int             cpuColorMapSize;    // width=height of the full colormap
	
		bool					colorMapStarted;
		
		float					hGauss;
		float					roughDistance;

	//Member Functions
	//-----------------
	protected:

	public:
	
		void init (void);

		TerrainColorMap (void)
		{
			init();
		}

		void destroy (void);

		~TerrainColorMap (void)
		{
			destroy();
		}

		long init (char *fileName);

		void getColorMapData (MemoryPtr ourRAM, long index, long width);
				
		DWORD resolveTextureHandle (VertexPtr vMin, VertexPtr vMax, TerrainUVData *uvData, long* resultTexture, bool realizeTexture);
		DWORD getTextureHandle (VertexPtr vMin, VertexPtr vMax, TerrainUVData *uvData);

		DWORD peekDetailHandle (void) const
		{
			return detailTextureNodeIndex;
		}

		DWORD getNormalMapHandle (long resultTexture) {
			if (hasNormalMap && normalMapTextures && resultTexture >= 0 && resultTexture < (long)numNormalMapTextures) {
				tex_resolve(normalMapTextures[resultTexture].mcTextureNodeIndex);
				return normalMapTextures[resultTexture].mcTextureNodeIndex;
			}
			return 0xffffffff;
		}

		DWORD getDetailNormalHandle (void) {
			if (detailNormalNodeIndex != 0xffffffff)
				tex_resolve(detailNormalNodeIndex);
			return detailNormalNodeIndex;
		}

		bool getHasNormalMap (void) { return hasNormalMap; }

		DWORD getDetailHandle (void)
		{
			tex_resolve(detailTextureNodeIndex);
			return (detailTextureNodeIndex);
		}
		long saveDetailTexture(const char *fileName);

		DWORD getWaterTextureHandle (void)
		{
			tex_resolve(waterTextureNodeIndex);
			return waterTextureNodeIndex;
		}
		long saveWaterTexture(const char *fileName);

		DWORD getWaterDetailHandle (long frameNum)
		{
			if ((frameNum >= 0) && (frameNum < (long)numWaterDetailFrames))
			{
				tex_resolve(waterDetailNodeIndex[frameNum]);
				return waterDetailNodeIndex[frameNum];
			}
			else
				return 0xffffffff;
		}
		long saveWaterDetail(const char *fileName);

		DWORD getWaterDetailNumFrames (void)
		{
			return numWaterDetailFrames;
		}
		
		float getWaterDetailFrameRate (void)
		{
			return waterDetailFrameRate;
		}

		float getDetailTilingFactor (void)
		{
			return detailTextureTilingFactor;
		}

		float getWaterTextureTilingFactor(void)
		{
			return waterTextureTilingFactor;
		}

		float getWaterDetailTilingFactor(void)
		{
			return waterDetailTilingFactor;
		}
		
		void setWaterDetailFrameRate (float frameRate)
		{
			waterDetailFrameRate = frameRate;
		}

		void setDetailTilingFactor (float tf)
		{
			detailTextureTilingFactor = tf;
		}

		void setWaterTextureTilingFactor (float tf)
		{
			waterTextureTilingFactor = tf;
		}

		void setWaterDetailTilingFactor (float tf)
		{
			waterDetailTilingFactor = tf;
		}

		long saveTilingFactors(FitIniFile *fitFile);
		
 		//Mike, these functions will reload these textures from disk.
		// This allows us to change them in the editor and reload here.
		// Pass in the filename of the mission!!!!
		void resetBaseTexture (const char *fileName);
		void resetDetailTexture (const char *fileName);
		void resetWaterTexture (const char *fileName);
		void resetWaterDetailTextures (const char *fileName);
		
		//Pass in filename of height map to write new data to.
		void refractalizeBaseMesh (const char *fileName, long Threshold, long Noise);
		
		void burnInShadows (bool doBumpPass = true, const char * fileName = NULL);
		
		void recalcLight(const char *fileName);

		static long getNumTypes (void)
		{
			return TOTAL_COLORMAP_TYPES;
		}

		static long getTextureNameID (long idNum)
		{
			if ((idNum >= 0) && (idNum < TOTAL_COLORMAP_TYPES))
				return terrainTypeIDs[idNum];

			return -1;
		}

		//Used by editor for TacMap
		void getScaledColorMap (MemoryPtr bfr, long width);
};

typedef TerrainColorMap *TerrainColorMapPtr;
//---------------------------------------------------------------------------
#endif

