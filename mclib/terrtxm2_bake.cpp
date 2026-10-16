// TerrainColorMap: fractal base mesh, shadow burn-in, detail / water texture reset and light recalc, and the base texture reset.
// Declared in the class headers; shared names in terrtxm2_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "terrtxm2_internal.h"
#include "mc2fmt.h"
#include "tga_scratch.h"
#include "appearance_rng.h"

using mc2::tga::loadTga;
using mc2::tga::tgaPixelsPresent;


//---------------------------------------------------------------------------
inline void fractalPass (float *heightMap, long edgeSize, long THRESHOLD, long NOISE)
{
	//Add a bunch o Gaussian noise to the new hi-res height map to break up the lines.
	// This works as follows:
	// If there is a Height Difference between the points, Add noise based on difference.
	// If there is NO height difference, DO NOT ADD NOISE.  Let smooth be smooth!
	std::vector<float> noiseStore(static_cast<size_t>(edgeSize) * static_cast<size_t>(edgeSize));
	float *noiseMap = noiseStore.data();
	for (long y=THRESHOLD;y<(edgeSize-THRESHOLD);y++)
	{
		for (long x=THRESHOLD;x<(edgeSize-THRESHOLD);x++)
		{
			// Texture noise is render-only: it draws from the appearance stream so the bake, which a texture cache may skip,
			// never moves the stream the simulation rolls on.
			if (mc2_appearance_rng::randomNumber(THRESHOLD) == 0)
			{
				float noise = (NOISE * 0.5f) - mc2_appearance_rng::randomNumber(NOISE);
				noiseMap[x + (y * edgeSize)] = noise;
			}
			else
			{
				noiseMap[x + (y * edgeSize)] = 0.0f;
			}
		}
	}
	
	for (long i=0;i<edgeSize * edgeSize;i++)
	{
		heightMap[i] += noiseMap[i];
	}
	
}

//---------------------------------------------------------------------------
void rescaleMap (float *dst, float *src, long dstSize, long srcSize)
{
	Image source, dest;
	source.xSize = source.ySize = srcSize;
	source.data = src;
	
	dest.xSize = dest.ySize = dstSize;
	dest.data = dst;
	
//	zoom(&dest, &source, mitchellFilter, MitchellSupport);
	zoom(&dest, &source, triangleFilter, triangleSupport);
}

//---------------------------------------------------------------------------
void TerrainColorMap::refractalizeBaseMesh (const char *fileName, long Threshold, long Noise)
{
	//Find max and min vertex elevations for scaling below.
	float maxVertex = land->getTerrainElevation(0,0);
	float minVertex = maxVertex;
	for (long y=0;y<Terrain::realVerticesMapSide;y++)
	{
		for (long x=0;x<Terrain::realVerticesMapSide;x++)
		{
			float currentElevation = land->getTerrainElevation(y,x);
			if (currentElevation < minVertex)
				minVertex = currentElevation;
				
			if (currentElevation > maxVertex)
				maxVertex = currentElevation;
		}
	}
	
	float scalar = 0.0f;
	//Check if Map has no elevation!!
	if (maxVertex != minVertex)		
	{
		scalar = 255.0f / (maxVertex - minVertex); 
	}
		
	//Create an image which is x by x for the existing terrain.
	// Scale it into the range of 0 to 255.
	// these can be floats!!!!!!!
	std::vector<float> srcStore(static_cast<size_t>(Terrain::realVerticesMapSide) * static_cast<size_t>(Terrain::realVerticesMapSide));
	float *srcData = srcStore.data(); 
	
	for (int y=0;y<Terrain::realVerticesMapSide;y++)
	{
		for (long x=0;x<Terrain::realVerticesMapSide;x++)
		{
			float currentElevation = land->getTerrainElevation(y,x);
			float scaledValue = (currentElevation - minVertex) * scalar;
			srcData[x + (y * Terrain::realVerticesMapSide)] = scaledValue;
		}
	}
	
	//Spit out original Targa as well.
	std::vector<unsigned char> sourceStore(static_cast<size_t>(Terrain::realVerticesMapSide) * static_cast<size_t>(Terrain::realVerticesMapSide));
	MemoryPtr sourceData = sourceStore.data();
	
	float *tmpEdge = srcData;
	MemoryPtr tmpOutput = sourceData;
	for (long i=0;i<Terrain::realVerticesMapSide * Terrain::realVerticesMapSide;i++)
	{
		*tmpOutput = (BYTE)CLAMP(*tmpEdge,BLACK_PIXEL,WHITE_PIXEL);
		tmpOutput++;
		tmpEdge++;
	}

	FullPathFileName OEMheightName;
	OEMheightName.init(terrainPath,fileName,".tga");
	
	SetFileAttributes(OEMheightName,FILE_ATTRIBUTE_NORMAL);
 	
	TGAFileHeader tgaOutput;
	tgaOutput.cm_entry_size = 0;
	tgaOutput.cm_first_entry = 0;
	tgaOutput.cm_length = 0;
	tgaOutput.color_map = 0;
	tgaOutput.height = Terrain::realVerticesMapSide;
	tgaOutput.image_descriptor = 32;	//Right side up!!
	tgaOutput.image_id_len = 0;
	tgaOutput.image_type = UNC_GRAY;
	tgaOutput.pixel_depth = 8;
	tgaOutput.width = Terrain::realVerticesMapSide;
	tgaOutput.x_origin = 0;
	tgaOutput.y_origin = 0;

	File OEMtgaFile;
	long result = OEMtgaFile.create(OEMheightName);
	if (result != NO_ERR)
		STOP(("Couldnt create height map %s on refractalize.  Error: %d",(const char*)OEMheightName,result));
	
	OEMtgaFile.write((MemoryPtr)&tgaOutput,sizeof(TGAFileHeader));
	OEMtgaFile.write(sourceData,Terrain::realVerticesMapSide * Terrain::realVerticesMapSide);
	
	OEMtgaFile.close();
 	
	long edgeSize = (numTexturesAcross * COLOR_MAP_RES);
	std::vector<float> dstStore(static_cast<size_t>(edgeSize) * static_cast<size_t>(edgeSize));
	float *dstData = dstStore.data();

	rescaleMap(dstData,srcData,edgeSize,Terrain::realVerticesMapSide);
	
	//Now, apply refractalization to dstData.
	if (Noise)
		fractalPass(dstData,edgeSize,Threshold,Noise);
	
	//Save dstData as the height map.
	char newName[1024];
	mc2::fmt::to_buf(newName, "{}.height", mc2::fmt::safe(fileName));
		
	FullPathFileName heightName;
	heightName.init(terrainPath,newName,".tga");
	
	SetFileAttributes(heightName,FILE_ATTRIBUTE_NORMAL);
	
	std::vector<unsigned char> outputStore(static_cast<size_t>(edgeSize) * static_cast<size_t>(edgeSize));
	MemoryPtr outputData = outputStore.data();
	
	tmpEdge = dstData;
	tmpOutput = outputData;
	for (int i=0;i<edgeSize*edgeSize;i++)
	{
		*tmpOutput = (BYTE)CLAMP(*tmpEdge,BLACK_PIXEL,WHITE_PIXEL);
		tmpOutput++;
		tmpEdge++;
	}

	tgaOutput.cm_entry_size = 0;
	tgaOutput.cm_first_entry = 0;
	tgaOutput.cm_length = 0;
	tgaOutput.color_map = 0;
	tgaOutput.height = edgeSize;
	tgaOutput.image_descriptor = 32;	//Right side up!!
	tgaOutput.image_id_len = 0;
	tgaOutput.image_type = UNC_TRUE;
	tgaOutput.pixel_depth = 24;
	tgaOutput.width = edgeSize;
	tgaOutput.x_origin = 0;
	tgaOutput.y_origin = 0;

	//flipTopToBottom(outputData,8,edgeSize,edgeSize);
	
	File tgaFile;
	result = tgaFile.create(heightName);
	if (result != NO_ERR)
		STOP(("Couldnt create height map %s on refractalize.  Error: %d",(const char*)heightName,result));
	
	tgaFile.write((MemoryPtr)&tgaOutput,sizeof(TGAFileHeader));

	MemoryPtr outputByte = outputData;
	std::vector<unsigned char> output24Store(static_cast<size_t>(edgeSize) * static_cast<size_t>(edgeSize) * 3);
	MemoryPtr output24Bit = output24Store.data();
	MemoryPtr output24Temp = output24Bit;
	for (int i=0;i<edgeSize*edgeSize;i++)
	{
		*output24Temp = *outputByte;
		output24Temp++;
		*output24Temp = *outputByte;
		output24Temp++;
		*output24Temp = *outputByte;
		output24Temp++;

		outputByte++;
	}
	
	tgaFile.write(output24Bit,edgeSize * edgeSize * 3);
	tgaFile.close();
	
		
	//ReBurn the shadows.
	recalcLight(fileName);

}

float ContrastEnhance = 1.5f;
float ShadowEnhance = 2.0f;
//---------------------------------------------------------------------------
void TerrainColorMap::burnInShadows (bool doBumpPass, const char *fileName)
{
	//-----------------------------------------------------------------
	// for each pixel in the colormap, figure out where light is and
	// darken/lighten color based on light angle and shadows
	
	//first create a height map which is pixel for pixel identical to
	// the color map.
	long pixelWidth = numTexturesAcross * COLOR_MAP_TEXTURE_SIZE;
	std::vector<float> heightStore(static_cast<size_t>(pixelWidth) * static_cast<size_t>(pixelWidth));
	float *heightMap = heightStore.data();
	
	std::vector<float> shadowStore(static_cast<size_t>(pixelWidth) * static_cast<size_t>(pixelWidth));
	float *shadowMap = shadowStore.data();
	
	float worldUnitsPerPixel = (Terrain::worldUnitsMapSide / pixelWidth);
	
	float startY = Terrain::mapTopLeft3d.y;
	float *hMap = heightMap;
	float *sMap = shadowMap;

	if (Terrain::userMin == Terrain::userMax)
	{
		//Spread 'em out so the lighting actually works!!
		Terrain::userMin = 0;
		Terrain::userMax = 500;
	}

	//First, check if there is a height Map on disk.  If so, use it, not a fractally generated one.
	bool heightMapExists = false;
	float highPoint = 0.0f;
	if (fileName)
	{
		char newName[1024];
		mc2::fmt::to_buf(newName, "{}.height", mc2::fmt::safe(fileName));
		
		FullPathFileName heightName;
		heightName.init(terrainPath,newName,".tga");
		
		if (fileExists(heightName))
		{
			heightMapExists = true;
			
			File heightMapFile;
			long result = heightMapFile.open(heightName);
			if (result != NO_ERR)
				STOP(("Unable to find Hi-Res Height Data"));
				
			std::vector<unsigned char> tgaFileImage;
			TGAFileHeader heightMapInfo{};
			if (!loadTga(heightMapFile, tgaFileImage, heightMapInfo, "hi-res height map"))
				return;
			
			if (heightMapInfo.width != pixelWidth)
			{
				PAUSE(("Hi-Res Height Map wrong size %d. Should be %d. No shadows will be burnt.  Press Continue",heightMapInfo.width,pixelWidth));
				return;
			}

			if (heightMapInfo.image_type == UNC_TRUE)
			{
				MemoryPtr loadBuffer = tgaFileImage.data() + sizeof(TGAFileHeader);
				if (heightMapInfo.width != heightMapInfo.height)
					STOP(("Height Map is not a perfect Square"));
				if (!tgaPixelsPresent(tgaFileImage, heightMapInfo, "hi-res height map"))
					return;
				
				//-----------------------------------------------------------------
				// Check if 24 or 32 bit.  If 24, do the necessary stuff to it.
				// FIrst find MAX and MIN values for height field.
				BYTE mapMin = 255;
				BYTE mapMax = 0;
				if (heightMapInfo.pixel_depth == 24)
				{
					//24-Bit color means we must skip to every third color data.
					MemoryPtr lMap = loadBuffer;
					for (long i = 0;i<(heightMapInfo.width * heightMapInfo.width);i++)
					{
						BYTE val = *lMap;
						lMap += 3;
						if (val > mapMax)
							mapMax = val;
							
						if (val < mapMin)
							mapMin = val;
					}
				}
				else
				{
					//32-bit color means skip three instead of two bytes.
					MemoryPtr lMap = loadBuffer;
					for (long i = 0;i<(heightMapInfo.width * heightMapInfo.width);i++)
					{
						BYTE val = *lMap;
						lMap += 4;
						if (val > mapMax)
							mapMax = val;
							
						if (val < mapMin)
							mapMin = val;
					}
				}
				
				//Now create Height Map in Floating Point Space.				
				if (heightMapInfo.pixel_depth == 24)
				{
					//24-Bit color means we must skip to every third color data.
					float * hMap = heightMap;
					MemoryPtr lMap = loadBuffer;
					for (long i = 0;i<(heightMapInfo.width * heightMapInfo.width);i++)
					{
						float val = (float)(*lMap);
						lMap += 3;
						float h = Terrain::userMin;
						if (0.0 != mapMax)
						{
							h += (val - mapMin)/mapMax * ( Terrain::userMax - Terrain::userMin );
							*hMap = h * ShadowEnhance;
   						
							if (highPoint < *hMap)
								highPoint = *hMap;
								
							hMap++;
 						}
					}
				}
				else
				{
					//32-bit color means skip three instead of two bytes.
					float * hMap = heightMap;
					MemoryPtr lMap = loadBuffer;
					for (long i = 0;i<(heightMapInfo.width * heightMapInfo.width);i++)
					{
						float val = (float)(*lMap);
						lMap += 4;
						float h = Terrain::userMin;
						if (0.0 != mapMax)
						{
							h += (val - mapMin)/mapMax * ( Terrain::userMax - Terrain::userMin );
							*hMap = h * ShadowEnhance;
							
							if (highPoint < *hMap)
								highPoint = *hMap;
								
							hMap++;
						}
					}
				}
				
				//------------------------------------------------------------------------
				// Must check image_descriptor to see if we need to un upside down image.
				//bool left = (colorMapInfo.iimage_descriptor & 16) != 0;
				bool top = (heightMapInfo.image_descriptor & 32) != 0;
		
				if (!top)
				{
					flipTopToBottom((MemoryPtr)heightMap,32,heightMapInfo.width,heightMapInfo.height);
				}
			}
		}
	}
	else
	{
		long skipLong = 1;
		for (long y=0;y<pixelWidth;y++)
		{
			float startX = Terrain::mapTopLeft3d.x;
			for (long x=0;x<pixelWidth;x++)
			{
				Stuff::Vector3D pos;
				pos.x = startX;
				pos.y = startY;
				pos.z = 0.0f;
				
				if (!(x%skipLong) && !(y%skipLong))
					*hMap = land->getTerrainElevation(pos);
				else
					*hMap = 0.0f;
				
				*sMap = 0.0f;
				
				if (highPoint < *hMap)
					highPoint = *hMap;
					
				hMap++;
				sMap++;
				
				startX += worldUnitsPerPixel;
			}
			startY -= worldUnitsPerPixel;
		}
	}
	
	//----------------------------------------------------------
	// We now have a MUCH more accurate height map then we did.
	// Do normal calculations first.
	Stuff::Vector3D lightDir;
	lightDir.x = lightDir.y = 0.0f;
	lightDir.z = 1.0f;
	
	if (eye)
		lightDir = eye->lightDirection;
		
	Stuff::Vector3D shadowDir(lightDir);
	shadowDir *= worldUnitsPerPixel;		//Distance which is the per pixel distance for checking shadows
	
	//---------------------
	//Shadow Map Pass Here
	highPoint *= ShadowEnhance;
	float *thisHeight = heightMap;
	float *thisShadow = shadowMap;
	for (long y=0;y<pixelWidth;y++)
	{
		for (long x=0;x<pixelWidth;x++)
		{
			//---------------------------------------------
			// No problem
			// Generate at will!
			float v0,v1,v2,v3,v4,v5,v6,v7,v8;

			v0 = *thisHeight;
			if (x && y)
				v1 = *(thisHeight - pixelWidth - 1);
			else
				v1 = v0;

			if (y)
				v2 = *(thisHeight - pixelWidth    );
			else
				v2 = v0;

			if (y && (x < (pixelWidth - 1)))
				v3 = *(thisHeight - pixelWidth + 1);
			else
				v3 = v0;

			if (x < (pixelWidth - 1))
				v4 = *(thisHeight              + 1);
			else
				v4 = v0;

			if ((x < (pixelWidth - 1)) && (y < (pixelWidth - 1)))
				v5 = *(thisHeight + pixelWidth + 1);
			else
				v5 = v0;

			if ((y < (pixelWidth - 1)))
				v6 = *(thisHeight + pixelWidth	  );
			else
				v6 = v0;

			if (x && (y < (pixelWidth - 1)))
				v7 = *(thisHeight + pixelWidth - 1);
			else
				v7 = v0;

			if (x)
				v8 = *(thisHeight              - 1);
			else
				v8 = v0;
			
			if (Terrain::recalcShadows || forceShadowBurnIn)
			{
				//-----------------------------------------------------
				// Try and project shadow lines.
				Stuff::Vector3D vertexPos;
				vertexPos.x = ((float(x) * worldUnitsPerPixel) + Terrain::mapTopLeft3d.x);
				vertexPos.y = (Terrain::mapTopLeft3d.y - (float(y) * worldUnitsPerPixel));
				vertexPos.z = *thisHeight * ShadowEnhance;
					
				if ((shadowDir.x != 0.0f) || (shadowDir.y != 0.0f))
				{
					vertexPos.Add(vertexPos,shadowDir);
					if (!heightMapExists)
					{
						while (eye && (vertexPos.z < highPoint) && Terrain::IsValidTerrainPosition(vertexPos))
						{
							float elev = land->getTerrainElevation(vertexPos) * ShadowEnhance;
							if (elev >= vertexPos.z)
							{
								*thisShadow = -1.0f;		//Mark as shadowed
								break;						//Stop Looking!!
							}
							
							vertexPos.Add(vertexPos,shadowDir);
						}
					}
					else
					{
						float tx = x;
						float ty = y;
						while (eye && (vertexPos.z < highPoint) && Terrain::IsValidTerrainPosition(vertexPos))
						{
							//Figure out index into array from vertexPos and use REAL height data for shadows.
							long tileX = tx;
							long tileY = ty;
							
							float elev = heightMap[tileX + (tileY * pixelWidth)];
							if (elev > vertexPos.z)
							{
								*thisShadow = -1.0f;		//Mark as shadowed
								break;						//Stop Looking!!
							}

							tx += lightDir.x;
							ty -= lightDir.y;
							vertexPos.Add(vertexPos,shadowDir);
						}							
					}
				}
			}
				
			if (*thisShadow != -1.0f)
			{
				Stuff::Vector3D		normals[8];
				Stuff::Vector3D		triVect[2];
				
				//-------------------------------------
				// Tri 021
				triVect[0].x = 0.0f;
				triVect[0].y = worldUnitsPerPixel;
				triVect[0].z = (v2 - v0) * ContrastEnhance;
				
				triVect[1].x = -worldUnitsPerPixel;
				triVect[1].y = worldUnitsPerPixel;
				triVect[1].z = (v1 - v0) * ContrastEnhance;
				
				normals[0].Cross(triVect[0],triVect[1]);
				gosASSERT(normals[0].z > 0.0);
				
				normals[0].Normalize(normals[0]);
				
				//-------------------------------------
				// Tri 032
				triVect[0].x = worldUnitsPerPixel;
				triVect[0].y = worldUnitsPerPixel;
				triVect[0].z = (v3 - v0) * ContrastEnhance;
				
				triVect[1].x = 0.0f;
				triVect[1].y = worldUnitsPerPixel;
				triVect[1].z = (v2 - v0) * ContrastEnhance;
				
				normals[1].Cross(triVect[0],triVect[1]);
				gosASSERT(normals[1].z > 0.0);
				
				normals[1].Normalize(normals[1]);
				
				//-------------------------------------
				// Tri 043
				triVect[0].x = worldUnitsPerPixel;
				triVect[0].y = 0.0f;
				triVect[0].z = (v4 - v0) * ContrastEnhance;
	
				triVect[1].x = worldUnitsPerPixel;
				triVect[1].y = worldUnitsPerPixel;
				triVect[1].z = (v3 - v0) * ContrastEnhance;
				
			
				normals[2].Cross(triVect[0],triVect[1]);
				gosASSERT(normals[2].z > 0.0);
				
				normals[2].Normalize(normals[2]);
				
				//-------------------------------------
				// Tri 054
				triVect[0].x = worldUnitsPerPixel;
				triVect[0].y = -worldUnitsPerPixel;
				triVect[0].z = (v5 - v0) * ContrastEnhance;
				
				triVect[1].x = worldUnitsPerPixel;
				triVect[1].y = 0.0f;
				triVect[1].z = (v4 - v0) * ContrastEnhance;
				
				normals[3].Cross(triVect[0],triVect[1]);
				gosASSERT(normals[3].z > 0.0);
				
				normals[3].Normalize(normals[3]);
					
				//-------------------------------------
				// Tri 065
				triVect[0].x = 0;
				triVect[0].y = -worldUnitsPerPixel;
				triVect[0].z = (v6 - v0) * ContrastEnhance;
				
				triVect[1].x = worldUnitsPerPixel;
				triVect[1].y = -worldUnitsPerPixel;
				triVect[1].z = (v5 - v0) * ContrastEnhance;
				
				normals[4].Cross(triVect[0],triVect[1]);
				gosASSERT(normals[4].z > 0.0);
				
				normals[4].Normalize(normals[4]);
				
				//-------------------------------------
				// Tri 076
				triVect[0].x = -worldUnitsPerPixel;
				triVect[0].y = -worldUnitsPerPixel;
				triVect[0].z = (v7 - v0) * ContrastEnhance;
				
				triVect[1].x = 0.0;
				triVect[1].y = -worldUnitsPerPixel;
				triVect[1].z = (v6 - v0) * ContrastEnhance;
				
				normals[5].Cross(triVect[0],triVect[1]);
				gosASSERT(normals[5].z > 0.0);
				
				normals[5].Normalize(normals[5]);
		
				//-------------------------------------
				// Tri 087
				triVect[0].x = -worldUnitsPerPixel;
				triVect[0].y = 0.0;
				triVect[0].z = (v8 - v0) * ContrastEnhance;
				
				triVect[1].x = -worldUnitsPerPixel;
				triVect[1].y = -worldUnitsPerPixel;
				triVect[1].z = (v7 - v0) * ContrastEnhance;
				
				normals[6].Cross(triVect[0],triVect[1]);
				gosASSERT(normals[6].z > 0.0);
				
				normals[6].Normalize(normals[6]);
		
				//-------------------------------------
				// Tri 018
				triVect[0].x = -worldUnitsPerPixel;
				triVect[0].y = worldUnitsPerPixel;
				triVect[0].z = (v1 - v0) * ContrastEnhance;
				
				triVect[1].x = -worldUnitsPerPixel;
				triVect[1].y = 0.0;
				triVect[1].z = (v8 - v0) * ContrastEnhance;
				
				normals[7].Cross(triVect[0],triVect[1]);
				gosASSERT(normals[7].z > 0.0);
				
				normals[7].Normalize(normals[7]);
				
				Stuff::Vector3D vertexNormal;
				vertexNormal.x = normals[0].x + normals[1].x + normals[2].x + normals[3].x + normals[4].x + normals[5].x + normals[6].x + normals[7].x;
				vertexNormal.y = normals[0].y + normals[1].y + normals[2].y + normals[3].y + normals[4].y + normals[5].y + normals[6].y + normals[7].y;
				vertexNormal.z = normals[0].z + normals[1].z + normals[2].z + normals[3].z + normals[4].z + normals[5].z + normals[6].z + normals[7].z;
				vertexNormal.x /= 8.0;
				vertexNormal.y /= 8.0;
				vertexNormal.z /= 8.0;
	
				gosASSERT(vertexNormal.z > 0.0);
				
				*thisShadow = vertexNormal * lightDir;
			}
			
			thisHeight++;
			thisShadow++;
		}
	}
	
	//Smooth out the shadow map to make shadows better.
	for (long i=pixelWidth;i<((pixelWidth * pixelWidth)-pixelWidth);i++)
	{
		if (shadowMap[i] == -1.0f)
		{
			float v1,v2,v3,v4,v5,v6,v7,v8;
			//All of the pixels around this one should be no more then 0.3f
			v1 = shadowMap[i - pixelWidth - 1];
			v2 = shadowMap[i - pixelWidth    ];
			v3 = shadowMap[i - pixelWidth + 1];
			v4 = shadowMap[i              + 1];
			v5 = shadowMap[i + pixelWidth + 1];
			v6 = shadowMap[i + pixelWidth    ];
			v7 = shadowMap[i + pixelWidth - 1];
			v8 = shadowMap[i              - 1];
			
			if (v1 > 0.2f)
				shadowMap[i - pixelWidth - 1] = 0.2f;
				
			if (v2 > 0.2f)
				shadowMap[i - pixelWidth    ] = 0.2f;
				
 			if (v3 > 0.2f)
				shadowMap[i - pixelWidth + 1] = 0.2f;
				
 			if (v4 > 0.2f)
				shadowMap[i              + 1] = 0.2f;
				
 			if (v5 > 0.2f)
				shadowMap[i + pixelWidth + 1] = 0.2f;
				
 			if (v6 > 0.2f)
				shadowMap[i + pixelWidth    ] = 0.2f;
				
 			if (v7 > 0.2f)
				shadowMap[i + pixelWidth - 1] = 0.2f;
				
 			if (v8 > 0.2f)
				shadowMap[i              - 1] = 0.2f;
 		}
	}	
	
	// Shadow map is now done being calculated.
	// Apply the shadows to the magical terrain.
	DWORD *cMap = (DWORD *)ColorMap;
	for (int i=0;i<(pixelWidth * pixelWidth);i++)
	{
		if (shadowMap[i] != 0.0f)
		{
			float lightFactor = 0.0f;
			if (shadowMap[i] != -1.0f)	//Otherwise, its a shadow and light is 0!!
			{
				lightFactor = shadowMap[i];
			}
			else
			{
				lightFactor = 0.0f;
			}
			
			DWORD lightr, lightg, lightb;
			lightr = eye->getLightRed(lightFactor);
			lightg = eye->getLightGreen(lightFactor);
			lightb = eye->getLightBlue(lightFactor);
			
			DWORD currentColor = *cMap;
			
			float currentBlue = currentColor & 0xff;
			float currentGreen = (currentColor >> 8) & 0xff;
			float currentRed = (currentColor >> 16) & 0xff;
			
			currentRed *= ((float)lightr / 255.0f);
			currentBlue *= ((float)lightb / 255.0f);
			currentGreen *= ((float)lightg / 255.0f);
			
			lightr = currentRed;
			lightg = currentGreen;
			lightb = currentBlue;
			
			*cMap = (0xff << 24) + (lightr << 16) + (lightg << 8) + (lightb);
		}
		cMap++;
	}
	
	//All Done.  Get rid of the height and Shadow maps
}
	
//---------------------------------------------------------------------------
inline bool textureIsOKFormat (const char *fileName)
{
	File tgaFile;
	long result = tgaFile.open(fileName);
	if (result == NO_ERR)
	{
		struct TGAFileHeader tgaHeader;
		tgaFile.read((MemoryPtr)&tgaHeader,sizeof(TGAFileHeader));
		if (((tgaHeader.image_type == UNC_TRUE) || (tgaHeader.image_type == RLE_TRUE)) &&
			(tgaHeader.width == tgaHeader.height) &&
			((tgaHeader.width == 32) ||
			(tgaHeader.width == 64) ||
			(tgaHeader.width == 128) ||
			(tgaHeader.width == 256) ||
			(tgaHeader.width == 512) ||
			(tgaHeader.width == 1024) ||
			(tgaHeader.width == 2048) ||
			(tgaHeader.width == 4096)))
			return true;

		tgaFile.close();
	}

	return false;
}

//---------------------------------------------------------------------------
void TerrainColorMap::resetDetailTexture (const char *fileName)
{
	if (!textureIsOKFormat(fileName))
	{
		char msg[2048];
		mc2::fmt::to_buf(msg, "Texture {} is not 24bit or 32bit, 32x32, 64x64, 128x128 or 256x256 TGA File.  Not loaded!", mc2::fmt::safe(fileName));
		mc2MessageBox( msg, nullptr, MBOK, IDOK );
		return;
	}

	mcTextureManager->removeTextureNode (detailTextureNodeIndex);
	detailTextureNodeIndex = 0xffffffff;

#if 0
	//Load up the detail texture.
	// Detail texture is named same as colormap with .detail in name.
	// if no file exists, no texture drawn.
	char dName[1024];
	mc2::fmt::to_buf(dName, "{}.detail", mc2::fmt::safe(fileName));
		
	FullPathFileName detailFile;
	detailFile.init(texturePath,dName,".tga");

	if (fileExists(detailFile))		//Otherwise, its already 0xffffffff!!
		detailTextureNodeIndex = mcTextureManager->loadTexture(detailFile,gos_Texture_Alpha,gosHint_DontShrink, 0, 0x1);
#else
	detailTextureNodeIndex = mcTextureManager->loadTexture(fileName,gos_Texture_Alpha,gosHint_DontShrink, 0, 0x1);
#endif
}

//---------------------------------------------------------------------------
void TerrainColorMap::resetWaterTexture (const char *fileName)
{
	if (!textureIsOKFormat(fileName))
	{
		char msg[2048];
		mc2::fmt::to_buf(msg, "Texture {} is not 24bit or 32bit, 32x32, 64x64, 128x128 or 256x256 TGA File.  Not loaded!", mc2::fmt::safe(fileName));
		mc2MessageBox( msg, nullptr, MBOK, IDOK );
		return;
	}

	mcTextureManager->removeTextureNode (waterTextureNodeIndex);
	waterTextureNodeIndex = 0xffffffff;
	
#if 0
	//load up the water texture.
	// Water texture is named the same as the colormap with .water in name.
	// If no file exists, no texture drawn!!
	char dName[1024];
	mc2::fmt::to_buf(dName, "{}.water", mc2::fmt::safe(fileName));
	
	FullPathFileName waterFile;
	waterFile.init(texturePath,dName,".tga");
	
	if (fileExists(waterFile))    //Otherwise, its already 0xffffffff!! 
		waterTextureNodeIndex = mcTextureManager->loadTexture(waterFile,gos_Texture_Solid,0, 0, 0x1);
#else
	waterTextureNodeIndex = mcTextureManager->loadTexture(fileName,gos_Texture_Solid,0, 0, 0x1);
#endif
}

//---------------------------------------------------------------------------
void TerrainColorMap::resetWaterDetailTextures (const char *fileName)
{
	//Then, load up the water detail texture(s).
	// Water detail texture is named the same as the colormap with .water0000 in name
	// Where 0000 is the frame number of the animation for the water detail.
	// If the first frame does not exist, no texture is drawn!!
	for (long i=0;i<MAX_WATER_DETAIL_TEXTURES;i++)
	{
		char waterFile[1024];
		if (strlen("0000.tga") > strlen(fileName))
		{
			strcpy(waterFile, "0000.tga");
		}
		else
		{
			strcpy(waterFile, fileName);
			char dName[1024];
			mc2::fmt::to_buf(dName, "{:04d}.tga", mc2::fmt::as_i32(i));
			char *subStringToBeReplaced = &(waterFile[strlen(waterFile)-strlen("0000.tga")]);
			strcpy(subStringToBeReplaced, dName);
		}
		
		bool textureIsOK = true;
		if (!textureIsOKFormat(waterFile))
		{
			//PAUSE(("Texture %s is Not 24bit or 32bit TGA File.  Not loaded.  Press Continue",fileName));
			textureIsOK = false;
		}

		if (textureIsOK && fileExists(waterFile))
		{
			if (!i)		//If we found the first one OK, erase the current ones!!
			{
				for (long j=0;j<MAX_WATER_DETAIL_TEXTURES;j++)
				{
					mcTextureManager->removeTextureNode (waterDetailNodeIndex[j]);
					waterDetailNodeIndex[j] = 0xffffffff;
				}

				numWaterDetailFrames = 0;
			}

			waterDetailNodeIndex[i] = mcTextureManager->loadTexture(waterFile,gos_Texture_Alpha,gosHint_DontShrink, 0, 0x1);
			numWaterDetailFrames++;
		}
		else
		{
			if (!i)
				break;		//No water detail!
				
			waterDetailNodeIndex[i] = 0xffffffff;
		}
	}
}

//---------------------------------------------------------------------------
void TerrainColorMap::recalcLight(const char *fileName)
{
	colorMapRAMHeap = new UserHeap;
	colorMapRAMHeap->init(COLOR_MAP_HEAP_SIZE,"ColorMap");
		
	FullPathFileName colorMapName;
	colorMapName.init(texturePath,fileName,".tga");
		
	char newName[1024];
	mc2::fmt::to_buf(newName, "{}.burnin", mc2::fmt::safe(fileName));
	
	FullPathFileName burnInName;
	burnInName.init(texturePath,newName,".tga");

	File colorMapFile;
	long result = colorMapFile.open(colorMapName);
	if (result != NO_ERR)
	{
		PAUSE(("Unable to open Terrain Color Map %s.  Cannot Re-light map.  Press Continue", (const char*)colorMapName));
		return;
	}
			
	std::vector<unsigned char> tgaFileImage;
	TGAFileHeader colorMapInfo{};
	if (!loadTga(colorMapFile, tgaFileImage, colorMapInfo, "colour map"))
		return;

	if (colorMapInfo.image_type == UNC_TRUE)
	{
		MemoryPtr loadBuffer = tgaFileImage.data() + sizeof(TGAFileHeader);
		if (colorMapInfo.width != colorMapInfo.height)
			STOP(("Color Map is not a perfect Square"));
		if (!tgaPixelsPresent(tgaFileImage, colorMapInfo, "colour map"))
			return;
			
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
			flipTopToBottom(ColorMap,32,colorMapInfo.width,colorMapInfo.height);
		}

		//Apply shadow map.  calc every time for now.  Save as in editor, eventually.
		burnInShadows(true,fileName);
		saveTGAFile(ColorMap,fileName,colorMapInfo.width);

		if (skipColormapTiles()) {
			mc2::fmt::print_out("[COLORMAP] skip {:d}^2 tile GL upload ({:d}x{:d}) [COLORMAP-TILES-RETIRE-1]\n", mc2::fmt::as_i32((long)numTexturesAcross), mc2::fmt::as_i32(colorMapInfo.width), mc2::fmt::as_i32(colorMapInfo.width));
			numTextures = 0;
		} else {
			numTextures *= numTextures;

			txmRAM = (ColorMapRAM *)colorMapRAMHeap->Malloc(sizeof(ColorMapRAM) * numTextures);
			gosASSERT(txmRAM != nullptr);

			//Now, divide up the color map into separate COLOR_MAP_TEXTURE_SIZE textures.
			// and hand the data to the textureManager.

			for (unsigned long i=0;i<numTextures;i++)
			{
				mcTextureManager->removeTextureNode(textures[i].mcTextureNodeIndex);

				txmRAM[i].ourRAM = (MemoryPtr)colorMapRAMHeap->Malloc(sizeof(DWORD) * COLOR_MAP_TEXTURE_SIZE * COLOR_MAP_TEXTURE_SIZE);
				gosASSERT(txmRAM[i].ourRAM != nullptr);

				getColorMapData(txmRAM[i].ourRAM,i, colorMapInfo.width);

				textures[i].mcTextureNodeIndex = mcTextureManager->textureFromMemory((DWORD *)txmRAM[i].ourRAM,gos_Texture_Solid,gosHint_DontShrink,COLOR_MAP_TEXTURE_SIZE);
			}
		}
	}

	//At this point, the color Map DATA should be freeable!!
	// All of the textures have been passed to the mcTextureManager!
	{ ZoneScopedN("TerrainColorMap::init colorMap cleanup"); if (txmRAM) { for (unsigned long i=0;i<numTextures;i++)
	{
		colorMapRAMHeap->Free(txmRAM[i].ourRAM);
		txmRAM[i].ourRAM = nullptr;
	}

	colorMapRAMHeap->Free(txmRAM);
	txmRAM = nullptr;
	}

	colorMapRAMHeap->Free(ColorMap);
	ColorMap = nullptr;

	if (colorMapRAMHeap)
	{
		delete colorMapRAMHeap;
		colorMapRAMHeap = nullptr;
	}
	}
}

//---------------------------------------------------------------------------
void TerrainColorMap::resetBaseTexture (const char *fileName)
{
	//First, free up the existing colormap.
	for (unsigned long i=0;i<numTextures;i++)
	{
		mcTextureManager->removeTextureNode(textures[i].mcTextureNodeIndex);
		textures[i].mcTextureNodeIndex = 0xffffffff;
	}
			
	colorMapRAMHeap = new UserHeap;
	colorMapRAMHeap->init(COLOR_MAP_HEAP_SIZE,"ColorMap");
		
	FullPathFileName colorMapName;
	colorMapName.init(texturePath,fileName,".tga");
		
	char newName[1024];
	mc2::fmt::to_buf(newName, "{}.burnin", mc2::fmt::safe(fileName));
	
	FullPathFileName burnInName;
	burnInName.init(texturePath,newName,".tga");

	bool burnedIn = false;
	MemoryPtr tgaFileImage = nullptr;

	TGAFileHeader colorMapInfo;

	// BURNIN-DISK-JPEG: try the <name>.burnin.jpg sidecar first. On success it
	// decodes (stb_image) straight into a B,G,R,A ColorMap, and we synthesize a
	// 32-bit/top-down colorMapInfo so the downstream tiling code runs unchanged.
	bool jpgLoaded = false;
	{
		long jpgW = 0;
		MemoryPtr jpgColorMap = tryLoadBurninJpg(texturePath, fileName, colorMapRAMHeap, &jpgW);
		if (jpgColorMap != nullptr)
		{
			ColorMap = jpgColorMap;
			memset(&colorMapInfo, 0, sizeof(colorMapInfo));
			colorMapInfo.image_type = UNC_TRUE;
			colorMapInfo.width = (short)jpgW;
			colorMapInfo.height = (short)jpgW;
			colorMapInfo.pixel_depth = 32;
			colorMapInfo.image_descriptor = 32; // top-down: no flip (decode already top-down)
			burnedIn = true;
			jpgLoaded = true;
		}
	}

	if (!jpgLoaded)
	{
		File colorMapFile;
		long result = colorMapFile.open(burnInName);
		if (result != NO_ERR)
		{
			result = colorMapFile.open(colorMapName);
			if (result != NO_ERR)
				STOP(("Unable to open Terrain Color Map %s",(const char*)colorMapName));
		}
		else
		{
			burnedIn = true;
		}

		tgaFileImage = (MemoryPtr)malloc(colorMapFile.fileSize());
		gosASSERT(tgaFileImage != nullptr);

		colorMapFile.read(tgaFileImage,colorMapFile.fileSize());

		memcpy(&colorMapInfo,tgaFileImage,sizeof(TGAFileHeader));
	}

	if (colorMapInfo.image_type == UNC_TRUE)
	{
		if (colorMapInfo.width != colorMapInfo.height)
			STOP(("Color Map is not a perfect Square"));

		if (!jpgLoaded)
		{
			MemoryPtr loadBuffer = tgaFileImage + sizeof(TGAFileHeader);

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
		}

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
			flipTopToBottom(ColorMap,32,colorMapInfo.width,colorMapInfo.height);
		}

		//Apply shadow map.  calc every time for now.  Save as in editor, eventually.
		if (!burnedIn)
		{
			burnInShadows(true,fileName);
			saveTGAFile(ColorMap,fileName,colorMapInfo.width);
		}

		if (skipColormapTiles()) {
			mc2::fmt::print_out("[COLORMAP] skip {:d}^2 tile GL upload (reset, {:d}x{:d}) [COLORMAP-TILES-RETIRE-1]\n", mc2::fmt::as_i32((long)numTexturesAcross), mc2::fmt::as_i32(colorMapInfo.width), mc2::fmt::as_i32(colorMapInfo.width));
			numTextures = 0;
		} else {
			numTextures *= numTextures;
			textures = (ColorMapTextures *)colorMapHeap->Malloc(sizeof(ColorMapTextures) * numTextures);
			gosASSERT(textures != nullptr);

			txmRAM = (ColorMapRAM *)colorMapRAMHeap->Malloc(sizeof(ColorMapRAM) * numTextures);
			gosASSERT(txmRAM != nullptr);

			//Now, divide up the color map into separate COLOR_MAP_TEXTURE_SIZE textures.
			// and hand the data to the textureManager.

			for (unsigned long i=0;i<numTextures;i++)
			{
				txmRAM[i].ourRAM = (MemoryPtr)colorMapRAMHeap->Malloc(sizeof(DWORD) * COLOR_MAP_TEXTURE_SIZE * COLOR_MAP_TEXTURE_SIZE);
				gosASSERT(txmRAM[i].ourRAM != nullptr);

				getColorMapData(txmRAM[i].ourRAM,i, colorMapInfo.width);

				textures[i].mcTextureNodeIndex = mcTextureManager->textureFromMemory((DWORD *)txmRAM[i].ourRAM,gos_Texture_Solid,gosHint_DontShrink,COLOR_MAP_TEXTURE_SIZE);
			}
		}
	}

	// Retain CPU copy of colormap for terrain displacement HSV classification
	// and as RGBA8 fallback for BuildColormapAtlas (required when BPTC absent).
	{
		if (cpuColorMap) { free(cpuColorMap); cpuColorMap = nullptr; }
		long pixels = (long)colorMapInfo.width * (long)colorMapInfo.width;
		cpuColorMap = (unsigned char*)malloc(pixels * 4);
		gosASSERT(cpuColorMap != nullptr);
		cpuColorMapSize = colorMapInfo.width;
		memcpy(cpuColorMap, ColorMap, pixels * 4);
		mc2::fmt::print_out("[COLORMAP] retained colormap on CPU ({:d}x{:d})\n", mc2::fmt::as_i32(colorMapInfo.width), mc2::fmt::as_i32(colorMapInfo.width));
	}

	// COLORMAP-BC7-KTX2-1: probe for .burnin.ktx2 sidecar. When present and gate ON,
	// BuildColormapAtlas uploads compressed BC7 instead of RGBA8 cpuColorMap.
	// cpuColorMap is always kept as RGBA8 fallback (needed if BPTC cap is absent).
	ktx2ColormapPath[0] = '\0';
	if (useKtx2Colormap()) {
		char ktx2Name[1024];
		mc2::fmt::to_buf(ktx2Name, "{}.burnin", mc2::fmt::safe(fileName));
		FullPathFileName ktx2Path;
		ktx2Path.init(texturePath, ktx2Name, ".ktx2");
		FILE* probe = std::fopen((const char*)ktx2Path, "rb");
		if (probe) {
			fclose(probe);
			mc2::fmt::to_buf_n(ktx2ColormapPath, sizeof(ktx2ColormapPath), "{}", mc2::fmt::safe((const char*)ktx2Path));
			mc2::fmt::print_out("[COLORMAP] COLORMAP-BC7-KTX2-1: found sidecar {}\n", mc2::fmt::safe(ktx2ColormapPath));
		}
	}

	//At this point, the color Map DATA should be freeable!!
	// All of the textures have been passed to the mcTextureManager!
	if (txmRAM) {
	for (int i=0;i<numTextures;i++)
	{
		colorMapRAMHeap->Free(txmRAM[i].ourRAM);
		txmRAM[i].ourRAM = nullptr;
	}

	colorMapRAMHeap->Free(txmRAM);
	txmRAM = nullptr;
	}

	colorMapRAMHeap->Free(ColorMap);
	ColorMap = nullptr;

	if (colorMapRAMHeap)
	{
		delete colorMapRAMHeap;
		colorMapRAMHeap = nullptr;
	}
}
