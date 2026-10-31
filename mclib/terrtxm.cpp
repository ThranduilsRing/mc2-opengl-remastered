//---------------------------------------------------------------------------
//
// TerrTxm.cpp -- File contains class code for the Terrain Textures
#include "content_error.h"
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#include "texture_assets.h"
#ifndef TERRTXM_H
#include"terrtxm.h"
#endif

#ifndef MATHFUNC_H
#include"mathfunc.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef INIFILE_H
#include"inifile.h"
#endif
#include "tga_scratch.h"
#include"../GameOS/gameos/gos_profiler.h"

#ifndef TGAINFO_H
#include"tgainfo.h"
#endif

#ifndef TERRAIN_H
#include"terrain.h"
#endif

#include"platform_windows.h"
#include "Gates/gate.h"
#include "mc2_diag_verbose.h"
#include "mc2fmt.h"
#include "Gates/gate_cold.h"

//---------------------------------------------------------------------------
// Static Globals
constexpr int TILE_THRESHOLD = 0;

long tileCacheReqs = 0;
long tileCacheHits = 0;
long tileCacheMiss = 0;

int TERRAIN_TXM_SIZE = 64;

//---------------------------------------------------------------------------
// OVERLAY-TILE-HIRES-1 (gate MC2_OVERLAY_TILE_HIRES, default ON).
// Historically initOverlay() hard-assumed 64x64 (TERRAIN_TXM_SIZE) and never
// resynced to the TGA header the way initTexture() does, so upscaled overlay
// tiles could not be read (wrong stride + loadTGATexture header assert). This
// helper is the single gate read: 0 = legacy path (byte-identical), else the
// preferred hi-res tile edge. "1" is shorthand for 256. Values must be
// power-of-two squares in [128,1024]; anything else disables the gate with a
// one-shot warning (never crash on a bad env var).
int MC2_OverlayTileHiresSize (void)
{
	static int s_size = -1;
	if (s_size < 0)
	{
		s_size = 0;
		const std::string hiresStr = mc2::gate::str<"MC2_OVERLAY_TILE_HIRES">();
		const char* v = hiresStr.c_str();
		if (v[0] && !(v[0] == '0' && v[1] == 0))
		{
			long req = (v[0] == '1' && v[1] == 0) ? 256 : atol(v);
			const bool pow2 = (req > 0) && ((req & (req - 1)) == 0);
			if (pow2 && req >= 128 && req <= 1024)
			{
				s_size = static_cast<int>(req);
				if (mc2DiagVerbose()) {
					mc2::fmt::print_out("[OVERLAY_TILE_HIRES v1] enabled tileEdge={} dir={}Overlays (per-file fallback to {}Overlays)\n",
						s_size, s_size, TERRAIN_TXM_SIZE);
				}
			}
			else
			{
				printf("[OVERLAY_TILE_HIRES v1] IGNORED MC2_OVERLAY_TILE_HIRES='%s' (want 1 or pow2 in [128,1024]) -- legacy 64px path\n", v);
			}
			fflush(stdout);
		}
	}
	return s_size;
}

constexpr int MAX_MASK_NUM = 14;

#define USE_TRANSITIONS
constexpr int MAX_MC2_TRANSITIONS = 8192;

bool TerrainTextures::terrainTexturesInstrumented = false;
long TerrainTextures::nextAvailable = 0;
long TerrainTextures::numTxms = 0;

extern bool InEditor;
//---------------------------------------------------------------------------
// Class TerrainTextures
//
// At Present, this class doesn't load textures til it needs 'em.  Once loaded
// they are loaded forever.  This may not be a problem, but if it becomes one,
// the textures can be preloaded during the init.
long TerrainTextures::init (const char *fileName, const char *baseName)
{
	metadataLoaded = true;
	if (InEditor)
		quickLoad = false;
	else
	{
		quickLoad = true;

		//Check for the magic list and then for all of the textures on the magic list.
		FullPathFileName listPath;
		listPath.init(texturePath,baseName,".lst");

		File listFile;
		long result = listFile.open(listPath);
		if (result == NO_ERR)
		{
			// Empty .lst means pre-cooked TXM set was never generated for this mission.
			// Treat it the same as a missing .lst so initMask() loads mask data and
			// createTransition() can generate + cache the .txm files on first load.
			bool anyListed = false;
			while (!listFile.eof())
			{
				char listName[1024];
				listFile.readString(reinterpret_cast<MemoryPtr>(listName));
				anyListed = true;
				if (!fileExists(listName))
				{
					quickLoad = false;
					break;
				}
			}
			if (!anyListed)
				quickLoad = false;
		}
		else
		{
			quickLoad = false;
		}
	}

	// MC2_FORCE_TXM_REGEN=1 -> ignore the pre-cooked .txm composite cache and
	// regenerate terrain texture composites from the source .tga (cement_N,
	// overlays, masks) so edited terrain art shows without an editor re-cook.
	// Same effect as InEditor's quickLoad=false. The .txm hash is keyed by the
	// texture-combo ID (content-blind), so a .tga edit never invalidates it on
	// its own; this is the override. Regen re-caches new loose .txm (loose wins
	// over FST), so the new art persists even after the env is unset.
	if (mc2::gate::get<"MC2_FORCE_TXM_REGEN">())
		quickLoad = false;

	tileCacheReqs = 0;
	tileCacheHits = 0;
	tileCacheMiss = 0;

	globalMipLevel = 0;

	FullPathFileName txmFileName;

	//--------------------------------------------------------
	// First setup the user heap with amount of RAM requested


	txmFileName.init(texturePath,fileName,".FIT");

	FitIniFile textureFile;

	long result = textureFile.open(txmFileName);
	gosASSERT(result == NO_ERR);

	result = textureFile.seekBlock("Main");
	gosASSERT(result == NO_ERR);

	result = textureFile.readIdLong("MaxTerrainTextures",numTxms);
	gosASSERT(result == NO_ERR);

	result = textureFile.readIdLong("MaxTerrainTypes",numTypes);
	gosASSERT(result == NO_ERR);

	result = textureFile.readIdLong("MaxTerrainOverlays",numOverlays);
	gosASSERT(result == NO_ERR);

	result = textureFile.readIdLong("MaxTerrainDetails",numDetails);
	gosASSERT(result == NO_ERR);

	//result = textureFile.readIdLong("MaxTerrainTransitions",numTransitions);
	//gosASSERT(result == NO_ERR);
	numTransitions = MAX_MC2_TRANSITIONS;

	unsigned long terrainTileCacheSize = 0;
	result = textureFile.readIdULong("TextureMemSize",terrainTileCacheSize);
	gosASSERT(result == NO_ERR);

	gosASSERT(result == NO_ERR);

	gosASSERT(result == NO_ERR);

	localBaseName = static_cast<char *>(std::malloc(strlen(baseName)+1));
	strcpy(localBaseName,baseName);

	//--------------------------------------------------------
	// Create the lists of pointers to the textures.
	textures = static_cast<TerrainTXMPtr>(std::malloc(numTxms * sizeof(TerrainTXM)));
	gosASSERT(textures != nullptr);

	memset(textures,-1,numTxms * sizeof(TerrainTXM));

	// Indirect-terrain cement-atlas slice: bulk memset(-1) above leaves
	// TerrainTXM::flags as 0xFFFFFFFF, making isCement() report true on every
	// uninitialized slot. Explicitly zero flags so only setTexture() can opt in.
	for (long i = 0; i < numTxms; ++i)
		textures[i].flags = 0;

	types = static_cast<MC_TerrainTypePtr>(std::malloc(numTypes * sizeof(MC_TerrainType)));
	gosASSERT(types != nullptr);

	memset(types,0,numTypes * sizeof(MC_TerrainType));

	overlays = static_cast<MC_OverlayTypePtr>(std::malloc(numOverlays * sizeof(MC_OverlayType)));
	gosASSERT(overlays != nullptr);

	memset(overlays, 0, numOverlays * sizeof(MC_OverlayType));

	details = static_cast<MC_DetailTypePtr>(std::malloc(numDetails * sizeof(MC_DetailType)));
	gosASSERT(details != nullptr);

	memset(details, 0, numDetails * sizeof(MC_DetailType));

	transitions = static_cast<TransitionTypePtr>(std::malloc(numTransitions * sizeof(TransitionType)));
	gosASSERT(transitions != nullptr);

	memset(transitions, -1, numTransitions * sizeof(TransitionType));

	//-----------------------------------
	// Read in the Base Terrain Textures.
	for (int i=0;i<numTypes;i++)
	{
		char blockName[512];
		mc2::fmt::to_buf(blockName,"TerrainType{:d}",mc2::fmt::as_i32(i));

		result = textureFile.seekBlock(blockName);
		if (result == NO_ERR)
		{
			result = textureFile.readIdLong("TerrainId",types[i].terrainId);
			gosASSERT(result == NO_ERR);
	
			char tmpy[2048];
			result = textureFile.readIdString("TerrainName",tmpy,2047);
			gosASSERT(result == NO_ERR);
			types[i].terrainName = static_cast<char *>(std::malloc(strlen(tmpy)+1));
			gosASSERT(types[i].terrainName != nullptr);
			strcpy(types[i].terrainName,tmpy);
	
			result = textureFile.readIdString("MaskName",tmpy,2047);
			gosASSERT(result == NO_ERR);
			types[i].maskName = static_cast<char *>(std::malloc(strlen(tmpy)+1));
			gosASSERT(types[i].maskName != nullptr);
			strcpy(types[i].maskName,tmpy);
	
			result = textureFile.readIdLong("TerrainPriority",types[i].terrainPriority);
			gosASSERT(result == NO_ERR);
	
			result = textureFile.readIdULong("TerrainTacRGB",types[i].terrainMapRGB);
			gosASSERT(result == NO_ERR);

			result = textureFile.readIdLong( "NameID", types[i].nameId );
			gosASSERT( result == NO_ERR );
	
			types[i].textureData = nullptr;
			types[i].maskData = nullptr;
		}
		else			//MUST load up blank ones or overlays will get trashed whenever we add a terrain type!!
		{
			types[i].terrainId = types[0].terrainId;
			types[i].terrainName = nullptr;
			types[i].maskName = nullptr;
			types[i].terrainPriority = types[0].terrainPriority;
			types[i].terrainMapRGB = types[0].terrainMapRGB;
			types[i].textureData = nullptr;
			types[i].maskData = nullptr;
		}
	}

	//-------------------------------------------------------------
	// Now preload the masks and base texture data
	// Assume as per art lead on 4/1/99 only one txm and 14 masks.
	for (int i=0;i<numTypes;i++)
	{
		if ((i && (types[i].terrainId != types[0].terrainId)) || !i)
		{
			types[i].textureData = static_cast<MemoryPtr *>(std::malloc(sizeof(MemoryPtr) * MC_MAX_MIP_LEVELS));
			gosASSERT(types[i].textureData != nullptr);
			
			memset(types[i].textureData,0,sizeof(MemoryPtr) * MC_MAX_MIP_LEVELS);

			types[i].maskData = static_cast<MemoryPtr *>(std::malloc(sizeof(MemoryPtr) * MC_MASK_NUM * MC_MAX_MIP_LEVELS));
			gosASSERT(types[i].maskData != nullptr);
			
			memset(types[i].maskData,0,sizeof(MemoryPtr) * MC_MASK_NUM);

			long terrainTextureIndex = initTexture(i);
			types[i].baseTXMIndex = terrainTextureIndex;

			initMask(i);
		}
		else if (i && (types[i].terrainId == types[0].terrainId))
		{
			MemoryPtr ourRAM = static_cast<MemoryPtr>(std::malloc(16 * 16 * sizeof(DWORD)));
			gosASSERT(ourRAM != nullptr);
			
			memset(ourRAM,0,16 * 16 * sizeof(DWORD));
			
			textures[nextAvailable].mcTextureNodeIndex = mc2::textures::fromRawPixels(reinterpret_cast<DWORD *>(ourRAM),gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink,16);
			// textureFromMemoryRaw memcpy's into its own cache block, so this buffer is scratch (BUGS 53: it was never freed, 181 blocks per mission).
			std::free(ourRAM);
			mc2::textures::setSessionLifetime(textures[nextAvailable].mcTextureNodeIndex, 0x1);
			nextAvailable++;

			if (nextAvailable > numTxms)
				STOP(("Too Many Terrain Textures!  Its at %d right now!",numTxms));

			types[i].baseTXMIndex = nextAvailable-1;
		}
	}

	//------------------------------------------
	// Read in the Overlay Textures and Shapes.
	for (int i=0;i<numOverlays;i++)
	{
		char blockName[512];
		mc2::fmt::to_buf(blockName,"OverlayType{:d}",mc2::fmt::as_i32(i));

		result = textureFile.seekBlock(blockName);
		gosASSERT(result == NO_ERR);

		result = textureFile.readIdLong("OverlayId",overlays[i].overlayId);
		gosASSERT(result == NO_ERR);

		char tmpy[2048];
		result = textureFile.readIdString("OverlayName",tmpy,2047);
		gosASSERT(result == NO_ERR);
		overlays[i].overlayName = static_cast<char *>(std::malloc(strlen(tmpy)+1));
		gosASSERT(overlays[i].overlayName != nullptr);
		strcpy(overlays[i].overlayName,tmpy);

		result = textureFile.readIdLong("NumOverlays",overlays[i].numTextures);
		gosASSERT(result == NO_ERR);

		result = textureFile.readIdLong("OldOverlayNum",overlays[i].oldOverlayId);
		gosASSERT(result == NO_ERR);

		result = textureFile.readIdBoolean("IsMLR",overlays[i].isMLRAppearance);
		gosASSERT(result == NO_ERR);

   		result = textureFile.readIdULong("TerrainTacRGB",overlays[i].terrainMapRGB);
   		gosASSERT(result == NO_ERR);
		
 		overlays[i].overlayData = nullptr;		//Contains MLR shape or TerrainTXM Data
 		overlays[i].overlayDataSizes = nullptr;	//OVERLAY-TILE-HIRES-1: per-tile edge sizes
	}

	//--------------------------------------------------------------
	// Now preload the overlay texture data 
	for (int i=0;i<numOverlays;i++)
	{
		overlays[i].overlayData = static_cast<MemoryPtr *>(std::malloc(sizeof(MemoryPtr) * overlays[i].numTextures * MC_MAX_MIP_LEVELS));
		gosASSERT(overlays[i].overlayData != nullptr);

		memset(overlays[i].overlayData,0,sizeof(MemoryPtr) * overlays[i].numTextures * MC_MAX_MIP_LEVELS);

		// OVERLAY-TILE-HIRES-1: parallel per-tile edge-size table. Default every
		// slot to the legacy TERRAIN_TXM_SIZE; initOverlay() overwrites per file
		// when the hires gate resyncs a tile to its actual TGA dimensions.
		overlays[i].overlayDataSizes = static_cast<long *>(std::malloc(sizeof(long) * overlays[i].numTextures * MC_MAX_MIP_LEVELS));
		gosASSERT(overlays[i].overlayDataSizes != nullptr);
		for (long s=0;s<overlays[i].numTextures * MC_MAX_MIP_LEVELS;s++)
			overlays[i].overlayDataSizes[s] = TERRAIN_TXM_SIZE;

		for (long j=0;j<overlays[i].numTextures;j++)
		{
			long overlayTextureIndex = initOverlay(i,j,overlays[i].overlayName);
			if (!j)
				overlays[i].baseTXMIndex = overlayTextureIndex;
				
			if (!i && !j)
				firstOverlay = overlayTextureIndex;
		}
	}

	//------------------------------------------
	// Read in the Detail Textures
	for (int i=0;i<numDetails;i++)
	{
		char blockName[512];
		mc2::fmt::to_buf(blockName,"DetailType{:d}",mc2::fmt::as_i32(i));

		result = textureFile.seekBlock(blockName);
		gosASSERT(result == NO_ERR);

		result = textureFile.readIdLong("DetailId",details[i].detailId);
		gosASSERT(result == NO_ERR);

		char tmpy[2048];
		result = textureFile.readIdString("DetailName",tmpy,2047);
		gosASSERT(result == NO_ERR);
		details[i].detailName = static_cast<char *>(std::malloc(strlen(tmpy)+1));
		gosASSERT(details[i].detailName != nullptr);
		strcpy(details[i].detailName,tmpy);

		result = textureFile.readIdLong("NumDetails",details[i].numDetails);
		gosASSERT(result == NO_ERR);

		result = textureFile.readIdFloat("FrameRate",details[i].frameRate);
		gosASSERT(result == NO_ERR);
		
		result = textureFile.readIdFloat("TilingFactor",details[i].tilingFactor);
		gosASSERT(result == NO_ERR);
		
 		details[i].detailData = nullptr;
	}

	//--------------------------------------------------------------
	// Now preload the detail texture data 
	for (int i=0;i<numDetails;i++)
	{
		details[i].detailData = static_cast<MemoryPtr *>(std::malloc(sizeof(MemoryPtr) * details[i].numDetails * MC_MAX_MIP_LEVELS));
		gosASSERT(details[i].detailData != nullptr);
		
		memset(details[i].detailData,0,sizeof(MemoryPtr) * details[i].numDetails * MC_MAX_MIP_LEVELS);

		for (long j=0;j < details[i].numDetails; j++)
		{
			long detailTextureIndex = initDetail(i,j);
			if (j==0)
				details[i].baseTXMIndex = detailTextureIndex;
		}
	}


	return(NO_ERR);
}

void TerrainTextures::initializeStatistics()
{
	
	if (!terrainTexturesInstrumented)
	{
			StatisticFormat( "" );
			StatisticFormat( "MechCommander 2 Terrain Texture" );
			StatisticFormat( "===============================" );
			StatisticFormat( "" );

			AddStatistic("Total Textures","Textures",gos_DWORD, static_cast<void*>(&(numTxms)), Stat_Total);

			AddStatistic("Textures Used ","Textures",gos_DWORD, static_cast<void*>(&(nextAvailable)), Stat_Total);

			StatisticFormat( "" );
			StatisticFormat( "" );

			terrainTexturesInstrumented = true;
	}
}
//---------------------------------------------------------------------------
long TerrainTextures::textureFromMemoryAlpha (MemoryPtr ourRAM, long mipLevel)
{
	// TERRAIN-ROAD-COLORKEY-TRANSPARENCY-1: this is the keyed (alpha) terrain-tile
	// upload boundary — the single shared path for every createTransition tile
	// (prebaked .txm and runtime build). MC2's magenta 0xFF00FF is the legacy
	// transparent colorkey, but textureFromMemoryRaw uploads raw and does NOT
	// honor gos_Texture_Keyed, so road/transition tiles whose non-road background
	// is magenta (e.g. forceAlphaOpaque slammed every pixel opaque, or the base
	// type texture is a magenta placeholder) render as an opaque full-tile magenta
	// patch under the road overlay. Convert the colorkey to alpha=0 here so the
	// overlay draw blends it away and the terrain base shows through. Exact
	// near-magenta (reserved colorkey) — real terrain art never uses it. The
	// tolerance also catches anti-aliased colorkey edges produced by the
	// createTransition combine's linear blend (e.g. 254,20,250), which an exact
	// 0xFF00FF match leaves as a thin opaque fringe. `mipLevel` is the tile width
	// in texels (see textureFromMemoryRaw width arg). Solid cement tiles
	// (textureFromMemory, gos_Texture_Solid) are unaffected.
	// Zero the WHOLE pixel (RGB + alpha), not just alpha: the tile is sampled with
	// GL_LINEAR, so road-edge texels interpolate against the keyed background. If we
	// left RGB=magenta at alpha 0, that magenta RGB bleeds into the road edge under
	// linear filtering and shows as a magenta fringe outlining every road. Zeroing
	// RGB makes the keyed region transparent-black so edges fade cleanly to the
	// terrain underneath with no colored halo.
	{
		DWORD* px = reinterpret_cast<DWORD*>(ourRAM);                 // BGRA-in-memory: B=byte0,G=byte1,R=byte2
		const long n = mipLevel * mipLevel;
		for (long i = 0; i < n; ++i) {
			const DWORD c = px[i];
			const long b = static_cast<long>((c & 0xFFu)), g = static_cast<long>(((c >> 8) & 0xFFu)), r = static_cast<long>(((c >> 16) & 0xFFu));
			// Chroma-based magenta test: red AND blue both clearly dominate green.
			// Catches pure 0xFF00FF and the anti-aliased magenta->cement edge pixels
			// (e.g. 200,108,200) that a flat g<80 cutoff leaves as a thin pink seam.
			// Greys (r~=g~=b), road asphalt, and cement never satisfy r>g+K && b>g+K.
			if (r > 150 && b > 150 && r > g + 40 && b > g + 40)
				px[i] = 0u;                         // transparent black -> no magenta/pink fringe
		}
	}
	textures[nextAvailable].mcTextureNodeIndex = mc2::textures::fromRawPixels(reinterpret_cast<DWORD *>(ourRAM),gos_Texture_Keyed,gosHint_DisableMipmap | gosHint_DontShrink,mipLevel);
	mc2::textures::setSessionLifetime(textures[nextAvailable].mcTextureNodeIndex, 0x1);

	nextAvailable++;

	if (nextAvailable > numTxms)
		STOP(("Too Many Terrain Textures!  Its at %d right now!",numTxms));

	return(nextAvailable-1);	
}

//---------------------------------------------------------------------------
long TerrainTextures::textureFromMemory (MemoryPtr ourRAM, long mipLevel)
{
	textures[nextAvailable].mcTextureNodeIndex = mc2::textures::fromRawPixels(reinterpret_cast<DWORD *>(ourRAM),gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink,mipLevel);
	mc2::textures::setSessionLifetime(textures[nextAvailable].mcTextureNodeIndex, 0x1);

	nextAvailable++;

	if (nextAvailable > numTxms)
		STOP(("Too Many Terrain Textures!  Its at %d right now!",numTxms));

	return(nextAvailable-1);	
}
	
//---------------------------------------------------------------------------
long TerrainTextures::loadTextureMemory (const char *textureName, long mipSize)
{
	textures[nextAvailable].mcTextureNodeIndex = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);

	nextAvailable++;

	if (nextAvailable > numTxms)
		STOP(("Too Many Terrain Textures!  Its at %d right now!",numTxms));

	return(nextAvailable-1);
}	

//---------------------------------------------------------------------------
long TerrainTextures::loadDetailMemory (const char *textureName, long mipSize)
{
	textures[nextAvailable].mcTextureNodeIndex = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DontShrink, 0, 0x1);

	nextAvailable++;

	if (nextAvailable > numTxms)
		STOP(("Too Many Terrain Textures!  Its at %d right now!",numTxms));

	return(nextAvailable-1);
}	

//---------------------------------------------------------------------------
long TerrainTextures::loadOverlayMemory (const char *textureName)
{
	textures[nextAvailable].mcTextureNodeIndex = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);

	nextAvailable++;

	if (nextAvailable > numTxms)
		STOP(("Too Many Terrain Textures!  Its at %d right now!",numTxms));

	return(nextAvailable-1);
}	

//---------------------------------------------------------------------------
long TerrainTextures::initDetail (long typeNum, long detailNum)
{
	//-----------------------------------------------------------------------
	// This function loads the TGA pointed to by name and number into sysRAM
	// It then copies the raw TGA texture to GOS Texture Memory.
	// We now control the MIP levels.  Load all MIP levels here!
	long txmResult = -1;
	for (int j=0;j<MC_MAX_MIP_LEVELS;j++)
	{
		//---------------------------------------------
		// Change Texture Path to reflect texture size
		char mipPath[512];
		long mipSize = 0;
		switch (j)
		{
			case 0:
				mc2::fmt::to_buf(mipPath,"{}{:d}" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32(TERRAIN_TXM_SIZE));
				mipSize = TERRAIN_TXM_SIZE;
				break;	

			case 1:
				mc2::fmt::to_buf(mipPath,"{}{:d}" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>1)));
				mipSize = (TERRAIN_TXM_SIZE>>1);
				break;

			case 2:
				mc2::fmt::to_buf(mipPath,"{}{:d}" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>2)));
				mipSize = (TERRAIN_TXM_SIZE>>2);
				break;

			case 3:
				mc2::fmt::to_buf(mipPath,"{}{:d}" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>3)));
				mipSize = (TERRAIN_TXM_SIZE>>3);
				break;
		}

		char tmpy[512];
		mc2::fmt::to_buf(tmpy,"{}{:04d}",mc2::fmt::safe(details[typeNum].detailName),mc2::fmt::as_i32(detailNum));

		FullPathFileName fileName;
		fileName.init(mipPath,tmpy,".tga");

		long result = loadDetailMemory(fileName,mipSize);
		if (j==0)
			txmResult = result;
	}

	return(txmResult);
}	

//---------------------------------------------------------------------------
long TerrainTextures::initTexture (long typeNum)
{
	//-----------------------------------------------------------------------
	// This function loads the TGA pointed to by name and number into sysRAM
	// It then copies the raw TGA texture to GOS Texture Memory.
	// We now control the MIP levels.  Load all MIP levels here!
	long txmResult = -1;
	for (long j=0;j<MC_MAX_MIP_LEVELS;j++)
	{
		//---------------------------------------------
		// Change Texture Path to reflect texture size
		char mipPath[512];
		long mipSize = 0;
		switch (j)
		{
			case 0:
				mc2::fmt::to_buf(mipPath,"{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(texturePath),mc2::fmt::as_i32(TERRAIN_TXM_SIZE));
				mipSize = TERRAIN_TXM_SIZE;
				break;	

			case 1:
				mc2::fmt::to_buf(mipPath,"{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>1)));
				mipSize = (TERRAIN_TXM_SIZE>>1);
				break;

			case 2:
				mc2::fmt::to_buf(mipPath,"{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>2)));
				mipSize = (TERRAIN_TXM_SIZE>>2);
				break;

			case 3:
				mc2::fmt::to_buf(mipPath,"{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>3)));
				mipSize = (TERRAIN_TXM_SIZE>>3);
				break;
		}

		FullPathFileName fileName;
		fileName.init(mipPath,types[typeNum].terrainName,".tga");

		long result = 0;
		if (InEditor || !quickLoad)
		{
			File tgaFile;
			result = tgaFile.open(fileName);
			gosASSERT(result == NO_ERR);
	
			struct TGAFileHeader header;
	
			tgaFile.read(reinterpret_cast<MemoryPtr>(&header),sizeof(TGAFileHeader));
	
			if (mipSize != header.width)
				mipSize = header.width;
	
			tgaFile.seek(0);
	
			MemoryPtr ourRAM = static_cast<MemoryPtr>(std::malloc(mipSize * mipSize * sizeof(DWORD)));
			gosASSERT(ourRAM != nullptr);
	
			loadTGATexture(&tgaFile,ourRAM,mipSize,mipSize);
	
			types[typeNum].textureData[j] = ourRAM;
	
			tgaFile.close();
		}

		result = loadTextureMemory(fileName,mipSize);
		if (j==0)
			txmResult = result;
	}

	return(txmResult);
}	

//---------------------------------------------------------------------------
void TerrainTextures::initMask (long typeNum)
{
	if (quickLoad)
		return;

	//----------------------------------------------------------------------------
	// The masks are stored in sysRAM only.  Need to load MC_MASK_NUM * miplevels
	// per terrain type.
	for (long i=0;i<MC_MAX_MIP_LEVELS;i++)
	{
		//---------------------------------------------
		// Change Texture Path to reflect texture size
		char mipPath[512];
		long mipSize = 0;
		switch (i)
		{
			case 0:
				mc2::fmt::to_buf(mipPath,"{}{:d}mask" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>0)));
				mipSize = (TERRAIN_TXM_SIZE>>0);
				break;	

			case 1:
				mc2::fmt::to_buf(mipPath,"{}{:d}mask" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>1)));
				mipSize = (TERRAIN_TXM_SIZE>>1);
				break;

			case 2:
				mc2::fmt::to_buf(mipPath,"{}{:d}mask" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>2)));
				mipSize = (TERRAIN_TXM_SIZE>>2);
				break;

			case 3:
				mc2::fmt::to_buf(mipPath,"{}{:d}mask" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>3)));
				mipSize = (TERRAIN_TXM_SIZE>>3);
				break;
		}

		for (long j=0;j<MC_MASK_NUM;j++)
		{
			char tmpy[512];
			mc2::fmt::to_buf(tmpy,"{}{:04d}",mc2::fmt::safe(types[typeNum].maskName),mc2::fmt::as_i32(j));

			FullPathFileName fileName;
			fileName.init(mipPath,tmpy,".tga");

			File tgaFile;

#ifdef _DEBUG
			long result =
#endif
			tgaFile.open(fileName);
			gosASSERT(result == NO_ERR);

			MemoryPtr ourRAM = static_cast<MemoryPtr>(std::malloc(mipSize * mipSize * sizeof(BYTE)));
			gosASSERT(ourRAM != nullptr);

			loadTGAMask(&tgaFile,ourRAM,mipSize,mipSize);

			types[typeNum].maskData[j+(i*MC_MASK_NUM)] = ourRAM;

			tgaFile.close();
		}
	}
}	

//---------------------------------------------------------------------------
long TerrainTextures::initOverlay (long overlayNum, long txmNum, char *txmName)
{
	long txmResult = -1;
	for (long j=0;j<MC_MAX_MIP_LEVELS;j++)
	{
		//---------------------------------------------
		// Change Texture Path to reflect texture size
		char mipPath[512];
		long mipSize = 0;
		switch (j)
		{
			case 0:
				mc2::fmt::to_buf(mipPath,"{}{:d}Overlays" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32(TERRAIN_TXM_SIZE));
				mipSize = TERRAIN_TXM_SIZE;
				break;	

			case 1:
				mc2::fmt::to_buf(mipPath,"{}{:d}Overlays" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>1)));
				mipSize = (TERRAIN_TXM_SIZE>>1);
				break;

			case 2:
				mc2::fmt::to_buf(mipPath,"{}{:d}Overlays" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>2)));
				mipSize = (TERRAIN_TXM_SIZE>>2);
				break;

			case 3:
				mc2::fmt::to_buf(mipPath,"{}{:d}Overlays" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>3)));
				mipSize = (TERRAIN_TXM_SIZE>>3);
				break;
		}

		char tmpy[512];
		mc2::fmt::to_buf(tmpy,"{}{:04d}",mc2::fmt::safe(txmName),mc2::fmt::as_i32(txmNum));

		FullPathFileName fileName;
		fileName.init(mipPath,tmpy,".tga");

		// OVERLAY-TILE-HIRES-1: probe hires overlays by default, with per-file fallback.
		// (1) Folder-by-size selection: the "%dOverlays" convention above IS the
		//     engine's intended res-folder mechanism (same TerrainTextureRes pref
		//     scheme as the base "%d"/"%dmask" folders) but TERRAIN_TXM_SIZE is
		//     clamped to 64 at startup, so 128Overlays/ has been dead weight.
		//     Under the gate, probe <hires>Overlays/ first, per-file fallback to
		//     the legacy folder — partial hi-res tile sets are legal.
		// (2) Header resync: mirror initTexture() (see "mipSize != header.width"
		//     above) so the sysRAM copy is read at the file's true stride instead
		//     of hard-assuming 64 (the old garble/assert failure).
		const int hiresEdge = (j == 0) ? MC2_OverlayTileHiresSize() : 0;
		FullPathFileName hiresName;
		bool useHires = false;
		if (hiresEdge > 0)
		{
			char hiresPath[512];
			mc2::fmt::to_buf(hiresPath,"{}{:d}Overlays" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32(hiresEdge));
			hiresName.init(hiresPath,tmpy,".tga");

			File probe;
			if (probe.open(hiresName) == NO_ERR)
			{
				useHires = true;
				probe.close();
			}
		}
		const char* chosenName = useHires ? static_cast<const char*>(hiresName) : static_cast<const char*>(fileName);

		// OVERLAY-TILE-HIRES-1 DIAG: settle "is 256 actually loading or falling
		// back to 64?" -- when the hires gate is armed, count hires-hits vs
		// legacy-fallbacks and print a running tally + the first few chosen paths
		// Only emit diagnostics when verbose logging is explicitly requested.
		if (hiresEdge > 0 && mc2DiagVerbose())
		{
			static int s_hiresHit = 0, s_hiresMiss = 0, s_hiresLogged = 0;
			if (useHires) ++s_hiresHit; else ++s_hiresMiss;
			if (s_hiresLogged < 8) {
				++s_hiresLogged;
				printf("[OVERLAY_TILE_HIRES v1] tile chose %s path=%s (hits=%d misses=%d)\n",
				       useHires ? "HIRES" : "fallback64", chosenName, s_hiresHit, s_hiresMiss);
				fflush(stdout);
			}
		}

		long result = 0;
		//------------------------------------------------------------
		if (InEditor || !quickLoad)
		{
			File tgaFile;

			result = tgaFile.open(chosenName);
			gosASSERT(result == NO_ERR);

			if (hiresEdge > 0)
			{
				// Resync to the TGA header like initTexture() does. Accept only
				// sane square power-of-two tiles; otherwise keep the declared
				// size (legacy behavior, incl. its loadTGATexture size assert).
				struct TGAFileHeader header;
				tgaFile.read(reinterpret_cast<MemoryPtr>(&header),sizeof(TGAFileHeader));
				tgaFile.seek(0);

				const long hw = header.width;
				const bool pow2 = (hw > 0) && ((hw & (hw - 1)) == 0);
				if ((hw == header.height) && pow2 && (hw >= 16) && (hw <= 1024) && (mipSize != hw))
					mipSize = hw;
			}

			MemoryPtr ourRAM = static_cast<MemoryPtr>(std::malloc(mipSize * mipSize * sizeof(DWORD)));
			gosASSERT(ourRAM != nullptr);

			loadTGATexture(&tgaFile,ourRAM,mipSize,mipSize);

			overlays[overlayNum].overlayData[txmNum] = ourRAM;
			if ((j == 0) && overlays[overlayNum].overlayDataSizes)
				overlays[overlayNum].overlayDataSizes[txmNum] = mipSize;

			tgaFile.close();
		}
		//------------------------------------------------------------
		result = loadOverlayMemory(chosenName);

		if (j==0)
			txmResult = result;
	}

	return(txmResult);
}	

//---------------------------------------------------------------------------
long TerrainTextures::setOverlay (DWORD overlayInfo)
{
	//-------------------------------------------------------------
	// This still uses the old Overlay Indicies from MC1 and MCX!
	long txmHandle = 0xffff0000;
	for (long i=0;i<numOverlays;i++)
	{
		if ((overlayInfo >= overlays[i].oldOverlayId) && 
			(overlayInfo <= (overlays[i].oldOverlayId+18)))
		{
			int index = (overlayInfo - overlays[i].oldOverlayId);
			if ( overlays[i].numTextures> 9 ) // roads have new indices
			{
				switch( index )
				{
				case 0:
				case 1:
				case 2:
					index = 0;
					break;

				case 3:
				case 4:
				case 5:
					index = 1;
					break;

				case 6:
				case 7:
				case 8:
				case 9:
				case 10:
				case 11:
				case 12:
				case 13:
				case 14:
					index = index - 4;
					break;

				case 15:
					 index = 9;
					 break;
				case 17:
					index = 7;
					break;
				case 18:
				case 16:
					index = index - 8;
					break;

				}
			}
			
			txmHandle = overlays[i].baseTXMIndex + ( index * MC_MAX_MIP_LEVELS);
			txmHandle <<= 16;
		}
	}

	return(txmHandle);
}

//---------------------------------------------------------------------------
// this doesn't use the old indices. Those shouldn't be necessary.
long TerrainTextures::getOverlayHandle( Overlays id, int Offset )
{
	long txmHandle = 0xffff0000;
	gosASSERT( id > -2 && id < numOverlays );
	
	if ( id > -1 && id < numOverlays )
	{
		txmHandle = overlays[id].baseTXMIndex + (Offset * MC_MAX_MIP_LEVELS );
		txmHandle <<= 16;
	}
	
	return(txmHandle);

}

void TerrainTextures::getOverlayInfoFromHandle( long handle, Overlays& id, DWORD& Offset )
{
	id = INVALID_OVERLAY;
	Offset = -1;

	for ( int i = 0; i < numOverlays; ++i )
	{
		if ( (handle >> 16) >= overlays[i].baseTXMIndex 
			 && (handle >> 16) < overlays[i].baseTXMIndex + overlays[i].numTextures * MC_MAX_MIP_LEVELS )
		{
			id = static_cast<Overlays>(i);
			long tmp = (handle >> 16) - overlays[i].baseTXMIndex;
			Offset = tmp/MC_MAX_MIP_LEVELS;			
		}
	}
}

//---------------------------------------------------------------------------
void TerrainTextures::combineTxm (MemoryPtr dest, DWORD binNumber, long type, long mipLevel)
{
	MemoryPtr combineRAM = types[type].textureData[mipLevel];
	MemoryPtr maskRAM = types[type].maskData[(binNumber-1) + (mipLevel * MC_MASK_NUM)];

	// Guard missing terrain-texture/mask data: a mod map (e.g. MCO-Carver5) referencing a
	// texture type whose textureData/maskData never loaded leaves these NULL -> the blend
	// loop reads maskRAM[i]/combineRAM at 0x0 (crash at mission init, terrtxm.cpp:874).
	// Skip the blend (leaves the base tile) instead of crashing.
	if (!combineRAM || !maskRAM)
		return;

	long mipSize = 0;
	switch (mipLevel)
	{
		case 0:
			mipSize = (TERRAIN_TXM_SIZE>>0);
			break;	

		case 1:
			mipSize = (TERRAIN_TXM_SIZE>>1);
			break;

		case 2:
			mipSize = (TERRAIN_TXM_SIZE>>2);
			break;

		case 3:
			mipSize = (TERRAIN_TXM_SIZE>>3);
			break;
	}

	for (long i=0;i<(mipSize * mipSize);i++)
	{
		if (maskRAM[i])
		{
			float combPercent = float(maskRAM[i]) / 255.0f;
			float destPercent = 1.0f - combPercent;

			if (combPercent == 1.0f)
			{
				*dest = *combineRAM;
				dest++;
				combineRAM++;

				*dest = *combineRAM;
				dest++;
				combineRAM++;

				*dest = *combineRAM;
				dest++;
				combineRAM++;

				if (*dest < maskRAM[i])
					*dest = maskRAM[i];
				dest++;
				combineRAM++;
			}
			else
			{
				float combValue = combPercent * float(*combineRAM);
				float destValue = destPercent * float(*dest);
				combValue += destValue;
				unsigned char value = static_cast<unsigned char>(combValue);
				*dest = value;
				dest++;
				combineRAM++;

				combValue = combPercent * float(*combineRAM);
				destValue = destPercent * float(*dest);
				combValue += destValue;
				value = static_cast<unsigned char>(combValue);
				*dest = value;
				dest++;
				combineRAM++;


				combValue = combPercent * float(*combineRAM);
				destValue = destPercent * float(*dest);
				combValue += destValue;
				value = static_cast<unsigned char>(combValue);
				*dest = value;
				dest++;
				combineRAM++;

				//This is the alpha channel.  DO NOT COMBINE!!!
				// Just copy in the CombineTXM AlphA!!
				//combValue = combPercent * float(*combineRAM);
				//destValue = destPercent * float(*dest);
				//combValue += destValue;
				//value = (unsigned char)combValue;
				if (*dest < maskRAM[i])
					*dest = maskRAM[i];
				dest++;
				combineRAM++;
			}
		}
		else
		{
			dest += 4;
			combineRAM += 4;
		}
	}
}	

//---------------------------------------------------------------------------
void forceAlphaOpaque(MemoryPtr dest, long mipLevel)
{
	long mipSize = 0;
	switch (mipLevel)
	{
		case 0:
			mipSize = (TERRAIN_TXM_SIZE>>0);
			break;	

		case 1:
			mipSize = (TERRAIN_TXM_SIZE>>1);
			break;

		case 2:
			mipSize = (TERRAIN_TXM_SIZE>>2);
			break;

		case 3:
			mipSize = (TERRAIN_TXM_SIZE>>3);
			break;
	}

	for (long i=0;i<(mipSize * mipSize);i++)
	{
		dest++;
		dest++;
		dest++;
		*dest = 0xff;
		dest++;
	}
}

//---------------------------------------------------------------------------
void TerrainTextures::combineOverlayTxm (MemoryPtr dest, long type, long mipLevel)
{
	//Convert overlay Type to actual overlay Index.
	long oType = 0;
	long oIndx = 0;
	for (long i=0;i<numOverlays;i++)
	{
		if ((type >= overlays[i].baseTXMIndex) &&
			(type < (overlays[i].baseTXMIndex + overlays[i].numTextures)))
		{
			oIndx = type - overlays[i].baseTXMIndex;
			oType = i;
			break;
		}
	}

	MemoryPtr combineRAM = overlays[oType].overlayData[oIndx];
	if (combineRAM == nullptr)
	{
		// Overlay texture slot unloaded or missing (e.g. quickLoad path skipped
		// TGA fill, or 'type' matched no overlay range leaving default oType/oIndx=0).
		// TCE ships overlay types that hit this; skip silently to avoid null-deref.
		return;
	}
	gosASSERT(combineRAM != nullptr);

	long mipSize = 0;
	switch (mipLevel)
	{
		case 0:
			mipSize = (TERRAIN_TXM_SIZE>>0);
			break;

		case 1:
			mipSize = (TERRAIN_TXM_SIZE>>1);
			break;

		case 2:
			mipSize = (TERRAIN_TXM_SIZE>>2);
			break;

		case 3:
			mipSize = (TERRAIN_TXM_SIZE>>3);
			break;
	}

	// OVERLAY-TILE-HIRES-1: when the gated hires loader resynced this overlay
	// tile to a different edge size than the 64px transition-bake dest, the
	// legacy lock-step pointer walk below would read the wrong stride. Resample
	// the overlay (nearest, exact for the pow2/pow2 ratio) down/up to the dest
	// grid instead. Gate OFF => overlayDataSizes[] is always TERRAIN_TXM_SIZE
	// => ovSize == mipSize at mip 0 and this branch never runs (mips 1-3 are
	// dead: MC_MAX_MIP_LEVELS == 1).
	const long ovSize = (overlays[oType].overlayDataSizes && mipLevel == 0)
		? overlays[oType].overlayDataSizes[oIndx] : mipSize;
	if (ovSize != mipSize && ovSize > 0)
	{
		for (long y = 0; y < mipSize; ++y)
		{
			const long sy = y * ovSize / mipSize;
			for (long x = 0; x < mipSize; ++x)
			{
				const long sx = x * ovSize / mipSize;
				const unsigned char* src = combineRAM + ((sy * ovSize + sx) << 2);
				unsigned char* dst = dest + ((y * mipSize + x) << 2);

				const float combPercent = float(src[3]) / 255.0f;
				if (combPercent == 0.0f)
					continue;
				if (combPercent == 1.0f)
				{
					dst[0] = src[0];
					dst[1] = src[1];
					dst[2] = src[2];
				}
				else
				{
					const float destPercent = 1.0f - combPercent;
					dst[0] = static_cast<unsigned char>((combPercent * float(src[0]) + destPercent * float(dst[0])));
					dst[1] = static_cast<unsigned char>((combPercent * float(src[1]) + destPercent * float(dst[1])));
					dst[2] = static_cast<unsigned char>((combPercent * float(src[2]) + destPercent * float(dst[2])));
				}
				//Alpha: DO NOT COMBINE (match the legacy loop) — keep the max.
				if (dst[3] < src[3])
					dst[3] = src[3];
			}
		}
		return;
	}

	for (int i=0;i<(mipSize * mipSize);i++)
	{
		float combPercent = float(*(combineRAM+3)) / 255.0f;
		float destPercent = 1.0f - combPercent;

		if (combPercent != 0.0)
		{
			if (combPercent == 1.0f)
			{
				*dest = *combineRAM;
				dest++;
				combineRAM++;
	
				*dest = *combineRAM;
				dest++;
				combineRAM++;
	
				*dest = *combineRAM;
				dest++;
				combineRAM++;
	
				if (*dest < *combineRAM)
					*dest = *combineRAM;
				dest++;
				combineRAM++;
			}
			else
			{
				float combValue = combPercent * float(*combineRAM);
				float destValue = destPercent * float(*dest);
				combValue += destValue;
				unsigned char value = static_cast<unsigned char>(combValue);
				*dest = value;
				dest++;
				combineRAM++;
	
				combValue = combPercent * float(*combineRAM);
				destValue = destPercent * float(*dest);
				combValue += destValue;
				value = static_cast<unsigned char>(combValue);
				*dest = value;
				dest++;
				combineRAM++;
	
	
				combValue = combPercent * float(*combineRAM);
				destValue = destPercent * float(*dest);
				combValue += destValue;
				value = static_cast<unsigned char>(combValue);
				*dest = value;
				dest++;
				combineRAM++;
	
				//combValue = combPercent * float(*combineRAM);
				//destValue = destPercent * float(*dest);
				//combValue += destValue;
				//value = (unsigned char)combValue;
				if (*dest < *combineRAM)
					*dest = *combineRAM;
				dest++;
				combineRAM++;
			}
		}
		else
		{
			dest += 4;
			combineRAM += 4;
		}
	}
}	

//---------------------------------------------------------------------------
void TerrainTextures::purgeTransitions (void)
{
	for (long i=0;i<nextTransition;i++)
	{
		mc2::textures::release(textures[transitions[i].baseTXMIndex].mcTextureNodeIndex);
	}
	
	nextTransition = 0;
	nextAvailable = firstTransition;
}

//---------------------------------------------------------------------------
inline bool isCementType (DWORD type)
{
	bool isCement = ((type == BASE_CEMENT_TYPE) ||
					((type >= START_CEMENT_TYPE) && (type <= END_CEMENT_TYPE)));
	return isCement;
}

//---------------------------------------------------------------------------
long TerrainTextures::createTransition (DWORD typeInfo, DWORD overlayInfo)
{
	long result = -1;
	long txmResult = 0;

	if (nextTransition < MAX_MC2_TRANSITIONS)
	{
		//------------------------------------------------------
		// Search Existing Transitions for this typeInfo.
		// Does not need to be fast yet.  May not ever since we
		// only do this on load can be done on Heidi's end.
		for (long i=0;i<nextTransition;i++)
		{
			if ((typeInfo == transitions[i].transitionIndex) && 
				(overlayInfo == transitions[i].overlayIndex))
			{
				return(transitions[i].baseTXMIndex);
			}
		}

		//---------------------------------------------
		// TESTING......
		// We may have already saved out a texture which
		// is the combined version.  Check for it.  Load it, if its there and move on.
		//-----------------------------------------------------------
		// TEST.....
		// What if we saved off all of the textures instead of combining
		// every time we load?  Should lower load time.  Lets see how big
		// for all of the textures...
		// About 600K for mc2_01.  With redundancies, assume about 600K per...
		// About 20 Mb larger build.
		if (quickLoad)
		{
			char nameData[256];
			mc2::fmt::to_buf(nameData,"{:08d}.{:08d}.txm",mc2::fmt::as_i32(typeInfo),mc2::fmt::as_i32(overlayInfo));

			FullPathFileName testPath;
			testPath.init(texturePath,nameData,"");

			File txmFile;
			long fr = txmFile.open(testPath);
			if (fr == NO_ERR)
			{
				long mipSize = TERRAIN_TXM_SIZE;
				std::vector<unsigned char> fileRAM;
				const bool cacheRead = mc2::tga::readAllBytes(txmFile, fileRAM);

				std::vector<unsigned char> lzBuffer(mipSize * mipSize * sizeof(DWORD));
				long bufferSize = cacheRead ? static_cast<long>(LZDecomp(lzBuffer.data(),fileRAM.data(),fileRAM.size(),mipSize * mipSize * sizeof(DWORD))) : -1;
				if (!cacheRead || bufferSize != (mipSize * mipSize * sizeof(DWORD)))
					mc2::content::error(mc2::content::Kind::Texture, static_cast<const char*>(testPath), "invalid transition cache; regenerating");

				txmFile.close();

				if (cacheRead && bufferSize == (mipSize * mipSize * sizeof(DWORD))) {
				long result = textureFromMemoryAlpha(lzBuffer.data(),mipSize);
				txmResult = result;

				transitions[nextTransition].transitionIndex = typeInfo;
				transitions[nextTransition].baseTXMIndex = txmResult;
				transitions[nextTransition].overlayIndex = overlayInfo;
				nextTransition++;

				return (txmResult);
				}
			}
		}

		if (!listTransitionFile)
		{
			//Check for the magic list and then for all of the textures on the magic list.
			CreateDirectory(texturePath,nullptr);

			FullPathFileName listPath;
			listPath.init(texturePath,localBaseName,".lst");

			listTransitionFile = std::make_unique<File>();
			listTransitionFile->create(listPath);
		}

		//---------------------------------
		// No transition yet.  Make it go!
		for (long kmp=0;kmp<MC_MAX_MIP_LEVELS;kmp++)
		{
			//---------------------------------------------
			// Change Texture Path to reflect texture size
			char mipPath[512];
			long mipSize = 0;
			switch (kmp)
			{
				case 0:
					mc2::fmt::to_buf(mipPath,"{}{:d}" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>0)));
					mipSize = (TERRAIN_TXM_SIZE>>0);
					break;	

				case 1:
					mc2::fmt::to_buf(mipPath,"{}{:d}" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>1)));
					mipSize = (TERRAIN_TXM_SIZE>>1);
					break;

				case 2:
					mc2::fmt::to_buf(mipPath,"{}{:d}" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>2)));
					mipSize = (TERRAIN_TXM_SIZE>>2);
					break;


				case 3:
					mc2::fmt::to_buf(mipPath,"{}{:d}" PATH_SEPARATOR "",mc2::fmt::safe(texturePath),mc2::fmt::as_i32((TERRAIN_TXM_SIZE>>3)));
					mipSize = (TERRAIN_TXM_SIZE>>3);
					break;
			}

			//-------------------------------------------------------
			// This one does not exist.  Make it.
			MemoryPtr ourRAM = static_cast<MemoryPtr>(std::malloc(mipSize * mipSize * sizeof(DWORD)));

			BYTE v0Type = typeInfo & 0x000000ff;
			BYTE v1Type = typeInfo >> 8;
			BYTE v2Type = typeInfo >> 16;
			BYTE v3Type = typeInfo >> 24;

			BYTE priTypes[4];
			priTypes[0] = typeInfo & 0x000000ff;
			priTypes[1] = typeInfo >> 8;
			priTypes[2] = typeInfo >> 16;
			priTypes[3] = typeInfo >> 24;

			//------------------------------------------
			// Sort types by priority.
			for (int i=0;i<4;i++)
			{
				for (long j=i;j<4;j++)
				{
					if (types[priTypes[j]].terrainPriority < types[priTypes[i]].terrainPriority)
					{
						BYTE t = priTypes[i];
						priTypes[i] = priTypes[j];
						priTypes[j] = t;
					}
				}
			}

			//-----------------------------------------------
			// Draw by Priority and Mask.
			// Priority 0 is ALWAYS just copy that texture.
			// Use a random one if possible.
			MemoryPtr texture = types[priTypes[0]].textureData[kmp];
			if (!Terrain::terrainTextures2 || isCementType(priTypes[0]))
			{
				//Base is now a BLANK ALPHA MASK for new Terrain
				//This means we shove a blank transparent texture
				//into the starting buffer.  Easily done with memset(dest,0,sizeof);
				// Guard missing terrain-texture data: a mod map referencing a texture
				// type whose textureData[kmp] never loaded leaves `texture` NULL ->
				// memcpy reads from 0x0 (crash at mission init, terrtxm.cpp:1273; e.g.
				// MCO-Carver5). Blank the tile instead of crashing.
				if (texture)
					memcpy(ourRAM,texture,mipSize * mipSize * sizeof(DWORD));
				else
					memset(ourRAM,0,mipSize * mipSize * sizeof(DWORD));
				forceAlphaOpaque(ourRAM,kmp);
			}
			else
				memset(ourRAM,0x01,mipSize * mipSize * sizeof(DWORD));

			//-------------------------------------------------------------
			// Create Binary number based on types which MATCH priType[1].
			DWORD binNumber = 0;
			if (priTypes[0] != priTypes[1])
			{
				binNumber = ((v0Type == priTypes[1])*8) + 
							((v1Type == priTypes[1])*4) + 
							((v2Type == priTypes[1])*2) + 
							((v3Type == priTypes[1]));

				//--------------------------------------------------------------
				// We now have the mask number, send texture to Combiner school
				// ONLY send it to school in the new universe if its type is cement.
				// Otherwise, NO!
				if (!Terrain::terrainTextures2 || isCementType(priTypes[1]))
					combineTxm(ourRAM,binNumber,priTypes[1],kmp);
			}

			if (priTypes[1] != priTypes[2])
			{
				binNumber = ((v0Type == priTypes[2])*8) + 
							((v1Type == priTypes[2])*4) + 
							((v2Type == priTypes[2])*2) + 
							((v3Type == priTypes[2]));

				//--------------------------------------------------------------
				// We now have the mask number, send texture to Combiner school
				// ONLY send it to school in the new universe if its type is cement.
				// Otherwise, NO!
				if (!Terrain::terrainTextures2 || isCementType(priTypes[2]))
					combineTxm(ourRAM,binNumber,priTypes[2],kmp);
			}

			if (priTypes[2] != priTypes[3])
			{
				binNumber = ((v0Type == priTypes[3])*8) + 
							((v1Type == priTypes[3])*4) + 
							((v2Type == priTypes[3])*2) + 
							((v3Type == priTypes[3]));

				//--------------------------------------------------------------
				// We now have the mask number, send texture to Combiner school
				// ONLY send it to school in the new universe if its type is cement.
				// Otherwise, NO!
				if (!Terrain::terrainTextures2 || isCementType(priTypes[3]))
					combineTxm(ourRAM,binNumber,priTypes[3],kmp);
			}

			if (overlayInfo != 0xffff)
			{
				//ONLY Combine certain overlays to prevent dumb things from old universe from showing up
				if (!Terrain::terrainTextures2)
					combineOverlayTxm(ourRAM,overlayInfo,kmp);
				else
				{
					if ((overlayInfo < 285) || (overlayInfo > 292))		//Don't draw the stupid rough stuff
						combineOverlayTxm(ourRAM,overlayInfo,kmp);
				}
			}
			
			//-------------------------------------------------
			// OurRAM at this point is the new magical Texture
			if (!Terrain::terrainTextures2)
				result = textureFromMemory(ourRAM,mipSize);
			else
				result = textureFromMemoryAlpha(ourRAM,mipSize);

			//-----------------------------------------------------------
			// TEST.....
			// What if we saved off all of the textures instead of combining
			// every time we load?  Should lower load time.  Lets see how big
			// for all of the textures...
			//
			char nameData[256];
			mc2::fmt::to_buf(nameData,"{:08d}.{:08d}.txm",mc2::fmt::as_i32(typeInfo),mc2::fmt::as_i32(overlayInfo));

			FullPathFileName testPath;
			testPath.init(texturePath,nameData,"");

			if (listTransitionFile)
			{
				listTransitionFile->writeString(testPath);
				listTransitionFile->writeByte(0);
			}

			std::vector<unsigned char> lzBuffer(mipSize * mipSize * sizeof(DWORD) * 2);
			long bufferSize = static_cast<long>(LZCompress(lzBuffer.data(),ourRAM,(mipSize * mipSize * sizeof(DWORD))));

			File txmFile;
			txmFile.create(testPath);
			txmFile.write(lzBuffer.data(),bufferSize);
			txmFile.close();
				
			if (kmp == 0)
				txmResult = result;

			std::free(ourRAM);
		}

		transitions[nextTransition].transitionIndex = typeInfo;
		transitions[nextTransition].baseTXMIndex = txmResult;
		transitions[nextTransition].overlayIndex = overlayInfo;
		nextTransition++;
	}

	return(txmResult);
}	

//---------------------------------------------------------------------------
long TerrainTextures::setTexture (DWORD typeInfo, DWORD overlayInfo)
{
	BYTE v0Type = typeInfo & 0x000000ff;
	BYTE v1Type = typeInfo >> 8;
	BYTE v2Type = typeInfo >> 16;
	BYTE v3Type = typeInfo >> 24;

	gosASSERT(v0Type < numTypes);
	gosASSERT(v1Type < numTypes);
	gosASSERT(v2Type < numTypes);
	gosASSERT(v3Type < numTypes);

	if ((v0Type == v1Type) && (v2Type == v3Type) && (v1Type == v3Type) && (overlayInfo == 0xffff))
	{
		long txmHandle = types[v0Type].baseTXMIndex;
		
		if (
			((v0Type == BASE_CEMENT_TYPE) ||
			((v0Type >= START_CEMENT_TYPE) && (v0Type <= END_CEMENT_TYPE))) ||
			((v1Type == BASE_CEMENT_TYPE) ||
			((v1Type >= START_CEMENT_TYPE) && (v1Type <= END_CEMENT_TYPE))) ||
			((v2Type == BASE_CEMENT_TYPE) ||
			((v2Type >= START_CEMENT_TYPE) && (v2Type <= END_CEMENT_TYPE))) ||
			((v3Type == BASE_CEMENT_TYPE) ||
			((v3Type >= START_CEMENT_TYPE) && (v3Type <= END_CEMENT_TYPE)))
			)
			textures[txmHandle].flags = MC2_TERRAIN_CEMENT_FLAG;		//No ALPHA.  Its a Solid cement texture.
		else
			textures[txmHandle].flags = 0;
			
		return(txmHandle);
	}
	else
	{
#ifdef USE_TRANSITIONS
		//------------------------------
		// Must create Transition here.
		// BUT NOW we only create them for CEMENT!!
		// New Terrain you know!!
		long txmHandle = 0xffffffff;
		if ( (overlayInfo != 0xffff) ||
			((v0Type == BASE_CEMENT_TYPE) ||
			((v0Type >= START_CEMENT_TYPE) && (v0Type <= END_CEMENT_TYPE))) ||
			((v1Type == BASE_CEMENT_TYPE) ||
			((v1Type >= START_CEMENT_TYPE) && (v1Type <= END_CEMENT_TYPE))) ||
			((v2Type == BASE_CEMENT_TYPE) ||
			((v2Type >= START_CEMENT_TYPE) && (v2Type <= END_CEMENT_TYPE))) ||
			((v3Type == BASE_CEMENT_TYPE) ||
			((v3Type >= START_CEMENT_TYPE) && (v3Type <= END_CEMENT_TYPE)))
			)
		{
			txmHandle = createTransition(typeInfo,overlayInfo);
			if (firstTransition == -1)
				firstTransition = txmHandle;
				
			textures[txmHandle].flags = MC2_TERRAIN_CEMENT_FLAG;
		}
		else
		{
			txmHandle = types[v0Type].baseTXMIndex;
			textures[txmHandle].flags = 0;
		}
			
		//Complex here.  If we are a cement texture and ANY other vxType was NOT cement, ALPHA is TRUE.
		// If we are a road and ANY other vxType was NOT cement, ALPHA is TRUE,
		// Otherwise, its false.  Thus, simply mark it true for any non-solid cement!!
		// We simply won't use anything that's not cement or road or runway, etc.
		if ((textures[txmHandle].flags & MC2_TERRAIN_CEMENT_FLAG) == MC2_TERRAIN_CEMENT_FLAG)
		{
			if (
				(((v0Type != BASE_CEMENT_TYPE) &&
				(v0Type < START_CEMENT_TYPE)) || (v0Type > END_CEMENT_TYPE))
				)
				textures[txmHandle].flags |= MC2_TERRAIN_ALPHA_FLAG;
				
			if (
				(((v1Type != BASE_CEMENT_TYPE) &&
				(v1Type < START_CEMENT_TYPE)) || (v1Type > END_CEMENT_TYPE))
				)
				textures[txmHandle].flags |= MC2_TERRAIN_ALPHA_FLAG;
	 
			if (
				(((v2Type != BASE_CEMENT_TYPE) &&
				(v2Type < START_CEMENT_TYPE)) || (v2Type > END_CEMENT_TYPE))
				)
				textures[txmHandle].flags |= MC2_TERRAIN_ALPHA_FLAG;
	 
			if (
				(((v3Type != BASE_CEMENT_TYPE) &&
				(v3Type < START_CEMENT_TYPE)) || (v3Type > END_CEMENT_TYPE))
				)
				textures[txmHandle].flags |= MC2_TERRAIN_ALPHA_FLAG;
		}
    		
#else
		long txmHandle = types[v0Type].baseTXMIndex;
#endif
		return(txmHandle);
	}
}							  

//---------------------------------------------------------------------------
long TerrainTextures::setDetail (DWORD typeInfo, DWORD frameNum)
{
	if (typeInfo < numDetails)
	{
		long txmHandle = details[typeInfo].baseTXMIndex + (frameNum * MC_MAX_MIP_LEVELS );
		return(txmHandle);
	}
	
	return 0xffffffff;
}

//---------------------------------------------------------------------------
void TerrainTextures::update (void)
{
	ZoneScopedN("TerrainTextures::update");
	if (metadataLoaded)
	{
		// BUGS 53: UserHeap::destroy() hands nothing back to the allocator (gos_DestroyMemoryHeap deletes the heap record only),
		// so every block init() took from tileRAMHeap leaked once per mission (538 blocks on mc2_03). Free them here, where the
		// pointers are dropped. Owner of each block: types[] / overlays[] / details[] entries own their name strings and mip tables;
		// the mip entries are non-null only when init() ran the non-quickLoad path (editor, or no .lst); none of them aliases
		// another table (the tables hold one malloc each, written once in init()).
		auto freeBlock = [this](void* block) { if (block) std::free(block); };
		auto freeTable = [&freeBlock](MemoryPtr* table, long entries) {
			if (!table)
				return;
			for (long m = 0; m < entries; m++)
				freeBlock(table[m]);
			freeBlock(table);
		};
		for (long i = 0; types && i < numTypes; i++)
		{
			freeBlock(types[i].terrainName);
			freeBlock(types[i].maskName);
			freeTable(types[i].textureData, MC_MAX_MIP_LEVELS);
			freeTable(types[i].maskData, MC_MASK_NUM * MC_MAX_MIP_LEVELS);
		}
		for (long i = 0; overlays && i < numOverlays; i++)
		{
			freeBlock(overlays[i].overlayName);
			freeTable(overlays[i].overlayData, overlays[i].numTextures * MC_MAX_MIP_LEVELS);
			freeBlock(overlays[i].overlayDataSizes);
		}
		for (long i = 0; details && i < numDetails; i++)
		{
			freeBlock(details[i].detailName);
			freeTable(details[i].detailData, details[i].numDetails * MC_MAX_MIP_LEVELS);
		}
		freeBlock(types);
		freeBlock(overlays);
		freeBlock(transitions);
		freeBlock(localBaseName);

		//We can now safely purge all transitions cause the MC2 texture manager has them all!!
		numTypes = 0;
		types = nullptr;

		numOverlays = 0;
		overlays = nullptr;
		
		numTransitions = 0;
		transitions = nullptr;
		nextTransition = 0;

		for (long i=0;i<numDetails;i++)
		{
			details[i].detailName = nullptr;
			details[i].detailData = nullptr;
		}
			
		if (listTransitionFile)
		{
			listTransitionFile->close();
			listTransitionFile.reset();
		}

		localBaseName = nullptr;
		metadataLoaded = false;

	}
}

//---------------------------------------------------------------------------
void TerrainTextures::destroy (void)
{
	// [TXMMGR-TEXTURE-LEAK-FIX-1] Release every master texture-node slot this
	// per-mission TerrainTextures allocated. All of these are pinned neverFLUSH
	// (setTextureNeverFlush / loadTexture nFlush=0x1 in initDetail / initTexture /
	// loadOverlayMemory etc.), so the per-mission flush() skips them. Without an
	// explicit releaseNode() per slot here, the detail / type / overlay
	// (e.g. data/textures/64overlays/) nodes accumulate in masterTextureNodes[]
	// every mission and eventually STOP("TOO Many textures"). textures[] is
	// memset(-1) at init so unused slots hold 0xffffffff; nextAvailable bounds
	// the live region. These are rebuilt next mission in init(), so freeing on
	// unload is correct.
	if (mc2::textures::ready() && textures)
	{
		for (long i=0;i<nextAvailable;i++)
		{
			if (textures[i].mcTextureNodeIndex != 0xffffffff)
			{
				mc2::textures::release(textures[i].mcTextureNodeIndex);
				textures[i].mcTextureNodeIndex = 0xffffffff;
			}
		}
	}
	nextAvailable = 0;

	update();

	// tileHeap blocks (BUGS 53): the heap's destroy frees none of them. textures[] and details[] stay readable all mission
	// (isCement, getTilingFactor), so they are released here and only here.

	{
		if (textures)
			std::free(textures);
		if (details)
			std::free(details);
	}
	textures = nullptr;
	details = nullptr;
	numTxms = 0;
	numDetails = 0;

}

//---------------------------------------------------------------------------
//		
//	Edit Log
//
//---------------------------------------------------------------------------
