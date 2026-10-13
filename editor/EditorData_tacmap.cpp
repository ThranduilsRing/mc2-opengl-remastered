// EditorData: tactical map drawing/save/load, and the teams/players mission data.
// Declared in the class headers; shared names in EditorData_internal.h.
#include "EditorData_internal.h"


bool EditorData::saveHeightMap( File* file )
{
	long row, column;
	float highest = land->getHighestVertex( row, column );
	float lowest = land->getLowestVertex( row, column );

	TGAFileHeader header;
	memset( &header, 0, sizeof( header ) );

	header.height = header.width = land->realVerticesMapSide;

	header.x_origin = 0;
	header.y_origin = 0;
	header.image_type = UNC_GRAY;
	header.pixel_depth = 8;
	header.image_descriptor = 0;

	file->write( (BYTE*)&header, sizeof( header ) );

	// now write image, upside down
	for ( int j = land->realVerticesMapSide - 1; j >= 0; j-- )
	{
		for ( int i = 0; i < land->realVerticesMapSide; i++ )
		{
			float height = land->getTerrainElevation( j, i );

			// turn this into 256 scale
			float difference = height - lowest;
			float ratio = (difference * 256)/(highest - lowest);
			BYTE final = ratio + .5 > 255. ? 255 : (int)(ratio + .5);

			// I could buffer this up if I need to make it faster.
			file->write( &final, sizeof( BYTE ) );
		}
	}

	return true;


}


struct TGARecs
{
	char fileName[64];
	MemoryPtr tgaData;
	long height;
	long width;
};

#define CONTRAST		128.0f
constexpr float BRIGHTNESS = 0.5f;
constexpr float LIGHT_YAW = -250.0f;
constexpr float LIGHT_PITCH = 32.0f;

inline bool isCementType (DWORD type)
{
	bool isCement = ((type == BASE_CEMENT_TYPE) ||
					((type >= START_CEMENT_TYPE) && (type <= END_CEMENT_TYPE)));
	return isCement;
}

void EditorData::drawTacMap( BYTE* pDest, long dataSize, int tacMapSize )
{
	EditorInterface::instance()->SetBusyMode();

	long ramSize = ((land->realVerticesMapSide) * MAPCELL_DIM) * ((land->realVerticesMapSide) * MAPCELL_DIM);

	DWORD* pShrunken = (DWORD*)pDest;
	
	memset( pShrunken, 0, dataSize );

	gosASSERT(tacMapBmp != NULL);
	memset(tacMapBmp,0,ramSize*4);

	Stuff::Vector3D optimalLight;
	optimalLight.x = cos(LIGHT_YAW * DEGREES_TO_RADS);
	optimalLight.y = sin(LIGHT_YAW * DEGREES_TO_RADS);
	optimalLight.z = sin(LIGHT_PITCH * DEGREES_TO_RADS);

	if (!land->mapData) { return; }
	PostcompVertexPtr currentVertex = land->mapData->getData();

	long i = 0;
	long y = 0;

	float maxElv=0, minElv=20000000;
	for (i=0;i<(land->realVerticesMapSide * land->realVerticesMapSide);i++)
	{
		if (currentVertex->elevation > maxElv)
			maxElv = currentVertex->elevation;

		if (currentVertex->elevation < minElv)
			minElv = currentVertex->elevation;

		currentVertex++;
	}

	currentVertex = land->mapData->getData();
	float elevationRange = maxElv - minElv;
	elevationRange /= 128.0f;			//Used to scale color based on elevation.

	//
	if (land->terrainTextures2)
	{
		land->terrainTextures2->getScaledColorMap((MemoryPtr)tacMapBmp,((land->realVerticesMapSide) * MAPCELL_DIM));
	}

	for (y=0;y<(land->realVerticesMapSide) * MAPCELL_DIM;y++)
	{
		for (long x=0;x<(land->realVerticesMapSide) * MAPCELL_DIM;x++)
		{
			PostcompVertex *pVertex1 = currentVertex;
				
			DWORD terrainType1RGB = 0xffffffff;

			if (pVertex1->elevation < (Terrain::waterElevation - MapData::shallowDepth))
				terrainType1RGB = land->terrainTextures->getTextureTypeRGB(0); 
			else if (pVertex1->elevation < Terrain::waterElevation)
				terrainType1RGB = 0x002e7599; 		//Derek's magic number for shallow water.
			else if (isCementType(pVertex1->terrainType))
				terrainType1RGB = land->terrainTextures->getTextureTypeRGB(pVertex1->terrainType);

			float lightIntensity1 = 1.0f;

			unsigned long *tBMP = &(tacMapBmp[(x) + ((y) * land->realVerticesMapSide * 3)]);

			if (terrainType1RGB != 0xffffffff)
				*tBMP = terrainType1RGB;

			tBMP++;

			if (x && !(x % 3))
			{
				currentVertex++;
			}
		}
		
		currentVertex = land->mapData->getData(); 
		currentVertex += land->realVerticesMapSide * (y / 3);
	}

	unsigned long *tBMP = tacMapBmp;
	for( long i = 0; i < land->realVerticesMapSide * MAPCELL_DIM; i++ )
	{
		for ( int j = 0; j < land->realVerticesMapSide * MAPCELL_DIM; j++ )
		{
			EditorObject* pObj = EditorObjectMgr::instance()->getObjectAtCell( j, i );
			if ( pObj && EditorObjectMgr::instance()->getAppearanceType( pObj->getID() ) == TREED_TYPE )
			{
				DWORD green = ((*tBMP & 0x0000ff00) >> 8);
				green += 10;
				if (green > 255)
					green = 255;
				
				*tBMP &= 0xffff00ff;
				*tBMP += (green << 8);
			}

			tBMP++;
		}
	}

	currentVertex = land->mapData->getData();
	for (long y=0;y<(land->realVerticesMapSide-1);y++)
	{
		for (long x=0;x<(land->realVerticesMapSide-1);x++)
		{
			PostcompVertex *pVertex1 = currentVertex;
				
			Overlays o = (Overlays)-1;
			unsigned long offset = 0;
			
			land->getOverlay(y,x,o,offset);
			
			DWORD terrainTypeRGB = 0xffffffff;
			if (o != (Overlays)-1)
				terrainTypeRGB = land->terrainTextures->getOverlayTypeRGB(o);

			if (terrainTypeRGB == 0xffffffff)
				continue;

			PostcompVertex *pVertex2 = currentVertex + 1;
			PostcompVertex *pVertex3 = currentVertex + land->realVerticesMapSide + 1;
			PostcompVertex *pVertex4 = currentVertex + land->realVerticesMapSide;
			
 			float lightIntensity1 = pVertex1->vertexNormal * optimalLight;
			float lightIntensity2 = pVertex2->vertexNormal * optimalLight;
			float lightIntensity3 = pVertex3->vertexNormal * optimalLight;
			float lightIntensity4 = pVertex4->vertexNormal * optimalLight;

			unsigned long *tBMP = &(tacMapBmp[(x * 3) + ((y * 3) * land->realVerticesMapSide * 3)]); 
			float totalR1, totalG1, totalB1, totalR2, totalG2, totalB2, totalR3, totalG3, totalB3, totalR4, totalG4, totalB4;

			long elvOffset1 = (lightIntensity1 - 0.5f) * CONTRAST;
			totalR1 = ((terrainTypeRGB>>16) & 0x000000ff);
			totalG1 = ((terrainTypeRGB>>8) & 0x000000ff);
			totalB1 = (terrainTypeRGB & 0x000000ff);

			totalR1 += elvOffset1;
			if (totalR1 > 255.0f)
				totalR1 = 255.0f;

			totalG1 += elvOffset1;
			if (totalG1 > 255.0f)
				totalG1 = 255.0f;

			totalB1 += elvOffset1;
			if (totalB1 > 255.0f)
				totalB1 = 255.0f;

			if (totalR1 < 0.0f)
				totalR1 = 0.0f;
			if (totalG1 < 0.0f)
				totalG1 = 0.0f;
			if (totalB1 < 0.0f)
				totalB1 = 0.0f;

			DWORD color1 = (((DWORD)totalR1) << 16) + (((DWORD)totalG1) << 8) + ((DWORD)totalB1); 

			long elvOffset2 = (lightIntensity2 - 0.5f) * CONTRAST;
			totalR2 = ((terrainTypeRGB>>16) & 0x000000ff);
			totalG2 = ((terrainTypeRGB>>8) & 0x000000ff);
			totalB2 = (terrainTypeRGB & 0x000000ff);

			totalR2 += elvOffset2;
			totalG2 += elvOffset2;
			totalB2 += elvOffset2;

			if (totalR2 > 255.0f)
				totalR2 = 255.0f;
			if (totalG2 > 255.0f)
				totalG2 = 255.0f;
			if (totalB2 > 255.0f)
				totalB2 = 255.0f;

			if (totalR2 < 0.0f)
				totalR2 = 0.0f;
			if (totalG2 < 0.0f)
				totalG2 = 0.0f;
			if (totalB2 < 0.0f)
				totalB2 = 0.0f;

			DWORD color2 = (((DWORD)totalR2) << 16) + (((DWORD)totalG2) << 8) + ((DWORD)totalB2); 

			long elvOffset4 = (lightIntensity4 - 0.5f) * CONTRAST;
			totalR4 = ((terrainTypeRGB>>16) & 0x000000ff);
			totalG4 = ((terrainTypeRGB>>8) & 0x000000ff);
			totalB4 = (terrainTypeRGB & 0x000000ff);

			totalR4 += elvOffset4;
			totalG4 += elvOffset4;
			totalB4 += elvOffset4;

			if (totalR4 > 255.0f)
				totalR4 = 255.0f;
			if (totalG4 > 255.0f)
				totalG4 = 255.0f;
			if (totalB4 > 255.0f)
				totalB4 = 255.0f;

			if (totalR4 < 0.0f)
				totalR4 = 0.0f;
			if (totalG4 < 0.0f)
				totalG4 = 0.0f;
			if (totalB4 < 0.0f)
				totalB4 = 0.0f;

			DWORD color4 = (((DWORD)totalR4) << 16) + (((DWORD)totalG4) << 8) + ((DWORD)totalB4); 

			long elvOffset3 = (lightIntensity3 - 0.5f) * CONTRAST;
			totalR3 = ((terrainTypeRGB>>16) & 0x000000ff);
			totalG3 = ((terrainTypeRGB>>8) & 0x000000ff);
			totalB3 = (terrainTypeRGB & 0x000000ff);

			totalR3 += elvOffset3;
			totalG3 += elvOffset3;
			totalB3 += elvOffset3;

			if (totalR3 > 255.0f)
				totalR3 = 255.0f;
			if (totalG3 > 255.0f)
				totalG3 = 255.0f;
			if (totalB3 > 255.0f)
				totalB3 = 255.0f;

			if (totalR3 < 0.0f)
				totalR3 = 0.0f;
			if (totalG3 < 0.0f)
				totalG3 = 0.0f;
			if (totalB3 < 0.0f)
				totalB3 = 0.0f;

			DWORD color3 = (((DWORD)totalR3) << 16) + (((DWORD)totalG3) << 8) + ((DWORD)totalB3); 

			*tBMP = color1;
			tBMP++;

			*tBMP = color1;
			tBMP++;

			*tBMP = color2;
			tBMP += (land->realVerticesMapSide*3)-2;

			*tBMP = color1;
			tBMP++;

			*tBMP = color1;
			tBMP++;

			*tBMP = color2;
			tBMP += (land->realVerticesMapSide*3)-2;

			*tBMP = color4;
			tBMP++;

			*tBMP = color4;
			tBMP++;

			*tBMP = color3;
			currentVertex++;
		}
			
		currentVertex++;
	}
	 
	tBMP = tacMapBmp;
	TGARecs *tRecs = (TGARecs *)systemHeap->Malloc(sizeof(TGARecs) * 255);
	memset(tRecs,0,sizeof(TGARecs) * 255);
	
	for( i = 0; i < land->realVerticesMapSide * MAPCELL_DIM; i++ )
	{
		for ( int j = 0; j < land->realVerticesMapSide * MAPCELL_DIM; j++ )
		{
			EditorObject* pObj = EditorObjectMgr::instance()->getObjectAtCell( i, j );
			if ( pObj && EditorObjectMgr::instance()->getTacMapColor(pObj->getID()) )
			{
				DWORD tgaColor = EditorObjectMgr::instance()->getTacMapColor(pObj->getID());
				float tBlue = tgaColor & 0x000000ff;
				float tGreen = (tgaColor >> 8) & 0x000000ff;
				float tRed = (tgaColor >> 16) & 0x000000ff;
				float alpha = (tgaColor >> 24) & 0x000000ff;
				float alphaFactor = alpha / 255.0;
									
				DWORD bmpColor = *tBMP;
				float bBlue = bmpColor & 0x000000ff;
				float bGreen = (bmpColor >> 8) & 0x000000ff;
				float bRed = (bmpColor >> 16) & 0x000000ff;
									
				float nBlue = (tBlue * alphaFactor) + (bBlue * (1.0f - alphaFactor));
				float nGreen = (tGreen * alphaFactor) + (bGreen * (1.0f - alphaFactor));
				float nRed = (tRed * alphaFactor) + (bRed * (1.0f - alphaFactor));
				
				if (nBlue > 255.0f)
					nBlue = 255.0f;
					
				if (nGreen > 255.0f)
					nGreen = 255.0f;
					
				if (nRed > 255.0f)
					nRed = 255.0f;
				
				BYTE nb = nBlue,ng = nGreen,nr = nRed;
				DWORD nColor = (nr << 16) + (ng << 8) + (nb);

				if (i && j && 
					(i < ((land->realVerticesMapSide * MAPCELL_DIM) - 1)) &&
					(j < ((land->realVerticesMapSide * MAPCELL_DIM) - 1)))
				{
					unsigned long *tmptBMP = tBMP + (land->realVerticesMapSide * MAPCELL_DIM);
					tmptBMP -= 1;
					
					*tmptBMP = nColor;
					tmptBMP++;
		
					*tmptBMP = nColor; 
					tmptBMP++;
		
					*tmptBMP = nColor; 
					tmptBMP += (land->realVerticesMapSide*3)-2;
		
					*tmptBMP = nColor; 
					tmptBMP++;
		
					*tmptBMP = nColor; 
					tmptBMP++;
		
					*tmptBMP = nColor; 
					tmptBMP += (land->realVerticesMapSide*3)-2;
		
					*tmptBMP = nColor; 
					tmptBMP++;
		
					*tmptBMP = nColor; 
					tmptBMP++;
		
					*tmptBMP = nColor; 
				}
			}

			if ( pObj && 
				strcmp(EditorObjectMgr::instance()->getTGAFileName(pObj->getID()),"NONE") != 0 )
			{
				long recNum = -1;
				MemoryPtr tgaRAM = nullptr;
				long tHeight = 0, tWidth = 0;
				for (long it = 0;it<255;it++)
				{
					if (tRecs[it].tgaData && strcmp(EditorObjectMgr::instance()->getTGAFileName(pObj->getID()),tRecs[it].fileName) == 0)
					{
						recNum = it;
						break;
					}
				}
				
				if (recNum != -1)
				{
					tgaRAM = tRecs[recNum].tgaData;
					tWidth = tRecs[recNum].width;
					tHeight = tRecs[recNum].height;
				}
				else
				{
					File tgaFile;
					long result = ~NO_ERR;
					if (0 != strcmp("0", EditorObjectMgr::instance()->getTGAFileName(pObj->getID())))
					{
						FullPathFileName tgaName;
						tgaName.init(artPath,EditorObjectMgr::instance()->getTGAFileName(pObj->getID()),".tga");
						
						result = tgaFile.open(tgaName);
					}
					if (result == NO_ERR)
					{
						TGAFileHeader header;
						tgaFile.read((MemoryPtr)&header,sizeof(TGAFileHeader));
						tgaFile.seek(0);
						
						tgaRAM = (MemoryPtr)systemHeap->Malloc(header.width * header.height * sizeof(DWORD));
						
						loadTGATexture (&tgaFile, tgaRAM, header.width, header.height);
						
						for (long it = 0;it<255;it++)
						{
							if (!(tRecs[it].tgaData))
							{
								tRecs[it].tgaData = tgaRAM;
								strcpy(tRecs[it].fileName,EditorObjectMgr::instance()->getTGAFileName(pObj->getID()));
								tRecs[it].height = tHeight = header.height;
								tRecs[it].width = tWidth = header.width;
								break;
							}
						}
					}
				}
   				
				if (tgaRAM)
				{
					unsigned long *tmptBMP = tBMP;
					unsigned long *tgaBMP = (unsigned long *)tgaRAM;
					for (long ib=0;ib<tHeight;ib++)
					{
						if (((i - (tHeight >> 1)) >= 0) &&
							(((tHeight >> 1) + i) < (land->realVerticesMapSide * MAPCELL_DIM)))
						{
							if (((j - (tWidth >> 1)) >= 0) &&
								(((tWidth >> 1) + j) < (land->realVerticesMapSide * MAPCELL_DIM)))
							{
								tmptBMP = tBMP + ((land->realVerticesMapSide * MAPCELL_DIM) * (ib - (tHeight >> 1)));
								tmptBMP -= (tWidth >> 1);
								
								for (long jb=0;jb<tWidth;jb++)
								{
									DWORD tgaColor = *tgaBMP;
									float tBlue = tgaColor & 0x000000ff;
									float tGreen = (tgaColor >> 8) & 0x000000ff;
									float tRed = (tgaColor >> 16) & 0x000000ff;
									float alpha = (tgaColor >> 24) & 0x000000ff;
									float alphaFactor = alpha / 255.0;
									
									DWORD bmpColor = *tmptBMP;
									float bBlue = bmpColor & 0x000000ff;
									float bGreen = (bmpColor >> 8) & 0x000000ff;
									float bRed = (bmpColor >> 16) & 0x000000ff;
									
									float nBlue = (tBlue * alphaFactor) + (bBlue * (1.0f - alphaFactor));
									float nGreen = (tGreen * alphaFactor) + (bGreen * (1.0f - alphaFactor));
									float nRed = (tRed * alphaFactor) + (bRed * (1.0f - alphaFactor));
									
									if (nBlue > 255.0f)
										nBlue = 255.0f;
										
									if (nGreen > 255.0f)
										nGreen = 255.0f;
										
									if (nRed > 255.0f)
										nRed = 255.0f;
									
									BYTE nb = nBlue,ng = nGreen,nr = nRed;
									DWORD nColor = (nr << 16) + (ng << 8) + (nb);
									
									*tmptBMP = nColor;
									tmptBMP++;
									tgaBMP++;
								}
							}
						}
					}
				}
			}
			
			tBMP++;
		}
	}
	
	for (long it = 0;it<255;it++)
	{
		if (tRecs[it].tgaData)
		{
			systemHeap->Free(tRecs[it].tgaData);
			tRecs[it].tgaData = nullptr;	
		}
	}
	
	systemHeap->Free(tRecs);
	tRecs = nullptr;
	
	double xRatio = 3. * (land->realVerticesMapSide)/tacMapSize;
	double yRatio = 3. * (land->realVerticesMapSide)/tacMapSize;

	DWORD *pTmp = pShrunken;
	for ( i = 0; i < tacMapSize; i++ )
	{
		y = ((float)i * yRatio) + .5;

		int j = 0;
		for ( ; j < tacMapSize; j++ )
		{
			long x = ((float)j * xRatio) + .5;
			unsigned long *tBMP = &(tacMapBmp[(x) + ((y) * land->realVerticesMapSide * 3)]); 
			*pTmp++ = *tBMP;			
		}

		for ( ; j < 128; ++j ) // leave the edges blank.
		{
			pTmp++;
		}
	}

	EditorInterface::instance()->UnsetBusyMode();
}

void EditorData::makeTacMap(BYTE*& pReturn, long& dataSize, int tacMapSize )
{
	// the bitmap is always 128 * 128, the size of the tac map can be anything,
	// the edges will be filled in with transparency
	
	//-----------------------------------------------------
	// Create a block of RAM for Bitmap.
	// 24 Bits of color at Cell resolution.
	long outputSize = 128 * 128 * 4;

	BYTE* pOutput = (BYTE*)malloc( outputSize + sizeof( TGAFileHeader ) );
	pReturn = pOutput;
	BYTE* pShrunken = pOutput + sizeof( TGAFileHeader );
	
	dataSize = outputSize + sizeof( TGAFileHeader );

	memset( pShrunken, 0, outputSize );
	
	drawTacMap( pShrunken, outputSize, tacMapSize );

	TGAFileHeader* header = (TGAFileHeader*)pOutput;
	memset( header, 0, sizeof( TGAFileHeader ) );

	header->height = header->width = 128;

	header->x_origin = 0;
	header->y_origin = 0;
	header->image_type = UNC_TRUE;
	header->pixel_depth = 32;
	header->image_descriptor = 32;

}

//---------------------------------------------------------------------------
void EditorData::loadTacMap(PacketFile *file, BYTE*& pReturn, long dataSize, int tacMapSize )
{
	// the bitmap is always 128 * 128, the size of the tac map can be anything,
	// the edges will be filled in with transparency
	
	long outputSize = 128 * 128 * 4;

	BYTE* pOutput = new BYTE[ outputSize + sizeof( TGAFileHeader )];
	pReturn = pOutput;
	BYTE* pShrunken = pOutput + sizeof( TGAFileHeader );
	
	dataSize = outputSize + sizeof( TGAFileHeader );

	memset( pShrunken, 0, outputSize );

	if ((file->seekPacket(3) == NO_ERR) && (file->getPacketSize() != 0))
	{
		file->readPacket(3,pOutput);
	}
	else
	{
		makeTacMap(pReturn,dataSize,tacMapSize);
	}
}

//---------------------------------------------------------------------------
bool EditorData::saveTacMap (PacketFile* file, long whichPacket )
{
	BYTE* data = nullptr;
	long size = 0;

	makeTacMap( data, size, 128 );
	file->writePacket( whichPacket, data, size);

	File test;
	test.open( "tgatest.tga", CREATE );
	test.write( data, size);
	test.close();
	
	delete []  data;

	return true;
}

bool CTeam::operator==(const CTeam &rhs) const {
	bool retval = false;
	if ((m_alignment == rhs.m_alignment) && (m_objectives == rhs.m_objectives)) {
		retval = true;
	}
	return retval;
}

CTeams::CTeams() {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_teamArray[i].Alignment(i);
	}
}

void CTeams::Clear() {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_teamArray[i].Clear();
	}
}

CTeams &CTeams::operator=(const CTeams &master) {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_teamArray[i] = master.m_teamArray[i];
	}
	return (*this);
}

bool CTeams::operator==(const CTeams &rhs) const {
	bool retval = true;
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		if (!(rhs.m_teamArray[i] == m_teamArray[i])) {
			retval = false;
			break;
		}
	}
	return retval;
}

bool CTeams::Read( FitIniFile* missionFile ) {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		if (!m_teamArray[i].Read(missionFile)) return false;
	}
	return true;
}

bool CTeams::Save( FitIniFile* missionFile ) {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_teamArray[i].Save(missionFile);
	}
	return true;
}

CTeam &CTeams::TeamRef(int i) {
	gosASSERT(GAME_MAX_PLAYERS > i); gosASSERT(0 <= i);
	return m_teamArray[i];
}

void CTeams::handleObjectInvalidation(const EditorObject *pObj) {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_teamArray[i].handleObjectInvalidation(pObj);
	}
}

bool CTeams::NoteThePositionsOfObjectsReferenced() {
	bool retval = true;
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		bool res = m_teamArray[i].NoteThePositionsOfObjectsReferenced();
		if (false == res) {
			retval = false;
		}
	}
	return retval;
}

bool CTeams::RestoreObjectPointerReferencesFromNotedPositions() {
	bool retval = true;
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		bool res = m_teamArray[i].RestoreObjectPointerReferencesFromNotedPositions();
		if (false == res) {
			retval = false;
		}
	}
	return retval;
}

bool CTeams::ThereAreObjectivesWithNoConditions() {
	bool retval = false;
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		bool res = m_teamArray[i].ThereAreObjectivesWithNoConditions();
		if (true == res) {
			retval = true;
			break;
		}
	}
	return retval;
}

void CPlayer::DefaultTeam(int team) {
	gosASSERT((GAME_MAX_PLAYERS > team) && (0 <= team));
	m_defaultTeam = team%GAME_MAX_PLAYERS;
}

bool CPlayer::Read( FitIniFile* missionFile, int playerNum )
{
	long result = 0;
	EString tmpStr;
	tmpStr.Format("Player%d", playerNum);
	result = missionFile->seekBlock(tmpStr.Data());
	result = sReadIdWholeNum(missionFile, "DefaultTeam", m_defaultTeam);
	return result;
}

bool CPlayer::Save( FitIniFile* missionFile, int playerNum )
{
	EString tmpStr;
	tmpStr.Format("Player%d", playerNum);
	missionFile->writeBlock( tmpStr.Data() );
	missionFile->writeIdULong( "DefaultTeam", DefaultTeam() );
	return true;
}

void CPlayers::Clear() {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_playerArray[i].Clear();
	}
	/*set player2's default to team2*/
	m_playerArray[1].DefaultTeam(1);
}

bool CPlayers::Read( FitIniFile* missionFile ) {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_playerArray[i].Read(missionFile, i);
	}
	return true;
}

bool CPlayers::Save( FitIniFile* missionFile ) {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_playerArray[i].Save(missionFile, i);
	}
	return true;
}
