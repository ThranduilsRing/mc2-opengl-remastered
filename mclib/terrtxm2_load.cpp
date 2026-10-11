// TerrainColorMap: getScaledColorMap and TerrainColorMap::init(fileName): the mission colormap load.
// Declared in the class headers; shared names in terrtxm2_internal.h.
#include "terrtxm2_internal.h"
#include "mc2fmt.h"


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

		colorMapRAMHeap = new UserHeap;
		colorMapRAMHeap->init(COLOR_MAP_HEAP_SIZE,"ColorMap");

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
				return;
			}
		}
			
		MemoryPtr tgaFileImage = (MemoryPtr)malloc(colorMapFile.fileSize());
		gosASSERT(tgaFileImage != nullptr);
		
		colorMapFile.read(tgaFileImage,colorMapFile.fileSize());
		
		TGAFileHeader colorMapInfo;
		memcpy(&colorMapInfo,tgaFileImage,sizeof(TGAFileHeader));
		
		if (colorMapInfo.image_type == UNC_TRUE)
		{
			MemoryPtr loadBuffer = tgaFileImage + sizeof(TGAFileHeader);
			if (colorMapInfo.width != colorMapInfo.height)
				STOP(("Color Map is not a perfect Square"));
			
			//-----------------------------------------------------------------
			// Check if 24 or 32 bit.  If 24, do the necessary stuff to it.
			ColorMap = (MemoryPtr)colorMapRAMHeap->Malloc(colorMapInfo.width * colorMapInfo.width * sizeof(DWORD));
			gosASSERT(ColorMap != nullptr);
			
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
			
			free(tgaFileImage);
			tgaFileImage = nullptr;
			
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
	float xSkip = cMapWidth / float(dWidth);
	float ySkip = xSkip;

	long xOffset = 0, yOffset = 0;

	DWORD *currentColor = (DWORD *)ColorMap;
	DWORD *currentBfr = (DWORD *)bfr;
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
		currentColor = ((DWORD *)ColorMap) + long(xOffset * xSkip) + (long(yOffset*ySkip) * (long)cMapWidth);
	}

	if (!wasColorMapAround)
	{
		delete colorMapRAMHeap;
		colorMapRAMHeap = nullptr;

		ColorMap = nullptr;
	}
}

static long sReadIdFloat(FitIniFile* missionFile, const char *varName, float &value) {
	long result = 0;
	float tmpFloat;
	result = missionFile->readIdFloat((char *)varName, tmpFloat);
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
	bool usedJPG = false;
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
			sprintf(dName,"defaults" PATH_SEPARATOR "default_detail");
			detailFile.init(texturePath,dName,".tga");
		}
		if (fileExists(detailFile))		//Otherwise, its already 0xffffffff!!
		{
			ZoneScopedN("TerrainColorMap::init detailTexture");
			detailTextureNodeIndex = mcTextureManager->loadTexture(detailFile,gos_Texture_Alpha,gosHint_DontShrink, 0, 0x1);
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
			sprintf(dName,"defaults" PATH_SEPARATOR "default_water");
			waterFile.init(texturePath,dName,".tga");
		}
		if (fileExists(waterFile))
		{
			ZoneScopedN("TerrainColorMap::init waterTexture");
			waterTextureNodeIndex = mcTextureManager->loadTexture(waterFile,gos_Texture_Solid,0, 0, 0x1);
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
				sprintf(waterDetailBaseName,"defaults" PATH_SEPARATOR "default_water");
			}
		}
		for (long i=0;i<MAX_WATER_DETAIL_TEXTURES;i++)
		{
			waterDetailNodeIndex[i] = 0xffffffff;
		}
		mc2::fmt::to_buf(dName, "{}{:04d}", mc2::fmt::safe(waterDetailBaseName), mc2::fmt::as_i32(0));
		waterFile.init(texturePath,dName,".tga");
		{ ZoneScopedN("TerrainColorMap::init waterDetailTextures"); resetWaterDetailTextures((const char *)waterFile); }

		{
			detailTextureTilingFactor = 30.0f;
			waterTextureTilingFactor = 48.0f;
			waterDetailTilingFactor = 45.0f;

			FullPathFileName missionFitFilePath;
			missionFitFilePath.init(missionPath,fileName,".fit");
			FitIniFile missionFitIni;
			if (fileExists(missionFitFilePath))
			{
				missionFitIni.open((char*)(const char*)missionFitFilePath);
				long result = missionFitIni.seekBlock( "Terrain" );
				gosASSERT( result == NO_ERR );
				result = sReadIdFloat(&missionFitIni, "DetailTextureTilingFactor", detailTextureTilingFactor);
				result = sReadIdFloat(&missionFitIni, "WaterTextureTilingFactor", waterTextureTilingFactor);
				result = sReadIdFloat(&missionFitIni, "WaterDetailTilingFactor", waterDetailTilingFactor);
				missionFitIni.close();
			}
		}

		colorMapHeap = new UserHeap;
		colorMapHeap->init(COLOR_TXM_HEAP_SIZE,"ColorTXM");

		colorMapRAMHeap = new UserHeap;
		colorMapRAMHeap->init(COLOR_MAP_HEAP_SIZE,"ColorMap");
		
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
		MemoryPtr jpgColorMap = tryLoadBurninJpg(texturePath, fileName, colorMapRAMHeap, &jpgColorMapWidth);
		if (jpgColorMap != nullptr)
		{
			ColorMap = jpgColorMap;

			numTextures = jpgColorMapWidth / COLOR_MAP_TEXTURE_SIZE;
			numTexturesAcross = numTextures;
			fractionPerTexture = 1.0f / numTextures;

			float checkNum = float(jpgColorMapWidth) / float(COLOR_MAP_TEXTURE_SIZE);
			if (checkNum != float(numTextures))
				STOP(("Color Map is %d pixels wide which is not even divisible by %d",jpgColorMapWidth,COLOR_MAP_TEXTURE_SIZE));

			if (skipColormapTiles()) {
				printf("[COLORMAP] skip %ld^2 tile GL upload (%ldx%ld) [COLORMAP-TILES-RETIRE-1]\n",
				       (long)numTexturesAcross, jpgColorMapWidth, jpgColorMapWidth);
				numTextures = 0;
			} else {
				numTextures *= numTextures;
				textures = (ColorMapTextures *)colorMapHeap->Malloc(sizeof(ColorMapTextures) * numTextures);
				gosASSERT(textures != nullptr);

				txmRAM = (ColorMapRAM *)colorMapRAMHeap->Malloc(sizeof(ColorMapRAM) * numTextures);
				gosASSERT(txmRAM != nullptr);

				//Now, divide up the color map into separate COLOR_MAP_TEXTURE_SIZE textures.
				// and hand the data to the textureManager.
				{
					ZoneScopedN("TerrainColorMap::init jpgColorMap tileLoop");
					for (unsigned long i=0;i<numTextures;i++)
					{
						{
							ZoneScopedN("TerrainColorMap::init jpgColorMap tileAlloc");
							txmRAM[i].ourRAM = (MemoryPtr)colorMapRAMHeap->Malloc(sizeof(DWORD) * COLOR_MAP_TEXTURE_SIZE * COLOR_MAP_TEXTURE_SIZE);
							gosASSERT(txmRAM[i].ourRAM != nullptr);
						}
						{
							ZoneScopedN("TerrainColorMap::init jpgColorMap tileExtract");
							getColorMapData(txmRAM[i].ourRAM,i, jpgColorMapWidth);
						}
						{
							ZoneScopedN("TerrainColorMap::init jpgColorMap tileCache");
							textures[i].mcTextureNodeIndex = mcTextureManager->textureFromMemoryRaw((DWORD *)txmRAM[i].ourRAM,gos_Texture_Solid,gosHint_DontShrink,COLOR_MAP_TEXTURE_SIZE);
							mcTextureManager->setTextureNeverFlush(textures[i].mcTextureNodeIndex, 0x1);
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
				cpuColorMap = (unsigned char*)malloc(pixels * 4);
				gosASSERT(cpuColorMap != nullptr);
				cpuColorMapSize = jpgColorMapWidth;
				memcpy(cpuColorMap, ColorMap, pixels * 4);
				printf("[COLORMAP] retained colormap on CPU (%ldx%ld) [jpg]\n", jpgColorMapWidth, jpgColorMapWidth);
			}
			// ColorMap is colorMapRAMHeap-allocated -> normal cleanup path (usedJPG stays false).
		}
		else
		{
			bool burnedIn = false;
			MemoryPtr tgaFileImage = nullptr;

			if (tgaFileImage == nullptr)
			{
				File colorMapFile;
				result = colorMapFile.open(burnInName);
				if (result != NO_ERR)
				{
					result = colorMapFile.open(colorMapName);
					if (result != NO_ERR)
						STOP(("Unable to open Terrain Color Map %s", (const char*)colorMapName));
				}
				else
				{
					burnedIn = true;
				}

				ZoneScopedN("TerrainColorMap::init colorMap read");
				tgaFileImage = (MemoryPtr)malloc(colorMapFile.fileSize());
				gosASSERT(tgaFileImage != nullptr);
				colorMapFile.read(tgaFileImage,colorMapFile.fileSize());
			}

			TGAFileHeader colorMapInfo;
			memcpy(&colorMapInfo,tgaFileImage,sizeof(TGAFileHeader));

			if (colorMapInfo.image_type == UNC_TRUE)
			{
				MemoryPtr loadBuffer = tgaFileImage + sizeof(TGAFileHeader);
				if (colorMapInfo.width != colorMapInfo.height)
					STOP(("Color Map is not a perfect Square"));

				//-----------------------------------------------------------------
				// Check if 24 or 32 bit.  If 24, do the necessary stuff to it.
				long cmapBytes = (long)colorMapInfo.width * (long)colorMapInfo.width * (long)sizeof(DWORD);
				ColorMap = (MemoryPtr)colorMapRAMHeap->Malloc(cmapBytes);
				gosASSERT(ColorMap != nullptr);

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

				free(tgaFileImage);
				tgaFileImage = nullptr;

				numTextures = colorMapInfo.width / COLOR_MAP_TEXTURE_SIZE;
				numTexturesAcross = numTextures;
				fractionPerTexture = 1.0f / numTextures;

				float checkNum = float(colorMapInfo.width) / float(COLOR_MAP_TEXTURE_SIZE);
				if (checkNum != float(numTextures))
					STOP(("Color Map is %d pixels wide which is not even divisible by %d",colorMapInfo.width,COLOR_MAP_TEXTURE_SIZE));

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
					printf("[COLORMAP] skip %ld^2 tile GL upload (%dx%d) [COLORMAP-TILES-RETIRE-1]\n",
					       (long)numTexturesAcross, colorMapInfo.width, colorMapInfo.width);
					numTextures = 0;
				} else {
					numTextures *= numTextures;
					textures = (ColorMapTextures *)colorMapHeap->Malloc(sizeof(ColorMapTextures) * numTextures);
					gosASSERT(textures != nullptr);

					txmRAM = (ColorMapRAM *)colorMapRAMHeap->Malloc(sizeof(ColorMapRAM) * numTextures);
					gosASSERT(txmRAM != nullptr);

					//Now, divide up the color map into separate COLOR_MAP_TEXTURE_SIZE textures.
					// and hand the data to the textureManager.

					{
						ZoneScopedN("TerrainColorMap::init colorMap tileLoop");
						for (unsigned long i=0;i<numTextures;i++)
						{
							{
								ZoneScopedN("TerrainColorMap::init colorMap tileAlloc");
								txmRAM[i].ourRAM = (MemoryPtr)colorMapRAMHeap->Malloc(sizeof(DWORD) * COLOR_MAP_TEXTURE_SIZE * COLOR_MAP_TEXTURE_SIZE);
								gosASSERT(txmRAM[i].ourRAM != nullptr);
							}
							{
								ZoneScopedN("TerrainColorMap::init colorMap tileExtract");
								getColorMapData(txmRAM[i].ourRAM,i, colorMapInfo.width);
							}
							{
								ZoneScopedN("TerrainColorMap::init colorMap tileCache");
								textures[i].mcTextureNodeIndex = mcTextureManager->textureFromMemoryRaw((DWORD *)txmRAM[i].ourRAM,gos_Texture_Solid,gosHint_DontShrink,COLOR_MAP_TEXTURE_SIZE);
								mcTextureManager->setTextureNeverFlush(textures[i].mcTextureNodeIndex, 0x1);
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
				long pixels = (long)colorMapInfo.width * (long)colorMapInfo.width;
				cpuColorMap = (unsigned char*)malloc(pixels * 4);
				gosASSERT(cpuColorMap != nullptr);
				cpuColorMapSize = colorMapInfo.width;
				memcpy(cpuColorMap, ColorMap, pixels * 4);
				printf("[COLORMAP] retained colormap on CPU (%dx%d)\n", colorMapInfo.width, colorMapInfo.width);
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
		FILE* probe = std::fopen((const char*)ktx2Path, "rb");
		if (probe) {
			fclose(probe);
			mc2::fmt::to_buf_n(ktx2ColormapPath, sizeof(ktx2ColormapPath), "{}", mc2::fmt::safe((const char*)ktx2Path));
			printf("[COLORMAP] COLORMAP-BC7-KTX2-1: found sidecar %s\n", ktx2ColormapPath);
		}
	}

	//At this point, the color Map DATA should be freeable!!
	// All of the textures have been passed to the mcTextureManager!
	{
		ZoneScopedN("TerrainColorMap::init colorMap cleanup");
		if (txmRAM) {
		for (unsigned long i=0;i<numTextures;i++)
		{
			colorMapRAMHeap->Free(txmRAM[i].ourRAM);
			txmRAM[i].ourRAM = nullptr;
		}

		colorMapRAMHeap->Free(txmRAM);
		txmRAM = nullptr;
		}
		
		if (usedJPG)
		{
			gos_Free(ColorMap);
			ColorMap = nullptr;
		}
		else
		{
			colorMapRAMHeap->Free(ColorMap);
			ColorMap = nullptr;
		}
		
		if (colorMapRAMHeap)
		{
			delete colorMapRAMHeap;
			colorMapRAMHeap = nullptr;
		}
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
				MemoryPtr nmData = nullptr;
				{
					ZoneScopedN("TerrainColorMap::init normalMap read");
					nmData = (MemoryPtr)malloc(nmFile.fileSize());
					nmFile.read(nmData, nmFile.fileSize());
				}

				TGAFileHeader nmInfo;
				memcpy(&nmInfo, nmData, sizeof(TGAFileHeader));

			if (nmInfo.image_type == UNC_TRUE && nmInfo.width == nmInfo.height)
				{
					DWORD nmTilesAcross = nmInfo.width / COLOR_MAP_TEXTURE_SIZE;
					numNormalMapTextures = nmTilesAcross * nmTilesAcross;

					long nmHeapSize = (long)nmInfo.width * (long)nmInfo.width * (long)sizeof(DWORD);
					MemoryPtr nmColorMap = (MemoryPtr)malloc(nmHeapSize);

					// Load pixel data (24 or 32 bit)
					MemoryPtr loadBuf = nmData + sizeof(TGAFileHeader);
					{
						ZoneScopedN("TerrainColorMap::init normalMap decode");
						if (nmInfo.pixel_depth == 24)
						{
							MemoryPtr cMap = nmColorMap;
							MemoryPtr lMap = loadBuf;
							for (long i = 0; i < (long)nmInfo.width * (long)nmInfo.width; i++)
							{
								*cMap++ = *lMap++; *cMap++ = *lMap++; *cMap++ = *lMap++; *cMap++ = 0xff;
							}
						}
						else
						{
							memcpy(nmColorMap, loadBuf, (long)nmInfo.width * (long)nmInfo.width * sizeof(DWORD));
						}
					}

					// Check if we need to flip
					bool top = (nmInfo.image_descriptor & 32) != 0;
					if (!top)
						flipTopToBottom(nmColorMap, 32, nmInfo.width, nmInfo.height);


					// Allocate texture handles (use malloc, not colorMapHeap which is freed)
					{
						ZoneScopedN("TerrainColorMap::init normalMap allocHandles");
						normalMapTextures = (ColorMapTextures *)malloc(sizeof(ColorMapTextures) * numNormalMapTextures);
					}

					// Slice and upload tiles (same grid as colormap)
					MemoryPtr tileRAM = nullptr;
					{
						ZoneScopedN("TerrainColorMap::init normalMap allocTileBuffer");
						tileRAM = (MemoryPtr)malloc(sizeof(DWORD) * COLOR_MAP_TEXTURE_SIZE * COLOR_MAP_TEXTURE_SIZE);
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
							normalMapTextures[i].mcTextureNodeIndex = mcTextureManager->textureFromMemoryRaw(
								(DWORD *)tileRAM, gos_Texture_Solid, gosHint_DontShrink, COLOR_MAP_TEXTURE_SIZE);
							mcTextureManager->setTextureNeverFlush(normalMapTextures[i].mcTextureNodeIndex, 0x1);
						}
					}
					}

					hasNormalMap = true;
					free(tileRAM);
					free(nmColorMap);
				}
				free(nmData);
			}
		}

		// Load detail normal map (shared tiling texture)
		FullPathFileName detailNmPath;
		detailNmPath.init(texturePath, "detail_normal", ".tga");
		if (fileExists(detailNmPath))
		{
			// Load detail normal manually via textureFromMemory (loadTexture crashes during terrain init)
			File detNmFile;
			if (detNmFile.open(detailNmPath) == NO_ERR)
			{
				MemoryPtr detData = (MemoryPtr)malloc(detNmFile.fileSize());
				detNmFile.read(detData, detNmFile.fileSize());
				TGAFileHeader detInfo;
				memcpy(&detInfo, detData, sizeof(TGAFileHeader));
				if (detInfo.image_type == UNC_TRUE && detInfo.width == detInfo.height)
				{
					long detPixels = (long)detInfo.width * (long)detInfo.width;
					DWORD *detRGBA = (DWORD*)malloc(detPixels * sizeof(DWORD));
					MemoryPtr loadBuf = detData + sizeof(TGAFileHeader);
					if (detInfo.pixel_depth == 24)
					{
						MemoryPtr cMap = (MemoryPtr)detRGBA;
						MemoryPtr lMap = loadBuf;
						for (long i = 0; i < detPixels; i++)
						{
							*cMap++ = *lMap++; *cMap++ = *lMap++; *cMap++ = *lMap++; *cMap++ = 0xff;
						}
					}
					else
					{
						memcpy(detRGBA, loadBuf, detPixels * sizeof(DWORD));
					}
					bool top = (detInfo.image_descriptor & 32) != 0;
					if (!top)
						flipTopToBottom((MemoryPtr)detRGBA, 32, detInfo.width, detInfo.height);

					// Create GL texture directly (bypasses MC2 texture manager to avoid handle issues)
					unsigned int glTexId = gos_CreateTerrainNormalTexture((const unsigned char*)detRGBA, detInfo.width);
					gos_SetTerrainDetailNormalTexture(glTexId);
					free(detRGBA);
				}
				free(detData);
			}
		}
	}

	// Load detail displacement map (for POM - same tiling as detail normal)
	FullPathFileName detailDispPath;
	detailDispPath.init(texturePath, "detail_displacement", ".tga");
	if (fileExists(detailDispPath))
	{
		File detDispFile;
		if (detDispFile.open(detailDispPath) == NO_ERR)
		{
			MemoryPtr dispData = (MemoryPtr)malloc(detDispFile.fileSize());
			detDispFile.read(dispData, detDispFile.fileSize());
			TGAFileHeader dispInfo;
			memcpy(&dispInfo, dispData, sizeof(TGAFileHeader));
			if (dispInfo.image_type == UNC_TRUE && dispInfo.width == dispInfo.height)
			{
				long dispPixels = (long)dispInfo.width * (long)dispInfo.width;
				DWORD *dispRGBA = (DWORD*)malloc(dispPixels * sizeof(DWORD));
				MemoryPtr loadBuf = dispData + sizeof(TGAFileHeader);
				if (dispInfo.pixel_depth == 24)
				{
					MemoryPtr cMap = (MemoryPtr)dispRGBA;
					MemoryPtr lMap = loadBuf;
					for (long i = 0; i < dispPixels; i++)
					{
						*cMap++ = *lMap++; *cMap++ = *lMap++; *cMap++ = *lMap++; *cMap++ = 0xff;
					}
				}
				else
				{
					memcpy(dispRGBA, loadBuf, dispPixels * sizeof(DWORD));
				}
				bool top = (dispInfo.image_descriptor & 32) != 0;
				if (!top)
					flipTopToBottom((MemoryPtr)dispRGBA, 32, dispInfo.width, dispInfo.height);

				unsigned int glTexId = gos_CreateTerrainNormalTexture((const unsigned char*)dispRGBA, dispInfo.width);
				gos_SetTerrainDisplacementTexture(glTexId);
				free(dispRGBA);
			}
			free(dispData);
		}
	}

	// Load terrain material texture arrays (4 PBR material sets)
	printf("[SPLATTING] starting material array load\n");
	{
		ZoneScopedN("TerrainColorMap::init materialArrays");
		const char* normalNames[9] = {
			"mat0_normal", "mat1_normal", "mat2_normal", "mat3_normal", "mat4_normal",
			"mat5_normal", "mat6_normal", "mat7_normal", "mat8_normal"
		};
		const char* dispNames[9] = {
			"mat0_displacement", "mat1_displacement", "mat2_displacement", "mat3_displacement", "mat4_displacement",
			"mat5_displacement", "mat6_displacement", "mat7_displacement", "mat8_displacement"
		};
		const unsigned char* normalLayers[9] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
		const unsigned char* dispLayers[9] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
		int arrayWidth = 0;
		bool allLoaded = true;

		for (int mat = 0; mat < 9; mat++)
		{
			FullPathFileName nmPath;
			nmPath.init(texturePath, normalNames[mat], ".tga");
			printf("[SPLATTING] checking: %s\n", (const char*)nmPath);
			if (!fileExists(nmPath)) {
				printf("[SPLATTING] NOT FOUND: %s%s\n", (const char*)nmPath, mat >= 4 ? " (optional slot skipped)" : "");
				// mat4+ are optional — missing file leaves the slot empty but doesn't fail the load.
				if (mat >= 4) continue;
				allLoaded = false; break;
			}

			File nmFile;
			if (nmFile.open(nmPath) != NO_ERR) { if (mat >= 4) continue; allLoaded = false; break; }
			MemoryPtr nmData = (MemoryPtr)malloc(nmFile.fileSize());
			nmFile.read(nmData, nmFile.fileSize());
			TGAFileHeader nmInfo;
			memcpy(&nmInfo, nmData, sizeof(TGAFileHeader));

			if (nmInfo.image_type != UNC_TRUE || nmInfo.width != nmInfo.height)
			{ free(nmData); allLoaded = false; break; }

			if (arrayWidth == 0) arrayWidth = nmInfo.width;
			else if (nmInfo.width != arrayWidth)
			{ free(nmData); allLoaded = false; break; }

			long pixels = (long)nmInfo.width * (long)nmInfo.width;
			DWORD* rgba = (DWORD*)malloc(pixels * sizeof(DWORD));
			MemoryPtr loadBuf = nmData + sizeof(TGAFileHeader);
			if (nmInfo.pixel_depth == 24) {
				MemoryPtr cMap = (MemoryPtr)rgba;
				MemoryPtr lMap = loadBuf;
				for (long i = 0; i < pixels; i++)
				{ *cMap++ = *lMap++; *cMap++ = *lMap++; *cMap++ = *lMap++; *cMap++ = 0xff; }
			} else {
				memcpy(rgba, loadBuf, pixels * sizeof(DWORD));
			}
			bool top = (nmInfo.image_descriptor & 32) != 0;
			if (!top) flipTopToBottom((MemoryPtr)rgba, 32, nmInfo.width, nmInfo.height);
			normalLayers[mat] = (const unsigned char*)rgba;
			free(nmData);
		}

		// Load displacement layers (same pattern) — slot 4+ optional.
		if (allLoaded) {
			for (int mat = 0; mat < 9; mat++)
			{
				FullPathFileName dispPath;
				dispPath.init(texturePath, dispNames[mat], ".tga");
				if (!fileExists(dispPath)) { if (mat >= 4) continue; allLoaded = false; break; }

				File dispFile;
				if (dispFile.open(dispPath) != NO_ERR) { if (mat >= 4) continue; allLoaded = false; break; }
				MemoryPtr dispData = (MemoryPtr)malloc(dispFile.fileSize());
				dispFile.read(dispData, dispFile.fileSize());
				TGAFileHeader dispInfo;
				memcpy(&dispInfo, dispData, sizeof(TGAFileHeader));

				if (dispInfo.image_type != UNC_TRUE || dispInfo.width != arrayWidth)
				{ free(dispData); allLoaded = false; break; }

				long pixels = (long)dispInfo.width * (long)dispInfo.width;
				DWORD* rgba = (DWORD*)malloc(pixels * sizeof(DWORD));
				MemoryPtr loadBuf = dispData + sizeof(TGAFileHeader);
				if (dispInfo.pixel_depth == 24) {
					MemoryPtr cMap = (MemoryPtr)rgba;
					MemoryPtr lMap = loadBuf;
					for (long i = 0; i < pixels; i++)
					{ *cMap++ = *lMap++; *cMap++ = *lMap++; *cMap++ = *lMap++; *cMap++ = 0xff; }
				} else {
					memcpy(rgba, loadBuf, pixels * sizeof(DWORD));
				}
				bool top = (dispInfo.image_descriptor & 32) != 0;
				if (!top) flipTopToBottom((MemoryPtr)rgba, 32, dispInfo.width, dispInfo.height);
				dispLayers[mat] = (const unsigned char*)rgba;
				free(dispData);
			}
		}

		if (allLoaded) {
			printf("[SPLATTING] all loaded OK, width=%d, creating individual textures\n", arrayWidth);
			for (int i = 0; i < 9; i++) {
				if (!normalLayers[i]) continue;  // optional slot absent
				unsigned int nmId = gos_CreateTerrainNormalTexture(normalLayers[i], arrayWidth);
				printf("[SPLATTING] matNormal%d GL id=%u\n", i, nmId);
				gos_SetTerrainMaterialNormal(i, nmId);
			}
		} else {
			printf("[SPLATTING] FAILED to load all material textures\n");
		}

		// Retain CPU copy of matNormal2 alpha for terrain elevation displacement
		if (normalLayers[2]) {
			if (cpuDispAlpha) { free(cpuDispAlpha); cpuDispAlpha = nullptr; }
			long pixels = (long)arrayWidth * (long)arrayWidth;
			cpuDispAlpha = (unsigned char*)malloc(pixels);
			gosASSERT(cpuDispAlpha != nullptr);
			cpuDispAlphaSize = arrayWidth;
			const unsigned char* src = normalLayers[2];
			for (long i = 0; i < pixels; i++) {
				cpuDispAlpha[i] = src[i * 4 + 3]; // alpha channel (RGBA layout)
			}
			printf("[SPLATTING] retained matNormal2 alpha on CPU (%dx%d)\n", arrayWidth, arrayWidth);
		}

		for (int i = 0; i < 9; i++) {
			if (normalLayers[i]) free((void*)normalLayers[i]);
			if (dispLayers[i]) free((void*)dispLayers[i]);
		}
	}

	// ROAD-PBR-ABANDON-1: asphalt_albedo.tga / gravel_albedo.tga loaders removed —
	// the PBR/normal-layer road path is retired (USER RULING 2026-07-18).

	// TODO: noise and random textures for cell bombing (add back after splatting works)

	return 0;
}
