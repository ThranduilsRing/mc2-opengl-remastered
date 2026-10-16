// TG shapes: ASE number/name helpers and the TG_TypeNode / TG_TypeShape / TG_Shape lifecycle, binary copy and creation.
// Declared in the class headers; shared names in tgl_internal.h.
#include "tgl_internal.h"
#include "mc2fmt.h"


//-------------------------------------------------------------------------------
// EDGE-LIGHT-BILLBOARD-CULL-1 (master-FAIL #8): case-insensitive substring test
// used at shape load to flag beacon/runway edge-light glow cards. Node names in
// stock content are "Redbeaconlight02/03", "Greenbeaconlight01", "Bluebeaconlight"
// (runwaylight.tgl uses "Redbeaconlight03"); all share the "beaconlight" token,
// which cleanly excludes the flags and the barrels/streetlight props that share
// the Lightsnflags.tga atlas. These cards are single flat quads with no
// billboarding, so they backface-cull (vanish) as the RTS camera orbits toward
// top-down; Render() renders them two-sided when the gate below is on.
static bool tgNameContainsCI(const char* hay, const char* needle)
{
	if (!hay || !needle) return false;
	const size_t nlen = strlen(needle);
	if (nlen == 0) return false;
	for (const char* p = hay; *p; ++p)
		if (S_strnicmp(p, needle, (int)nlen) == 0)
			return true;
	return false;
}

void GetNumberData (char *rawData, char *result)
{
	long startIndex = 0;
	long endIndex = 0;
	while (	(rawData[startIndex] != '+') &&
			(rawData[startIndex] != '-') &&
			(rawData[startIndex] != '.') &&
			((rawData[startIndex] < '0') ||
			(rawData[startIndex] > '9')))
	{
		startIndex++;
	}

	endIndex = startIndex;
	while (	(rawData[endIndex] == '+') ||
			(rawData[endIndex] == '-') ||
			(rawData[endIndex] == '.') ||
			((rawData[endIndex] >= '0') &&
			(rawData[endIndex] <= '9')))
	{
		endIndex++;
	}

	strncpy(result,&rawData[startIndex],endIndex - startIndex);
	result[endIndex-startIndex] = 0;
}	

void GetWordData (char *rawData, char *result)
{
	long startIndex = 0;
	long endIndex = 0;
	while (	(rawData[startIndex] < 'A') ||
			((rawData[startIndex] > 'Z') &&
			(rawData[startIndex] < 'a')) ||
			(rawData[startIndex] > 'z'))
	{
		startIndex++;
	}

	endIndex = startIndex;
	while (	((rawData[endIndex] >= 'A') && (rawData[endIndex] <= 'Z')) ||
			(rawData[endIndex] == '_') ||
			((rawData[endIndex] >= 'a') && (rawData[endIndex] <= 'z')) )
	{
		endIndex++;
	}

	strncpy(result,&rawData[startIndex],endIndex - startIndex);
	result[endIndex-startIndex] = 0;
}	

void GetNameData (char *rawData, char *result)
{
	long startIndex = 0;
	long endIndex = 0;
	while (	(rawData[startIndex] != '"') )
	{
		startIndex++;
	}

	startIndex++;
	endIndex = startIndex;
	while (	(rawData[endIndex] != '"') )
	{
		endIndex++;
	}

	strncpy(result,&rawData[startIndex],endIndex - startIndex);
	result[endIndex-startIndex] = 0;
}	


 void *TG_TypeNode::operator new (size_t mySize)
 {
	void *result = TG_Shape::tglHeap->Malloc(mySize);
	return result;
 }
 
 void TG_TypeNode::operator delete (void *us)
 {
	TG_Shape::tglHeap->Free(us);
 }
 
void TG_TypeNode::destroy (void)
{
	init();
}

//-------------------------------------------------------------------------------
// This function creates an instance of a TG_Shape from a TG_TypeNode.  Saves RAM.
TG_ShapePtr TG_TypeNode::CreateFrom (void)
{
	TG_ShapePtr newShape = nullptr;
	newShape = (TG_ShapePtr)TG_Shape::tglHeap->Malloc(sizeof(TG_Shape));
	gosASSERT(newShape != nullptr);
	
	//listOfVertices
	newShape->numVertices = 0;
	newShape->listOfColors = nullptr;
	newShape->listOfVertices = nullptr;
	newShape->listOfShadowVertices = nullptr;

	//listOfTriangles
	newShape->numTriangles = 0;
	newShape->listOfTriangles = nullptr;

	//listOfVisibleFaces
	newShape->numVisibleFaces = 0;
	newShape->listOfVisibleFaces = nullptr;

	//listOfVisibleShadows
	newShape->numVisibleShadows = 0;
	newShape->listOfVisibleShadows = nullptr;

	//Other Data
	newShape->myType = this;
	if (newShape->myType == nullptr)
		STOP(("ShapeType NULL.  BAD Shape in MAX FILE"));
		
	newShape->aRGBHighlight = 0x00000000;
	newShape->lightsOut = false;
	newShape->recalcShadows = true;
	newShape->shapeScalar = 0.0f;

	for (long i=0;i<MAX_SHADOWS;i++)
		newShape->shadowsVisible[i] = false;

	newShape->isSpotlight = (S_strnicmp(newShape->getNodeName(),"SpotLight_",10) == 0);
	newShape->isWindow = (S_strnicmp(newShape->getNodeName(),"LitWin_",6) == 0);
	// EDGE-LIGHT-BILLBOARD-CULL-1 (master-FAIL #8): flag beacon/runway edge-light
	// glow cards (see tgNameContainsCI above) so Render() can render them two-sided.
	newShape->isLightBillboard = tgNameContainsCI(newShape->getNodeName(), "beaconlight");

	if (newShape->isSpotlight)
		newShape->noShadow = true;
	else
		newShape->noShadow = false;

	return newShape;
}

//-------------------------------------------------------------------------------
//Function return 0 is OK.  -1 if file is not ASE Format or missing data.
//This function simply parses the ASE buffers handed to it.  This allows
//users to load the ase file themselves and manage their own memory for it.
//It allocates memory for internal Lists.  These are straight tglHeap->Mallocs at present.
//
long TG_TypeNode::MakeFromHelper (BYTE *aseBuffer, const char *fileName)
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

	strncpy(nodeId,nodeString, TG_NODE_ID-1);
	nodeId[TG_NODE_ID - 1] = '\0'; //sebi
		 
	char* parentName = strstr(nodeName,ASE_NODE_PARENT);

	//-------------------------------------------------------------------
	// Must also check to make sure we HAVE a parent.
	// We will get the next GeomObject's parent if we don't check length!
	if ((parentName != nullptr) && ((parentName - nodeName) < MAX_SCAN_LENGTH))
	{
		parentName += strlen(ASE_NODE_PARENT)+1;
		GetNameData(parentName,nodeString);
		
		strncpy(parentId,nodeString, TG_NODE_ID-1);
		parentId[TG_NODE_ID - 1] = '\0'; //sebi
	}
	else
	{
		strcpy(parentId,"None");
	}

	//----------------------------------------------------
	// Store off NODE ABS position for heirarchy
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

	return(0);
}	

bool TG_TypeNode::LoadBinaryCopy (File &binFile)
{
	//Other Data
	nodeCenter.x = binFile.readFloat();
	nodeCenter.y = binFile.readFloat();
	nodeCenter.z = binFile.readFloat();
	
	relativeNodeCenter.x = binFile.readFloat();
	relativeNodeCenter.y = binFile.readFloat();
	relativeNodeCenter.z = binFile.readFloat();
	
	binFile.read((MemoryPtr)nodeId,TG_NODE_ID);
	binFile.read((MemoryPtr)parentId,TG_NODE_ID);
	return true;
}

void TG_TypeNode::SaveBinaryCopy (File &binFile)
{
	binFile.writeInt(TYPE_NODE);
	
	//Other Data
	binFile.writeFloat(nodeCenter.x);
	binFile.writeFloat(nodeCenter.y);
	binFile.writeFloat(nodeCenter.z);
	
	binFile.writeFloat(relativeNodeCenter.x);
	binFile.writeFloat(relativeNodeCenter.y);
	binFile.writeFloat(relativeNodeCenter.z);
	
	binFile.write((MemoryPtr)nodeId,TG_NODE_ID);
	binFile.write((MemoryPtr)parentId,TG_NODE_ID);
}

//-------------------------------------------------------------------------------
// Class TG_Shape && TG_TypeShape

//-------------------------------------------------------------------------------
// TG_TypeShape
// This function creates an instance of a TG_Shape from a TG_TypeShape.  Saves RAM.
TG_ShapePtr TG_TypeShape::CreateFrom (void)
{
	TG_ShapePtr newShape = nullptr;
	void* memarea = TG_Shape::tglHeap->Malloc(sizeof(TG_Shape));
	newShape = ::new(memarea) TG_Shape();
	gosASSERT(newShape != nullptr);
	
	//listOfVertices
	newShape->numVertices = numTypeVertices;
	if (numTypeVertices)
	{
		//Mark with invalid pointer so we now to get RAM from POOL! 	
		newShape->listOfVertices = nullptr;
		newShape->listOfColors = nullptr;
	}
	else
	{
		//Otherwise mark with NULL so we know not to reference!!
		newShape->listOfVertices = nullptr;
		newShape->listOfColors = nullptr;
	}

	//listOfShadowVertices
	if (numTypeVertices)
	{
		//These are UNIQUE to each instance and do not change much per frame.
		// Thus, store them between frames to save processing time!
		newShape->listOfShadowVertices = (TG_ShadowVertexPtr)TG_Shape::tglHeap->Malloc(sizeof(TG_ShadowVertex) * numTypeVertices * MAX_SHADOWS);
		gosASSERT(newShape->listOfShadowVertices != nullptr);

		memset(newShape->listOfShadowVertices,0xff,sizeof(TG_ShadowVertex) * numTypeVertices * MAX_SHADOWS);
		
		//Mark with invalid pointer so we now to get RAM from POOL! 	
		newShape->listOfShadowTVertices = nullptr;
	}
	else
	{
		//Otherwise mark with NULL so we know not to reference!!
		newShape->listOfShadowVertices = nullptr;
		newShape->listOfShadowTVertices = nullptr;
	}

	//listOfTriangles
	newShape->numTriangles = numTypeTriangles;
	if (numTypeTriangles)
	{
		//Mark with invalid pointer so we now to get RAM from POOL! 	
		newShape->listOfTriangles = nullptr;
	}
	else
	{
		//Otherwise mark with NULL so we know not to reference!!
		newShape->listOfTriangles = nullptr;
	}

	//listOfVisibleFaces
	newShape->numVisibleFaces = 0;
	if (numTypeTriangles)
	{
		//Mark with invalid pointer so we now to get RAM from POOL! 	
		newShape->listOfVisibleFaces = nullptr;
	}
	else
	{
		//Otherwise mark with NULL so we know not to reference!!
		newShape->listOfVisibleFaces = nullptr;
	}

	//listOfVisibleShadows
	newShape->numVisibleShadows = 0;
	if (numTypeTriangles)
	{
		//Mark with invalid pointer so we now to get RAM from POOL! 	
		newShape->listOfVisibleShadows = nullptr;
	}
	else
	{
		//Otherwise mark with NULL so we know not to reference!!
		newShape->listOfVisibleShadows = nullptr;
	}

	//Other Data
	newShape->myType = this;
	if (newShape->myType == nullptr)
		STOP(("ShapeType NULL.  BAD Shape in MAX FILE"));
 	
	newShape->aRGBHighlight = 0x00000000;
	newShape->lightsOut = false;
	newShape->recalcShadows = true;
	newShape->shapeScalar = 0.0f;

	for (long i=0;i<MAX_SHADOWS;i++)
		newShape->shadowsVisible[i] = false;
		
	newShape->isSpotlight = (S_strnicmp(newShape->getNodeName(),"SpotLight_",10) == 0);
	newShape->isWindow = (S_strnicmp(newShape->getNodeName(),"LitWin_",6) == 0);
	// EDGE-LIGHT-BILLBOARD-CULL-1 (master-FAIL #8): flag beacon/runway edge-light
	// glow cards (see tgNameContainsCI above) so Render() can render them two-sided.
	newShape->isLightBillboard = tgNameContainsCI(newShape->getNodeName(), "beaconlight");

	if (newShape->isSpotlight)
		newShape->noShadow = true;
	else
		newShape->noShadow = false;

	return newShape;
}

bool TG_TypeShape::LoadBinaryCopy (File &binFile)
{
	gosVERTEX_FORMAT_RECORD vdecl[] =
	{ 
		{0, 3, false, sizeof(TG_HWTypeVertex), 0, gosVERTEX_ATTRIB_TYPE::FLOAT },
		{1, 3, false, sizeof(TG_HWTypeVertex), offsetof(TG_HWTypeVertex, normal) , gosVERTEX_ATTRIB_TYPE::FLOAT },
		{2, 4, true, sizeof(TG_HWTypeVertex), offsetof(TG_HWTypeVertex, aRGBLight) , gosVERTEX_ATTRIB_TYPE::UNSIGNED_BYTE},
		{3, 2, false, sizeof(TG_HWTypeVertex), offsetof(TG_HWTypeVertex, u) , gosVERTEX_ATTRIB_TYPE::FLOAT },
	};

	vdecl_ = gos_CreateVertexDeclaration(vdecl, sizeof(vdecl) / sizeof(gosVERTEX_FORMAT_RECORD));

	//listOfTypeVertices
	numTypeVertices = binFile.readInt();
	if (numTypeVertices > kMaxShapeVertices)
	{
		MC2_LOG(Io, Error, "[SHAPE_COUNT_BAD] vertices={} (over {}; shape left empty)", numTypeVertices, kMaxShapeVertices);
		numTypeVertices = 0;
		return false;
	}
	if (numTypeVertices)
	{
		typeVertexStore.resize(numTypeVertices);
		listOfTypeVertices = typeVertexStore.data();

		binFile.read((MemoryPtr)listOfTypeVertices,sizeof(TG_TypeVertex) * numTypeVertices);

	}
	else
	{
		listOfTypeVertices = nullptr;
	}

	//listOfTypeTriangles
	numTypeTriangles = binFile.readInt();
	if (numTypeTriangles > kMaxShapeTriangles)
	{
		MC2_LOG(Io, Error, "[SHAPE_COUNT_BAD] triangles={} (over {}; shape left without triangles)", numTypeTriangles, kMaxShapeTriangles);
		numTypeTriangles = 0;
		return false;
	}
	if (numTypeTriangles)
	{
		typeTriangleStore.resize(numTypeTriangles);
		listOfTypeTriangles = typeTriangleStore.data();

		binFile.read((MemoryPtr)listOfTypeTriangles,sizeof(TG_TypeTriangle) * numTypeTriangles);
	}
	else
	{
		listOfTypeTriangles = nullptr;
	}

	//listOfTextures
	numTextures = binFile.readInt();
	if (numTextures > kMaxShapeTextures)
	{
		MC2_LOG(Io, Error, "[SHAPE_COUNT_BAD] textures={} (over {}; shape left without textures)", numTextures, kMaxShapeTextures);
		numTextures = 0;
		return false;
	}
	if (numTextures)
	{
		tinyTextureStore.resize(numTextures);
		listOfTextures = tinyTextureStore.data();

		binFile.read((MemoryPtr)listOfTextures,sizeof(TG_TinyTexture) * numTextures);
	}
	else
	{
		listOfTextures = nullptr;
	}

	if (numTypeVertices)
	{
		// no real index buffer for now...
		uint32_t num_vertices = numTypeTriangles * 3;
		uint32_t num_indices = numTypeTriangles * 3;

		uint16_t* ib = new uint16_t[num_indices];
		TG_HWTypeVertex* vb = new TG_HWTypeVertex[num_vertices];

		// sebi 2026-04-22: bounds-check triangle vertex indices. Wolfman
		// MC2X mech .tgl binaries sometimes have out-of-range Vertices[j]
		// (likely from a slightly different struct layout in his MakeASE);
		// previously we'd index `listOfTypeVertices[<out_of_range>]` and
		// crash with a mid-heap read violation. Now we fall back to vertex 0
		// and keep loading — the mesh will look wrong but the mech won't
		// crash the mechbay on open.
		static unsigned int s_mslBoundsWarnings = 0;
		for (uint32_t i = 0; i < numTypeTriangles; i++)
		{
			TG_TypeTriangle triType = listOfTypeTriangles[i];

			for (uint32_t j = 0; j < 3; ++j)
			{
				int vidx = triType.Vertices[j];
				if (vidx < 0 || vidx >= numTypeVertices) {
					if (s_mslBoundsWarnings++ < 8) {
						mc2::fmt::print_out("[MSL] triangle {:d} vertex {:d} index {:d} out of range [0,{:d})\n", mc2::fmt::as_u32(i), mc2::fmt::as_u32(j), mc2::fmt::as_i32(vidx), mc2::fmt::as_i32((int)numTypeVertices)); fflush(stdout);
					}
					vidx = 0;
				}

				vb[3 * i + j].aRGBLight = listOfTypeVertices[vidx].aRGBLight;
				vb[3 * i + j].normal = listOfTypeVertices[vidx].normal;
				vb[3 * i + j].position = listOfTypeVertices[vidx].position;

				ib[3 * i + j] = 3 * i + j;
			}

			vb[3*i + 0].u = triType.uvdata.u0;
			vb[3*i + 0].v = triType.uvdata.v0;
			vb[3*i + 1].u = triType.uvdata.u1;
			vb[3*i + 1].v = triType.uvdata.v1;
			vb[3*i + 2].u = triType.uvdata.u2;
			vb[3*i + 2].v = triType.uvdata.v2;
		}

		ib_ = gos_CreateBuffer(gosBUFFER_TYPE::INDEX, gosBUFFER_USAGE::STATIC_DRAW, sizeof(uint16_t), num_indices, ib);
		vb_ = gos_CreateBuffer(gosBUFFER_TYPE::VERTEX, gosBUFFER_USAGE::STATIC_DRAW, sizeof(TG_HWTypeVertex), num_vertices, vb);

		delete[] ib;
		delete[] vb;
	}


	//Other Data
	nodeCenter.x = binFile.readFloat();
	nodeCenter.y = binFile.readFloat();
	nodeCenter.z = binFile.readFloat();
	
	relativeNodeCenter.x = binFile.readFloat();
	relativeNodeCenter.y = binFile.readFloat();
	relativeNodeCenter.z = binFile.readFloat();
	
	alphaTestOn = false;
	filterOn = true;

	binFile.read((MemoryPtr)nodeId,TG_NODE_ID);
	binFile.read((MemoryPtr)parentId,TG_NODE_ID);
	return true;
}

void TG_TypeShape::SaveBinaryCopy (File &binFile)
{
	binFile.writeInt(SHAPE_NODE);
	
	//listOfTypeVertices
	binFile.writeInt(numTypeVertices);
	if (numTypeVertices)
	{
		binFile.write((MemoryPtr)listOfTypeVertices,sizeof(TG_TypeVertex) * numTypeVertices);
	}

	//listOfTypeTriangles
	binFile.writeInt(numTypeTriangles);
	if (numTypeTriangles)
	{
		binFile.write((MemoryPtr)listOfTypeTriangles,sizeof(TG_TypeTriangle) * numTypeTriangles);
	}

	//listOfTextures
	binFile.writeInt(numTextures);
	if (numTextures)
	{
		binFile.write((MemoryPtr)listOfTextures,sizeof(TG_TinyTexture) * numTextures);
	}

	//Other Data
	binFile.writeFloat(nodeCenter.x);
	binFile.writeFloat(nodeCenter.y);
	binFile.writeFloat(nodeCenter.z);
	
	binFile.writeFloat(relativeNodeCenter.x);
	binFile.writeFloat(relativeNodeCenter.y);
	binFile.writeFloat(relativeNodeCenter.z);
	
	binFile.write((MemoryPtr)nodeId,TG_NODE_ID);
	binFile.write((MemoryPtr)parentId,TG_NODE_ID);
}

void TG_TypeShape::destroy (void)
{
	std::vector<TG_TypeVertex>().swap(typeVertexStore);
	listOfTypeVertices = nullptr;

	std::vector<TG_TypeTriangle>().swap(typeTriangleStore);
	listOfTypeTriangles = nullptr;

	std::vector<TG_TinyTexture>().swap(tinyTextureStore);
	listOfTextures = nullptr;

	numTypeVertices = numTypeTriangles = numTextures = 0;

	if (vb_) {
		gos_DestroyBuffer(vb_);
		vb_ = nullptr;
	}

	if (ib_) {
		gos_DestroyBuffer(ib_);
		ib_ = nullptr;
	}

	if (vdecl_) {
		gos_DestroyVertexDeclaration(vdecl_);
		vdecl_ = nullptr;
	}
}	

//-------------------------------------------------------------------------------
// 2026-05-10: out-of-line so myType (protected) stays inside the class.
// SHAPE_NODE accessor for non-friend callers — see tgl.h declaration.
bool TG_Shape::IsShapeNode() const
{
	if (!myType) return false;
	return const_cast<TG_TypeNodePtr>(myType)->GetNodeType() == SHAPE_NODE;
}

//-------------------------------------------------------------------------------
// FOLIAGE-ALPHAFLAG-1: out-of-line for the same reason as IsShapeNode above —
// myType is protected. Only SHAPE_NODEs have a TG_TypeShape with alphaTestOn;
// anything else classifies as not-alpha-test.
bool TG_Shape::GetIsAlphaTest() const
{
	if (!IsShapeNode()) return false;
	return static_cast<const TG_TypeShape*>(myType)->alphaTestOn;
}

void TG_Shape::destroy (void)
{
	//THIS IS CORRECT!!!!
	// They come from a common pool now, Deleteing them would be BAD!!
	listOfVertices = nullptr;
	listOfColors = nullptr;
	listOfTriangles = nullptr;
	listOfVisibleFaces = nullptr;
	listOfVisibleShadows = nullptr;
	listOfShadowTVertices = nullptr;

	if (listOfShadowVertices)
		tglHeap->Free(listOfShadowVertices);
	listOfShadowVertices = nullptr;

	numVertices = numTriangles = numVisibleFaces = 0;
}	

//-------------------------------------------------------------------------------
//Function return 0 is OK.  -1 if file is not ASE Format or missing data.
//This function simply parses the ASE buffers handed to it.  This allows
//users to load the ase file themselves and manage their own memory for it.
//It allocates memory for internal Lists.  These are straight tglHeap->Mallocs at present.
//
// NOTE: Only takes the first HELPOBJECT from the ASE file.  Multi-object
// Files will require user intervention to parse!!
long TG_TypeShape::MakeFromHelper (BYTE *aseBuffer, const char *fileName)
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

	numTypeVertices = 0;

	numTypeTriangles = 0;

	return(0);
}	
