//-------------------------------------------------------------------------------
//
// Multiple TG Shape Layer
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
// Replace MLR for a myriad number of reasons.
//
// Started 4/16/99
//
// FFS
//
//-------------------------------------------------------------------------------

//-------------------------------------------------------------------------------
// Include Files
#ifndef MSL_H
#include"msl.h"
#endif

#include "gos_object_recon_tracy.h"  // [OBJECT_RECON v1] slice-2 recon-zero
#include "cpu_proj_cost_split.h"     // F3 CPU projection cost-baseline (RAII scope)
#include "../GameOS/gameos/gos_static_prop_batcher.h"    // Stage 2.D.2 CacheGpuLightData
#include "../GameOS/gameos/gos_static_prop_killswitch.h" // g_useGpuObjects extern
#include "../GameOS/gameos/gos_mech_killswitch.h"        // g_useGpuMechs extern (Slice B1)
#include "../GameOS/gameos/gos_profiler.h"  // PERF DIAGNOSTIC 2026-05-06: ZoneScopedN for CacheGpuLightData breakdown
#include "../code/frame_jobs.h"              // FRAME-JOBS-2D: isFrameJobsWorkerThread()
#include <atomic>

// 2026-05-05: frame-stamp the cache so registry::flush() can skip stale entries
// (actors whose update() was culled this frame; their cached UBO slot index now
// points to a slot whose content was filled by a different actor).
extern uint32_t g_mc2FrameCounter;

// FRAME-JOBS-2D: counts ResubmitCachedGpuLightData calls made from worker threads.
// Must remain 0 every frame — nonzero means the split boundary is broken.
std::atomic<int> g_workerResubmitCalls{0};

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef MATHFUNC_H
#include"mathfunc.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef CELINE_H
#include"celine.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#include<toolos.hpp>


#include "../ARM/Microsoft.Xna.Arm.h"
using namespace Microsoft::Xna::Arm;

//-------------------------------------------------------------------------------
extern void GetNumberData (char *rawData, char *result);
extern void GetNameData (char *rawData, char *result);
extern void GetWordData (char *rawData, char *result);

extern int ObjectTextureSize;

#define CURRENT_SHAPE_VERSION		0xBAFDECAF
#define CURRENT_ANIM_VERSION		0xBADDECAF

// sebi: defined in windows.h
#ifndef LINUX_BUILD
#define MAX_PATH					256
#endif

//-------------------------------------------------------------------------------
void GetNextLine (char *rawData, char *result)
{
	long startIndex = 0;
	long endIndex = 0;
	while (	(rawData[startIndex] != '\n') )
	{
		startIndex++;
	}

	startIndex++;
	endIndex = startIndex;
	while (	(rawData[endIndex] != '\n') )
	{
		endIndex++;
	}

	strncpy(result,&rawData[startIndex],endIndex - startIndex);
	result[endIndex-startIndex] = 0;
}
	
//-------------------------------------------------------------------------------
bool useSlerping = false;
extern bool useVertexLighting;
extern bool useFaceLighting;
bool useShadows = true;
extern bool silentMode;			//Used for automated builds to keep errors from popping up.

//-------------------------------------------------------------------------------
// TG_TypeMultiShape
void *TG_TypeMultiShape::operator new (size_t mySize)
{
	void *result = TG_Shape::tglHeap->Malloc(mySize);
	return result;
}
 
//-------------------------------------------------------------------------------
 void TG_TypeMultiShape::operator delete (void *us)
{
	TG_Shape::tglHeap->Free(us);
}

//-------------------------------------------------------------------------------
// TG_MultiShape & TG_TypeMultiShape
//This function copies the entire multi-shape to the new pointer.
//It does malloc and the newShape is a completely independant copy
//That must be destroyed or memory will leak!
TG_MultiShapePtr TG_TypeMultiShape::CreateFrom (void)
{
	TG_MultiShapePtr newShape = NULL;
	newShape = new TG_MultiShape;
	gosASSERT(newShape != NULL);
	
#ifdef _DEBUG
	if (numTG_TypeShapes == 0)
		STOP(("Tried to create a shape with no data Named %s",shapeName));
#endif

	//listOfShapes
	newShape->numTG_Shapes = numTG_TypeShapes;
	newShape->listOfShapes = (TG_ShapeRecPtr)TG_Shape::tglHeap->Malloc(sizeof(TG_ShapeRec) * numTG_TypeShapes);
	gosASSERT(newShape->listOfShapes != NULL);

	memset(newShape->listOfShapes,0,sizeof(TG_ShapeRec) * numTG_TypeShapes);

	for (long i=0;i<numTG_TypeShapes;i++)
	{
		newShape->listOfShapes[i].node = listOfTypeShapes[i]->CreateFrom();
	}

	//Setup Heirarchy again because pointers are not valid!
	for (int i=0;i<numTG_TypeShapes;i++)
	{
		newShape->listOfShapes[i].parentNode = NULL;
		newShape->listOfShapes[i].currentAnimation = NULL;
		newShape->listOfShapes[i].shapeToWorld = Stuff::LinearMatrix4D::Identity;
		newShape->listOfShapes[i].worldToShape = Stuff::LinearMatrix4D::Identity;
		newShape->listOfShapes[i].calcedThisFrame = -1;
		newShape->listOfShapes[i].processMe = true;
		newShape->listOfShapes[i].baseRotation.w = 1.0f;
		newShape->listOfShapes[i].baseRotation.x = 
		newShape->listOfShapes[i].baseRotation.y = 
		newShape->listOfShapes[i].baseRotation.z = 0.0f;

		//----------------------------------------------------------------------------------
		// For each shape, look for another node whose NodeId matches this shape's parentId
		for (long j=0;j<numTG_TypeShapes;j++)
		{
			if (strcmp(newShape->listOfShapes[i].node->myType->getParentId(),newShape->listOfShapes[j].node->myType->getNodeId()) == 0)
			{
				//-------------------------------
				// Found it!
				newShape->listOfShapes[i].parentNode = &(newShape->listOfShapes[j]);
				break;
			}
		}

		//----------------------------------------------------------------------------
		// Do NOT need to reset nodeCenters as they are correct for each node copied!
	}

	newShape->frameNum = 0.0f;

	newShape->myMultiType = this;
	
	newShape->isHudElement = false;
	
	return newShape;
}	

//-------------------------------------------------------------------------------
long TG_TypeMultiShape::LoadBinaryCopy (const char *fileName)
{
	File binFile;
	long result = binFile.open(fileName);
	if (result != NO_ERR)
		return -1;
	
	DWORD version = binFile.readInt();
	if (version == CURRENT_SHAPE_VERSION)
	{
		numTG_TypeShapes = binFile.readInt();
		numTextures = binFile.readInt();
	
		//ListOfTextures
		if (numTextures)
		{
			listOfTextures = (TG_TexturePtr)TG_Shape::tglHeap->Malloc(sizeof(TG_Texture) * numTextures);
			gosASSERT(listOfTextures != NULL);
		
			binFile.read((MemoryPtr)listOfTextures,sizeof(TG_Texture) * numTextures);
		}
		else
		{
			listOfTextures = NULL;
		}
	
		//listOfShapes
		listOfTypeShapes = (TG_TypeNodePtr *)TG_Shape::tglHeap->Malloc(sizeof(TG_TypeNodePtr) * numTG_TypeShapes);
		gosASSERT(listOfTypeShapes != NULL);
	
		memset(listOfTypeShapes,0,sizeof(TG_TypeNodePtr) * numTG_TypeShapes);
	
		bool aborted = false;
		long loadedShapes = 0;
		for (long i=0;i<numTG_TypeShapes;i++)
		{
			int nodeType = binFile.readInt();

			if (nodeType == SHAPE_NODE)
			{
				listOfTypeShapes[i] = new TG_TypeShape;
				listOfTypeShapes[i]->init();
				listOfTypeShapes[i]->LoadBinaryCopy(binFile);
				loadedShapes++;
			}
			else if (nodeType == TYPE_NODE)
			{
				listOfTypeShapes[i] = new TG_TypeNode;
				listOfTypeShapes[i]->init();
				listOfTypeShapes[i]->LoadBinaryCopy(binFile);
				loadedShapes++;
			}
			else
			{
				// sebi 2026-04-22: Wolfman MC2X mech .tgl binaries contain
				// node types beyond the stock SHAPE_NODE/TYPE_NODE enum
				// (we've seen e.g. 41, 50 show up). We don't know the
				// payload length for unknown types, so we can't skip past
				// them — continuing would read garbage for the next
				// nodeType (confirmed: subsequent reads produce random
				// 32-bit ints and wildly-out-of-range vertex indices).
				//
				// Abort the whole shape load cleanly: mark as aborted,
				// break the loop, and return -1 below. Caller
				// (Mech3DAppearanceType::init) will see failure and leave
				// the mech without an appearance rather than crashing the
				// mechbay.
				printf("[MSL] unknown nodeType=%d at index %ld/%ld — aborting load of this shape\n",
					nodeType, i, (long)numTG_TypeShapes); fflush(stdout);
				aborted = true;
				break;
			}
		}

		if (aborted)
		{
			// Clear any partially-constructed shapes so the MultiShape is
			// cleanly empty. Caller treats -1 as unsupported.
			for (long i = 0; i < loadedShapes; ++i) {
				if (listOfTypeShapes[i]) {
					delete listOfTypeShapes[i];
					listOfTypeShapes[i] = NULL;
				}
			}
			numTG_TypeShapes = 0;
			return -1;
		}

		//Setup Heirarchy again because pointers are not valid!
		for (int i=0;i<numTG_TypeShapes;i++)
		{
			// Guard against NULL slots (unknown nodeType above or allocation failure).
			if (!listOfTypeShapes[i])
				continue;

			//----------------------------------------------------------------------------
			// Do NOT need to reset nodeCenters as they are correct for each node copied!

			//--------------------------------------------------------
			//ONLY use nodes which are not spotlight or _PAB or LOS_
			if ((S_strnicmp(listOfTypeShapes[i]->getNodeId(),"_PAB",4) != 0) &&
				(S_strnicmp(listOfTypeShapes[i]->getNodeId(),"LOS_",4) != 0) &&
				(S_strnicmp(listOfTypeShapes[i]->getNodeId(),"SpotLight",9) != 0))
			{
				//------------------------------------------------------------------
				// Scan list of vertices and create minBox, maxBox and extentRadius
				for (long j=0;j<listOfTypeShapes[i]->GetNumTypeVertices();j++)
				{
					Stuff::Vector3D relNodeCenter;
					relNodeCenter = listOfTypeShapes[i]->GetRelativeNodeCenter();
					
					TG_TypeShapePtr typeShape = (TG_TypeShapePtr)listOfTypeShapes[i];
					
					if (minBox.x > typeShape->listOfTypeVertices[j].position.x + relNodeCenter.x)
						minBox.x = typeShape->listOfTypeVertices[j].position.x + relNodeCenter.x;
												 
					if (maxBox.x < typeShape->listOfTypeVertices[j].position.x + relNodeCenter.x)
						maxBox.x = typeShape->listOfTypeVertices[j].position.x + relNodeCenter.x;
												 
					if (minBox.y > typeShape->listOfTypeVertices[j].position.y + relNodeCenter.y)
						minBox.y = typeShape->listOfTypeVertices[j].position.y + relNodeCenter.y;
												 
					if (maxBox.y < typeShape->listOfTypeVertices[j].position.y + relNodeCenter.y)
						maxBox.y = typeShape->listOfTypeVertices[j].position.y + relNodeCenter.y;
												 
					if (minBox.z > typeShape->listOfTypeVertices[j].position.z + relNodeCenter.z)
						minBox.z = typeShape->listOfTypeVertices[j].position.z + relNodeCenter.z;
												 
					if (maxBox.z < typeShape->listOfTypeVertices[j].position.z + relNodeCenter.z)
						maxBox.z = typeShape->listOfTypeVertices[j].position.z + relNodeCenter.z;
				}
			}
		}
	
		float minBoxLength = minBox.GetLength();
		float maxBoxLength = maxBox.GetLength();
		if (minBoxLength > extentRadius)
			extentRadius = minBoxLength;

		if (maxBoxLength > extentRadius)
			extentRadius = maxBoxLength;
	}
	else
	{
		return -1;
	}

	return 0;
}

//-------------------------------------------------------------------------------
// Track D — narrow construction API (see msl.h for rationale).

void TG_TypeMultiShape::AllocateImportedShapes(int numShapes)
{
	gosASSERT(numShapes > 0);
	gosASSERT(listOfTypeShapes == NULL);  // freshly init()'d multi-shape

	numTG_TypeShapes = numShapes;
	listOfTypeShapes = (TG_TypeNodePtr *)TG_Shape::tglHeap->Malloc(
		sizeof(TG_TypeNodePtr) * numTG_TypeShapes);
	gosASSERT(listOfTypeShapes != NULL);
	memset(listOfTypeShapes, 0, sizeof(TG_TypeNodePtr) * numTG_TypeShapes);

	// Construct empty TG_TypeShape per slot — caller fills via
	// listOfTypeShapes[i]->InitFromImportedMesh(...). This mirrors the
	// LoadBinaryCopy SHAPE_NODE branch (msl.cpp:222-228).
	for (long i = 0; i < numTG_TypeShapes; i++)
	{
		listOfTypeShapes[i] = new TG_TypeShape;
		listOfTypeShapes[i]->init();
	}
}

void TG_TypeMultiShape::SetImportedTextures(DWORD count,
                                            const char* const* names,
                                            const bool* alphas)
{
	gosASSERT(listOfTextures == NULL);  // freshly init()'d multi-shape

	numTextures = count;
	if (numTextures == 0)
	{
		listOfTextures = NULL;
		return;
	}

	listOfTextures = (TG_TexturePtr)TG_Shape::tglHeap->Malloc(
		sizeof(TG_Texture) * numTextures);
	gosASSERT(listOfTextures != NULL);
	memset(listOfTextures, 0, sizeof(TG_Texture) * numTextures);

	for (DWORD i = 0; i < numTextures; i++)
	{
		// Texture name: importer hands us a stripped basename without ext;
		// store as-is. NULLTXM matches ParseASEFile's empty-name fallback.
		const char* n = (names && names[i]) ? names[i] : "NULLTXM";
		strncpy(listOfTextures[i].textureName, n,
		        sizeof(listOfTextures[i].textureName) - 1);
		listOfTextures[i].textureName[sizeof(listOfTextures[i].textureName) - 1] = '\0';
		// Sentinel handles per mc2_texture_handle_is_live.md — never cache the
		// live gosTextureHandle at import time. The engine's texture-resolve
		// pass (e.g. Mech3DAppearanceType::init via SetTextureHandle) wires
		// real values once MC_TextureManager has loaded the bitmap.
		listOfTextures[i].mcTextureNodeIndex = 0xffffffff;
		listOfTextures[i].gosTextureHandle   = 0xffffffff;
		listOfTextures[i].textureAlpha       = alphas ? alphas[i] : false;
	}

	// Wire per-shape TG_TinyTexture linkage by delegating to the existing
	// virtual method on each TG_TypeShape (mirrors the ASE-path call at
	// msl.cpp:826). Helper TG_TypeNode entries (TYPE_NODE) skip this — they
	// have no per-shape texture state.
	for (long i = 0; i < numTG_TypeShapes; i++)
	{
		if (listOfTypeShapes[i] && listOfTypeShapes[i]->GetNodeType() == SHAPE_NODE)
		{
			static_cast<TG_TypeShape*>(listOfTypeShapes[i])
				->CreateListOfTextures(listOfTextures, numTextures);
		}
	}
}

//-------------------------------------------------------------------------------
// Track D — format-probe entry point. See msl.h for full contract.
//
// Probes for .glb (preferred) at {tglPath}{baseName}.glb. On hit, runs the
// Assimp importer; on miss or failure, falls through to LoadTGMultiShapeFromASE
// at {tglPath}{baseName}.ase. The ASE fallback is the load-bearing guarantee
// for stock-install playability — without a .glb sidecar we MUST behave
// identically to a build that never had the importer compiled in.
#ifdef ENABLE_ASSIMP_IMPORTER
#include "assimp_importer.h"
#endif

// Track D — env-gated diagnostic trace. Set MC2_ASSIMP_TRACE=1 in the env to
// emit per-load probe + import-result lines on stderr. Default off; matches
// the [TGL_POOL v1] / [DESTROY v1] convention (memory:debug_instrumentation_rule).
static const bool s_assimpTrace_msl = (getenv("MC2_ASSIMP_TRACE") != NULL);
#define ASSIMP_TRACE_MSL(fmt, ...) \
    do { if (s_assimpTrace_msl) { \
        fprintf(stderr, "[ASSIMP_TRACE] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); } } while (0)

long TG_TypeMultiShape::LoadFromFile(const char* baseName)
{
	if (!baseName || !*baseName)
		return -1;

	ASSIMP_TRACE_MSL("LoadFromFile baseName='%s'", baseName);

#ifdef ENABLE_ASSIMP_IMPORTER
	// Runtime killswitch: MC2_ASSIMP_IMPORT=0 forces legacy ASE for all asset
	// classes without requiring a rebuild. Default is enabled (any value other
	// than "0", or env var absent). Compile-time ENABLE_ASSIMP_IMPORTER is the
	// master gate; this is the per-launch override.
	{
		const char* kEnv = getenv("MC2_ASSIMP_IMPORT");
		if (!kEnv || strcmp(kEnv, "0") != 0) {
			// Probe order: .glb then .fbx. On any importer failure, fall
			// through to ASE so a broken modder asset can't render the mech
			// un-loadable. Stock-install playability is paramount.
			static const char* const kImportExts[] = { ".glb", ".fbx" };
			for (size_t e = 0; e < sizeof(kImportExts) / sizeof(kImportExts[0]); ++e)
			{
				FullPathFileName probePath;
				probePath.init(tglPath, baseName, kImportExts[e]);
				const bool exists = fileExists(probePath, FILE_ON_DISK);
				ASSIMP_TRACE_MSL("  probe '%s' exists=%d",
				                  (const char*)probePath, (int)exists);
				if (exists)
				{
					ASSIMP_TRACE_MSL("  calling ImportGeometryFromFile...");
					long r = ImportGeometryFromFile(probePath, this);
					ASSIMP_TRACE_MSL("  ImportGeometryFromFile returned %ld", r);
					if (r == 0)
						return NO_ERR;
					// Importer logged the reason via STOP/PAUSE; fall through.
					break;
				}
			}
			ASSIMP_TRACE_MSL("  no modern source found, falling through to ASE");
		} else {
			ASSIMP_TRACE_MSL("  MC2_ASSIMP_IMPORT=0, skipping modern probe");
		}
	}
#endif // ENABLE_ASSIMP_IMPORTER

	// Default fallback: legacy ASE path. Behavior is byte-equivalent to the
	// pre-Track-D build for any baseName whose .glb is absent.
	FullPathFileName asePath;
	asePath.init(tglPath, baseName, ".ase");
	return LoadTGMultiShapeFromASE(asePath);
}

//-------------------------------------------------------------------------------
void TG_TypeMultiShape::SaveBinaryCopy (const char *fileName)
{
	File binFile;
	binFile.create(fileName);

    //sebi    
	binFile.writeInt(CURRENT_SHAPE_VERSION);
	binFile.writeInt(numTG_TypeShapes);
	binFile.writeInt(numTextures);
	
	//ListOfTextures
	if (numTextures)
	{
		binFile.write((MemoryPtr)listOfTextures,sizeof(TG_Texture) * numTextures);
	}
	
	//listOfShapes
	for (long i=0;i<numTG_TypeShapes;i++)
	{
		listOfTypeShapes[i]->SaveBinaryCopy(binFile);
	}
}	

//-------------------------------------------------------------------------------
void TG_TypeMultiShape::destroy (void)
{
	for (long i=0;i<numTG_TypeShapes;i++)
	{
		if (listOfTypeShapes[i])
		{
			listOfTypeShapes[i]->destroy();
			delete listOfTypeShapes[i];
			listOfTypeShapes[i] = NULL;
		}
	}

	if (listOfTypeShapes)
		TG_Shape::tglHeap->Free(listOfTypeShapes);
		
	listOfTypeShapes = NULL;

	if (listOfTextures)
		TG_Shape::tglHeap->Free(listOfTextures);
	listOfTextures= NULL;

	numTextures = 0;
	numTG_TypeShapes = 0;
	
#ifdef _DEBUG
	if (shapeName)
	{
		free(shapeName);
		shapeName = NULL;
	}
#endif
}	

//-------------------------------------------------------------------------------
void TG_MultiShape::destroy (void)
{
	for (long i=0;i<numTG_Shapes;i++)
	{
		if (listOfShapes[i].node)
		{
			listOfShapes[i].node->destroy();
			TG_Shape::tglHeap->Free(listOfShapes[i].node);
		}
	}

	if (listOfShapes)
		TG_Shape::tglHeap->Free(listOfShapes);
	listOfShapes = NULL;	

	numTG_Shapes = 0;
}	

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  -1 if file not found or file not ASE Format.
//Function runs through each piece of ASE file and creates a separate
//TG_Shape for each one.  First pass is count number of GEOMOBJECTS.
//Second pass is load each one.
long TG_TypeMultiShape::LoadTGMultiShapeFromASE (const char *fileName, bool forceMakeBinary, IProviderEngine* armProvider)
{
	//-----------------------------------------------------
	// Fully loads and parses an ASE File.  These files are
	// output by 3D Studio MAX's ASCII Scene Exporter.

	//------------------------------------------------------
	// New!  Checks for Binary file to load instead.
	//
	// If Binary is older or equal to ASE, re-parses ASE and
	// saves binary.
	//
	// If Binary does not exist, does the above, too!
	// 
	// If Binary exists and is newer, just loads that.
	// MUCH FASTER!
	//
	bool makeBinary = false;

	char drive[MAX_PATH];
	char dir[MAX_PATH];
	char name[MAX_PATH];
	char ext[MAX_PATH];
	_splitpath(fileName,drive,dir,name,ext);

#ifdef _DEBUG
	shapeName = (char *)malloc(strlen(name) + 1);
	strcpy(shapeName,name);
#endif
	
	FullPathFileName binaryFileName;
	binaryFileName.init(tglPath,name,".tgl");
	
	//Load Table is as follows:
	//	ASE		TGL		FST			Result:
	//	0		0		0			STOP
	//	0		0		1			OK to load if Shape version good, STOp Otherwise
	//	0		1		0			Same as above
	//	0		1		1			Same as above
	//	1		0		0			Set MakeBinary true.
	//	1		0		1			Set MakeBinary true.
	//	1		1		0			If TGL older then ase, set Make Bindary true.
	//	1		1		1			Same as above.

	if( !forceMakeBinary)
	{
		if (!fileExists(fileName, FILE_ON_DISK) && !fileExists(binaryFileName))		//Line 0
		{
	#ifdef _DEBUG
	//		if (!silentMode)
	//			PAUSE(("Unable to locate shape data for %s.  No ASE or TGL File",fileName));
	#endif
			makeBinary = true;
		}
		else if (!fileExists(fileName, FILE_ON_DISK) && fileExists(binaryFileName))	//Line 1-3
		{
			File binFile;
			
			long result = binFile.open(binaryFileName);
			if (result != NO_ERR)
			{
				if (!silentMode)
					STOP(("Unable to locate shape data for %s.	TGL File BAD Format",fileName));
			}
			else
			{
				int versionId = binFile.readInt();
				if (versionId != CURRENT_SHAPE_VERSION)
				{
					if (!silentMode)
						STOP(("Unable to locate shape data for %s.	TGL File BAD Format",fileName));
				}
			}
		}
		else if (fileExists(fileName, FILE_ON_DISK) && (!fileExists(binaryFileName) || (fileExists(binaryFileName, FILE_ON_FST))))	//Line 4-5
		{
			makeBinary = true;
		}
		else if (fileExists(fileName, FILE_ON_DISK) && ((fileExists(binaryFileName) == 1) && file1OlderThan2(binaryFileName,fileName)))	//Line 6-7
		{
			makeBinary = true;
		}
	}
	else
	{
		makeBinary = true;
	}

	if (!makeBinary)
	{
		return (LoadBinaryCopy(binaryFileName));
	}
	else
	{
		//------------------------------------------
		// Check if exists 
		if (!gos_FileExists(fileName))
			return(-1);

		long aseFileSize = gos_FileSize(fileName);
	
		//---------------------------------------
		// Create Buffer to read entire file into
		BYTE *aseContents = (BYTE *)malloc(aseFileSize+1);
		gosASSERT(aseContents != NULL);
	
		//---------------------------------------
		// Open File and read it and close it
		HGOSFILE aseFile;
		gos_OpenFile(&aseFile,fileName,READONLY);
	
#ifdef _DEBUG
		long dataRead = 
#endif
		gos_ReadFile(aseFile,aseContents,aseFileSize);
		gosASSERT(dataRead == aseFileSize);
	
		gos_CloseFile(aseFile);
	
		aseContents[aseFileSize] = 0;
	
		//----------------------------------------
		// Check for valid ASE Header data.
		if (S_strnicmp(ASE_HEADER,(char *)aseContents,strlen(ASE_HEADER)) != 0)
			return(-1);
		
		//---------------------------------------
		// Texture Data Next.
		//If the Material Class is Standard, ONLY one texture possible.
		//If the Material Class is Sub-Object, Multiple textures possible.
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
				gosASSERT(textureData != NULL);
	
				textureData += strlen(ASE_SUBMATERIAL_COUNT);
				GetNumberData(textureData,numberData);
	
				numTextures += atol(numberData);
			}
	
			aseBuffer = (unsigned char *)textureData;
		}

		IProviderAssetPtr armAssetPtr = NULL;
		if (armProvider)
		{
			armAssetPtr = armProvider->OpenAsset(fileName, 
				AssetType_Physical, ProviderType_Primary);
			armAssetPtr->AddProperty("Type", "Shape");
		}
	
		if (numTextures)
		{
			numTextures *= 2;
			listOfTextures = (TG_TexturePtr)TG_Shape::tglHeap->Malloc(sizeof(TG_Texture) * numTextures);
			gosASSERT(listOfTextures != NULL);
			memset(listOfTextures,0x0,sizeof(TG_Texture) * numTextures);

			char *txmData = (char *)aseContents;
			for (int i=0;i<(numTextures/2);i++)
			{
				//-------------------------------------------------------------------------------
				// Get and store texture Name.  Will need multiple for Multi-Sub if implemented
				char textId[256];
                // sebi: looks like if there is only one texture then ASE_MATERIAL_BITMAP_ID has no index
				// anyway original code is incorrect, because it passes i but has no format specifier for it
				//sprintf(textId,"%s",ASE_MATERIAL_BITMAP_ID,i);
				sprintf(textId, "%s", ASE_MATERIAL_BITMAP_ID);
		
				char *txmTemp = txmData;
				txmData = strstr(txmData,textId);
				
				if (txmData == NULL)
				{
#ifdef _DEBUG
					if (!silentMode)
						PAUSE(("WARNING: Material ID %d in Shape %s has no Texture Bitmap!!",(i+1),fileName));
#endif						
					txmData = txmTemp;
					strcpy(listOfTextures[i].textureName,"NULLTXM");
				}
				else
				{
					txmData += strlen(textId);
					GetNameData(txmData,listOfTextures[i].textureName);
				}
				
				if (listOfTextures[i].textureName[0] == 0)
				{
					strcpy(listOfTextures[i].textureName,"NULLTXM");
				}
				
				// ARM
				if (armAssetPtr && strcmp(listOfTextures[i].textureName, "NULLTXM") != 0) 
				{				
					// The filenames refer to art files on some share (\\aas1), just get the base filename
					// and assume it's in the "Data\TGL\128\" directory
					char * baseFilename = strrchr(listOfTextures[i].textureName, PATH_SEPARATOR_AS_CHAR);
					
					if (baseFilename)
					{
						char localFilename[1024] = "Data" PATH_SEPARATOR "TGL" PATH_SEPARATOR "128" PATH_SEPARATOR;
						strcat(localFilename, baseFilename+1);
						armAssetPtr->AddRelationship("Texture", localFilename);
					}
					else
					{
						armAssetPtr->AddRelationship("Texture", listOfTextures[i].textureName);
					}
				}

				listOfTextures[i].mcTextureNodeIndex = 0xffffffff;
				listOfTextures[i].gosTextureHandle = 0xffffffff;
				listOfTextures[i].textureAlpha = false;
			}
			
   			for (int i=(numTextures/2);i<numTextures;i++)
   			{
   				//-------------------------------------------------------------------------------
   				// add the word Shadow the texture name so we can load shadows if we need to!
   				strncpy(listOfTextures[i].textureName,listOfTextures[i-(numTextures/2)].textureName,strlen(listOfTextures[i-(numTextures/2)].textureName)-4);
   				strcat(listOfTextures[i].textureName,"X.tga");
   				listOfTextures[i].mcTextureNodeIndex = 0xffffffff;
				listOfTextures[i].gosTextureHandle = 0xffffffff;
   				listOfTextures[i].textureAlpha = true;
   			}

			for (int i=0;i<numTextures;i++)
			{
				// Try to touch all of the textures we need.
				char txmName[1024];
				GetTextureName(i,txmName,256);
		
				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR ,tglPath,ObjectTextureSize);
			
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
			
				if( fileExists(textureName) == 1 )
				{
					File *touchTexture = new File();
					touchTexture->open(textureName);
					touchTexture->close();
					delete touchTexture;
				}
			}
		}
		else
			listOfTextures = NULL;
	
		//------------------------------------------------------------
		// FIRST PASS.  Count number of GEOMObjects & HelperObjects
		numTG_TypeShapes = 0;
		char *aseScan = (char *)aseContents;
		aseScan = strstr(aseScan,ASE_OBJECT);
		while (aseScan != NULL)
		{
			numTG_TypeShapes++;
			aseScan += strlen(ASE_OBJECT)+1;
			aseScan = strstr(aseScan,ASE_OBJECT);
		}
	
		aseScan = (char *)aseContents;
		aseScan = strstr(aseScan,ASE_HELP_OBJECT);
		while (aseScan != NULL)
		{
			//--------------------------------------------------------
			// Check if NodeName is handle_ or WORLD.  These are bad!
			// Do not count them!
			aseScan = strstr(aseScan,ASE_NODE_NAME);
			gosASSERT(aseScan != NULL);								//Node must have a NAME, right?
			aseScan += strlen(ASE_NODE_NAME)+1;
	
			char chkWord[512];
			GetWordData(aseScan,chkWord);
	
			if (strstr(chkWord,"handle_") == NULL && strstr(chkWord,"World") == NULL)
			{
				numTG_TypeShapes++;
			}
	
			aseScan += strlen(ASE_HELP_OBJECT)+1;
			aseScan = strstr(aseScan,ASE_HELP_OBJECT);
		}
	
		listOfTypeShapes = (TG_TypeNodePtr *)TG_Shape::tglHeap->Malloc(sizeof(TG_TypeNodePtr) * (numTG_TypeShapes+1));
		gosASSERT(listOfTypeShapes != NULL);
	
		memset(listOfTypeShapes,0,sizeof(TG_TypeNodePtr) * (numTG_TypeShapes+1));
	
		//------------------------------------------------------------
		// SECOND PASS.  Load HelperObjects.
		long startIndex = 0;
	
		aseScan = (char *)aseContents;
		aseScan = strstr(aseScan,ASE_HELP_OBJECT);
		while (aseScan != NULL)
		{
			listOfTypeShapes[startIndex] = new TG_TypeNode;
			listOfTypeShapes[startIndex]->init();
	
			//---------------------------------------------------
			// Calling from top will load just first HELPEROBJECT!
			// We now make the "top" the start of each HELPEROBJECT!
			// DO NOT LOAD handle_ helpers and WORLD halpers.
			// Decrement number of shapes accordingly.
			char *aseEnd = aseScan + strlen(ASE_HELP_OBJECT)+1;
			aseEnd = strstr(aseEnd,ASE_HELP_OBJECT);
	
			if (aseEnd)
			{
				char *aseData = (char *)malloc(aseFileSize+1);
				ZeroMemory(aseData, aseFileSize+1);
				memcpy(aseData,aseScan,aseEnd - aseScan);
	
				//--------------------------------------------------------
				// Check if NodeName is handle_ or WORLD.  These are bad!
				// Do not count them!
				aseScan = strstr(aseScan,ASE_NODE_NAME);
				gosASSERT(aseScan != NULL);								//Node must have a NAME, right?
				aseScan += strlen(ASE_NODE_NAME)+1;
	
				char chkWord[512];
				GetWordData(aseScan,chkWord);
	
				//-------------------------------------------------------------------
				// Is this a forbidden helper object?
				if ((strstr(chkWord,"handle") == NULL) && (strstr(chkWord,"World") == NULL))
				{
					long parseResult = listOfTypeShapes[startIndex]->MakeFromHelper((unsigned char *)aseData,fileName);
					if (parseResult != 0)
						return(parseResult);
	
					startIndex++;
				}
	
				free(aseData);
			}
			else
			{
				//--------------------------------------------------------
				// Check if NodeName is handle_ or WORLD.  These are bad!
				// Do not count them!
				char *scanStart = aseScan;
				scanStart = strstr(scanStart,ASE_NODE_NAME);
				gosASSERT(scanStart != NULL);								//Node must have a NAME, right?
				scanStart += strlen(ASE_NODE_NAME)+1;
	
				char chkWord[512];
				GetWordData(scanStart,chkWord);
	
				//-------------------------------------------------------------------
				// Is this a forbidden helper object?
				if ((strstr(chkWord,"handle") == NULL) && (strstr(chkWord,"World") == NULL))
				{
					//-------------------------------------------------------------------
					long parseResult = listOfTypeShapes[startIndex]->MakeFromHelper((unsigned char *)aseScan,fileName);
					if (parseResult != 0)
						return(parseResult);
	
					startIndex++;
				}
			}
	
			aseScan += strlen(ASE_HELP_OBJECT);
			aseScan = strstr(aseScan,ASE_HELP_OBJECT);
		}
	
		//------------------------------------------------------------
		// THIRD PASS.  Load GeomObjects.
		aseScan = (char *)aseContents;
		aseScan = strstr(aseScan,ASE_OBJECT);
		while (aseScan != NULL)
		{
			listOfTypeShapes[startIndex] = new TG_TypeShape;
			listOfTypeShapes[startIndex]->init();
			listOfTypeShapes[startIndex]->CreateListOfTextures(listOfTextures,numTextures);
	
			//---------------------------------------------------
			// Calling from top will load just first GEOMOBJECT!
			// We now make the "top" the start of each GEOMOBJECT!
			char *aseEnd = aseScan + strlen(ASE_OBJECT)+1;
			aseEnd = strstr(aseEnd,ASE_OBJECT);
	
			if (aseEnd)
			{
				char *aseData = (char *)malloc(aseFileSize+1);
				ZeroMemory(aseData, aseFileSize+1);
				memcpy(aseData,aseScan,aseEnd - aseScan);
				
				long parseResult = listOfTypeShapes[startIndex]->ParseASEFile((unsigned char *)aseData,fileName);
				if (parseResult != 0)
					return(parseResult);
	
				free(aseData);
			}
			else
			{
				long parseResult = listOfTypeShapes[startIndex]->ParseASEFile((unsigned char *)aseScan,fileName);
				if (parseResult != 0)
					return(parseResult);
			}
	
			startIndex++;
			aseScan += strlen(ASE_OBJECT)+1;
			aseScan = strstr(aseScan,ASE_OBJECT);
		}
	
 		//------------------------------------------------------------------
		// FOURTH PASS.  Setup Heirarchy ALWAYS or NO Shape BOUNDS!!!
		{
			aseScan = (char *)aseContents;
			for (long i=0;i<numTG_TypeShapes;i++)
			{
				//----------------------------------------------------------------------------------
				// For each shape, look for another node whose NodeId matches this shape's parentId
				TG_TypeNodePtr parentNode = NULL;
				for (long j=0;j<numTG_TypeShapes;j++)
				{
					if (strcmp(listOfTypeShapes[i]->getParentId(),listOfTypeShapes[j]->getNodeId()) == 0)
					{
						parentNode = listOfTypeShapes[j];
						break;
					}
				}
	
				listOfTypeShapes[i]->movePosRelativeCenterNode();
	
				if (parentNode)
					listOfTypeShapes[i]->MoveNodeCenterRelative(parentNode->GetNodeCenter());
					
				//--------------------------------------------------------
				//ONLY use nodes which are not spotlight or _PAB or LOS_
				if ((S_strnicmp(listOfTypeShapes[i]->getNodeId(),"_PAB",4) != 0) &&
					(S_strnicmp(listOfTypeShapes[i]->getNodeId(),"LOS_",4) != 0) && 
					(S_strnicmp(listOfTypeShapes[i]->getNodeId(),"SpotLight",9) != 0))
				{
				
					//------------------------------------------------------------------
					// Scan list of vertices and create minBox, maxBox and extentRadius
					for (int j=0;j<listOfTypeShapes[i]->GetNumTypeVertices();j++)
					{
						Stuff::Vector3D relNodeCenter;
						relNodeCenter = listOfTypeShapes[i]->GetRelativeNodeCenter();
						
						TG_TypeShapePtr typeShape = (TG_TypeShapePtr)listOfTypeShapes[i];
						if (minBox.x > typeShape->listOfTypeVertices[j].position.x + relNodeCenter.x)
							minBox.x = typeShape->listOfTypeVertices[j].position.x + relNodeCenter.x;
													 
						if (maxBox.x < typeShape->listOfTypeVertices[j].position.x + relNodeCenter.x)
							maxBox.x = typeShape->listOfTypeVertices[j].position.x + relNodeCenter.x;
													 
						if (minBox.y > typeShape->listOfTypeVertices[j].position.y + relNodeCenter.y)
							minBox.y = typeShape->listOfTypeVertices[j].position.y + relNodeCenter.y;
													 
						if (maxBox.y < typeShape->listOfTypeVertices[j].position.y + relNodeCenter.y)
							maxBox.y = typeShape->listOfTypeVertices[j].position.y + relNodeCenter.y;
													 
						if (minBox.z > typeShape->listOfTypeVertices[j].position.z + relNodeCenter.z)
							minBox.z = typeShape->listOfTypeVertices[j].position.z + relNodeCenter.z;
													 
						if (maxBox.z < typeShape->listOfTypeVertices[j].position.z + relNodeCenter.z)
							maxBox.z = typeShape->listOfTypeVertices[j].position.z + relNodeCenter.z;
					}
				}
			}
		
			float minBoxLength = minBox.GetLength();
			float maxBoxLength = maxBox.GetLength();
			if (minBoxLength > extentRadius)
				extentRadius = minBoxLength;
				
			if (maxBoxLength > extentRadius)
				extentRadius = maxBoxLength;
		}

		if (armAssetPtr != NULL)
		{
			armAssetPtr->Close();
		}
	
		free(aseContents);
		aseContents = NULL;
	
		SaveBinaryCopy(binaryFileName);
	}

	return(0);
}	

//-------------------------------------------------------------------------------
// TG_TypeMultiShape
void *TG_MultiShape::operator new (size_t mySize)
{
	void *result = TG_Shape::tglHeap->Malloc(mySize);
	return result;
}
 
//-------------------------------------------------------------------------------
 void TG_MultiShape::operator delete (void *us)
{
	TG_Shape::tglHeap->Free(us);
}

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  realLength if textureName is longer then nameLength-1.
//Returns -1 if texture requested is out of range.
//This function digs the texture name(s) out of the ASE file so that the
//User can load and manage them anyway they want to.
long TG_TypeMultiShape::GetTextureName (DWORD textureNum, char *tName, long nameLength)
{
	if (textureNum >= numTextures)
		return(-1);
		
	char baseName[512];
	char extension[256];
	_splitpath(listOfTextures[textureNum].textureName,NULL,NULL,baseName,extension);

	char basePath[1024];
	sprintf(basePath,"%s%s",baseName,extension);

	if (nameLength < (strlen(basePath)+1))
		return(strlen(basePath)+1);
		
	strcpy(tName,basePath);
	return(0);
}	

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
// Assigns a MCTextureNodeIndex to this NOT a GOS handle anymore.  Must go through
// cache in case of too many handles.
// MC2_TEX_LIFECYCLE_TRACE=1 — see mclib/txmmgr.cpp for full event taxonomy.
// Logs every TG_TypeMultiShape::SetTextureHandle call (the bdactor.cpp/genactor.cpp
// "actor.update() → re-cache" pathway). Absence of these logs for a given multiType
// during gameplay = appearance->touch() ran instead of update() (UPDATE_SKIP=1 path).
static const bool s_msl_texLifecycleTrace =
    (getenv("MC2_TEX_LIFECYCLE_TRACE") != nullptr);

long TG_TypeMultiShape::SetTextureHandle (DWORD textureNum, DWORD gosTextureHandle)
{
	if (textureNum >= numTextures)
		return(-1);

	listOfTextures[textureNum].mcTextureNodeIndex = gosTextureHandle;
	listOfTextures[textureNum].gosTextureHandle = 0xffffffff;

	// 2026-05-11 leaf propagation. Mirror the multi-level write into every
	// leaf TG_TypeShape's TG_TinyTexture so consumers reading at leaf level
	// (GpuStaticPropBatcher::finalizeGeometry's texture-array build reads
	// `src->listOfTextures[slot].mcTextureNodeIndex`, where src is a leaf
	// TG_TypeShape) see the live nodeIdx. Without this, damage-shape leaves
	// permanently report 0xFFFFFFFF (the CreateListOfTextures default) and
	// the per-packet texture-array build skips them with layer=-1.
	for (long i = 0; i < numTG_TypeShapes; ++i) {
		if (listOfTypeShapes[i]) {
			listOfTypeShapes[i]->SetTextureHandle(textureNum, gosTextureHandle);
		}
	}

	// PERF DIAGNOSTIC 2026-05-07: per-frame call rate makes the unfiltered
	// log unusable for diagnosis (see capture 3 — console flooding tanks
	// frame rate). Rate-limit to: (a) first 32 events per session, then
	// (b) 1-in-256 sampling, plus (c) periodic summary every 1024 events.
	// Set MC2_TEX_LIFECYCLE_TRACE_VERBOSE=1 to restore unfiltered output
	// for narrow-window debugging where flood is acceptable.
	if (s_msl_texLifecycleTrace) {
		static uint32_t s_eventCount = 0;
		static const bool s_verbose =
		    (getenv("MC2_TEX_LIFECYCLE_TRACE_VERBOSE") != nullptr);
		const bool firstWindow = (s_eventCount < 32);
		const bool sample = ((s_eventCount & 0xFF) == 0);  // 1/256
		if (s_verbose || firstWindow || sample) {
			printf("[TEX_LIFECYCLE v1] event=recache_multi multiType=%p texNum=%lu nodeIdx=0x%08lx numTex=%ld total=%u\n",
			       (void*)this, (unsigned long)textureNum,
			       (unsigned long)gosTextureHandle, (long)numTextures,
			       s_eventCount);
			fflush(stdout);
		}
		++s_eventCount;
	}

	return(0);
}

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
//This function takes the gosTextureHandle passed in and assigns it to the
//textureNum entry of the listOfTextures;
long TG_TypeMultiShape::SetTextureAlpha (DWORD textureNum, bool alphaFlag)
{
	if (textureNum >= numTextures)
		return(-1);

	// SP-BATCHER-ALPHASCAN-GATE-1: bump the global alpha generation ONLY when the
	// bit actually flips, so the batcher's per-frame alpha re-resolve scan can
	// early-out on the (overwhelmingly common) no-event case. Cheap relaxed atomic.
	if (listOfTextures[textureNum].textureAlpha != alphaFlag)
		g_staticPropAlphaGeneration.fetch_add(1, std::memory_order_relaxed);

	listOfTextures[textureNum].textureAlpha = alphaFlag;

	// 2026-05-11 leaf propagation: TG_TypeShape leaves carry their own
	// listOfTextures (TG_TinyTexture array) which is COPIED from this
	// multi-type at ASE-load time via CreateListOfTextures (msl.cpp:834)
	// and NEVER refreshed by this setter. Consumers reading at leaf level
	// (GpuStaticPropBatcher reads `type.source->listOfTextures[slot].textureAlpha`,
	// where type.source is a TG_TypeShape leaf) saw stale `false` values
	// for damage-shape textures even after bdactor's setObjStatus loop
	// called SetTextureAlpha(true) on the multi-shape. That produced the
	// destroyed-prop opaque-rectangle alpha bug. Mirror the multi-level
	// write down to every leaf so leaf-reads pick up the live value.
	for (long i = 0; i < numTG_TypeShapes; ++i) {
		if (listOfTypeShapes[i]) {
			listOfTypeShapes[i]->SetTextureAlpha(textureNum, alphaFlag);
		}
	}

	return(0);
}

//-------------------------------------------------------------------------------
//This function rotates the heirarchy from this node down.  Used for torso twists, arms, etc.
long TG_MultiShape::SetNodeRotation (const char *nodeName, Stuff::UnitQuaternion *rot)
{
	for (long i=0;i<numTG_Shapes;i++)
	{
		if (S_stricmp(listOfShapes[i].node->myType->getNodeId(),nodeName) == 0)
		{
			//-------------------------------
			// Found it!
			listOfShapes[i].baseRotation = *rot;
			return i;
		}
	}

	return -1;
}	

//----------------------------------------------------------------------------------------------------
//This function returns the world translated position of the vertex asked for with rotation applied.
Stuff::Vector3D TG_MultiShape::GetShapeVertexInEditor(long shapeNum, long vertexNum, float rotation)
{
	Stuff::Vector3D result;
	result.x = result.y = result.z = 0.0f;
	
	if ((shapeNum >= 0) && (shapeNum < numTG_Shapes) && 
		(vertexNum >= 0) && (vertexNum < myMultiType->listOfTypeShapes[shapeNum]->GetNumTypeVertices()))
	{
		//-------------------------------
		// Get the vertex in question.
		Stuff::Vector3D vertexPos;
		TG_TypeShapePtr typeShape = (TG_TypeShapePtr)myMultiType->listOfTypeShapes[shapeNum];
		vertexPos.x = -(typeShape->listOfTypeVertices[vertexNum].position.x + typeShape->GetRelativeNodeCenter().x + GetRootNodeCenter().x);
		vertexPos.z = typeShape->listOfTypeVertices[vertexNum].position.y + typeShape->GetRelativeNodeCenter().y + GetRootNodeCenter().y; 
		vertexPos.y = typeShape->listOfTypeVertices[vertexNum].position.z + typeShape->GetRelativeNodeCenter().z + GetRootNodeCenter().z;
		
		//---------------------------------------
		// Rotate vertex if rotation is NON-zero
		if (rotation != 0.0f)
		{
			Rotate(vertexPos,rotation);
		}
		
		result = vertexPos;


	}
	
	return result;
}

//----------------------------------------------------------------------------------------------------
//This function returns the world translated position of the vertex asked for with rotation applied.
Stuff::Vector3D TG_MultiShape::GetShapeVertexInWorld(long shapeNum, long vertexNum, float rotation)
{
	Stuff::Vector3D result;
	result.x = result.y = result.z = 0.0f;

	if ((shapeNum >= 0) && (shapeNum < numTG_Shapes) && 
		(vertexNum >= 0) && (vertexNum < myMultiType->listOfTypeShapes[shapeNum]->GetNumTypeVertices()))
	{
		//-------------------------------
		// Get the vertex in question.
		Stuff::Point3D vertexPos;
		TG_TypeShapePtr typeShape = (TG_TypeShapePtr)myMultiType->listOfTypeShapes[shapeNum];
		
		vertexPos.Multiply(typeShape->listOfTypeVertices[vertexNum].position,listOfShapes[shapeNum].shapeToWorld);
		
		result.x = -vertexPos.x;
		result.y = vertexPos.z;
		result.z = vertexPos.y;
		
		//---------------------------------------
		// Rotate vertex if rotation is NON-zero
		if (rotation != 0.0f)
		{
			Rotate(result,rotation);
		}
	}
	
	return result;
}

//-------------------------------------------------------------------------------
//This function sets the fog values for the shape.  Straight fog right now.
void TG_MultiShape::SetFogRGB (DWORD fRGB)
{
	for (long i=0;i<numTG_Shapes;i++)
	{
		listOfShapes[i].node->SetFogRGB(fRGB);
	}
}	

//-------------------------------------------------------------------------------
//This function sets the list of lights used by the TransformShape function
//to light the shape.
//Function returns 0 if lightList entries are all OK.  -1 otherwise.
//
long TG_MultiShape::SetLightList (TG_LightPtr *lightList, DWORD nLights)
{
	//-----------------------------------------------------
	// Static member of TG_Shape.  Only need to call once.
	// NOT a static function in case lights need to change
	// on a shape by shape basis!
	return (listOfShapes[0].node->SetLightList(lightList,nLights));
}	

//-------------------------------------------------------------------------------
Stuff::Vector3D TG_MultiShape::GetTransformedNodePosition (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot, const char *nodeId)
{
	Stuff::LinearMatrix4D 	shapeOrigin;
	Stuff::LinearMatrix4D 	localShapeOrigin;
//	Stuff::LinearMatrix4D 	invShapeOrigin;
	
	shapeOrigin.BuildRotation(*rot);
	shapeOrigin.BuildTranslation(*pos);
//	invShapeOrigin.Invert(shapeOrigin);
	
	Stuff::EulerAngles angles(*rot);

	long i=0;

	Stuff::Vector3D result;
	result.x = result.y = result.z = 0.0f;
	
	for (i=0;i<numTG_Shapes;i++)
	{
		//-----------------------------------------------------------------
		// Scan through the list of shapes and dig out the number needed.
		// DO NOT UPDATE THE HEIRARCHY!!!!
		// This thing may not have updated itself this turn yet!!!
		if (S_stricmp(listOfShapes[i].node->myType->getNodeId(),nodeId) == 0)
		{
			result.x = -listOfShapes[i].shapeToWorld.entries[3];
			result.z = listOfShapes[i].shapeToWorld.entries[7];
			result.y = listOfShapes[i].shapeToWorld.entries[11];
			
			return result;
		}
	}
	
	return result;
}

//-------------------------------------------------------------------------------
Stuff::Vector3D TG_MultiShape::GetTransformedNodePosition (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot, long nodeId)
{
	Stuff::LinearMatrix4D 	shapeOrigin;
	Stuff::LinearMatrix4D 	localShapeOrigin;
//	Stuff::LinearMatrix4D 	invShapeOrigin;
	
	shapeOrigin.BuildRotation(*rot);
	shapeOrigin.BuildTranslation(*pos);
//	invShapeOrigin.Invert(shapeOrigin);
	
	Stuff::EulerAngles angles(*rot);

	Stuff::Vector3D result;
	result.x = result.y = result.z = 0.0f;
	
	if ((nodeId >= 0) && (nodeId < numTG_Shapes))
	{
		result.x = -listOfShapes[nodeId].shapeToWorld.entries[3];
		result.z = listOfShapes[nodeId].shapeToWorld.entries[7];
		result.y = listOfShapes[nodeId].shapeToWorld.entries[11];
			
		return result;
	}
	
	return result;
}

Stuff::UnitQuaternion moveem;
//-------------------------------------------------------------------------------
//This function does the actual transform math, clip checks and lighting math.
//The matrices passed in are the translation/rotation matrix for the shape and
//Its inverse.  Also will animate.
//Function returns -1 if all vertex screen positions are off screen.
//Function returns 0 if any one vertex screen position is off screen.
//Function returns 1 is all vertex screen positions are on screen.
// NOTE:  THIS IS NOT A RIGOROUS CLIP!!!!!!!!!

float yawRotation = 0.0f;

#ifdef LAB_ONLY
__int64 MCTimeTransformandLight 	= 0;
__int64 MCTimeAnimationandMatrix	= 0;
__int64 MCTimePerShapeTransform		= 0;
#endif

// Slice 2 (object-offload) — Stage 2.B: dispatch flag for the per-leaf
// MultiTransformShape call inside TransformMultiShape's body. Set true by
// TransformMultiShape_PositionsOnly, cleared after the inner call returns.
// File-static so the state is local to the per-frame transform dispatch.
// Single-threaded by construction (MC2 update is serial; no actor's update
// runs while another's TransformMultiShape is in flight).
static bool s_multiShapePositionsOnly = false;
// Track B: when true, TransformMultiShape populates shapeToWorld but skips the
// pool-allocating MultiTransformShape_* dispatch and the s_cameraOrigin read.
static bool s_buildRecipeOnly = false;

long TG_MultiShape::TransformMultiShape (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot)
{
	::mc2_object_recon::Scope _recon_mShape_(
		&::mc2_object_recon::g_per_frame.mShape_total_ns,
		&::mc2_object_recon::g_per_frame.mShape_calls);
	// F3 CPU projection cost-baseline: per-multishape scope, aggregated
	// into the tgl_transform per-frame bucket. No-op when env OFF.
	::mc2_cpu_proj_cost::Scope _f3_tgl_scope(
	    ::mc2_cpu_proj_cost::BUCKET_TGL_TRANSFORM);
	::mc2_cpu_proj_cost::add_workload_tgl_transform(numTG_Shapes);
    //Profile T&L so I can break out GameLogic from T&L
#ifdef LAB_ONLY
    __int64 x;
    x=GetCycles();
#endif

    Stuff::LinearMatrix4D 	shapeOrigin;
    Stuff::LinearMatrix4D	shadowOrigin;
    Stuff::LinearMatrix4D 	localShapeOrigin;

    shapeOrigin.BuildRotation(*rot);
    shapeOrigin.BuildTranslation(*pos);

    Stuff::EulerAngles angles(*rot);
    yawRotation = angles.yaw;

    shadowOrigin.BuildRotation(Stuff::EulerAngles(-angles.pitch,0.0f,0.0f));
    shadowOrigin.BuildTranslation(*pos);

    long i=0;
    Stuff::Point3D camPosition;
    // Guard null — s_cameraOrigin is unset during Mission::init (before first Camera::update).
    // s_buildRecipeOnly skips per-vertex dispatch so camPosition is never used in that mode.
    if (TG_Shape::s_cameraOrigin)
        camPosition = *TG_Shape::s_cameraOrigin;
    else
        { camPosition.x = camPosition.y = camPosition.z = 0.0f; }

    Stuff::Matrix4D  shapeToClip, rootShapeToClip;
    Stuff::Point3D backFacePoint;

    TG_ShapeRecPtr childChain[MAX_NODES];

    // D-gpu-pose-instrument: PerShapeLoop wraps the per-shape iteration.
    // Per-mech-per-call (fires once per TransformMultiShape invocation),
    // not per-leaf-iteration. Captures the WHOLE loop including hierarchy
    // walk + per-leaf dispatch + MultiTransformShadows. Future GPU-compute
    // target.
    { ZoneScopedN("TG.MultiShape.PerShapeLoop");
    for (i=0;i<numTG_Shapes;i++)
    {
        //----------------------------------------------
        // Must set each transform!  Animating Textures!
        for (long j=0;j<myMultiType->numTextures;j++)
        {
            listOfShapes[i].node->myType->SetTextureHandle(j,myMultiType->listOfTextures[j].mcTextureNodeIndex);
            listOfShapes[i].node->myType->SetTextureAlpha(j,myMultiType->listOfTextures[j].textureAlpha); 
        }

        //-----------------------------------------------------------------
        // Heirarchy Animation Code.
        //
        // Simple, really.  For each shape in list, traverse back UP
        // the heirarchy and store the traversal pointers in a temp list.
        // Starting at the TOP of the heirarchy and for each shape, 
        // check if matrix set for that shape. If so, next node down.  If
        // not, copy above matrix into node and apply animation data.
        // Do this until at end of this heirarchy.
        long curChild = 0;
        childChain[curChild] = &listOfShapes[i];
        while (childChain[curChild]->parentNode)
        {
            curChild++;

            gosASSERT(curChild < MAX_NODES);

            childChain[curChild] = childChain[curChild-1]->parentNode;
        }

        long fNum = float2long(frameNum);

        Stuff::Point3D zero;
        zero.x = zero.y = zero.z = 0.0f;
        for (int j=curChild;j>=0;j--)
        {
            if (childChain[j]->calcedThisFrame != turn)
            {
                if (j == curChild)		//This is the ROOT Node.
                {
                    //----------------------------------------------------
                    // Top O the hierarchy.  Used passed in shapeMatrices
                    // Apply any animation data, if data is non-NULL
                    if (childChain[j]->currentAnimation)
                    {
                        //--------------------------------------
                        // Slerp between current and next frame
                        Stuff::UnitQuaternion slerpQuat;
                        slerpQuat.x = slerpQuat.y = slerpQuat.z = 0.0f;
                        slerpQuat.w = 1.0f;

                        if (childChain[j]->currentAnimation->quat)
                            slerpQuat = childChain[j]->currentAnimation->quat[fNum];

                        //--------------------------------------
                        //First Apply Animation to this local piece of heirarchy.
                        // If piece had base rotation, apply it.  Otherwise, no.
                        Stuff::UnitQuaternion totalRotation = slerpQuat;
                        if ((childChain[j]->baseRotation.w == 1.0f) && 
                                (childChain[j]->baseRotation.x == 0.0f) &&
                                (childChain[j]->baseRotation.y == 0.0f) && 
                                (childChain[j]->baseRotation.z == 0.0f))
                        {
                        }
                        else
                        {
                            totalRotation.Multiply(slerpQuat,childChain[j]->baseRotation);
                            totalRotation.Normalize();
                        }

                        localShapeOrigin.BuildRotation(totalRotation);

                        if (childChain[j]->currentAnimation->pos)
                            localShapeOrigin.BuildTranslation(childChain[j]->currentAnimation->pos[fNum]);		//SPECIAL.  ROOT HAS ITS OWN OFFSETS!
                        else
                            localShapeOrigin.BuildTranslation(childChain[j]->node->myType->GetNodeCenter());

                        childChain[j]->localShapeToWorld = localShapeOrigin;

                        //------------------------------------------------------------------
                        //Then move the piece of the heirarchy into the frame of the parent
                        childChain[j]->shapeToWorld.Multiply(childChain[j]->localShapeToWorld,shapeOrigin);

                        childChain[j]->worldToShape.Invert(childChain[j]->shapeToWorld);
                        childChain[j]->calcedThisFrame = turn;
                    }
                    else
                    {
                        localShapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
                        localShapeOrigin.BuildTranslation(childChain[j]->node->myType->GetNodeCenter());

                        childChain[j]->localShapeToWorld = localShapeOrigin;

                        childChain[j]->shapeToWorld.Multiply(childChain[j]->localShapeToWorld,shapeOrigin);

                        childChain[j]->worldToShape.Invert(childChain[j]->shapeToWorld);
                        childChain[j]->calcedThisFrame = turn;
                    }
                }
                else
                {
                    //----------------------------------------------------------------------
                    // Not Top O the Heirarchy.  Figure out matrix based on Animation data.
                    if (childChain[j]->currentAnimation)
                    {
                        //--------------------------------------
                        // Slerp between current and next frame
                        Stuff::UnitQuaternion slerpQuat;
                        slerpQuat.x = slerpQuat.y = slerpQuat.z = 0.0f;
                        slerpQuat.w = 1.0f;

                        if (childChain[j]->currentAnimation->quat)
                            slerpQuat = childChain[j]->currentAnimation->quat[fNum];

                        //--------------------------------------
                        //First Apply Animation to this local piece of heirarchy.
                        // If piece had base rotation, apply it.  Otherwise, no.
                        Stuff::UnitQuaternion totalRotation = slerpQuat;
                        if ((childChain[j]->baseRotation.w == 1.0f) && 
                                (childChain[j]->baseRotation.x == 0.0f) &&
                                (childChain[j]->baseRotation.y == 0.0f) && 
                                (childChain[j]->baseRotation.z == 0.0f))
                        {
                        }
                        else
                        {
                            totalRotation.Multiply(slerpQuat,childChain[j]->baseRotation);
                            totalRotation.Normalize();
                        }

                        localShapeOrigin.BuildRotation(totalRotation);

                        if (childChain[j]->currentAnimation->pos)
                            localShapeOrigin.BuildTranslation(childChain[j]->currentAnimation->pos[fNum]);
                        else
                            localShapeOrigin.BuildTranslation(childChain[j]->node->myType->GetRelativeNodeCenter());

                        childChain[j]->localShapeToWorld = localShapeOrigin;

                        //------------------------------------------------------------------
                        //Then move the piece of the heirarchy into the frame of the parent
                        childChain[j]->localShapeToWorld.Multiply(localShapeOrigin,childChain[j+1]->localShapeToWorld);

                        //------------------------------------------------------------------
                        // Then deal with global translation.
                        childChain[j]->shapeToWorld.Multiply(childChain[j]->localShapeToWorld,shapeOrigin);

                        childChain[j]->worldToShape.Invert(childChain[j]->shapeToWorld);
                        childChain[j]->calcedThisFrame = turn;
                    }
                    else
                    {
                        //--------------------------------------
                        // NO Animation if we are here.
                        //
                        // Apply Base Rotation.  If it is Zero, no problem!
                        Stuff::UnitQuaternion totalRotation = childChain[j]->baseRotation;
                        localShapeOrigin.BuildRotation(totalRotation);
                        localShapeOrigin.BuildTranslation(childChain[j]->node->myType->GetRelativeNodeCenter());

                        childChain[j]->localShapeToWorld = localShapeOrigin;

                        //------------------------------------------------------------------
                        //Then move the piece of the heirarchy into the frame of the parent
                        localShapeOrigin = childChain[j]->localShapeToWorld;
                        childChain[j]->localShapeToWorld.Multiply(localShapeOrigin,childChain[j+1]->localShapeToWorld);

                        //------------------------------------------------------------------
                        // Then deal with global translation.
                        childChain[j]->shapeToWorld.Multiply(childChain[j]->localShapeToWorld,shapeOrigin);

                        childChain[j]->worldToShape.Invert(childChain[j]->shapeToWorld);
                        childChain[j]->calcedThisFrame = turn;
                    }
                }
            }
        }

        if (useFaceLighting || useVertexLighting)
        {
            //----------------------------------------------------
            // Setup Lighting here.
            if (Environment.Renderer != 3)
            {
                for (long iLight=0;iLight<TG_Shape::s_numLights;iLight++)
                {
                    if ((TG_Shape::s_listOfLights[iLight] != NULL) && (TG_Shape::s_listOfLights[iLight]->active))
                    {
                        switch (TG_Shape::s_listOfLights[iLight]->lightType)
                        {
                            case TG_LIGHT_AMBIENT:
                                {
                                    //No Setup needed for Ambient light
                                }
                                break;

                            case TG_LIGHT_INFINITE:
                                {
                                    if (TG_Shape::s_listOfLights[iLight] != NULL)
                                    {
                                        TG_Shape::s_lightToShape[iLight].Multiply(TG_Shape::s_listOfLights[iLight]->lightToWorld,listOfShapes[i].worldToShape);
                                        Stuff::UnitVector3D uVec;
                                        TG_Shape::s_lightToShape[iLight].GetLocalForwardInWorld(&uVec);
                                        TG_Shape::s_lightDir[iLight] = uVec;

                                        if (listOfShapes[i].parentNode == NULL)
                                        {
                                            TG_Shape::s_rootLightDir[iLight] = TG_Shape::s_lightDir[iLight];
                                            //if (angles.yaw != 0.0f )
                                            //	RotateLight(TG_Shape::s_rootLightDir[iLight],-angles.yaw);
                                        }
                                    }
                                }
                                break;

                            case TG_LIGHT_INFINITEWITHFALLOFF:
                                {
                                    if (TG_Shape::s_listOfLights[iLight] != NULL)
                                    {
                                        TG_Shape::s_lightToShape[iLight].Multiply(TG_Shape::s_listOfLights[iLight]->lightToWorld,listOfShapes[i].worldToShape);
                                        Stuff::UnitVector3D uVec;
                                        TG_Shape::s_lightToShape[iLight].GetLocalForwardInWorld(&uVec);
                                        TG_Shape::s_lightDir[iLight] = uVec;

                                        if (listOfShapes[i].parentNode == NULL)
                                        {
                                            TG_Shape::s_rootLightDir[iLight] = TG_Shape::s_lightDir[iLight];
                                        }
                                    }
                                }
                                break;

                            case TG_LIGHT_POINT:
                                {
                                    if (TG_Shape::s_listOfLights[iLight] != NULL)
                                    {
                                        Stuff::Point3D lightPos;
                                        lightPos = TG_Shape::s_listOfLights[iLight]->direction;

                                        Stuff::Point3D shapePosition;
                                        shapePosition = listOfShapes[i].shapeToWorld;

                                        shapePosition -= lightPos;
                                        shapePosition.y = 0.0f;
                                        TG_Shape::s_lightDir[iLight] = shapePosition;
                                        //if (angles.yaw != 0.0f )
                                        //	RotateLight(TG_Shape::s_lightDir[iLight],-angles.yaw);
                                    }

                                    if (listOfShapes[i].parentNode == NULL)
                                    {
                                        TG_Shape::s_rootLightDir[iLight] = TG_Shape::s_lightDir[iLight];
                                    }
                                }
                                break;

                            case TG_LIGHT_TERRAIN:
                                {
                                    if (TG_Shape::s_listOfLights[iLight] != NULL)
                                    {
                                        Stuff::Point3D lightPos;
                                        lightPos = TG_Shape::s_listOfLights[iLight]->direction;

                                        Stuff::Point3D shapePosition;
                                        shapePosition = listOfShapes[i].shapeToWorld;

                                        shapePosition -= lightPos;
                                        shapePosition.y = 0.0f;
                                        TG_Shape::s_lightDir[iLight] = shapePosition;
                                    }

                                    if (listOfShapes[i].parentNode == NULL)
                                    {
                                        TG_Shape::s_rootLightDir[iLight] = TG_Shape::s_lightDir[iLight];
                                    }
                                }
                                break;

                            case TG_LIGHT_SPOT:
                                {
                                    if (TG_Shape::s_listOfLights[iLight] != NULL)
                                    {
                                        Stuff::Point3D lightPos;
                                        lightPos = TG_Shape::s_listOfLights[iLight]->direction;

                                        Stuff::Point3D shapePosition;
                                        shapePosition = listOfShapes[i].shapeToWorld;

                                        shapePosition -= lightPos;
                                        shapePosition.y = 0.0f;
                                        TG_Shape::s_lightDir[iLight] = shapePosition;
                                        //if (angles.yaw != 0.0f )
                                        //	RotateLight(TG_Shape::s_lightDir[iLight],-angles.yaw);

                                        lightPos = TG_Shape::s_listOfLights[iLight]->spotDir;
                                        shapePosition = listOfShapes[i].shapeToWorld;

                                        shapePosition -= lightPos;
                                        shapePosition.y = 0.0f;
                                        TG_Shape::s_spotDir[iLight] = shapePosition;
                                        //if (angles.yaw != 0.0f )
                                        //	RotateLight(TG_Shape::s_spotDir[iLight],-angles.yaw);

                                        if (listOfShapes[i].parentNode == NULL)
                                        {
                                            TG_Shape::s_rootLightDir[iLight] = TG_Shape::s_spotDir[iLight];
                                        }
                                    }
                                }
                                break;
                        }
                    }
                }
            }
        }

        shapeToClip.Multiply(listOfShapes[i].shapeToWorld,TG_Shape::s_worldToClip);
        backFacePoint.Multiply(camPosition,listOfShapes[i].worldToShape);

#ifdef LAB_ONLY
        x=GetCycles()-x;
        MCTimeAnimationandMatrix += x;
        x=GetCycles();
#endif

        // Track B: recipe-build-only mode — shapeToWorld is now populated above.
        // Skip pool-allocating per-vertex/per-face dispatch entirely.
        if (s_buildRecipeOnly)
            continue;

        // Slice 2 (object-offload) — Stage 2.B: dispatch on the file-static
        // flag set by TransformMultiShape_PositionsOnly. The positions-only
        // variant strips the per-vertex / per-face lighting kernels; slice 1
        // batcher provides GPU vertex lighting via the calc_light() kernel
        // finished in lighting.hglsl (Stage 2.C). Default branch (flag false)
        // is the unchanged legacy call.
        // D-gpu-pose-instrument: PerLeaf zone wraps the per-shape dispatch.
        // Per-leaf-iteration (fires N times per loop). Per-leaf work is
        // ~500ns (pool alloc + per-vertex projection + per-face cull),
        // above the 100ns floor; aggregate Tracy overhead <1% frame budget.
        { ZoneScopedN("TG.MultiShape.PerLeaf");
        if (s_multiShapePositionsOnly)
        {
            listOfShapes[i].node->MultiTransformShape_PositionsOnly(&shapeToClip,&backFacePoint,listOfShapes[i].parentNode,isHudElement,alphaValue,isClamped);
        }
        else
        {
            listOfShapes[i].node->MultiTransformShape(&shapeToClip,&backFacePoint,listOfShapes[i].parentNode,isHudElement,alphaValue,isClamped);
        }
        } // end PerLeaf zone

        // D-gpu-pose-instrument: ShadowProj wraps the MultiTransformShadows
        // dispatch (per-light × per-vertex shadow projection). Per-leaf-
        // iteration; only fires when useShadows && d_useShadows.
        if (useShadows && d_useShadows)
        {
            ZoneScopedN("TG.MultiShape.ShadowProj");
            listOfShapes[i].node->MultiTransformShadows(pos, &(listOfShapes[i].shapeToWorld),yawRotation);
        }

#ifdef LAB_ONLY
        x=GetCycles()-x;
        MCTimePerShapeTransform += x;
        x=GetCycles();
#endif
    }
    } // end PerShapeLoop zone

#ifdef LAB_ONLY
    MCTimeTransformandLight = MCTimeAnimationandMatrix + MCTimePerShapeTransform;
#endif
    return(0);
}

//-------------------------------------------------------------------------------
// Slice 2 (object-offload) — Stage 2.B
// TransformMultiShape_PositionsOnly: thin wrapper that flips the file-static
// per-leaf dispatch flag, calls the existing TransformMultiShape (so the
// heirarchy animation, shapeToClip computation, and TG_Shape::s_* state setup
// all run unchanged), then clears the flag. The flag is read at the per-leaf
// MultiTransformShape call site near the bottom of TransformMultiShape's body.
//-------------------------------------------------------------------------------
long TG_MultiShape::TransformMultiShape_PositionsOnly (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot)
{
    s_multiShapePositionsOnly = true;
    long result = TransformMultiShape(pos, rot);
    s_multiShapePositionsOnly = false;
    return result;
}

//-------------------------------------------------------------------------------
// Track B (widen registry): recipe-build-only variant.
// Runs the hierarchy traversal to populate listOfShapes[i].shapeToWorld per
// leaf without touching TGL vertex/face/color pools and without requiring
// TG_Shape::s_cameraOrigin to be non-null. Called from BldgAppearance/
// TreeAppearance::registerStatic during Mission::init before Camera::update.
//-------------------------------------------------------------------------------
long TG_MultiShape::TransformMultiShape_BuildRecipe (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot)
{
    s_buildRecipeOnly = true;
    long result = TransformMultiShape(pos, rot);
    s_buildRecipeOnly = false;
    return result;
}

//-------------------------------------------------------------------------------
// Slice D-leaf-skip-v2 (2026-05-09): thin runtime wrapper that reuses the
// s_buildRecipeOnly mechanism for the GPU mech body callsite. Skip per-leaf
// dispatch + MultiTransformShadows; preserve hierarchy walk. Recon proved
// per-leaf state on mechShape has zero practical consumer in modern + GPU
// mech mode (Slice A bypasses Render(true); RenderShadows unreachable on
// tessellation; PerPolySelect-on-mechs theoretical via fallback findObjectByMouse
// but findMoverByMouse rect-only test catches all real mech selection).
//-------------------------------------------------------------------------------
long TG_MultiShape::TransformMultiShape_HierarchyOnly (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot)
{
    // ANIM-HIERARCHYONLY-DEDUP-1: this was a byte-identical copy of
    // TransformMultiShape_BuildRecipe (same s_buildRecipeOnly mechanism). Kept as a
    // distinct entry point for the GPU-mech-body callsite (semantic name) but now
    // DELEGATES to the single implementation — no duplicated flag dance. Byte-identical.
    return TransformMultiShape_BuildRecipe(pos, rot);
}

//-------------------------------------------------------------------------------
// Slice 2 (object-offload) — Stage 2.D.2 fix:
// Cache the GPU light-data index while worldLights[0]->aRGB is correct for
// THIS actor (during update(), immediately after SetLightList is called).
// By the time submitMultiShape() runs (during renderLists()), later actors
// will have overwritten worldLights[0]->aRGB for their own terrain positions.
// Calling GatherGpuObjectLightDataOnly() there produces the WRONG light color.
//
// This method locates the first SHAPE_NODE leaf (same heuristic as
// submitMultiShape's firstShapeNodeLeaf scan) and calls
// GatherGpuObjectLightDataOnly() to cache the dedup-cache index in
// cachedGpuLightIndex_. submitMultiShape then reads cachedGpuLightIndex_
// instead of calling GatherGpuObjectLightDataOnly() itself.
//
// Only active when g_useGpuObjects is true; no-op otherwise so non-GPU
// paths have zero overhead.
//-------------------------------------------------------------------------------
void TG_MultiShape::CacheGpuLightData()
{
    // PERF 2026-05-07: stripped hot Tracy zones CacheGpuLightData,
    // CacheGpuLightData findLeaf, and CacheGpuLightData GatherGpuObjectLightDataOnly.
    //
    // Slice B1 (2026-05-09): also honor g_useGpuMechs so an operator
    // running MC2_GPU_OBJECTS=0 MC2_GPU_MECHS=1 still gets the cache
    // refresh for mechs. Adversarial review MAJOR-1.
    if (!g_useGpuObjects && !g_useGpuMechs)
        return;

    // Find first SHAPE_NODE leaf — same logic as submitMultiShape.
    TG_Shape* firstShapeNodeLeaf = nullptr;
    {
        for (int i = 0; i < numTG_Shapes; ++i) {
            TG_ShapeRec& rec = listOfShapes[i];
            if (!rec.processMe || !rec.node) continue;
            TG_Shape* child = rec.node;
            if (!child->myType) continue;
            if (child->myType->GetNodeType() != SHAPE_NODE) continue;
            firstShapeNodeLeaf = child;
            break;
        }
    }

    if (firstShapeNodeLeaf != nullptr) {
        cachedGpuLightIndex_ = firstShapeNodeLeaf->GatherGpuObjectLightDataOnly();
        cachedFrame_         = g_mc2FrameCounter;
    }
}

// [LIGHTBAKE v1] First SHAPE_NODE leaf's durable post-decompose
// lightData_ (TG_Shape member; TG_MultiShape is friend of TG_Shape).
// Call AFTER CacheGpuLightData() has run this frame:
// GatherGpuObjectLightDataOnly -> addLightDataStructureWithPerActorColor
// wrote the post-template-copy + in-place decomposeFirstActiveLightColor
// struct into leaf->lightData_, which resetLightData does NOT touch
// (unlike the per-frame scratch slot). Same leaf scan as
// CacheGpuLightData for identity.
const TG_HWLightsData* TG_MultiShape::peekCachedLeafLightData()
{
    TG_Shape* firstShapeNodeLeaf = nullptr;
    for (int i = 0; i < numTG_Shapes; ++i) {
        TG_ShapeRec& rec = listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        TG_Shape* child = rec.node;
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;
        firstShapeNodeLeaf = child;
        break;
    }
    if (firstShapeNodeLeaf == nullptr) return nullptr;
    return &firstShapeNodeLeaf->lightData_;
}

// [LIGHTBAKE v1] Re-emit a mission-load-baked constant into a per-frame
// slot WITHOUT GatherLights/decompose/template recompute. Per-frame slot
// WRITE stays (Shape-C O(1) consumer); the RECOMPUTE is what dies. Sets
// cachedGpuLightIndex_/cachedFrame_ so the existing
// staticReg.lightDataIndex = getCachedGpuLightIndex() capture + batcher
// read are unchanged.
void TG_MultiShape::EmitBakedGpuLightData(int32_t recipeIndex, const TG_HWLightsData& baked)
{
    // [LIGHTBAKE v2] THE substitutive edit (adversarial-review C1 locus):
    // the static recipe's permanent light slot IS recipeIndex (the CPU
    // mirror lives at lightData_[recipeIndex], written once by
    // mc2WriteStaticLightSlot, re-shipped every frame by the unchanged
    // whole-buffer upload). So this is a pure pointer assignment -- NO
    // mc2SubmitBakedLightSlot, NO addLightDataStructure, NO 1792B FNV,
    // NO 1792B memcmp. This is what takes `addLightDataStructure scan`
    // -> ~0 for the static class (the ~1840 calls/~1ms/frame survivor).
    (void)baked;  // identity is recipeIndex; the struct already persists in the slot
    cachedGpuLightIndex_ = static_cast<uint32_t>(recipeIndex);
    cachedFrame_         = g_mc2FrameCounter;
}

void TG_MultiShape::ResubmitCachedGpuLightData()
{
    // FRAME-JOBS-2D invariant: this function must never run on worker threads.
    // g_workerResubmitCalls > 0 means split boundary is broken — touchWorkerPrepass
    // called into the light-data path instead of touchSerialCommit.
    if (isFrameJobsWorkerThread())
        g_workerResubmitCalls.fetch_add(1, std::memory_order_relaxed);

    // Slice B1 (2026-05-09): see CacheGpuLightData rationale.
    if (!g_useGpuObjects && !g_useGpuMechs)
        return;

    // [LIGHTBRIDGE v1] C6 repoint: if this multi already populated a VALID
    // slot THIS frame, that slot is still live (per-frame reset by
    // resetLightData/clearArrays at frame start) — return it and skip the
    // direct addLightDataStructure 1792B FNV + 1792B memcmp entirely. The
    // cachedGpuLightIndex_ != sentinel guard is load-bearing: the
    // registration path (gos_static_prop_registry.cpp) sets cachedFrame_
    // without a valid index. Kill-switch OFF -> legacy resubmit bit-for-bit.
    extern bool mc2LightBridgeRepointEnabled();
    if (mc2LightBridgeRepointEnabled() &&
        cachedFrame_ == g_mc2FrameCounter &&
        cachedGpuLightIndex_ != 0xFFFFFFFFu) {
        static bool s_c6Logged = false;
        if (!s_c6Logged) {
            s_c6Logged = true;
            printf("[LIGHTBRIDGE v1] event=c6_fastpath_hit\n");
            fflush(stdout);
        }
        return;
    }

    TG_Shape* firstShapeNodeLeaf = nullptr;
    for (int i = 0; i < numTG_Shapes; ++i) {
        TG_ShapeRec& rec = listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        TG_Shape* child = rec.node;
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;
        firstShapeNodeLeaf = child;
        break;
    }

    if (firstShapeNodeLeaf != nullptr) {
        cachedGpuLightIndex_ = firstShapeNodeLeaf->ResubmitCachedLightData();
        cachedFrame_         = g_mc2FrameCounter;
    }
}

//-------------------------------------------------------------------------------
//This function takes the current listOfVisibleFaces and draws them using
//gos_DrawTriangle.
void TG_MultiShape::Render (bool refreshTextures, float forceZ)
{
	for (long i=0;i<numTG_Shapes;i++)
	{
		if (listOfShapes[i].processMe && listOfShapes[i].node)
		{
			//-------------------------------------------------------
			// Only need to due for unique instance items like mechs
			if (refreshTextures)
			{
				for (long j=0;j<myMultiType->numTextures;j++)
				{
					listOfShapes[i].node->myType->SetTextureHandle(j,myMultiType->listOfTextures[j].mcTextureNodeIndex);
					listOfShapes[i].node->myType->SetTextureAlpha(j,myMultiType->listOfTextures[j].textureAlpha); 
				}
			}

			Stuff::Matrix4D  shapeToClip;
			shapeToClip.Multiply(listOfShapes[i].shapeToWorld, TG_Shape::s_worldToClip);
		
            Stuff::Matrix4D shape2world(listOfShapes[i].shapeToWorld);
			listOfShapes[i].node->Render(forceZ,isHudElement,alphaValue,isClamped, &shapeToClip, &shape2world);
		}
	}
}	

//-------------------------------------------------------------------------------
//This function takes the current listOfVisibleFaces and draws them using
//gos_DrawTriangle.
void TG_MultiShape::RenderShadows (bool refreshTextures)
{
	long start = 0;
	for (long i=0;i<numTG_Shapes;i++)
	{
		if (listOfShapes[i].processMe && listOfShapes[i].node)
		{
			//-------------------------------------------------------
			// Only need to due for unique instance items like mechs
			if (refreshTextures)
			{
				for (long j=0;j<myMultiType->numTextures;j++)
				{
					listOfShapes[i].node->myType->SetTextureHandle(j,myMultiType->listOfTextures[j].mcTextureNodeIndex);
					listOfShapes[i].node->myType->SetTextureAlpha(j,myMultiType->listOfTextures[j].textureAlpha); 
				}
			}

			start = listOfShapes[i].node->RenderShadows(start);
		}
	}
}	

//-------------------------------------------------------------------------------
// This function takes the shape named nodeName and all of its children and stops processing
// them forever.  Since we can never re-attach a mech arm in the field, this is OK!
// However, should we want this functionality, it is easy to add!
void TG_MultiShape::StopUsing (const char *nodeName)
{
	//First, find all shapes which are children of nodeName, including nodeName!
	TG_ShapeRecPtr childChain[MAX_NODES];
	TG_ShapeRecPtr detachables[MAX_NODES];
	
	long curShape = 0;
	long i=0;
	for (i=0;i<numTG_Shapes;i++)
	{
		//----------------------------------------------
		long curChild = 0;
		childChain[curChild] = &listOfShapes[i];
		while (childChain[curChild]->parentNode)
		{
			curChild++;

			gosASSERT(curChild < MAX_NODES);

			childChain[curChild] = childChain[curChild-1]->parentNode;
		}

		Stuff::Point3D zero;
		zero.x = zero.y = zero.z = 0.0f;
		for (long j=curChild;j>=0;j--)
		{
			if (S_stricmp(childChain[j]->node->getNodeName(),nodeName) == 0)
			{
				detachables[curShape] = childChain[0];
				curShape++;
				break;
			}
		}	
	}
	
	//Then, mark all of the shapes found with a "don't process me" flag
	for (i=0;i<curShape;i++)
	{
		//Keep the joint_xUARM so that the shoulder sparks work.
		if (S_stricmp(detachables[i]->node->getNodeName(),nodeName) != 0)
			detachables[i]->processMe = false;
	}
}

//-------------------------------------------------------------------------------
// This function takes the shape named nodeName and all of its children, detaches them
// from the current heirarchy and stuffs them into a new MultiShape which it passes back
// Uses are endless but for now limited to blowing the arms off of the mechs!
TG_MultiShapePtr TG_MultiShape::Detach (const char *nodeName)
{
	//First, find all shapes which are children of nodeName, including nodeName!
	TG_ShapeRecPtr childChain[MAX_NODES];
	TG_ShapeRecPtr detachables[MAX_NODES];
	
	long curShape = 0;
	long i=0;
	for (i=0;i<numTG_Shapes;i++)
	{
		//----------------------------------------------
		long curChild = 0;
		childChain[curChild] = &listOfShapes[i];
		while (childChain[curChild]->parentNode)
		{
			curChild++;

			gosASSERT(curChild < MAX_NODES);

			childChain[curChild] = childChain[curChild-1]->parentNode;
		}

		Stuff::Point3D zero;
		zero.x = zero.y = zero.z = 0.0f;
		for (long j=curChild;j>=0;j--)
		{
			if (S_stricmp(childChain[j]->node->getNodeName(),nodeName) == 0)
			{
				detachables[curShape] = childChain[0];
				curShape++;
				break;
			}
		}	
	}
	
	if (!curShape)
		return NULL;

		//OK to return no shape now.
		//STOP(("Unable to detach %s from shape",nodeName));
		
	//Then, create a new TG_MultiShape and stuff it with the shapes found.
	TG_MultiShapePtr resultShape = new TG_MultiShape;
	resultShape->myMultiType = myMultiType;
	resultShape->numTG_Shapes = curShape;
	
	resultShape->listOfShapes = (TG_ShapeRecPtr)TG_Shape::tglHeap->Malloc(sizeof(TG_ShapeRec) * curShape);
	gosASSERT(resultShape->listOfShapes != NULL);

	memset(resultShape->listOfShapes,0,sizeof(TG_ShapeRec) * curShape);

	resultShape->frameNum = 0;
	
	for (i=0;i<curShape;i++)
	{
		resultShape->listOfShapes[i] = *detachables[i];
		
		if (S_stricmp(resultShape->listOfShapes[i].node->getNodeName(),nodeName) == 0)
			resultShape->listOfShapes[i].parentNode = NULL;		//Set new top O heirarchy.
		
		for (long j=0;j<numTG_Shapes;j++)
		{
			if (&listOfShapes[j] == detachables[i])
			{
				//Mark this shape as removable from the previous heirarchy
				listOfShapes[j].node = NULL;
				listOfShapes[j].parentNode = NULL;
			}
		}
	}

	//Rebuild resultShape parent node pointers!
	for (i=0;i<resultShape->numTG_Shapes;i++)
	{
		//----------------------------------------------------------------------------------
		// For each shape, look for another node whose NodeId matches this shape's parentId
		for (long j=0;j<resultShape->numTG_Shapes;j++)
		{
			if (strcmp(resultShape->listOfShapes[i].node->myType->getParentId(),resultShape->listOfShapes[j].node->myType->getNodeId()) == 0)
			{
				//-------------------------------
				// Found it!
				resultShape->listOfShapes[i].parentNode = &(resultShape->listOfShapes[j]);
				break;
			}
		}
	}
	
 	//-------------------------------------------------------------
	// Rebuild the currentShape heirarchy to reflect removed things
	TG_ShapeRecPtr newListOfShapes = (TG_ShapeRecPtr)TG_Shape::tglHeap->Malloc(sizeof(TG_ShapeRec) * (numTG_Shapes - curShape));
	gosASSERT(newListOfShapes != NULL);

	memset(newListOfShapes,0,sizeof(TG_ShapeRec) * (numTG_Shapes - curShape));
	
	long newShapeIndex = 0;
	for (i=0;i<numTG_Shapes;i++)
	{
		if (listOfShapes[i].node != NULL)
		{
			newListOfShapes[newShapeIndex] = listOfShapes[i];
			newShapeIndex++;
		}
	}

	//-------------------------------------------------------------
	// Move the new list over the old list.
	if (listOfShapes)
		TG_Shape::tglHeap->Free(listOfShapes);
	listOfShapes = newListOfShapes;
	
	numTG_Shapes -= curShape;

	//Rebuild parent node pointers!
	for (i=0;i<numTG_Shapes;i++)
	{
		//----------------------------------------------------------------------------------
		// For each shape, look for another node whose NodeId matches this shape's parentId
		for (long j=0;j<numTG_Shapes;j++)
		{
			if (strcmp(listOfShapes[i].node->myType->getParentId(),listOfShapes[j].node->myType->getNodeId()) == 0)
			{
				//-------------------------------
				// Found it!
				listOfShapes[i].parentNode = &(listOfShapes[j]);
				break;
			}
		}
	}

	return resultShape;
}

//-------------------------------------------------------------------------------
// Tells me if the passed in nodeName is a child of the parentName.
bool TG_MultiShape::isChildOf (const char *nodeName, const char* parentName)
{
	//First, find all shapes which are children of nodeName, including nodeName!
	TG_ShapeRecPtr childChain[MAX_NODES];
	TG_ShapeRecPtr detachables[MAX_NODES];
	
	long curShape = 0;
	long i=0;
	for (i=0;i<numTG_Shapes;i++)
	{
		//----------------------------------------------
		long curChild = 0;
		childChain[curChild] = &listOfShapes[i];
		while (childChain[curChild]->parentNode)
		{
			curChild++;

			gosASSERT(curChild < MAX_NODES);

			childChain[curChild] = childChain[curChild-1]->parentNode;
		}

		Stuff::Point3D zero;
		zero.x = zero.y = zero.z = 0.0f;
		for (long j=curChild;j>=0;j--)
		{
			if (S_stricmp(childChain[j]->node->getNodeName(),parentName) == 0)
			{
				detachables[curShape] = childChain[0];
				curShape++;
				break;
			}
		}	
	}
	
	if (!curShape)
		return false;
		
	for (i=0;i<curShape;i++)
	{
		if (S_stricmp(detachables[i]->node->getNodeName(), nodeName) == 0)
			return true;
	}
	
	return false;
}

//-------------------------------------------------------------------------------
// class TG_AnimateShape
//-------------------------------------------------------------------------------
void *TG_AnimateShape::operator new (size_t mySize)
{
	void *result = TG_Shape::tglHeap->Malloc(mySize);
	return result;
}
 
//-------------------------------------------------------------------------------
 void TG_AnimateShape::operator delete (void *us)
{
	TG_Shape::tglHeap->Free(us);
}
//-------------------------------------------------------------------------------
void _TG_Animation::SaveBinaryCopy (File *binFile)
{
	if (S_stricmp(nodeId,"NONE") != 0)
	{
        // sebi: nodeId is not generally TG_NODE_ID bytes long
        char buf[TG_NODE_ID] = {0};
        strncpy(buf, nodeId, TG_NODE_ID);
		binFile->write((MemoryPtr)buf,TG_NODE_ID);	
		binFile->writeInt(-1);			//ShapeIds ALWAYS start with -1.  We will scan on frame 1 please!
		binFile->writeInt(numFrames);
		binFile->writeFloat(frameRate);
		binFile->writeFloat(tickRate);
		
		if (quat)
			binFile->writeInt(sizeof(Stuff::UnitQuaternion) * numFrames);
		else
			binFile->writeInt(0);
			
		if (pos)
			binFile->writeInt(sizeof(Stuff::Point3D) * numFrames);
		else
			binFile->writeInt(0);
		
		if (quat)
			binFile->write((MemoryPtr)quat,sizeof(Stuff::UnitQuaternion) * numFrames);
			
		if (pos)
			binFile->write((MemoryPtr)pos,sizeof(Stuff::Point3D) * numFrames);
	}
}

//-------------------------------------------------------------------------------
void _TG_Animation::LoadBinaryCopy (File *binFile)
{
	binFile->read((MemoryPtr)nodeId,TG_NODE_ID);	
	shapeId = binFile->readInt();
	numFrames = binFile->readInt();
	frameRate = binFile->readFloat();
	tickRate = binFile->readFloat();
	
	int quatRAM = binFile->readInt();
	int posRAM = binFile->readInt();		
	
	if (quatRAM)
	{
		quat = (Stuff::UnitQuaternion *)TG_Shape::tglHeap->Malloc(sizeof(Stuff::UnitQuaternion) * numFrames); 
		binFile->read((MemoryPtr)quat,quatRAM);	
	}
	else
		quat = NULL;
		
	if (posRAM)
	{
		pos = (Stuff::Point3D *)TG_Shape::tglHeap->Malloc(sizeof(Stuff::Point3D) * numFrames); 
		binFile->read((MemoryPtr)pos,posRAM);	
	}
	else
		pos = NULL;
}

//-------------------------------------------------------------------------------
long TG_AnimateShape::LoadBinaryCopy (const char *fileName)
{
	File binFile;
	long result = binFile.open(fileName);
	if (result != NO_ERR)
		return -1;
		
	int animFileVersion = binFile.readInt();
	if (animFileVersion == CURRENT_ANIM_VERSION)
	{
		count = binFile.readInt();
		
		if (count)
		{
			listOfAnimation = (TG_AnimationPtr)TG_Shape::tglHeap->Malloc(sizeof(TG_Animation) * count);
			gosASSERT(listOfAnimation != NULL);
 			
			for (long i=0;i<count;i++)
				listOfAnimation[i].LoadBinaryCopy(&binFile);
		}
		else
			listOfAnimation = NULL;
	}
	else
	{
		return -1;
	}
	
	return 0;
}

//-------------------------------------------------------------------------------
void TG_AnimateShape::SaveBinaryCopy (const char *fileName)
{
	File binFile;
	binFile.create(fileName);
	
	binFile.writeInt(CURRENT_ANIM_VERSION);
	
	//ListOfAnimations
	actualCount = 0;
	if (count)
	{
 		for (long i=0;i<count;i++)
		{
			if (S_stricmp(listOfAnimation[i].nodeId,"NONE") != 0)
				actualCount++;
		}
	}

	binFile.writeInt(actualCount);

	//ListOfAnimations
	if (count)
	{
 		for (long i=0;i<count;i++)
			listOfAnimation[i].SaveBinaryCopy(&binFile);
	}
}

//-------------------------------------------------------------------------------
//This function frees all of the RAM allocated by this class and resets vars to initial state.
void TG_AnimateShape::destroy (void)
{
	if (listOfAnimation)
	{
		for ( int i = 0; i < count; ++i )
		{
			TG_Shape::tglHeap->Free( listOfAnimation[i].pos );
			listOfAnimation[i].pos = NULL;

			TG_Shape::tglHeap->Free( listOfAnimation[i].quat );
			listOfAnimation[i].quat = NULL;
		}

		if (listOfAnimation)
			TG_Shape::tglHeap->Free(listOfAnimation);
			
		listOfAnimation = NULL;
	}
}	

FilePtr tglLogFile = (FilePtr)NULL;

//-------------------------------------------------------------------------------
//This function loads the animation data contained in the file passed in.
//It sets up a pointer to the multi-shape so that animation data for each
//Node in the Multi-Shape can be loaded.
//It mallocs memory.
long TG_AnimateShape::LoadTGMultiShapeAnimationFromASE (const char *fileName, TG_TypeMultiShapePtr shape, bool skipIfBinary, bool forceMakeBinary)
{
	//------------------------------------------------------
	// New!  Checks for Binary file to load instead.
	//
	// If Binary is older or equal to ASE, re-parses ASE and
	// saves binary.
	//
	// If Binary does not exist, does the above, too!
	// 
	// If Binary exists and is newer, just loads that.
	// MUCH FASTER!
	//
	bool makeBinary = false;

	char drive[MAX_PATH];
	char dir[MAX_PATH];
	char name[MAX_PATH];
	char ext[MAX_PATH];
	_splitpath(fileName,drive,dir,name,ext);

	FullPathFileName binaryFileName;
	binaryFileName.init(tglPath,name,".agl");

	if( !forceMakeBinary )
	{
		if (fileExists(binaryFileName) && !fileExists(fileName))
		{
			File binFile;
			
			long result = binFile.open(binaryFileName);
			if (result != NO_ERR)
			{
				makeBinary = true;
			}
			else
			{
				int versionId = binFile.readInt();
				if (versionId != CURRENT_ANIM_VERSION)
					makeBinary = true;
			}
		}
		else
		{
			makeBinary = true;
		}
	}
	else
	{
		makeBinary = true;
	}

	if (!makeBinary)
	{
		if (!skipIfBinary)
			return (LoadBinaryCopy(binaryFileName));
		else
			return 0;
	}
	else
	{
		//-----------------------------------------------------
		// Fully loads and parses an ASE File.  These files are
		// output by 3D Studio MAX's ASCII Scene Exporter.
		//
		shapeIdsSet = false;
	
		//------------------------------------------
		// Check if exists 
		if (!gos_FileExists(fileName))
			return(-1);
		
		long aseFileSize = gos_FileSize(fileName);
	
		//---------------------------------------
		// Create Buffer to read entire file into
		BYTE *aseContents = (BYTE *)malloc(aseFileSize+1);
		gosASSERT(aseContents != NULL);
	
		//---------------------------------------
		// Open File and read it and close it
		HGOSFILE aseFile;
		gos_OpenFile(&aseFile,fileName,READONLY);
	
	#ifdef _DEBUG
		long dataRead = 
	#endif
		gos_ReadFile(aseFile,aseContents,aseFileSize);
		gosASSERT(dataRead == aseFileSize);
	
		gos_CloseFile(aseFile);
	
		aseContents[aseFileSize] = 0;
	
		//----------------------------------------
		// Check for valid ASE Header data.
		if (S_strnicmp(ASE_HEADER,(char *)aseContents,strlen(ASE_HEADER)) != 0)
			return(-1);
	
		//------------------------------------------
		// Get first frame of animation from header
		long firstFrame, lastFrame;
		char *frameId = strstr((char *)aseContents,ASE_ANIM_FIRST_FRAME);
		gosASSERT(frameId != NULL);
		frameId += strlen(ASE_ANIM_FIRST_FRAME)+1;
	
		char numData[512];
		GetNumberData(frameId,numData);
		firstFrame = atol(numData);
	
		frameId = strstr((char *)aseContents,ASE_ANIM_LAST_FRAME);
		gosASSERT(frameId != NULL);
	
		frameId += strlen(ASE_ANIM_LAST_FRAME)+1;
	
		GetNumberData(frameId,numData);
		lastFrame = atol(numData);
	
		//gosASSERT(firstFrame == 0);
		gosASSERT(firstFrame <= lastFrame);
	
		//if (firstFrame == lastFrame)
			//No Animation data at all. Possible?
	
		long numFrames = (lastFrame - firstFrame) + 1;
	
		frameId = strstr((char *)aseContents,ASE_ANIM_FRAME_SPEED);
		gosASSERT(frameId != NULL);
	
		frameId += strlen(ASE_ANIM_FRAME_SPEED)+1;
	
		GetNumberData(frameId,numData);
		float frameRate = (float)atof(numData);
	
		frameId = strstr((char *)aseContents,ASE_ANIM_TICKS_FRAME);
		gosASSERT(frameId != NULL);
	
		frameId += strlen(ASE_ANIM_TICKS_FRAME)+1;
	
		GetNumberData(frameId,numData);
		float tickRate = (float)atof(numData);
	
		//----------------------------------------------------------------------------
		// For each TG_Shape in MultiShape passed in, find any and all animation Data
		count = shape->GetNumShapes();
		listOfAnimation = (TG_AnimationPtr)TG_Shape::tglHeap->Malloc(sizeof(TG_Animation) * shape->GetNumShapes());
		gosASSERT(listOfAnimation != NULL);
	
		//----------------------------------------------------------------------------
		// Scan the Nodes.
		actualCount = 0;
		bool countUp = false;
		for (long i=0;i<count;i++)
		{
			char *nodeId = shape->GetNodeId(i);
			gosASSERT(nodeId != NULL);
		
			char *animScan = (char *)aseContents;
			animScan = strstr(animScan,ASE_ANIMATION);
	
			char nodeName[512];
			sprintf(nodeName,"*NODE_NAME \"%s\"",nodeId);
			while (animScan != NULL)
			{
				animScan += strlen(ASE_ANIMATION)+1;
	
				//------------------------------------------------------
				// We found a TM_ANIMATION Section.
				// Check if the VERY NEXT LINE is the correct NodeName
				char nextLine[1024];
				GetNextLine(animScan,nextLine);
	
				if (strstr(nextLine,nodeName) == NULL)
				{
					animScan = strstr(animScan,ASE_ANIMATION);
				}
				else
				{
					animScan += strlen(nodeName)+1;
					break;
				}
			}
	
			if (animScan == NULL)
			{
				//No Animation Data for this Node.
				strcpy(listOfAnimation[i].nodeId,"NONE");
				listOfAnimation[i].numFrames = numFrames;
				listOfAnimation[i].frameRate = frameRate;
				listOfAnimation[i].tickRate = tickRate;
				listOfAnimation[i].quat = NULL;
				listOfAnimation[i].pos = NULL;
				listOfAnimation[i].shapeId = 0xffffffff;
			}
			else
			{
				//Start with No Animation Data for this Node.
				strcpy(listOfAnimation[i].nodeId,"NONE");
				listOfAnimation[i].numFrames = numFrames;
				listOfAnimation[i].frameRate = frameRate;
				listOfAnimation[i].tickRate = tickRate;
				listOfAnimation[i].quat = NULL;
				listOfAnimation[i].pos = NULL;
				listOfAnimation[i].shapeId = 0xffffffff;

				//---------------------------------
				// Check for positional data first!
				char* scanStart = animScan;
	
				char numData[512];
				char nextLine[1024];
				float timeStamp = firstFrame * tickRate;
	
				//----------------------------------------------------
				// Then the very NEXT LINE most be POS_TRACK data OR
				// there is ONLY rotational Data for this node.
				GetNextLine(animScan,nextLine);
				if (strstr(nextLine,ASE_ANIM_POS_HEADER) != NULL)
				{
					animScan = strstr(animScan,ASE_ANIM_POS_HEADER);
					if (animScan)
					{
						countUp = true;
						actualCount++;
						animScan += strlen(ASE_ANIM_POS_HEADER);
		
						//-----------------------------------------------------------
						// We have positional data at least.  Store everything off.
						listOfAnimation[i].pos = (Stuff::Point3D *)TG_Shape::tglHeap->Malloc(sizeof(Stuff::Point3D) * numFrames);
		
						strcpy(listOfAnimation[i].nodeId,nodeId);
						listOfAnimation[i].numFrames = numFrames;
						listOfAnimation[i].frameRate = frameRate;
						listOfAnimation[i].tickRate = tickRate;
						listOfAnimation[i].shapeId = 0xffffffff;
		
						Stuff::Point3D thisOffset = shape->GetNodeCenter(i);
		
						char LineData[1024];
						GetNextLine(animScan,LineData);
						animScan += strlen(LineData)+1;
		
						for (long j=0;j<listOfAnimation[i].numFrames;j++)
						{
							char *scanData;
							sprintf(nodeName,"%s %d",ASE_ANIM_POS_SAMPLE,(int)timeStamp);
							scanData = strstr(LineData,nodeName);
		
							if (scanData)
							{
								scanData += strlen(nodeName)+1;
		
								GetNumberData(scanData,numData);
								thisOffset.x = -(float)atof(numData);
								scanData += strlen(numData)+1;
		
								GetNumberData(scanData,numData);
								thisOffset.z = (float)atof(numData);
								scanData += strlen(numData)+1;
		
								GetNumberData(scanData,numData);
								thisOffset.y = (float)atof(numData);
								scanData += strlen(numData)+1;
		
								GetNextLine(animScan,LineData);
								animScan += strlen(LineData)+1;
							}
							else
							{
								//Otherwise Node Center did not move.  Use last Node Center
							}
		
							listOfAnimation[i].pos[j] = thisOffset;
							timeStamp += tickRate;
						}
					}
				}
											   
				//-------------------------------------------------------------
				// Check for rotational data. Again, use nextLine.
				
				//----------------------------------------------------
				// Then the very NEXT LINE most be POS_TRACK data OR
				// there is ONLY rotational Data for this node.
				GetNextLine(animScan,nextLine);
				if (strstr(nextLine,ASE_ANIM_ROT_HEADER) != NULL)
				{
					animScan = scanStart;
					timeStamp = firstFrame * tickRate;
		
					animScan = strstr(animScan,ASE_ANIM_ROT_HEADER);
		
					if (animScan)
					{
						countUp = true;
						actualCount++;
						animScan += strlen(ASE_ANIM_ROT_HEADER);
		
						//-----------------------------------------------------------
						// We have rotational data at least.  Store everything off.
						listOfAnimation[i].quat = (Stuff::UnitQuaternion *)TG_Shape::tglHeap->Malloc(sizeof(Stuff::UnitQuaternion) * numFrames);
						gosASSERT(listOfAnimation[i].quat != NULL);
		
						//-------------------------------------------
						// Setup basic variables.  May do this twice.
						strcpy(listOfAnimation[i].nodeId,nodeId);
						listOfAnimation[i].numFrames = numFrames;
						listOfAnimation[i].frameRate = frameRate;
						listOfAnimation[i].tickRate = tickRate;
		
						char LineData[1024];
						GetNextLine(animScan,LineData);
						animScan += strlen(LineData)+1;
			
						for (long j=0;j<listOfAnimation[i].numFrames;j++)
						{
							char *scanData;
							sprintf(nodeName,"%s %d",ASE_ANIM_ROT_SAMPLE,(int)timeStamp);
							scanData = strstr(LineData,nodeName);
		
							Stuff::UnitQuaternion thisFrame;
	
							float b=0.0f,c=0.0f,d=0.0f,phi=0.0f;
		
							if (scanData)
							{
								scanData += strlen(nodeName)+1;
		
								GetNumberData(scanData,numData);
								b = (float)atof(numData);
								scanData += strlen(numData)+1;
		
								GetNumberData(scanData,numData);
								c = (float)atof(numData);
								scanData += strlen(numData)+1;
		
								GetNumberData(scanData,numData);
								d = (float)atof(numData);
								scanData += strlen(numData)+1;
		
								GetNumberData(scanData,numData);
								phi = (float)atof(numData);
		
								GetNextLine(animScan,LineData);
								animScan += strlen(LineData)+1;
			
								//--------------------------------------------
								// MAX Writes out Quaternions as Angle, Axis.
								// Must Convert to real quaternion here.
								thisFrame.w = (float)cos(phi / 2.0f);
								thisFrame.x = b * (float)sin(phi / 2.0f);
								thisFrame.y = d * (float)sin(-phi / 2.0f);
								thisFrame.z = c * (float)sin(-phi / 2.0f);
							}
							else
							{
								//Otherwise rotation is 0.
								thisFrame.w = 1.0f;
								thisFrame.x = 0.0f;
								thisFrame.y = 0.0f;
								thisFrame.z = 0.0f;
							}
		
							if (!j)
							{
								thisFrame.Normalize();
								listOfAnimation[i].quat[j] = thisFrame;
							}
							else
							{
								thisFrame.Normalize();
								listOfAnimation[i].quat[j].Multiply(listOfAnimation[i].quat[j-1],thisFrame);
								listOfAnimation[i].quat[j].Normalize();
							}
		
							timeStamp += tickRate;
						}
					}
				}
				
				countUp = false;
			}
		}
	
		free(aseContents);
		aseContents = NULL;
		
		SaveBinaryCopy(binaryFileName);
	}

	return(0);
}	

//-------------------------------------------------------------------------------
//This function copies the pointers to the animation data in this class to the
//TG_MultiShape being drawn.  Nothing else happens.
void TG_AnimateShape::SetAnimationState (TG_MultiShapePtr shape)
{
	long i=0;
	shape->ClearAnimation();

	if (!shapeIdsSet)
	{
		for (long j=0;j<count;j++)
		{
			bool foundNode = false;
			for (i=0;i<shape->GetNumShapes();i++)
			{
				if (S_stricmp(listOfAnimation[j].nodeId,shape->GetNodeId(i)) == 0)
				{
					shape->SetCurrentAnimation(i,&listOfAnimation[j]);
					listOfAnimation[j].shapeId = i;
					foundNode = true;
					break;
				}
			}

			if (!foundNode)
				listOfAnimation[j].shapeId = -1;	//This node is missing.  Probably an arm off!
		}
		
		shapeIdsSet = true;
	}
	else
	{
		for (long j=0;j<count;j++)
		{
			if (listOfAnimation[j].shapeId != 0xffffffff)
				shape->SetCurrentAnimation(listOfAnimation[j].shapeId,&listOfAnimation[j]);
		}
	}
}	


void TG_MultiShape::Touch() {
	for (long i = 0; i < numTG_Shapes; i++)
		if (listOfShapes[i].node)
			listOfShapes[i].node->Touch();
}
//-------------------------------------------------------------------------------
