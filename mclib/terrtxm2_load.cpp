// TerrainColorMap: getScaledColorMap and TerrainColorMap::init(fileName): the mission colormap load.
// Declared in the class headers; shared names in terrtxm2_internal.h.
#include "mc2_czstring.h"
#include <bit>
#include "terrtxm2_internal.h"
#include "content_error.h"
#include "texture_assets.h"
#include "mc2fmt.h"
#include "tga_scratch.h"
#include "terrain_normal_load.h"
#include "../GameOS/gameos/terrain_field_store.h"
#include "../GameOS/gameos/gos_render.h"

using mc2::tga::loadTga;
using mc2::tga::tgaPixelsPresent;

//---------------------------------------------------------------------------
//Used by editor for TacMap
void TerrainColorMap::getScaledColorMap (MemoryPtr bfr, long dWidth)
{
	bool wasColorMapAround = true;

	if (!ColorMap)
	{
		wasColorMapAround = false;
		char *fileName = Terrain::colorMapName;
		if (!fileName)
			fileName = Terrain::terrainName;


 		FullPathFileName colorMapName;
		colorMapName.init(texturePath,fileName,".tga");
		
		char newName[1024];
		mc2::fmt::to_buf(newName, "{}.burnin", mc2::fmt::safe(fileName));
	
		FullPathFileName burnInName;
		burnInName.init(texturePath,newName,".tga");

		//This is what I meant.  DO NOT use burnin unless we have to!
 		File colorMapFile;
		long result = colorMapFile.open(burnInName);
		if (result != NO_ERR)
		{
			result = colorMapFile.open(colorMapName);
			if (result != NO_ERR)
			{
#if defined(DEBUG) || defined(PROFILE)
				PAUSE(("Unable to open Terrain Color Map %s.  Press Continue",colorMapName));
#endif
				mc2::content::error(mc2::content::Kind::Texture, static_cast<const char*>(colorMapName), "missing colormap; flat fallback");
				return;
			}
		}
			
		std::vector<unsigned char> tgaFileImage;
		TGAFileHeader colorMapInfo{};
		if (!loadTga(colorMapFile, tgaFileImage, colorMapInfo, "colour map"))
			return;
		
		if (colorMapInfo.image_type == UNC_TRUE)
		{
			MemoryPtr loadBuffer = tgaFileImage.data() + sizeof(TGAFileHeader);
			if (colorMapInfo.width != colorMapInfo.height)
			{
				mc2::content::error(mc2::content::Kind::Texture, static_cast<const char*>(colorMapName), "colormap not square; flat fallback");
				return;
			}
			if (!tgaPixelsPresent(tgaFileImage, colorMapInfo, "colour map"))
				return;
			
			//-----------------------------------------------------------------
			// Check if 24 or 32 bit.  If 24, do the necessary stuff to it.
			ownColorMap(static_cast<size_t>(colorMapInfo.width) * static_cast<size_t>(colorMapInfo.width) * sizeof(DWORD));
			
			if (colorMapInfo.pixel_depth == 24)
			{
				//24-Bit color means we must add in an opaque alpha to each 24 bits of color data.
				MemoryPtr cMap = ColorMap;
				MemoryPtr lMap = loadBuffer;
				for (long i = 0;i<(colorMapInfo.width * colorMapInfo.width);i++)
				{
					*cMap = *lMap;		//Red
					cMap++;
					lMap++;
					
					*cMap = *lMap;		//Green
					cMap++;
					lMap++;
					
					*cMap = *lMap;		//Blue
					cMap++;
					lMap++;
					
					*cMap = 0xff;		 //Alpha
					cMap++;
				}
			}
			else
			{
				//32-bit color means all we have to do is copy the buffer.
				memcpy(ColorMap,loadBuffer,colorMapInfo.width * colorMapInfo.width * sizeof(DWORD));
			}
			
			
			DWORD numTextures = colorMapInfo.width / COLOR_MAP_TEXTURE_SIZE;
			numTexturesAcross = numTextures;
			
			//------------------------------------------------------------------------
			// Must check image_descriptor to see if we need to un upside down image.
			//bool left = (colorMapInfo.iimage_descriptor & 16) != 0;
			bool top = (colorMapInfo.image_descriptor & 32) != 0;
	
			if (!top)
			{
				flipTopToBottom(ColorMap,32,colorMapInfo.width,colorMapInfo.height);
			}
		}
	}

	//Grab every xth pixel from every yth scan line!
	float cMapWidth = numTexturesAcross * COLOR_MAP_TEXTURE_SIZE;
	float xSkip = cMapWidth / static_cast<float>(dWidth);
	float ySkip = xSkip;

	long xOffset = 0, yOffset = 0;

	DWORD *currentColor = std::bit_cast<DWORD *>(ColorMap);
	DWORD *currentBfr = std::bit_cast<DWORD *>(bfr);
	for (long i=0;i<(dWidth * dWidth);i++)
	{
		*currentBfr = *currentColor;
		xOffset++;
		if (xOffset >= dWidth)
		{
			xOffset = 0;
			yOffset++;
		}

		currentBfr++;
		currentColor = (std::bit_cast<DWORD *>(ColorMap)) + static_cast<long>(xOffset * xSkip) + (static_cast<long>(yOffset*ySkip) * static_cast<long>(cMapWidth));
	}

	if (!wasColorMapAround)
		releaseColorMapScratch();
}

static long sReadIdFloat(FitIniFile* missionFile, mc2::czstring varName, float &value) {
	long result = 0;
	float tmpFloat;
	result = missionFile->readIdFloat(const_cast<char *>(varName), tmpFloat);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpFloat;
	}
	return result;
}

//---------------------------------------------------------------------------
long TerrainColorMap::init (char *fileName)
{
	ZoneScopedN("TerrainColorMap::init");
	auto flatFallback = [&] {
		releaseColorMapScratch();
		if (cpuColorMap) free(cpuColorMap);
		cpuColorMap = static_cast<unsigned char*>(malloc(4));
		gosASSERT(cpuColorMap != nullptr);
		if (!cpuColorMap) return -1;
		cpuColorMap[0] = 255; cpuColorMap[1] = 0;
		cpuColorMap[2] = 255; cpuColorMap[3] = 255;
		cpuColorMapSize = 1;
		numTextures = 0;
		numTexturesAcross = fractionPerTexture = 1.0f;
		colorMapStarted = true;
		ktx2ColormapPath[0] = '\0';
		return NO_ERR;
	};
	if (!colorMapStarted)
	{
		ZoneScopedN("TerrainColorMap::init cold");
		//Load up the detail texture first.
		// Detail texture is named same as colormap with .detail in name.
		// if no file exists, no texture drawn.
		char dName[1024];
		mc2::fmt::to_buf(dName, "{}.detail", mc2::fmt::safe(fileName));
		
		FullPathFileName detailFile;
		detailFile.init(texturePath,dName,".tga");

		if (!fileExists(detailFile))
		{
			/* If a detail texture doesn't exist for this mission, use the default detail texture.*/
			detailFile.destroy();
			mc2::fmt::to_buf(dName, "defaults" PATH_SEPARATOR "default_detail");
			detailFile.init(texturePath,dName,".tga");
		}
		if (fileExists(detailFile))		//Otherwise, its already 0xffffffff!!
		{
			ZoneScopedN("TerrainColorMap::init detailTexture");
			detailTextureNodeIndex.reset(mc2::textures::load(detailFile,gos_Texture_Alpha,gosHint_DontShrink, 0, 0x1));
		}
		else
			gosASSERT(false);
		
		//Ok, now load up the water texture.
		// Water texture is named the same as the colormap with .water in name.
		// If no file exists, no texture drawn!!
		mc2::fmt::to_buf(dName, "{}.water", mc2::fmt::safe(fileName));
		
		FullPathFileName waterFile;
		waterFile.init(texturePath,dName,".tga");
		
		if (!fileExists(waterFile))
		{
			/* If a water texture doesn't exist for this mission, use the default water texture.*/
			waterFile.destroy();
			mc2::fmt::to_buf(dName, "defaults" PATH_SEPARATOR "default_water");
			waterFile.init(texturePath,dName,".tga");
		}
		if (fileExists(waterFile))
		{
			ZoneScopedN("TerrainColorMap::init waterTexture");
			waterTextureNodeIndex.reset(mc2::textures::load(waterFile,gos_Texture_Solid,0, 0, 0x1));
		}
		else
			gosASSERT(false);

		//Then, load up the water detail texture(s).
		// Water detail texture is named the same as the colormap with .water0000 in name
		// Where 0000 is the frame number of the animation for the water detail.
		// If the first frame does not exist, no texture is drawn!!
		char waterDetailBaseName[1024];
		mc2::fmt::to_buf(waterDetailBaseName, "{}.water", mc2::fmt::safe(fileName));
		{
			mc2::fmt::to_buf(dName, "{}{:04d}", mc2::fmt::safe(waterDetailBaseName), mc2::fmt::as_i32(0));
			
			FullPathFileName waterFile;
			waterFile.init(texturePath,dName,".tga");
			
			if (!fileExists(waterFile))
			{
				mc2::fmt::to_buf(waterDetailBaseName, "defaults" PATH_SEPARATOR "default_water");
			}
		}
		for (auto & waterDetailNodeIndexElem : waterDetailNodeIndex)
		{
			waterDetailNodeIndexElem.reset();
		}
		mc2::fmt::to_buf(dName, "{}{:04d}", mc2::fmt::safe(waterDetailBaseName), mc2::fmt::as_i32(0));
		waterFile.init(texturePath,dName,".tga");
		{ ZoneScopedN("TerrainColorMap::init waterDetailTextures"); resetWaterDetailTextures(static_cast<const char *>(waterFile)); }

		{
			detailTextureTilingFactor = 30.0f;
			waterTextureTilingFactor = 48.0f;
			waterDetailTilingFactor = 45.0f;

			FullPathFileName missionFitFilePath;
			missionFitFilePath.init(missionPath,fileName,".fit");
			FitIniFile missionFitIni;
			if (fileExists(missionFitFilePath))
			{
				missionFitIni.open(const_cast<char*>(static_cast<const char*>(missionFitFilePath)));
				long result = missionFitIni.seekBlock( "Terrain" );
				gosASSERT( result == NO_ERR );
				result = sReadIdFloat(&missionFitIni, "DetailTextureTilingFactor", detailTextureTilingFactor);
				result = sReadIdFloat(&missionFitIni, "WaterTextureTilingFactor", waterTextureTilingFactor);
				result = sReadIdFloat(&missionFitIni, "WaterDetailTilingFactor", waterDetailTilingFactor);
				missionFitIni.close();
			}
		}

 		FullPathFileName colorMapName;
		colorMapName.init(texturePath,fileName,".tga");
		
		char newName[1024];
		mc2::fmt::to_buf(newName, "{}.burnin", mc2::fmt::safe(fileName));
	
		FullPathFileName burnInName;
		burnInName.init(texturePath,newName,".tga");

		FullPathFileName burnInJpg;
		burnInJpg.init(texturePath,newName,".jpg");

		long result = NO_ERR;

		// BURNIN-DISK-JPEG: probe <name>.burnin.jpg first. On success it
		// decodes (stb_image) straight into a B,G,R,A ColorMap matching the
		// .burnin.tga path, so we can skip the TGA read/parse/flip entirely.
		long jpgColorMapWidth = 0;
		graphics::loading_work([&] { colorMapStore = tryLoadBurninJpg(texturePath, fileName, &jpgColorMapWidth); });
		if (!colorMapStore.empty())
		{
			ColorMap = colorMapStore.data();

			numTextures = jpgColorMapWidth / COLOR_MAP_TEXTURE_SIZE;
			numTexturesAcross = numTextures;
			fractionPerTexture = 1.0f / numTextures;

			float checkNum = static_cast<float>(jpgColorMapWidth) / static_cast<float>(COLOR_MAP_TEXTURE_SIZE);
			if (checkNum != static_cast<float>(numTextures))
				{
					mc2::content::error(mc2::content::Kind::Texture, static_cast<const char*>(colorMapName), "colormap tile size mismatch; flat fallback");
				return flatFallback();
				}

			if (skipColormapTiles()) {
				mc2::fmt::print_out("[COLORMAP] skip {:d}^2 tile GL upload ({:d}x{:d}) [COLORMAP-TILES-RETIRE-1]\n", mc2::fmt::as_i32(static_cast<long>(numTexturesAcross)), mc2::fmt::as_i32(jpgColorMapWidth), mc2::fmt::as_i32(jpgColorMapWidth));
				numTextures = 0;
			} else {
				numTextures *= numTextures;
				ownTextureHandles(numTextures);
				ownTileRam(numTextures);

				//Now, divide up the color map into separate COLOR_MAP_TEXTURE_SIZE textures.
				// and hand the data to the textureManager.
				{
					ZoneScopedN("TerrainColorMap::init jpgColorMap tileLoop");
					for (unsigned long i=0;i<numTextures;i++)
					{
						{
							ZoneScopedN("TerrainColorMap::init jpgColorMap tileExtract");
							getColorMapData(txmRAM[i].ourRAM,i, jpgColorMapWidth);
						}
						{
							ZoneScopedN("TerrainColorMap::init jpgColorMap tileCache");
							textures[i].mcTextureNodeIndex.reset(mc2::textures::fromRawPixels(std::bit_cast<DWORD *>(txmRAM[i].ourRAM),gos_Texture_Solid,gosHint_DontShrink,COLOR_MAP_TEXTURE_SIZE));
							mc2::textures::setSessionLifetime(textures[i].mcTextureNodeIndex.get(), 0x1);
						}
					}
				}
			}

			colorMapStarted = true;

			// Retain full-atlas CPU copy for the indirect terrain draw bridge
			// (same as the TGA path below — required for terrain displacement
			// HSV classification + colormap atlas upload).
			if (ColorMap) {
				if (cpuColorMap) { free(cpuColorMap); cpuColorMap = nullptr; }
				long pixels = jpgColorMapWidth * jpgColorMapWidth;
				cpuColorMap = static_cast<unsigned char*>(malloc(pixels * 4));
				gosASSERT(cpuColorMap != nullptr);
				cpuColorMapSize = jpgColorMapWidth;
				memcpy(cpuColorMap, ColorMap, pixels * 4);
				mc2::fmt::print_out("[COLORMAP] retained colormap on CPU ({:d}x{:d}) [jpg]\n", mc2::fmt::as_i32(jpgColorMapWidth), mc2::fmt::as_i32(jpgColorMapWidth));
			}
			// colorMapStore owns the decoded sidecar; the cleanup below releases it with the rest of the scratch.
		}
		else
		{
			bool burnedIn = false;
			std::vector<unsigned char> tgaFileImage;
			TGAFileHeader colorMapInfo{};

			if (tgaFileImage.empty())
			{
				File colorMapFile;
				result = colorMapFile.open(burnInName);
				if (result != NO_ERR)
				{
					result = colorMapFile.open(colorMapName);
					if (result != NO_ERR)
				{
					mc2::content::error(mc2::content::Kind::Texture, static_cast<const char*>(colorMapName), "missing colormap; flat fallback");
					return flatFallback();
				}
				}
				else
				{
					burnedIn = true;
				}

				ZoneScopedN("TerrainColorMap::init colorMap read");
				if (!loadTga(colorMapFile, tgaFileImage, colorMapInfo, "colour map"))
				return flatFallback();
			}


			if (colorMapInfo.image_type == UNC_TRUE)
			{
				MemoryPtr loadBuffer = tgaFileImage.data() + sizeof(TGAFileHeader);
				if (colorMapInfo.width != colorMapInfo.height)
				{
					mc2::content::error(mc2::content::Kind::Texture, static_cast<const char*>(colorMapName), "colormap not square; flat fallback");
					return flatFallback();
				}
				if (!tgaPixelsPresent(tgaFileImage, colorMapInfo, "colour map"))
					return flatFallback();

				//-----------------------------------------------------------------
				// Check if 24 or 32 bit.  If 24, do the necessary stuff to it.
				long cmapBytes = static_cast<long>(colorMapInfo.width) * static_cast<long>(colorMapInfo.width) * static_cast<long>(sizeof(DWORD));
				ownColorMap(static_cast<size_t>(cmapBytes));

				{ ZoneScopedN("TerrainColorMap::init colorMap decode");
				if (colorMapInfo.pixel_depth == 24)
				{
					//24-Bit color means we must add in an opaque alpha to each 24 bits of color data.
					MemoryPtr cMap = ColorMap;
					MemoryPtr lMap = loadBuffer;
					for (long i = 0;i<(colorMapInfo.width * colorMapInfo.width);i++)
					{
						*cMap = *lMap;		//Red
						cMap++;
						lMap++;

						*cMap = *lMap;		//Green
						cMap++;
						lMap++;

						*cMap = *lMap;		//Blue
						cMap++;
						lMap++;

						*cMap = 0xff;		 //Alpha
						cMap++;
					}
				}
				else
				{
					//32-bit color means all we have to do is copy the buffer.
					memcpy(ColorMap,loadBuffer,colorMapInfo.width * colorMapInfo.width * sizeof(DWORD));
				}
				}


				numTextures = colorMapInfo.width / COLOR_MAP_TEXTURE_SIZE;
				numTexturesAcross = numTextures;
				fractionPerTexture = 1.0f / numTextures;

				float checkNum = static_cast<float>(colorMapInfo.width) / static_cast<float>(COLOR_MAP_TEXTURE_SIZE);
				if (checkNum != static_cast<float>(numTextures))
					{
						mc2::content::error(mc2::content::Kind::Texture, static_cast<const char*>(colorMapName), "colormap tile size mismatch; flat fallback");
					return flatFallback();
					}

				//------------------------------------------------------------------------
				// Must check image_descriptor to see if we need to un upside down image.
				//bool left = (colorMapInfo.iimage_descriptor & 16) != 0;
				bool top = (colorMapInfo.image_descriptor & 32) != 0;

				if (!top)
				{
					{ ZoneScopedN("TerrainColorMap::init colorMap flip"); flipTopToBottom(ColorMap,32,colorMapInfo.width,colorMapInfo.height); }
				}

				//Apply shadow map.  calc every time for now.  Save as in editor, eventually.
				if (!burnedIn)
				{
					{ ZoneScopedN("TerrainColorMap::init burnInShadows"); burnInShadows(true,fileName); }
					saveTGAFile(ColorMap,fileName,colorMapInfo.width);
				}

				if (skipColormapTiles()) {
					mc2::fmt::print_out("[COLORMAP] skip {:d}^2 tile GL upload ({:d}x{:d}) [COLORMAP-TILES-RETIRE-1]\n", mc2::fmt::as_i32(static_cast<long>(numTexturesAcross)), mc2::fmt::as_i32(colorMapInfo.width), mc2::fmt::as_i32(colorMapInfo.width));
					numTextures = 0;
				} else {
					numTextures *= numTextures;
					ownTextureHandles(numTextures);
					ownTileRam(numTextures);

					//Now, divide up the color map into separate COLOR_MAP_TEXTURE_SIZE textures.
					// and hand the data to the textureManager.

					{
						ZoneScopedN("TerrainColorMap::init colorMap tileLoop");
						for (unsigned long i=0;i<numTextures;i++)
						{
							{
								ZoneScopedN("TerrainColorMap::init colorMap tileExtract");
								getColorMapData(txmRAM[i].ourRAM,i, colorMapInfo.width);
							}
							{
								ZoneScopedN("TerrainColorMap::init colorMap tileCache");
								textures[i].mcTextureNodeIndex.reset(mc2::textures::fromRawPixels(std::bit_cast<DWORD *>(txmRAM[i].ourRAM),gos_Texture_Solid,gosHint_DontShrink,COLOR_MAP_TEXTURE_SIZE));
								mc2::textures::setSessionLifetime(textures[i].mcTextureNodeIndex.get(), 0x1);
							}
							if (i % 50 == 0)
							{
							}
						}
					}
				}
			}

			colorMapStarted = true;

			// Retain full-atlas CPU copy for the indirect terrain draw bridge.
			// cpuColorMap is uploaded once per mission as a single GL_TEXTURE_2D
			// by BuildColormapAtlas() (gos_terrain_indirect.cpp). Must capture
			// here (inside the TGA scope where colorMapInfo is in scope and
			// ColorMap is still valid) before the cleanup block frees ColorMap.
			if (colorMapInfo.image_type == UNC_TRUE && ColorMap) {
				if (cpuColorMap) { free(cpuColorMap); cpuColorMap = nullptr; }
				long pixels = static_cast<long>(colorMapInfo.width) * static_cast<long>(colorMapInfo.width);
				cpuColorMap = static_cast<unsigned char*>(malloc(pixels * 4));
				gosASSERT(cpuColorMap != nullptr);
				cpuColorMapSize = colorMapInfo.width;
				memcpy(cpuColorMap, ColorMap, pixels * 4);
				mc2::fmt::print_out("[COLORMAP] retained colormap on CPU ({:d}x{:d})\n", mc2::fmt::as_i32(colorMapInfo.width), mc2::fmt::as_i32(colorMapInfo.width));
			}
		}
	}

	// COLORMAP-BC7-KTX2-1: probe for .burnin.ktx2 sidecar (same as recalcLight path).
	ktx2ColormapPath[0] = '\0';
	if (cpuColorMap && useKtx2Colormap()) {
		char ktx2Name[1024];
		mc2::fmt::to_buf(ktx2Name, "{}.burnin", mc2::fmt::safe(fileName));
		FullPathFileName ktx2Path;
		ktx2Path.init(texturePath, ktx2Name, ".ktx2");
		FILE* probe = std::fopen(static_cast<const char*>(ktx2Path), "rb");
		if (probe) {
			fclose(probe);
			mc2::fmt::to_buf_n(ktx2ColormapPath, sizeof(ktx2ColormapPath), "{}", mc2::fmt::safe(static_cast<const char*>(ktx2Path)));
			mc2::fmt::print_out("[COLORMAP] COLORMAP-BC7-KTX2-1: found sidecar {}\n", mc2::fmt::safe(ktx2ColormapPath));
		}
	}

	//At this point, the color Map DATA should be freeable!!
	// All of the textures have been passed to the mc2::textures::ready()!
	{
		ZoneScopedN("TerrainColorMap::init colorMap cleanup");
		releaseColorMapScratch();
	}

	//----------------------------------------------------------------------
	// Load normal map if it exists (for terrain normal-mapped lighting)
	{
		char *nmFileName = Terrain::colorMapName ? Terrain::colorMapName : Terrain::terrainName;
		if (!nmFileName) nmFileName = fileName;
		char nmName[1024];
		mc2::fmt::to_buf(nmName, "{}.normalmap", mc2::fmt::safe(nmFileName));
		FullPathFileName normalMapPath;
		normalMapPath.init(texturePath, nmName, ".tga");

		if (fileExists(normalMapPath))
		{
			ZoneScopedN("TerrainColorMap::init normalMap");
			File nmFile;
			if (nmFile.open(normalMapPath) == NO_ERR)
			{
				std::vector<unsigned char> nmData;
				TGAFileHeader nmInfo{};
				bool nmLoaded = false;
				{
					ZoneScopedN("TerrainColorMap::init normalMap read");
					nmLoaded = loadTga(nmFile, nmData, nmInfo, "normal map");
				}

			if (nmLoaded && nmInfo.image_type == UNC_TRUE && nmInfo.width == nmInfo.height && tgaPixelsPresent(nmData, nmInfo, "normal map"))
				{
					DWORD nmTilesAcross = nmInfo.width / COLOR_MAP_TEXTURE_SIZE;
					numNormalMapTextures = nmTilesAcross * nmTilesAcross;

					long nmHeapSize = static_cast<long>(nmInfo.width) * static_cast<long>(nmInfo.width) * static_cast<long>(sizeof(DWORD));
					std::vector<unsigned char> nmColorMapStore(static_cast<size_t>(nmHeapSize));
					MemoryPtr nmColorMap = nmColorMapStore.data();

					// Load pixel data (24 or 32 bit)
					MemoryPtr loadBuf = nmData.data() + sizeof(TGAFileHeader);
					{
						ZoneScopedN("TerrainColorMap::init normalMap decode");
						if (nmInfo.pixel_depth == 24)
						{
							MemoryPtr cMap = nmColorMap;
							MemoryPtr lMap = loadBuf;
							for (long i = 0; i < static_cast<long>(nmInfo.width) * static_cast<long>(nmInfo.width); i++)
							{
								*cMap++ = *lMap++; *cMap++ = *lMap++; *cMap++ = *lMap++; *cMap++ = 0xff;
							}
						}
						else
						{
							memcpy(nmColorMap, loadBuf, static_cast<long>(nmInfo.width) * static_cast<long>(nmInfo.width) * sizeof(DWORD));
						}
					}

					// Check if we need to flip
					bool top = (nmInfo.image_descriptor & 32) != 0;
					if (!top)
						flipTopToBottom(nmColorMap, 32, nmInfo.width, nmInfo.height);


					// Allocate texture handles (they outlive the colour map scratch)
					{
						ZoneScopedN("TerrainColorMap::init normalMap allocHandles");
						normalMapStore = std::vector<ColorMapTextures>(numNormalMapTextures);
						normalMapTextures = normalMapStore.data();
					}

					// Slice and upload tiles (same grid as colormap)
					std::vector<unsigned char> nmTileStore;
					MemoryPtr tileRAM = nullptr;
					{
						ZoneScopedN("TerrainColorMap::init normalMap allocTileBuffer");
						nmTileStore.assign(sizeof(DWORD) * COLOR_MAP_TEXTURE_SIZE * COLOR_MAP_TEXTURE_SIZE, 0);
						tileRAM = nmTileStore.data();
					}
					{ ZoneScopedN("TerrainColorMap::init normalMap tileLoop");
					for (DWORD i = 0; i < numNormalMapTextures; i++)
					{
						// Use same tile extraction as colormap
						long numWide = nmInfo.width / COLOR_MAP_TEXTURE_SIZE;
						long startCol = ((i % numWide) * COLOR_MAP_TEXTURE_SIZE);
						long startRow = ((i / numWide) * nmInfo.width * COLOR_MAP_TEXTURE_SIZE);
						if (startCol > 0) startCol -= 1;
						if (startRow > 0) startRow -= nmInfo.width;

						{
							ZoneScopedN("TerrainColorMap::init normalMap tileExtract");
							MemoryPtr src = nmColorMap + (startCol + startRow) * sizeof(DWORD);
							MemoryPtr dst = tileRAM;
							for (long row = 0; row < COLOR_MAP_TEXTURE_SIZE; row++)
							{
								memcpy(dst, src, COLOR_MAP_TEXTURE_SIZE * sizeof(DWORD));
								dst += COLOR_MAP_TEXTURE_SIZE * sizeof(DWORD);
								src += nmInfo.width * sizeof(DWORD);
							}
						}

						{
							ZoneScopedN("TerrainColorMap::init normalMap tileCache");
							normalMapTextures[i].mcTextureNodeIndex.reset(mc2::textures::fromRawPixels(
								std::bit_cast<DWORD *>(tileRAM), gos_Texture_Solid, gosHint_DontShrink, COLOR_MAP_TEXTURE_SIZE));
							mc2::textures::setSessionLifetime(normalMapTextures[i].mcTextureNodeIndex.get(), 0x1);
						}
					}
					}

					hasNormalMap = true;
				}
			}
		}


	}

	// Load terrain material texture arrays (4 PBR material sets)
	mc2::fmt::print_out("[SPLATTING] starting material array load\n");
	{
		ZoneScopedN("TerrainColorMap::init materialArrays");
		const char* normalNames[9] = {
			"mat0_normal", "mat1_normal", "mat2_normal", "mat3_normal", "mat4_normal",
			"mat5_normal", "mat6_normal", "mat7_normal", "mat8_normal"
		};
		const unsigned char* normalLayers[9] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
		// The decoded layers; the pointers above view these until the block ends.
		std::vector<unsigned char> normalStore[9];
		int arrayWidth = 0;
		bool usedCooked = false;
		if (auto* fields = gos_TerrainFieldStore()) {
			std::vector<RenderCore::KtxImage> cooked(6);
			FullPathFileName grassPath;
			grassPath.init(texturePath, normalNames[1], ".tga");
			bool turf = false;
			usedCooked = mc2::terrain::loadCookedNormals(std::span(cooked), LookupModOwner(grassPath) != nullptr,
				[&](int mat, bool wantTurf) {
					FullPathFileName nmPath;
					nmPath.init(texturePath, normalNames[mat], ".tga");
					auto layer = mc2::textures::loadCookedSidecar(wantTurf ? mc2::terrain::kGrassTurfSource : static_cast<const char*>(nmPath));
					if (mat == 1) turf = wantTurf && layer.has_value();
					return layer;
				},
				[&](std::span<const RenderCore::KtxImage> layers) { return fields->setMaterialNormals(layers); });
			if (!usedCooked) fields->setMaterialNormals({});
			mc2::fmt::print_out("[SPLATTING] cooked detail normals: {}{}\n", usedCooked ? "cooked sidecars" : "absent, TGA import",
				usedCooked && turf ? ", grass layer = grass_turf_normal" : "");
		}

		const bool allLoaded = mc2::terrain::loadNormalSources(usedCooked, [&](int mat)
		{
			FullPathFileName nmPath;
			nmPath.init(texturePath, normalNames[mat], ".tga");
			mc2::fmt::print_out("[SPLATTING] checking: {}\n", mc2::fmt::safe(static_cast<const char*>(nmPath)));
			if (!fileExists(nmPath)) {
				mc2::fmt::print_out("[SPLATTING] NOT FOUND: {}{}\n", mc2::fmt::safe(static_cast<const char*>(nmPath)), mc2::fmt::safe(mat >= 4 ? " (optional slot skipped)" : ""));
				// mat4+ are optional — missing file leaves the slot empty but doesn't fail the load.
				return mat >= 4;
			}

			File nmFile;
			if (nmFile.open(nmPath) != NO_ERR) return mat >= 4;
			std::vector<unsigned char> nmData;
			TGAFileHeader nmInfo{};
			if (!loadTga(nmFile, nmData, nmInfo, "terrain material normal map"))
			{ return false; }
			if (nmInfo.image_type != UNC_TRUE || nmInfo.width != nmInfo.height || !tgaPixelsPresent(nmData, nmInfo, "terrain material normal map"))
			{ return false; }

			if (arrayWidth == 0) arrayWidth = nmInfo.width;
			else if (nmInfo.width != arrayWidth)
			{ return false; }

			long pixels = static_cast<long>(nmInfo.width) * static_cast<long>(nmInfo.width);
			if (!mc2::tga::expandPixels(nmData, sizeof(TGAFileHeader), static_cast<size_t>(pixels), nmInfo.pixel_depth))
			{ return false; }
			normalStore[mat] = std::move(nmData);
			MemoryPtr rgba = normalStore[mat].data() + sizeof(TGAFileHeader);
			bool top = (nmInfo.image_descriptor & 32) != 0;
			if (!top) flipTopToBottom(reinterpret_cast<MemoryPtr>(rgba), 32, nmInfo.width, nmInfo.height);
			normalLayers[mat] = reinterpret_cast<const unsigned char*>(rgba);
			return true;
		});

		if (allLoaded && !usedCooked) {
			mc2::fmt::print_out("[SPLATTING] all loaded OK, width={:d}, creating individual textures\n", mc2::fmt::as_i32(arrayWidth));
			for (int i = 0; i < 9; i++) {
				if (!normalLayers[i]) continue;  // optional slot absent
				const DWORD nmId = gos_NewTerrainNormalTexture(normalLayers[i], arrayWidth);
				mc2::fmt::print_out("[SPLATTING] matNormal{:d} handle={:d}\n", mc2::fmt::as_i32(i), mc2::fmt::as_u32(nmId));
				gos_SetTerrainMaterialNormal(i, nmId);
			}
		} else if (!allLoaded) {
			mc2::fmt::print_out("[SPLATTING] FAILED to load all material textures\n");
		}

		// Retain CPU copy of matNormal2 alpha for terrain elevation displacement
		if (normalLayers[2]) {
			if (cpuDispAlpha) { free(cpuDispAlpha); cpuDispAlpha = nullptr; }
			long pixels = static_cast<long>(arrayWidth) * static_cast<long>(arrayWidth);
			cpuDispAlpha = static_cast<unsigned char*>(malloc(pixels));
			gosASSERT(cpuDispAlpha != nullptr);
			cpuDispAlphaSize = arrayWidth;
			const unsigned char* src = normalLayers[2];
			mc2::terrain::copyDisplacementAlpha({src, static_cast<size_t>(pixels) * 4},
			                                   {cpuDispAlpha, static_cast<size_t>(pixels)});
			mc2::fmt::print_out("[SPLATTING] retained matNormal2 alpha on CPU ({:d}x{:d})\n", mc2::fmt::as_i32(arrayWidth), mc2::fmt::as_i32(arrayWidth));
		}

	}

	// ROAD-PBR-ABANDON-1: asphalt_albedo.tga / gravel_albedo.tga loaders removed —
	// the PBR/normal-layer road path is retired (USER RULING 2026-07-18).

	// TODO: noise and random textures for cell bombing (add back after splatting works)

	return 0;
}
