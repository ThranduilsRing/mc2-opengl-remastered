// TG shapes: ASE number/name helpers and the TG_TypeNode / TG_TypeShape / TG_Shape lifecycle, binary copy and creation.
// Declared in the class headers; shared names in tgl_internal.h.
#include "mc2_czstring.h"
#include <memory>
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
static bool tgNameContainsCI(mc2::czstring hay, mc2::czstring needle)
{
	if (!hay || !needle) return false;
	const size_t nlen = strlen(needle);
	if (nlen == 0) return false;
	const std::string_view haystack(hay);
	for (size_t i = 0; i < haystack.size(); ++i)
		if (S_strnicmp(hay + i, needle, static_cast<int>(nlen)) == 0)
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


void TG_TypeNode::destroy (void)
{
    stableIdentity_.renew(this);
	init();
}

//-------------------------------------------------------------------------------
// This function creates an instance of a TG_Shape from a TG_TypeNode.  Saves RAM.
std::unique_ptr<TG_Shape> TG_TypeNode::CreateFrom (void)
{
	auto newShape = std::make_unique<TG_Shape>();
	gosASSERT(newShape != nullptr);

	newShape->numVertices = 0;
	newShape->numTriangles = 0;

	//Other Data
	newShape->myType = this;
	if (newShape->myType == nullptr)
		STOP(("ShapeType NULL.  BAD Shape in MAX FILE"));
		
	newShape->aRGBHighlight = 0x00000000;
	newShape->lightsOut = false;
	newShape->shapeScalar = 0.0f;

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
//It allocates memory for internal Lists (vector stores owned by the shape).
//
long TG_TypeNode::MakeFromHelper (BYTE *aseBuffer, mc2::czstring fileName)
{
	char *nodeName = strstr(reinterpret_cast<char *>(aseBuffer),ASE_NODE_NAME);
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
	nodeName = strstr(reinterpret_cast<char *>(aseBuffer),ASE_NODE_POS);
	gosASSERT(nodeName != nullptr);

	nodeName += strlen(ASE_NODE_POS)+1;

	char numData[512];

	GetNumberData(nodeName,numData);
	nodeCenter.x = -static_cast<float>(atof(numData));
	nodeName += strlen(numData)+1;

	GetNumberData(nodeName,numData);
	nodeCenter.z = static_cast<float>(atof(numData));
	nodeName += strlen(numData)+1;

	GetNumberData(nodeName,numData);
	nodeCenter.y = static_cast<float>(atof(numData));

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
	
	binFile.read(reinterpret_cast<MemoryPtr>(nodeId),TG_NODE_ID);
	binFile.read(reinterpret_cast<MemoryPtr>(parentId),TG_NODE_ID);
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
	
	binFile.write(reinterpret_cast<MemoryPtr>(nodeId),TG_NODE_ID);
	binFile.write(reinterpret_cast<MemoryPtr>(parentId),TG_NODE_ID);
}

//-------------------------------------------------------------------------------
// Class TG_Shape && TG_TypeShape

//-------------------------------------------------------------------------------
// TG_TypeShape
// This function creates an instance of a TG_Shape from a TG_TypeShape.  Saves RAM.
std::unique_ptr<TG_Shape> TG_TypeShape::CreateFrom (void)
{
	auto newShape = std::make_unique<TG_Shape>();
	gosASSERT(newShape != nullptr);
	
	newShape->numVertices = numTypeVertices;
	newShape->numTriangles = numTypeTriangles;

	//Other Data
	newShape->myType = this;
	if (newShape->myType == nullptr)
		STOP(("ShapeType NULL.  BAD Shape in MAX FILE"));
 	
	newShape->aRGBHighlight = 0x00000000;
	newShape->lightsOut = false;
	newShape->shapeScalar = 0.0f;
		
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

		binFile.read(reinterpret_cast<MemoryPtr>(listOfTypeVertices),sizeof(TG_TypeVertex) * numTypeVertices);

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

		binFile.read(reinterpret_cast<MemoryPtr>(listOfTypeTriangles),sizeof(TG_TypeTriangle) * numTypeTriangles);
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

		binFile.read(reinterpret_cast<MemoryPtr>(listOfTextures),sizeof(TG_TinyTexture) * numTextures);
	}
	else
	{
		listOfTextures = nullptr;
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

	binFile.read(reinterpret_cast<MemoryPtr>(nodeId),TG_NODE_ID);
	binFile.read(reinterpret_cast<MemoryPtr>(parentId),TG_NODE_ID);
	return true;
}

void TG_TypeShape::SaveBinaryCopy (File &binFile)
{
	binFile.writeInt(SHAPE_NODE);
	
	//listOfTypeVertices
	binFile.writeInt(numTypeVertices);
	if (numTypeVertices)
	{
		binFile.write(reinterpret_cast<MemoryPtr>(listOfTypeVertices),sizeof(TG_TypeVertex) * numTypeVertices);
	}

	//listOfTypeTriangles
	binFile.writeInt(numTypeTriangles);
	if (numTypeTriangles)
	{
		binFile.write(reinterpret_cast<MemoryPtr>(listOfTypeTriangles),sizeof(TG_TypeTriangle) * numTypeTriangles);
	}

	//listOfTextures
	binFile.writeInt(numTextures);
	if (numTextures)
	{
		binFile.write(reinterpret_cast<MemoryPtr>(listOfTextures),sizeof(TG_TinyTexture) * numTextures);
	}

	//Other Data
	binFile.writeFloat(nodeCenter.x);
	binFile.writeFloat(nodeCenter.y);
	binFile.writeFloat(nodeCenter.z);
	
	binFile.writeFloat(relativeNodeCenter.x);
	binFile.writeFloat(relativeNodeCenter.y);
	binFile.writeFloat(relativeNodeCenter.z);
	
	binFile.write(reinterpret_cast<MemoryPtr>(nodeId),TG_NODE_ID);
	binFile.write(reinterpret_cast<MemoryPtr>(parentId),TG_NODE_ID);
}

// TG_TypeShape::destroy is inline in tgl.h since B7o.6b (the unit tier tests the production body).

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
	numVertices = numTriangles = 0;
}	

//-------------------------------------------------------------------------------
//Function return 0 is OK.  -1 if file is not ASE Format or missing data.
//This function simply parses the ASE buffers handed to it.  This allows
//users to load the ase file themselves and manage their own memory for it.
//It allocates memory for internal Lists (vector stores owned by the shape).
//
// NOTE: Only takes the first HELPOBJECT from the ASE file.  Multi-object
// Files will require user intervention to parse!!
long TG_TypeShape::MakeFromHelper (BYTE *aseBuffer, mc2::czstring fileName)
{
	char *nodeName = strstr(reinterpret_cast<char *>(aseBuffer),ASE_NODE_NAME);
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

	nodeName = strstr(reinterpret_cast<char *>(aseBuffer),ASE_NODE_POS);
	gosASSERT(nodeName != nullptr);

	nodeName += strlen(ASE_NODE_POS)+1;

	char numData[512];

	GetNumberData(nodeName,numData);
	nodeCenter.x = -static_cast<float>(atof(numData));
	nodeName += strlen(numData)+1;

	GetNumberData(nodeName,numData);
	nodeCenter.z = static_cast<float>(atof(numData));
	nodeName += strlen(numData)+1;

	GetNumberData(nodeName,numData);
	nodeCenter.y = static_cast<float>(atof(numData));

	numTypeVertices = 0;

	numTypeTriangles = 0;

	return(0);
}	
