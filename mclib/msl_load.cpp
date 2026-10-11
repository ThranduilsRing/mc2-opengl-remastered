// Multi shape layer: TG_TypeMultiShape loading: binary copy, ASE import, texture setup, save and destroy, with the asset-load audit events.
// Declared in the class headers; shared names in msl_internal.h.
#include "msl_internal.h"
#include "mc2fmt.h"


//-------------------------------------------------------------------------------
//-------------------------------------------------------------------------------
// META-FIX design B PART 0 — loader-internal [ASSET_LOAD v1] classification
// chokepoint. ONE instrumentation point per fail-soft return inside the three
// shape-load entry points (LoadTGMultiShapeFromASE / LoadBinaryCopy /
// LoadFromFile) covers all ~33 current call sites and every future one.
// Classification (reviewed contract, CLASS B):
//   parse_fail   — file existed, content bad (bad header / missing material /
//                  unknown nodeType / bad tgl version). Logged ALWAYS,
//                  rate-limited per unique (event,path) via mc2AuditOnce
//                  (cap 64 keys — INFRA-1 helper; reviewed cap was 32).
//   file_missing — no file at all. Logged ONLY under MC2_ASSET_TRACE, because
//                  missing OPTIONAL files (arms/shadows/damage/anims) are
//                  NORMAL stock degradation and an always-on line would
//                  red-bucket healthy tier1 runs. REQUIRED slots get their
//                  own always-on `required_missing` from tgl_load_guard.
// Smoke: the `asset_load_fail` red bucket counts parse_fail (+
// required_missing + appr_type_fail) — deliberately NOT bare file_missing.
static const bool s_assetTrace_msl = mc2::gate::get<"MC2_ASSET_TRACE">();

static unsigned long long mslAssetKey(const char* event, const char* path)
{
	// djb2 over event+path — dedup key for mc2AuditOnce.
	unsigned long long h = 5381;
	for (const char* p = event; p && *p; ++p) h = h * 33 + (unsigned char)*p;
	for (const char* p = path;  p && *p; ++p) h = h * 33 + (unsigned char)*p;
	return h;
}

static void mslAssetLoadEvent(const char* event, const char* loader,
                              const char* reason, const char* path,
                              bool always)
{
	if (!always && !s_assetTrace_msl)
		return;
	if (!mc2AuditOnce("ASSET_LOAD", mslAssetKey(event, path)))
		return;
	mc2AuditLinef("[ASSET_LOAD v1] event=%s loader=%s reason=%s path=%s",
		event, loader, reason, (path && path[0]) ? path : "?");
}

long TG_TypeMultiShape::LoadBinaryCopy (const char *fileName)
{
	File binFile;
	long result = binFile.open(fileName);
	if (result != NO_ERR)
	{
		// Missing/unopenable .tgl — optional-slot degradation is normal;
		// trace-gated (see PART 0 header above).
		mslAssetLoadEvent("file_missing", "binary", "open_fail", fileName, false);
		return -1;
	}
	
	DWORD version = binFile.readInt();
	if (version == CURRENT_SHAPE_VERSION)
	{
		numTG_TypeShapes = binFile.readInt();
		numTextures = binFile.readInt();
	
		//ListOfTextures
		if (numTextures)
		{
			listOfTextures = (TG_TexturePtr)TG_Shape::tglHeap->Malloc(sizeof(TG_Texture) * numTextures);
			gosASSERT(listOfTextures != nullptr);
		
			binFile.read((MemoryPtr)listOfTextures,sizeof(TG_Texture) * numTextures);
		}
		else
		{
			listOfTextures = nullptr;
		}
	
		//listOfShapes
		listOfTypeShapes = (TG_TypeNodePtr *)TG_Shape::tglHeap->Malloc(sizeof(TG_TypeNodePtr) * numTG_TypeShapes);
		gosASSERT(listOfTypeShapes != nullptr);
	
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
					listOfTypeShapes[i] = nullptr;
				}
			}
			numTG_TypeShapes = 0;
			// META-FIX design B PART 0: file existed, content bad -> always-on.
			mslAssetLoadEvent("parse_fail", "binary", "unknown_node_type", fileName, true);
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
		// META-FIX design B PART 0: .tgl present but wrong shape version —
		// content bad, always-on parse_fail.
		mslAssetLoadEvent("parse_fail", "binary", "bad_version", fileName, true);
		return -1;
	}

	return 0;
}

//-------------------------------------------------------------------------------
// Track D — narrow construction API (see msl.h for rationale).

void TG_TypeMultiShape::AllocateImportedShapes(int numShapes)
{
	gosASSERT(numShapes > 0);
	gosASSERT(listOfTypeShapes == nullptr);  // freshly init()'d multi-shape

	numTG_TypeShapes = numShapes;
	listOfTypeShapes = (TG_TypeNodePtr *)TG_Shape::tglHeap->Malloc(
		sizeof(TG_TypeNodePtr) * numTG_TypeShapes);
	gosASSERT(listOfTypeShapes != nullptr);
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
	gosASSERT(listOfTextures == nullptr);  // freshly init()'d multi-shape

	numTextures = count;
	if (numTextures == 0)
	{
		listOfTextures = nullptr;
		return;
	}

	listOfTextures = (TG_TexturePtr)TG_Shape::tglHeap->Malloc(
		sizeof(TG_Texture) * numTextures);
	gosASSERT(listOfTextures != nullptr);
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
static const bool s_assimpTrace_msl = mc2::gate::get<"MC2_ASSIMP_TRACE">();
#define ASSIMP_TRACE_MSL(fmt, ...) \
    do { if (s_assimpTrace_msl) { \
        fprintf(stderr, "[ASSIMP_TRACE] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); } } while (0)

long TG_TypeMultiShape::LoadFromFile(const char* baseName)
{
	if (!baseName || !*baseName)
	{
		// META-FIX design B PART 0: degenerate caller input — trace-gated.
		mslAssetLoadEvent("file_missing", "import", "empty_base", baseName, false);
		return -1;
	}

	ASSIMP_TRACE_MSL("LoadFromFile baseName='%s'", baseName);

#ifdef ENABLE_ASSIMP_IMPORTER
	// Runtime killswitch: MC2_ASSIMP_IMPORT=0 forces legacy ASE for all asset
	// classes without requiring a rebuild. Default is enabled (any value other
	// than "0", or env var absent). Compile-time ENABLE_ASSIMP_IMPORTER is the
	// master gate; this is the per-launch override.
	{
		if (mc2::gate::get<"MC2_ASSIMP_IMPORT">()) {
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
					// META-FIX design B PART 0: probed file EXISTED but the
					// importer rejected it — content bad, always-on.
					mslAssetLoadEvent("parse_fail", "import", "import_fail",
						(const char*)probePath, true);
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
			listOfTypeShapes[i] = nullptr;
		}
	}

	if (listOfTypeShapes)
		TG_Shape::tglHeap->Free(listOfTypeShapes);
		
	listOfTypeShapes = nullptr;

	if (listOfTextures)
		TG_Shape::tglHeap->Free(listOfTextures);
	listOfTextures= nullptr;

	numTextures = 0;
	numTG_TypeShapes = 0;
	
#ifdef _DEBUG
	if (shapeName)
	{
		free(shapeName);
		shapeName = nullptr;
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
	listOfShapes = nullptr;	

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
		{
			// META-FIX design B PART 0: no ASE and no fresh .tgl — optional
			// slots (arms/shadow/damage) hit this on healthy stock installs,
			// so trace-gated only (required slots log via tgl_load_guard).
			mslAssetLoadEvent("file_missing", "ase", "no_ase", fileName, false);
			return(-1);
		}

		long aseFileSize = gos_FileSize(fileName);
	
		//---------------------------------------
		// Create Buffer to read entire file into
		BYTE *aseContents = (BYTE *)malloc(aseFileSize+1);
		gosASSERT(aseContents != nullptr);
	
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
		{
			// META-FIX design B PART 0: file existed, content bad -> always-on.
			mslAssetLoadEvent("parse_fail", "ase", "bad_header", fileName, true);
			return(-1);
		}

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
		{
			// META-FIX design B PART 0: parse_fail (missing material block).
			mslAssetLoadEvent("parse_fail", "ase", "no_material_count", fileName, true);
			return(-1);
		}

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
			{
				// META-FIX design B PART 0: parse_fail (missing material class).
				mslAssetLoadEvent("parse_fail", "ase", "no_material_class", fileName, true);
				return(-1);
			}
	
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

		IProviderAssetPtr armAssetPtr = nullptr;
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
			gosASSERT(listOfTextures != nullptr);
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
				mc2::fmt::to_buf(textId, "{}", mc2::fmt::safe(ASE_MATERIAL_BITMAP_ID));
		
				char *txmTemp = txmData;
				txmData = strstr(txmData,textId);
				
				if (txmData == nullptr)
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
			listOfTextures = nullptr;
	
		//------------------------------------------------------------
		// FIRST PASS.  Count number of GEOMObjects & HelperObjects
		numTG_TypeShapes = 0;
		char *aseScan = (char *)aseContents;
		aseScan = strstr(aseScan,ASE_OBJECT);
		while (aseScan != nullptr)
		{
			numTG_TypeShapes++;
			aseScan += strlen(ASE_OBJECT)+1;
			aseScan = strstr(aseScan,ASE_OBJECT);
		}
	
		aseScan = (char *)aseContents;
		aseScan = strstr(aseScan,ASE_HELP_OBJECT);
		while (aseScan != nullptr)
		{
			//--------------------------------------------------------
			// Check if NodeName is handle_ or WORLD.  These are bad!
			// Do not count them!
			aseScan = strstr(aseScan,ASE_NODE_NAME);
			gosASSERT(aseScan != nullptr);								//Node must have a NAME, right?
			aseScan += strlen(ASE_NODE_NAME)+1;
	
			char chkWord[512];
			GetWordData(aseScan,chkWord);
	
			if (strstr(chkWord,"handle_") == nullptr && strstr(chkWord,"World") == nullptr)
			{
				numTG_TypeShapes++;
			}
	
			aseScan += strlen(ASE_HELP_OBJECT)+1;
			aseScan = strstr(aseScan,ASE_HELP_OBJECT);
		}
	
		listOfTypeShapes = (TG_TypeNodePtr *)TG_Shape::tglHeap->Malloc(sizeof(TG_TypeNodePtr) * (numTG_TypeShapes+1));
		gosASSERT(listOfTypeShapes != nullptr);
	
		memset(listOfTypeShapes,0,sizeof(TG_TypeNodePtr) * (numTG_TypeShapes+1));
	
		//------------------------------------------------------------
		// SECOND PASS.  Load HelperObjects.
		long startIndex = 0;
	
		aseScan = (char *)aseContents;
		aseScan = strstr(aseScan,ASE_HELP_OBJECT);
		while (aseScan != nullptr)
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
				gosASSERT(aseScan != nullptr);								//Node must have a NAME, right?
				aseScan += strlen(ASE_NODE_NAME)+1;
	
				char chkWord[512];
				GetWordData(aseScan,chkWord);
	
				//-------------------------------------------------------------------
				// Is this a forbidden helper object?
				if ((strstr(chkWord,"handle") == nullptr) && (strstr(chkWord,"World") == nullptr))
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
				gosASSERT(scanStart != nullptr);								//Node must have a NAME, right?
				scanStart += strlen(ASE_NODE_NAME)+1;
	
				char chkWord[512];
				GetWordData(scanStart,chkWord);
	
				//-------------------------------------------------------------------
				// Is this a forbidden helper object?
				if ((strstr(chkWord,"handle") == nullptr) && (strstr(chkWord,"World") == nullptr))
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
		while (aseScan != nullptr)
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
				TG_TypeNodePtr parentNode = nullptr;
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

		if (armAssetPtr != nullptr)
		{
			armAssetPtr->Close();
		}
	
		free(aseContents);
		aseContents = nullptr;
	
		SaveBinaryCopy(binaryFileName);
	}

	return(0);
}	
