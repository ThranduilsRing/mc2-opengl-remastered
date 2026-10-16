// TG shapes: ASE file parsing and shape loading (TG_TypeShape::ParseASEFile, LoadTGShapeFromASE), textures, imported meshes, camera/fog/light setup.
// Declared in the class headers; shared names in tgl_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "tgl_internal.h"
#include "mc2fmt.h"


//-------------------------------------------------------------------------------
//Function return 0 is OK.  -1 if file is not ASE Format or missing data.
//This function simply parses the ASE buffers handed to it.  This allows
//users to load the ase file themselves and manage their own memory for it.
//It allocates memory for internal Lists.  These are straight tglHeap->Mallocs at present.
//
// NOTE: Only takes the first GEOMOBJECT from the ASE file.  Multi-object
// Files will require user intervention to parse!!
long TG_TypeShape::ParseASEFile (BYTE *aseBuffer, const char *fileName)
{
	char *nodeName = strstr((char *)aseBuffer,ASE_NODE_NAME);
	gosASSERT(nodeName != nullptr);

	char nodeString[1024];
	nodeName += strlen(ASE_NODE_NAME)+1;
	GetNameData(nodeName,nodeString);

#ifdef _DEBUG
	if (strlen(nodeString) >= TG_NODE_ID)
	{
		if (!silentMode)
			PAUSE(("WARNING: Node ID %s in Shape %s is greater then %d characters!!",nodeString,fileName, TG_NODE_ID-1));
	}
#endif

	//sebi: ORIG BUG FIX srtrncpy will not add 0 at the end of source string bigger then destination one
	//strncpy(nodeId,nodeString, 24);
	strncpy(nodeId,nodeString, TG_NODE_ID-1);
	nodeId[TG_NODE_ID - 1] = '\0';
	
	char* parentName = strstr(nodeName,ASE_NODE_PARENT);

	//-------------------------------------------------------------------
	// Must also check to make sure we HAVE a parent.
	// We will get the next GeomObject's parent if we don't check length!
	if ((parentName != nullptr) && ((parentName - nodeName) < MAX_SCAN_LENGTH))
	{
		parentName += strlen(ASE_NODE_PARENT)+1;
		GetNameData(parentName,nodeString);
		
		strncpy(parentId,nodeString, TG_NODE_ID-1);
		parentId[TG_NODE_ID - 1] = '\0';
	}
	else
	{
		strcpy(parentId,"None");
	}

	nodeName = strstr((char *)aseBuffer,ASE_NODE_POS);
	gosASSERT(nodeName != nullptr);

	nodeName += strlen(ASE_NODE_POS)+1;

	char numData[512];

	GetNumberData(nodeName,numData);
	nodeCenter.x = -(float)atof(numData);
	nodeName += strlen(numData)+1;

	GetNumberData(nodeName,numData);
	nodeCenter.z = (float)atof(numData);
	nodeName += strlen(numData)+1;

	GetNumberData(nodeName,numData);
	nodeCenter.y = (float)atof(numData);

	char *vertexCount = strstr((char *)aseBuffer,ASE_NUM_VERTEX);
	gosASSERT(vertexCount != nullptr);
	
	//sebi: ORIG BUG FIX
	//vertexCount += strlen(ASE_NUM_FACE)+1;
	vertexCount += strlen(ASE_NUM_VERTEX)+1;
	numTypeVertices = atol(vertexCount);
	if (numTypeVertices == 0)			//NO Data for this node.  WARN and then do NOT allocate anything!
	{
#ifdef _DEBUG
		if (!silentMode)
			PAUSE(("WARNING: No Vertices for Shape Node %s in Shape %s!",nodeId,fileName));
#endif
		init();

		return (0);
	}

	char *faceCount = strstr((char *)aseBuffer,ASE_NUM_FACE);
	gosASSERT(faceCount != nullptr);
		
	faceCount += strlen(ASE_NUM_FACE)+1;
	numTypeTriangles = atol(faceCount);
	gosASSERT(numTypeTriangles != 0);

	if (numTypeVertices > kMaxShapeVertices || numTypeTriangles > kMaxShapeTriangles)
	{
		MC2_LOG(Io, Error, "[SHAPE_COUNT_BAD] file={} vertices={} triangles={} (over {} / {}; node skipped)", fileName, numTypeVertices, numTypeTriangles, kMaxShapeVertices, kMaxShapeTriangles);
		init();
		return (-1);
	}

	typeVertexStore.resize(numTypeVertices);
	listOfTypeVertices = typeVertexStore.data();

	memset(listOfTypeVertices,0xff,sizeof(TG_TypeVertex) * numTypeVertices);

	std::vector<int> vertexNormalCount(numTypeVertices, 0);

	for (int i=0;i<numTypeVertices;i++)
	{
		char numberData[256];

		char vertexID[256];
		mc2::fmt::to_buf(vertexID, "{}{: 5d}", mc2::fmt::safe(ASE_MESH_VERTEX_ID), mc2::fmt::as_i32(i));

		char *vertexData = strstr((char *)aseBuffer,vertexID);
		gosASSERT(vertexData != nullptr);
			
		vertexData += strlen(vertexID)+1;

		GetNumberData(vertexData,numberData);
		vertexData += strlen(numberData)+1;
		listOfTypeVertices[i].position.x = -(float)atof(numberData);

		GetNumberData(vertexData,numberData);
		vertexData += strlen(numberData)+1;
		listOfTypeVertices[i].position.z = (float)atof(numberData);

		GetNumberData(vertexData,numberData);
		listOfTypeVertices[i].position.y = (float)atof(numberData);

		//------------------------------------------------
		// Next, Vertex Normal
		// These are "funny" because MAX writes out a
		// separate normal for each instance of the vertex
		// wrt a face.  So we must catch EVERY instance
		// and average together and then normalize.
		// As such, we must now extract during the Face part below
		// Just init the normal here!
		listOfTypeVertices[i].normal.x = 
		listOfTypeVertices[i].normal.y = 
		listOfTypeVertices[i].normal.z = 0.0f;


		listOfTypeVertices[i].aRGBLight 			= 0xff000000;

	}

	typeTriangleStore.resize(numTypeTriangles);
	listOfTypeTriangles = typeTriangleStore.data();

	memset(listOfTypeTriangles,0xff,sizeof(TG_TypeTriangle) * numTypeTriangles);

	for (int i=0;i<numTypeTriangles;i++)
	{
		char numberData[256];

		char faceId[256];
		mc2::fmt::to_buf(faceId, "{}{: 5d}:", mc2::fmt::safe(ASE_MESH_FACE_ID), mc2::fmt::as_i32(i));

		char *faceData = strstr((char *)aseBuffer,faceId);
		gosASSERT(faceData != nullptr);

		faceData = strstr(faceData,"A:");
		gosASSERT(faceData != nullptr);
			
		GetNumberData(faceData,numberData);
		listOfTypeTriangles[i].Vertices[0] = atol(numberData);

		faceData = strstr(faceData,"B:");
		gosASSERT(faceData != nullptr);

		GetNumberData(faceData,numberData);
		listOfTypeTriangles[i].Vertices[1] = atol(numberData);

		faceData = strstr(faceData,"C:");
		gosASSERT(faceData != nullptr);
	
		GetNumberData(faceData,numberData);
		listOfTypeTriangles[i].Vertices[2] = atol(numberData);

		if ((numTextures/2) == 1)		//We don't need the MATLID, its always 0.  Now divBytwo to load shadow texture as well.
			listOfTypeTriangles[i].localTextureHandle = 0;
		else
		{
			//Must parse MATLID to find material ID.
			faceData = strstr(faceData,ASE_FACE_MATERIAL_ID);
			gosASSERT(faceData != nullptr);

			faceData += strlen(ASE_FACE_MATERIAL_ID)+1;

			GetNumberData(faceData,numberData);

			int matLid = atol(numberData);
			if (matLid >= (numTextures/2))
				matLid = (numTextures/2) - 1;

			listOfTypeTriangles[i].localTextureHandle = matLid;

		}

		listOfTypeTriangles[i].renderStateFlags = 0;		//No Flags yet.

		//
		mc2::fmt::to_buf(faceId, "{}", mc2::fmt::safe(ASE_NUM_TVFACES));

		faceData = strstr((char *)aseBuffer,faceId);
		if (faceData)
		{
			faceData += strlen(faceId);
			GetNumberData(faceData,numberData);
	
			Stuff::Vector3D tVFaces;
	
			mc2::fmt::to_buf(faceId, "{} {:d}", mc2::fmt::safe(ASE_MESH_TFACE_ID), mc2::fmt::as_i32(i));
	
			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != nullptr);
	
			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			tVFaces.x = (float)atol(numberData);
			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			tVFaces.y = (float)atol(numberData);
			faceData += strlen(numberData)+1;
	
			GetNumberData(faceData,numberData);
			tVFaces.z = (float)atol(numberData);
	
			mc2::fmt::to_buf(faceId, "{}", mc2::fmt::safe(ASE_NUM_TVERTEX));
	
			faceData = strstr((char *)aseBuffer,faceId);
			gosASSERT(faceData != nullptr);
	
			mc2::fmt::to_buf(faceId, "{} {:d}", mc2::fmt::safe(ASE_MESH_TVERT_ID), mc2::fmt::as_i32((int)tVFaces.x));
	
			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != nullptr);
	
			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			listOfTypeTriangles[i].uvdata.u0 = (float)atof(numberData);
	
			if ((listOfTypeTriangles[i].uvdata.u0 > 100.0f) || (listOfTypeTriangles[i].uvdata.u0 < -100.0f))
			{
#ifdef _DEBUG
				if (!silentMode)
					PAUSE(("WARNING: U0 is %f which is out of range in Shape %s!",listOfTypeTriangles[i].uvdata.u0,fileName));
#endif
				listOfTypeTriangles[i].uvdata.u0 = 0.0f;
			}
	
			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			listOfTypeTriangles[i].uvdata.v0 = 1.0f - (float)atof(numberData);
	
			if ((listOfTypeTriangles[i].uvdata.v0 > 100.0f) || (listOfTypeTriangles[i].uvdata.v0 < -100.0f))
			{
#ifdef _DEBUG
				if (!silentMode)
					PAUSE(("WARNING: V0 is %f which is out of range in Shape %s!",listOfTypeTriangles[i].uvdata.v0,fileName));
#endif
				listOfTypeTriangles[i].uvdata.v0 = 0.0f;
			}
	
			mc2::fmt::to_buf(faceId, "{}", mc2::fmt::safe(ASE_NUM_TVERTEX));
	
			faceData = strstr((char *)aseBuffer,faceId);
			gosASSERT(faceData != nullptr);
	
			mc2::fmt::to_buf(faceId, "{} {:d}", mc2::fmt::safe(ASE_MESH_TVERT_ID), mc2::fmt::as_i32((int)tVFaces.y));
	
			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != nullptr);
	
			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			listOfTypeTriangles[i].uvdata.u1 = (float)atof(numberData);
			if ((listOfTypeTriangles[i].uvdata.u1 > 100.0f) || (listOfTypeTriangles[i].uvdata.u1 < -100.0f))
			{
#ifdef _DEBUG
				if (!silentMode)
					PAUSE(("WARNING: U1 is %f which is out of range in Shape %s!",listOfTypeTriangles[i].uvdata.u1,fileName));
#endif
				listOfTypeTriangles[i].uvdata.u1 = 0.0f;
			}
	
			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			listOfTypeTriangles[i].uvdata.v1 = 1.0f - (float)atof(numberData);
			if ((listOfTypeTriangles[i].uvdata.v1 > 100.0f) || (listOfTypeTriangles[i].uvdata.v1 < -100.0f))
			{
#ifdef _DEBUG
				if (!silentMode)
					PAUSE(("WARNING: V1 is %f which is out of range in Shape %s!",listOfTypeTriangles[i].uvdata.v1,fileName));
#endif
				listOfTypeTriangles[i].uvdata.v1 = 0.0f;
			}
	
			mc2::fmt::to_buf(faceId, "{}", mc2::fmt::safe(ASE_NUM_TVERTEX));
	
			faceData = strstr((char *)aseBuffer,faceId);
			gosASSERT(faceData != nullptr);
	
			mc2::fmt::to_buf(faceId, "{} {:d}", mc2::fmt::safe(ASE_MESH_TVERT_ID), mc2::fmt::as_i32((int)tVFaces.z));
	
			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != nullptr);
	
			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			listOfTypeTriangles[i].uvdata.u2 = (float)atof(numberData);
			if ((listOfTypeTriangles[i].uvdata.u2 > 100.0f) || (listOfTypeTriangles[i].uvdata.u2 < -100.0f))
			{
#ifdef _DEBUG
				if (!silentMode)
					PAUSE(("WARNING: U2 is %f which is out of range in Shape %s!",listOfTypeTriangles[i].uvdata.u2,fileName));
#endif
				listOfTypeTriangles[i].uvdata.u2 = 0.0f;
			}
	
			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			listOfTypeTriangles[i].uvdata.v2 = 1.0f - (float)atof(numberData);
			if ((listOfTypeTriangles[i].uvdata.v2 > 100.0f) || (listOfTypeTriangles[i].uvdata.v2 < -100.0f))
			{
#ifdef _DEBUG
				if (!silentMode)
					PAUSE(("WARNING: V2 is %f which is out of range in Shape %s!",listOfTypeTriangles[i].uvdata.v2,fileName));
#endif
				listOfTypeTriangles[i].uvdata.v2 = 0.0f;
			}
		}
		else
		{
			listOfTypeTriangles[i].uvdata.u0 = 
			listOfTypeTriangles[i].uvdata.v0 = 
   			listOfTypeTriangles[i].uvdata.u1 = 
   			listOfTypeTriangles[i].uvdata.v1 = 
   			listOfTypeTriangles[i].uvdata.u2 = 
   			listOfTypeTriangles[i].uvdata.v2 = 0.0f;
		}


		mc2::fmt::to_buf(faceId, "{}", mc2::fmt::safe(ASE_NUM_CVFACES));

		faceData = strstr((char *)aseBuffer,faceId);
		if (faceData)
		{
			gosASSERT(faceData != nullptr);

			faceData += strlen(faceId);
			GetNumberData(faceData,numberData);

			Stuff::Vector3D cVFaces;

			mc2::fmt::to_buf(faceId, "{} {:d}", mc2::fmt::safe(ASE_MESH_CFACE_ID), mc2::fmt::as_i32(i));

			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != nullptr);

			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			cVFaces.x = (float)atol(numberData);
			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			cVFaces.y = (float)atol(numberData);
			faceData += strlen(numberData)+1;

			GetNumberData(faceData,numberData);
			cVFaces.z = (float)atol(numberData);

			mc2::fmt::to_buf(faceId, "{}", mc2::fmt::safe(ASE_NUM_CVERTEX));

			faceData = strstr((char *)aseBuffer,faceId);
			gosASSERT(faceData != nullptr);

			mc2::fmt::to_buf(faceId, "{} {:d}", mc2::fmt::safe(ASE_MESH_VERTCOL_ID), mc2::fmt::as_i32((int)cVFaces.x));

			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != nullptr);

			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			float red = (float)atof(numberData);

			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			float green = (float)atof(numberData);

			faceData += strlen(numberData)+1;

			GetNumberData(faceData,numberData);
			float blue = (float)atof(numberData);

			DWORD redC = red * 0xff;
			DWORD greenC = green * 0xff;
			DWORD blueC = blue * 0xff;

			listOfTypeVertices[listOfTypeTriangles[i].Vertices[0]].aRGBLight = (0xff << 24) + (redC << 16) + (greenC << 8) + blueC;

			mc2::fmt::to_buf(faceId, "{}", mc2::fmt::safe(ASE_NUM_CVERTEX));

			faceData = strstr((char *)aseBuffer,faceId);
			gosASSERT(faceData != nullptr);

			mc2::fmt::to_buf(faceId, "{} {:d}", mc2::fmt::safe(ASE_MESH_VERTCOL_ID), mc2::fmt::as_i32((int)cVFaces.y));

			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != nullptr);

			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			red = (float)atof(numberData);

			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			green = (float)atof(numberData);

			faceData += strlen(numberData)+1;

			GetNumberData(faceData,numberData);
			blue = (float)atof(numberData);

			redC = red * 0xff;
			greenC = green * 0xff;
			blueC = blue * 0xff;

			listOfTypeVertices[listOfTypeTriangles[i].Vertices[1]].aRGBLight = (0xff << 24) + (redC << 16) + (greenC << 8) + blueC;

			mc2::fmt::to_buf(faceId, "{}", mc2::fmt::safe(ASE_NUM_CVERTEX));

			faceData = strstr((char *)aseBuffer,faceId);
			gosASSERT(faceData != nullptr);

			mc2::fmt::to_buf(faceId, "{} {:d}", mc2::fmt::safe(ASE_MESH_VERTCOL_ID), mc2::fmt::as_i32((int)cVFaces.z));

			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != nullptr);

			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			red = (float)atof(numberData);

			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			green = (float)atof(numberData);

			faceData += strlen(numberData)+1;

			GetNumberData(faceData,numberData);
			blue = (float)atof(numberData);

			redC = red * 0xff;
			greenC = green * 0xff;
			blueC = blue * 0xff;

			listOfTypeVertices[listOfTypeTriangles[i].Vertices[2]].aRGBLight = (0xff << 24) + (redC << 16) + (greenC << 8) + blueC;
		}
		else
		{
			listOfTypeVertices[listOfTypeTriangles[i].Vertices[0]].aRGBLight = 
			listOfTypeVertices[listOfTypeTriangles[i].Vertices[1]].aRGBLight = 
			listOfTypeVertices[listOfTypeTriangles[i].Vertices[2]].aRGBLight = 0xff000000;
		}


		mc2::fmt::to_buf(faceId, "{} {:d}", mc2::fmt::safe(ASE_FACE_NORMAL_ID), mc2::fmt::as_i32(i));
		faceData = strstr((char *)aseBuffer,faceId);
		gosASSERT(faceData != nullptr);		

		faceData += strlen(faceId);
			
		GetNumberData(faceData,numberData);
		listOfTypeTriangles[i].faceNormal.x = -(float)atof(numberData);
		faceData += strlen(numberData)+1;
			
		GetNumberData(faceData,numberData);
		listOfTypeTriangles[i].faceNormal.z = (float)atof(numberData);
		faceData += strlen(numberData)+1;

		GetNumberData(faceData,numberData);
		listOfTypeTriangles[i].faceNormal.y = (float)atof(numberData);

		char vertexID[256];
		mc2::fmt::to_buf(vertexID, "{}", mc2::fmt::safe(ASE_VERTEX_NORMAL_ID));
		char *vertexData = strstr(faceData,vertexID);
		GetNumberData(vertexData,numberData);
		int vertexNum = atol(numberData);
		vertexNormalCount[vertexNum]++;

		vertexData += strlen(vertexID)+strlen(numberData)+1;

			GetNumberData(vertexData,numberData);
			vertexData += strlen(numberData)+1;
			listOfTypeVertices[vertexNum].normal.x += -(float)atof(numberData);

			GetNumberData(vertexData,numberData);
			vertexData += strlen(numberData)+1;
			listOfTypeVertices[vertexNum].normal.z += (float)atof(numberData);

			GetNumberData(vertexData,numberData);
			listOfTypeVertices[vertexNum].normal.y += (float)atof(numberData);

		vertexData = strstr(vertexData,vertexID);
		GetNumberData(vertexData,numberData);
		vertexNum = atol(numberData);
		vertexNormalCount[vertexNum]++;

		vertexData += strlen(vertexID)+strlen(numberData)+1;

			GetNumberData(vertexData,numberData);
			vertexData += strlen(numberData)+1;
			listOfTypeVertices[vertexNum].normal.x += -(float)atof(numberData);

			GetNumberData(vertexData,numberData);
			vertexData += strlen(numberData)+1;
			listOfTypeVertices[vertexNum].normal.z += (float)atof(numberData);

			GetNumberData(vertexData,numberData);
			listOfTypeVertices[vertexNum].normal.y += (float)atof(numberData);

		vertexData = strstr(vertexData,vertexID);
		GetNumberData(vertexData,numberData);
		vertexNum = atol(numberData);
		vertexNormalCount[vertexNum]++;

		vertexData += strlen(vertexID)+strlen(numberData)+1;

			GetNumberData(vertexData,numberData);
			vertexData += strlen(numberData)+1;
			listOfTypeVertices[vertexNum].normal.x += -(float)atof(numberData);

			GetNumberData(vertexData,numberData);
			vertexData += strlen(numberData)+1;
			listOfTypeVertices[vertexNum].normal.z += (float)atof(numberData);

			GetNumberData(vertexData,numberData);
			listOfTypeVertices[vertexNum].normal.y += (float)atof(numberData);

	}

	//-----------------------------------------------
	// Must average together the vertexNormals
	for (int i=0;i<numTypeVertices;i++)
	{
		if (vertexNormalCount[i] == 0)
		{
			listOfTypeVertices[i].normal.x = 0.0f;
			listOfTypeVertices[i].normal.y = 0.0f;
			listOfTypeVertices[i].normal.z = 0.0f;
		}
		else
		{
			listOfTypeVertices[i].normal.x /= float(vertexNormalCount[i]);
			listOfTypeVertices[i].normal.y /= float(vertexNormalCount[i]);
			listOfTypeVertices[i].normal.z /= float(vertexNormalCount[i]);

			if (listOfTypeVertices[i].normal.GetLength() > Stuff::SMALL)
				listOfTypeVertices[i].normal.Normalize(listOfTypeVertices[i].normal);
		}
	}

	return(0);
}	

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  -1 if file not found or file not ASE Format.		
//This function loads the ASE file into the TG_Triangle and TG_Vertex lists.
//It allocates memory.  These are straight tglHeap->Mallocs at present.
//
// NOTE: Only takes the first GEOMOBJECT from the ASE file.  Multi-object
// Files will require user intervention to parse!!
int TG_TypeShape::LoadTGShapeFromASE (const char *fileName)
{
	//-----------------------------------------------------
	// Fully loads and parses an ASE File.  These files are
	// output by 3D Studio MAX's ASCII Scene Exporter.

	long aseFileSize = gos_FileSize(fileName);
	gosASSERT(aseFileSize > 0);

	// One zero byte past the file: the header and texture searches below run string functions over the contents.
	std::vector<BYTE> aseStore(static_cast<size_t>(aseFileSize) + 1, 0);
	BYTE *aseContents = aseStore.data();

	HGOSFILE aseFile;
	gos_OpenFile(&aseFile,fileName,READONLY);

#ifdef _DEBUG
	long dataRead = 
#endif
		gos_ReadFile(aseFile,aseContents,aseFileSize);
	gosASSERT(dataRead == aseFileSize);

	gos_CloseFile(aseFile);

	gosASSERT(S_strnicmp(ASE_HEADER,(char *)aseContents,strlen(ASE_HEADER)) == 0);

	char textureId[256];
	char *textureData;

	char numberData[256];
	sprintf(textureId,ASE_MATERIAL_COUNT);
	textureData = strstr((char *)aseContents,textureId);
	if (!textureData)
		return(-1);

	textureData += strlen(ASE_MATERIAL_COUNT)+1;
	GetNumberData(textureData,numberData);
	long numMaterials = atol(numberData);

	numTextures = 0;
	unsigned char *aseBuffer = aseContents;
	for (long nmt=0;nmt<numMaterials;nmt++)
	{
		sprintf(textureId,ASE_MATERIAL_CLASS);
		textureData = strstr((char *)aseBuffer,textureId);
		if (!textureData)
			return(-1);

		textureData += strlen(ASE_MATERIAL_CLASS)+1;
		GetWordData(textureData,numberData);

		if (strstr(numberData,"Standard"))
		{
			numTextures++;
		}
		else if (strstr(numberData,"Multi"))
		{
			//----------------------------------------------------
			//NOT SUPPORTED YET!  Multiple Textures per building!
			// Support NOW!  Must have trees!
			textureData = strstr(textureData,ASE_SUBMATERIAL_COUNT);
			gosASSERT(textureData != nullptr);

			textureData += strlen(ASE_SUBMATERIAL_COUNT);
			GetNumberData(textureData,numberData);

			numTextures += atol(numberData);
		}

		aseBuffer = (unsigned char *)textureData;
	}

	tinyTextureStore.assign(numTextures, TG_TinyTexture{});
	listOfTextures = tinyTextureStore.data();

	for (long i=0;i<numTextures;i++)
	{
		//-------------------------------------------------------------------------------
		// Get and store texture Name.  Will need multiple for Multi-Sub if implemented
		// NEVER GET ACTUAL TEXTURES HERE.  ALWAYS HAPPENS IN MULTI!!!!!!!!!!
		// Just set 'em!
		listOfTextures[i].mcTextureNodeIndex = 0xffffffff;
		listOfTextures[i].gosTextureHandle = 0xffffffff;
		listOfTextures[i].textureAlpha = false;
	}

	//---------------------------------------------------
	// Calling from top will load just first GEOMOBJECT!
	long parseResult = ParseASEFile(aseContents,fileName);

	return(parseResult);
}	

//-------------------------------------------------------------------------------
//Need this so that Multi-Shapes can let each shape know texture info.
void TG_TypeShape::CreateListOfTextures (TG_TexturePtr list, DWORD numTxms)
{
	numTextures = numTxms;
	if (numTextures)
	{
		tinyTextureStore.resize(numTextures);
		listOfTextures = tinyTextureStore.data();

	   for (long i=0;i<numTextures;i++)
	   {
		   //-------------------------------------------------------------------------------
		   // Get and store texture Name.  Will need multiple for Multi-Sub if implemented
		   // NEVER GET ACTUAL TEXTURES HERE.  ALWAYS HAPPENS IN MULTI!!!!!!!!!!
		   // Just set 'em!
		   listOfTextures[i].mcTextureNodeIndex = list[i].mcTextureNodeIndex;
		   listOfTextures[i].gosTextureHandle = list[i].gosTextureHandle; 
		   listOfTextures[i].textureAlpha = list[i].textureAlpha;
	   }
 	}
	else
		listOfTextures = nullptr;
}	

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
//This function takes the gosTextureHandle passed in and assigns it to the
//textureNum entry of the listOfTextures;
long TG_TypeShape::SetTextureHandle (DWORD textureNum, DWORD gosTextureHandle)
{
	if (textureNum >= numTextures)
		return(-1);

	listOfTextures[textureNum].mcTextureNodeIndex = gosTextureHandle;
	listOfTextures[textureNum].gosTextureHandle = mcTextureManager->get_gosTextureHandle(gosTextureHandle);

	return(0);
}	

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
//This function takes the gosTextureHandle passed in and assigns it to the
//textureNum entry of the listOfTextures;
long TG_TypeShape::SetTextureAlpha (DWORD textureNum, bool alphaFlag)
{
	if (textureNum >= numTextures)
		return(-1);

	listOfTextures[textureNum].textureAlpha = alphaFlag;

	return(0);
}

//-------------------------------------------------------------------------------
// Track D — populate this shape from a format-agnostic importer (FBX/GLB).
// The caller moves in its vertex and triangle vectors; the shape owns them and copies node identity. The shape's
// init() defaults (alphaTestOn=false, filterOn=true, hot* sentinels) remain
// in place; callers that need different values use the existing setters.
void TG_TypeShape::InitFromImportedMesh(const char* nodeIdIn, const char* parentIdIn,
                                        const Stuff::Point3D& center,
                                        std::vector<TG_TypeVertex>&& vertices,
                                        std::vector<TG_TypeTriangle>&& triangles)
{
	// Node identity. VALIDATE-SCENE-ASSIMP-NAME-LEN-1: GLB node names may exceed
	// the fixed TG_NODE_ID field; strncpy truncates safely. Log original->truncated
	// so the mapping is visible (matters for M2 animation binding; static import
	// does not bind by name). Stock ASE names already fit, so this never fires for them.
	if (nodeIdIn && strlen(nodeIdIn) >= TG_NODE_ID) {
		char trunc[TG_NODE_ID];
		strncpy(trunc, nodeIdIn, TG_NODE_ID - 1);
		trunc[TG_NODE_ID - 1] = '\0';
		mc2::fmt::print_out("[MECH_SKEL] nodeId truncated: '{}' -> '{}'\n", mc2::fmt::safe(nodeIdIn), mc2::fmt::safe(trunc));
		fflush(stdout);
	}
	strncpy(nodeId, nodeIdIn ? nodeIdIn : "", TG_NODE_ID - 1);
	nodeId[TG_NODE_ID - 1] = '\0';
	strncpy(parentId, parentIdIn ? parentIdIn : "None", TG_NODE_ID - 1);
	parentId[TG_NODE_ID - 1] = '\0';

	// Node transform pivot in MC2 coordinate space (importer applies the
	// 3DS-Max → MC2 axis flip before calling us).
	nodeCenter = center;
	relativeNodeCenter = center;

	// Take ownership of the caller's geometry; the lists stay views of the stores.
	typeVertexStore    = std::move(vertices);
	typeTriangleStore  = std::move(triangles);
	numTypeVertices    = static_cast<DWORD>(typeVertexStore.size());
	numTypeTriangles   = static_cast<DWORD>(typeTriangleStore.size());
	listOfTypeVertices  = typeVertexStore.data();
	listOfTypeTriangles = typeTriangleStore.data();
}
