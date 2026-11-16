// Multi shape layer: TG_TypeMultiShape loading: binary copy, ASE import, texture setup, save and destroy, with the asset-load audit events.
// Declared in the class headers; shared names in msl_internal.h.
#include "mc2_czstring.h"
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "msl_internal.h"
#include "mc2fmt.h"
#include "content_ir.h"
#include <filesystem>


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
//   file_missing — no file at all. Never logged, because missing OPTIONAL
//                  files (arms/shadows/damage/anims) are NORMAL stock
//                  degradation and an always-on line would red-bucket
//                  healthy tier1 runs. REQUIRED slots get their own
//                  always-on `required_missing` from tgl_load_guard.
// Smoke: the `asset_load_fail` red bucket counts parse_fail (+
// required_missing + appr_type_fail) — deliberately NOT bare file_missing.

static unsigned long long mslAssetKey(mc2::czstring event, mc2::czstring path)
{
	// djb2 over event+path — dedup key for mc2AuditOnce.
	unsigned long long h = 5381;
	for (const char* p = event; p && *p; ++p) h = h * 33 + static_cast<unsigned char>(*p);
	for (const char* p = path;  p && *p; ++p) h = h * 33 + static_cast<unsigned char>(*p);
	return h;
}

static void mslAssetLoadEvent(mc2::czstring event, mc2::czstring loader,
                              const char* reason, const char* path)
{
	if (!mc2AuditOnce("ASSET_LOAD", mslAssetKey(event, path)))
		return;
	mc2AuditLinef("[ASSET_LOAD v1] event={} loader={} reason={} path={}",
		mc2::fmt::safe(event), mc2::fmt::safe(loader), mc2::fmt::safe(reason), mc2::fmt::safe((path && path[0]) ? path : "?"));
}

mc2::content::Asset TG_TypeMultiShape::ExtractContentIR()
{
    mc2::content::Asset asset;
    asset.legacyBounds = true;
    for (const auto& t : textureStore)
        asset.materials.push_back({std::string(t.textureName, strnlen(t.textureName, sizeof(t.textureName))), t.textureAlpha});
    for (long i = 0; i < numTG_TypeShapes; ++i) {
        auto* node = listOfTypeShapes[i];
        mc2::content::Node n;
        // Legacy TGL fields may have no terminator and contain arbitrary padding
        // in their last byte. Match the runtime's 23-character identity limit.
        n.name.assign(node->getNodeId(), strnlen(node->getNodeId(), TG_NODE_ID - 1));
        n.parent.assign(node->getParentId(), strnlen(node->getParentId(), TG_NODE_ID - 1));
        n.center = node->GetNodeCenter(); n.relativeCenter = node->GetRelativeNodeCenter();
        n.helper = node->GetNodeType() != SHAPE_NODE;
        if (S_strnicmp(n.name.c_str(), "SpotLight_", 10) == 0 || S_strnicmp(n.name.c_str(), "LitWin", 6) == 0) {
            mc2::content::Socket socket;
            socket.name = socket.parent = n.name;
            socket.type = S_strnicmp(n.name.c_str(), "SpotLight_", 10) == 0 ? "spot" : "point";
            socket.intensityClass = "legacy";
            socket.position = n.center;
            asset.sockets.push_back(std::move(socket));
        }
        if (!n.helper) {
            auto* shape = static_cast<TG_TypeShape*>(node);
            n.vertices = std::move(shape->typeVertexStore);
            n.triangles = std::move(shape->typeTriangleStore);
            // MC2X legacy binaries sometimes carry bad indices. Retain the historical
            // vertex-zero compatibility repair before common validation/publication.
            for (auto& t : n.triangles) for (auto& v : t.Vertices)
                if (v >= n.vertices.size()) v = 0;
        }
        asset.nodes.push_back(std::move(n));
    }
    return asset;
}

long TG_TypeMultiShape::BuildContentIR(mc2::content::Asset&& asset, std::string_view sourcePath, float* groundDy)
{
    const std::string sourceStorage(sourcePath);
    const char* source = sourceStorage.c_str();
    if (!mc2::content::cook(asset, source)) {
        mslAssetLoadEvent("parse_fail", "content", "invalid_ir", source);
        return -1;
    }
    // Nothing reaches the runtime until the whole selected state/LOD has passed validation.
    destroy();
    init();
    contentSockets_ = std::move(asset.sockets);
    numTG_TypeShapes = static_cast<int>(asset.nodes.size());
    bindTypeNodes(numTG_TypeShapes);
    minBox = glm::vec3(100000.0f); maxBox = glm::vec3(-100000.0f);
    for (size_t i = 0; i < asset.nodes.size(); ++i) {
        auto& n = asset.nodes[i];
        if (n.helper) {
            auto node = std::make_unique<TG_TypeNode>();
            node->init();
            node->InitContentNode(n.name.c_str(), n.parent.c_str(), n.center, n.relativeCenter);
            adoptTypeNode(static_cast<long>(i), std::move(node));
            continue;
        }
        const bool excluded = asset.legacyBounds &&
            (S_strnicmp(n.name.c_str(), "_PAB", 4) == 0 || S_strnicmp(n.name.c_str(), "LOS_", 4) == 0 ||
             S_strnicmp(n.name.c_str(), "SpotLight", 9) == 0);
        if (!excluded) for (const auto& v : n.vertices) {
            const auto p = v.position + (asset.legacyBounds ? n.relativeCenter : glm::vec3(0));
            minBox = glm::min(minBox, p);
            maxBox = glm::max(maxBox, p);
        }
        auto shape = std::make_unique<TG_TypeShape>();
        shape->init();
        shape->InitFromImportedMesh(n.name.c_str(), n.parent.c_str(), n.center, std::move(n.vertices), std::move(n.triangles));
        // Shape loads also run on the sim worker (damage/LOD swaps). Rendering
        // consumes the mesh stores; the unused legacy buffers/declaration must
        // not register render resources here.
        shape->InitContentNode(n.name.c_str(), n.parent.c_str(), n.center, n.relativeCenter);
        adoptTypeNode(static_cast<long>(i), std::move(shape));
    }
    if (minBox.x > maxBox.x) minBox = maxBox = glm::vec3(0);
    extentRadius = std::max(glm::length(minBox), glm::length(maxBox));
    if (!asset.legacyBounds) extentRadius = std::max(0.5f * glm::length(maxBox - minBox), 1.0f);
    float dy = asset.ground == 2 ? -minBox.y : (asset.ground == 1 ? -maxBox.y : 0.0f);
    if (groundDy) *groundDy = dy;
    if (dy != 0) {
        for (auto* n : typeNodeView) if (n->GetNodeType() == SHAPE_NODE)
            static_cast<TG_TypeShape*>(n)->TranslateTypeVerticesY(dy);
        minBox.y += dy; maxBox.y += dy;
    }
    std::vector<const char*> names;
    auto alphas = std::make_unique<bool[]>(asset.materials.size());
    for (size_t i = 0; i < asset.materials.size(); ++i) {
        names.push_back(asset.materials[i].textureName.c_str());
        alphas[i] = asset.materials[i].alpha;
    }
    SetImportedTextures(static_cast<DWORD>(names.size()), names.data(), alphas.get());
    return NO_ERR;
}

long TG_TypeMultiShape::LoadBinaryCopy(mc2::czstring fileName)
{
    mc2::content::Asset cached;
    cached.legacyBounds = true;
    if (mc2::content::loadCached(cached, fileName)) return BuildContentIR(std::move(cached), fileName);
    TG_TypeMultiShape frontend;
    const long result = frontend.DecodeStockBinary(fileName);
    return result == NO_ERR ? BuildContentIR(frontend.ExtractContentIR(), fileName) : result;
}

static bool preferStockBinary(mc2::czstring ase, mc2::czstring binary, bool forceASE)
{
    if (forceASE || !fileExists(binary)) return false;
    if (!fileExists(ase, FILE_ON_DISK)) return true;
    // Format preference must never let a base ASE shadow a higher-priority mod TGL.
    return LookupModPriority(binary) < LookupModPriority(ase);
}

long TG_TypeMultiShape::LoadTGMultiShapeFromASE(mc2::czstring fileName, bool forceMakeBinary, IProviderEngine* armProvider)
{
    ZoneScopedN("Shape.Load");
    ZoneText(fileName, strlen(fileName));
    const auto name = std::filesystem::path(fileName).stem().string();
    FullPathFileName binary;
    binary.init(tglPath, name.c_str(), ".tgl");
    const char* source = preferStockBinary(fileName, binary, forceMakeBinary)
        ? static_cast<const char*>(binary) : fileName;
    mc2::content::Asset cached;
    cached.legacyBounds = true;
    if (mc2::content::loadCached(cached, source)) return BuildContentIR(std::move(cached), source);
    TG_TypeMultiShape frontend;
    const long result = frontend.DecodeStockASE(fileName, forceMakeBinary, armProvider);
    return result == NO_ERR ? BuildContentIR(frontend.ExtractContentIR(), source) : result;
}

long TG_TypeMultiShape::DecodeStockBinary (std::string_view sourcePath)
{
    const std::string sourceStorage(sourcePath);
    const char* fileName = sourceStorage.c_str();
	File binFile;
	long result = binFile.open(fileName);
	if (result != NO_ERR)
	{
		// Missing/unopenable .tgl — optional-slot degradation is normal
		// (see PART 0 header above).
		return -1;
	}
	
	DWORD version = binFile.readInt();
	if (version == CURRENT_SHAPE_VERSION)
	{
		numTG_TypeShapes = binFile.readInt();
		numTextures = binFile.readInt();
		if (numTG_TypeShapes < 0 || numTG_TypeShapes > kMaxMultiShapeNodes || numTextures < 0 || numTextures > static_cast<int>(kMaxShapeTextures))
		{
			MC2_LOG(Io, Error, "[SHAPE_COUNT_BAD] file={} shapes={} textures={} (over {} / {}; multi-shape rejected)", fileName, numTG_TypeShapes, numTextures, kMaxMultiShapeNodes, kMaxShapeTextures);
			numTG_TypeShapes = numTextures = 0;
			mslAssetLoadEvent("parse_fail", "binary", "bad_count", fileName);
			return -1;
		}

		//ListOfTextures
		if (numTextures)
		{
			textureStore.resize(numTextures);
			listOfTextures = textureStore.data();

			binFile.read(reinterpret_cast<MemoryPtr>(listOfTextures),sizeof(TG_Texture) * numTextures);
		}
		else
		{
			listOfTextures = nullptr;
		}

		//listOfShapes
		bindTypeNodes(numTG_TypeShapes);

		bool aborted = false;
		const char* abortReason = "unknown_node_type";
		long loadedShapes = 0;
		for (long i=0;i<numTG_TypeShapes;i++)
		{
			int nodeType = binFile.readInt();

			if (nodeType == SHAPE_NODE)
			{
				adoptTypeNode(i, std::make_unique<TG_TypeShape>());
				listOfTypeShapes[i]->init();
				const bool nodeOK = listOfTypeShapes[i]->LoadBinaryCopy(binFile);
				loadedShapes++;
				if (!nodeOK)
				{
					// The stream stopped mid-node: reading on would take the next node's type from its bytes.
					abortReason = "bad_count";
					aborted = true;
					break;
				}
			}
			else if (nodeType == TYPE_NODE)
			{
				adoptTypeNode(i, std::make_unique<TG_TypeNode>());
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
				mc2::fmt::print_out("[MSL] unknown nodeType={:d} at index {:d}/{:d} — aborting load of this shape\n",
					mc2::fmt::as_i32(nodeType), mc2::fmt::as_i32(i), mc2::fmt::as_i32(static_cast<long>(numTG_TypeShapes))); fflush(stdout);
				aborted = true;
				break;
			}
		}

		if (aborted)
		{
			// Clear any partially-constructed shapes so the MultiShape is
			// cleanly empty. Caller treats -1 as unsupported.
			for (long i = 0; i < loadedShapes; ++i) {
				typeNodeStore[i].reset();
				listOfTypeShapes[i] = nullptr;
			}
			numTG_TypeShapes = 0;
			// META-FIX design B PART 0: file existed, content bad -> always-on.
			mslAssetLoadEvent("parse_fail", "binary", abortReason, fileName);
			return -1;
		}


	}
	else
	{
		// META-FIX design B PART 0: .tgl present but wrong shape version —
		// content bad, always-on parse_fail.
		mslAssetLoadEvent("parse_fail", "binary", "bad_version", fileName);
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
	bindTypeNodes(numTG_TypeShapes);

	// Construct empty TG_TypeShape per slot — caller fills via
	// listOfTypeShapes[i]->InitFromImportedMesh(...). This mirrors the
	// LoadBinaryCopy SHAPE_NODE branch (msl.cpp:222-228).
	for (long i = 0; i < numTG_TypeShapes; i++)
	{
		adoptTypeNode(i, std::make_unique<TG_TypeShape>());
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

	textureStore.assign(numTextures, TG_Texture{});
	listOfTextures = textureStore.data();

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
		// real values once legacy texture manager has loaded the bitmap.
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
#include "mech_anim_runtime.h"
#endif

long TG_TypeMultiShape::LoadFromFile(mc2::czstring baseName)
{
	if (!baseName || !*baseName)
	{
		// META-FIX design B PART 0: degenerate caller input.
		return -1;
	}


#ifdef ENABLE_ASSIMP_IMPORTER
	// Compile-time ENABLE_ASSIMP_IMPORTER is the only gate (the MC2_ASSIMP_IMPORT
	// runtime killswitch was retired).
	{
		{
			// Probe order: .glb then .fbx. On any importer failure, fall
			// through to ASE so a broken modder asset can't render the mech
			// un-loadable. Stock-install playability is paramount.
			static const char* const kImportExts[] = { ".glb", ".fbx" };
			for (const auto & importExt : kImportExts)
			{
				FullPathFileName probePath;
				probePath.init(tglPath, baseName, importExt);
				const bool exists = fileExists(probePath, FILE_ON_DISK);
				if (exists)
				{
					// P2 BOUNDARY ADAPTER (bt2018 lane; thin, to be replaced by the content-pipeline
					// resource provider): Assimp and the <stem>.package.json sidecar open files with
					// raw OS calls that do not see the mod VFS. Hand them the mounted mod's real path
					// when a mod serves this key; base-data paths pass through unchanged.
					const char* modPath = LookupModAbsolutePath(static_cast<const char*>(probePath));
					long r = ImportGeometryFromFile(modPath ? modPath : static_cast<const char*>(probePath), this);
					if (r == 0)
						return NO_ERR;
					// META-FIX design B PART 0: probed file EXISTED but the
					// importer rejected it — content bad, always-on.
					mslAssetLoadEvent("parse_fail", "import", "import_fail",
						static_cast<const char*>(probePath));
					// Importer logged the reason via STOP/PAUSE; fall through.
					break;
				}
			}
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
void TG_TypeMultiShape::SaveBinaryCopy (mc2::czstring fileName)
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
		binFile.write(reinterpret_cast<MemoryPtr>(listOfTextures),sizeof(TG_Texture) * numTextures);
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
#ifdef ENABLE_ASSIMP_IMPORTER
	mc2mechanim::UnregisterImportedType(this);	// no stale entry / leaked aiScene past the type
#endif
    stableIdentity_.renew(this);
    contentSockets_.clear();
	// The owners release the nodes (~TG_TypeNode is virtual, B7o.1: the derived destructor runs destroy() once); the view goes with them.
	std::vector<std::unique_ptr<TG_TypeNode>>().swap(typeNodeStore);
	std::vector<TG_TypeNodePtr>().swap(typeNodeView);
	listOfTypeShapes = nullptr;

	std::vector<TG_Texture>().swap(textureStore);
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
    animationBinding_.invalidate();
    stableIdentity_.renew(this);
	// Each record owns its node (B7o.6b): releasing the records releases the nodes.
	std::vector<TG_ShapeRec>().swap(shapeRecStore);
	listOfShapes = nullptr;

	numTG_Shapes = 0;
    instanceTextures_.clear(); instanceMaterials_.clear(); materialSet_={};
}	

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  -1 if file not found or file not ASE Format.
//Function runs through each piece of ASE file and creates a separate
//TG_Shape for each one.  First pass is count number of GEOMOBJECTS.
//Second pass is load each one.
long TG_TypeMultiShape::DecodeStockASE (std::string_view sourcePath, bool forceMakeBinary, IProviderEngine* armProvider)
{
    const std::string sourceStorage(sourcePath);
    const char* fileName = sourceStorage.c_str();
    // A loose ASE is the editable source, regardless of timestamps. Stock-only
    // installs retain the packed/loose TGL compatibility frontend.
    char drive[MAX_PATH], dir[MAX_PATH], name[MAX_PATH], ext[MAX_PATH];
    _splitpath(fileName, drive, dir, name, ext);
    FullPathFileName binaryFileName;
    binaryFileName.init(tglPath, name, ".tgl");
    const bool makeBinary = !preferStockBinary(fileName, binaryFileName, forceMakeBinary);

	if (!makeBinary)
	{
		return (DecodeStockBinary(static_cast<const char*>(binaryFileName)));
	}
	else
	{
		//------------------------------------------
		// Check if exists
		if (!gos_FileExists(fileName))
		{
			// META-FIX design B PART 0: no ASE and no fresh .tgl — optional
			// slots (arms/shadow/damage) hit this on healthy stock installs,
			// so nothing is logged (required slots log via tgl_load_guard).
			return(-1);
		}

		long aseFileSize = gos_FileSize(fileName);
	
		//---------------------------------------
		// Create Buffer to read entire file into
		BYTE *aseContents = static_cast<BYTE *>(malloc(aseFileSize+1));
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
		if (S_strnicmp(ASE_HEADER,reinterpret_cast<char *>(aseContents),strlen(ASE_HEADER)) != 0)
		{
			// META-FIX design B PART 0: file existed, content bad -> always-on.
			mslAssetLoadEvent("parse_fail", "ase", "bad_header", fileName);
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
		textureData = strstr(reinterpret_cast<char *>(aseContents),textureId);
		if (!textureData)
		{
			// META-FIX design B PART 0: parse_fail (missing material block).
			mslAssetLoadEvent("parse_fail", "ase", "no_material_count", fileName);
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
			textureData = strstr(reinterpret_cast<char *>(aseBuffer),textureId);
			if (!textureData)
			{
				// META-FIX design B PART 0: parse_fail (missing material class).
				mslAssetLoadEvent("parse_fail", "ase", "no_material_class", fileName);
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
	
			aseBuffer = reinterpret_cast<unsigned char *>(textureData);
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
			textureStore.assign(numTextures, TG_Texture{});
			listOfTextures = textureStore.data();

			char *txmData = reinterpret_cast<char *>(aseContents);
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
				mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
			
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
			
				if( fileExists(textureName) == 1 )
				{
					File touchTexture;
					touchTexture.open(textureName);
					touchTexture.close();
				}
			}
		}
		else
			listOfTextures = nullptr;
	
		//------------------------------------------------------------
		// FIRST PASS.  Count number of GEOMObjects & HelperObjects
		numTG_TypeShapes = 0;
		char *aseScan = reinterpret_cast<char *>(aseContents);
		aseScan = strstr(aseScan,ASE_OBJECT);
		while (aseScan != nullptr)
		{
			numTG_TypeShapes++;
			aseScan += strlen(ASE_OBJECT)+1;
			aseScan = strstr(aseScan,ASE_OBJECT);
		}
	
		aseScan = reinterpret_cast<char *>(aseContents);
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
	
		bindTypeNodes(numTG_TypeShapes + 1);
	
		//------------------------------------------------------------
		// SECOND PASS.  Load HelperObjects.
		long startIndex = 0;
	
		aseScan = reinterpret_cast<char *>(aseContents);
		aseScan = strstr(aseScan,ASE_HELP_OBJECT);
		while (aseScan != nullptr)
		{
			adoptTypeNode(startIndex, std::make_unique<TG_TypeNode>());
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
				char *aseData = static_cast<char *>(malloc(aseFileSize+1));
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
					long parseResult = listOfTypeShapes[startIndex]->MakeFromHelper(reinterpret_cast<unsigned char *>(aseData),fileName);
					if (parseResult != 0)
					{
						mslAssetLoadEvent("parse_fail", "ase", "node_parse_fail", fileName);
						return(parseResult);
					}
	
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
					long parseResult = listOfTypeShapes[startIndex]->MakeFromHelper(reinterpret_cast<unsigned char *>(aseScan),fileName);
					if (parseResult != 0)
					{
						mslAssetLoadEvent("parse_fail", "ase", "node_parse_fail", fileName);
						return(parseResult);
					}
	
					startIndex++;
				}
			}
	
			aseScan += strlen(ASE_HELP_OBJECT);
			aseScan = strstr(aseScan,ASE_HELP_OBJECT);
		}
	
		//------------------------------------------------------------
		// THIRD PASS.  Load GeomObjects.
		aseScan = reinterpret_cast<char *>(aseContents);
		aseScan = strstr(aseScan,ASE_OBJECT);
		while (aseScan != nullptr)
		{
			adoptTypeNode(startIndex, std::make_unique<TG_TypeShape>());
			listOfTypeShapes[startIndex]->init();
			listOfTypeShapes[startIndex]->CreateListOfTextures(listOfTextures,numTextures);
	
			//---------------------------------------------------
			// Calling from top will load just first GEOMOBJECT!
			// We now make the "top" the start of each GEOMOBJECT!
			char *aseEnd = aseScan + strlen(ASE_OBJECT)+1;
			aseEnd = strstr(aseEnd,ASE_OBJECT);
	
			if (aseEnd)
			{
				// Keep whole-file zero padding: legacy scans can pass NUL.
				std::unique_ptr<char, decltype(&free)> aseOwner(
					static_cast<char*>(malloc(aseFileSize+1)), &free);
				char* aseData = aseOwner.get();
				ZeroMemory(aseData, aseFileSize+1);
				memcpy(aseData,aseScan,aseEnd - aseScan);
				
				long parseResult = listOfTypeShapes[startIndex]->ParseASEFile(reinterpret_cast<unsigned char *>(aseData),fileName);
				if (parseResult != 0)
				{
					mslAssetLoadEvent("parse_fail", "ase", "node_parse_fail", fileName);
					return(parseResult);
				}
	
			}
			else
			{
				long parseResult = listOfTypeShapes[startIndex]->ParseASEFile(reinterpret_cast<unsigned char *>(aseScan),fileName);
				if (parseResult != 0)
				{
					mslAssetLoadEvent("parse_fail", "ase", "node_parse_fail", fileName);
					return(parseResult);
				}
			}
	
			startIndex++;
			aseScan += strlen(ASE_OBJECT)+1;
			aseScan = strstr(aseScan,ASE_OBJECT);
		}
	
 		//------------------------------------------------------------------
		// FOURTH PASS.  Setup Heirarchy ALWAYS or NO Shape BOUNDS!!!
		{
			aseScan = reinterpret_cast<char *>(aseContents);
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
					
			}
		
		}

		if (armAssetPtr != nullptr)
		{
			armAssetPtr->Close();
		}
	
		free(aseContents);
		aseContents = nullptr;
	
	}

	return(0);
}	
